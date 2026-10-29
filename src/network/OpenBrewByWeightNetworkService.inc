OpenBrewByWeightNetwork::OpenBrewByWeightNetwork() : settings_(g_networkSettings) {}

bool OpenBrewByWeightNetwork::begin(const PersistedSettings &settings,
                               const NetworkBridgeCallbacks &callbacks) {
  if (beginCompleted_ || instance_ != nullptr ||
      acceptedCommandQueue_ ||
      taskHandle_ != nullptr || taskStopped_ ||
      statusResponseMux_ || workBuf_ != nullptr ||
      callbacks.copyControlStatus == nullptr ||
      callbacks.copyControlGate == nullptr ||
      callbacks.refreshControlStatus == nullptr ||
      callbacks.enqueueWebCommand == nullptr ||
      callbacks.copyDebugEvents == nullptr ||
      callbacks.clearResetHistory == nullptr ||
      callbacks.reportTaskWatchdogFault == nullptr ||
      callbacks.requestSafeRestart == nullptr) {
    return false;
  }
  quietIdfWifiDriverWarnings();
  settings_ = settings;
  callbacks_ = callbacks;
  // A factory password (paddle recovery, USB reset) cannot back a login:
  // drop everything but the notice choice before the first request.
  WebAccessStored access;
  if (loadWebAccessStored(access) == WebAccessLoad::INVALID) {
    lifecycleLog("Web UI access settings unreadable; the Web UI password is off");
  }
  g_webAccess = WebAccessState{};
  g_webAccess.stored = access;
  if (passwordIsFactoryDefault(settings_)) webAccessDisable(g_webAccess);
  memset(&access, 0, sizeof(access));
  beginCompanion();
  stopRequested_.store(false, std::memory_order_release);
  acceptedCommandQueue_.reset(
      xQueueCreate(WEB_COMMAND_QUEUE_LENGTH, sizeof(WebCommand)));
  if (!acceptedCommandQueue_) {
    return false;
  }
  statusResponseMux_.reset(xSemaphoreCreateMutex());
  if (!statusResponseMux_) {
    acceptedCommandQueue_.reset();
    return false;
  }
  taskStopped_.reset(xSemaphoreCreateBinary());
  if (!taskStopped_) {
    statusResponseMux_.reset();
    acceptedCommandQueue_.reset();
    return false;
  }
  workBuf_ = static_cast<NetworkWorkBuf *>(
      allocExternal(sizeof(NetworkWorkBuf), AllocationOwner::NETWORK));
  if (workBuf_ == nullptr) {
    taskStopped_.reset();
    statusResponseMux_.reset();
    acceptedCommandQueue_.reset();
    noteWorkBufExternal(false);
    return false;
  }
  new (workBuf_) NetworkWorkBuf{};
  static_assert(sizeof(uiStreams_) / sizeof(uiStreams_[0]) ==
                    sizeof(workBuf_->uiFingerprints) / sizeof(workBuf_->uiFingerprints[0]),
                "Every UI session needs an external fingerprint slot");
  noteWorkBufExternal(true);
  memset(&g_wifiScan, 0, sizeof(g_wifiScan));
  memset(&g_wifiScanWorking, 0, sizeof(g_wifiScanWorking));
  static_assert(REQUEST_BODY_CAPACITY == 2048,
                "NetworkWorkBuf::requestBody must match REQUEST_BODY_CAPACITY");
  g_work = workBuf_;
  instance_ = this;
  initJsonParser();
  ntpAppliedSettingsGeneration_ = ntpSettingsGeneration_;
  g_wallClock.reset();
  if (!ensureRfCoexBt()) {
    log(DebugCategory::NETWORK, DebugCode::INITIALIZATION_FAILED,
        BOOT_SUBSYSTEM_NETWORK, rfCoexLastError());
  }
  // Webhook owns a task when enabled. Start it only after all passive Network
  // resources exist, and make the rollback below join it before freeing them.
  if (!webhooks_.begin(settings.webhook)) {
    log(DebugCategory::NETWORK, DebugCode::INITIALIZATION_FAILED,
        BOOT_SUBSYSTEM_NETWORK, 1);
  }
  // Pin beside the Wi-Fi/LwIP stacks on PRO_CPU (core 0).
  // Stack stays internal: NVS writes run with both caches (flash and PSRAM
  // mappings) disabled, so a task with a PSRAM stack could not perform them.
  if (xTaskCreatePinnedToCore(taskEntry, "network_manager",
                              NETWORK_MANAGER_TASK_STACK_SIZE, this,
                              tskIDLE_PRIORITY + 1, &taskHandle_,
                              0) != pdPASS) {
    taskHandle_ = nullptr;
    // stop() also rolls back a partially initialized/degraded webhook in
    // strict reverse ownership order.
    (void)stop();
    return false;
  }
  beginCompleted_ = true;
  return true;
}
bool OpenBrewByWeightNetwork::stop() {
  if (instance_ != nullptr && instance_ != this) return false;

  TaskHandle_t task = taskHandle_;
  if (task != nullptr) {
    stopRequested_.store(true, std::memory_order_release);
    xTaskNotifyGive(task);
    if (!taskStopped_ ||
        xSemaphoreTake(taskStopped_.get(),
                       pdMS_TO_TICKS(NETWORK_STOP_TIMEOUT_MS)) !=
            pdTRUE) {
      return false;
    }
  }

  // Network task is joined: no mDNS caller remains, release its task/socket.
  stopMdns();

  if (!webhooks_.stop()) return false;

  instance_ = nullptr;
  g_work = nullptr;
  if (workBuf_ != nullptr) {
    workBuf_->~NetworkWorkBuf();
    heapCapsFree(workBuf_);
    workBuf_ = nullptr;
    noteWorkBufExternal(false);
  }
  statusResponseMux_.reset();
  acceptedCommandQueue_.reset();
  taskStopped_.reset();
  callbacks_ = {};
  stopRequested_.store(false, std::memory_order_release);
  return true;
}

bool OpenBrewByWeightNetwork::lockWorkBuf() {
  if (workBuf_ == nullptr || !statusResponseMux_) {
    return false;
  }
  return xSemaphoreTake(statusResponseMux_.get(), pdMS_TO_TICKS(2500)) ==
         pdTRUE;
}

void OpenBrewByWeightNetwork::unlockWorkBuf() {
  xSemaphoreGive(statusResponseMux_.get());
}

void OpenBrewByWeightNetwork::unlockJsonBody() {
  if (workBuf_ != nullptr) {
    memset(workBuf_->requestBody, 0, sizeof(workBuf_->requestBody));
  }
  unlockWorkBuf();
}

esp_err_t OpenBrewByWeightNetwork::lockJsonBody(httpd_req_t *request,
                                           const char *invalidMessage) {
  if (!lockWorkBuf()) {
    return workBufBusy(request);
  }
  bool envelopeFailed = false;
  if (!readJsonBody(request, workBuf_->requestBody, &envelopeFailed)) {
    unlockJsonBody();
    if (envelopeFailed) return relayEnvelopeReject(request);
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                     invalidMessage);
  }
  return ESP_OK;
}

// Shared tail for destructive-action handlers: parse the JSON body while the
// work buffer is locked and accept it only when it carries exactly
// {"confirm": expectedConfirmation}. Handlers keep their own error responses.
bool OpenBrewByWeightNetwork::lockConfirmedBody(httpd_req_t *request,
                                           const char *invalidMessage,
                                           const char *expectedConfirmation) {
  if (lockJsonBody(request, invalidMessage) != ESP_OK) {
    return false;
  }
  char confirmation[32] = {};
  cJSON *root = parseJsonDocument(workBuf_->requestBody);
  static const char *const fields[] = {"confirm"};
  const bool confirmed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonString(root, "confirm", confirmation, sizeof(confirmation), false) &&
      strcmp(confirmation, expectedConfirmation) == 0;
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  unlockJsonBody();
  memset(confirmation, 0, sizeof(confirmation));
  return confirmed;
}

esp_err_t OpenBrewByWeightNetwork::workBufBusy(httpd_req_t *request) {
  actionLogf("http: %.*s answered busy: response workspace unavailable",
             static_cast<int>(strcspn(request->uri, "?")), request->uri);
  return sendError(request, STATUS_UNAVAILABLE, "STATUS_BUSY",
                   "Status snapshot is busy; retry shortly.");
}

bool OpenBrewByWeightNetwork::enqueueAcceptedCommand(const WebCommand &command) {
  return acceptedCommandQueue_ &&
         xQueueSend(acceptedCommandQueue_.get(), &command, 0) == pdTRUE;
}

void OpenBrewByWeightNetwork::requestNtpSyncIfNeeded() {
  dataMux_.lock();
  // Activity-triggered sync is an automatic path: skip it entirely while the
  // NTP toggle is off so the pending flag does not churn on every activity.
  if (settings_.runtime.ntpSyncEnabled &&
      wallClockNeedsActivityNtpSync(g_wallClock, millis())) {
    ntpActivitySyncPending_ = true;
  }
  dataMux_.unlock();
}

NetworkStatusSnapshot OpenBrewByWeightNetwork::snapshot() {
  NetworkStatusSnapshot copy;
  dataMux_.lock();
  copy = status_;
  const bool pendingConfirm =
      staConfirmArmed_ &&
      settings_.staConfigState ==
          static_cast<uint8_t>(StaConfigState::PENDING) &&
      (!status_.apActive || setupKeepsSoftAp(status_.setupPhase));
  const uint32_t deadline = staConfirmDeadlineMs_;
  dataMux_.unlock();
  copy.taskAgeMs = static_cast<uint32_t>(
      millis() - lastTaskProgressAtMs_.load(std::memory_order_relaxed));
  copy.taskStackMinBytes = taskStackMinBytes_.load(std::memory_order_relaxed);
  if (pendingConfirm) {
    const uint32_t now = millis();
    copy.confirmRemainingMs =
        static_cast<int32_t>(deadline - now) > 0 ? (deadline - now) : 0;
  } else {
    copy.confirmRemainingMs = 0;
  }
  return copy;
}

NetworkHeapTelemetrySnapshot OpenBrewByWeightNetwork::heapTelemetrySnapshot() {
  TaskLockGuard lock(dataMux_);
  return {httpHeap_.aggregate, wifiHeap_.aggregate, otaHeap_.aggregate};
}

void OpenBrewByWeightNetwork::beginOwnedHeapLifecycle(
    HeapLifecycleTracker &tracker, HeapLifecycleEvent event) {
  const HeapCapSnapshot before = sampleHeapCaps();
  dataMux_.lock();
  beginHeapLifecycle(tracker, event, before);
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::finishOwnedHeapLifecycle(
    HeapLifecycleTracker &tracker, HeapLifecycleResult result) {
  const HeapCapSnapshot after = sampleHeapCaps();
  dataMux_.lock();
  (void)finishHeapLifecycle(tracker, result, after);
  dataMux_.unlock();
}

// Network task only: refreshes and returns the shared working copy.
PersistedSettings &OpenBrewByWeightNetwork::workingSettings() {
  copySettings(g_networkWorkingSettings);
  return g_networkWorkingSettings;
}

void OpenBrewByWeightNetwork::copySettings(PersistedSettings &output) {
  dataMux_.lock();
  output = settings_;
  dataMux_.unlock();
}

NetworkSettingsSnapshot OpenBrewByWeightNetwork::networkSettingsSnapshot() {
  NetworkSettingsSnapshot output;
  TaskLockGuard lock(dataMux_);
  output.staConfigured = settings_.staConfigured;
  output.staOpen = settings_.staOpen;
  output.staWifiSleep = settings_.staWifiSleep;
  output.staConfigState = settings_.staConfigState;
  output.staIpMode = settings_.staIpMode;
  memcpy(output.staSsid, settings_.staSsid, sizeof(output.staSsid));
  memcpy(output.staPassword, settings_.staPassword, sizeof(output.staPassword));
  memcpy(output.devicePassword, settings_.devicePassword, sizeof(output.devicePassword));
  memcpy(output.deviceName, settings_.deviceName, sizeof(output.deviceName));
  memcpy(output.staIp, settings_.staIp, sizeof(output.staIp));
  memcpy(output.staNetmask, settings_.staNetmask, sizeof(output.staNetmask));
  memcpy(output.staGateway, settings_.staGateway, sizeof(output.staGateway));
  memcpy(output.staDns1, settings_.staDns1, sizeof(output.staDns1));
  memcpy(output.staDns2, settings_.staDns2, sizeof(output.staDns2));
  return output;
}

StaJoinHints OpenBrewByWeightNetwork::staJoinHints() {
  StaJoinHints hints;
  dataMux_.lock();
  hints.staConfigured = settings_.staConfigured;
  hints.staOpen = settings_.staOpen;
  hints.staConfigState = settings_.staConfigState;
  memcpy(hints.staSsid, settings_.staSsid, sizeof(hints.staSsid));
  dataMux_.unlock();
  return hints;
}

bool OpenBrewByWeightNetwork::enqueueWebhook(const WebhookEvent &event) {
  if (firmwareCompatibilityMode()) {
    return false;
  }
  return webhooks_.enqueue(event);
}

WebhookConfig OpenBrewByWeightNetwork::webhookConfig() const {
  return webhooks_.config();
}

void OpenBrewByWeightNetwork::clearStagedWebhook(uint32_t requestId) {
  dataMux_.lock();
  if (requestId == 0 || stagedWebhookRequestId_ == requestId) {
    stagedWebhook_ = WebhookConfig{};
    stagedWebhookRequestId_ = 0;
  }
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::clearStagedLineaMicra(uint32_t requestId) {
  dataMux_.lock();
  if (requestId == 0 || stagedLineaMicraRequestId_ == requestId) {
    wipeLineaMicraSettings(stagedLineaMicra_);
    stagedLineaMicraRequestId_ = 0;
  }
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::mergePreferredScaleMac(PersistedSettings &settings) {
  if (callbacks_.copyPreferredScaleMac != nullptr) {
    callbacks_.copyPreferredScaleMac(settings.preferredScaleMac,
                                     sizeof(settings.preferredScaleMac));
  }
  if (callbacks_.copyPreferredScaleName != nullptr) {
    callbacks_.copyPreferredScaleName(settings.preferredScaleName,
                                      sizeof(settings.preferredScaleName));
  }
  if (callbacks_.copyScaleHistory != nullptr) {
    callbacks_.copyScaleHistory(settings.scaleHistory);
  }
}

void OpenBrewByWeightNetwork::overlayLiveShotSettings(PersistedSettings &settings) {
  RuntimeConfig liveRuntime = settings.runtime;
  ShotPresetBank livePresets = settings.presets;
  if (callbacks_.copyRuntimeConfig != nullptr) {
    callbacks_.copyRuntimeConfig(&liveRuntime);
  }
  if (callbacks_.copyPresetBank != nullptr) {
    callbacks_.copyPresetBank(&livePresets);
  }
  overlayLivePersistedSettings(settings, liveRuntime, livePresets);
  if (callbacks_.copyBullseyeConfig != nullptr) {
    callbacks_.copyBullseyeConfig(&settings.bullseyeMelody);
  }
  mergePreferredScaleMac(settings);
}

void OpenBrewByWeightNetwork::syncPreferredScale(const char *mac, const char *name) {
  if (mac == nullptr || !validPreferredScaleMac(mac)) {
    return;
  }
  char safeName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  if (name != nullptr && validPreferredScaleName(name)) {
    copyCString(safeName, sizeof(safeName), name);
  }
  dataMux_.lock();
  copyCString(settings_.preferredScaleMac, sizeof(settings_.preferredScaleMac),
              mac);
  copyCString(settings_.preferredScaleName, sizeof(settings_.preferredScaleName),
              safeName);
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::syncLiveRuntime(const RuntimeConfig &runtime,
                                         const ShotPresetBank *presets,
                                         bool ntpSettingsChanged) {
  dataMux_.lock();
  settings_.runtime = runtime;
  if (presets != nullptr) {
    settings_.presets = *presets;
  }
  if (ntpSettingsChanged && ++ntpSettingsGeneration_ == 0)
    ++ntpSettingsGeneration_;
  dataMux_.unlock();
}

bool OpenBrewByWeightNetwork::ntpSettingsChanged(const RuntimeConfig &before,
                                            const RuntimeConfig &after) {
  return before.ntpSyncEnabled != after.ntpSyncEnabled ||
         before.ntpServerPreset != after.ntpServerPreset ||
         strncmp(before.ntpServerCustom, after.ntpServerCustom,
                 sizeof(before.ntpServerCustom)) != 0;
}

void OpenBrewByWeightNetwork::syncLiveBullseye(
    const BullseyeMelodyConfig &config) {
  dataMux_.lock();
  settings_.bullseyeMelody = config;
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::syncDurableStorageRevision(uint32_t storageRevision) {
  dataMux_.lock();
  settings_.storageRevision = storageRevision;
  dataMux_.unlock();
}

uint32_t OpenBrewByWeightNetwork::allocateRequestId() {
  dataMux_.lock();
  const uint32_t id = nextRequestId_++;
  if (nextRequestId_ == 0 || nextRequestId_ >= 0x80000000UL) {
    nextRequestId_ = 1;
  }
  dataMux_.unlock();
  return id == 0 ? 1 : id;
}

void OpenBrewByWeightNetwork::recordCommandResult(
    uint32_t requestId, CommandResultState state) {
  if (requestId == 0 || requestId >= 0x80000000UL) {
    return;
  }
  dataMux_.lock();
  if (stagedPresetRequestId_ == requestId &&
      (state == CommandResultState::PERSISTED ||
       state == CommandResultState::FAILED ||
       state == CommandResultState::CANCELED)) {
    stagedPresetRequestId_ = 0;
  }
  const bool sameRequest = status_.lastCommandRequestId == requestId;
  const bool nonRegressing =
      !sameRequest || static_cast<uint8_t>(state) >=
                          static_cast<uint8_t>(status_.lastCommandState);
  if (status_.lastCommandRequestId == 0 || (sameRequest && nonRegressing) ||
      static_cast<int32_t>(requestId - status_.lastCommandRequestId) > 0) {
    status_.lastCommandRequestId = requestId;
    status_.lastCommandState = state;
  }
  dataMux_.unlock();
}

esp_err_t OpenBrewByWeightNetwork::sendAccepted(httpd_req_t *request,
                                           uint32_t requestId,
                                           const char *extraJson) {
  recordCommandResult(requestId, CommandResultState::QUEUED);
  char response[160] = {};
  if (extraJson == nullptr) {
    snprintf(response, sizeof(response),
             "{\"accepted\":true,\"requestId\":%lu}",
             static_cast<unsigned long>(requestId));
  } else {
    snprintf(response, sizeof(response),
             "{\"accepted\":true,\"requestId\":%lu,%s}",
             static_cast<unsigned long>(requestId), extraJson);
  }
  return sendJson(request, STATUS_ACCEPTED, response);
}

esp_err_t OpenBrewByWeightNetwork::runConfirmedClear(
    httpd_req_t *request, const char *lockedDetail,
    const char *expectedConfirmation, const char *notConfirmedCode,
    const char *notConfirmedDetail, bool (*clearFn)(),
    const char *failedCode, const char *failedDetail) {
  const ControlGateSnapshot status = controlGate();
  if (!historyMutationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE", lockedDetail);
  }
  if (!lockConfirmedBody(request, "An explicit confirmation is required.",
                         expectedConfirmation)) {
    return sendError(request, STATUS_UNPROCESSABLE, notConfirmedCode,
                     notConfirmedDetail);
  }
  if (clearFn == nullptr || !clearFn()) {
    return sendError(request, STATUS_UNAVAILABLE, failedCode, failedDetail);
  }
  return sendJson(request, STATUS_OK, "{\"cleared\":true}");
}

esp_err_t OpenBrewByWeightNetwork::runDeleteById(httpd_req_t *request,
                                            const char *lockedDetail,
                                            bool (*deleteFn)(uint32_t),
                                            const char *invalidDetail,
                                            const char *notFoundCode,
                                            const char *notFoundDetail) {
  const ControlGateSnapshot status = controlGate();
  if (!historyMutationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE", lockedDetail);
  }
  const esp_err_t bodyStatus =
      lockJsonBody(request, "A bounded JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(workBuf_->requestBody);
  uint32_t id = 0;
  static const char *const fields[] = {"id"};
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonUint32(root, "id", id) && id != 0;
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD",
                     invalidDetail);
  }
  if (deleteFn == nullptr || !deleteFn(id)) {
    return sendError(request, STATUS_NOT_FOUND, notFoundCode, notFoundDetail);
  }
  return sendJson(request, STATUS_OK, "{\"deleted\":true}");
}

esp_err_t OpenBrewByWeightNetwork::enqueueAcceptedCommand(
    httpd_req_t *request, WebCommandType type, bool unsafeWebUiOverride,
    const char *queueFullDetail, const char *extraJson) {
  WebCommand command;
  command.type = type;
  command.requestId = allocateRequestId();
  command.unsafeWebUiOverride = unsafeWebUiOverride;
  if (!callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     queueFullDetail);
  }
  return sendAccepted(request, command.requestId, extraJson);
}

void OpenBrewByWeightNetwork::taskEntry(void *parameter) {
  static_cast<OpenBrewByWeightNetwork *>(parameter)->taskLoop();
}

void OpenBrewByWeightNetwork::taskLoop() {
  const bool watchdogSubscribed = subscribeCurrentTaskToWatchdog();
  if (!watchdogSubscribed) {
    callbacks_.reportTaskWatchdogFault();
  }
  uint32_t telemetryAtMs = 0;
  while (!stopRequested_.load(std::memory_order_acquire)) {
    const uint32_t taskProgressAtMs = millis();
    lastTaskProgressAtMs_.store(taskProgressAtMs, std::memory_order_relaxed);
    if (static_cast<uint32_t>(taskProgressAtMs - telemetryAtMs) >=
        HEALTH_TELEMETRY_INTERVAL_MS) {
      telemetryAtMs = taskProgressAtMs;
      taskStackMinBytes_ =
          static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    }
    service();
    if (!feedCurrentTaskWatchdog()) {
      callbacks_.reportTaskWatchdogFault();
    }
    // Critical control/scale transitions notify this low-priority task so NTP
    // is canceled immediately; normal network maintenance remains 20 Hz.
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
  }
  if (watchdogSubscribed && esp_task_wdt_delete(nullptr) != ESP_OK) {
    callbacks_.reportTaskWatchdogFault();
  }
  // HTTP/socket shutdown can wait; unsubscribe before the bounded join path
  // so an intentional stop cannot be mistaken for a hung service loop.
  stopNtp();
  stopNetwork();
  taskHandle_ = nullptr;
  if (taskStopped_) xSemaphoreGive(taskStopped_.get());
  vTaskDelete(nullptr);
}

void OpenBrewByWeightNetwork::service() {
  const uint32_t now = millis();

  // NTP and machine-API cancellation can wait inside ESP-IDF, so only this
  // low-priority task services them. Gate setters merely publish atomics.
  abortNtpForRfGate();

  // Drain CLI link mutations even when SoftAP/HTTP startup is still failing.
  // A successful AP_START / WIFI_CONNECT can mark startup complete and skip
  // the automatic retry that would otherwise fight the user's command.
  processAcceptedCommands();
  serviceWebAccess();

  // Boot confirmation must also run while a requested restart is waiting.
  serviceOtaRollback(now);

  // /setup live apply: associate the freshly saved network on this task
  // without a restart, keeping the SoftAP (and /setup) alive throughout.
  // Deferred RF gates retry here; the connect itself arms the 180 s window.
  if (setupPhase() == SetupPhase::QUEUED &&
      static_cast<int32_t>(now - liveApplyRetryAtMs_) >= 0) {
    if (!snapshot().apActive) {
      // The onboarding AP went away before the connect could start (AP_STOP
      // or teardown): fall back to the ordinary save-and-restart path.
      advanceSetupPhase(SetupEvent::APPLY_RESTARTS);
      restartPending_ = true;
      restartRequestedAtMs_ = now;
    } else {
      // A new user attempt also releases a USB WIFI_DISCONNECT hold.
      staReconnectHeld_ = false;
      if (beginStationConnect(networkSettingsSnapshot(), now)) {
        advanceSetupPhase(SetupEvent::ATTEMPT_STARTED);
        armPendingConfirmWindow(now);
      } else {
        liveApplyRetryAtMs_ = now + 1000;
      }
    }
  }

  if (restartPending_) {
    // NVS is already committed. Do not touch the radio on the way to
    // ESP.restart(): Arduino WiFi.mode(WIFI_OFF) deinits the driver
    // (esp_wifi_deinit) and tears down BT coexistence while bleTask is
    // still sending VHCI packets. The S3 restart path already resets
    // Wi-Fi/BT MAC in esp_system_reset_modules_on_exit().
    if (controlAllowsNetworkMutation() &&
        static_cast<uint32_t>(now - restartRequestedAtMs_) >=
            RESTART_DELAY_MS) {
      restartPending_ = false;
      callbacks_.requestSafeRestart();
    }
    return;
  }

  if (!startupComplete_ &&
      static_cast<int32_t>(now - networkRetryAtMs_) >= 0) {
    if (startNetwork()) {
      startupFailures_ = 0;
      networkRetryAtMs_ = 0;
      dataMux_.lock();
      status_.startupFailures = 0;
      dataMux_.unlock();
      // startStation/beginStationConnect delay and wait for STA_START, so
      // `now` is stale. serviceStaState would treat the connect timer as
      // expired (uint32 wrap) and nest SoftAP+httpd on this stack.
      return;
    } else {
      if (startupFailures_ < UINT8_MAX) {
        ++startupFailures_;
      }
      const uint8_t shift = startupFailures_ > 5 ? 5 : startupFailures_;
      uint32_t backoff = NETWORK_RETRY_MIN_MS << shift;
      if (backoff > NETWORK_RETRY_MAX_MS) {
        backoff = NETWORK_RETRY_MAX_MS;
      }
      networkRetryAtMs_ = now + backoff;
      dataMux_.lock();
      status_.startupFailures = startupFailures_;
      dataMux_.unlock();
      log(DebugCategory::NETWORK, DebugCode::NETWORK_RETRY,
          startupFailures_, static_cast<int32_t>(backoff));
    }
  }
  // Confirmation and rollback of a pending image cannot wait for the network
  // to come up: that is the very failure they exist to recover from.
  if (!startupComplete_) {
    return;
  }
  bool confirmRequested = false;
  dataMux_.lock();
  confirmRequested = pendingConfirmRequest_;
  if (confirmRequested) {
    pendingConfirmRequest_ = false;
  }
  dataMux_.unlock();
  if (confirmRequested) {
    confirmPendingNetwork("public WebUI request");
  }
  serviceStaState(now);
  applyWifiPowerSave();
  serviceWifiScan(now);
  serviceUiStream(now);
  serviceCompanion(now);

  const NetworkStatusSnapshot networkSnapshot = snapshot();
  // serviceStaState already reconciled staState against the WiFi driver
  // earlier in this pass; re-reading WiFi.status() here just repeats it.
  const bool staConnected =
      networkSnapshot.staState == StaState::CONNECTED &&
      networkSnapshot.wifiConfigured && networkSnapshot.iface.staIp[0] != '\0';
  publishMachineIntegrationNetworkState(
      staConnected, networkSnapshot.apActive,
      outboundShotActive.load(std::memory_order_acquire),
      scaleConnecting_.load(std::memory_order_acquire));
  serviceNtp(now, staConnected);

  const uint8_t apClients = networkSnapshot.apActive
                                ? static_cast<uint8_t>(
                                      WiFi.softAPgetStationNum())
                                : 0;
  dataMux_.lock();
  status_.apClients = apClients;
  dataMux_.unlock();
  serviceSoftApIdle(now, networkSnapshot.apActive, apClients,
                    controlGate().physicalActivatorOn);
  if (extendedStatusAtMs_ == 0 ||
      static_cast<uint32_t>(now - extendedStatusAtMs_) >=
          NETWORK_EXTENDED_STATUS_INTERVAL_MS) {
    extendedStatusAtMs_ = now;
    refreshExtendedStatus(now);
  }

  // Gate check is a 16-byte ControlGateSnapshot, not the 4 KiB status
  // snapshot. Do not reintroduce a stack copy here: httpd_stop() / LwIP
  // still share this frame.
  const bool safeForNetworkChange = controlAllowsNetworkMutation();
  if (apRestartPending_ && safeForNetworkChange &&
      static_cast<uint32_t>(now - restartRequestedAtMs_) >= RESTART_DELAY_MS) {
    apRestartPending_ = false;
    if (ensureAccessPoint(now, true)) {
      startupFailures_ = 0;
      dataMux_.lock();
      status_.startupFailures = 0;
      dataMux_.unlock();
    } else {
      startupComplete_ = false;
      if (startupFailures_ < UINT8_MAX) {
        ++startupFailures_;
      }
      networkRetryAtMs_ = now + NETWORK_RETRY_MIN_MS;
      dataMux_.lock();
      status_.startupFailures = startupFailures_;
      dataMux_.unlock();
      log(DebugCategory::NETWORK, DebugCode::NETWORK_RETRY,
          startupFailures_, NETWORK_RETRY_MIN_MS);
    }
  }

}

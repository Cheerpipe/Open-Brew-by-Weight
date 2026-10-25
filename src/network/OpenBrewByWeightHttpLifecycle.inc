// Linea Micra settings persist without a maintenance lease (see the control
// task's SAVE_LINEA_MICRA case); every other accepted write holds one.
static bool leaseFreeCommand(const WebCommand &command) {
  return command.type == WebCommandType::SAVE_LINEA_MICRA &&
         command.maintenanceLeaseId == 0;
}

void OpenBrewByWeightNetwork::processAcceptedCommands() {
  if (!acceptedCommandQueue_) {
    return;
  }

  if (completionPending_) {
    if (!callbacks_.enqueueWebCommand(completionCommand_)) {
      return;
    }
    completionPending_ = false;
    completionCommand_ = WebCommand{};
  }

  const uint32_t now = millis();
  if (!acceptedCommandPending_) {
    if (xQueueReceive(acceptedCommandQueue_.get(), &acceptedCommand_, 0) !=
        pdTRUE) {
      return;
    }
    acceptedCommandPending_ = true;
    acceptedCommandAttempts_ = 0;
    acceptedCommandReceivedAtMs_ = now;
    acceptedCommandRetryAtMs_ = now;
  }
  if (static_cast<int32_t>(now - acceptedCommandRetryAtMs_) < 0) {
    return;
  }

  if (isCliNetworkAction(acceptedCommand_.type)) {
    recordCommandResult(acceptedCommand_.requestId,
                        CommandResultState::RESERVED);
    ++acceptedCommandAttempts_;
    if (!processAcceptedCommand(acceptedCommand_)) {
      if (acceptedCommandAttempts_ < COMMAND_MAX_ATTEMPTS) {
        acceptedCommandRetryAtMs_ =
            now + COMMAND_RETRY_MIN_MS * acceptedCommandAttempts_;
        log(DebugCategory::SECURITY, DebugCode::COMMAND_RETRY,
            acceptedCommand_.requestId, acceptedCommandAttempts_);
        return;
      }
      recordCommandResult(acceptedCommand_.requestId,
                          CommandResultState::FAILED);
      actionLogf("ERR %s failed after retries",
                 webCommandTypeName(acceptedCommand_.type));
      acceptedCommandPending_ = false;
      acceptedCommand_ = WebCommand{};
      return;
    }
    recordCommandResult(acceptedCommand_.requestId,
                        CommandResultState::APPLIED);
    noteCliNetworkProgress();
    acceptedCommandPending_ = false;
    acceptedCommand_ = WebCommand{};
    return;
  }

  if (acceptedCommand_.type == WebCommandType::MAINTENANCE_COMPLETE &&
      acceptedCommand_.maintenanceLeaseId == 0) {
    const CommandResultState terminalState =
        acceptedCommand_.resultState == CommandResultState::APPLIED ||
                acceptedCommand_.resultState == CommandResultState::PERSISTED ||
                acceptedCommand_.resultState == CommandResultState::FAILED ||
                acceptedCommand_.resultState == CommandResultState::CANCELED
            ? acceptedCommand_.resultState
            : (acceptedCommand_.succeeded ? CommandResultState::APPLIED
                                          : CommandResultState::CANCELED);
    recordCommandResult(
        acceptedCommand_.requestId, terminalState);
    acceptedCommandPending_ = false;
    acceptedCommand_ = WebCommand{};
    return;
  }

  // Maintenance commands used to keep a ControlStatusSnapshot in this
  // frame. Gate fields live in ControlGateSnapshot so WEBUI_RESTART does not
  // carry that object into httpd_stop() / LwIP.
  processAcceptedMaintenanceCommand(now);
}

void OpenBrewByWeightNetwork::processAcceptedMaintenanceCommand(uint32_t now) {
  const ControlGateSnapshot control = controlGate();
  const bool matchingLease =
      leaseFreeCommand(acceptedCommand_) ||
      (acceptedCommand_.maintenanceLeaseId != 0 &&
       control.maintenanceLeaseActive &&
       control.maintenanceLeaseId == acceptedCommand_.maintenanceLeaseId);
  if (!matchingLease) {
    // The control task publishes the lease after enqueueing the command. Wait
    // for that publication, but reject a lease that was canceled/replaced.
    const bool publicationTimedOut =
        static_cast<uint32_t>(now - acceptedCommandReceivedAtMs_) >=
        MAINTENANCE_PUBLICATION_TIMEOUT_MS;
    if (control.maintenanceLeaseActive || control.activeCycle ||
        control.physicalActivatorOn || publicationTimedOut) {
      if (acceptedCommand_.type == WebCommandType::SAVE_WEBHOOK) {
        clearStagedWebhook(acceptedCommand_.stagedConfigRequestId);
      }
      if (acceptedCommand_.type == WebCommandType::SAVE_LINEA_MICRA) {
        clearStagedLineaMicra(acceptedCommand_.stagedConfigRequestId);
      }
      if (acceptedCommand_.type == WebCommandType::START_WIFI_SCAN) {
        // The handler published an optimistic QUEUED for this command; the
        // scan itself never starts, so retire that state for the clients.
        abortWifiScan(now, false);
      }
      (void)enqueueMaintenanceCompletion(
          acceptedCommand_, false, CommandResultState::CANCELED);
      acceptedCommandPending_ = false;
      acceptedCommand_ = WebCommand{};
    }
    return;
  }
  if (control.activeCycle || control.relayClosed ||
      control.physicalActivatorOn) {
    if (acceptedCommand_.type == WebCommandType::SAVE_WEBHOOK) {
      clearStagedWebhook(acceptedCommand_.stagedConfigRequestId);
    }
    if (acceptedCommand_.type == WebCommandType::SAVE_LINEA_MICRA) {
      clearStagedLineaMicra(acceptedCommand_.stagedConfigRequestId);
    }
    if (acceptedCommand_.type == WebCommandType::START_WIFI_SCAN) {
      abortWifiScan(now, false);
    }
    (void)enqueueMaintenanceCompletion(
        acceptedCommand_, false, CommandResultState::CANCELED);
    acceptedCommandPending_ = false;
    acceptedCommand_ = WebCommand{};
    return;
  }

  recordCommandResult(acceptedCommand_.requestId,
                      CommandResultState::RESERVED);
  ++acceptedCommandAttempts_;
  if (!processAcceptedCommand(acceptedCommand_)) {
    if (acceptedCommandAttempts_ < COMMAND_MAX_ATTEMPTS) {
      const uint32_t backoff =
          COMMAND_RETRY_MIN_MS * acceptedCommandAttempts_;
      acceptedCommandRetryAtMs_ = now + backoff;
      log(DebugCategory::SECURITY, DebugCode::COMMAND_RETRY,
          acceptedCommand_.requestId, acceptedCommandAttempts_);
      return;
    }
    if (acceptedCommand_.type == WebCommandType::SAVE_WEBHOOK) {
      clearStagedWebhook(acceptedCommand_.stagedConfigRequestId);
    }
    if (acceptedCommand_.type == WebCommandType::SAVE_LINEA_MICRA) {
      clearStagedLineaMicra(acceptedCommand_.stagedConfigRequestId);
    }
    (void)enqueueMaintenanceCompletion(acceptedCommand_, false);
    log(DebugCategory::SECURITY, DebugCode::COMMAND_FAILED,
        acceptedCommand_.requestId, acceptedCommandAttempts_);
    acceptedCommandPending_ = false;
    acceptedCommand_ = WebCommand{};
    return;
  }

  if (acceptedCommand_.type != WebCommandType::START_WIFI_SCAN) {
    (void)enqueueMaintenanceCompletion(acceptedCommand_, true);
  }
  acceptedCommandPending_ = false;
  acceptedCommand_ = WebCommand{};
}

bool OpenBrewByWeightNetwork::enqueueMaintenanceCompletion(
    const WebCommand &command, bool succeeded,
    CommandResultState failureState) {
  WebCommand result;
  result.type = WebCommandType::MAINTENANCE_COMPLETE;
  result.requestId = command.requestId;
  result.maintenanceLeaseId = command.maintenanceLeaseId;
  result.succeeded = succeeded;
  recordCommandResult(
      command.requestId,
      succeeded ? (command.type == WebCommandType::PERSIST_RUNTIME ||
                           command.type == WebCommandType::SAVE_NETWORK ||
                           command.type == WebCommandType::SAVE_WEBHOOK ||
                           command.type == WebCommandType::FORGET_NETWORK ||
                           command.type == WebCommandType::CHANGE_DEVICE_PASSWORD ||
                           command.type == WebCommandType::RESET_DEVICE_PASSWORD ||
                           command.type == WebCommandType::RESET_NETWORK_AP ||
                           command.type == WebCommandType::FACTORY_RESET ||
                           command.type == WebCommandType::CLEAR_RESET_HISTORY ||
                           command.type == WebCommandType::RESTART
                       ? CommandResultState::PERSISTED
                       : CommandResultState::APPLIED)
                : failureState);
  // A lease-free command has no lease for the control task to release.
  if (leaseFreeCommand(command) || callbacks_.enqueueWebCommand(result)) {
    return true;
  }
  completionCommand_ = result;
  completionPending_ = true;
  return false;
}

bool OpenBrewByWeightNetwork::processAcceptedCommand(const WebCommand &command) {
  if (isCliNetworkAction(command.type)) {
    return handleCliNetworkAction(command, millis());
  }
  if (command.type == WebCommandType::START_WIFI_SCAN) {
    dataMux_.lock();
    g_wifiScan = WifiScanSnapshot{};
    g_wifiScan.state = WifiScanState::QUEUED;
    g_wifiScan.updatedAtMs = millis();
    scanRequested_ = true;
    scanMaintenanceLeaseId_ = command.maintenanceLeaseId;
    scanRequestId_ = command.requestId;
    dataMux_.unlock();
    return true;
  }
  return processPersistedCommand(command);
}

bool OpenBrewByWeightNetwork::processPersistedCommand(const WebCommand &command) {
  if (command.type == WebCommandType::CLEAR_RESET_HISTORY) {
    return callbacks_.clearResetHistory(command.resetHistoryUnsafeCount);
  }
  PersistedSettings &next = workingSettings();
  char previousDeviceName[DEVICE_NAME_CAPACITY] = {};
  memcpy(previousDeviceName, next.deviceName, sizeof(previousDeviceName));
  bool persist = false;
  bool factoryReset = false;
  bool applyWifiPsAfterPersist = false;
  switch (command.type) {
    case WebCommandType::PERSIST_RUNTIME:
      if (command.persistPresets && callbacks_.copyPresetBank != nullptr) {
        callbacks_.copyPresetBank(&next.presets);
      }
      {
        const RuntimeConfig composed =
            composeEffectiveConfig(command.config, next.presets);
        if (validateRuntimeConfig(composed) != ConfigValidationError::NONE) {
          log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
          return false;
        }
      }
      next.runtime = command.config;
      persist = true;
      break;

    case WebCommandType::SAVE_WEBHOOK: {
      WebhookConfig staged = {};
      dataMux_.lock();
      const bool matches = stagedWebhookRequestId_ != 0 &&
                           stagedWebhookRequestId_ == command.stagedConfigRequestId;
      if (matches) staged = stagedWebhook_;
      dataMux_.unlock();
      if (!matches || !validWebhookConfig(staged)) {
        log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
        return false;
      }
      next.webhook = staged;
      persist = true;
      break;
    }

    case WebCommandType::SAVE_LINEA_MICRA: {
      LineaMicraPersistedSettings staged = {};
      dataMux_.lock();
      const bool matches = stagedLineaMicraRequestId_ != 0 &&
                           stagedLineaMicraRequestId_ ==
                               command.stagedConfigRequestId;
      if (matches) staged = stagedLineaMicra_;
      dataMux_.unlock();
      if (!matches || !validLineaMicraSettings(staged)) {
        log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
        return false;
      }
      // A Settings-page change lives in the PSRAM record; the deferred
      // settings persist decides when it reaches flash.
      dataMux_.lock();
      settings_.lineaMicra = staged;
      dataMux_.unlock();
      next.lineaMicra = staged;
      wipeLineaMicraSettings(staged);
      clearStagedLineaMicra(command.stagedConfigRequestId);
      publishMachineIntegrationConfig(next, next.storageRevision);
      networkSettingsPersistRequested.store(true, std::memory_order_release);
      return true;
    }

    case WebCommandType::SAVE_NETWORK: {
      char password[WIFI_PASSWORD_CAPACITY] = {};
      const bool reusePassword = shouldReuseSavedWifiCredentials(
          command.network.ssid, command.network.password, command.network.openNetwork,
          next.staConfigured, next.staSsid, next.staOpen);
      copyCString(password, sizeof(password),
                  reusePassword ? next.staPassword : command.network.password);
      if (!validWifiSsid(command.network.ssid) ||
          !validWifiPassword(password, command.network.openNetwork) ||
          !validStaAddressConfig(command.network.staIpMode, command.network.staIp,
                                 command.network.staNetmask, command.network.staGateway,
                                 command.network.staDns1, command.network.staDns2)) {
        memset(password, 0, sizeof(password));
        log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
        return false;
      }
      const bool sameCredentials =
          next.staConfigured && reusePassword &&
          strcmp(next.staSsid, command.network.ssid) == 0 &&
          next.staOpen == command.network.openNetwork &&
          next.staIpMode == command.network.staIpMode &&
          memcmp(next.staIp, command.network.staIp, sizeof(next.staIp)) == 0 &&
          memcmp(next.staNetmask, command.network.staNetmask,
                 sizeof(next.staNetmask)) == 0 &&
          memcmp(next.staGateway, command.network.staGateway,
                 sizeof(next.staGateway)) == 0 &&
          memcmp(next.staDns1, command.network.staDns1, sizeof(next.staDns1)) == 0 &&
          memcmp(next.staDns2, command.network.staDns2, sizeof(next.staDns2)) == 0;
      // Specified sleep and/or device name + identical STA: persist the
      // delta if anything changed, otherwise no-op. Never restart (a second
      // Save must not go PENDING).
      if (sameCredentials &&
          (command.network.wifiSleepSpecified ||
           command.network.deviceNameSpecified)) {
        memset(password, 0, sizeof(password));
        bool preferenceChanged = false;
        if (command.network.deviceNameSpecified &&
            strcmp(next.deviceName, command.network.deviceName) != 0) {
          if (!validDeviceName(command.network.deviceName)) {
            log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
            return false;
          }
          memset(next.deviceName, 0, sizeof(next.deviceName));
          copyCString(next.deviceName, sizeof(next.deviceName),
                      command.network.deviceName);
          preferenceChanged = true;
        }
        if (command.network.wifiSleepSpecified &&
            next.staWifiSleep != command.network.wifiSleep) {
          next.staWifiSleep = command.network.wifiSleep;
          preferenceChanged = true;
          applyWifiPsAfterPersist = true;
        }
        if (preferenceChanged) {
          persist = true;
        }
        break;
      }
      if (!command.network.commitConfirmed && next.staConfigured &&
          next.staConfigState ==
              static_cast<uint8_t>(StaConfigState::CONFIRMED)) {
        copyActiveStaToLkg(next);
      }
      next.staConfigured = true;
      next.staOpen = command.network.openNetwork;
      if (command.network.wifiSleepSpecified) {
        next.staWifiSleep = command.network.wifiSleep;
      }
      if (command.network.deviceNameSpecified) {
        memset(next.deviceName, 0, sizeof(next.deviceName));
        copyCString(next.deviceName, sizeof(next.deviceName),
                    command.network.deviceName);
      }
      memset(next.staSsid, 0, sizeof(next.staSsid));
      memset(next.staPassword, 0, sizeof(next.staPassword));
      copyCString(next.staSsid, sizeof(next.staSsid), command.network.ssid);
      copyCString(next.staPassword, sizeof(next.staPassword), password);
      memset(password, 0, sizeof(password));
      next.staIpMode = command.network.staIpMode;
      memcpy(next.staIp, command.network.staIp, sizeof(next.staIp));
      memcpy(next.staNetmask, command.network.staNetmask, sizeof(next.staNetmask));
      memcpy(next.staGateway, command.network.staGateway, sizeof(next.staGateway));
      memcpy(next.staDns1, command.network.staDns1, sizeof(next.staDns1));
      memcpy(next.staDns2, command.network.staDns2, sizeof(next.staDns2));
      finalizeSavedStaCredentials(next, command.network.commitConfirmed);
      persist = true;
      if (command.network.applyLive) {
        // /setup onboarding: the network task consumes the flag and connects
        // live under the SoftAP (no restart).
        restartPending_ = false;
        advanceSetupPhase(SetupEvent::APPLY_SAVED);
        liveApplyRetryAtMs_ = 0;
      } else {
        restartPending_ = true;
      }
      break;
    }

    case WebCommandType::FORGET_NETWORK:
      clearStaNetwork(next);
      persist = true;
      restartPending_ = true;
      break;

    case WebCommandType::CHANGE_DEVICE_PASSWORD:
      if (!setDevicePassword(next, command.network.password)) {
        log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
        return false;
      }
      persist = true;
      apRestartPending_ = snapshot().apActive && !command.network.applyLive;
      break;

    case WebCommandType::RESET_DEVICE_PASSWORD:
      if (!initializeDefaultDevicePassword(next)) {
        return false;
      }
      persist = true;
      apRestartPending_ = snapshot().apActive;
      log(DebugCategory::SECURITY, DebugCode::DEVICE_PASSWORD_RESET);
      break;

    case WebCommandType::RESET_NETWORK_AP:
      clearStaNetwork(next);
      if (!initializeDefaultDevicePassword(next)) {
        return false;
      }
      persist = true;
      restartPending_ = true;
      log(DebugCategory::SECURITY, DebugCode::NETWORK_RESET);
      break;

    case WebCommandType::FACTORY_RESET:
      if (callbacks_.resetAllDurableStores == nullptr ||
          callbacks_.releaseNvsSpaceForFactoryReset == nullptr) {
        return false;
      }
      if (!ensureFactoryResetIntent(
              [&]() { return callbacks_.releaseNvsSpaceForFactoryReset(); })) {
        return false;
      }
      if (!callbacks_.resetAllDurableStores(next)) {
        if (!otaRollbackRestartPending_) {
          apRestartPending_ = false;
        }
        log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
        return false;
      }
      (void)clearRecoveryIntent();
      factoryReset = true;
      restartPending_ = true;
      log(DebugCategory::SECURITY, DebugCode::FACTORY_RESET);
      break;

    case WebCommandType::RESTART:
      persist = true;
      restartPending_ = true;
      log(DebugCategory::SECURITY, DebugCode::RESTART_REQUESTED);
      break;

    default:
      // Network-command types (Wi-Fi, AP, WebUI, scan) are owned by the
      // network task, not the control task: they must not persist here and
      // always reach this default. Keep the enum list single-sourced in
      // isCliNetworkAction().
      return false;
  }

  if (persist) {
    overlayLiveShotSettings(next);
    if (!savePersistedSettings(next)) {
      const bool restartAnyway = command.type == WebCommandType::RESTART;
      if (!otaRollbackRestartPending_ && !restartAnyway) {
        restartPending_ = false;
      }
      apRestartPending_ = false;
      if (restartAnyway) {
        // A broken/full NVS must not strand a committed OTA image or make
        // restart unavailable as a recovery action. Live unsaved changes may
        // be lost, but the last verified durable settings remain intact.
        restartRequestedAtMs_ = millis();
        return true;
      }
      // NVS write failure is not a config validation reject. PERSIST_RUNTIME
      // failures are reported clearly when the maintenance lease completes.
      if (command.type != WebCommandType::PERSIST_RUNTIME) {
        log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
      }
      return false;
    }
    dataMux_.lock();
    settings_ = next;
    status_.wifiConfigured = next.staConfigured;
    status_.staConfigState = next.staConfigState;
    dataMux_.unlock();
    if (command.type == WebCommandType::SAVE_WEBHOOK) {
      webhooks_.setConfig(next.webhook);
      clearStagedWebhook(command.stagedConfigRequestId);
    }
    publishMachineIntegrationConfig(next, next.storageRevision);
    publishConfiguredAddressStatus();
    if (strcmp(previousDeviceName, next.deviceName) != 0)
      applyMdnsName();
    log(DebugCategory::CONFIG, DebugCode::CONFIG_PERSISTED,
        static_cast<int32_t>(next.runtime.revision));
    if (command.type == WebCommandType::CHANGE_DEVICE_PASSWORD) {
      // Every other Web UI session signs in again; Home Assistant keeps its
      // own token. A USB change has no session to keep.
      uint8_t keep[WEB_ACCESS_TOKEN_BYTES] = {};
      bool keepValid = false;
      dataMux_.lock();
      memcpy(keep, passwordKeepToken_, sizeof(keep));
      keepValid = passwordKeepValid_ && passwordKeepRequestId_ == command.requestId;
      memset(passwordKeepToken_, 0, sizeof(passwordKeepToken_));
      passwordKeepValid_ = false;
      dataMux_.unlock();
      webAccessSignOut(keepValid ? keep : nullptr, false);
      memset(keep, 0, sizeof(keep));
    } else if (command.type == WebCommandType::RESET_DEVICE_PASSWORD ||
               command.type == WebCommandType::RESET_NETWORK_AP) {
      webAccessSignOut(nullptr, true);
    }
  } else if (factoryReset) {
    dataMux_.lock();
    g_webAccess = WebAccessState{};  // the durable reset erased its record
    settings_ = next;
    status_.wifiConfigured = false;
    status_.staConfigState =
        static_cast<uint8_t>(StaConfigState::CONFIRMED);
    dataMux_.unlock();
    publishConfiguredAddressStatus();
    if (strcmp(previousDeviceName, next.deviceName) != 0)
      applyMdnsName();
    publishMachineIntegrationConfig(next, next.storageRevision);
    log(DebugCategory::CONFIG, DebugCode::CONFIG_PERSISTED,
        static_cast<int32_t>(next.runtime.revision));
  }

  if (restartPending_ || apRestartPending_) {
    restartRequestedAtMs_ = millis();
  }
  if (applyWifiPsAfterPersist) {
    applyWifiPowerSave();
  }
  return true;
}

bool OpenBrewByWeightNetwork::handleCliNetworkAction(const WebCommand &command,
                                                uint32_t now) {
  refreshExtendedStatus(now);
  switch (command.type) {
    case WebCommandType::WIFI_CONNECT:
    case WebCommandType::WIFI_DISCONNECT:
    case WebCommandType::WIFI_RESTART:
      return handleCliWifiAction(command, now);
    case WebCommandType::AP_START:
    case WebCommandType::AP_STOP:
      return handleCliApAction(command, now);
    case WebCommandType::WEBUI_START:
    case WebCommandType::WEBUI_STOP:
    case WebCommandType::WEBUI_RESTART:
      return handleCliWebUiAction(command, now);
    default:
      return false;
  }
}

bool OpenBrewByWeightNetwork::handleCliWifiAction(const WebCommand &command,
                                             uint32_t now) {
  switch (command.type) {
    case WebCommandType::WIFI_CONNECT: {
      const NetworkSettingsSnapshot settings = networkSettingsSnapshot();
      if (!settings.staConfigured) {
        actionLog("ERR no STA configured; use SET_WIFI");
        printActionSnapshot("WIFI_CONNECT", false);
        return true;
      }
      const NetworkStatusSnapshot status = snapshot();
      if (status.staState == StaState::CONNECTED &&
          WiFi.status() == WL_CONNECTED) {
        actionLog("OK already connected");
        printActionSnapshot("WIFI_CONNECT", true);
        return true;
      }
      staReconnectHeld_ = false;
      if (!beginStationConnect(settings, now)) {
        actionLog("WIFI_CONNECT deferred; brew RF active");
        printActionSnapshot("WIFI_CONNECT", false);
        return true;
      }
      actionLog("WIFI_CONNECT associating saved STA");
      printActionSnapshot("WIFI_CONNECT", true);
      return true;
    }
    case WebCommandType::WIFI_DISCONNECT: {
      staReconnectHeld_ = true;
      WiFi.disconnect(false, false);
      stopNtp();
      staNtpEligibleAtMs_ = 0;
      g_wallClock.markDisabled();
      dataMux_.lock();
      status_.staState = status_.wifiConfigured ? StaState::DISCONNECTED
                                                : StaState::NOT_CONFIGURED;
      status_.iface.staIp[0] = '\0';
      clearStaLinkMetrics();
      dataMux_.unlock();
      actionLog("WIFI_DISCONNECT STA down; reconnect held");
      printActionSnapshot("WIFI_DISCONNECT", true);
      return true;
    }
    case WebCommandType::WIFI_RESTART: {
      const NetworkSettingsSnapshot settings = networkSettingsSnapshot();
      if (!settings.staConfigured) {
        actionLog("ERR no STA configured; use SET_WIFI");
        printActionSnapshot("WIFI_RESTART", false);
        return true;
      }
      staReconnectHeld_ = false;
      stopNtp();
      staNtpEligibleAtMs_ = 0;
      g_wallClock.markDisabled();
      dataMux_.lock();
      status_.staState = StaState::DISCONNECTED;
      status_.iface.staIp[0] = '\0';
      clearStaLinkMetrics();
      dataMux_.unlock();
      if (!beginStationConnect(settings, now)) {
        actionLog("WIFI_RESTART deferred; brew RF active");
        printActionSnapshot("WIFI_RESTART", false);
        return true;
      }
      actionLog("WIFI_RESTART reconnecting saved STA");
      printActionSnapshot("WIFI_RESTART", true);
      return true;
    }
    default:
      return false;
  }
}

bool OpenBrewByWeightNetwork::handleCliApAction(const WebCommand &command,
                                           uint32_t now) {
  switch (command.type) {
    case WebCommandType::AP_START: {
      apStartHeld_ = false;
      const SetupPhase kept = advanceSetupPhase(SetupEvent::KEEP_REQUESTED);
      const bool ok = ensureAccessPoint(now, true);
      if (!ok && kept == SetupPhase::KEEP) {
        advanceSetupPhase(SetupEvent::KEEP_RELEASED);
      }
      actionLog(ok ? "AP_START SoftAP requested" : "ERR AP_START failed");
      printActionSnapshot("AP_START", ok);
      return true;
    }
    case WebCommandType::AP_STOP: {
      apStartHeld_ = true;
      advanceSetupPhase(SetupEvent::KEEP_RELEASED);
      const bool staUp = snapshot().staState == StaState::CONNECTED &&
                         WiFi.status() == WL_CONNECTED;
      if (staUp) {
        stopSoftApLeaveHttp();
      } else {
        stopSoftAp(true);
      }
      actionLog("AP_STOP SoftAP down; auto-raise held");
      printActionSnapshot("AP_STOP", true);
      return true;
    }
    default:
      return false;
  }
}

bool OpenBrewByWeightNetwork::handleCliWebUiAction(const WebCommand &command,
                                              uint32_t now) {
  (void)now;
  switch (command.type) {
    case WebCommandType::WEBUI_START: {
      httpStartHeld_ = false;
      if (server_ != nullptr) {
        actionLog("OK already running");
        printActionSnapshot("WEBUI_START", true);
        return true;
      }
      const bool ok = startHttpServer();
      dataMux_.lock();
      status_.networkActive = ok && (status_.apActive ||
                                     status_.staState == StaState::CONNECTED);
      status_.httpActive = ok;
      dataMux_.unlock();
      actionLog(ok ? "WEBUI_START httpd up" : "ERR WEBUI_START httpd_start failed");
      if (!ok) {
        actionLogf("WEBUI_START heap hint free=%u",
                   static_cast<unsigned>(ESP.getFreeHeap()));
      }
      printActionSnapshot("WEBUI_START", ok);
      return true;
    }
    case WebCommandType::WEBUI_STOP: {
      httpStartHeld_ = true;
      stopHttpServer();
      dataMux_.lock();
      status_.networkActive = false;
      status_.httpActive = false;
      dataMux_.unlock();
      actionLog("WEBUI_STOP httpd down; auto-start held");
      printActionSnapshot("WEBUI_STOP", true);
      return true;
    }
    case WebCommandType::WEBUI_RESTART: {
      httpStartHeld_ = false;
      stopHttpServer();
      const bool ok = startHttpServer();
      dataMux_.lock();
      status_.networkActive = ok && (status_.apActive ||
                                     status_.staState == StaState::CONNECTED);
      status_.httpActive = ok;
      dataMux_.unlock();
      actionLog(ok ? "WEBUI_RESTART httpd up" : "ERR WEBUI_RESTART failed");
      if (!ok) {
        actionLogf("WEBUI_RESTART heap hint free=%u",
                   static_cast<unsigned>(ESP.getFreeHeap()));
      }
      printActionSnapshot("WEBUI_RESTART", ok);
      return true;
    }
    default:
      return false;
  }
}

void OpenBrewByWeightNetwork::log(DebugCategory category, DebugCode code,
                             int32_t argument1, int32_t argument2) {
  if (callbacks_.addDebugEvent != nullptr) {
    callbacks_.addDebugEvent(category, code, argument1, argument2);
  }
}

void OpenBrewByWeightNetwork::actionLog(const char *message) {
  if (message != nullptr) {
    ::serialTraceCategory(networkTextLogLevel(message), DebugCategory::NETWORK,
                          message);
  }
}

void OpenBrewByWeightNetwork::actionLogf(const char *fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  char line[DEBUG_EVENT_TEXT_CAPACITY] = {};
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  ::serialTraceCategory(networkTextLogLevel(line), DebugCategory::NETWORK,
                        line);
}

void OpenBrewByWeightNetwork::lifecycleLog(const char *message) {
  if (message != nullptr) {
    ::serialTraceCategory(networkTextLogLevel(message), DebugCategory::NETWORK,
                          message);
  }
}

void OpenBrewByWeightNetwork::lifecycleLogf(const char *fmt, ...) {
  if (fmt == nullptr) {
    return;
  }
  char line[DEBUG_EVENT_TEXT_CAPACITY] = {};
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  ::serialTraceCategory(networkTextLogLevel(line), DebugCategory::NETWORK,
                        line);
}

void OpenBrewByWeightNetwork::printActionSnapshot(const char *command, bool ok) {
  refreshExtendedStatus(millis());
  const NetworkStatusSnapshot status = snapshot();
  actionLogf("%s %s ssid=%s mode=%s wifiStatus=%ld %s sta=%s ip=%s rssi=%d "
             "ap=%s apIp=%s clients=%u http=%s holds sta/ap/http=%d/%d/%d",
             command != nullptr ? command : "NETWORK",
             ok ? "OK" : "ERR",
             status_.iface.staSsid[0] != '\0' ? status_.iface.staSsid : "-",
             serialCliWifiModeName(status.wifiMode),
             static_cast<long>(status.wifiStatus),
             serialCliWlStatusName(status.wifiStatus),
             staStateName(status.staState),
             status_.iface.staIp[0] != '\0' ? status_.iface.staIp : "-",
             status.staLinkMetricsValid ? static_cast<int>(status.staRssi) : 0,
             status.apActive ? "up" : "down",
             status_.iface.apIp,
             static_cast<unsigned>(status.apClients),
             status.httpActive ? "up" : "down",
             status.staReconnectHeld ? 1 : 0,
             status.apStartHeld ? 1 : 0,
             status.httpStartHeld ? 1 : 0);
}

void OpenBrewByWeightNetwork::noteCliNetworkProgress() {
  if (startupComplete_) {
    return;
  }
  const NetworkStatusSnapshot status = snapshot();
  if (!status.apActive && server_ == nullptr &&
      status.staState != StaState::CONNECTING &&
      status.staState != StaState::CONNECTED) {
    return;
  }
  startupComplete_ = true;
  startupFailures_ = 0;
  networkRetryAtMs_ = 0;
  dataMux_.lock();
  status_.startupFailures = 0;
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::refreshExtendedStatus(uint32_t now) {
  const TimeStatusSnapshot timeStatus = g_wallClock.snapshot(now);
  const wl_status_t wifiStatus = WiFi.status();
  const bool staConnected = wifiStatus == WL_CONNECTED;
  const uint8_t wifiMode = static_cast<uint8_t>(WiFi.getMode());
  const uint8_t channel = static_cast<uint8_t>(WiFi.channel());
  WifiPsLive wifiPs = WifiPsLive::UNKNOWN;
  if (wifiMode != static_cast<uint8_t>(WIFI_OFF)) {
    wifi_ps_type_t ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&ps) == ESP_OK) {
      switch (ps) {
        case WIFI_PS_NONE:
          wifiPs = WifiPsLive::NONE;
          break;
        case WIFI_PS_MIN_MODEM:
          wifiPs = WifiPsLive::MIN_MODEM;
          break;
        case WIFI_PS_MAX_MODEM:
          wifiPs = WifiPsLive::MAX_MODEM;
          break;
        default:
          wifiPs = WifiPsLive::UNKNOWN;
          break;
      }
    }
  }
  char staMac[18] = {};
  char staBssid[18] = {};
  char apMac[18] = {};
  uint8_t mac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
    formatWifiMac(mac, staMac, sizeof(staMac));
  }
  if (staConnected) {
    const uint8_t *bssid = WiFi.BSSID();
    if (bssid != nullptr) {
      formatWifiMac(bssid, staBssid, sizeof(staBssid));
    }
  }
  if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
    formatWifiMac(mac, apMac, sizeof(apMac));
  }
  char apSsid[WIFI_SSID_CAPACITY] = {};
  fillSoftApSsid(apSsid, sizeof(apSsid));
  const bool ntpArm = ntpMayArm(now, staConnected);
  const bool httpActive = server_ != nullptr;
  const bool factoryPassword = passwordIsFactoryDefault(settings_);
  dataMux_.lock();
  status_.httpActive = httpActive;
  status_.wifiMode = wifiMode;
  status_.channel = channel;
  status_.wifiPs = wifiPs;
  status_.wifiCoex = snapshotRfCoexPreference();
  status_.wifiStatus = static_cast<int32_t>(wifiStatus);
  status_.staReconnectHeld =
      staReconnectHeld_ || setupHoldsReconnect(status_.setupPhase);
  status_.apStartHeld = apStartHeld_;
  status_.httpStartHeld = httpStartHeld_;
  status_.devicePasswordFactory = factoryPassword;
  status_.scanState = static_cast<uint8_t>(g_wifiScan.state);
  status_.ntpState = static_cast<uint8_t>(timeStatus.state);
  status_.ntpMayArm = ntpArm;
  status_.staConnectAgeMs =
      staConnectStartedAtMs_ != 0
          ? static_cast<uint32_t>(now - staConnectStartedAtMs_)
          : 0;
  status_.staReconnectAgeMs =
      staReconnectAttemptAtMs_ != 0
          ? static_cast<uint32_t>(now - staReconnectAttemptAtMs_)
          : 0;
  status_.staReconnectDelayMs = staReconnectIntervalMs_;
  memset(status_.iface.staMac, 0, sizeof(status_.iface.staMac));
  memset(status_.iface.staBssid, 0, sizeof(status_.iface.staBssid));
  memset(status_.iface.apMac, 0, sizeof(status_.iface.apMac));
  memset(status_.iface.apSsid, 0, sizeof(status_.iface.apSsid));
  copyCString(status_.iface.staMac, sizeof(status_.iface.staMac), staMac);
  copyCString(status_.iface.staBssid, sizeof(status_.iface.staBssid), staBssid);
  copyCString(status_.iface.apMac, sizeof(status_.iface.apMac), apMac);
  copyCString(status_.iface.apSsid, sizeof(status_.iface.apSsid), apSsid);
  memset(status_.iface.ntpActiveServer, 0, sizeof(status_.iface.ntpActiveServer));
  copyCString(status_.iface.ntpActiveServer, sizeof(status_.iface.ntpActiveServer),
              timeStatus.activeServer);
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::ntpSyncNotificationCallback(struct timeval *tv) {
  OpenBrewByWeightNetwork *self = instance_;
  if (tv == nullptr || self == nullptr ||
      !self->ntpCallbackAccepting_.load(std::memory_order_acquire) ||
      outboundScaleInhibited()) {
    return;
  }
  g_wallClock.queueSyncFromCallback(static_cast<uint32_t>(tv->tv_sec));
  if (self->taskHandle_ != nullptr) {
    xTaskNotifyGive(self->taskHandle_);
  }
}

void OpenBrewByWeightNetwork::stopNtp() {
  // Close the callback gate before stopping lwIP so a late completion from an
  // aborted attempt cannot be accepted after a shot/scale gate transition.
  ntpCallbackAccepting_.store(false, std::memory_order_release);
  if (ntpStarted_ || esp_sntp_enabled()) {
    esp_sntp_stop();
    ntpStarted_ = false;
  }
  completeOutboundAcquisitionPause(OutboundClient::NTP, millis());
}

bool OpenBrewByWeightNetwork::ntpMayArm(uint32_t now, bool staConnected) const {
  if (!staConnected || server_ == nullptr) {
    return false;
  }
  if (brewRfActive()) {
    return false;
  }
  if (outboundScaleInhibited() || scaleConnecting_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (staNtpEligibleAtMs_ != 0 &&
      static_cast<int32_t>(now - staNtpEligibleAtMs_) < 0) {
    return false;
  }
  return true;
}

void OpenBrewByWeightNetwork::abortInFlightNtp() {
  stopNtp();
  g_wallClock.cancelSyncing();
  if (ntpManualAttempt_) {
    ntpManualAttempt_ = false;
    ntpManualSyncPending_ = true;
  } else {
    ntpRearmPending_ = true;
  }
}

void OpenBrewByWeightNetwork::abortNtpForRfGate() {
  const bool gateActive =
      brewRfActive() || outboundScaleInhibited() || scaleConnecting_.load(std::memory_order_acquire);
  const bool abortRequested =
      ntpAbortRequested_.exchange(false, std::memory_order_acq_rel);
  if (!gateActive && !abortRequested) {
    return;
  }
  const TimeStatusSnapshot timeStatus = g_wallClock.snapshot(millis());
  if (ntpStarted_ || timeStatus.state == TimeSyncState::SYNCING) {
    abortInFlightNtp();
  } else {
    g_wallClock.cancelPendingSync();
  }
  completeOutboundAcquisitionPause(OutboundClient::NTP, millis());
}

bool OpenBrewByWeightNetwork::armNtp(uint32_t now, bool staConnected,
                                uint32_t expectedGateGeneration) {
  auto gateStable = [&]() {
    return rfGateGeneration_.load(std::memory_order_acquire) ==
               expectedGateGeneration &&
           ntpMayArm(millis(), staConnected);
  };
  if (!gateStable()) return false;

  // One locked snapshot of the NTP runtime fields: the control task rewrites
  // settings_.runtime wholesale under dataMux_, so reading it unlocked here
  // would race a torn copy.
  uint8_t ntpServerPreset = 0;
  char ntpServerCustom[NTP_SERVER_HOST_CAPACITY] = {};
  dataMux_.lock();
  ntpServerPreset = settings_.runtime.ntpServerPreset;
  memcpy(ntpServerCustom, settings_.runtime.ntpServerCustom,
         sizeof(ntpServerCustom));
  dataMux_.unlock();
  resolveNtpServerHost(ntpServerPreset, ntpServerCustom, ntpFailoverIndex_,
                       ntpServerBuffer_);
  g_wallClock.setSyncing(ntpServerBuffer_, now);
  ntpSyncStartedAtMs_ = now;
  esp_sntp_set_time_sync_notification_cb(ntpSyncNotificationCallback);
  ntpCallbackAccepting_.store(true, std::memory_order_release);

  // Close the check/use window before touching lwIP. A gate transition also
  // closes callback acceptance synchronously in its lock-free setter.
  if (!gateStable()) {
    stopNtp();
    g_wallClock.cancelSyncing();
    return false;
  }

  if (esp_sntp_enabled()) {
    esp_sntp_setservername(0, ntpServerBuffer_);
    sntp_restart();
    ntpStarted_ = true;
  } else {
    configTime(0, 0, ntpServerBuffer_);
    ntpStarted_ = true;
  }

  // Control/scale never waits for stopNtp. If it won the race with the actual
  // arm, detect its generation here and tear SNTP down on this network task.
  if (!gateStable()) {
    stopNtp();
    g_wallClock.cancelSyncing();
    return false;
  }
  return true;
}

void OpenBrewByWeightNetwork::handleNtpFailure(uint32_t now) {
  stopNtp();
  ntpManualAttempt_ = false;
  if (ntpFailoverIndex_ < 3) {
    ++ntpFailoverIndex_;
  }
  g_wallClock.markFailed(now, NTP_MAX_CONSECUTIVE_FAILURES);
  recordBootHeap(BootHeapStage::NTP_DONE, BootHeapResult::FAILED, millis());
  log(DebugCategory::NETWORK, DebugCode::TIME_SYNC_FAIL, ntpFailoverIndex_, 0);
}

void OpenBrewByWeightNetwork::serviceNtp(uint32_t now, bool staConnected) {
  g_wallClock.rebaseAnchor(now);
  dataMux_.lock();
  const uint32_t settingsGeneration = ntpSettingsGeneration_;
  const bool ntpSyncEnabled = settings_.runtime.ntpSyncEnabled;
  dataMux_.unlock();
  if (settingsGeneration != ntpAppliedSettingsGeneration_) {
    ntpAppliedSettingsGeneration_ = settingsGeneration;
    ntpRearmPending_ = true;
  }

  if (!staConnected) {
    stopNtp();
    staNtpEligibleAtMs_ = 0;
    g_wallClock.markDisabled();
    return;
  }

  // Automatic sync disabled: never arm, retry, or resync. An explicit Sync
  // now still completes: its pending request or in-flight attempt (marked by
  // ntpManualAttempt_) keeps this gate open until it settles. cancelSyncing
  // preserves an existing anchor; it also clears auto retry state.
  if (firmwareCompatibilityMode() ||
      (!ntpSyncEnabled && !ntpManualSyncPending_ && !ntpManualAttempt_)) {
    if (ntpStarted_) {
      stopNtp();
    }
    g_wallClock.cancelSyncing();
    ntpRearmPending_ = false;
    ntpActivitySyncPending_ = false;
    return;
  }

  // Brew RF or scale connecting: do not arm, and abort any in-flight SNTP
  // without counting a failure so NTP rearms when the gate clears. This gate
  // precedes callback application: a completion racing with cancellation is
  // discarded rather than publishing a sync during critical activity.
  TimeStatusSnapshot timeStatus = g_wallClock.snapshot(now);
  const uint32_t gateGeneration =
      rfGateGeneration_.load(std::memory_order_acquire);
  if (!ntpMayArm(now, staConnected)) {
    if (ntpStarted_ || timeStatus.state == TimeSyncState::SYNCING) {
      abortInFlightNtp();
    } else {
      // Clears a callback that arrived just after a prior stop/cancel pass.
      g_wallClock.cancelPendingSync();
    }
    return;
  }

  if (g_wallClock.applyPendingSync(now)) {
    // SNTP is one-shot at the application level. Leaving it enabled would let
    // lwIP resync periodically without consulting the shot/BLE gate.
    stopNtp();
    ntpManualAttempt_ = false;
    ntpFailoverIndex_ = 0;
    recordBootHeap(BootHeapStage::NTP_DONE, BootHeapResult::OK, millis());
    log(DebugCategory::NETWORK, DebugCode::TIME_SYNC_OK);
    return;
  }

  timeStatus = g_wallClock.snapshot(now);

  if (timeStatus.state == TimeSyncState::SYNCING) {
    if (static_cast<uint32_t>(now - ntpSyncStartedAtMs_) >=
        NTP_FIRST_SYNC_TIMEOUT_MS) {
      handleNtpFailure(now);
    }
    return;
  }

  if (ntpRearmPending_ || ntpManualSyncPending_ || ntpActivitySyncPending_) {
    const bool manualRequest = ntpManualSyncPending_;
    ntpRearmPending_ = false;
    ntpManualSyncPending_ = false;
    ntpActivitySyncPending_ = false;
    ntpFailoverIndex_ = 0;
    if (armNtp(now, staConnected, gateGeneration)) {
      ntpManualAttempt_ = manualRequest;
    } else if (manualRequest) {
      // Arming is momentarily blocked (RF gate or post-connect settle):
      // keep the explicit request queued instead of converting it into an
      // auto rearm the disabled gate could drop.
      ntpManualSyncPending_ = true;
    } else {
      ntpRearmPending_ = true;
    }
    return;
  }

  switch (timeStatus.state) {
    case TimeSyncState::OFF:
    case TimeSyncState::FAILED:
      if (timeStatus.nextRetryInMs > 0) {
        return;
      }
      if (!armNtp(now, staConnected, gateGeneration)) {
        ntpRearmPending_ = true;
      }
      return;
    case TimeSyncState::SYNCED:
    case TimeSyncState::STALE:
      if (timeStatus.lastSyncAgeMs >= NTP_RESYNC_INTERVAL_MS) {
        if (!armNtp(now, staConnected, gateGeneration)) {
          ntpRearmPending_ = true;
        }
      }
      return;
    case TimeSyncState::SYNCING:
      return;
  }
}

bool OpenBrewByWeightNetwork::startHttpServer() {
  if (server_ != nullptr) {
    return true;
  }
  beginOwnedHeapLifecycle(httpHeap_, HeapLifecycleEvent::HTTP_START);
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.task_priority = tskIDLE_PRIORITY + 1;
  config.stack_size = HTTP_SERVER_TASK_STACK_SIZE;
  // Pin httpd to core 0, as TASK_SCHEDULE_CONTRACTS already declares and the
  // Micra websocket client practices: unpinned, this control-priority task
  // migrates onto core 1 and timeslices with the control loop while serving
  // heavy WebUI responses, absorbing core-1 wall time into whatever loop
  // phase is executing.
  config.core_id = 0;
  // Keep the httpd task stack in internal RAM (default task_caps). OTA upload
  // erases/writes flash on this task. XIP usually keeps cache available, but
  // flash mapping and driver paths can still disable it.
  // Concurrent peers: the claimed WebUI stream plus a takeover handshake, two
  // lazy HTML/JS or Stats reads, a Home Assistant request, and an OTA range on
  // the worker plus its session poll. Handlers on the httpd task still run
  // serially over the shared response scratch. ESP-IDF uses
  // (max_open_sockets + 3) lwIP sockets; see CONFIG_LWIP_MAX_SOCKETS. TCP
  // buffers are allocated per open connection and prefer PSRAM.
  config.max_open_sockets = 7;
  // Exact special routes precede wildcard dispatchers so claim, integration,
  // and OTA retain their distinct authentication boundaries. Keep ten spare
  // slots; exhausting this tears down the whole Web UI at startup.
  config.max_uri_handlers = 49;
  config.uri_match_fn = httpd_uri_match_wildcard;
  // Safari sends a long UA + Accept-Language + optional Cookie/Sec-Fetch-*;
  // the IDF default (1024) is enough most of the time but intermittent
  // long browser headers have returned 431 Request Header Fields Too Large.
  config.max_req_hdr_len = 2048;
  config.max_resp_headers = 12;
  // Queued connections already hold lwIP netconns (one per
  // CONFIG_LWIP_MAX_SOCKETS), so a deeper backlog would starve other sockets.
  config.backlog_conn = 4;
  config.lru_purge_enable = true;
  // Keep stuck clients from occupying sockets under BLE/Wi-Fi coex stalls.
  config.recv_wait_timeout = 5;
  config.send_wait_timeout = 5;
  if (httpd_start(&server_, &config) != ESP_OK) {
    server_ = nullptr;
    finishOwnedHeapLifecycle(httpHeap_, HeapLifecycleResult::FAILURE);
    recordBootHeap(BootHeapStage::HTTP_READY, BootHeapResult::FAILED, millis());
    lifecycleLog("httpd_start failed");
    return false;
  }

  const bool registered =
      registerHandler(server_, "/", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/diagnostic", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/log", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/stats", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/history", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/admin", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/settings", HTTP_GET, rootHandler) &&
      registerHandler(server_, "/setup", HTTP_GET, rootHandler) &&
      // One exact route per WEB_ASSETS row (scripts/gen_web_ui.js); the
      // wildcard covers the size-specific iOS icon probes.
      registerHandler(server_, "/app.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/app.css", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/runtime.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/ota-image.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/records.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/diagnostic.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/admin.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/settings.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/js/setup.js", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/manifest.webmanifest", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/icons/icon-192.png", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/favicon.ico", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/apple-touch-icon*", HTTP_GET, assetHandler) &&
      registerHandler(server_, "/api/v1/ui/claim", HTTP_POST, claimHandler) &&
      registerHandler(server_, "/api/v1/ui/ui-stream", HTTP_GET,
                      uiStreamHandler, true, uiStreamHandshake) &&
#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1
      // Unlock routes are not served in development builds; administration
      // is public there and the UI never calls this endpoint.
      true &&
#else
      registerHandler(server_, "/api/v1/ui/unlock", HTTP_POST, unlockHandler) &&
#endif
      registerHandler(server_, "/api/v1/integration*", HTTP_GET,
                      integrationHandler) &&
      registerHandler(server_, "/api/v1/integration*", HTTP_POST,
                      integrationHandler) &&
      registerHandler(server_, "/api/v1/integration*", HTTP_PUT,
                      integrationHandler) &&
      registerHandler(server_, "/api/v1/ota", HTTP_GET, otaStatusHandler) &&
      registerHandler(server_, "/api/v1/ota", HTTP_POST, otaUploadHandler) &&
      registerHandler(server_, "/api/v1/ota/session", HTTP_GET, otaSessionHandler) &&
      registerHandler(server_, "/api/v1/ota/session", HTTP_POST, otaSessionHandler) &&
      registerHandler(server_, "/api/v1/ota", HTTP_PATCH, otaPatchHandler) &&
      registerHandler(server_, "/api/v1/ota/flash", HTTP_POST, otaFlashHandler) &&
      registerHandler(server_, "/api/v1/ota/abort", HTTP_POST, otaAbortHandler) &&
      registerHandler(server_, "/api/v1/*", HTTP_GET, ownedApiHandler) &&
      registerHandler(server_, "/api/v1/*", HTTP_POST, ownedApiHandler) &&
      registerHandler(server_, "/api/v1/*", HTTP_PUT, ownedApiHandler);
  if (!registered ||
      httpd_register_err_handler(server_, HTTPD_404_NOT_FOUND,
                                 notFoundHandler) != ESP_OK) {
    (void)httpd_stop(server_);
    server_ = nullptr;
    uiStreamWorkPending_.store(false, std::memory_order_release);
    finishOwnedHeapLifecycle(httpHeap_, HeapLifecycleResult::FAILURE);
    recordBootHeap(BootHeapStage::HTTP_READY, BootHeapResult::FAILED, millis());
    return false;
  }
  finishOwnedHeapLifecycle(httpHeap_, HeapLifecycleResult::SUCCESS);
  recordBootHeap(BootHeapStage::HTTP_READY, BootHeapResult::OK, millis());
  return true;
}

// Runs on httpd only. Every setup failure falls back to running the request
// inline on httpd, which is the pre-worker behavior.
esp_err_t OpenBrewByWeightNetwork::runOnOtaWorker(httpd_req_t *request,
                                             OtaWork work) {
  // Seq-cst pairing with stopHttpServer(): either it sees this job pending,
  // or this handler sees the stop and never hands a session to the worker.
  otaJobsPending_.fetch_add(1);
  if (otaWorkerStopping_.load()) {
    otaJobsPending_.fetch_sub(1);
    return sendError(request, STATUS_UNAVAILABLE, "OTA_SERVER_STOPPING",
                     "The web server is restarting; retry shortly.");
  }
  if (otaJobs_ == nullptr) {
    otaJobs_ = xQueueCreate(OTA_JOB_QUEUE_LENGTH, sizeof(OtaJob));
  }
  OtaJob job;
  job.work = work;
  if (otaJobs_ == nullptr ||
      httpd_req_async_handler_begin(request, &job.request) != ESP_OK) {
    actionLog("ota: worker memory unavailable; request ran on httpd");
    const esp_err_t result = work(request);
    otaJobsPending_.fetch_sub(1);
    return result;
  }
  if (xQueueSend(otaJobs_, &job, 0) != pdTRUE) {
    actionLog("ota: worker queue full; request ran on httpd");
    runOtaJob(job);
  } else if (!otaWorkerRunning_.exchange(true) &&
             xTaskCreatePinnedToCore(otaWorkerTask, "ota_worker",
                                     HTTP_SERVER_TASK_STACK_SIZE, this,
                                     tskIDLE_PRIORITY + 1, nullptr, 0) != pdPASS) {
    actionLog("ota: worker task did not start; requests ran on httpd");
    drainOtaJobs();
    otaWorkerRunning_.store(false);
  }
  return ESP_OK;
}

void OpenBrewByWeightNetwork::runOtaJob(const OtaJob &job) {
  (void)job.work(job.request);
  (void)httpd_req_async_handler_complete(job.request);
  otaJobsPending_.fetch_sub(1);
}

void OpenBrewByWeightNetwork::drainOtaJobs() {
  OtaJob job;
  while (xQueueReceive(otaJobs_, &job, 0) == pdTRUE) runOtaJob(job);
}

void OpenBrewByWeightNetwork::otaWorkerTask(void *context) {
  OpenBrewByWeightNetwork &self = *static_cast<OpenBrewByWeightNetwork *>(context);
  do {
    self.drainOtaJobs();
    self.otaWorkerRunning_.store(false);
    // A job queued after the last receive saw the worker running and started
    // no task: keep draining unless that handler has already started one.
  } while (uxQueueMessagesWaiting(self.otaJobs_) != 0 &&
           !self.otaWorkerRunning_.exchange(true));
  vTaskDelete(nullptr);
}

void OpenBrewByWeightNetwork::stopHttpServer() {
  if (server_ != nullptr) {
    // OTA jobs hold async copies of httpd sessions: answer them before
    // httpd_stop frees the sessions. A range write stops at its next sector
    // because otaTransferStillSafe() reports the stop; the wait stays bounded
    // by the OTA receive attempts, as an inline upload once bounded httpd_stop.
    otaWorkerStopping_.store(true);
    while (otaJobsPending_.load() != 0) {
      (void)feedCurrentTaskWatchdog();
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    beginOwnedHeapLifecycle(httpHeap_, HeapLifecycleEvent::HTTP_STOP);
    const esp_err_t stopped = httpd_stop(server_);
    otaWorkerStopping_.store(false);
    server_ = nullptr;
    finishOwnedHeapLifecycle(
        httpHeap_, stopped == ESP_OK ? HeapLifecycleResult::SUCCESS
                                     : HeapLifecycleResult::FAILURE);
  }
}


esp_err_t OpenBrewByWeightNetwork::sendJson(httpd_req_t *request,
                                       const char *status,
                                       const char *json) {
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, JSON_CONTENT_TYPE);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  // Close after each API response so keep-alive does not pin sockets on the
  // ESP while BLE/Wi-Fi coex delays responses. httpd ignores this header, so
  // the session is closed here too: its FIN also releases the tail Nagle holds
  // until the status line's ACK, which modem sleep delays.
  httpd_resp_set_hdr(request, "Connection", "close");
  const size_t length = json == nullptr ? 0 : strlen(json);
  const esp_err_t sent = sendBody(request, json, length);
  (void)httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
  return sent;
}

esp_err_t OpenBrewByWeightNetwork::sendError(httpd_req_t *request,
                                        const char *status,
                                        const char *error,
                                        const char *message) {
  char body[256] = {};
  snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}",
           error, message);
  return sendJson(request, status, body);
}

namespace {

constexpr const char *WEB_UI_CLIENT_HEADER = "X-WebUI-Client";

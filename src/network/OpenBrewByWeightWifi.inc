bool OpenBrewByWeightNetwork::controlAllowsNetworkMutation() {
  return controlAllowsConfiguration(controlGate());
}

// Shared WiFi failure bookkeeping (see header): schedule the retry backoff
// after a bump, publishing the counter; optionally log the retry.
void OpenBrewByWeightNetwork::noteNetworkRetryBackoff(bool logRetry) {
  const uint32_t now = millis();
  startupComplete_ = false;
  if (startupFailures_ < UINT8_MAX) {
    ++startupFailures_;
  }
  networkRetryAtMs_ = now + NETWORK_RETRY_MIN_MS;
  dataMux_.lock();
  status_.startupFailures = startupFailures_;
  dataMux_.unlock();
  if (logRetry) {
    log(DebugCategory::NETWORK, DebugCode::NETWORK_RETRY, startupFailures_,
        NETWORK_RETRY_MIN_MS);
  }
}

// Shared WiFi failure bookkeeping (see header): reset the failure counter,
// optionally flipping the published STA state to DISCONNECTED.
void OpenBrewByWeightNetwork::clearNetworkFailureStatus(bool staDisconnected) {
  startupFailures_ = 0;
  dataMux_.lock();
  status_.startupFailures = 0;
  if (staDisconnected) {
    status_.staState = StaState::DISCONNECTED;
  }
  dataMux_.unlock();
}

// Settings NVS writes on this task must respect the shared durable-write
// gate: defer while a shot/machine circuit is busy, during a maintenance
// lease, or while the scale is mid-connect (see durableFlashWriteAllowed).
bool OpenBrewByWeightNetwork::networkFlashWriteAllowed() const {
  const ControlGateSnapshot control = controlGate();
  return durableFlashWriteAllowed(
      control.activeCycle || control.relayClosed || control.machineRunning ||
          control.physicalActivatorOn || control.maintenanceLeaseActive,
      scaleConnecting_.load(std::memory_order_relaxed));
}

ControlGateSnapshot OpenBrewByWeightNetwork::controlGate() const {
  ControlGateSnapshot gate;
  if (callbacks_.copyControlGate != nullptr) {
    callbacks_.copyControlGate(gate);
  }
  return gate;
}

SetupPhase OpenBrewByWeightNetwork::advanceSetupPhase(SetupEvent event) {
  TaskLockGuard lock(dataMux_);
  status_.setupPhase = nextSetupPhase(status_.setupPhase, event);
  // A phase that keeps the SoftAP owns it: a grace left by an earlier saved
  // network (the summary's Change) must not close it mid-attempt.
  if (setupKeepsSoftAp(status_.setupPhase)) status_.apTeardownAtMs = 0;
  return status_.setupPhase;
}

SetupPhase OpenBrewByWeightNetwork::setupPhase() {
  TaskLockGuard lock(dataMux_);
  return status_.setupPhase;
}

// Station is up while the SoftAP is still raised: close it unless onboarding
// keeps it, giving associated phones the grace window first.
void OpenBrewByWeightNetwork::serviceSoftApTeardown(
    const NetworkStatusSnapshot &status, uint32_t now) {
  if (!status.apActive) return;
  switch (softApTeardownStep(status.setupPhase, status.apClients,
                             status.apTeardownAtMs, now)) {
    case SoftApTeardown::ARM_GRACE:
      dataMux_.lock();
      status_.apTeardownAtMs = now + SOFTAP_TEARDOWN_GRACE_MS;
      dataMux_.unlock();
      lifecycleLog("WiFi STA up; SoftAP closes after grace (clients associated)");
      break;
    case SoftApTeardown::STOP: stopSoftApKeepStation(); break;
    case SoftApTeardown::KEEP: break;
  }
}

bool OpenBrewByWeightNetwork::lockWorkBufForStatus() {
  if (callbacks_.refreshControlStatus != nullptr) {
    callbacks_.refreshControlStatus();
  }
  return lockWorkBuf();
}

void OpenBrewByWeightNetwork::loadControlStatus(ControlStatusSnapshot &control) {
  callbacks_.copyControlStatus(control);
}

bool OpenBrewByWeightNetwork::startNetwork() {
  const NetworkSettingsSnapshot settings = networkSettingsSnapshot();
  const uint32_t now = millis();
  [[maybe_unused]] static const auto staDisconnectHandler = WiFi.onEvent(
      [this](arduino_event_id_t, arduino_event_info_t info) {
        staDisconnectDetail_.store(packStaDisconnectDetail(
            info.wifi_sta_disconnected.reason, info.wifi_sta_disconnected.rssi));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  // STA link-local IPv6 lets mDNS answer AAAA queries instead of leaving
  // getaddrinfo clients to time out; the SoftAP stays IPv4-only.
  WiFi.enableIPv6();
  dataMux_.lock();
  status_.wifiConfigured = settings.staConfigured;
  copyCString(status_.iface.apIp, sizeof(status_.iface.apIp), AP_IP);
  dataMux_.unlock();
  publishConfiguredAddressStatus();
  // Boot memory capture starts before Wi-Fi, mDNS and the HTTP server raise.
  // The published heap minimum starts here too: ESP-IDF v6.1's lifetime
  // minimum under-reports by the internal reserve pool (esp-idf#19261).
  (void)heap_caps_monitor_local_minimum_free_size_start();
  recordBootHeap(BootHeapStage::NETWORK_START, BootHeapResult::OK, now, sampleHeapCaps());
  if (settings.staConfigured) {
    startStation(settings, now);
    startupComplete_ = true;
  } else if (apAutoRaiseExhausted_ || apStartHeld_) {
    lifecycleLog(apStartHeld_ ? "SoftAP raise held at boot"
                              : "SoftAP auto-raise exhausted at boot");
    startupComplete_ = false;
    // The startNetwork retry that actually raises Wi-Fi runs ensureMdns().
    return false;
  } else {
    startupComplete_ = ensureAccessPoint(now);
  }
  // mdns_init() registers handlers on Arduino's default event loop, which
  // only the first WiFi.mode() above creates; init fails without it.
  ensureMdns();
  return startupComplete_;
}

void OpenBrewByWeightNetwork::publishConfiguredAddressStatus() {
  const NetworkSettingsSnapshot settings = networkSettingsSnapshot();
  char staSsid[WIFI_SSID_CAPACITY] = {};
  char configuredIp[16] = {};
  char configuredNetmask[16] = {};
  char configuredGateway[16] = {};
  char configuredDns1[16] = {};
  char configuredDns2[16] = {};
  char deviceName[DEVICE_NAME_CAPACITY] = {};
  char mdnsHost[DEVICE_NAME_CAPACITY] = {};
  if (settings.staConfigured) {
    copyCString(staSsid, sizeof(staSsid), settings.staSsid);
  }
  if (validDeviceName(settings.deviceName)) {
    copyCString(deviceName, sizeof(deviceName), settings.deviceName);
    if (deviceNameToMdnsHost(mdnsHost, sizeof(mdnsHost),
                             settings.deviceName) == 0) {
      mdnsHost[0] = '\0';
    }
  }
  if (settings.staIpMode == static_cast<uint8_t>(StaIpMode::STATIC)) {
    formatIpv4(settings.staIp, configuredIp);
    formatIpv4(settings.staNetmask, configuredNetmask);
    formatIpv4(settings.staGateway, configuredGateway);
    formatIpv4(settings.staDns1, configuredDns1);
    if (!ipv4IsZero(settings.staDns2)) {
      formatIpv4(settings.staDns2, configuredDns2);
    }
  }
  dataMux_.lock();
  status_.staOpen = settings.staConfigured && settings.staOpen;
  status_.staWifiSleep = settings.staWifiSleep;
  status_.staIpMode = settings.staIpMode;
  status_.staConfigState = settings.staConfigState;
  memset(status_.iface.staSsid, 0, sizeof(status_.iface.staSsid));
  copyCString(status_.iface.staSsid, sizeof(status_.iface.staSsid), staSsid);
  copyCString(status_.iface.configuredIp, sizeof(status_.iface.configuredIp), configuredIp);
  copyCString(status_.iface.configuredNetmask, sizeof(status_.iface.configuredNetmask),
              configuredNetmask);
  copyCString(status_.iface.configuredGateway, sizeof(status_.iface.configuredGateway),
              configuredGateway);
  copyCString(status_.iface.configuredDns1, sizeof(status_.iface.configuredDns1),
              configuredDns1);
  copyCString(status_.iface.configuredDns2, sizeof(status_.iface.configuredDns2),
              configuredDns2);
  copyCString(status_.deviceName, sizeof(status_.deviceName), deviceName);
  copyCString(status_.mdnsHost, sizeof(status_.mdnsHost), mdnsHost);
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::armPendingConfirmWindow(uint32_t now, uint32_t timeoutMs) {
  dataMux_.lock();
  staConfirmArmed_ = true;
  staConfirmDeadlineMs_ = now + timeoutMs;
  dataMux_.unlock();
  ::serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "WiFi STA config pending confirmation for %lu s",
                         static_cast<unsigned long>(timeoutMs / 1000));
}

void OpenBrewByWeightNetwork::clearPendingConfirmWindow() {
  dataMux_.lock();
  staConfirmArmed_ = false;
  staConfirmDeadlineMs_ = 0;
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::requestPendingNetworkConfirm(bool fromOwner) {
  dataMux_.lock();
  // Runs on the httpd task: read the published phase under the lock. A page
  // reaching the device confirms a network change, except a joined onboarding
  // attempt, which waits for the owner's "Save Wi-Fi" (the confirm action).
  const bool pending =
      settings_.staConfigState ==
          static_cast<uint8_t>(StaConfigState::PENDING) &&
      settings_.staConfigured &&
      setupConfirmAllowed(status_.setupPhase, status_.apActive) &&
      (fromOwner || !setupAwaitsOwnerSave(status_.setupPhase, status_.apActive));
  if (pending) {
    pendingConfirmRequest_ = true;
  }
  dataMux_.unlock();
}

// Runs on the httpd task: ends the test's confirm window now, so the network
// task's expiry path disconnects and reverts it (retrying a closed write gate).
void OpenBrewByWeightNetwork::requestPendingNetworkDiscard() {
  TaskLockGuard lock(dataMux_);
  if (staConfirmArmed_ && status_.apActive && setupDiscardAllowed(status_.setupPhase)) {
    staConfirmDeadlineMs_ = millis();
  }
}

bool OpenBrewByWeightNetwork::confirmPendingNetwork(const char *reason) {
  PersistedSettings &next = workingSettings();
  NetworkStatusSnapshot status = snapshot();
  if (next.staConfigState != static_cast<uint8_t>(StaConfigState::PENDING) ||
      !next.staConfigured || status.staState != StaState::CONNECTED ||
      !setupConfirmAllowed(status.setupPhase, status.apActive)) {
    return false;
  }
  if (!networkFlashWriteAllowed()) {
    // Retry on a later service pass; re-arm the pending-confirm request.
    dataMux_.lock();
    pendingConfirmRequest_ = true;
    dataMux_.unlock();
    return false;
  }
  next.staConfigState = static_cast<uint8_t>(StaConfigState::CONFIRMED);
  copyActiveStaToLkg(next);
  overlayLiveShotSettings(next);
  if (!savePersistedSettings(next)) {
    return false;
  }
  dataMux_.lock();
  settings_ = next;
  status_.staConfigState = next.staConfigState;
  pendingConfirmRequest_ = false;
  dataMux_.unlock();
  if (setupKeepsSoftAp(status.setupPhase)) {
    // Config confirmed: schedule the SoftAP teardown instead of stopping on
    // the next pass, so the success screen stays readable inside the OS
    // captive window (this covers a CLI AP_START keep too — a confirmed
    // network change ends the fallback window; re-issue AP_START to keep
    // the AP). An earlier "finish" linger stays in force.
    staReconnectHeld_ = false;
    dataMux_.lock();
    status_.setupPhase = nextSetupPhase(status_.setupPhase, SetupEvent::CONFIRMED);
    if (status.apActive && status_.apTeardownAtMs == 0) {
      status_.apTeardownAtMs = millis() + SOFTAP_TEARDOWN_GRACE_MS;
    }
    dataMux_.unlock();
  }
  clearPendingConfirmWindow();
  publishConfiguredAddressStatus();
  // A saved onboarding choice (or a legacy one bound to this Wi-Fi) is now
  // final: boot applies it, after the setup network closes (or now, without one).
  SetupIntent intent;
  if (loadSetupIntent(intent)) {
    setupRestartPending_.store(true, std::memory_order_release);
    if (!status.apActive) {
      requestSetupRestart();
    }
  }
  log(DebugCategory::CONFIG, DebugCode::CONFIG_PERSISTED,
      static_cast<int32_t>(next.runtime.revision));
  ::serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "WiFi STA config confirmed%s%s%s",
                         reason != nullptr && reason[0] != '\0' ? " (" : "",
                         reason != nullptr && reason[0] != '\0' ? reason : "",
                         reason != nullptr && reason[0] != '\0' ? ")" : "");
  return true;
}

bool OpenBrewByWeightNetwork::revertPendingNetwork(uint32_t,
                                              const char *reason) {
  PersistedSettings &next = workingSettings();
  if (next.staConfigState != static_cast<uint8_t>(StaConfigState::PENDING)) {
    return false;
  }
  if (!networkFlashWriteAllowed()) {
    // staConfirmArmed_ stays set: callers' retry backoff re-drives the
    // revert once the durable-write gate opens.
    return false;
  }
  ::serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "WiFi STA pending config reverted%s%s%s",
                         reason != nullptr && reason[0] != '\0' ? " (" : "",
                         reason != nullptr && reason[0] != '\0' ? reason : "",
                         reason != nullptr && reason[0] != '\0' ? ")" : "");
  if (!restoreLkgToActive(next)) {
    clearStaNetwork(next);
  }
  overlayLiveShotSettings(next);
  if (!savePersistedSettings(next)) {
    return false;
  }
  // A legacy onboarding choice bound to the reverted Wi-Fi goes with it (boot
  // also drops it on the binding mismatch); part one's unbound choice stays.
  SetupIntent intent;
  if (loadSetupIntent(intent) && intent.bound) (void)clearSetupIntent();
  dataMux_.lock();
  settings_ = next;
  status_.wifiConfigured = next.staConfigured;
  status_.staConfigState = next.staConfigState;
  dataMux_.unlock();
  clearPendingConfirmWindow();
  publishConfiguredAddressStatus();
  // Live-apply revert: end the onboarding phase unconditionally, but
  // remember the onboarding context — a first-use revert must keep the
  // SoftAP up as the fallback network serving /setup, even when the station
  // briefly connected and then went unconfirmed.
  // A reverted onboarding attempt cannot leave the reconnect loop held.
  staReconnectHeld_ = false;
  dataMux_.lock();
  const bool onboardingRevert = setupKeepsSoftAp(status_.setupPhase);
  status_.setupPhase = nextSetupPhase(status_.setupPhase, SetupEvent::REVERTED);
  if (!next.staConfigured) {
    status_.staState = StaState::NOT_CONFIGURED;
    status_.iface.staIp[0] = '\0';
    clearStaLinkMetrics();
  }
  dataMux_.unlock();
  log(DebugCategory::CONFIG, DebugCode::CONFIG_PERSISTED,
      static_cast<int32_t>(next.runtime.revision));
  stopNtp();
  staNtpEligibleAtMs_ = 0;
  g_wallClock.markDisabled();
  // Keep staEverConnected_: SoftAP auto-raise stays boot-only after any prior
  // successful STA join this process (AP_START / reboot still available).
  if (next.staConfigured) {
    // Do not startStation/ensureAccessPoint here: httpd_start nested under
    // service()/serviceStaState overflowed the 10 KiB network task (pthread
    // TLS LoadProhibited).
    startupComplete_ = false;
    return true;
  }
  if (staEverConnected_ && !onboardingRevert) {
    stopHttpServer();
    stopCaptivePortalDns();
    WiFi.softAPdisconnect(true);
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.disconnect(false, false);
    }
    dataMux_.lock();
    status_.networkActive = false;
    status_.apActive = false;
    syncScaleSoftApRadio(false);
    status_.apClients = 0;
    status_.staState = StaState::NOT_CONFIGURED;
    status_.iface.staIp[0] = '\0';
    status_.apTeardownAtMs = 0;
    clearStaLinkMetrics();
    dataMux_.unlock();
    lifecycleLog(
        "WiFi STA cleared after prior connect; SoftAP suppressed (AP_START or reboot)");
    return true;
  }
  startupComplete_ = false;
  return true;
}

void OpenBrewByWeightNetwork::applyStationAddressConfig(
    const NetworkSettingsSnapshot &settings) {
  if (settings.staIpMode == static_cast<uint8_t>(StaIpMode::STATIC)) {
    const IPAddress ip(settings.staIp[0], settings.staIp[1], settings.staIp[2],
                       settings.staIp[3]);
    const IPAddress gateway(settings.staGateway[0], settings.staGateway[1],
                            settings.staGateway[2], settings.staGateway[3]);
    const IPAddress netmask(settings.staNetmask[0], settings.staNetmask[1],
                            settings.staNetmask[2], settings.staNetmask[3]);
    const IPAddress dns1(settings.staDns1[0], settings.staDns1[1],
                         settings.staDns1[2], settings.staDns1[3]);
    const IPAddress dns2(settings.staDns2[0], settings.staDns2[1],
                         settings.staDns2[2], settings.staDns2[3]);
    if (!ipv4IsZero(settings.staDns2)) {
      WiFi.config(ip, gateway, netmask, dns1, dns2);
    } else {
      WiFi.config(ip, gateway, netmask, dns1);
    }
  }
  // DHCP: do not WiFi.config(INADDR_NONE,...). That path calls STA.begin()
  // with tryConnect=true and can disconnect while STA_START is in flight.
}

void OpenBrewByWeightNetwork::clearStaLinkMetrics() {
  status_.staLinkMetricsValid = false;
  status_.staRssi = 0;
  status_.staSignalLevel = 0;
  status_.staSignalQualityPct = 0;
  status_.iface.staBssid[0] = '\0';
}

bool OpenBrewByWeightNetwork::brewRfActive() const {
  return controlCriticalRfActive_.load(std::memory_order_acquire);
}

void OpenBrewByWeightNetwork::syncScaleLinkRf(bool connectingOrUp) {
  scaleConnectingOrUp_.store(connectingOrUp, std::memory_order_relaxed);
}

void OpenBrewByWeightNetwork::syncScaleConnectingRf(bool connecting) {
  connecting = connecting || outboundScaleInhibited();
  const bool changed =
      scaleConnecting_.exchange(connecting, std::memory_order_acq_rel) !=
      connecting;
  if (connecting) {
    // The lwIP callback may race the low-priority task that stops SNTP. Close
    // its publication gate immediately without touching network APIs here.
    ntpCallbackAccepting_.store(false, std::memory_order_release);
    ntpAbortRequested_.store(true, std::memory_order_release);
  }
  if (changed) {
    rfGateGeneration_.fetch_add(1U, std::memory_order_acq_rel);
  }
  if (changed && taskHandle_ != nullptr) {
    xTaskNotifyGive(taskHandle_);
  }
}

void OpenBrewByWeightNetwork::syncControlCriticalRf(bool active, bool publishTransition) {
  const bool changed =
      controlCriticalRfActive_.exchange(active, std::memory_order_acq_rel) !=
      active;
  webhooks_.setControlCritical(active);
  if (active) {
    ntpCallbackAccepting_.store(false, std::memory_order_release);
    ntpAbortRequested_.store(true, std::memory_order_release);
  }
  if (changed) {
    rfGateGeneration_.fetch_add(1U, std::memory_order_acq_rel);
  }
  if (changed || publishTransition) {
    uiStreamUrgent_.store(true, std::memory_order_release);
    if (taskHandle_ != nullptr) xTaskNotifyGive(taskHandle_);
  }
}

void OpenBrewByWeightNetwork::syncScaleHuntRf(bool huntActive) {
  scaleHuntRfActive_.store(huntActive, std::memory_order_relaxed);
}

void OpenBrewByWeightNetwork::applyWifiPowerSave(bool apStarting) {
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_OFF) {
    powerNetworkBusy.store(OpenBrewByWeightOta::instance().busy(),
                           std::memory_order_release);
    lastAppliedWifiPsValid_ = false;
    return;
  }
  bool sleepAllowed = false;
  bool apActive = false;
  bool staAssociated = false;
  dataMux_.lock();
  sleepAllowed = settings_.staWifiSleep;
  apActive = status_.apActive || apStarting;
  staAssociated = status_.staState == StaState::CONNECTED;
  dataMux_.unlock();
  const bool otaBusy = OpenBrewByWeightOta::instance().busy();
  powerNetworkBusy.store(apActive || otaBusy || !staAssociated,
                         std::memory_order_release);
  const WifiPowerSaveMode desired = desiredWifiPowerSave(
      sleepAllowed, apActive, staAssociated, otaBusy);
  const wifi_ps_type_t desiredPs = desired == WifiPowerSaveMode::MIN_MODEM
                                       ? WIFI_PS_MIN_MODEM
                                       : WIFI_PS_NONE;
  const uint8_t inputs = static_cast<uint8_t>(
      (static_cast<uint8_t>(mode) << 4) | (sleepAllowed << 3) |
      (apActive << 2) | (staAssociated << 1) | otaBusy);
  const uint32_t now = millis();
  // Called every 50 ms service pass: query the driver only on a state edge
  // or once per retry interval, not on every pass.
  if (lastAppliedWifiPsValid_ && inputs == lastWifiPsInputs_ &&
      static_cast<int32_t>(now - wifiPsNextWriteAtMs_) < 0) {
    return;
  }
  lastWifiPsInputs_ = inputs;
  wifi_ps_type_t current = WIFI_PS_NONE;
  if (esp_wifi_get_ps(&current) == ESP_OK && lastAppliedWifiPsValid_ &&
      current == desiredPs && lastAppliedWifiPs_ == desiredPs) {
    wifiPsNextWriteAtMs_ = now + WIFI_PS_RETRY_MS;
    return;
  }
  if (!apStarting && lastAppliedWifiPs_ == desiredPs &&
      wifiPsNextWriteAtMs_ != 0 &&
      static_cast<int32_t>(now - wifiPsNextWriteAtMs_) < 0) {
    return;
  }
  if (!WiFi.setSleep(desiredPs)) {
    lastAppliedWifiPsValid_ = false;
    wifiPsNextWriteAtMs_ = now + WIFI_PS_RETRY_MS;
    lifecycleLog("WiFi power save set failed");
    return;
  }
  wifi_ps_type_t verified = WIFI_PS_NONE;
  if (esp_wifi_get_ps(&verified) != ESP_OK || verified != desiredPs) {
    // Arduino 3.x setSleep is a no-op until STA starts. SoftAP needs the
    // driver updated whenever setSleep reported success but get_ps missed.
    if (esp_wifi_set_ps(desiredPs) != ESP_OK ||
        esp_wifi_get_ps(&verified) != ESP_OK || verified != desiredPs) {
      lastAppliedWifiPsValid_ = false;
      wifiPsNextWriteAtMs_ = now + WIFI_PS_RETRY_MS;
      lifecycleLog("WiFi power save verify mismatch");
      return;
    }
  }
  lastAppliedWifiPs_ = desiredPs;
  lastAppliedWifiPsValid_ = true;
  wifiPsNextWriteAtMs_ = now + WIFI_PS_RETRY_MS;
}

// The responder is passive: it answers queries and sends the component's
// low-rate announcements. MIN_MODEM modem sleep wakes every DTIM, so
// multicast (and discovery) keeps working with Wi-Fi sleep enabled; no RF
// gating is needed on scale-connect/brew like SNTP requires.
bool OpenBrewByWeightNetwork::ensureMdns() {
  if (mdnsActive_) {
    return true;
  }
  const esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    recordBootHeap(BootHeapStage::MDNS_READY, BootHeapResult::FAILED, millis());
    lifecycleLogf("mDNS init failed (0x%x)", static_cast<unsigned>(err));
    return false;
  }
  mdnsActive_ = true;
  mdnsServiceActive_ = false;
  activeMdnsHost_[0] = '\0';
  applyMdnsName();
  recordBootHeap(BootHeapStage::MDNS_READY, BootHeapResult::OK, millis());
  return true;
}

void OpenBrewByWeightNetwork::applyMdnsName() {
  char name[DEVICE_NAME_CAPACITY] = {};
  {
    TaskLockGuard lock(dataMux_);
    memcpy(name, settings_.deviceName, sizeof(name));
  }
  if (!validDeviceName(name)) {
    return;
  }
  char host[DEVICE_NAME_CAPACITY] = {};
  if (deviceNameToMdnsHost(host, sizeof(host), name) == 0) {
    return;
  }
  char instance[DEVICE_NAME_CAPACITY] = {};
  if (deviceNameToInstanceLabel(instance, sizeof(instance), name) == 0) {
    return;
  }
  // DHCP-side hostname; visible in routers once STA (re)associates.
  WiFi.setHostname(host);
  if (!mdnsActive_) {
    return;  // ensureMdns() applies the name right after mdns_init().
  }
  if (strcmp(host, activeMdnsHost_) == 0 && mdnsServiceActive_) {
    return;
  }
  if (strcmp(host, activeMdnsHost_) != 0) {
    if (mdns_hostname_set(host) != ESP_OK) {
      lifecycleLog("mDNS hostname set failed");
      return;
    }
    copyCString(activeMdnsHost_, sizeof(activeMdnsHost_), host);
  }
  if (mdnsServiceActive_) {
    (void)mdns_service_instance_name_set("_http", "_tcp", instance);
  } else if (mdns_service_add(instance, "_http", "_tcp", 80, nullptr, 0) ==
             ESP_OK) {
    mdnsServiceActive_ = true;
    // Home Assistant discovery matcher: identification TXT on the one
    // advertised service. Best-effort; failure only disables discovery.
    (void)mdns_service_txt_item_set("_http", "_tcp", "obbw", "1");
  } else {
    lifecycleLog("mDNS service add failed");
    return;
  }
  lifecycleLogf("mDNS announcing %s.local", host);
}

void OpenBrewByWeightNetwork::stopMdns() {
  if (!mdnsActive_) {
    return;
  }
  mdns_free();
  mdnsActive_ = false;
  mdnsServiceActive_ = false;
  activeMdnsHost_[0] = '\0';
}

bool OpenBrewByWeightNetwork::beginStationConnect(const NetworkSettingsSnapshot &settings,
                                             uint32_t now) {
  if (brewRfActive()) {
    lifecycleLog("WiFi STA associate deferred; brew RF active");
    return false;
  }
  if (scaleConnecting_.load(std::memory_order_relaxed)) {
    lifecycleLog("WiFi STA associate deferred; scale connecting");
    return false;
  }
  beginOwnedHeapLifecycle(wifiHeap_, HeapLifecycleEvent::WIFI_CONNECT);
  const bool apActive = snapshot().apActive;
  if (apActive) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  // Drop any UI scan so association owns the radio.
  abortWifiScan(now, false);
  // Drop a stale STA association so 10 s retries do not accumulate IDF/DHCP
  // state. wifioff=false, eraseap=false: driver and SoftAP stay up (BLE coex).
  WiFi.disconnect(false, false);
  dataMux_.lock();
  status_.staState = StaState::CONNECTING;
  status_.iface.staIp[0] = '\0';
  clearStaLinkMetrics();
  dataMux_.unlock();
  log(DebugCategory::NETWORK, DebugCode::STA_CONNECTING);
  if (apActive) {
    lifecycleLog(settings.staIpMode == static_cast<uint8_t>(StaIpMode::STATIC)
                     ? "WiFi STA retrying with static IP; SoftAP stays up"
                     : "WiFi STA retrying; SoftAP stays up");
  } else if (settings.staIpMode == static_cast<uint8_t>(StaIpMode::STATIC)) {
    lifecycleLog("WiFi STA connecting with static IP; AP disabled");
  } else {
    lifecycleLog("WiFi STA connecting; AP disabled");
  }
  // Do NOT pre-scan and lock a BSSID: an incomplete scan (BLE coexistence)
  // often sees only the weak BSS and then forces sticky association to it.
  // Let the IDF connect path scan every channel and sort by RSSI.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (!feedCurrentTaskWatchdog()) {
    callbacks_.reportTaskWatchdogFault();
  }

  staConnectStartedAtMs_ = millis();
  staReconnectAttemptAtMs_ = staConnectStartedAtMs_;
  // A user action, link loss or settings change starts a fresh backoff;
  // the automatic retry path doubles it after this call returns.
  staReconnectIntervalMs_ = STA_RECONNECT_INTERVAL_MS;
  lifecycleLog("WiFi STA associating with all-channel RSSI sort");
  // begin(false): enable STA and wait for STA_START without connect().
  // WiFi.begin() uses begin(true) then connect(ssid), and that extra connect()
  // can disconnect while the start handler runs (ESP_ERR_WIFI_STOP_STATE 12308).
  if (!WiFi.STA.begin(false)) {
    finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::FAILURE);
    lifecycleLog("WiFi STA enable failed");
    return false;
  }
  applyWifiPowerSave();
  applyStationAddressConfig(settings);
  staDisconnectDetail_.store(0);
  if (!WiFi.STA.connect(settings.staSsid,
                        settings.staOpen ? nullptr : settings.staPassword)) {
    setStaState(StaState::DISCONNECTED);
    finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::FAILURE);
    lifecycleLog("WiFi STA connect request failed");
    return false;
  }
  return true;
}

void OpenBrewByWeightNetwork::startStation(const NetworkSettingsSnapshot &settings,
                                      uint32_t now) {
  stopHttpServer();
  stopCaptivePortalDns();
  abortWifiScan(now, false);
  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.softAPdisconnect(true);
  }
  WiFi.mode(WIFI_STA);
  recordBootHeap(BootHeapStage::WIFI_INIT,
                 WiFi.getMode() != WIFI_OFF ? BootHeapResult::OK : BootHeapResult::FAILED,
                 millis());
  // Do not clear staEverConnected_: once STA has joined this boot, SoftAP must
  // not auto-raise again (serial AP_START or reboot only).
  clearPendingConfirmWindow();
  dataMux_.lock();
  status_.networkActive = false;
  status_.apActive = false;
  syncScaleSoftApRadio(false);
  status_.apClients = 0;
  status_.staState = StaState::CONNECTING;
  status_.iface.staIp[0] = '\0';
  clearStaLinkMetrics();
  status_.windowRemainingMs = 0;
  status_.apTeardownAtMs = 0;
  status_.confirmRemainingMs = 0;
  dataMux_.unlock();
  publishConfiguredAddressStatus();
  if (!beginStationConnect(settings, now)) {
    dataMux_.lock();
    if (status_.staState == StaState::CONNECTING) {
      status_.staState = StaState::DISCONNECTED;
    }
    dataMux_.unlock();
  }
}

// The same idle-safe restart as the Restart button: a pour finishes first.
void OpenBrewByWeightNetwork::requestSetupRestart() {
  WebCommand restart;
  restart.type = WebCommandType::RESTART;
  restart.requestId = allocateRequestId();
  if (callbacks_.enqueueWebCommand(restart)) {
    setupRestartPending_.store(false, std::memory_order_release);
  }
}

void OpenBrewByWeightNetwork::stopSoftAp(bool stopHttp) {
  if (!snapshot().apActive) {
    return;
  }
  stopCaptivePortalDns();
  beginOwnedHeapLifecycle(wifiHeap_, HeapLifecycleEvent::AP_STOP);
  // Keep WIFI_AP_STA (or current mode) after SoftAP stop so the fresh STA
  // association is not reset by a mode switch to WIFI_STA.
  WiFi.softAPdisconnect(true);
  if (stopHttp) {
    stopHttpServer();
  }
  dataMux_.lock();
  status_.apActive = false;
  syncScaleSoftApRadio(false);
  status_.apClients = 0;
  if (stopHttp) {
    status_.networkActive = false;
  }
  status_.windowRemainingMs = 0;
  status_.apTeardownAtMs = 0;
  status_.setupPhase = nextSetupPhase(status_.setupPhase, SetupEvent::AP_STOPPED);
  status_.setupBrew = SetupBrew::NONE;
  dataMux_.unlock();
  clearSoftApIdleState();
  log(DebugCategory::NETWORK, DebugCode::AP_STOPPED);
  if (setupRestartPending_.load(std::memory_order_acquire)) {
    requestSetupRestart();
  }
  lifecycleLog(stopHttp ? "WiFi SoftAP stopped; HTTP stopped"
                        : "WiFi SoftAP stopped; HTTP kept");
  applyWifiPowerSave();
  finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::SUCCESS);
}

void OpenBrewByWeightNetwork::stopSoftApKeepStation() {
  stopSoftAp(true);
}

void OpenBrewByWeightNetwork::stopSoftApLeaveHttp() {
  stopSoftAp(false);
}

void OpenBrewByWeightNetwork::clearSoftApIdleState() {
  apIdleDeadlineMs_ = 0;
  lastApClients_ = 0;
}

void OpenBrewByWeightNetwork::armSoftApIdleDeadline(uint32_t now) {
  if (setupKeepsSoftAp(setupPhase())) {
    clearSoftApIdleState();
    return;
  }
  apIdleDeadlineMs_ = now + SOFTAP_IDLE_TIMEOUT_MS;
  lastApClients_ = 0;
}

void OpenBrewByWeightNetwork::serviceSoftApIdle(uint32_t now, bool apActive,
                                           uint8_t clients, bool activatorOn) {
  if (!apActive || setupKeepsSoftAp(setupPhase())) {
    if (!apActive) {
      clearSoftApIdleState();
    } else {
      lastApClients_ = clients;
      apIdleDeadlineMs_ = 0;
    }
    return;
  }

  // Full mode: turning the machine on ends the automatic setup network at
  // once, even with phones joined, so the scale search is not held back.
  const bool brewing = activatorOn && !firmwareCompatibilityMode();
  if (!brewing && clients > 0) {
    // Someone joined the setup network: retry the saved network promptly.
    if (lastApClients_ == 0) staReconnectIntervalMs_ = STA_RECONNECT_INTERVAL_MS;
    apIdleDeadlineMs_ = 0;
    lastApClients_ = clients;
    return;
  }

  // Zero SoftAP stations: arm/re-arm on raise or when the last client leaves.
  if (lastApClients_ > 0 || apIdleDeadlineMs_ == 0) {
    armSoftApIdleDeadline(now);
  }
  lastApClients_ = 0;

  if (!brewing && (apIdleDeadlineMs_ == 0 ||
                   static_cast<int32_t>(now - apIdleDeadlineMs_) < 0)) {
    return;
  }

  apAutoRaiseExhausted_ = true;
  clearSoftApIdleState();
  lifecycleLog(brewing ? "WiFi SoftAP closed: machine turned on; down for rest of boot"
                       : "WiFi SoftAP idle timeout; SoftAP down for rest of boot");
  dataMux_.lock();
  const bool staConnected = status_.staState == StaState::CONNECTED;
  dataMux_.unlock();
  const bool staUp = staConnected && WiFi.status() == WL_CONNECTED;
  if (staUp) {
    stopSoftApLeaveHttp();
  } else {
    stopSoftAp(true);
  }
}

bool OpenBrewByWeightNetwork::wifiScanInProgress() {
  bool busy = false;
  dataMux_.lock();
  busy = scanRequested_ || g_wifiScan.state == WifiScanState::QUEUED ||
         g_wifiScan.state == WifiScanState::RUNNING;
  dataMux_.unlock();
  return busy;
}

void OpenBrewByWeightNetwork::abortWifiScan(uint32_t now, bool logTimeout) {
  bool wasRunning = false;
  bool wasQueuedOrRequested = false;
  dataMux_.lock();
  wasRunning = g_wifiScan.state == WifiScanState::RUNNING;
  wasQueuedOrRequested =
      scanRequested_ || g_wifiScan.state == WifiScanState::QUEUED || wasRunning;
  if (wasQueuedOrRequested) {
    scanRequested_ = false;
    g_wifiScan.state = logTimeout ? WifiScanState::FAILED : WifiScanState::CANCELED;
    g_wifiScan.updatedAtMs = now;
    g_wifiScan.count = 0;
  }
  dataMux_.unlock();
  if (wasRunning) {
    esp_wifi_scan_stop();
    WiFi.scanDelete();
  }
  if (!wasQueuedOrRequested) {
    return;
  }
  log(DebugCategory::NETWORK,
      logTimeout ? DebugCode::WIFI_SCAN_ERROR : DebugCode::WIFI_SCAN_CANCELED,
      logTimeout ? -1 : 0);
  if (scanMaintenanceLeaseId_ != 0) {
    WebCommand command;
    command.type = WebCommandType::START_WIFI_SCAN;
    command.requestId = scanRequestId_;
    command.maintenanceLeaseId = scanMaintenanceLeaseId_;
    (void)enqueueMaintenanceCompletion(command, false,
                                       logTimeout ? CommandResultState::FAILED
                                                  : CommandResultState::CANCELED);
    scanMaintenanceLeaseId_ = 0;
    scanRequestId_ = 0;
  }
}

bool OpenBrewByWeightNetwork::ensureAccessPoint(uint32_t now, bool force) {
  const NetworkSettingsSnapshot settings = networkSettingsSnapshot();
  NetworkStatusSnapshot status = snapshot();
  if (!force && status.apActive && status.networkActive && server_ != nullptr) {
    return true;
  }
  if (!force && (apAutoRaiseExhausted_ || apStartHeld_)) {
    lifecycleLog(apStartHeld_ ? "SoftAP raise held"
                              : "SoftAP auto-raise exhausted");
    return false;
  }
  beginOwnedHeapLifecycle(wifiHeap_, HeapLifecycleEvent::AP_START);

  abortWifiScan(now, false);
  // Re-raising tears the current AP down first; the captive DNS must not
  // outlive the interface it answers on.
  stopCaptivePortalDns();
  const bool keepStation = settings.staConfigured;
  const bool staLinkUp = keepStation && WiFi.status() == WL_CONNECTED;
  const bool wantHttp = !httpStartHeld_;
  const bool keepHttp = wantHttp && staLinkUp && server_ != nullptr;
  if (wantHttp && !keepHttp) {
    stopHttpServer();
  }
  if (!keepStation) {
    if (WiFi.getMode() != WIFI_OFF) {
      WiFi.disconnect(false, false);
    }
    WiFi.mode(WIFI_AP);
  } else {
    // Preserve any in-flight STA association attempt while SoftAP is raised.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_AP_STA);
  }
  recordBootHeap(BootHeapStage::WIFI_INIT,
                 WiFi.getMode() != WIFI_OFF ? BootHeapResult::OK : BootHeapResult::FAILED,
                 millis());
  applyWifiPowerSave(true);
  syncScaleSoftApRadio(true);
  const IPAddress ip(192, 168, 4, 1);
  // dns=ip makes the DHCP lease offer the device as DNS server; without it
  // Arduino leaves option 6 out and AP clients cannot resolve probe hosts.
  const bool configured =
      WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0), (uint32_t)0, ip);
  char apSsid[WIFI_SSID_CAPACITY] = {};
  fillSoftApSsid(apSsid, sizeof(apSsid));
  const bool apReady =
      configured && WiFi.softAP(apSsid, settings.devicePassword);
  const bool httpReady =
      keepHttp || (wantHttp && apReady && startHttpServer());
  networkStartedAtMs_ = now;
  dataMux_.lock();
  status_.networkActive = apReady && httpReady;
  status_.apActive = apReady;
  syncScaleSoftApRadio(apReady);
  status_.apClients = 0;
  memset(status_.iface.apSsid, 0, sizeof(status_.iface.apSsid));
  copyCString(status_.iface.apSsid, sizeof(status_.iface.apSsid), apSsid);
  if (!settings.staConfigured) {
    status_.staState = StaState::NOT_CONFIGURED;
  } else if (status_.staState == StaState::CONNECTED && !staLinkUp) {
    status_.staState = StaState::DISCONNECTED;
  }
  if (!staLinkUp &&
      (!keepStation || status_.staState != StaState::CONNECTED)) {
    status_.iface.staIp[0] = '\0';
    clearStaLinkMetrics();
  }
  status_.windowRemainingMs = 0;
  status_.apTeardownAtMs = 0;
  dataMux_.unlock();
  log(DebugCategory::NETWORK, DebugCode::AP_STARTED, apReady, httpReady);
  lifecycleLogf("%s%s at %s",
                keepStation ? "WiFi recovery SoftAP " : "WiFi SoftAP ",
                apReady && (httpReady || !wantHttp) ? "ready" : "failed",
                AP_IP);
  if (!apReady || (wantHttp && !httpReady)) {
    if (!keepHttp) {
      stopHttpServer();
    }
    WiFi.softAPdisconnect(true);
    // Keep the driver initialized: WIFI_OFF deinits and breaks BLE coexistence.
    dataMux_.lock();
    status_.apActive = false;
    status_.networkActive = false;
    dataMux_.unlock();
    clearSoftApIdleState();
    applyWifiPowerSave();
    syncScaleSoftApRadio(false);
    finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::FAILURE);
    return false;
  }
  armSoftApIdleDeadline(now);
  applyWifiPowerSave(true);
  startCaptivePortalDns();
  finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::SUCCESS);
  return true;
}

void OpenBrewByWeightNetwork::startCaptivePortalDns() {
  if (captiveDnsActive_) {
    return;
  }
  // A raised SoftAP is a fresh portal session: probes bounce to /setup again,
  // even if this DNS start attempt ends up failing.
  portalExitRequested_.store(false, std::memory_order_release);
  const IPAddress ip(192, 168, 4, 1);
  if (!captiveDns_.start(53, "*", ip)) {
    lifecycleLog("Captive portal DNS start failed");
    return;
  }
  captiveDnsActive_ = true;
  lifecycleLog("Captive portal DNS up (UDP/53)");
}

void OpenBrewByWeightNetwork::stopCaptivePortalDns() {
  if (!captiveDnsActive_) {
    return;
  }
  captiveDns_.stop();
  captiveDnsActive_ = false;
  lifecycleLog("Captive portal DNS down");
}

void OpenBrewByWeightNetwork::stopNetwork() {
  beginOwnedHeapLifecycle(wifiHeap_, HeapLifecycleEvent::WIFI_DISCONNECT);
  esp_wifi_scan_stop();
  WiFi.scanDelete();
  stopHttpServer();
  stopCaptivePortalDns();
  WiFi.softAPdisconnect(true);
  if (WiFi.getMode() != WIFI_OFF && WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(false, false);
  }
  // Do not WiFi.mode(WIFI_OFF): Arduino 3.x deinits the driver there
  // (esp_wifi_deinit) and breaks BLE VHCI coexistence.
  dataMux_.lock();
  scanRequested_ = false;
  g_wifiScan = WifiScanSnapshot{};
  status_.networkActive = false;
  status_.apActive = false;
  syncScaleSoftApRadio(false);
  status_.apClients = 0;
  status_.staState = status_.wifiConfigured ? StaState::DISCONNECTED
                                            : StaState::NOT_CONFIGURED;
  status_.iface.staIp[0] = '\0';
  clearStaLinkMetrics();
  status_.windowRemainingMs = 0;
  status_.apTeardownAtMs = 0;
  status_.setupPhase = nextSetupPhase(status_.setupPhase, SetupEvent::AP_STOPPED);
  dataMux_.unlock();
  clearSoftApIdleState();
  log(DebugCategory::NETWORK, DebugCode::AP_STOPPED);
  finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::SUCCESS);
}

void OpenBrewByWeightNetwork::serviceStaState(uint32_t now) {
  NetworkStatusSnapshot status = snapshot();
  if (!status.wifiConfigured) {
    return;
  }

  const bool brewRf = brewRfActive();
  const bool scaleConnecting =
      scaleConnecting_.load(std::memory_order_relaxed);
  const bool reconnectHeld =
      staReconnectHeld_ || setupHoldsReconnect(status.setupPhase);
  // Abort an in-flight scan/associate only. A STA that already has an IP
  // must promote to CONNECTED: associated traffic can coexist under PREFER_BT.
  if ((brewRf || scaleConnecting) && status.staState == StaState::CONNECTING &&
      WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    setStaState(StaState::DISCONNECTED);
    if (status.setupPhase == SetupPhase::ATTEMPTING) {
      // Same hold as the aged-attempt path: an aborted onboarding attempt
      // must not enter the 10 s reconnect loop (it aborts every UI scan and
      // flip-flops the state the /setup page is polling).
      advanceSetupPhase(SetupEvent::ATTEMPT_FAILED);
      lifecycleLog("WiFi STA onboarding attempt aborted; retry from the page");
    }
    lifecycleLog(brewRf ? "WiFi STA associate aborted; brew RF active"
                        : "WiFi STA associate aborted; scale connecting");
    applyWifiPowerSave();
    finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::CANCELLED);
    return;
  }

  if (status.staState == StaState::CONNECTED) {
    if (WiFi.status() != WL_CONNECTED) {
      beginOwnedHeapLifecycle(wifiHeap_, HeapLifecycleEvent::WIFI_DISCONNECT);
      dataMux_.lock();
      status_.staState = StaState::DISCONNECTED;
      status_.networkActive = false;
      status_.iface.staIp[0] = '\0';
      clearStaLinkMetrics();
      dataMux_.unlock();
      log(DebugCategory::NETWORK, DebugCode::STA_FAILED,
          static_cast<int32_t>(WiFi.status()),
          staDisconnectDetail_.exchange(0));
      stopNtp();
      staNtpEligibleAtMs_ = 0;
      g_wallClock.markDisabled();
      // After a prior successful STA join this session, never auto-raise SoftAP
      // on link loss — only retry station (SoftAP via AP_START or reboot).
      staConnectStartedAtMs_ = now;
      staReconnectAttemptAtMs_ = now;
      lifecycleLog("WiFi STA disconnected; retrying STA (no SoftAP after prior connect)");
      applyWifiPowerSave();
      finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::SUCCESS);
      if (!reconnectHeld && !wifiScanInProgress() && !brewRf &&
          !scaleConnecting) {
        beginStationConnect(networkSettingsSnapshot(), now);
      } else if (reconnectHeld) {
        lifecycleLog("STA reconnect held; skipping retry");
      } else if (brewRf) {
        lifecycleLog("STA reconnect deferred; brew RF active");
      } else if (scaleConnecting) {
        lifecycleLog("STA reconnect deferred; scale connecting");
      }
      return;
    }
    serviceSoftApTeardown(status, now);
    if (!status.staLinkMetricsValid || staRssiSampledAtMs_ == 0 ||
        static_cast<uint32_t>(now - staRssiSampledAtMs_) >=
            STA_RSSI_SAMPLE_INTERVAL_MS) {
      staRssiSampledAtMs_ = now;
      const int32_t rssi = WiFi.RSSI();
      dataMux_.lock();
      status_.staLinkMetricsValid = true;
      status_.staRssi = clampWifiRssi(rssi);
      status_.staSignalLevel = connectionSignalLevel(status_.staSignalLevel, true, status_.staRssi);
      status_.staSignalQualityPct = wifiRssiToSignalQualityPct(rssi);
      dataMux_.unlock();
    }
    if (!httpStartHeld_ && server_ == nullptr &&
        static_cast<int32_t>(now - httpRetryAtMs_) >= 0) {
      const bool httpReady = startHttpServer();
      httpRetryAtMs_ = httpReady ? 0 : now + HTTP_RETRY_MS;
      dataMux_.lock();
      status_.networkActive = httpReady;
      dataMux_.unlock();
      if (!httpReady) {
        lifecycleLog("HTTP start retry failed on STA");
      }
    }
    if (staConfirmArmed_ &&
        staJoinHints().staConfigState ==
            static_cast<uint8_t>(StaConfigState::PENDING) &&
        static_cast<int32_t>(now - staConfirmDeadlineMs_) >= 0) {
      WiFi.disconnect(false, false);
      if (!revertPendingNetwork(now, "confirm timeout")) {
        noteNetworkRetryBackoff(false);
      } else {
        clearNetworkFailureStatus(false);
      }
    }
    return;
  }

  if ((status.staState == StaState::CONNECTING ||
       status.staState == StaState::DISCONNECTED ||
       status.staState == StaState::FAILED) &&
      WiFi.status() == WL_CONNECTED) {
    char address[16] = {};
    formatIp(WiFi.localIP(), address);
    char previousAddress[16] = {};
    finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::SUCCESS);
    serviceSoftApTeardown(status, now);
    const bool mayStartHttp = !httpStartHeld_ &&
                              static_cast<int32_t>(now - httpRetryAtMs_) >= 0;
    const bool httpReady = mayStartHttp && startHttpServer();
    httpRetryAtMs_ = httpReady || httpStartHeld_ ? 0 : now + HTTP_RETRY_MS;
    staEverConnected_ = true;
    {
      const int32_t rssi = WiFi.RSSI();
      char bssidText[18] = {};
      const uint8_t *bssid = WiFi.BSSID();
      if (bssid != nullptr) {
        formatWifiMac(bssid, bssidText, sizeof(bssidText));
      }
      dataMux_.lock();
      status_.staState = StaState::CONNECTED;
      status_.networkActive = httpReady;
      copyCString(previousAddress, sizeof(previousAddress), status_.iface.staIp);
      copyCString(status_.iface.staIp, sizeof(status_.iface.staIp), address);
      status_.staLinkMetricsValid = true;
      status_.staRssi = clampWifiRssi(rssi);
      status_.staSignalLevel = connectionSignalLevel(status_.staSignalLevel, true, status_.staRssi);
      status_.staSignalQualityPct = wifiRssiToSignalQualityPct(rssi);
      status_.windowRemainingMs = 0;
      copyCString(status_.iface.staBssid, sizeof(status_.iface.staBssid), bssidText);
      dataMux_.unlock();
      if (callbacks_.reportStationIpChange != nullptr &&
          strcmp(previousAddress, address) != 0) {
        callbacks_.reportStationIpChange(address);
      }
      staRssiSampledAtMs_ = now;
      log(DebugCategory::NETWORK, DebugCode::STA_CONNECTED);
      lifecycleLogf("WiFi STA connected; IP: %s bssid=%s rssi=%ld http=%s",
                    address, bssidText[0] != '\0' ? bssidText : "-",
                    static_cast<long>(rssi),
                    httpReady ? "up" : (httpStartHeld_ ? "held" : "down"));
    }
    staNtpEligibleAtMs_ = now + NTP_STA_SETTLE_MS;
    ntpRearmPending_ = true;
    const StaJoinHints hints = staJoinHints();
    if (hints.staConfigState == static_cast<uint8_t>(StaConfigState::PENDING)) {
      armPendingConfirmWindow(now, status.apActive && setupKeepsSoftAp(status.setupPhase)
                                       ? SETUP_CONFIRM_TIMEOUT_MS
                                       : STA_CONFIRM_TIMEOUT_MS);
    }
    if (status.setupPhase == SetupPhase::ATTEMPTING ||
        status.setupPhase == SetupPhase::FAILED) {
      // Only a lease on the saved SSID proves this attempt. The owner then
      // saves it from the page ("Save Wi-Fi" confirms); until then the setup
      // network stays up, and a phone that lost it when the SoftAP followed
      // the station's channel can rejoin and resume on the same step.
      wifi_ap_record_t ap = {};
      const bool joined =
          esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
          strncmp(reinterpret_cast<const char *>(ap.ssid), hints.staSsid,
                  sizeof(ap.ssid)) == 0;
      advanceSetupPhase(joined ? SetupEvent::JOINED : SetupEvent::JOINED_OTHER);
    }
    applyWifiPowerSave();
    return;
  }

  // Live-apply onboarding window: the pending config armed the confirm timer
  // at connect start but never reached a user confirmation (page closed, or
  // wrong credentials kept the retries from connecting). Revert to the
  // previous state; the SoftAP keeps serving /setup for another attempt.
  if (status.apActive && setupKeepsSoftAp(status.setupPhase) &&
      staConfirmArmed_ &&
      !wifiScanInProgress() &&
      static_cast<int32_t>(now - staConfirmDeadlineMs_) >= 0) {
    WiFi.disconnect(false, false);
    if (!revertPendingNetwork(now, "confirm timeout")) {
      // Durable-write gate closed: retry in a second without the startup
      // backoff (noteNetworkRetryBackoff would clear startupComplete_ and
      // restart networking, dropping the onboarding SoftAP).
      dataMux_.lock();
      staConfirmDeadlineMs_ = now + 1000;
      dataMux_.unlock();
    } else {
      clearNetworkFailureStatus(false);
    }
    return;
  }

  if (status.staState == StaState::CONNECTING &&
      static_cast<uint32_t>(millis() - staConnectStartedAtMs_) >=
          STA_CONNECT_TIMEOUT_MS &&
      !status.apActive) {
    // SoftAP auto-raise is boot/bootstrap only (never after a prior CONNECTED).
    if (wifiScanInProgress()) {
      return;
    }
    // Log before our own disconnect() reports a reason of its own.
    log(DebugCategory::NETWORK, DebugCode::STA_FAILED,
        static_cast<int32_t>(WiFi.status()), staDisconnectDetail_.exchange(0));
    WiFi.disconnect(false, false);
    finishOwnedHeapLifecycle(wifiHeap_, HeapLifecycleResult::FAILURE);
    setStaState(StaState::FAILED);
    const bool pending = staJoinHints().staConfigState ==
                         static_cast<uint8_t>(StaConfigState::PENDING);
    if (pending) {
      lifecycleLog("WiFi STA failed; reverting pending config");
      if (!revertPendingNetwork(now, "connect timeout")) {
        noteNetworkRetryBackoff(true);
      } else {
        clearNetworkFailureStatus(false);
      }
      return;
    }
    if (staEverConnected_) {
      lifecycleLog("WiFi STA failed; SoftAP suppressed after prior connect");
      clearNetworkFailureStatus(true);
      staReconnectAttemptAtMs_ = now;
      return;
    }
    lifecycleLog("WiFi STA failed; SoftAP up, STA retry continues");
    if (apStartHeld_ || apAutoRaiseExhausted_) {
      lifecycleLog(apStartHeld_ ? "SoftAP raise held after STA fail"
                                : "SoftAP auto-raise exhausted after STA fail");
      clearNetworkFailureStatus(true);
    } else if (!ensureAccessPoint(now)) {
      noteNetworkRetryBackoff(true);
    } else {
      clearNetworkFailureStatus(true);
    }
    staReconnectAttemptAtMs_ = now;
    return;
  }

  // SoftAP after STA bootstrap window while still DISCONNECTED/FAILED (e.g.
  // scan delayed the reconnect begin). Never after a prior CONNECTED session,
  // AP_STOP hold, or SoftAP idle exhaustion. Also skip while brew RF is active
  // so SoftAP raise cannot steal airtime.
  if (!staEverConnected_ && !status.apActive && !brewRf && !scaleConnecting &&
      !apStartHeld_ && !apAutoRaiseExhausted_ &&
      (status.staState == StaState::DISCONNECTED ||
       status.staState == StaState::FAILED) &&
      static_cast<uint32_t>(millis() - staConnectStartedAtMs_) >=
          STA_CONNECT_TIMEOUT_MS &&
      !wifiScanInProgress()) {
    if (!ensureAccessPoint(now)) {
      noteNetworkRetryBackoff(true);
      return;
    }
    staReconnectAttemptAtMs_ = now;
  }

  if (status.staState == StaState::CONNECTING && status.apActive &&
      !wifiScanInProgress()) {
    const wl_status_t wifiStatus = WiFi.status();
    const bool terminalFail = wifiStatus == WL_CONNECT_FAILED ||
                              wifiStatus == WL_NO_SSID_AVAIL ||
                              wifiStatus == WL_CONNECTION_LOST;
    const bool liveOnboarding = status.setupPhase == SetupPhase::ATTEMPTING;
    const bool attemptAged =
        static_cast<uint32_t>(millis() - staConnectStartedAtMs_) >=
        (liveOnboarding ? LIVE_APPLY_ATTEMPT_MS : STA_RECOVERY_ATTEMPT_MS);
    if (terminalFail || attemptAged) {
      setStaState(StaState::DISCONNECTED);
      staReconnectAttemptAtMs_ = now;
      if (liveOnboarding) {
        // Hold the reconnect loop: the /setup page owns the next attempt,
        // and each reconnect would abort any UI scan (empty picker after a
        // failed onboarding connect). A new save, confirm, or the 180 s
        // window revert releases the hold.
        advanceSetupPhase(SetupEvent::ATTEMPT_FAILED);
        lifecycleLog(terminalFail
                         ? "WiFi STA onboarding attempt failed; retry from the page"
                         : "WiFi STA onboarding attempt timed out; retry from the page");
      } else if (terminalFail) {
        lifecycleLog("WiFi STA recovery attempt ended; will retry");
      }
      return;
    }
  }

  if ((status.staState == StaState::DISCONNECTED ||
       status.staState == StaState::FAILED) &&
      !reconnectHeld && !wifiScanInProgress() && !brewRf &&
      !scaleConnecting &&
      static_cast<uint32_t>(now - staReconnectAttemptAtMs_) >=
          staReconnectIntervalMs_) {
    const uint32_t next = staReconnectIntervalMs_ >= STA_RECONNECT_MAX_INTERVAL_MS / 2U
        ? STA_RECONNECT_MAX_INTERVAL_MS : staReconnectIntervalMs_ * 2U;
    if (beginStationConnect(networkSettingsSnapshot(), now)) staReconnectIntervalMs_ = next;
  }
}

void OpenBrewByWeightNetwork::serviceWifiScan(uint32_t now) {
  const ControlGateSnapshot control = controlGate();
  const bool matchingMaintenanceLease =
      scanMaintenanceLeaseId_ != 0 && control.maintenanceLeaseActive &&
      control.maintenanceLeaseId == scanMaintenanceLeaseId_ &&
      !control.activeCycle && !control.relayClosed &&
      !control.physicalActivatorOn;
  const bool safe = controlAllowsConfiguration(control) ||
                    matchingMaintenanceLease;

  bool requested = false;
  WifiScanState state;
  dataMux_.lock();
  requested = scanRequested_;
  state = g_wifiScan.state;
  dataMux_.unlock();

  // A bare QUEUED set optimistically by the scan handler is still waiting
  // for its own maintenance lease to publish; canceling it here races the
  // command pipeline and logs a spurious cancel on every /setup or Admin
  // scan. Only cancel scans this task owns (requested) or that are on the
  // radio (RUNNING).
  if (scaleHuntRfActive_.load(std::memory_order_relaxed) ||
      scaleConnecting_.load(std::memory_order_relaxed)) {
    const bool canceled = requested || state == WifiScanState::RUNNING;
    if (canceled) {
      abortWifiScan(now, false);
    }
    return;
  }

  if (!safe) {
    const bool canceled = requested || state == WifiScanState::RUNNING;
    if (canceled) {
      abortWifiScan(now, false);
    }
    return;
  }

  if (requested && state == WifiScanState::QUEUED) {
    const int16_t result =
        WiFi.scanNetworks(true, false, false, 120);
    dataMux_.lock();
    scanRequested_ = false;
    if (result == WIFI_SCAN_RUNNING) {
      g_wifiScan.state = WifiScanState::RUNNING;
      g_wifiScan.updatedAtMs = now;
    }
    dataMux_.unlock();
    if (result == WIFI_SCAN_RUNNING) {
      log(DebugCategory::NETWORK, DebugCode::WIFI_SCAN_STARTED);
      return;
    }
    finishWifiScan(result, now);
    return;
  }

  if (state != WifiScanState::RUNNING) {
    return;
  }
  uint32_t scanStartedAtMs = 0;
  dataMux_.lock();
  scanStartedAtMs = g_wifiScan.updatedAtMs;
  dataMux_.unlock();
  if (static_cast<uint32_t>(now - scanStartedAtMs) >= WIFI_SCAN_TIMEOUT_MS) {
    abortWifiScan(now, true);
    return;
  }
  const int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    return;
  }
  finishWifiScan(result, now);
}

void OpenBrewByWeightNetwork::finishWifiScan(int16_t resultCount, uint32_t now) {
  WifiScanSnapshot &completed = g_wifiScanWorking;
  completed = WifiScanSnapshot{};
  completed.updatedAtMs = now;
  if (resultCount < 0) {
    completed.state = WifiScanState::FAILED;
    WiFi.scanDelete();
    dataMux_.lock();
    g_wifiScan = completed;
    dataMux_.unlock();
    log(DebugCategory::NETWORK, DebugCode::WIFI_SCAN_ERROR, resultCount);
    if (scanMaintenanceLeaseId_ != 0) {
      WebCommand command;
      command.type = WebCommandType::START_WIFI_SCAN;
      command.requestId = scanRequestId_;
      command.maintenanceLeaseId = scanMaintenanceLeaseId_;
      (void)enqueueMaintenanceCompletion(command, false);
      scanMaintenanceLeaseId_ = 0;
      scanRequestId_ = 0;
    }
    return;
  }

  completed.state = WifiScanState::READY;
  // Arduino's SCAN_DONE handler already copied the IDF AP list into
  // WiFiScanClass::_scanResult. A second IDF fetch returns 0 APs.
  for (int16_t index = 0;
       index < resultCount && completed.count < MAX_WIFI_SCAN_RESULTS;
       ++index) {
    const wifi_ap_record_t *apPtr = static_cast<const wifi_ap_record_t *>(
        WiFi.getScanInfoByIndex(index));
    if (apPtr == nullptr) {
      continue;
    }
    const wifi_ap_record_t &ap = *apPtr;
    const size_t length =
        strnlen(reinterpret_cast<const char *>(ap.ssid), sizeof(ap.ssid));
    if (length == 0 || length >= WIFI_SSID_CAPACITY) {
      continue;
    }
    const int32_t rssi = ap.rssi;
    size_t duplicate = completed.count;
    for (size_t item = 0; item < completed.count; ++item) {
      if (strcmp(completed.networks[item].ssid,
                 reinterpret_cast<const char *>(ap.ssid)) == 0) {
        duplicate = item;
        break;
      }
    }
    if (duplicate < completed.count) {
      if (rssi > completed.networks[duplicate].rssi) {
        completed.networks[duplicate].rssi = rssi;
        completed.networks[duplicate].channel = ap.primary;
        completed.networks[duplicate].open = ap.authmode == WIFI_AUTH_OPEN;
      }
      continue;
    }
    WifiScanNetwork &network = completed.networks[completed.count++];
    memcpy(network.ssid, ap.ssid, length);
    network.ssid[length] = '\0';
    network.rssi = rssi;
    network.channel = ap.primary;
    network.open = ap.authmode == WIFI_AUTH_OPEN;
  }

  for (size_t outer = 1; outer < completed.count; ++outer) {
    WifiScanNetwork current = completed.networks[outer];
    size_t inner = outer;
    while (inner > 0 && completed.networks[inner - 1].rssi < current.rssi) {
      completed.networks[inner] = completed.networks[inner - 1];
      --inner;
    }
    completed.networks[inner] = current;
  }
  WiFi.scanDelete();
  dataMux_.lock();
  g_wifiScan = completed;
  dataMux_.unlock();
  log(DebugCategory::NETWORK, DebugCode::WIFI_SCAN_COMPLETE,
      completed.count, resultCount);
  if (scanMaintenanceLeaseId_ != 0) {
    WebCommand command;
    command.type = WebCommandType::START_WIFI_SCAN;
    command.requestId = scanRequestId_;
    command.maintenanceLeaseId = scanMaintenanceLeaseId_;
    (void)enqueueMaintenanceCompletion(command, true);
    scanMaintenanceLeaseId_ = 0;
    scanRequestId_ = 0;
  }
}

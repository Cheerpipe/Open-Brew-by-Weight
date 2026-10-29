esp_err_t OpenBrewByWeightNetwork::networkHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char action[16] = {};
  WebCommand command;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  static const char *const actionFields[] = {"action"};
  static const char *const brewFields[] = {"action", "brewByWeight", "presetId"};
  static const char *const saveFields[] = {
      "action", "ssid",    "password", "open", "wifiSleep", "ipMode", "ip",
      "netmask", "gateway", "dns1",    "dns2", "name",      "apply", "baseRevision"};
  const char *networkError = nullptr;
  SetupBrew brew = SetupBrew::NONE;
  uint8_t brewPresetId = 0;
  bool brewSaveFailed = false, networkStale = false;
  bool parsed = root != nullptr &&
                jsonString(root, "action", action, sizeof(action), false);
  // skip, discard, and brew act only for a peer on the running setup network.
  const bool setupPeer = self.snapshot().apActive &&
                         socketServedBySoftAp(httpd_req_to_sockfd(request));
  if (parsed && strcmp(action, "confirm") != 0 && strcmp(action, "skip") != 0 &&
      strcmp(action, "finish") != 0) {
    if (!self.requireAdminUnlock(request)) {
      cJSON_Delete(root);
      self.unlockJsonBody();
      command.wipePassword();
      return ESP_OK;
    }
  }
  // The factory password is public: setup (save or skip) needs the owner's own.
  if (parsed && (strcmp(action, "save") == 0 || strcmp(action, "skip") == 0) &&
      socketServedBySoftAp(httpd_req_to_sockfd(request)) &&
      [&self] { TaskLockGuard lock(self.dataMux_); return passwordIsFactoryDefault(self.settings_); }()) {
    cJSON_Delete(root);
    self.unlockJsonBody();
    return sendError(request, STATUS_CONFLICT, "DEVICE_PASSWORD_REQUIRED",
                     "Choose your own device password before finishing setup.");
  }
  if (parsed && strcmp(action, "confirm") == 0) {
    parsed = jsonHasOnlyUniqueFields(root, actionFields, 1);
    if (!parsed) {
      networkError = "Confirm request must include only action=\"confirm\".";
    } else {
      const StaJoinHints sta = self.staJoinHints();
      const NetworkStatusSnapshot network = self.snapshot();
      if (sta.staConfigState !=
              static_cast<uint8_t>(StaConfigState::PENDING) ||
          !sta.staConfigured ||
          !setupConfirmAllowed(network.setupPhase, network.apActive) ||
          network.staState != StaState::CONNECTED) {
        parsed = false;
        networkError = "No pending network configuration to confirm.";
      } else {
        self.requestPendingNetworkConfirm(true);
        cJSON_Delete(root);
        self.unlockJsonBody();
        return self.sendAccepted(request, command.requestId,
                                 "\"state\":\"QUEUED\"");
      }
    }
  } else if (parsed && strcmp(action, "skip") == 0) {
    // Setup-context action (SoftAP peer, same trust as confirm): arm the
    // portal-exit probe answers so the OS captive window closes as "logged
    // in" while the association — and /setup at 192.168.4.1 — stay up.
    parsed = jsonHasOnlyUniqueFields(root, actionFields, 1);
    if (!parsed) {
      networkError = "Skip request must include only action=\"skip\".";
    } else if (!setupPeer) {
      parsed = false;
      networkError = "No setup network to skip.";
    } else {
      self.portalExitRequested_.store(true, std::memory_order_release);
      cJSON_Delete(root);
      self.unlockJsonBody();
      return sendJson(request, STATUS_OK, "{\"skipped\":true}");
    }
  } else if (parsed && strcmp(action, "finish") == 0) {
    // Setup-context action: valid while the saved network's teardown grace
    // runs, or (firmware off) after "skip"; everything is already saved, so
    // it only ends the session early. Arm the portal-exit probe answers,
    // then shorten the SoftAP teardown to a short linger so the captive
    // window can fetch its probe-success page before the link drops.
    parsed = jsonHasOnlyUniqueFields(root, actionFields, 1);
    if (!parsed) {
      networkError = "Finish request must include only action=\"finish\".";
    } else if (!socketServedBySoftAp(httpd_req_to_sockfd(request))) {
      parsed = false;
      networkError = "No setup network to finish.";
    } else {
      const uint32_t now = millis();
      self.dataMux_.lock();
      const bool grace =
          setupFinishAllowed(self.status_.setupPhase, self.status_.apActive,
                             self.status_.apTeardownAtMs, now);
      // After "Continue without Wi-Fi" the onboarding still ends here.
      const bool skipped = firmwareCompatibilityMode() && self.status_.apActive &&
                           self.status_.setupPhase == SetupPhase::NONE;
      self.dataMux_.unlock();
      if (!grace && !skipped) {
        parsed = false;
        networkError = "No setup network to finish.";
      } else {
        if (grace) {
          self.dataMux_.lock();
          self.status_.apTeardownAtMs = now + SOFTAP_FINISH_LINGER_MS;
          self.dataMux_.unlock();
        } else if (self.setupRestartPending_.load(std::memory_order_acquire)) {
          self.requestSetupRestart();
        }
        self.portalExitRequested_.store(true, std::memory_order_release);
        cJSON_Delete(root);
        self.unlockJsonBody();
        return sendJson(request, STATUS_OK, "{\"finished\":true}");
      }
    }
  } else if (parsed && strcmp(action, "discard") == 0) {
    // Setup-context action: "Keep" or "Continue without Wi-Fi" drops this
    // session's unsaved test now; nothing to drop is not an error.
    parsed = jsonHasOnlyUniqueFields(root, actionFields, 1);
    if (!parsed) {
      networkError = "Discard request must include only action=\"discard\".";
    } else if (!setupPeer) {
      parsed = false;
      networkError = "No setup network to discard a test from.";
    } else {
      self.requestPendingNetworkDiscard();
      cJSON_Delete(root);
      self.unlockJsonBody();
      return sendJson(request, STATUS_OK, "{\"discarded\":true}");
    }
  } else if (parsed && strcmp(action, "brew") == 0) {
    // Setup-context action: part one of onboarding saves the brewing choice
    // on its own, before and independent of Wi-Fi, without restarting.
    networkError = !jsonHasOnlyUniqueFields(root, brewFields, 3)
                       ? "Brew request accepts only action=\"brew\" and the brewing choice."
                   : !setupPeer ? "No setup network to save the brewing choice."
                                : parseSetupBrew(root, brew, brewPresetId);
    if (networkError != nullptr) {
      parsed = false;
    } else if (!self.saveSetupBrew(brew, brewPresetId)) {
      parsed = false;
      brewSaveFailed = true;
    } else {
      cJSON_Delete(root);
      self.unlockJsonBody();
      return sendJson(request, STATUS_OK, "{\"saved\":true}");
    }
  } else if (parsed && strcmp(action, "forget") == 0) {
    if (!self.webUiConfigurationAllowed(request, status)) {
      cJSON_Delete(root);
      self.unlockJsonBody();
      return sendError(request, STATUS_CONFLICT,
                       "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                       "Network settings are locked while a cycle is active.");
    }
    parsed = jsonHasOnlyUniqueFields(root, actionFields, 1);
    if (parsed) {
      command.type = WebCommandType::FORGET_NETWORK;
    } else {
      networkError = "Forget request must include only action=\"forget\".";
    }
  } else if (parsed && strcmp(action, "save") == 0) {
    if (!self.webUiConfigurationAllowed(request, status)) {
      cJSON_Delete(root);
      self.unlockJsonBody();
      return sendError(request, STATUS_CONFLICT,
                       "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                       "Network settings are locked while a cycle is active.");
    }
    command.setNetworkType(WebCommandType::SAVE_NETWORK);
    char ipMode[16] = {}, ipText[16] = {}, netmaskText[16] = {}, gatewayText[16] = {},
         dns1Text[16] = {}, dns2Text[16] = {};
    const bool hasIp = jsonFieldPresent(root, "ip"), hasNetmask = jsonFieldPresent(root, "netmask"),
               hasGateway = jsonFieldPresent(root, "gateway"),
               hasDns1 = jsonFieldPresent(root, "dns1"), hasDns2 = jsonFieldPresent(root, "dns2");
    if (!jsonHasOnlyUniqueFields(root, saveFields, 14) ||
        !jsonString(root, "ssid", command.network.ssid, sizeof(command.network.ssid), false) ||
        !jsonString(root, "password", command.network.password,
                    sizeof(command.network.password), true) ||
        !jsonBoolean(root, "open", command.network.openNetwork) ||
        !jsonBoolean(root, "wifiSleep", command.network.wifiSleep) ||
        !jsonString(root, "ipMode", ipMode, sizeof(ipMode), false)) {
      parsed = false;
      networkError =
          "Save request requires action, ssid, password, open, wifiSleep, and ipMode.";
    } else if (strcmp(ipMode, "dhcp") == 0) {
      command.network.staIpMode = static_cast<uint8_t>(StaIpMode::DHCP);
      if (hasIp || hasNetmask || hasGateway || hasDns1 || hasDns2) {
        parsed = false;
        networkError = "DHCP save must not include static address fields.";
      }
    } else if (strcmp(ipMode, "static") == 0) {
      command.network.staIpMode = static_cast<uint8_t>(StaIpMode::STATIC);
      if (!hasIp || !hasNetmask || !hasGateway || !hasDns1 ||
          !jsonString(root, "ip", ipText, sizeof(ipText), false) ||
          !jsonString(root, "netmask", netmaskText, sizeof(netmaskText),
                      false) ||
          !jsonString(root, "gateway", gatewayText, sizeof(gatewayText),
                      false) ||
          !jsonString(root, "dns1", dns1Text, sizeof(dns1Text), false) ||
          (hasDns2 && !jsonString(root, "dns2", dns2Text, sizeof(dns2Text),
                                  false)) ||
          !parseIpv4(ipText, command.network.staIp) ||
          !parseIpv4(netmaskText, command.network.staNetmask) ||
          !parseIpv4(gatewayText, command.network.staGateway) ||
          !parseIpv4(dns1Text, command.network.staDns1) ||
          (hasDns2 && !parseIpv4(dns2Text, command.network.staDns2))) {
        parsed = false;
        networkError =
            "Static IP save requires valid ip, netmask, gateway, and dns1.";
      }
    } else {
      parsed = false;
      networkError = "ipMode must be \"dhcp\" or \"static\".";
    }
    const bool hasName = cJSON_GetObjectItemCaseSensitive(root, "name") != nullptr;
    if (parsed && hasName &&
        !jsonString(root, "name", command.network.deviceName,
                    sizeof(command.network.deviceName), false)) {
      parsed = false;
      networkError = "Save request name must be 1-32 characters.";
    } else if (parsed && hasName &&
               !validDeviceName(command.network.deviceName)) {
      parsed = false;
      networkError =
          "Device name must be 1-32 letters, digits, spaces, or hyphens, "
          "without a leading or trailing space or hyphen.";
    }
    const bool hasApply = jsonFieldPresent(root, "apply");
    if (parsed && hasApply) {
      char applyMode[8] = {};
      if (!jsonString(root, "apply", applyMode, sizeof(applyMode), false) ||
          strcmp(applyMode, "live") != 0) {
        parsed = false;
        networkError = "apply must be \"live\" when provided.";
      } else if (!self.snapshot().apActive) {
        parsed = false;
        networkError = "Live apply is available only from the setup network.";
      } else {
        command.network.applyLive = true;
      }
    }
    // A form filled before another screen saved is refused (Admin's revision).
    networkStale = parsed &&
        !baseRevisionCurrent(root, networkRevision(self.settings_, self.dataMux_));
    parsed = parsed && !networkStale;
    if (parsed) {
      command.network.wifiSleepSpecified = true;
    }
    if (parsed && hasName) {
      command.network.deviceNameSpecified = true;
    }
    if (parsed && !validWifiSsid(command.network.ssid)) {
      parsed = false;
      networkError = "SSID must be 1–32 characters.";
    } else if (parsed) {
      const StaJoinHints sta = self.staJoinHints();
      const bool reusePassword = shouldReuseSavedWifiCredentials(
          command.network.ssid, command.network.password, command.network.openNetwork,
          sta.staConfigured, sta.staSsid, sta.staOpen);
      if (!reusePassword &&
          !validWifiPassword(command.network.password, command.network.openNetwork)) {
        parsed = false;
        networkError = command.network.openNetwork
                           ? "Open network password must be empty."
                           : (sta.staConfigured
                                  ? "Wi-Fi password must be 8–63 characters, "
                                    "or empty to keep the saved password."
                                  : "Wi-Fi password must be 8–63 characters for a "
                                    "secured network.");
      } else if (!validStaAddressConfig(command.network.staIpMode, command.network.staIp,
                                        command.network.staNetmask, command.network.staGateway,
                                        command.network.staDns1, command.network.staDns2)) {
        parsed = false;
        networkError =
            "Static IP, netmask, gateway, and DNS must be valid and on the "
            "same subnet (not 192.168.4.x).";
      }
    }
  } else {
    parsed = false;
    networkError =
        "action must be \"save\", \"forget\", \"confirm\", \"skip\", \"finish\", \"brew\", or \"discard\".";
  }
  cJSON_Delete(root);
  self.unlockJsonBody();
  if (!parsed) {
    command.wipePassword();
    if (brewSaveFailed) {
      return sendError(request, STATUS_UNAVAILABLE, "SETUP_BREW_SAVE_FAILED",
                       "Could not save the brewing choice.");
    }
    if (networkStale) {
      return sendError(request, STATUS_CONFLICT, "STALE_REVISION",
                       "Network settings changed; refresh and retry.");
    }
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_NETWORK",
                     networkError != nullptr
                         ? networkError
                         : "Invalid network SSID or password.");
  }
  if (!self.callbacks_.enqueueWebCommand(command)) {
    command.wipePassword();
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "Nothing was saved.");
  }
  command.wipePassword();
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::wifiScanStartHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     "Wi-Fi scanning is available only while Ready.");
  }

  WebCommand command;
  command.type = WebCommandType::START_WIFI_SCAN;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "Control is busy; scan was not started.");
  }

  self.dataMux_.lock();
  g_wifiScan.state = WifiScanState::QUEUED;
  self.dataMux_.unlock();

  return self.sendAccepted(request, command.requestId,
                           "\"state\":\"QUEUED\"");
}

esp_err_t OpenBrewByWeightNetwork::wifiScanStatusHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  if (!self.lockWorkBuf()) {
    return self.workBufBusy(request);
  }
  WifiScanSnapshot &scan = self.workBuf_->wifiScan;
  self.dataMux_.lock();
  scan = g_wifiScan;
  self.dataMux_.unlock();

  httpd_resp_set_type(request, JSON_CONTENT_TYPE);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "Connection", "close");
  char prefix[128] = {};
  snprintf(prefix, sizeof(prefix),
           "{\"state\":\"%s\",\"updatedAtMs\":%lu,\"networks\":[",
           wifiScanStateName(scan.state),
           static_cast<unsigned long>(scan.updatedAtMs));
  if (httpd_resp_send_chunk(request, prefix, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
    self.unlockWorkBuf();
    return ESP_FAIL;
  }
  for (size_t index = 0; index < scan.count; ++index) {
    if (httpd_resp_send_chunk(request, index == 0 ? "{\"ssid\":" :
                                                  ",{\"ssid\":",
                              HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        sendJsonStringChunk(request, scan.networks[index].ssid) != ESP_OK) {
      self.unlockWorkBuf();
      return ESP_FAIL;
    }
    char suffix[96] = {};
    snprintf(suffix, sizeof(suffix),
             ",\"rssi\":%ld,\"channel\":%u,\"open\":%s}",
             static_cast<long>(scan.networks[index].rssi),
             static_cast<unsigned>(scan.networks[index].channel),
             scan.networks[index].open ? "true" : "false");
    if (httpd_resp_send_chunk(request, suffix, HTTPD_RESP_USE_STRLEN) !=
        ESP_OK) {
      self.unlockWorkBuf();
      return ESP_FAIL;
    }
  }
  if (httpd_resp_send_chunk(request, "]}", 2) != ESP_OK) {
    self.unlockWorkBuf();
    return ESP_FAIL;
  }
  const esp_err_t sent = httpd_resp_send_chunk(request, nullptr, 0);
  self.unlockWorkBuf();
  return sent;
}

esp_err_t OpenBrewByWeightNetwork::devicePasswordHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  if (!self.webUiConfigurationAllowed(request, self.controlGate())) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     "The password cannot be changed while a cycle is active.");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  WebCommand command;
  command.setNetworkType(WebCommandType::CHANGE_DEVICE_PASSWORD);
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {"newPassword"};
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonString(root, "newPassword", command.network.password,
                 sizeof(command.network.password), false);
  cJSON_Delete(root);
  self.unlockJsonBody();
  if (!parsed) {
    command.wipePassword();
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_DEVICE_PASSWORD",
                     "New device password is required.");
  }
  if (!validDevicePassword(command.network.password)) {
    command.wipePassword();
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_DEVICE_PASSWORD",
                     "New device password must be 8–63 characters.");
  }
  if (isFactoryDefaultPassword(command.network.password)) {
    command.wipePassword();
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_DEVICE_PASSWORD",
                     "New device password cannot be the factory default.");
  }
  // From the setup network, keep it running (new passphrase at its next start).
  command.network.applyLive = socketServedBySoftAp(httpd_req_to_sockfd(request));
  self.keepPasswordChangeSession(request, command.requestId);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    command.wipePassword();
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "Nothing was saved.");
  }
  command.wipePassword();
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::bleScanHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {"scanIntensity", "enabled"};
  char intensityText[12] = {};
  BleScanIntensity intensity = BleScanIntensity::BALANCED;
  bool enabled = false;
  const bool intensityPresent =
      root != nullptr && jsonFieldPresent(root, "scanIntensity");
  const bool enabledPresent =
      root != nullptr && jsonFieldPresent(root, "enabled");
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 2) &&
      (intensityPresent || enabledPresent) &&
      (!intensityPresent ||
       (jsonString(root, "scanIntensity", intensityText,
                   sizeof(intensityText), false) &&
        parseBleScanIntensityId(intensityText, intensity))) &&
      (!enabledPresent || jsonBoolean(root, "enabled", enabled));
  cJSON_Delete(root);
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_BLE_CONFIG",
                     "scanIntensity must be balanced, aggressive, or relaxed; "
                     "enabled must be a boolean.");
  }
  WebCommand command;
  command.type = WebCommandType::BLE_SCAN_INTENSITY;
  if (intensityPresent) {
    command.bleScan.specified |= BleScanCommandPayload::INTENSITY;
    command.bleScan.intensity = static_cast<uint8_t>(intensity);
  }
  if (enabledPresent) {
    command.bleScan.specified |= BleScanCommandPayload::ENABLED;
    command.bleScan.enabled = enabled ? 1 : 0;
  }
  command.requestId = self.allocateRequestId();
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "Scan intensity was not changed.");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::scaleProfileHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char action[16] = {};
  static const char *const fields[] = {"action"};
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonString(root, "action", action, sizeof(action), false);
  cJSON_Delete(root);
  self.unlockJsonBody();
  WebCommandType type;
  if (parsed && strcmp(action, "start") == 0) {
    type = WebCommandType::SCALE_PROFILE_START;
  } else if (parsed && strcmp(action, "stop") == 0) {
    type = WebCommandType::SCALE_PROFILE_STOP;
  } else if (parsed && strcmp(action, "delete") == 0) {
    type = WebCommandType::SCALE_PROFILE_DELETE;
  } else {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_PROFILE_ACTION",
                     "action must be start, stop, or delete.");
  }
  WebCommand command;
  command.type = type;
  command.requestId = self.allocateRequestId();
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "Scale profile state was not changed.");
  }
  return self.sendAccepted(request, command.requestId);
}

namespace {

// Bounded line accumulator for the streamed TXT download; flushes in
// HTTP-sized chunks so no full text buffer is ever allocated.
class ScaleProfileTextSink {
 public:
  explicit ScaleProfileTextSink(httpd_req_t *request) : request_(request) {}

  bool line(const char *text, size_t length) {
    if (length + 1 > sizeof(buffer_)) return false;
    if (used_ + length + 1 > sizeof(buffer_)) {
      if (!flush()) return false;
    }
    memcpy(buffer_ + used_, text, length);
    used_ += length;
    buffer_[used_++] = '\n';
    return true;
  }

  // printf-style line; formatting errors and truncation fail the export.
  __attribute__((format(printf, 2, 3))) bool linef(const char *format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return written > 0 && unsigned(written) < sizeof(text) &&
           line(text, static_cast<size_t>(written));
  }

  bool flush() {
    if (used_ == 0) return true;
    const bool ok = sendChunk(request_, buffer_, used_) == ESP_OK;
    used_ = 0;
    return ok;
  }

 private:
  httpd_req_t *request_;
  char buffer_[1024] = {};
  size_t used_ = 0;
};

constexpr size_t SCALE_PROFILE_DOWNLOAD_BATCH =
    FLASH_IO_CHUNK_BYTES / SCALE_PROFILE_RECORD_BYTES;
// Internal staging for one batch of flash-sourced records; lives for the
// whole handler call so httpd only ever sees stable bytes.
uint8_t scaleProfileDownloadBatch[FLASH_IO_CHUNK_BYTES];

void copyScaleProfileContextText(char *destination, size_t capacity,
                                 const char *source) {
  size_t used = 0;
  if (destination != nullptr && capacity > 0) {
    while (source != nullptr && source[used] != '\0' &&
           used + 1 < capacity) {
      destination[used] = source[used];
      ++used;
    }
    destination[used] = '\0';
  }
}

}  // namespace

esp_err_t OpenBrewByWeightNetwork::scaleProfileDownloadHandler(
    httpd_req_t *request) {
  if (!scaleProfiler().acquireDownloadLease()) {
    return sendError(request, STATUS_CONFLICT, "SCALE_PROFILE_BUSY",
                     "The scale profile is changing or already downloading.");
  }
  const bool fromRam = scaleProfiler().downloadFromRam();
  const ScaleProfilerStatus status = scaleProfiler().status(millis());
  uint32_t recordCount = 0;
  uint32_t sessionId = status.sessionId;
  uint64_t durationMs = status.elapsedMs;
  uint32_t stopReason = static_cast<uint32_t>(status.stopReason);
  uint32_t lostCount = status.lostCount;
  uint32_t weightCount = 0;
  uint32_t eventCount = 0;
  uint32_t utcStartSec = 0;
  char context[SCALE_PROFILE_CONTEXT_BYTES + 1];
  bool valid = false;
  if (fromRam) {
    recordCount = scaleProfiler().downloadRecordCount();
    const ScaleProfileHeader header = scaleProfiler().downloadHeader();
    weightCount = header.weightCount;
    eventCount = header.eventCount;
    utcStartSec = header.startWallUtcSec;
    scaleProfiler().copyDownloadContext(context, sizeof(context));
    valid = recordCount != 0;
  } else {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    ScaleProfileHeader header;
    if (scaleProfiler().storeReadHeader(header)) {
      recordCount = header.recordCount;
      sessionId = header.sessionId;
      durationMs = scaleProfileDurationMs(header);
      stopReason = header.stopReason;
      lostCount = header.lostCount;
      weightCount = header.weightCount;
      eventCount = header.eventCount;
      utcStartSec = header.startWallUtcSec;
      copyScaleProfileContextText(context, sizeof(context), header.context);
      valid = recordCount != 0;
      if (valid && !scaleProfiler().storeVerifyChecksum(header)) {
        scaleProfiler().releaseDownloadLease();
        return sendError(request, STATUS_SERVER_ERROR, "SCALE_PROFILE_CORRUPT",
                         "The saved scale profile failed its checksum.");
      }
    }
#endif
  }
  if (!valid) {
    scaleProfiler().releaseDownloadLease();
    return sendError(request, STATUS_NOT_FOUND, "NO_SCALE_PROFILE",
                     "No scale profile is available to download.");
  }
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(
      request, "Content-Disposition",
      "attachment; filename=\"openbrewbyweight-scale-profile.txt\"");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  ScaleProfileTextSink sink(request);
  char line[192];
  bool ok = sink.linef("# Open Brew by Weight scale profile") &&
            sink.linef("sessionId=%lu", static_cast<unsigned long>(sessionId));
  if (ok) {
    ok = utcStartSec != 0
             ? sink.linef("utcStart=%lu",
                          static_cast<unsigned long>(utcStartSec))
             : sink.linef("utcStart=unknown");
  }
  ok = ok && sink.linef("durationMs=%llu stopReason=%s",
                        static_cast<unsigned long long>(durationMs),
                        scaleProfilerStopReasonName(
                            static_cast<ScaleProfilerStopReason>(stopReason)));
  ok = ok && sink.linef("records=%lu weights=%lu events=%lu losses=%lu",
                        static_cast<unsigned long>(recordCount),
                        static_cast<unsigned long>(weightCount),
                        static_cast<unsigned long>(eventCount),
                        static_cast<unsigned long>(lostCount));
  if (ok) {
    const char *savedLabel =
        fromRam ? (status.persistence == ScaleProfilerPersistence::FAILED
                       ? "no (save failed)"
                       : "no (not yet saved)")
                : "yes";
    ok = sink.linef("saved=%s", savedLabel) &&
         sink.linef("units=grams precision=float32 time=sessionMs");
  }
  for (const char *c = context; ok && c != nullptr && *c != '\0';) {
    const char *end = strchr(c, '\n');
    const size_t length =
        end != nullptr ? static_cast<size_t>(end - c) : strlen(c);
    if (length > 0) ok = sink.line(c, length);
    if (end == nullptr) break;
    c = end + 1;
  }
  ScaleProfileRecord record;
  for (uint32_t index = 0; ok && index < recordCount; ++index) {
    if (fromRam) {
      record = scaleProfiler().downloadRecordAt(index);
    } else {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
      if (index % SCALE_PROFILE_DOWNLOAD_BATCH == 0) {
        const size_t count =
            index + SCALE_PROFILE_DOWNLOAD_BATCH < recordCount
                ? SCALE_PROFILE_DOWNLOAD_BATCH
                : static_cast<size_t>(recordCount - index);
        if (!scaleProfiler().storeReadRecords(index, scaleProfileDownloadBatch,
                                              count)) {
          ok = false;
          break;
        }
      }
      memcpy(&record,
             scaleProfileDownloadBatch +
                 (index % SCALE_PROFILE_DOWNLOAD_BATCH) *
                     SCALE_PROFILE_RECORD_BYTES,
             sizeof(record));
#endif
    }
    const size_t length = formatScaleProfileRow(line, sizeof(line), record);
    if (length == 0) {
      ok = false;
    } else {
      ok = sink.line(line, length);
    }
  }
  const bool flushed = sink.flush();
  const esp_err_t finished =
      flushed ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
  scaleProfiler().releaseDownloadLease();
  return ok && flushed ? finished : ESP_FAIL;
}

// Full audit-log export: the merged durable window + latch, oldest first,
// one record per line with the composed detail rebuilt from the stored
// whole-gram arguments and excerpt. When a record has no wall time it was
// captured before the clock synchronized; the line says so instead of
// inventing a date.
esp_err_t OpenBrewByWeightNetwork::auditLogExportHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  if (self.callbacks_.copyAuditEvents == nullptr) {
    return sendError(request, STATUS_SERVER_ERROR, "AUDIT_EXPORT_UNAVAILABLE",
                     "The audit log is not available.");
  }
  AuditStoreStatus store{};
  if (self.callbacks_.copyAuditStoreStatus != nullptr) {
    self.callbacks_.copyAuditStoreStatus(store);
  }
  httpd_resp_set_type(request, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "attachment; filename=openbrewbyweight-audit.txt");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  ScaleProfileTextSink sink(request);
  bool ok =
      sink.linef("# Open Brew by Weight audit log") &&
      sink.linef("# state=%s retained=%lu seqHigh=%lu",
                 store.state == AuditStoreState::READY ? "ready"
                 : store.state == AuditStoreState::UNSUPPORTED ? "ram-only"
                                                               : "io-error",
                 static_cast<unsigned long>(store.retained),
                 static_cast<unsigned long>(store.seqHigh)) &&
      sink.linef("# when (unset = clock not synchronized at capture)"
                 "\tlevel\tcategory\tevent\tdetail");
  AuditRecord batch[16];
  uint32_t after = 0;
  while (ok) {
    const size_t count =
        self.callbacks_.copyAuditEvents(after, batch, 16, nullptr, nullptr);
    if (count == 0) break;
    for (size_t index = 0; ok && index < count; ++index) {
      const AuditRecord &record = batch[index];
      // Sized for GCC's worst-case int widths; manual UTC formatting keeps
      // the newlib locale machinery (strftime) out of the image.
      char when[80];
      if (record.wallSec != 0) {
        const time_t seconds = static_cast<time_t>(record.wallSec);
        struct tm utc = {};
        gmtime_r(&seconds, &utc);
        snprintf(when, sizeof(when), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                 utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour,
                 utc.tm_min, utc.tm_sec);
      } else {
        snprintf(when, sizeof(when), "unset");
      }
      char detail[128];
      formatAuditRecordDetail(record, detail, sizeof(detail));
      ok = sink.linef("%s\t%s\t%s\t%s\t%s", when,
                      logLevelName(static_cast<LogLevel>(record.level)),
                      debugCategoryName(
                          static_cast<DebugCategory>(record.category)),
                      debugCodeName(static_cast<DebugCode>(record.code)),
                      detail);
      after = record.sequence;
    }
    if (count < 16) break;
  }
  const bool flushed = sink.flush();
  const esp_err_t finished =
      flushed ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
  return ok && flushed ? finished : ESP_FAIL;
}

// Internal-heap block maps: the boot stages plus a fresh view, which is
// skipped while a shot runs (the walk briefly holds the heap locks).
esp_err_t OpenBrewByWeightNetwork::heapMapDownloadHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const bool live = !self.controlCriticalRfActive_.load(std::memory_order_acquire);
  if (live) captureHeapMap(HeapMapSlot::NOW, millis());
  httpd_resp_set_type(request, "text/csv; charset=utf-8");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "attachment; filename=openbrewbyweight-heap-map.csv");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  ScaleProfileTextSink sink(request);
  bool ok = sink.linef("# Open Brew by Weight internal heap map, firmware %s, uptime %lu ms%s",
                       FW_VERSION, static_cast<unsigned long>(millis()),
                       live ? "" : ", live view skipped during a shot") &&
            sink.linef("stage,atMs,regionStart,regionEnd,address,size,state");
  char row[96];
  size_t cursor = 0;
  while (ok && heapMapRow(cursor, row, sizeof(row))) ok = sink.line(row, strlen(row));
  const bool flushed = sink.flush();
  const esp_err_t finished =
      flushed ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
  return ok && flushed ? finished : ESP_FAIL;
}

namespace {

struct OtaTransfer {
  OpenBrewByWeightNetwork *network = nullptr;
  httpd_req_t *request = nullptr;
};

bool readOtaHeader(httpd_req_t *request, const char *name, char *output,
                   size_t capacity) {
  if (request == nullptr || output == nullptr || capacity == 0) return false;
  const size_t length = httpd_req_get_hdr_value_len(request, name);
  return length > 0 && length + 1 <= capacity &&
         httpd_req_get_hdr_value_str(request, name, output, capacity) == ESP_OK;
}

bool parseOtaUint32(const char *text, uint32_t &value) {
  if (text == nullptr || text[0] == '\0') return false;
  uint64_t parsed = 0;
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') return false;
    parsed = parsed * 10U + static_cast<uint64_t>(*cursor - '0');
    if (parsed > UINT32_MAX) return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

bool validOtaTransferId(const char *value) {
  if (value == nullptr) return false;
  const size_t length = strlen(value);
  if (length < 16 || length >= OTA_TRANSFER_ID_CAPACITY) return false;
  for (size_t index = 0; index < length; ++index) {
    const char c = value[index];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

bool normalizeOtaSha256(char value[OTA_SHA256_HEX_CAPACITY]) {
  if (value == nullptr || strlen(value) != 64) return false;
  for (size_t index = 0; index < 64; ++index) {
    char &c = value[index];
    if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

bool validOtaVersion(const char *value) {
  if (value == nullptr || value[0] == '\0') return false;
  for (const char *cursor = value; *cursor != '\0'; ++cursor) {
    if (!otaVersionCharAllowed(*cursor)) return false;
  }
  return true;
}

bool parseContentRange(const char *value, uint32_t &offset, uint32_t &end,
                       uint32_t &total) {
  if (value == nullptr || strncmp(value, "bytes ", 6) != 0) return false;
  const char *cursor = value + 6;
  char first[16] = {};
  char last[16] = {};
  char size[16] = {};
  size_t firstLength = 0, lastLength = 0, sizeLength = 0;
  while (*cursor >= '0' && *cursor <= '9' && firstLength + 1 < sizeof(first))
    first[firstLength++] = *cursor++;
  if (*cursor++ != '-') return false;
  while (*cursor >= '0' && *cursor <= '9' && lastLength + 1 < sizeof(last))
    last[lastLength++] = *cursor++;
  if (*cursor++ != '/') return false;
  while (*cursor >= '0' && *cursor <= '9' && sizeLength + 1 < sizeof(size))
    size[sizeLength++] = *cursor++;
  return *cursor == '\0' && parseOtaUint32(first, offset) &&
         parseOtaUint32(last, end) && parseOtaUint32(size, total) && end >= offset;
}

const char *otaResultHttpStatus(OtaResult result) {
  switch (result) {
    case OtaResult::OK: return STATUS_OK;
    case OtaResult::UNAVAILABLE:
    case OtaResult::BUSY:
    case OtaResult::PENDING_VERIFY:
    case OtaResult::SAFETY_LOST:
    case OtaResult::NO_IDENTITY:
    case OtaResult::NOTHING_STAGED:
    case OtaResult::COMMITTED: return STATUS_CONFLICT;
    case OtaResult::SESSION_REQUIRED:
    case OtaResult::SESSION_CONFLICT:
    case OtaResult::SESSION_IDENTITY_MISMATCH:
    case OtaResult::OFFSET_MISMATCH:
    case OtaResult::SESSION_EXPIRED: return STATUS_CONFLICT;
    case OtaResult::TOO_LARGE: return STATUS_TOO_LARGE;
    case OtaResult::BAD_LENGTH:
    case OtaResult::BAD_IMAGE:
    case OtaResult::NO_TAG:
    case OtaResult::ARCH_MISMATCH:
    case OtaResult::DOWNGRADE: return STATUS_UNPROCESSABLE;
    case OtaResult::INVALID_RANGE: return STATUS_BAD_REQUEST;
    case OtaResult::HASH_MISMATCH: return STATUS_UNPROCESSABLE;
    case OtaResult::RECEIVE_FAILED: return STATUS_BAD_REQUEST;
    case OtaResult::WRITE_FAILED:
    case OtaResult::VERIFY_FAILED:
    case OtaResult::COMMIT_FAILED:
    case OtaResult::NO_MEMORY:
    case OtaResult::JOURNAL_FAILED:
    case OtaResult::INTERNAL: return STATUS_SERVER_ERROR;
  }
  return STATUS_SERVER_ERROR;
}

const char *otaResultMessage(OtaResult result) {
  switch (result) {
    case OtaResult::OK: return "The firmware image was verified.";
    case OtaResult::UNAVAILABLE:
      return "This controller has no second firmware slot, so it cannot be "
             "updated over Wi-Fi.";
    case OtaResult::BUSY: return "Another firmware update is already running.";
    case OtaResult::PENDING_VERIFY:
      return "The running firmware awaits boot confirmation. Check "
             "confirmBlockReason and confirmLastError in OTA status.";
    case OtaResult::BAD_LENGTH:
      return "The upload is too small to be a firmware image.";
    case OtaResult::TOO_LARGE:
      return "The firmware image does not fit in the update slot.";
    case OtaResult::RECEIVE_FAILED:
      return "The upload was interrupted. Resume this session from nextOffset; "
             "the accepted firmware prefix was preserved.";
    case OtaResult::BAD_IMAGE:
      return "This file is not an ESP32-S3 application image.";
    case OtaResult::NO_TAG:
      return "This image is not an Open Brew by Weight build.";
    case OtaResult::NO_IDENTITY:
      return "The running firmware has no complete hardware and machine "
             "identity, so no image can be checked against it. Reflash once "
             "over USB before using Wi-Fi updates again.";
    case OtaResult::SESSION_REQUIRED:
      return "Create an OTA session before sending image ranges.";
    case OtaResult::SESSION_CONFLICT:
      return "Another firmware transfer owns the update slot. Abort it before choosing another image.";
    case OtaResult::SESSION_IDENTITY_MISMATCH:
      return "The transfer identity does not match the declared image.";
    case OtaResult::OFFSET_MISMATCH:
      return "This range is not the next confirmed firmware range.";
    case OtaResult::INVALID_RANGE:
      return "Range headers must agree; offsets and non-final lengths must be "
             "aligned to 4096 bytes.";
    case OtaResult::HASH_MISMATCH:
      return "The received firmware does not match its declared SHA-256.";
    case OtaResult::SESSION_EXPIRED:
      return "The inactive firmware transfer expired. Create a new session.";
    case OtaResult::ARCH_MISMATCH:
      return "This image targets a different architecture, hardware profile, "
             "or machine profile.";
    case OtaResult::DOWNGRADE:
      return "This image is older than the running firmware.";
    case OtaResult::WRITE_FAILED:
      return "The update slot could not be written. The running firmware was "
             "not touched.";
    case OtaResult::VERIFY_FAILED:
      return "The uploaded image failed its checksum. Nothing was flashed.";
    case OtaResult::SAFETY_LOST:
      return "The paddle moved or a shot started during the upload, so the "
             "update was cancelled. The verified image was not kept.";
    case OtaResult::NOTHING_STAGED:
      return "Upload and verify a firmware image first.";
    case OtaResult::COMMIT_FAILED:
      return "The bootloader refused the staged image. Nothing was changed.";
    case OtaResult::NO_MEMORY:
      return "Not enough memory to receive a firmware image right now.";
    case OtaResult::JOURNAL_FAILED:
      return "The OTA recovery journal could not be saved. Check Diagnostic "
             "→ NVS; the running firmware was not replaced.";
    case OtaResult::INTERNAL:
      return "This firmware cannot verify updates. Reflash over USB.";
    case OtaResult::COMMITTED:
      return "A new firmware image is already flashed. Restart the controller "
             "to start it.";
  }
  return "The firmware update failed.";
}

void appendOtaTag(char *buffer, size_t capacity, size_t &used,
                  const char *name, const OtaImageTag &tag, bool valid,
                  const char *imageSha256 = nullptr) {
  if (used >= capacity) {
    return;
  }
  int written = 0;
  if (valid && tag.valid) {
    written = snprintf(buffer + used, capacity - used,
                       ",\"%s\":{\"arch\":\"%s\",\"hardware\":\"%s\","
                       "\"machine\":\"%s\",\"version\":\"%s\","
                       "\"packed\":%lu%s%s%s}",
                       name, tag.arch, tag.hardware, tag.machine, tag.version,
                       static_cast<unsigned long>(tag.packed),
                       imageSha256 != nullptr ? ",\"imageSha256\":\"" : "",
                       imageSha256 != nullptr ? imageSha256 : "",
                       imageSha256 != nullptr ? "\"" : "");
  } else {
    written = snprintf(buffer + used, capacity - used, ",\"%s\":null", name);
  }
  if (written > 0 && static_cast<size_t>(written) < capacity - used) {
    used += static_cast<size_t>(written);
  }
}

}  // namespace

bool OpenBrewByWeightNetwork::authorizeOtaRequest(httpd_req_t *request) {
  char password[WIFI_PASSWORD_CAPACITY] = {};
  // The header shares the sign-in failure counter and cooldown, so it cannot
  // be used to guess the password faster than the sign-in page. A relayed
  // request carries it sealed: the value relayAdmit opened is used instead.
  const bool relayed = relayPassword(request, password, sizeof(password));
  const bool passwordAuthorized =
      (relayed || httpd_req_get_hdr_value_str(request, DEVICE_PASSWORD_HEADER,
                                              password, sizeof(password)) == ESP_OK) &&
      password[0] != '\0' &&
      checkDevicePassword(request, password) == PasswordCheck::OK;
  memset(password, 0, sizeof(password));
  if (passwordAuthorized) {
    return true;
  }
  // A browser additionally needs its sign-in while the login is on.
  if (adminUnlockAllowed(request) && webAccessSessionAllowed(request)) {
    touchAdminUnlock();
    return true;
  }
  return false;
}

void OpenBrewByWeightNetwork::buildOtaJson(char *buffer, size_t capacity,
                                      const ControlGateSnapshot &control, uint32_t bootId) {
  if (buffer == nullptr || capacity == 0) {
    return;
  }
  buffer[0] = '\0';
  OpenBrewByWeightOta &ota = OpenBrewByWeightOta::instance();
  const OtaStatusSnapshot ota_ = ota.snapshot(false);
  const bool safe = controlAllowsConfiguration(control);
  int written = snprintf(
      buffer, capacity,
      "{\"otaProtocolVersion\":%u,\"available\":%s,\"state\":\"%s\",\"slotBytes\":%lu,\"receivedBytes\":%lu,\"expectedBytes\":%lu,\"lastResult\":\"%s\","
      "\"lastReceivedBytes\":%lu,\"lastExpectedBytes\":%lu,\"pendingVerify\":%s,"
      "\"confirmed\":%s,\"safe\":%s,\"lockReason\":\"%s\",\"passwordRequired\":true,"
      "\"passwordAvailable\":true,\"restartPending\":%s,\"runningIdentityValid\":%s,\"transferId\":\"%s\","
      "\"sha256\":\"%s\",\"sessionArch\":\"%s\","
      "\"sessionHardware\":\"%s\",\"sessionMachine\":\"%s\","
      "\"sessionVersion\":\"%s\",\"nextOffset\":%lu,\"chunkBytes\":%lu,"
      "\"sessionActive\":%s,\"sessionExpiresInMs\":%lu,\"lastChunkSha256\":\"%s\","
      "\"abortFailures\":%lu,\"journalFailures\":%lu",
      static_cast<unsigned int>(OTA_PROTOCOL_VERSION), ota_.available ? "true" : "false",
      OpenBrewByWeightOta::stateName(ota_.state),
      static_cast<unsigned long>(ota_.slotBytes), static_cast<unsigned long>(ota_.receivedBytes),
      static_cast<unsigned long>(ota_.expectedBytes), OpenBrewByWeightOta::resultName(ota_.lastResult),
      static_cast<unsigned long>(ota_.lastReceivedBytes), static_cast<unsigned long>(ota_.lastExpectedBytes),
      ota_.pendingVerify ? "true" : "false",
      ota_.confirmed ? "true" : "false", safe ? "true" : "false",
      configLockReason(control), otaRestartPending_.load(std::memory_order_acquire) ? "true" : "false",
      ota_.running.valid && otaArchIsUsable(ota_.running.arch) &&
              otaProfileIsUsable(ota_.running.hardware) &&
              otaProfileIsUsable(ota_.running.machine) ? "true" : "false",
      ota_.session.transferId, ota_.session.sha256, ota_.session.arch,
      ota_.session.hardware, ota_.session.machine, ota_.session.version,
      static_cast<unsigned long>(ota_.nextOffset), static_cast<unsigned long>(ota_.chunkBytes),
      ota_.sessionActive ? "true" : "false",
      static_cast<unsigned long>(ota_.sessionExpiresInMs), ota_.lastChunkSha256,
      static_cast<unsigned long>(ota_.abortFailures),
      static_cast<unsigned long>(ota_.journalFailures));
  if (written <= 0 || static_cast<size_t>(written) >= capacity) {
    snprintf(buffer, capacity, "{\"otaProtocolVersion\":%u,\"available\":false}", static_cast<unsigned int>(OTA_PROTOCOL_VERSION));
    return;
  }
  // One byte is held back so the closing brace always has somewhere to go: an
  // object left unclosed here is spliced into the admin status and would make
  // the whole Admin page unparseable, taking unrelated settings down with it.
  size_t used = static_cast<size_t>(written);
  const size_t tagCapacity = capacity - 1;
  written = snprintf(buffer + used, tagCapacity - used,
      ",\"bootId\":%lu,\"bootState\":%ld,\"confirmBlockReason\":\"%s\","
      "\"confirmAttempts\":%lu,\"confirmLastError\":%ld",
      static_cast<unsigned long>(bootId), static_cast<long>(ota_.bootState),
      ota_.confirmBlockReason, static_cast<unsigned long>(ota_.confirmAttempts),
      static_cast<long>(ota_.confirmLastError));
  if (written <= 0 || static_cast<size_t>(written) >= tagCapacity - used) {
    snprintf(buffer, capacity, "{\"otaProtocolVersion\":%u,\"available\":false}",
             static_cast<unsigned int>(OTA_PROTOCOL_VERSION));
    return;
  }
  used += static_cast<size_t>(written);
  appendOtaTag(buffer, tagCapacity, used, "running", ota_.running,
               ota_.running.valid, ota_.runningImageSha256);
  appendOtaTag(buffer, tagCapacity, used, "staged", ota_.staged,
               ota_.stagedValid, ota_.stagedImageSha256);
  if (used + 2 > capacity) {
    snprintf(buffer, capacity, "{\"otaProtocolVersion\":%u,\"available\":false}", static_cast<unsigned int>(OTA_PROTOCOL_VERSION));
    return;
  }
  buffer[used++] = '}';
  buffer[used] = '\0';
}
esp_err_t OpenBrewByWeightNetwork::sendOtaSnapshot(httpd_req_t *request,
                                              const char *httpStatus) {
  const ControlGateSnapshot control = controlGate();
  // A private PSRAM reply lets the send run without the shared lock, so the
  // OTA worker never holds httpd handlers for its network time. Without
  // PSRAM it falls back to sending from the shared buffer under the lock.
  char *const reply = static_cast<char *>(
      allocExternal(sizeof(workBuf_->otaJson), AllocationOwner::OTA));
  if (!lockWorkBuf()) {
    heapCapsFree(reply);
    return workBufBusy(request);
  }
  loadControlStatus(workBuf_->control);
  char *const json = reply != nullptr ? reply : workBuf_->otaJson;
  buildOtaJson(json, sizeof(workBuf_->otaJson), control, workBuf_->control.bootId);
  if (reply != nullptr) unlockWorkBuf();
  const esp_err_t sent = sendJson(request, httpStatus, json);
  if (reply != nullptr) heapCapsFree(reply);
  else unlockWorkBuf();
  return sent;
}

int OpenBrewByWeightNetwork::otaReadChunk(void *context, uint8_t *buffer,
                                     size_t capacity) {
  OtaTransfer *transfer = static_cast<OtaTransfer *>(context);
  if (transfer == nullptr || transfer->request == nullptr) {
    return -1;
  }
  // httpd's socket timeout is deliberately short so stuck clients release
  // sockets; a few retries absorb the stalls that Wi-Fi/BLE coexistence adds
  // without letting a genuinely dead peer hold the transfer open.
  for (uint8_t attempt = 0; attempt < OTA_RECEIVE_ATTEMPTS; ++attempt) {
    const int received = httpd_req_recv(
        transfer->request, reinterpret_cast<char *>(buffer), capacity);
    if (received > 0) {
      return received;
    }
    if (received != HTTPD_SOCK_ERR_TIMEOUT) {
      return -1;
    }
  }
  return -1;
}

bool OpenBrewByWeightNetwork::otaTransferStillSafe(void *context) {
  OtaTransfer *transfer = static_cast<OtaTransfer *>(context);
  if (transfer == nullptr || transfer->network == nullptr) {
    return false;
  }
  return !transfer->network->otaWorkerStopping_.load() &&
         controlAllowsConfiguration(transfer->network->controlGate());
}

void OpenBrewByWeightNetwork::otaTransferProgress(void *context, uint32_t received,
                                             uint32_t expected) {
  OtaTransfer *transfer = static_cast<OtaTransfer *>(context);
  if (transfer == nullptr || transfer->network == nullptr) {
    return;
  }
  transfer->network->actionLogf("ota: received %lu/%lu KiB",
                                static_cast<unsigned long>(received / 1024U),
                                static_cast<unsigned long>(expected / 1024U));
  transfer->network->touchAdminUnlock();
}
esp_err_t OpenBrewByWeightNetwork::otaStatusHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.authorizeOtaRequest(request)) {
    return sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                     "Send the device password, or unlock administration first.");
  }
  OpenBrewByWeightOta::instance().expireSession(
      millis(), controlAllowsConfiguration(self.controlGate()), false);
  return self.sendOtaSnapshot(request, STATUS_OK);
}
esp_err_t OpenBrewByWeightNetwork::otaUploadHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.authorizeOtaRequest(request)) {
    return sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                     "Send the device password, or unlock administration first.");
  }
  return sendError(request, STATUS_CONFLICT, "OTA_SESSION_REQUIRED",
                   "Create /api/v1/ota/session, then upload PATCH ranges.");
}

esp_err_t OpenBrewByWeightNetwork::otaSessionHandler(httpd_req_t *request) {
  return request->method == HTTP_GET
      ? otaSessionWork(request)
      : instance_->runOnOtaWorker(request, otaSessionWork);
}

esp_err_t OpenBrewByWeightNetwork::otaPatchHandler(httpd_req_t *request) {
  return instance_->runOnOtaWorker(request, otaPatchWork);
}

esp_err_t OpenBrewByWeightNetwork::otaFlashHandler(httpd_req_t *request) {
  return instance_->runOnOtaWorker(request, otaFlashWork);
}

esp_err_t OpenBrewByWeightNetwork::otaAbortHandler(httpd_req_t *request) {
  return instance_->runOnOtaWorker(request, otaAbortWork);
}
esp_err_t OpenBrewByWeightNetwork::otaSessionWork(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.authorizeOtaRequest(request)) {
    return sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                     "Send the device password, or unlock administration first.");
  }
  const ControlGateSnapshot control = self.controlGate();
  if (request->method == HTTP_GET) {
    // The Web UI and the OTA script poll this route; expire abandoned
    // sessions here so OTA busy never outlives its timeout.
    OpenBrewByWeightOta::instance().expireSession(
        millis(), controlAllowsConfiguration(control), false);
    return self.sendOtaSnapshot(request, STATUS_OK);
  }
  if (!controlAllowsConfiguration(control)) {
    return sendError(request, STATUS_CONFLICT, "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     "Stop the cycle and wait for Ready before updating firmware.");
  }
  if (self.otaRestartPending_.load(std::memory_order_acquire)) {
    return sendError(request, STATUS_CONFLICT, "OTA_RESTART_PENDING",
                     "A firmware image is already flashed and waiting for restart.");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON OTA session identity is required.");
  if (bodyStatus != ESP_OK) return bodyStatus;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  OtaSessionIdentity identity;
  static const char *const fields[] = {"size", "sha256", "arch", "hardware",
                                       "machine", "version", "transferId"};
  const bool parsed = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 7) &&
                      jsonUint32(root, "size", identity.size) &&
                      jsonString(root, "sha256", identity.sha256, sizeof(identity.sha256), false) &&
                      jsonString(root, "arch", identity.arch, sizeof(identity.arch), false) &&
                      jsonString(root, "hardware", identity.hardware,
                                 sizeof(identity.hardware), false) &&
                      jsonString(root, "machine", identity.machine,
                                 sizeof(identity.machine), false) &&
                      jsonString(root, "version", identity.version, sizeof(identity.version), false) &&
                      jsonString(root, "transferId", identity.transferId,
                                 sizeof(identity.transferId), false) &&
                      normalizeOtaSha256(identity.sha256) &&
                      otaArchIsUsable(identity.arch) &&
                      otaProfileIsUsable(identity.hardware) &&
                      otaProfileIsUsable(identity.machine) &&
                      validOtaVersion(identity.version) &&
                      validOtaTransferId(identity.transferId);
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_OTA_SESSION",
                     "size, sha256, arch, hardware, machine, version, and transferId must identify one image.");
  }
  OpenBrewByWeightOta &ota = OpenBrewByWeightOta::instance();
  self.beginOwnedHeapLifecycle(self.otaHeap_,
                               HeapLifecycleEvent::OTA_SESSION_BEGIN);
  const OtaResult result = ota.createSession(identity, millis());
  self.finishOwnedHeapLifecycle(
      self.otaHeap_, result == OtaResult::OK ? HeapLifecycleResult::SUCCESS
                                             : HeapLifecycleResult::FAILURE);
  if (result != OtaResult::OK) {
    return sendError(request, otaResultHttpStatus(result),
                     OpenBrewByWeightOta::resultName(result), otaResultMessage(result));
  }
  return self.sendOtaSnapshot(request, STATUS_OK);
}

esp_err_t OpenBrewByWeightNetwork::otaPatchWork(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  auto rejectRange = [request](const char *status, const char *error,
                               const char *message) {
    const esp_err_t sent = sendError(request, status, error, message);
    if (request->content_len > 0) {
      httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    }
    return sent;
  };
  if (!self.authorizeOtaRequest(request)) {
    return rejectRange(STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                       "Send the device password, or unlock administration first.");
  }
  if (!controlAllowsConfiguration(self.controlGate())) {
    return rejectRange(STATUS_CONFLICT, "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                       "Stop the cycle and wait for Ready before updating firmware.");
  }
  char transferId[OTA_TRANSFER_ID_CAPACITY] = {};
  char offsetText[16] = {};
  char totalText[16] = {};
  char contentRange[96] = {};
  uint32_t headerOffset = 0, offset = 0, declaredTotal = 0, rangeEnd = 0, rangeTotal = 0;
  if (!readOtaHeader(request, OTA_TRANSFER_HEADER, transferId, sizeof(transferId)) ||
      !readOtaHeader(request, OTA_OFFSET_HEADER, offsetText, sizeof(offsetText)) ||
      !readOtaHeader(request, OTA_LENGTH_HEADER, totalText, sizeof(totalText)) ||
      !readOtaHeader(request, "Content-Range", contentRange, sizeof(contentRange)) ||
      !parseOtaUint32(offsetText, headerOffset) || !parseOtaUint32(totalText, declaredTotal) ||
      !parseContentRange(contentRange, offset, rangeEnd, rangeTotal) ||
      headerOffset != offset || rangeTotal != declaredTotal || request->content_len <= 0 ||
      static_cast<uint32_t>(request->content_len) != rangeEnd - offset + 1U) {
    return rejectRange(STATUS_BAD_REQUEST, "OTA_INVALID_RANGE",
                       "PATCH must contain matching offset, length, and Content-Range headers.");
  }
  OpenBrewByWeightOta &ota = OpenBrewByWeightOta::instance();
  ota.expireSession(millis(), true);
  OtaStatusSnapshot snapshot = ota.snapshot();  // one ~0.5 KB copy, reused below
  if (strncmp(snapshot.session.transferId, transferId, sizeof(transferId)) != 0 ||
      snapshot.expectedBytes != declaredTotal) {
    return rejectRange(STATUS_CONFLICT, "OTA_SESSION_IDENTITY_MISMATCH",
                       "This range does not belong to the active OTA session.");
  }
  const uint32_t rangeLength = static_cast<uint32_t>(request->content_len);
  if (ota.isDuplicateRange(offset, rangeLength)) {
    httpd_resp_set_hdr(request, "Connection", "close");
    const esp_err_t sent = self.sendOtaSnapshot(request, STATUS_ALREADY_REPORTED);
    httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    return sent;
  }
  OtaTransfer transfer{&self, request};
  OtaStreamIo io;
  io.read = otaReadChunk;
  io.stillSafe = otaTransferStillSafe;
  io.progress = otaTransferProgress;
  io.context = &transfer;
  const OtaResult result = ota.writeRange(transferId, offset, rangeLength, io, millis());
  if (result != OtaResult::OK) {
    snapshot = ota.snapshot();
    self.log(DebugCategory::NETWORK, DebugCode::OTA_UPLOAD_REJECTED,
             static_cast<int32_t>(result),
             static_cast<int32_t>(snapshot.lastReceivedBytes / 1024U));
    return rejectRange(otaResultHttpStatus(result), OpenBrewByWeightOta::resultName(result),
                       otaResultMessage(result));
  }
  snapshot = ota.snapshot();
  if (snapshot.stagedValid) {
    self.log(DebugCategory::NETWORK, DebugCode::OTA_IMAGE_STAGED,
             static_cast<int32_t>(snapshot.receivedBytes / 1024U),
             static_cast<int32_t>(snapshot.staged.packed));
  }
  return self.sendOtaSnapshot(request, STATUS_OK);
}

esp_err_t OpenBrewByWeightNetwork::otaFlashWork(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.authorizeOtaRequest(request)) {
    return sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                     "Send the device password, or unlock administration first.");
  }
  OpenBrewByWeightOta &ota = OpenBrewByWeightOta::instance();
  const ControlGateSnapshot control = self.controlGate();
  if (!controlAllowsConfiguration(control)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     "Stop the cycle and wait for Ready before flashing.");
  }
  if (self.otaRestartPending_.load(std::memory_order_acquire)) {
    return sendError(request, STATUS_CONFLICT, "OTA_RESTART_PENDING",
                     "The flashed image is already waiting for the restart.");
  }
  const OtaImageTag staged = ota.stagedTag();
  self.beginOwnedHeapLifecycle(self.otaHeap_,
                               HeapLifecycleEvent::OTA_FLASH_BEGIN);
  const OtaResult result = ota.commit();
  self.finishOwnedHeapLifecycle(
      self.otaHeap_, result == OtaResult::OK ? HeapLifecycleResult::SUCCESS
                                             : HeapLifecycleResult::FAILURE);
  // A retried flash after a failed restart request asks for the restart again.
  if (result != OtaResult::OK && result != OtaResult::COMMITTED) {
    return sendError(request, otaResultHttpStatus(result),
                     OpenBrewByWeightOta::resultName(result),
                     otaResultMessage(result));
  }
  if (result == OtaResult::OK) {
    self.log(DebugCategory::NETWORK, DebugCode::OTA_FLASH_COMMITTED,
             static_cast<int32_t>(staged.packed));
  }

  // The restart runs through the ordinary maintenance lease after the shot
  // (if any) ends. The control task will not open K1 to make way for it.
  WebCommand command;
  command.type = WebCommandType::RESTART;
  command.requestId = self.allocateRequestId();
  if (!self.callbacks_.enqueueWebCommand(command)) {
    // The boot slot is already switched, so the new firmware comes up on the
    // next restart however it happens.
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "The image is flashed. Restart the controller to use it.");
  }
  self.otaRestartRequestedAtMs_.store(millis(), std::memory_order_relaxed);
  self.otaRestartPending_.store(true, std::memory_order_release);
  return self.sendOtaSnapshot(request, STATUS_ACCEPTED);
}

esp_err_t OpenBrewByWeightNetwork::otaAbortWork(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.authorizeOtaRequest(request)) {
    return sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                     "Send the device password, or unlock administration first.");
  }
  if (!controlAllowsConfiguration(self.controlGate())) {
    return sendError(request, STATUS_CONFLICT, "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     "Stop the cycle and wait for Ready before updating firmware.");
  }
  self.beginOwnedHeapLifecycle(self.otaHeap_, HeapLifecycleEvent::OTA_ABORT);
  const OtaResult result = OpenBrewByWeightOta::instance().discard();
  self.finishOwnedHeapLifecycle(self.otaHeap_, result == OtaResult::OK
      ? HeapLifecycleResult::SUCCESS : HeapLifecycleResult::FAILURE);
  if (result != OtaResult::OK) {
    return sendError(request, otaResultHttpStatus(result),
                     OpenBrewByWeightOta::resultName(result), otaResultMessage(result));
  }
  return self.sendOtaSnapshot(request, STATUS_OK);
}

void OpenBrewByWeightNetwork::serviceOtaRollback(uint32_t now) {
  // A committed image boots on the next restart however it happens, so a
  // restart the control task never granted must not leave the panel stuck
  // claiming one is on the way.
  if (otaRestartPending_.load(std::memory_order_acquire) &&
      static_cast<uint32_t>(
          now - otaRestartRequestedAtMs_.load(std::memory_order_relaxed)) >=
          OTA_RESTART_GIVE_UP_MS) {
    otaRestartPending_.store(false, std::memory_order_release);
    actionLog("ota: the restart never happened; restart manually to use the "
              "flashed image");
  }

  OpenBrewByWeightOta &ota = OpenBrewByWeightOta::instance();
  const ControlGateSnapshot control = controlGate();
  // Never wait behind an HTTP upload's OTA mutex in this watchdog task.
  ota.expireSession(now, controlAllowsConfiguration(control), false);
  const OtaPublishedState otaStatus = ota.publishedState();
  if (otaStatus.rejected || otaStatus.confirmed || otaStatus.busy) return;
  const OtaBootStatus boot = ota.serviceBoot(now,
      control.bootState == BootState::READY && startupComplete_ && server_ != nullptr,
      control.activeCycle || control.machineRunning || control.physicalActivatorOn,
      control.relayClosed,
      scaleConnectingOrUp_.load(std::memory_order_relaxed),
      OTA_CONFIRM_MIN_UPTIME_MS, OTA_CONFIRM_DEADLINE_MS);
  if (strcmp(otaBootReason_, boot.reason) != 0 || otaBootLastError_ != boot.lastError) {
    actionLogf("ota: boot=%ld reason=%s attempts=%lu error=%ld",
        static_cast<long>(boot.state), boot.reason,
        static_cast<unsigned long>(boot.attempts), static_cast<long>(boot.lastError));
    otaBootReason_ = boot.reason;
    otaBootLastError_ = boot.lastError;
  }
  if (ota.runningImageConfirmed()) {
    log(DebugCategory::NETWORK, strcmp(boot.reason, "NO_ROLLBACK") == 0
        ? DebugCode::OTA_ROLLBACK_FAILED : DebugCode::OTA_IMAGE_CONFIRMED);
    return;
  }
  if (!ota.runningImageRejected()) return;
  log(DebugCategory::NETWORK, DebugCode::OTA_ROLLBACK_ARMED,
      static_cast<int32_t>(now / 1000U));
  actionLog("ota: no Web UI after the update; rolling back on restart");
  otaRollbackRestartPending_ = true;
  restartPending_ = true;
  restartRequestedAtMs_ = now;
}

}  // namespace openbrewbyweight

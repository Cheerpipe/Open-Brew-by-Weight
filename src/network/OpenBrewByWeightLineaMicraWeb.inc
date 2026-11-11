// Maps the grace seconds accepted by the Web API to its packed option code.
bool lineaMicraGraceCodeFromSeconds(int16_t seconds, uint8_t &code) {
  for (uint8_t index = 0; index <= LINEA_MICRA_SHUTDOWN_GRACE_CODE_MAX;
       ++index) {
    if (LINEA_MICRA_SHUTDOWN_GRACE_SECONDS[index] ==
        static_cast<uint16_t>(seconds)) {
      code = index;
      return true;
    }
  }
  return false;
}

esp_err_t OpenBrewByWeightNetwork::lineaMicraHandler(httpd_req_t *request) {
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION != OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LA_MARZOCCO_CLOUD
  return sendError(request, "404 Not Found", "UNSUPPORTED_MACHINE",
                   nullptr, "UNSUPPORTED_MACHINE");
#else
  OpenBrewByWeightNetwork &self = *instance_;
  const ControlGateSnapshot gate = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, gate)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "MICRA_REQUIRES_READY");
  }
  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "MICRA_BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) return bodyStatus;

  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {
      "action", "username", "password", "serial", "applyTemperature",
      "observeState", "recognizeWakeGesture", "shutdownWithScale",
      "shutdownGraceSeconds", "powerOnWithScale", "scaleOffWithMachine",
      "boostScaleDetectionWhenOn", "reduceScaleScanningWhenOff",
      "connectionType"};
  char action[16] = {};
  char username[LINEA_MICRA_USERNAME_CAPACITY] = {};
  char password[LINEA_MICRA_PASSWORD_CAPACITY] = {};
  const bool known = root != nullptr &&
      jsonHasOnlyUniqueFields(root, fields, sizeof(fields) / sizeof(fields[0]));
  const bool actionOk = known &&
      jsonString(root, "action", action, sizeof(action), false);
  ValidationFailure parseError;
  if (!actionOk) {
    parseError = validationFailure("MICRA_ACTION_FIELDS_INVALID");
  }
  bool applyTemperature = false;
  bool observeState = false;
  bool recognizeWakeGesture = false;
  bool shutdownWithScale = false;
  bool powerOnWithScale = false;
  bool scaleOffWithMachine = false;
  bool boostScaleDetectionWhenOn = false;
  bool reduceScaleScanningWhenOff = false;
  int16_t shutdownGraceSeconds = -1;
  uint8_t shutdownGraceCode = 0;
  LineaMicraPersistedSettings candidate = {};
  struct SensitiveWiper {
    SensitiveWiper(char (&user)[LINEA_MICRA_USERNAME_CAPACITY],
                   char (&pass)[LINEA_MICRA_PASSWORD_CAPACITY],
                   LineaMicraPersistedSettings &settingsToWipe)
        : username(user), password(pass), settings(settingsToWipe) {}
    char (&username)[LINEA_MICRA_USERNAME_CAPACITY];
    char (&password)[LINEA_MICRA_PASSWORD_CAPACITY];
    LineaMicraPersistedSettings &settings;
    ~SensitiveWiper() {
      memset(username, 0, LINEA_MICRA_USERNAME_CAPACITY);
      memset(password, 0, LINEA_MICRA_PASSWORD_CAPACITY);
      wipeLineaMicraSettings(settings);
    }
  } sensitiveWiper(username, password, candidate);
  uint32_t currentRevision = 0;
  self.dataMux_.lock();
  candidate = self.settings_.lineaMicra;
  currentRevision = self.settings_.storageRevision;
  self.dataMux_.unlock();

  uint8_t connectionType = candidate.connectionType;
  if (parseError.reason == nullptr && jsonFieldPresent(root, "connectionType")) {
    char value[16] = {};
    if ((strcmp(action, "select") != 0 && strcmp(action, "save") != 0) ||
        !jsonString(root, "connectionType", value, sizeof(value), false) ||
        (strcmp(value, "websocket") != 0 && strcmp(value, "api") != 0)) {
      parseError = validationFailure("FIELD_ENUM", "connectionType",
                                     "websocket or api on select or save");
    } else {
      connectionType = static_cast<uint8_t>(strcmp(value, "api") == 0
          ? MicraConnectionType::API : MicraConnectionType::WEBSOCKET);
    }
  }

  if (parseError.reason == nullptr && strcmp(action, "connect") == 0) {
    if (!jsonString(root, "username", username, sizeof(username), false) ||
        !jsonString(root, "password", password, sizeof(password), false) ||
        jsonFieldPresent(root, "serial") ||
        jsonFieldPresent(root, "applyTemperature") ||
        jsonFieldPresent(root, "observeState") ||
        jsonFieldPresent(root, "recognizeWakeGesture") ||
        jsonFieldPresent(root, "shutdownWithScale") ||
        jsonFieldPresent(root, "shutdownGraceSeconds") ||
        jsonFieldPresent(root, "powerOnWithScale") ||
        jsonFieldPresent(root, "scaleOffWithMachine") ||
        jsonFieldPresent(root, "boostScaleDetectionWhenOn") ||
        jsonFieldPresent(root, "reduceScaleScanningWhenOff")) {
      parseError = validationFailure("MICRA_CONNECT_FIELDS_ONLY");
    }
  } else if (parseError.reason == nullptr && strcmp(action, "select") == 0) {
    char serial[LINEA_MICRA_SERIAL_CAPACITY] = {};
    if (!jsonString(root, "serial", serial, sizeof(serial), false) ||
        !jsonBoolean(root, "applyTemperature", applyTemperature) ||
        !jsonBoolean(root, "observeState", observeState) ||
        !jsonBoolean(root, "recognizeWakeGesture", recognizeWakeGesture) ||
        !jsonBoolean(root, "shutdownWithScale", shutdownWithScale) ||
        !jsonBoolean(root, "powerOnWithScale", powerOnWithScale) ||
        !jsonBoolean(root, "scaleOffWithMachine", scaleOffWithMachine) ||
        !jsonBoolean(root, "boostScaleDetectionWhenOn",
                     boostScaleDetectionWhenOn) ||
        !jsonBoolean(root, "reduceScaleScanningWhenOff",
                     reduceScaleScanningWhenOff) ||
        !jsonInt16(root, "shutdownGraceSeconds", shutdownGraceSeconds) ||
        !lineaMicraGraceCodeFromSeconds(shutdownGraceSeconds,
                                        shutdownGraceCode) ||
        jsonFieldPresent(root, "username") ||
        jsonFieldPresent(root, "password")) {
      parseError = validationFailure("MICRA_SELECT_FIELDS_REQUIRED");
    } else {
      wipeLineaMicraSettings(candidate);
      if (!selectMachineIntegrationDevice(serial, candidate)) {
        parseError = validationFailure("MICRA_SELECT_MACHINE_UNKNOWN");
      } else {
        candidate.connectionType = connectionType;
        setLineaMicraOptions(candidate, applyTemperature, observeState,
                             recognizeWakeGesture, shutdownWithScale,
                             powerOnWithScale, shutdownGraceCode,
                             scaleOffWithMachine, boostScaleDetectionWhenOn,
                             reduceScaleScanningWhenOff);
      }
    }
  } else if (parseError.reason == nullptr && strcmp(action, "save") == 0) {
    // A save is a patch: each absent option keeps its stored value.
    const uint8_t o = candidate.options, so = candidate.scaleOptions;
    applyTemperature = o & LINEA_MICRA_APPLY_TEMPERATURE;
    observeState = o & LINEA_MICRA_OBSERVE_STATE;
    recognizeWakeGesture = o & LINEA_MICRA_RECOGNIZE_WAKE;
    shutdownWithScale = o & LINEA_MICRA_SHUTDOWN_WITH_SCALE;
    powerOnWithScale = o & LINEA_MICRA_POWER_ON_WITH_SCALE;
    scaleOffWithMachine = so & LINEA_MICRA_SCALE_OFF_WITH_MACHINE;
    boostScaleDetectionWhenOn = so & LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON;
    reduceScaleScanningWhenOff = so & LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF;
    shutdownGraceSeconds = static_cast<int16_t>(lineaMicraShutdownGraceSeconds(o));
    const auto option = [&](const char *key, bool &value) {
      return !jsonFieldPresent(root, key) || jsonBoolean(root, key, value);
    };
    // Every member but `action` is an option, the transport or a rejected credential.
    if (!candidate.accountConfigured || cJSON_GetArraySize(root) < 2 ||
        !option("applyTemperature", applyTemperature) ||
        !option("observeState", observeState) ||
        !option("recognizeWakeGesture", recognizeWakeGesture) ||
        !option("shutdownWithScale", shutdownWithScale) ||
        !option("powerOnWithScale", powerOnWithScale) ||
        !option("scaleOffWithMachine", scaleOffWithMachine) ||
        !option("boostScaleDetectionWhenOn", boostScaleDetectionWhenOn) ||
        !option("reduceScaleScanningWhenOff", reduceScaleScanningWhenOff) ||
        (jsonFieldPresent(root, "shutdownGraceSeconds") &&
         !jsonInt16(root, "shutdownGraceSeconds", shutdownGraceSeconds)) ||
        !lineaMicraGraceCodeFromSeconds(shutdownGraceSeconds,
                                        shutdownGraceCode) ||
        jsonFieldPresent(root, "username") ||
        jsonFieldPresent(root, "password") ||
        jsonFieldPresent(root, "serial")) {
      parseError = validationFailure("MICRA_SAVE_FIELDS_REQUIRED");
    } else {
      candidate.connectionType = connectionType;
      setLineaMicraOptions(candidate, applyTemperature, observeState,
                           recognizeWakeGesture, shutdownWithScale,
                           powerOnWithScale, shutdownGraceCode,
                           scaleOffWithMachine, boostScaleDetectionWhenOn,
                           reduceScaleScanningWhenOff);
    }
  } else if (parseError.reason == nullptr &&
             (strcmp(action, "disconnect") == 0 ||
              strcmp(action, "refresh") == 0 ||
              strcmp(action, "acceptPolicy") == 0 ||
              strcmp(action, "relist") == 0)) {
    if (jsonFieldPresent(root, "username") ||
        jsonFieldPresent(root, "password") ||
        jsonFieldPresent(root, "serial") ||
        jsonFieldPresent(root, "applyTemperature") ||
        jsonFieldPresent(root, "observeState") ||
        jsonFieldPresent(root, "recognizeWakeGesture") ||
        jsonFieldPresent(root, "shutdownWithScale") ||
        jsonFieldPresent(root, "shutdownGraceSeconds") ||
        jsonFieldPresent(root, "powerOnWithScale") ||
        jsonFieldPresent(root, "scaleOffWithMachine") ||
        jsonFieldPresent(root, "boostScaleDetectionWhenOn") ||
        jsonFieldPresent(root, "reduceScaleScanningWhenOff")) {
      parseError = validationFailure("MICRA_READONLY_ACTIONS");
    }
  } else if (parseError.reason == nullptr) {
    parseError = validationFailure("MICRA_ACTION_UNKNOWN");
  }

  cJSON *passwordField = root == nullptr
                             ? nullptr
                             : cJSON_GetObjectItemCaseSensitive(root, "password");
  if (passwordField != nullptr && cJSON_IsString(passwordField) &&
      passwordField->valuestring != nullptr) {
    memset(passwordField->valuestring, 0, strlen(passwordField->valuestring));
  }
  if (root != nullptr) cJSON_Delete(root);
  memset(self.workBuf_->requestBody, 0, sizeof(self.workBuf_->requestBody));
  self.unlockJsonBody();
  if (parseError.reason != nullptr) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD", nullptr,
                     parseError.reason, parseError.params);
  }

  const bool relist = strcmp(action, "relist") == 0;
  const bool acceptPolicy = strcmp(action, "acceptPolicy") == 0;
  const bool refresh = acceptPolicy || strcmp(action, "refresh") == 0;
  if ((relist || refresh) && !candidate.accountConfigured) {
    return sendError(request, STATUS_CONFLICT, "MICRA_NOT_READY",
                     nullptr, "MICRA_NOT_READY");
  }
  if (relist || strcmp(action, "connect") == 0) {
    const NetworkStatusSnapshot network = self.snapshot();
    const bool staConnected = network.staState == StaState::CONNECTED &&
                              network.wifiConfigured &&
                              network.iface.staIp[0] != '\0';
    if (!staConnected || network.apActive) {
      return sendError(request, STATUS_CONFLICT, "MICRA_STA_REQUIRED",
                       nullptr, "MICRA_STA_REQUIRED");
    }
    const uint32_t requestId = self.allocateRequestId();
    if (relist ? !queueMachineIntegrationRelist(requestId)
               : !queueMachineIntegrationConnect(requestId, username, password)) {
      return sendError(request, STATUS_UNAVAILABLE, "MICRA_BUSY",
                       nullptr, "MICRA_BUSY");
    }
    return self.sendAccepted(request, requestId);
  }

  if (refresh) {
    LineaMicraRequest integrationRequest = {};
    integrationRequest.requestId = self.allocateRequestId();
    integrationRequest.configGeneration = currentRevision;
    integrationRequest.type = acceptPolicy ? LineaMicraRequestType::ACCEPT_POLICY
                                           : LineaMicraRequestType::OBSERVE_STATE;
    if (!queueMachineIntegrationRequest(integrationRequest)) {
      return sendError(request, STATUS_UNAVAILABLE, "MICRA_BUSY",
                       nullptr, "MICRA_BUSY");
    }
    return self.sendAccepted(request, integrationRequest.requestId);
  }

  if (strcmp(action, "disconnect") == 0) {
    disconnectLineaMicra(candidate);
    clearMachineIntegrationDiscovery();
  }
  if (!validLineaMicraSettings(candidate)) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_MICRA_CONFIG",
                     nullptr, "INVALID_MICRA_CONFIG");
  }
  WebCommand command;
  command.type = WebCommandType::SAVE_LINEA_MICRA;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  command.stagedConfigRequestId = command.requestId;
  self.dataMux_.lock();
  const bool available = self.stagedLineaMicraRequestId_ == 0;
  if (available) {
    self.stagedLineaMicra_ = candidate;
    self.stagedLineaMicraRequestId_ = command.requestId;
  }
  self.dataMux_.unlock();
  if (!available) {
    return sendError(request, STATUS_UNAVAILABLE, "MICRA_BUSY",
                     nullptr, "MICRA_CHANGE_PENDING");
  }
  if (!self.callbacks_.enqueueWebCommand(command)) {
    self.clearStagedLineaMicra(command.requestId);
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "MICRA_SAVE_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
#endif
}

// Maps the grace seconds accepted by the Web API to its packed option code.
bool laMarzoccoGraceCodeFromSeconds(int16_t seconds, uint8_t &code) {
  for (uint8_t index = 0; index <= LA_MARZOCCO_SHUTDOWN_GRACE_CODE_MAX;
       ++index) {
    if (LA_MARZOCCO_SHUTDOWN_GRACE_SECONDS[index] ==
        static_cast<uint16_t>(seconds)) {
      code = index;
      return true;
    }
  }
  return false;
}

esp_err_t OpenBrewByWeightNetwork::laMarzoccoHandler(httpd_req_t *request) {
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION != OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LA_MARZOCCO_CLOUD
  return sendError(request, "404 Not Found", "UNSUPPORTED_MACHINE",
                   nullptr, "UNSUPPORTED_MACHINE");
#else
  OpenBrewByWeightNetwork &self = *instance_;
  const ControlGateSnapshot gate = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, gate)) {
    // Refused before the body is read, so a power request among them is
    // logged here rather than by its own refusal line.
    static RefusalLog refusals;
    if (const uint32_t count = refusals.due())
      lifecycleLogf("La Marzocco: refused %lu request(s) while a cycle is active",
                    static_cast<unsigned long>(count));
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "LA_MARZOCCO_REQUIRES_READY");
  }
  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "LA_MARZOCCO_BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) return bodyStatus;

  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {
      "action", "username", "password", "serial", "applyTemperature",
      "observeState", "recognizeWakeGesture", "shutdownWithScale",
      "shutdownGraceSeconds", "powerOnWithScale", "scaleOffWithMachine",
      "boostScaleDetectionWhenOn", "reduceScaleScanningWhenOff",
      "connectionType", "on"};
  char action[16] = {};
  char username[LA_MARZOCCO_USERNAME_CAPACITY] = {};
  char password[LA_MARZOCCO_PASSWORD_CAPACITY] = {};
  const bool known = root != nullptr &&
      jsonHasOnlyUniqueFields(root, fields, sizeof(fields) / sizeof(fields[0]));
  // `on` belongs to `power` alone, which takes nothing else.
  const bool actionOk = known &&
      jsonString(root, "action", action, sizeof(action), false) &&
      (strcmp(action, "power") == 0 || !jsonFieldPresent(root, "on"));
  bool powerOn = false;
  ValidationFailure parseError;
  if (!actionOk) {
    parseError = validationFailure("LA_MARZOCCO_ACTION_FIELDS_INVALID");
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
  LaMarzoccoPersistedSettings candidate = {};
  struct SensitiveWiper {
    SensitiveWiper(char (&user)[LA_MARZOCCO_USERNAME_CAPACITY],
                   char (&pass)[LA_MARZOCCO_PASSWORD_CAPACITY],
                   LaMarzoccoPersistedSettings &settingsToWipe)
        : username(user), password(pass), settings(settingsToWipe) {}
    char (&username)[LA_MARZOCCO_USERNAME_CAPACITY];
    char (&password)[LA_MARZOCCO_PASSWORD_CAPACITY];
    LaMarzoccoPersistedSettings &settings;
    ~SensitiveWiper() {
      memset(username, 0, LA_MARZOCCO_USERNAME_CAPACITY);
      memset(password, 0, LA_MARZOCCO_PASSWORD_CAPACITY);
      wipeLaMarzoccoSettings(settings);
    }
  } sensitiveWiper(username, password, candidate);
  uint32_t currentRevision = 0;
  bool powerControl = false;  // Admin → Machine control
  self.dataMux_.lock();
  candidate = self.settings_.laMarzocco;
  currentRevision = self.settings_.storageRevision;
  powerControl = self.settings_.runtime.machinePowerControlEnabled;
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
          ? LaMarzoccoConnectionType::API : LaMarzoccoConnectionType::WEBSOCKET);
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
      parseError = validationFailure("LA_MARZOCCO_CONNECT_FIELDS_ONLY");
    }
  } else if (parseError.reason == nullptr && strcmp(action, "select") == 0) {
    char serial[LA_MARZOCCO_SERIAL_CAPACITY] = {};
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
        !laMarzoccoGraceCodeFromSeconds(shutdownGraceSeconds,
                                        shutdownGraceCode) ||
        jsonFieldPresent(root, "username") ||
        jsonFieldPresent(root, "password")) {
      parseError = validationFailure("LA_MARZOCCO_SELECT_FIELDS_REQUIRED");
    } else {
      wipeLaMarzoccoSettings(candidate);
      if (!selectMachineIntegrationDevice(serial, candidate)) {
        parseError = validationFailure("LA_MARZOCCO_SELECT_MACHINE_UNKNOWN");
      } else {
        candidate.connectionType = connectionType;
        setLaMarzoccoOptions(candidate, applyTemperature, observeState,
                             recognizeWakeGesture, shutdownWithScale,
                             powerOnWithScale, shutdownGraceCode,
                             scaleOffWithMachine, boostScaleDetectionWhenOn,
                             reduceScaleScanningWhenOff);
      }
    }
  } else if (parseError.reason == nullptr && strcmp(action, "save") == 0) {
    // A save is a patch: each absent option keeps its stored value.
    const uint8_t o = candidate.options, so = candidate.scaleOptions;
    applyTemperature = o & LA_MARZOCCO_APPLY_TEMPERATURE;
    observeState = o & LA_MARZOCCO_OBSERVE_STATE;
    recognizeWakeGesture = o & LA_MARZOCCO_RECOGNIZE_WAKE;
    shutdownWithScale = o & LA_MARZOCCO_SHUTDOWN_WITH_SCALE;
    powerOnWithScale = o & LA_MARZOCCO_POWER_ON_WITH_SCALE;
    scaleOffWithMachine = so & LA_MARZOCCO_SCALE_OFF_WITH_MACHINE;
    boostScaleDetectionWhenOn = so & LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON;
    reduceScaleScanningWhenOff = so & LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF;
    shutdownGraceSeconds = static_cast<int16_t>(laMarzoccoShutdownGraceSeconds(o));
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
        !laMarzoccoGraceCodeFromSeconds(shutdownGraceSeconds,
                                        shutdownGraceCode) ||
        jsonFieldPresent(root, "username") ||
        jsonFieldPresent(root, "password") ||
        jsonFieldPresent(root, "serial")) {
      parseError = validationFailure("LA_MARZOCCO_SAVE_FIELDS_REQUIRED");
    } else {
      candidate.connectionType = connectionType;
      setLaMarzoccoOptions(candidate, applyTemperature, observeState,
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
      parseError = validationFailure("LA_MARZOCCO_READONLY_ACTIONS");
    }
  } else if (parseError.reason == nullptr && strcmp(action, "power") == 0) {
    if (cJSON_GetArraySize(root) != 2 || !jsonBoolean(root, "on", powerOn)) {
      parseError = validationFailure("LA_MARZOCCO_POWER_FIELDS_REQUIRED");
    }
  } else if (parseError.reason == nullptr) {
    parseError = validationFailure("LA_MARZOCCO_ACTION_UNKNOWN");
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

  // The cloud is reachable only from station mode with the setup AP closed.
  const auto staReady = [&self] {
    const NetworkStatusSnapshot network = self.snapshot();
    return network.staState == StaState::CONNECTED && network.wifiConfigured &&
           network.iface.staIp[0] != '\0' && !network.apActive;
  };
  if (strcmp(action, "power") == 0) {
    // Manual power is gated here and, independently, in the La Marzocco
    // service: with Machine control off no client turns the machine on or off.
    // The switch is the owner's consent, so no Admin unlock is required.
    const char *const turn = powerOn ? "on" : "off";
    const auto refused = [turn](const char *reason) {
      static RefusalLog refusals;
      if (const uint32_t count = refusals.due())
        lifecycleLogf("Machine power: refused %lu manual request(s), the last to turn %s (%s)",
                      static_cast<unsigned long>(count), turn, reason);
    };
    if (!powerControl) {
      refused("Machine control is off");
      return sendError(request, STATUS_FORBIDDEN, "MACHINE_POWER_DISABLED",
                       nullptr, "MACHINE_POWER_DISABLED");
    }
    if (!candidate.accountConfigured) {
      refused("no La Marzocco account");
      return sendError(request, STATUS_CONFLICT, "LA_MARZOCCO_NOT_READY",
                       nullptr, "LA_MARZOCCO_NOT_READY");
    }
    // Never around a shot or another cycle, whatever the unsafe override says.
    if (!controlAllowsConfiguration(gate)) {
      refused("a cycle is active");
      return sendError(request, STATUS_CONFLICT, "MACHINE_POWER_REQUIRES_READY",
                       nullptr, "MACHINE_POWER_REQUIRES_READY");
    }
    // Never queued while the cloud is out of reach: it would run whenever
    // Wi-Fi came back, long after the person pressed the button.
    if (!staReady()) {
      refused("no Wi-Fi connection");
      return sendError(request, STATUS_CONFLICT, "LA_MARZOCCO_STA_REQUIRED",
                       nullptr, "LA_MARZOCCO_STA_REQUIRED");
    }
    LaMarzoccoRequest integrationRequest = {};
    integrationRequest.requestId = self.allocateRequestId();
    integrationRequest.type = powerOn ? LaMarzoccoRequestType::SET_POWER_ON
                                      : LaMarzoccoRequestType::SET_STANDBY;
    if (!queueMachineIntegrationRequest(integrationRequest)) {
      refused("busy");
      return sendError(request, STATUS_UNAVAILABLE, "LA_MARZOCCO_BUSY",
                       nullptr, "LA_MARZOCCO_BUSY");
    }
    lifecycleLogf("Machine power: manual request to turn %s accepted", turn);
    return self.sendAccepted(request, integrationRequest.requestId);
  }

  const bool relist = strcmp(action, "relist") == 0;
  const bool acceptPolicy = strcmp(action, "acceptPolicy") == 0;
  const bool refresh = acceptPolicy || strcmp(action, "refresh") == 0;
  if ((relist || refresh) && !candidate.accountConfigured) {
    return sendError(request, STATUS_CONFLICT, "LA_MARZOCCO_NOT_READY",
                     nullptr, "LA_MARZOCCO_NOT_READY");
  }
  if (relist || strcmp(action, "connect") == 0) {
    if (!staReady()) {
      return sendError(request, STATUS_CONFLICT, "LA_MARZOCCO_STA_REQUIRED",
                       nullptr, "LA_MARZOCCO_STA_REQUIRED");
    }
    const uint32_t requestId = self.allocateRequestId();
    if (relist ? !queueMachineIntegrationRelist(requestId)
               : !queueMachineIntegrationConnect(requestId, username, password)) {
      return sendError(request, STATUS_UNAVAILABLE, "LA_MARZOCCO_BUSY",
                       nullptr, "LA_MARZOCCO_BUSY");
    }
    return self.sendAccepted(request, requestId);
  }

  if (refresh) {
    LaMarzoccoRequest integrationRequest = {};
    integrationRequest.requestId = self.allocateRequestId();
    integrationRequest.configGeneration = currentRevision;
    integrationRequest.type = acceptPolicy ? LaMarzoccoRequestType::ACCEPT_POLICY
                                           : LaMarzoccoRequestType::OBSERVE_STATE;
    if (!queueMachineIntegrationRequest(integrationRequest)) {
      return sendError(request, STATUS_UNAVAILABLE, "LA_MARZOCCO_BUSY",
                       nullptr, "LA_MARZOCCO_BUSY");
    }
    return self.sendAccepted(request, integrationRequest.requestId);
  }

  if (strcmp(action, "disconnect") == 0) {
    disconnectLaMarzocco(candidate);
    clearMachineIntegrationDiscovery();
  }
  if (!validLaMarzoccoSettings(candidate)) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_LA_MARZOCCO_CONFIG",
                     nullptr, "INVALID_LA_MARZOCCO_CONFIG");
  }
  WebCommand command;
  command.type = WebCommandType::SAVE_LA_MARZOCCO;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  command.stagedConfigRequestId = command.requestId;
  self.dataMux_.lock();
  const bool available = self.stagedLaMarzoccoRequestId_ == 0;
  if (available) {
    self.stagedLaMarzocco_ = candidate;
    self.stagedLaMarzoccoRequestId_ = command.requestId;
  }
  self.dataMux_.unlock();
  if (!available) {
    return sendError(request, STATUS_UNAVAILABLE, "LA_MARZOCCO_BUSY",
                     nullptr, "LA_MARZOCCO_CHANGE_PENDING");
  }
  if (!self.callbacks_.enqueueWebCommand(command)) {
    self.clearStagedLaMarzocco(command.requestId);
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "LA_MARZOCCO_SAVE_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
#endif
}

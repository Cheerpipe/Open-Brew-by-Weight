namespace {

void integrationDeviceId(char output[18]) {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    copyCString(output, 18, "unknown");
    return;
  }
  formatWifiMac(mac, output, 18);
}

void integrationBluetoothMac(char output[18]) {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) {
    output[0] = '\0';
    return;
  }
  formatWifiMac(mac, output, 18);
}

bool validCorrelationId(const char *value) {
  const size_t length = value == nullptr ? 0 : strnlen(value, 65);
  if (length == 0 || length >= 65) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
      return false;
  }
  return true;
}


// Newest activation-history record (WebUI History card mirror).
bool buildIntegrationLastActivation(const ControlStatusSnapshot &control,
                                    char *output, size_t capacity) {
  if (control.lastActivationId == 0) {
    return snprintf(output, capacity, "null") == 4;
  }
  const int written = snprintf(
      output, capacity,
      "{\"id\":%lu,\"type\":\"%s\",\"durationS\":%.1f,"
      "\"hasWallTime\":%s,\"endedAtUnixSec\":%lu,\"endedAtLocalSec\":%lu}",
      static_cast<unsigned long>(control.lastActivationId),
      historyTypeName(static_cast<HistoryType>(control.lastActivationType)),
      static_cast<double>(control.lastActivationDurationDs) / 10.0,
      (control.lastActivationFlags & HISTORY_FLAG_WALL_TIME) != 0 ? "true"
                                                                  : "false",
      static_cast<unsigned long>(control.lastActivationEndedAtUnixSec),
      static_cast<unsigned long>(control.lastActivationEndedAtLocalSec));
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool buildIntegrationLastShot(const PersistedLastShot &shot, const char *detail,
                              bool savePending, char *output, size_t capacity) {
  const auto type = static_cast<LastShotType>(shot.shotType);
  if (!shot.valid) {
    return snprintf(output, capacity, "null") == 4;
  }
  char name[sizeof(shot.presetName) * 6] = {};
  if (!escapeJsonString(shot.presetName, name, sizeof(name))) return false;
  size_t used = 0;
  if (!jsonScratchAppend(
          output, capacity, &used,
          "{\"cycleId\":%lu,\"shotId\":%lu,\"uptimeMs\":%lu,\"durationMs\":%lu,"
          "\"targetWeightG\":%.1f,\"presetId\":%u,\"presetName\":\"%s\","
          "\"shotType\":\"%s\",\"stopDetail\":\"%s\",\"savePending\":%s",
          static_cast<unsigned long>(shot.cycleId),
          static_cast<unsigned long>(shot.shotLogId),
          static_cast<unsigned long>(shot.endedAtUptimeMs),
          static_cast<unsigned long>(shot.durationMs),
          static_cast<double>(goalWeightDgToGrams(shot.goalWeightDg)),
          static_cast<unsigned>(shot.presetId), name, lastShotTypeName(type),
          detail, savePending ? "true" : "false")) return false;
  if (shot.hasWallTime &&
      !jsonScratchAppend(
          output, capacity, &used,
          ",\"endedAtUnixSec\":%lu,\"endedAtLocalSec\":%lu",
          static_cast<unsigned long>(shot.endedAtUnixSec),
          static_cast<unsigned long>(shot.endedAtLocalSec)))
    return false;
  if (shot.rating != 0 &&
      !jsonScratchAppend(output, capacity, &used, ",\"rating\":%u",
                         static_cast<unsigned>(shot.rating)))
    return false;
  if (shot.firstDropElapsedMs != 0 &&
      !jsonScratchAppend(output, capacity, &used, ",\"firstDropMs\":%lu",
                         static_cast<unsigned long>(shot.firstDropElapsedMs)))
    return false;
  if (shot.weightValid &&
      !jsonScratchAppend(output, capacity, &used, ",\"weightG\":%.2f",
                         shot.currentWeightG)) return false;
  if (shot.averageFlowValid &&
      !jsonScratchAppend(output, capacity, &used, ",\"averageFlowGps\":%.2f",
                         shot.averageFlowGps)) return false;
  return jsonScratchAppend(output, capacity, &used, "}");
}

bool buildIntegrationQuickSettings(const QuickSettingsSnapshot &snapshot,
                                   char *output, size_t capacity) {
  const int written = snprintf(
      output, capacity,
      "{\"revision\":%lu,\"activePresetId\":%u,\"brewByWeight\":%s,"
      "\"noScaleBbwMode\":\"%s\",\"autoToManualGuardEnabled\":%s,"
      "\"slowExtractionGuardEnabled\":%s,\"fastExtractionGuardEnabled\":%s,"
      "\"avoidAccidentalTouchEnabled\":%s,\"cupProtectionEnabled\":%s}",
      static_cast<unsigned long>(snapshot.revision),
      static_cast<unsigned>(snapshot.activePresetId),
      snapshot.brewByWeight ? "true" : "false",
      noScaleBbwModeId(snapshot.noScaleBbwMode),
      snapshot.autoToManualGuardEnabled ? "true" : "false",
      snapshot.slowExtractionGuardEnabled ? "true" : "false",
      snapshot.fastExtractionGuardEnabled ? "true" : "false",
      snapshot.avoidAccidentalTouchEnabled ? "true" : "false",
      snapshot.cupProtectionEnabled ? "true" : "false");
  return written > 0 && static_cast<size_t>(written) < capacity;
}

}  // namespace

#include "OpenBrewByWeightStatsJson.inc"

esp_err_t OpenBrewByWeightNetwork::integrationHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const char *uri = request->uri;
  esp_err_t accessResult = ESP_OK;
  if (self.integrationAccessRoute(request, accessResult)) return accessResult;

  if (apiUriMatches(uri, "/api/v1/integration") &&
      request->method == HTTP_GET) {
    if (!self.lockWorkBufForStatus()) return self.workBufBusy(request);
    NetworkWorkBuf &work = *self.workBuf_;
    ControlStatusSnapshot &control = work.control;
    self.loadControlStatus(control);
    RecipeSnapshot &recipe = work.recipe;
    if (self.callbacks_.copyRecipeSnapshot != nullptr)
      self.callbacks_.copyRecipeSnapshot(&recipe);
    ShotLogRecord latestShot = {};
    const bool hasLastShot = self.callbacks_.copyHomeShot != nullptr &&
        self.callbacks_.copyHomeShot(latestShot, nullptr);
    const PersistedLastShot shot = hasLastShot
        ? shotLogProjectLastShot(latestShot, control.lastShot)
        : PersistedLastShot{};
    const bool lastShotOk = buildIntegrationLastShot(
        shot, hasLastShot ? shotLogStopDetailName(
            static_cast<ShotLogStopDetail>(latestShot.stopDetail)) : "none",
        self.callbacks_.shotLogSavePending != nullptr &&
            self.callbacks_.shotLogSavePending(),
        work.integrationLastShot, sizeof(work.integrationLastShot));
    const bool quickSettingsOk = buildIntegrationQuickSettings(
        quickSettingsSnapshot(recipe), work.integrationQuickSettings,
        sizeof(work.integrationQuickSettings));
    ShotStatsView stats = {};
    if (self.callbacks_.copyShotStats != nullptr)
      stats = self.callbacks_.copyShotStats();
    const bool statsOk = buildIntegrationStats(stats, work.integrationStats,
                                               sizeof(work.integrationStats));
    const bool lastActivationOk = buildIntegrationLastActivation(
        control, work.integrationLastActivation,
        sizeof(work.integrationLastActivation));
    const NetworkStatusSnapshot network = self.snapshot();
    char id[18] = {};
    char currentIp[16] = {};
    char mdnsHost[DEVICE_NAME_CAPACITY] = {};
    copyCString(currentIp, sizeof(currentIp), network.iface.staIp);
    copyCString(mdnsHost, sizeof(mdnsHost), network.mdnsHost);
    integrationDeviceId(id);
    char btMac[18] = {};
    integrationBluetoothMac(btMac);
    char firmware[32] = {};
    sanitizeJsonEmbed(FW_VERSION, firmware, sizeof(firmware));
    const int written = snprintf(
        self.workBuf_->statusJson, NetworkWorkBuf::kStatusJson,
        "{\"apiVersion\":2,\"minimumClientApiVersion\":2,"
        "\"deviceId\":\"%s\",\"wifiMac\":\"%s\",\"bluetoothMac\":\"%s\","
        "\"manufacturer\":\"Cheerpipe\","
        "\"model\":\"Open Brew by Weight\",\"firmwareVersion\":\"%s\","
        "\"hardwareProfile\":\"%s\",\"arch\":\"%s\","
        "\"machineName\":\"%s\",\"machineProfile\":\"%s\","
        "\"ip\":\"%s\",\"mdnsHost\":\"%s\","
        "\"capabilities\":[\"webhook_v1\",\"preset_select_v1\","
        "\"quick_settings_v1\",\"restart_v1\",\"stored_shots_v1\","
        "\"preset_rules_v1\",\"last_shot_curve_v1\"],"
        "\"shotState\":\"%s\",\"bootId\":%lu,\"activePresetId\":%u,"
        "\"presetRevision\":%lu,\"quickSettings\":%s,"
        "\"lastShot\":%s,"
        "\"lastActivation\":%s,\"stats\":%s,"
        "\"lastCommand\":{\"requestId\":%lu,\"state\":\"%s\"}}",
        id, id, btMac, firmware,
        OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID, FW_BOARD_ARCH_STRING,
        OPEN_BREW_BY_WEIGHT_MACHINE_BRAND " " OPEN_BREW_BY_WEIGHT_MACHINE_MODEL,
        OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID, currentIp, mdnsHost,
        control.activeCycle ? "brewing" : "idle",
        static_cast<unsigned long>(control.bootId),
        static_cast<unsigned>(recipe.presets.activeId),
        static_cast<unsigned long>(recipe.runtime.revision),
        work.integrationQuickSettings, work.integrationLastShot,
        work.integrationLastActivation, work.integrationStats,
        static_cast<unsigned long>(network.lastCommandRequestId),
        commandResultStateName(network.lastCommandState));
    const bool ok = lastShotOk && quickSettingsOk &&
                    statsOk && lastActivationOk && written > 0 &&
                    static_cast<size_t>(written) < NetworkWorkBuf::kStatusJson;
    const esp_err_t result = ok
        ? sendJson(request, STATUS_OK, self.workBuf_->statusJson)
        : sendError(request, STATUS_SERVER_ERROR, "RESPONSE_TOO_LARGE",
                    "The integration snapshot exceeded its fixed buffer.");
    self.unlockWorkBuf();
    return result;
  }

  if (apiUriMatches(uri, "/api/v1/integration/presets") &&
      request->method == HTTP_GET) {
    if (!self.lockWorkBuf()) return self.workBufBusy(request);
    RecipeSnapshot &recipe = self.workBuf_->recipe;
    if (self.callbacks_.copyRecipeSnapshot != nullptr)
      self.callbacks_.copyRecipeSnapshot(&recipe);
    // The Web UI's preset render (rules, first drop, recent arrivals), led by
    // this route's version and the revision the snapshot reports.
    refreshRenderedPresets(self.callbacks_, recipe.presets);
    char lead[48] = {};
    snprintf(lead, sizeof(lead), "\"apiVersion\":1,\"revision\":%lu,",
             static_cast<unsigned long>(recipe.runtime.revision));
    const bool ok = buildSlimPresetsJson(lead);
    self.workBuf_->presetsJsonHome = false;
    const esp_err_t result = ok
        ? sendJson(request, STATUS_OK, self.workBuf_->presetsJson)
        : sendError(request, STATUS_SERVER_ERROR, "RESPONSE_TOO_LARGE",
                    "The preset snapshot exceeded its fixed buffer.");
    self.unlockWorkBuf();
    return result;
  }

  // The newest eligible shot as one Stats row (columns named in `cols`) with
  // its curve, so a client draws it exactly like the Web UI Stats card.
  if (apiUriMatches(uri, "/api/v1/integration/shots/last") &&
      request->method == HTTP_GET) {
    static_assert(NetworkWorkBuf::kJsonItem + sizeof(kStatsRowColumns) + 64 <=
                      NetworkWorkBuf::kStatusJson,
                  "a full Stats row must fit the integration response");
    if (!self.lockWorkBuf()) return self.workBufBusy(request);
    NetworkWorkBuf &work = *self.workBuf_;
    ShotLogRecord latest = {};
    ShotCurveRecord &curve = work.statsCache.fetchCurve;
    const bool hasShot = self.callbacks_.copyHomeShot != nullptr &&
        self.callbacks_.copyHomeShot(latest, nullptr);
    // The newest good shot's curve is cached; busy right after boot reads as none.
    const bool hasCurve = hasShot && self.callbacks_.copyShotCurveById != nullptr &&
        self.callbacks_.copyShotCurveById(latest.id, curve) == ShotCurveRead::FOUND;
    const bool rowOk =
        !hasShot || self.formatShotStatsRow(work, latest, hasCurve ? &curve : nullptr) != 0;
    const int written = snprintf(
        work.statusJson, NetworkWorkBuf::kStatusJson,
        "{\"apiVersion\":1,\"cols\":\"%s\",\"shot\":%s}", kStatsRowColumns,
        hasShot ? work.jsonItem : "null");
    const bool ok = rowOk && written > 0 &&
                    static_cast<size_t>(written) < NetworkWorkBuf::kStatusJson;
    const esp_err_t result = ok
        ? sendJson(request, STATUS_OK, work.statusJson)
        : sendError(request, STATUS_SERVER_ERROR, "RESPONSE_TOO_LARGE",
                    "The last shot exceeded its fixed buffer.");
    self.unlockWorkBuf();
    return result;
  }

  if (apiUriMatches(uri, "/api/v1/integration/request") &&
      request->method == HTTP_GET) {
    const NetworkStatusSnapshot network = self.snapshot();
    char response[128] = {};
    snprintf(response, sizeof(response),
             "{\"apiVersion\":1,\"requestId\":%lu,\"state\":\"%s\"}",
             static_cast<unsigned long>(network.lastCommandRequestId),
             commandResultStateName(network.lastCommandState));
    return sendJson(request, STATUS_OK, response);
  }

  if (apiUriMatches(uri, "/api/v1/integration/webhook") &&
      request->method == HTTP_GET) {
    const WebhookConfig config = self.webhooks_.config();
    const WebhookStatus delivery = self.webhooks_.status();
    char url[WEBHOOK_URL_CAPACITY * 2] = {};
    sanitizeJsonEmbed(config.url, url, sizeof(url));
    char ago[12] = "null";
    if (delivery.lastAttemptAtMs != 0)
      snprintf(ago, sizeof(ago), "%lu",
               static_cast<unsigned long>(millis() - delivery.lastAttemptAtMs));
    if (!self.lockWorkBuf()) return self.workBufBusy(request);
    const int written = snprintf(
        self.workBuf_->statusJson, NetworkWorkBuf::kStatusJson,
        "{\"apiVersion\":1,\"enabled\":%s,\"url\":\"%s\","
        "\"brewState\":%s,\"firstDrop\":%s,\"end\":%s,"
        "\"presetChanges\":%s,\"deferDuringShot\":%s,"
        "\"delivery\":{\"workerReady\":%s,\"sending\":%s,\"held\":%s,"
        "\"queued\":%u,\"lastEvent\":\"%s\",\"lastAttemptAgoMs\":%s,"
        "\"lastSuccess\":%s,\"lastHttpStatus\":%u,\"lastError\":%ld,"
        "\"lastPhase\":\"%s\",\"lastCancellation\":\"%s\","
        "\"sent\":%lu,\"dropped\":%lu,\"staleConfigDropped\":%lu,"
        "\"workerStartFailures\":%lu}}",
        config.enabled ? "true" : "false", url,
        config.brewState ? "true" : "false",
        config.firstDrop ? "true" : "false",
        config.end ? "true" : "false",
        config.presetChanges ? "true" : "false",
        config.deferDuringShot ? "true" : "false",
        delivery.workerReady ? "true" : "false",
        delivery.sending ? "true" : "false", delivery.held ? "true" : "false",
        static_cast<unsigned>(delivery.queued), delivery.lastEvent,
        ago,
        delivery.lastSuccess ? "true" : "false",
        static_cast<unsigned>(delivery.lastHttpStatus),
        static_cast<long>(delivery.lastError),
        webhookRequestPhaseName(delivery.lastPhase),
        webhookCancellationReasonName(delivery.lastCancellation),
        static_cast<unsigned long>(delivery.sent),
        static_cast<unsigned long>(delivery.dropped),
        static_cast<unsigned long>(delivery.staleConfigDropped),
        static_cast<unsigned long>(delivery.workerStartFailures));
    const esp_err_t result =
        written > 0 && static_cast<size_t>(written) < NetworkWorkBuf::kStatusJson
            ? sendJson(request, STATUS_OK, self.workBuf_->statusJson)
            : sendError(request, STATUS_SERVER_ERROR, "RESPONSE_TOO_LARGE",
                        "The webhook status exceeded its fixed buffer.");
    self.unlockWorkBuf();
    return result;
  }

  if (apiUriMatches(uri, "/api/v1/integration/webhook/test") &&
      request->method == HTTP_POST) {
    const esp_err_t bodyStatus = self.lockJsonBody(
        request, "A bounded JSON request is required.");
    if (bodyStatus != ESP_OK) return bodyStatus;
    cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
    char correlation[65] = {};
    static const char *const fields[] = {"correlationId"};
    const bool parsed = root != nullptr &&
        jsonHasOnlyUniqueFields(root, fields, 1) &&
        jsonString(root, "correlationId", correlation, sizeof(correlation), false) &&
        validCorrelationId(correlation);
    if (root != nullptr) cJSON_Delete(root);
    uint32_t bootId = 0;
    if (parsed) {
      self.loadControlStatus(self.workBuf_->control);
      bootId = self.workBuf_->control.bootId;
    }
    self.unlockJsonBody();
    if (!parsed)
      return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                       "correlationId must be 1-64 URL-safe characters.");
    WebhookEvent event;
    event.type = WebhookEventType::TEST;
    event.uptimeMs = millis();
    event.bootId = bootId;
    copyCString(event.correlationId, sizeof(event.correlationId), correlation);
    if (!self.webhooks_.enqueue(event))
      return sendError(request, STATUS_UNAVAILABLE, "WEBHOOK_UNAVAILABLE",
                       "The test event could not be queued.");
    return sendJson(request, STATUS_ACCEPTED,
                    "{\"apiVersion\":1,\"queued\":true}");
  }

  if (apiUriMatches(uri, "/api/v1/integration/webhook") &&
      request->method == HTTP_PUT) {
    const ControlGateSnapshot gate = self.controlGate();
    if (!controlAllowsConfiguration(gate))
      return sendError(request, STATUS_CONFLICT,
                       "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                       "Webhook settings are locked while a cycle is active.");
    const esp_err_t bodyStatus = self.lockJsonBody(
        request, "A bounded webhook JSON request is required.");
    if (bodyStatus != ESP_OK) return bodyStatus;
    cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
    WebhookConfig candidate = {};
    static const char *const fields[] = {"enabled", "url", "brewState",
        "firstDrop", "end", "presetChanges", "deferDuringShot"};
    const bool parsed = root != nullptr &&
        jsonHasOnlyUniqueFields(root, fields, 7) &&
        jsonBoolean(root, "enabled", candidate.enabled) &&
        jsonString(root, "url", candidate.url, sizeof(candidate.url), true) &&
        jsonBoolean(root, "brewState", candidate.brewState) &&
        jsonBoolean(root, "firstDrop", candidate.firstDrop) &&
        jsonBoolean(root, "end", candidate.end) &&
        jsonBoolean(root, "presetChanges", candidate.presetChanges) &&
        jsonBoolean(root, "deferDuringShot", candidate.deferDuringShot) &&
        validWebhookConfig(candidate);
    if (root != nullptr) cJSON_Delete(root);
    self.unlockJsonBody();
    if (!parsed)
      return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST",
                       "A valid bounded http:// webhook configuration is required.");
    WebCommand command;
    command.type = WebCommandType::SAVE_WEBHOOK;
    command.requestId = self.allocateRequestId();
    command.stagedConfigRequestId = command.requestId;
    self.dataMux_.lock();
    const bool available = self.stagedWebhookRequestId_ == 0;
    if (available) {
      self.stagedWebhook_ = candidate;
      self.stagedWebhookRequestId_ = command.requestId;
    }
    self.dataMux_.unlock();
    if (!available)
      return sendError(request, STATUS_UNAVAILABLE, "REQUEST_BUSY",
                       "Another webhook update is pending.");
    if (!self.callbacks_.enqueueWebCommand(command)) {
      self.clearStagedWebhook(command.requestId);
      return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                       "Control is busy; webhook settings were not saved.");
    }
    return self.sendAccepted(request, command.requestId,
                             "\"apiVersion\":1");
  }

  if (apiUriMatches(uri, "/api/v1/integration/quick-settings") &&
      request->method == HTTP_PUT) {
    const ControlGateSnapshot gate = self.controlGate();
    if (!controlAllowsConfiguration(gate))
      return sendError(request, STATUS_CONFLICT,
                       "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                       "Quick Settings are locked while a cycle is active.");
    const esp_err_t bodyStatus = self.lockJsonBody(
        request, "A bounded Quick Settings JSON request is required.");
    if (bodyStatus != ESP_OK) return bodyStatus;
    cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
    IntegrationQuickSettingRequest parsedRequest;
    const bool parsed = parseIntegrationQuickSettingRequest(root, parsedRequest);
    if (root != nullptr) cJSON_Delete(root);
    self.unlockJsonBody();
    if (!parsed)
      return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                       "Provide baseRevision and one or more valid Quick Settings fields, "
                       "or revertAfter or revertNow alone.");
    RecipeSnapshot recipe;
    if (self.callbacks_.copyRecipeSnapshot != nullptr)
      self.callbacks_.copyRecipeSnapshot(&recipe);
    if (parsedRequest.baseRevision != recipe.runtime.revision)
      return sendError(request, STATUS_CONFLICT, "STALE_REVISION",
                       "Quick Settings changed; refresh and retry.");
    WebCommand command;
    command.type = WebCommandType::QUICK_SETTING;
    command.requestId = self.allocateRequestId();
    // A settings patch carries its field mask and, in the config copy, the
    // requested values; a revert action carries its policy.
    command.config = recipe.runtime;
    for (uint8_t index = 0; index < QUICK_SETTING_FIELD_COUNT; ++index)
      if (parsedRequest.mask & (1U << index))
        setQuickSettingValue(command.config, static_cast<QuickSettingField>(index),
                             parsedRequest.value[index]);
    command.presetAction = parsedRequest.revertAction != 0
        ? parsedRequest.revertAction : parsedRequest.mask;
    command.presetId = parsedRequest.revertPolicy;
    self.dataMux_.lock();
    const bool available = self.stagedPresetRequestId_ == 0;
    if (available) self.stagedPresetRequestId_ = command.requestId;
    self.dataMux_.unlock();
    if (!available)
      return sendError(request, STATUS_CONFLICT, "REQUEST_BUSY",
                       "Another settings update is pending.");
    if (!self.callbacks_.enqueueWebCommand(command)) {
      self.dataMux_.lock();
      rollbackIntegrationStagedRequest(self.stagedPresetRequestId_,
                                       command.requestId);
      self.dataMux_.unlock();
      return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                       "Control is busy; Quick Settings were not changed.");
    }
    return self.sendAccepted(request, command.requestId,
                             "\"apiVersion\":1");
  }

  if (apiUriMatches(uri, "/api/v1/integration/restart") &&
      request->method == HTTP_POST) {
    const esp_err_t bodyStatus = self.lockJsonBody(
        request, "An empty bounded JSON object is required.");
    if (bodyStatus != ESP_OK) return bodyStatus;
    cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
    const bool parsed = parseIntegrationRestartRequest(root);
    if (root != nullptr) cJSON_Delete(root);
    self.unlockJsonBody();
    if (!parsed)
      return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                       "Restart requires an empty JSON object.");
    return self.enqueueAcceptedCommand(request, WebCommandType::RESTART, false,
                                  "Control is busy; restart was not queued.",
                                  "\"apiVersion\":1");
  }

  if (apiUriMatches(uri, "/api/v1/integration/presets/active") &&
      request->method == HTTP_PUT) {
    const ControlGateSnapshot gate = self.controlGate();
    if (!controlAllowsConfiguration(gate))
      return sendError(request, STATUS_CONFLICT,
                       "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                       "Presets are locked while a cycle is active.");
    const esp_err_t bodyStatus = self.lockJsonBody(
        request, "A bounded JSON request is required.");
    if (bodyStatus != ESP_OK) return bodyStatus;
    cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
    uint8_t id = 0;
    static const char *const fields[] = {"id"};
    const bool parsed = root != nullptr &&
        jsonHasOnlyUniqueFields(root, fields, 1) && jsonUint8(root, "id", id) &&
        id != 0;
    if (root != nullptr) cJSON_Delete(root);
    self.unlockJsonBody();
    if (!parsed)
      return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                       "id must be a non-zero preset ID.");
    RecipeSnapshot recipe;
    if (self.callbacks_.copyRecipeSnapshot != nullptr)
      self.callbacks_.copyRecipeSnapshot(&recipe);
    if (findShotPreset(recipe.presets, id) == nullptr)
      return sendError(request, STATUS_NOT_FOUND, "PRESET_NOT_FOUND",
                       "The requested preset does not exist.");
    WebCommand command;
    command.type = WebCommandType::PRESET_OP;
    command.requestId = self.allocateRequestId();
    command.presetAction = static_cast<uint8_t>(PresetAction::APPLY);
    command.presetId = id;
    command.config = recipe.runtime;
    self.dataMux_.lock();
    const bool available = self.stagedPresetRequestId_ == 0;
    if (available) self.stagedPresetRequestId_ = command.requestId;
    self.dataMux_.unlock();
    if (!available)
      return sendError(request, STATUS_CONFLICT, "REQUEST_BUSY",
                       "Another preset update is pending.");
    if (!self.callbacks_.enqueueWebCommand(command))
      {
        self.dataMux_.lock();
        if (self.stagedPresetRequestId_ == command.requestId)
          self.stagedPresetRequestId_ = 0;
        self.dataMux_.unlock();
        return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                         "Control is busy; the preset was not changed.");
      }
    return self.sendAccepted(request, command.requestId,
                             "\"apiVersion\":1");
  }

  return sendError(request, STATUS_NOT_FOUND, "NOT_FOUND",
                   "Unknown integration API route.");
}

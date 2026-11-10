// Companion pairing and Admin routes (docs/COMPANION_LINK.md#pairing), split
// out of OpenBrewByWeightCompanion.inc at its size cap, behaviour unchanged:
// the companion's pair request and polling, and Admin → Companion's actions.

namespace openbrewbyweight {

// --- Pairing routes ------------------------------------------------------------

esp_err_t OpenBrewByWeightNetwork::companionPairHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  char address[COMPANION_ADDRESS_TEXT];
  if (!companionPeer(httpd_req_to_sockfd(request), address))
    return sendError(request, STATUS_FORBIDDEN, "COMPANION_PEER",
                     nullptr, "PAIRING_LOCAL_NETWORK_ONLY");
  const uint32_t now = millis();
  char json[192] = {};
  size_t used = 0;
  const auto appendControllerKey = [&](CompanionRequest &pending) {
    // The key is public: the companion needs it for the code and the secret.
    if (!pending.keyReady) return;
    pending.keyIssued = true;
    memcpy(json + used, ",\"controllerKey\":\"", 18);
    used += 18;
    (void)appendBase64(json, sizeof(json), used, pending.controllerKey, COMPANION_KEY_BYTES);
    json[used++] = '"';
  };
  if (request->method == HTTP_GET) {
    char query[64] = {}, id[COMPANION_ID_TEXT] = {};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK || !companionIdValid(id))
      return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", nullptr, "COMPANION_ID_INVALID");
    {
      TaskLockGuard lock(self.dataMux_);
      const char *state = companionPairPoll(g_companion.pairing, id, now);
      CompanionRequest &pending = g_companion.pairing.request;
      used = static_cast<size_t>(snprintf(json, sizeof(json), "{\"state\":\"%s\"", state));
      if (!strcmp(state, "pending") || !strcmp(state, "approved")) appendControllerKey(pending);
      if (!strcmp(state, "pending"))
        used += static_cast<size_t>(snprintf(json + used, sizeof(json) - used, ",\"expiresInS\":%lu",
            static_cast<unsigned long>((pending.untilMs - now + 999U) / 1000U)));
    }
    snprintf(json + used, sizeof(json) - used, "}");
    return sendJson(request, STATUS_OK, json);
  }
  if (!requireJsonContentType(request))
    return sendError(request, STATUS_UNSUPPORTED_MEDIA, "UNSUPPORTED_MEDIA_TYPE",
                     nullptr, "UNSUPPORTED_MEDIA_TYPE");
  const esp_err_t body = self.lockJsonBody(request, nullptr, "OTA_JSON_REQUEST_REQUIRED");
  if (body != ESP_OK) return body;
  // The first request commits to the companion's key; the same request with
  // `publicKey` instead reveals it once controllerKey was issued.
  static const char *const fields[] = {"id", "name", "version", "protocol", "commitment", "publicKey"};
  char id[COMPANION_ID_TEXT] = {}, name[COMPANION_NAME_TEXT] = {},
       version[COMPANION_VERSION_TEXT] = {}, keyText[96] = {};
  uint8_t key[COMPANION_KEY_BYTES] = {}, commitment[COMPANION_COMMITMENT_BYTES] = {};
  uint32_t protocol = 0;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  const bool reveal = root != nullptr && jsonFieldPresent(root, "publicKey");
  bool valid = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 6) &&
               jsonFieldPresent(root, "commitment") != reveal &&
               jsonString(root, "id", id, sizeof(id), false) && companionIdValid(id) &&
               jsonString(root, "name", name, sizeof(name), false) &&
               companionTextValid(name, 1, COMPANION_NAME_TEXT - 1, false) &&
               jsonString(root, "version", version, sizeof(version), true) &&
               companionTextValid(version, 0, COMPANION_VERSION_TEXT - 1, true) &&
               jsonUint32(root, "protocol", protocol) && protocol == COMPANION_PROTOCOL &&
               (reveal ? jsonString(root, "publicKey", keyText, sizeof(keyText), false) &&
                             companionBase64Decode(keyText, key, COMPANION_KEY_BYTES)
                       : jsonString(root, "commitment", keyText, sizeof(keyText), false) &&
                             companionBase64Decode(keyText, commitment, sizeof(commitment)));
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!valid)
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                     nullptr, "PAIRING_FIELDS_INVALID");
  const bool onCurve = reveal && key[0] == 0x04 && companionPublicKeyValid(key);
  CompanionPairResult result = CompanionPairResult::PENDING;
  bool accepted = true, denied = false;
  char label[COMPANION_NAME_TEXT] = {};
  {
    TaskLockGuard lock(self.dataMux_);
    CompanionRequest &pending = g_companion.pairing.request;
    if (reveal) {
      const bool open = pending.state == CompanionPairState::PENDING;
      accepted = companionPairReveal(g_companion.pairing, id, key, onCurve, now);
      denied = open && pending.state == CompanionPairState::DENIED;
    } else {
      result = companionPairRequest(g_companion.pairing, g_companion.stored.paired != 0, id, name,
                                    version, address, commitment, now);
    }
    memcpy(label, pending.name, sizeof(label));
    used = static_cast<size_t>(snprintf(json, sizeof(json), "{\"state\":\"pending\""));
    appendControllerKey(pending);
    snprintf(json + used, sizeof(json) - used, ",\"expiresInS\":%lu}",
             static_cast<unsigned long>((pending.untilMs - now + 999U) / 1000U));
  }
  if (denied)
    self.lifecycleLogf("Companion pairing request from '%s' denied: its key did not match its request",
                       label);
  if (!accepted)
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                     nullptr, "PAIRING_KEY_MISMATCH");
  switch (result) {
    case CompanionPairResult::IGNORED:
      return sendError(request, STATUS_FORBIDDEN, "COMPANION_IGNORED",
                       nullptr, "COMPANION_IGNORED");
    case CompanionPairResult::ALREADY_PAIRED:
      return sendError(request, STATUS_CONFLICT, "ALREADY_PAIRED",
                       nullptr, "ALREADY_PAIRED");
    case CompanionPairResult::BUSY:
      return sendError(request, STATUS_CONFLICT, "PAIRING_BUSY",
                       nullptr, "PAIRING_BUSY");
    case CompanionPairResult::RATE_LIMITED:
      return sendError(request, STATUS_TOO_MANY, "PAIRING_RATE_LIMITED",
                       nullptr, "PAIRING_RATE_LIMITED");
    case CompanionPairResult::NEW:
      // The network task makes this request's key pair.
      self.companionAwake_.store(true, std::memory_order_release);
      self.lifecycleLogf("Companion pairing request from '%s' at %s", label, address);
      if (self.taskHandle_ != nullptr) xTaskNotifyGive(self.taskHandle_);
      break;
    case CompanionPairResult::PENDING:
      break;
  }
  return sendJson(request, STATUS_ACCEPTED, json);
}

esp_err_t OpenBrewByWeightNetwork::companionAdminHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  const esp_err_t body = self.lockJsonBody(request, nullptr, "OTA_JSON_REQUEST_REQUIRED");
  if (body != ESP_OK) return body;
  static const char *const fields[] = {"action", "code", "lm", "ui", "log", "archive", "bc"};
  char action[12] = {}, code[8] = {};
  bool lm = true, ui = true, log = true, archive = true, bc = true;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  bool valid = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 7) &&
               jsonString(root, "action", action, sizeof(action), false);
  if (valid && jsonFieldPresent(root, "code")) valid = jsonString(root, "code", code, sizeof(code), false);
  const bool hasLm = valid && jsonFieldPresent(root, "lm");
  if (hasLm) valid = jsonBoolean(root, "lm", lm);
  const bool hasUi = valid && jsonFieldPresent(root, "ui");
  if (hasUi) valid = jsonBoolean(root, "ui", ui);
  const bool hasLog = valid && jsonFieldPresent(root, "log");
  if (hasLog) valid = jsonBoolean(root, "log", log);
  const bool hasArchive = valid && jsonFieldPresent(root, "archive");
  if (hasArchive) valid = jsonBoolean(root, "archive", archive);
  const bool hasBc = valid && jsonFieldPresent(root, "bc");
  if (hasBc) valid = jsonBoolean(root, "bc", bc);
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!valid)
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", nullptr, "AUTH_FIELDS_INVALID");
  const uint32_t now = millis();
  char name[COMPANION_NAME_TEXT] = {};
  if (!strcmp(action, "approve")) {
    bool digits = strlen(code) == 6;
    for (size_t index = 0; digits && index < 6; ++index) digits = code[index] >= '0' && code[index] <= '9';
    if (!digits)
      return sendError(request, STATUS_UNPROCESSABLE, "COMPANION_CODE",
                       nullptr, "COMPANION_CODE");
    // The expected code needs both keys; SHA-256 only, small enough for httpd.
    uint8_t companionKey[COMPANION_KEY_BYTES], controllerKey[COMPANION_KEY_BYTES];
    uint32_t serial = 0, expected = UINT32_MAX;
    bool ready = false;
    {
      TaskLockGuard lock(self.dataMux_);
      const CompanionRequest &pending = g_companion.pairing.request;
      ready = companionCodeReady(pending);
      serial = pending.serial;
      memcpy(companionKey, pending.companionKey, sizeof(companionKey));
      memcpy(controllerKey, pending.controllerKey, sizeof(controllerKey));
    }
    if (ready && !companionSas(companionKey, controllerKey, expected)) expected = UINT32_MAX;
    CompanionApproveResult result = CompanionApproveResult::NOT_PENDING;
    {
      TaskLockGuard lock(self.dataMux_);
      const CompanionRequest &pending = g_companion.pairing.request;
      // A code that became checkable after the first look is not counted wrong.
      if (pending.serial == serial)
        result = expected != UINT32_MAX
            ? companionPairApprove(g_companion.pairing, static_cast<uint32_t>(atol(code)),
                                   expected, lm, log, archive, bc, now)
            : pending.state == CompanionPairState::PENDING && !pending.approving
                ? CompanionApproveResult::NOT_READY
                : CompanionApproveResult::NOT_PENDING;
      memcpy(name, pending.name, sizeof(name));
    }
    switch (result) {
      case CompanionApproveResult::OK:
        // The network task derives the pairing secret and saves it.
        if (self.taskHandle_ != nullptr) xTaskNotifyGive(self.taskHandle_);
        return sendJson(request, STATUS_OK, "{\"ok\":true}");
      case CompanionApproveResult::WRONG_CODE:
        return sendError(request, STATUS_UNPROCESSABLE, "COMPANION_CODE",
                         nullptr, "COMPANION_CODE_MISMATCH");
      case CompanionApproveResult::DENIED:
        self.lifecycleLogf("Companion pairing request from '%s' denied after three wrong codes", name);
        return sendError(request, STATUS_UNPROCESSABLE, "COMPANION_CODE",
                         nullptr, "COMPANION_CODE_DENIED");
      case CompanionApproveResult::NOT_READY:
        return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_READY",
                         nullptr, "PAIRING_NOT_READY");
      default:
        return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_PENDING",
                         nullptr, "PAIRING_NOT_PENDING");
    }
  }
  if (!strcmp(action, "ignore")) {
    bool ignored;
    {
      TaskLockGuard lock(self.dataMux_);
      ignored = companionPairIgnore(g_companion.pairing, now);
      g_companion.ignoredDirty |= ignored;
      memcpy(name, g_companion.pairing.request.name, sizeof(name));
    }
    if (!ignored)
      return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_PENDING",
                       nullptr, "PAIRING_NOT_PENDING");
    self.lifecycleLogf("Companion pairing request from '%s' ignored", name);
  } else if (!strcmp(action, "unignore")) {
    {
      TaskLockGuard lock(self.dataMux_);
      companionUnignore(g_companion.pairing);
      g_companion.ignoredDirty = true;
    }
    // An unpaired controller sleeps the companion task that saves the list.
    self.companionAwake_.store(true, std::memory_order_release);
    self.lifecycleLog("Ignored companions may ask to pair again");
  } else if (!strcmp(action, "forget")) {
    int fds[2] = {-1, -1};
    bool forgotten;
    {
      TaskLockGuard lock(self.dataMux_);
      memcpy(name, g_companion.stored.name, sizeof(name));
      forgotten = companionForget(g_companion.stored);
      g_companion.dirty |= forgotten;
      if (forgotten) {
        g_companion.peerProtocol = 0;
        companionRelayDrop(g_companion.relay);
      }
      for (size_t index = 0; forgotten && index < 2; ++index) {
        CompanionLinkSlot &slot = g_companion.links[index];
        if (slot.fd < 0 || slot.closing) continue;
        slot.closing = true;
        slot.closeReason = "the companion was forgotten";
        fds[index] = slot.fd;
      }
    }
    if (!forgotten)
      return sendError(request, STATUS_CONFLICT, "COMPANION_NOT_PAIRED",
                       nullptr, "COMPANION_NOT_PAIRED");
    for (const int fd : fds)
      if (fd >= 0) (void)httpd_sess_trigger_close(request->handle, fd);
    self.closeRelayedUiStreams();
    self.lifecycleLogf("Companion '%s' forgotten", name);
  } else if (!strcmp(action, "switches") && (hasLm || hasUi || hasLog || hasArchive || hasBc) &&
             (kCompanionLm || !hasLm)) {
    bool paired, lmChanged = false, uiChanged = false, logChanged = false, archiveChanged = false,
         bcChanged = false, linkUp = false;
    {
      TaskLockGuard lock(self.dataMux_);
      CompanionStored &stored = g_companion.stored;
      paired = stored.paired != 0;
      if (paired) {
        lmChanged = hasLm && (stored.lmSwitch != 0) != lm;
        uiChanged = hasUi && (stored.uiOff == 0) != ui;
        logChanged = hasLog && (stored.logSwitch != 0) != log;
        if (hasLm) stored.lmSwitch = lm ? 1 : 0;
        if (hasUi) stored.uiOff = ui ? 0 : 1;
        if (hasLog) stored.logSwitch = log ? 1 : 0;
        archiveChanged = hasArchive && (stored.archiveSwitch != 0) != archive;
        if (hasArchive) stored.archiveSwitch = archive ? 1 : 0;
        bcChanged = hasBc && (stored.bcSwitch != 0) != bc;
        if (hasBc) stored.bcSwitch = bc ? 1 : 0;
        g_companion.dirty |= lmChanged || uiChanged || logChanged || archiveChanged || bcChanged;
        linkUp = g_companion.active != 0;
        // Its next relayed request is stale, and the new handshake carries ui and link.
        if (lmChanged || uiChanged || logChanged || archiveChanged)
          companionRelayDrop(g_companion.relay);
      }
    }
    if (!paired)
      return sendError(request, STATUS_CONFLICT, "COMPANION_NOT_PAIRED",
                       nullptr, "COMPANION_NOT_PAIRED");
    if (lmChanged) {
      machineIntegrationCompanionLink(linkUp, lm);
      self.lifecycleLog(lm ? "Companion: La Marzocco through the companion turned on"
                           : "Companion: La Marzocco through the companion turned off");
    }
    if (uiChanged) {
      // An open link learns it from the next cfg frame.
      companionOutboxChanged();
      if (!ui) self.closeRelayedUiStreams();
      self.lifecycleLog(ui ? "Companion: Web UI through the companion turned on"
                           : "Companion: Web UI through the companion turned off");
    }
    // An open link learns these from the next cfg frame; the relay handshake
    // re-answers whether the link is needed at all.
    if (logChanged || archiveChanged || bcChanged) companionOutboxChanged();
    if (logChanged)
      self.lifecycleLog(log ? "Companion: Diagnostic log through the companion turned on"
                            : "Companion: Diagnostic log through the companion turned off");
    if (archiveChanged)
      self.lifecycleLog(archive ? "Companion: Shot archive in the companion turned on"
                                : "Companion: Shot archive in the companion turned off");
    if (bcChanged)
      self.lifecycleLog(bc ? "Companion: Beanconqueror through the companion turned on"
                           : "Companion: Beanconqueror through the companion turned off");
  } else {
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", nullptr, "UNKNOWN_COMPANION_ACTION");
  }
  return sendJson(request, STATUS_OK, "{\"ok\":true}");
}

}  // namespace openbrewbyweight

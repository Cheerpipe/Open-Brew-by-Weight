// Optional Web UI login and Home Assistant pairing. g_webAccess is guarded by
// dataMux_; handlers change it in RAM and the network task persists it
// (serviceWebAccess). Tokens never reach logs, URLs or diagnostics.

namespace {

constexpr const char *WEB_UI_SESSION_HEADER = "X-WebUI-Session";

uint32_t webAccessNowUtc(uint32_t nowMs) {
  return g_wallClock.synced() ? g_wallClock.nowUtcSec(nowMs) : 0;
}

// `prefix` precedes the 32 hex digits ("Bearer " for the integration).
bool readWebAccessToken(httpd_req_t *request, const char *header,
                        const char *prefix, uint8_t token[WEB_ACCESS_TOKEN_BYTES]) {
  char text[8 + WEB_ACCESS_TOKEN_TEXT] = {};
  const size_t prefixLength = strlen(prefix);
  const bool ok =
      httpd_req_get_hdr_value_len(request, header) ==
          prefixLength + WEB_ACCESS_TOKEN_TEXT - 1 &&
      httpd_req_get_hdr_value_str(request, header, text, sizeof(text)) == ESP_OK &&
      strncmp(text, prefix, prefixLength) == 0 &&
      parseWebAccessToken(text + prefixLength, token);
  memset(text, 0, sizeof(text));
  return ok;
}

}  // namespace

OpenBrewByWeightNetwork::PasswordCheck OpenBrewByWeightNetwork::checkDevicePassword(
    httpd_req_t *request, const char *password) {
  const uint32_t now = millis();
  // The cooldown is keyed by the peer address, never the port; odd ids keep
  // 0 free for an empty slot. Through the companion's signed relay the peer
  // is the browser's address, never the companion's own.
  sockaddr_storage address = {};
  socklen_t addressLength = sizeof(address);
  uint32_t peer = 1;
  char screen[COMPANION_RELAY_SCREEN_TEXT], relayed[COMPANION_RELAY_ADDR_TEXT];
  if (relayIdentity(request, screen, relayed))
    peer = webAccessFnv(relayed, strlen(relayed)) | 1U;
  else if (getpeername(httpd_req_to_sockfd(request), reinterpret_cast<sockaddr *>(&address),
                       &addressLength) == 0)
    peer = (address.ss_family == AF_INET6
                ? webAccessFnv(&reinterpret_cast<const sockaddr_in6 &>(address).sin6_addr, 16)
                : webAccessFnv(&reinterpret_cast<const sockaddr_in &>(address).sin_addr, 4)) | 1U;
  char expected[WIFI_PASSWORD_CAPACITY] = {};
  dataMux_.lock();
  WebAccessPasswordPeer *slot = webAccessPasswordSlot(passwordPeers_, peer, now);
  const bool coolingDown = webAccessPasswordCooling(slot, now);
  memcpy(expected, settings_.devicePassword, sizeof(expected));
  dataMux_.unlock();
  const bool matches = !coolingDown && secretsMatch(password, expected);
  memset(expected, 0, sizeof(expected));
  if (coolingDown) return PasswordCheck::COOLDOWN;
  dataMux_.lock();
  const bool paused = webAccessPasswordAttempt(*slot, matches, now);
  dataMux_.unlock();
  if (paused) lifecycleLog("Device password rejected; attempts from this device paused for 30 s");
  else if (!matches) lifecycleLog("Device password rejected");
  return matches ? PasswordCheck::OK : PasswordCheck::WRONG;
}

bool OpenBrewByWeightNetwork::webAccessActive() {
  TaskLockGuard lock(dataMux_);
  return g_webAccess.stored.enabled != 0 && !passwordIsFactoryDefault(settings_);
}

bool OpenBrewByWeightNetwork::softApPeer(httpd_req_t *request) {
  bool apActive = false;
  {
    TaskLockGuard lock(dataMux_);
    apActive = status_.apActive;
  }
  return apActive && socketServedBySoftAp(httpd_req_to_sockfd(request));
}

bool OpenBrewByWeightNetwork::webAccessSessionAllowed(httpd_req_t *request) {
  if (!webAccessActive() || softApPeer(request)) return true;
  uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  if (!readWebAccessToken(request, WEB_UI_SESSION_HEADER, "", token)) return false;
  const uint32_t now = millis();
  bool valid = false;
  {
    TaskLockGuard lock(dataMux_);
    valid = webAccessSessionValid(g_webAccess, token, now, webAccessNowUtc(now));
  }
  memset(token, 0, sizeof(token));
  return valid;
}

void OpenBrewByWeightNetwork::webAccessSignOut(const uint8_t *keep, bool everything) {
  TaskLockGuard lock(dataMux_);
  if (everything) {
    webAccessDisable(g_webAccess);
    return;
  }
  webAccessRevokeSessions(g_webAccess, keep);
  // With the login off nobody holds a session, so the window keeps control.
  if (keep == nullptr && g_webAccess.stored.enabled != 0) {
    activeWebUiClientId_[0] = '\0';
    adminUnlocked_ = false;
    adminUnlock_ = CompanionPrincipal{};
    adminUnlockUntilMs_ = 0;
  }
}

// The window that changes the password stays signed in; applying that same
// request revokes every other session.
void OpenBrewByWeightNetwork::keepPasswordChangeSession(httpd_req_t *request,
                                                        uint32_t requestId) {
  uint8_t keep[WEB_ACCESS_TOKEN_BYTES] = {};
  const bool keepValid = readWebAccessToken(request, WEB_UI_SESSION_HEADER, "", keep);
  {
    TaskLockGuard lock(dataMux_);
    memcpy(passwordKeepToken_, keep, sizeof(keep));
    passwordKeepRequestId_ = requestId;
    passwordKeepValid_ = keepValid;
  }
  memset(keep, 0, sizeof(keep));
}

void OpenBrewByWeightNetwork::serviceWebAccess() {
  const uint32_t now = millis();
  if ((webAccessRetryAtMs_ != 0 && static_cast<int32_t>(now - webAccessRetryAtMs_) < 0) ||
      !networkFlashWriteAllowed()) {
    return;
  }
  WebAccessStored stored;
  {
    TaskLockGuard lock(dataMux_);
    if (!g_webAccess.dirty) return;
    stored = g_webAccess.stored;
    g_webAccess.dirty = false;
  }
  const bool saved = saveWebAccessStored(stored);
  memset(&stored, 0, sizeof(stored));
  if (saved) {
    webAccessRetryAtMs_ = 0;
    return;
  }
  {
    TaskLockGuard lock(dataMux_);
    g_webAccess.dirty = true;
  }
  webAccessRetryAtMs_ = (now + 30000U) | 1U;
  lifecycleLog("Web UI access settings could not be saved; retrying");
}

bool OpenBrewByWeightNetwork::readWebAccessBody(httpd_req_t *request,
                                                WebAccessBody &body,
                                                esp_err_t &sent) {
  sent = lockJsonBody(request, "A JSON request is required.");
  if (sent != ESP_OK) return false;
  cJSON *root = parseJsonDocument(workBuf_->requestBody);
  static const char *const fields[] = {"password", "remember", "action", "id", "allow", "device"};
  bool parsed = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 6);
  body.hasPassword = parsed && jsonFieldPresent(root, "password");
  if (body.hasPassword) {
    parsed = jsonString(root, "password", body.password, sizeof(body.password), false);
  }
  if (parsed && jsonFieldPresent(root, "remember")) parsed = jsonBoolean(root, "remember", body.remember);
  if (parsed && jsonFieldPresent(root, "action")) {
    parsed = jsonString(root, "action", body.action, sizeof(body.action), false);
  }
  if (parsed && jsonFieldPresent(root, "id")) parsed = jsonUint32(root, "id", body.id);
  if (parsed && jsonFieldPresent(root, "allow")) parsed = jsonBoolean(root, "allow", body.allow);
  if (parsed && jsonFieldPresent(root, "device")) {
    parsed = jsonString(root, "device", body.device, sizeof(body.device), true);
  }
  if (root != nullptr) cJSON_Delete(root);
  unlockJsonBody();
  if (!parsed) {
    sent = sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                     "The request fields are invalid.");
  }
  return parsed;
}

esp_err_t OpenBrewByWeightNetwork::sendPasswordCheckError(httpd_req_t *request,
                                                          PasswordCheck check) {
  return check == PasswordCheck::COOLDOWN
             ? sendError(request, STATUS_TOO_MANY, "AUTH_COOLDOWN",
                         "Too many attempts. Try again in 30 seconds.")
             : sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                         "Device password is incorrect.");
}

esp_err_t OpenBrewByWeightNetwork::authStatusHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const bool signedIn = self.webAccessSessionAllowed(request);
  const bool required = self.webAccessActive() && !self.softApPeer(request);
  // Device names hold only letters, digits, spaces and hyphens: no escaping.
  char name[DEVICE_NAME_CAPACITY] = {};
  {
    TaskLockGuard lock(self.dataMux_);
    copyCString(name, sizeof(name), self.settings_.deviceName);
  }
  char json[DEVICE_NAME_CAPACITY + 80] = {};
  snprintf(json, sizeof(json),
           "{\"loginRequired\":%s,\"signedIn\":%s,\"deviceName\":\"%s\"}",
           required ? "true" : "false", signedIn ? "true" : "false", name);
  return sendJson(request, STATUS_OK, json);
}

esp_err_t OpenBrewByWeightNetwork::authLoginHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.webAccessActive()) {
    return sendError(request, STATUS_CONFLICT, "AUTH_NOT_REQUIRED",
                     "This Web UI does not ask for a password.");
  }
  WebAccessBody body;
  esp_err_t sent = ESP_OK;
  if (!self.readWebAccessBody(request, body, sent)) return sent;
  const PasswordCheck check = self.checkDevicePassword(request, body.password);
  if (check != PasswordCheck::OK) return sendPasswordCheckError(request, check);
  uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  esp_fill_random(token, sizeof(token));
  const uint32_t now = millis();
  {
    TaskLockGuard lock(self.dataMux_);
    webAccessIssueSession(g_webAccess, token, body.remember, now, webAccessNowUtc(now),
                          body.device);
  }
  // Signing in also claims control for this window, and the password typed
  // just now opens Admin for it; a later claim clears that as usual. A
  // companion screen signs in on the companion's claim, as its claim attaches.
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  CompanionPrincipal holder;
  const bool claimed = readWebUiClientId(request, clientId, sizeof(clientId)) &&
                       self.webUiPrincipal(request, holder);
  if (claimed) {
    {
      TaskLockGuard lock(self.dataMux_);
      if (!holder.relayed || strcmp(self.activeWebUiClientId_, clientId) != 0) {
        memcpy(self.activeWebUiClientId_, clientId, sizeof(clientId));
        self.webUiOverrideActive_ = false;
        self.webUiOverrideUntilMs_ = 0;
        self.webUiOverride_ = CompanionPrincipal{};
      }
    }
    self.grantAdminUnlock(holder, now);
  }
  char json[128] = {};
  char text[WEB_ACCESS_TOKEN_TEXT] = {};
  formatWebAccessToken(token, text);
  snprintf(json, sizeof(json),
           "{\"token\":\"%s\",\"remember\":%s,\"active\":%s,\"compatibilityMode\":%s}",
           text, body.remember ? "true" : "false", claimed ? "true" : "false",
           firmwareCompatibilityMode() ? "true" : "false");
  memset(token, 0, sizeof(token));
  memset(text, 0, sizeof(text));
  self.lifecycleLog(body.remember ? "Web UI signed in (remembered device)"
                                  : "Web UI signed in");
  const esp_err_t result = sendJson(request, STATUS_OK, json);
  memset(json, 0, sizeof(json));
  return result;
}

esp_err_t OpenBrewByWeightNetwork::authLogoutHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  WebAccessBody body;
  esp_err_t sent = ESP_OK;
  if (!self.readWebAccessBody(request, body, sent)) return sent;
  uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  const bool hasToken = readWebAccessToken(request, WEB_UI_SESSION_HEADER, "", token);
  // A companion screen signs out alone: the companion's claim stays for the others.
  CompanionPrincipal caller;
  const bool hasClient = readWebUiClientId(request, clientId, sizeof(clientId)) &&
                         self.webUiPrincipal(request, caller);
  {
    TaskLockGuard lock(self.dataMux_);
    if (hasToken) webAccessRevokeSessions(g_webAccess, token, true);
    if (hasClient && !caller.relayed && strcmp(self.activeWebUiClientId_, clientId) == 0) {
      self.activeWebUiClientId_[0] = '\0';
    }
    if (hasClient && companionPrincipalHolds(self.adminUnlock_, caller)) {
      self.adminUnlocked_ = false;
      self.adminUnlock_ = CompanionPrincipal{};
    }
  }
  memset(token, 0, sizeof(token));
  return sendJson(request, STATUS_OK, "{\"signedIn\":false}");
}

// Admin → Access. Turning the login on or off asks for the current password
// even inside an unlocked Admin; the others act on the caller's own session.
esp_err_t OpenBrewByWeightNetwork::authAccessHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  WebAccessBody body;
  esp_err_t sent = ESP_OK;
  if (!self.readWebAccessBody(request, body, sent)) return sent;
  const bool enable = strcmp(body.action, "enable") == 0;
  const bool disable = strcmp(body.action, "disable") == 0;
  if (enable || disable) {
    bool factory = false;
    {
      TaskLockGuard lock(self.dataMux_);
      factory = passwordIsFactoryDefault(self.settings_);
    }
    if (enable && factory) {
      return sendError(request, STATUS_CONFLICT, "FACTORY_PASSWORD",
                       "Choose a new device password before turning on the login.");
    }
    if (!body.hasPassword) {
      return sendError(request, STATUS_UNPROCESSABLE, "DEVICE_PASSWORD_REQUIRED",
                       "Enter the device password.");
    }
    const PasswordCheck check = self.checkDevicePassword(request, body.password);
    if (check != PasswordCheck::OK) return sendPasswordCheckError(request, check);
  }
  if (enable) {
    // The person who turns the login on stays signed in on this device.
    uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
    esp_fill_random(token, sizeof(token));
    const uint32_t now = millis();
    {
      TaskLockGuard lock(self.dataMux_);
      g_webAccess.stored.enabled = 1;
      g_webAccess.dirty = true;
      webAccessIssueSession(g_webAccess, token, true, now, webAccessNowUtc(now), body.device);
    }
    char text[WEB_ACCESS_TOKEN_TEXT] = {};
    char json[80] = {};
    formatWebAccessToken(token, text);
    snprintf(json, sizeof(json), "{\"enabled\":true,\"token\":\"%s\"}", text);
    memset(token, 0, sizeof(token));
    memset(text, 0, sizeof(text));
    self.lifecycleLog("Web UI login turned on");
    const esp_err_t result = sendJson(request, STATUS_OK, json);
    memset(json, 0, sizeof(json));
    return result;
  }
  if (disable) {
    self.webAccessSignOut(nullptr, true);
    self.lifecycleLog("Web UI login turned off");
    return sendJson(request, STATUS_OK, "{\"enabled\":false}");
  }
  if (strcmp(body.action, "rename") == 0) {
    bool renamed = false;
    {
      TaskLockGuard lock(self.dataMux_);
      renamed = webAccessRenameId(g_webAccess, body.id, body.device);
    }
    return renamed ? sendJson(request, STATUS_OK, "{\"ok\":true}")
                   : sendError(request, STATUS_CONFLICT, "SESSION_NOT_FOUND",
                               "That device is no longer signed in.");
  }
  if (strcmp(body.action, "signOut") == 0) {
    bool revoked = false;
    {
      TaskLockGuard lock(self.dataMux_);
      revoked = webAccessRevokeId(g_webAccess, body.id);
    }
    if (!revoked) {
      return sendError(request, STATUS_CONFLICT, "SESSION_NOT_FOUND",
                       "That device is no longer signed in.");
    }
    self.lifecycleLog("Web UI signed out one device");
    return sendJson(request, STATUS_OK, "{\"ok\":true}");
  }
  if (strcmp(body.action, "signOutOthers") == 0) {
    uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
    const bool own = readWebAccessToken(request, WEB_UI_SESSION_HEADER, "", token);
    {
      TaskLockGuard lock(self.dataMux_);
      webAccessRevokeSessions(g_webAccess, own ? token : nullptr);
    }
    memset(token, 0, sizeof(token));
    self.lifecycleLog("Web UI signed out on other devices");
    return sendJson(request, STATUS_OK, "{\"ok\":true}");
  }
  if (strcmp(body.action, "forgetHomeAssistant") == 0) {
    {
      TaskLockGuard lock(self.dataMux_);
      webAccessForgetHomeAssistant(g_webAccess);
    }
    self.lifecycleLog("Home Assistant disconnected");
    return sendJson(request, STATUS_OK, "{\"ok\":true}");
  }
  return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST",
                   "Unknown access action.");
}

esp_err_t OpenBrewByWeightNetwork::authNoticeHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  WebAccessBody body;
  esp_err_t sent = ESP_OK;
  if (!self.readWebAccessBody(request, body, sent)) return sent;
  {
    TaskLockGuard lock(self.dataMux_);
    if (g_webAccess.stored.noticeDismissed == 0) {
      g_webAccess.stored.noticeDismissed = 1;
      g_webAccess.dirty = true;
    }
  }
  return sendJson(request, STATUS_OK, "{\"ok\":true}");
}

esp_err_t OpenBrewByWeightNetwork::authPairingHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  WebAccessBody body;
  esp_err_t sent = ESP_OK;
  if (!self.readWebAccessBody(request, body, sent)) return sent;
  uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  esp_fill_random(token, sizeof(token));
  const uint32_t now = millis();
  bool decided = false;
  {
    TaskLockGuard lock(self.dataMux_);
    decided = webAccessDecidePairing(g_webAccess, body.id, body.allow, token, now,
                                     webAccessNowUtc(now));
  }
  memset(token, 0, sizeof(token));
  if (!decided) {
    return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_PENDING",
                     "This Home Assistant request is no longer waiting.");
  }
  self.lifecycleLog(body.allow ? "Home Assistant pairing approved"
                               : "Home Assistant pairing denied");
  return sendJson(request, STATUS_OK, "{\"ok\":true}");
}

// Admin → Access: login state, Home Assistant, and every signed-in device by
// id (never by token). Labels are sanitized on store, so they embed as is.
bool OpenBrewByWeightNetwork::appendWebAccessStatus(httpd_req_t *request, size_t *used) {
  uint8_t own[WEB_ACCESS_TOKEN_BYTES] = {};
  const bool hasOwn = readWebAccessToken(request, WEB_UI_SESSION_HEADER, "", own);
  const uint32_t now = millis(), nowUtc = webAccessNowUtc(now);
  TaskLockGuard lock(dataMux_);
  const bool factory = passwordIsFactoryDefault(settings_);
  const int ownId = hasOwn ? webAccessSessionId(g_webAccess, own) : -1;
  memset(own, 0, sizeof(own));
  const WebAccessStored &stored = g_webAccess.stored;
  bool ok = statusJsonAppend(
      used,
      ",\"access\":{\"loginEnabled\":%s,\"factoryPassword\":%s,"
      "\"homeAssistantConnected\":%s,\"homeAssistantSinceUtcSec\":%lu,"
      "\"homeAssistantRecent\":%s,\"sessions\":[",
      stored.enabled != 0 && !factory ? "true" : "false", factory ? "true" : "false",
      stored.homeAssistant != 0 ? "true" : "false",
      static_cast<unsigned long>(stored.homeAssistantSinceUtc),
      integrationSeen_ && now - integrationSeenMs_ < 24UL * 3600UL * 1000UL ? "true" : "false");
  bool first = true;
  const auto session = [&](uint32_t id, const char *label, bool remembered, long agoS) {
    ok = ok && statusJsonAppend(used,
                                "%s{\"id\":%lu,\"label\":\"%s\",\"remembered\":%s,"
                                "\"agoS\":%ld,\"self\":%s}",
                                first ? "" : ",", static_cast<unsigned long>(id), label,
                                remembered ? "true" : "false", agoS,
                                static_cast<int>(id) == ownId ? "true" : "false");
    first = false;
  };
  for (uint8_t slot = 0; slot < WEB_ACCESS_REMEMBERED_SLOTS; ++slot) {
    if (!webAccessRememberedLive(stored, slot, nowUtc)) continue;
    const uint32_t usedUtc = stored.remembered[slot].usedUtc;
    // Daily stamps: -1 when the clock is not synced or never stamped.
    session(slot, stored.remembered[slot].label, true,
            nowUtc != 0 && usedUtc != 0 && nowUtc >= usedUtc ? static_cast<long>(nowUtc - usedUtc) : -1);
  }
  for (uint8_t slot = 0; slot < WEB_ACCESS_BROWSER_SLOTS; ++slot) {
    const auto &browser = g_webAccess.browser[slot];
    if (browser.live && static_cast<int32_t>(now - browser.untilMs) < 0) {
      session(WEB_ACCESS_REMEMBERED_SLOTS + slot, browser.label, false,
              static_cast<long>((now - browser.seenMs) / 1000U));
    }
  }
  return ok && statusJsonAppend(used, "]}");
}

// Integration API access routes, and the bearer check for every other
// integration route while the login is on. Returns true when it answered.
// Kept out of line so its buffers leave the httpd stack before the route runs.
__attribute__((noinline)) bool OpenBrewByWeightNetwork::integrationAccessRoute(
    httpd_req_t *request, esp_err_t &result) {
  const char *uri = request->uri;
  const bool active = webAccessActive();
  if (apiUriMatches(uri, "/api/v1/integration/info") && request->method == HTTP_GET) {
    char name[DEVICE_NAME_CAPACITY] = {};
    {
      TaskLockGuard lock(dataMux_);
      copyCString(name, sizeof(name), settings_.deviceName);
    }
    char firmware[48] = {};
    sanitizeJsonEmbed(FW_VERSION, firmware, sizeof(firmware));
    char json[DEVICE_NAME_CAPACITY + 160] = {};
    snprintf(json, sizeof(json),
             "{\"apiVersion\":1,\"name\":\"%s\",\"firmwareVersion\":\"%s\","
             "\"authRequired\":%s}",
             name, firmware, active ? "true" : "false");
    result = sendJson(request, STATUS_OK, json);
    return true;
  }
  const bool exchange =
      apiUriMatches(uri, "/api/v1/integration/auth") && request->method == HTTP_POST;
  const bool pair = apiUriMatches(uri, "/api/v1/integration/pair");
  if ((exchange || pair) && !active) {
    result = sendError(request, STATUS_CONFLICT, "AUTH_NOT_REQUIRED",
                       "This controller does not ask for a password.");
    return true;
  }
  const uint32_t now = millis();
  if (exchange) {
    WebAccessBody body;
    if (!readWebAccessBody(request, body, result)) return true;
    const PasswordCheck check = checkDevicePassword(request, body.password);
    if (check != PasswordCheck::OK) {
      result = sendPasswordCheckError(request, check);
      return true;
    }
    uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
    esp_fill_random(token, sizeof(token));
    {
      TaskLockGuard lock(dataMux_);
      webAccessSetHomeAssistant(g_webAccess, token, webAccessNowUtc(now));
    }
    char text[WEB_ACCESS_TOKEN_TEXT] = {};
    char json[80] = {};
    formatWebAccessToken(token, text);
    snprintf(json, sizeof(json), "{\"apiVersion\":1,\"token\":\"%s\"}", text);
    memset(token, 0, sizeof(token));
    memset(text, 0, sizeof(text));
    lifecycleLog("Home Assistant connected with the device password");
    result = sendJson(request, STATUS_OK, json);
    memset(json, 0, sizeof(json));
    return true;
  }
  if (pair && request->method == HTTP_POST) {
    WebAccessBody body;
    if (!readWebAccessBody(request, body, result)) return true;
    uint32_t random[2] = {};
    esp_fill_random(random, sizeof(random));
    const uint32_t id = random[0] != 0 ? random[0] : 1;
    bool started = false;
    char code[5] = {};
    {
      TaskLockGuard lock(dataMux_);
      started = webAccessStartPairing(g_webAccess, id,
                                      static_cast<uint16_t>(random[1] % 10000U), now);
      memcpy(code, g_webAccess.pairing.code, sizeof(code));
    }
    if (!started) {
      result = sendError(request, STATUS_TOO_MANY, "PAIRING_BUSY",
                         "Another pairing request is open. Try again shortly.");
      return true;
    }
    char json[96] = {};
    snprintf(json, sizeof(json),
             "{\"apiVersion\":1,\"id\":%lu,\"code\":\"%s\",\"expiresInS\":%lu}",
             static_cast<unsigned long>(id), code,
             static_cast<unsigned long>(WEB_ACCESS_PAIRING_MS / 1000U));
    lifecycleLog("Home Assistant asked to pair; waiting for approval in the Web UI");
    result = sendJson(request, STATUS_OK, json);
    return true;
  }
  if (pair && request->method == HTTP_GET) {
    char query[32] = {}, value[12] = {};
    char *end = nullptr;
    const bool hasId =
        httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "id", value, sizeof(value)) == ESP_OK;
    const unsigned long id = hasId ? strtoul(value, &end, 10) : 0;
    uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
    WebAccessPairing state = WebAccessPairing::NONE;
    if (hasId && end != value && *end == '\0') {
      TaskLockGuard lock(dataMux_);
      state = webAccessPairingStatus(g_webAccess, static_cast<uint32_t>(id), now, token);
    }
    char text[WEB_ACCESS_TOKEN_TEXT] = {};
    char json[96] = {};
    formatWebAccessToken(token, text);
    static const char *const names[] = {"unknown", "pending", "approved", "denied", "expired"};
    snprintf(json, sizeof(json), "{\"apiVersion\":1,\"state\":\"%s\"%s%s%s}",
             names[static_cast<uint8_t>(state)],
             state == WebAccessPairing::APPROVED ? ",\"token\":\"" : "",
             state == WebAccessPairing::APPROVED ? text : "",
             state == WebAccessPairing::APPROVED ? "\"" : "");
    memset(token, 0, sizeof(token));
    memset(text, 0, sizeof(text));
    result = sendJson(request, STATUS_OK, json);
    memset(json, 0, sizeof(json));
    return true;
  }
  // The Web UI's own Quick Settings calls ride its session, and only other
  // clients count as recent Home Assistant use.
  const bool webUi = httpd_req_get_hdr_value_len(request, WEB_UI_CLIENT_HEADER) > 0;
  if (!webUi) {
    TaskLockGuard lock(dataMux_);
    integrationSeen_ = true;
    integrationSeenMs_ = now;
  }
  if (!active) return false;
  if (webUi) {
    if (webAccessSessionAllowed(request)) return false;
    result = sendError(request, STATUS_UNAUTHORIZED, "AUTH_REQUIRED",
                       "Sign in with the device password.");
    return true;
  }
  uint8_t token[WEB_ACCESS_TOKEN_BYTES] = {};
  bool valid = readWebAccessToken(request, "Authorization", "Bearer ", token);
  if (valid) {
    TaskLockGuard lock(dataMux_);
    valid = webAccessHomeAssistantValid(g_webAccess, token);
  }
  memset(token, 0, sizeof(token));
  if (valid) return false;
  result = sendError(request, STATUS_UNAUTHORIZED, "INTEGRATION_AUTH_REQUIRED",
                     "Connect with the device password or approve this request in the Web UI.");
  return true;
}

const char *configLockReason(const ControlGateSnapshot &status) {
  if (status.activeCycle) return "active_shot";
  if (status.machineRunning) return "machine_running";
  if (status.physicalActivatorOn) return "activator_on";
  if (status.maintenanceLeaseActive) return "maintenance";
  if (status.state != StopperState::READY) return "not_ready";
  return "none";
}
const char *configLockReason(const ControlStatusSnapshot &status) {
  return configLockReason(controlGateOf(status));
}

bool apiUriMatches(const char *uri, const char *path) {
  if (uri == nullptr || path == nullptr) {
    return false;
  }
  const size_t length = strlen(path);
  return strncmp(uri, path, length) == 0 &&
         (uri[length] == '\0' || uri[length] == '?');
}

bool readWebUiClientId(httpd_req_t *request, char *output,
                       size_t capacity) {
  if (request == nullptr || output == nullptr || capacity < 2) {
    return false;
  }
  const size_t length =
      httpd_req_get_hdr_value_len(request, WEB_UI_CLIENT_HEADER);
  if (length < 16 || length + 1 > capacity) {
    return false;
  }
  if (httpd_req_get_hdr_value_str(request, WEB_UI_CLIENT_HEADER, output,
                                   capacity) != ESP_OK) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char character = output[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      output[0] = '\0';
      return false;
    }
  }
  return true;
}

// Refusals a LAN peer can repeat at will log at most once per 10 s per call
// site; the line reports how many happened since the previous one.
struct RefusalLog {
  std::atomic<uint32_t> loggedAtMs{0}, refused{0};
  // The count to report now, or 0 while the window is still quiet.
  uint32_t due() {
    refused.fetch_add(1);
    const uint32_t now = millis(), at = loggedAtMs.load();
    if (at != 0 && now - at < 10000) return 0;
    loggedAtMs.store(now | 1U);
    return refused.exchange(0);
  }
};

}  // namespace

esp_err_t OpenBrewByWeightNetwork::claimHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  if (!readWebUiClientId(request, clientId, sizeof(clientId))) {
    return sendError(request, STATUS_BAD_REQUEST, "UI_CLIENT_INVALID",
                     "X-WebUI-Client must be a 16-24 character lowercase hex id.");
  }
  if (!self.webAccessSessionAllowed(request)) {
    return sendError(request, STATUS_UNAUTHORIZED, "AUTH_REQUIRED",
                     "Sign in with the device password.");
  }
  self.dataMux_.lock();
  memcpy(self.activeWebUiClientId_, clientId, sizeof(clientId));
  self.webUiOverrideActive_ = false;
  self.webUiOverrideUntilMs_ = 0;
  self.dataMux_.unlock();
  self.clearAdminUnlock();
  memset(clientId, 0, sizeof(clientId));
  return sendJson(request, STATUS_OK,
         firmwareCompatibilityMode()
             ? "{\"active\":true,\"compatibilityMode\":true}"
             : "{\"active\":true,\"compatibilityMode\":false}");
}

#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1
// Development builds keep administration public: the UI never calls the
// unlock endpoints, so the handlers are not compiled and their routes are
// not registered. The unlock state machine below stays for release builds.
#else
esp_err_t OpenBrewByWeightNetwork::unlockHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireActiveWebUiClient(request)) return ESP_OK;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  if (!self.lockConfirmedBody(request,
                              "An explicit confirmation is required.",
                              "UNSAFE_WEBUI_OVERRIDE")) {
    return sendError(request, STATUS_UNPROCESSABLE, "UNLOCK_NOT_CONFIRMED",
                     "The unsafe WebUI override was not explicitly confirmed.");
  }
  const uint32_t now = millis();
  self.dataMux_.lock();
  self.webUiOverrideActive_ = true;
  self.webUiOverrideUntilMs_ = now + WEB_UI_OVERRIDE_MS;
  self.dataMux_.unlock();
  return sendJson(request, STATUS_OK, "{\"unlocked\":true}");
}
#endif

bool OpenBrewByWeightNetwork::webUiOverrideAllowed(httpd_req_t *request) {
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  if (!readWebUiClientId(request, clientId, sizeof(clientId))) return false;
  const uint32_t now = millis();
  bool allowed = false;
  dataMux_.lock();
  if (webUiOverrideActive_ && activeWebUiClientId_[0] != '\0' &&
      strcmp(activeWebUiClientId_, clientId) == 0) {
    if (static_cast<int32_t>(now - webUiOverrideUntilMs_) >= 0) {
      webUiOverrideActive_ = false;
      webUiOverrideUntilMs_ = 0;
    } else {
      allowed = true;
    }
  }
  dataMux_.unlock();
  memset(clientId, 0, sizeof(clientId));
  return allowed;
}

uint32_t OpenBrewByWeightNetwork::webUiOverrideRemainingMs(httpd_req_t *request) {
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  if (!readWebUiClientId(request, clientId, sizeof(clientId))) return 0;
  const uint32_t now = millis();
  uint32_t remaining = 0;
  dataMux_.lock();
  if (webUiOverrideActive_ && activeWebUiClientId_[0] != '\0' &&
      strcmp(activeWebUiClientId_, clientId) == 0 &&
      static_cast<int32_t>(now - webUiOverrideUntilMs_) < 0) {
    remaining = webUiOverrideUntilMs_ - now;
  }
  dataMux_.unlock();
  memset(clientId, 0, sizeof(clientId));
  return remaining;
}

bool OpenBrewByWeightNetwork::webUiConfigurationAllowed(
    httpd_req_t *request, const ControlGateSnapshot &status) {
  if (controlAllowsConfiguration(status)) {
    return true;
  }
  return webUiOverrideAllowed(request);
}

bool OpenBrewByWeightNetwork::historyMutationAllowed(
    httpd_req_t *request, const ControlGateSnapshot &status) {
  if (!controlAllowsHistoryMutation(status)) {
    return false;
  }
  return webUiConfigurationAllowed(request, status);
}

void OpenBrewByWeightNetwork::clearAdminUnlock() {
  dataMux_.lock();
  adminUnlocked_ = false;
  adminUnlockClientId_[0] = '\0';
  adminUnlockUntilMs_ = 0;
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::grantAdminUnlock(const char *clientId, uint32_t now) {
  if (clientId == nullptr) {
    return;
  }
  dataMux_.lock();
  memcpy(adminUnlockClientId_, clientId, sizeof(adminUnlockClientId_));
  adminUnlocked_ = true;
  adminUnlockUntilMs_ = now + ADMIN_UNLOCK_IDLE_MS;
  dataMux_.unlock();
}

void OpenBrewByWeightNetwork::touchAdminUnlock() {
  const uint32_t now = millis();
  dataMux_.lock();
  if (adminUnlocked_) {
    adminUnlockUntilMs_ = now + ADMIN_UNLOCK_IDLE_MS;
  }
  dataMux_.unlock();
}

bool OpenBrewByWeightNetwork::socketServedBySoftAp(int socketFd) {
  if (socketFd < 0) {
    return false;
  }
  // The dual-stack httpd listener reports IPv4 clients as IPv4-mapped IPv6
  // addresses; a native IPv6 peer never qualifies (the SoftAP is IPv4-only).
  const auto ipv4Host = [](const sockaddr_storage &address, uint32_t &host) {
    if (address.ss_family == AF_INET) {
      host = ntohl(reinterpret_cast<const sockaddr_in &>(address).sin_addr.s_addr);
      return true;
    }
    const auto &v6 = reinterpret_cast<const sockaddr_in6 &>(address).sin6_addr;
    if (address.ss_family != AF_INET6 || !IN6_IS_ADDR_V4MAPPED(&v6)) return false;
    host = ntohl(v6.un.u32_addr[3]);
    return true;
  };
  sockaddr_storage local = {}, peer = {};
  socklen_t length = sizeof(local);
  uint32_t localHost = 0, peerHost = 0;
  if (getsockname(socketFd, reinterpret_cast<sockaddr *>(&local), &length) != 0 ||
      !ipv4Host(local, localHost)) {
    return false;
  }
  length = sizeof(peer);
  if (getpeername(socketFd, reinterpret_cast<sockaddr *>(&peer), &length) != 0 ||
      !ipv4Host(peer, peerHost)) {
    return false;
  }
  // The local-address match is the discriminator: only a socket actually
  // served by the SoftAP interface address qualifies, so a LAN that happens
  // to number itself 192.168.4.0/24 never inherits the bypass.
  return localHost == 0xC0A80401u &&
         (peerHost & 0xFFFFFF00u) == 0xC0A80400u;
}

bool OpenBrewByWeightNetwork::adminUnlockAllowed(httpd_req_t *request) {
#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1
  // Development build: administration is public by design. Returning true
  // here makes every admin-gated handler and the status page report an
  // always-unlocked session without ever touching the unlock state.
  (void)request;
  return true;
#else
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  const bool hasClient =
      readWebUiClientId(request, clientId, sizeof(clientId));
  const uint32_t now = millis();
  bool allowed = false;
  bool apActive = false;
  dataMux_.lock();
  apActive = status_.apActive;
  if (hasClient && adminUnlocked_ && adminUnlockClientId_[0] != '\0' &&
      strcmp(adminUnlockClientId_, clientId) == 0) {
    if (static_cast<int32_t>(now - adminUnlockUntilMs_) >= 0) {
      adminUnlocked_ = false;
      adminUnlockClientId_[0] = '\0';
      adminUnlockUntilMs_ = 0;
    } else {
      allowed = true;
    }
  }
  dataMux_.unlock();
  memset(clientId, 0, sizeof(clientId));
  // SoftAP onboarding bypass: whoever joined the device network already
  // proved the WPA2 passphrase, which is the device password.
  if (!allowed && apActive &&
      socketServedBySoftAp(httpd_req_to_sockfd(request))) {
    allowed = true;
  }
  return allowed;
#endif
}

bool OpenBrewByWeightNetwork::requireAdminUnlock(httpd_req_t *request) {
  if (!adminUnlockAllowed(request)) {
    sendError(request, STATUS_UNAUTHORIZED, "ADMIN_LOCKED",
              "Unlock administration with the device password first.");
    return false;
  }
  touchAdminUnlock();
  return true;
}

bool OpenBrewByWeightNetwork::diagnosticPageEnabled() {
  bool visible = false;
  dataMux_.lock();
  visible = settings_.runtime.showDiagnosticPage;
  dataMux_.unlock();
  return visible;
}

#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1
// Password unlock/lock handlers are release-only (see unlockHandler above).
#else
esp_err_t OpenBrewByWeightNetwork::adminUnlockHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  char password[WIFI_PASSWORD_CAPACITY] = {};
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {"password"};
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonString(root, "password", password, sizeof(password), false);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    memset(password, 0, sizeof(password));
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_DEVICE_PASSWORD",
                     "password is required.");
  }

  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  if (!readWebUiClientId(request, clientId, sizeof(clientId))) {
    memset(password, 0, sizeof(password));
    return sendError(request, STATUS_BAD_REQUEST, "UI_CLIENT_INVALID",
                     "X-WebUI-Client must be a 16-24 character lowercase hex id.");
  }

  const PasswordCheck check = self.checkDevicePassword(request, password);
  memset(password, 0, sizeof(password));
  if (check != PasswordCheck::OK) {
    memset(clientId, 0, sizeof(clientId));
    return check == PasswordCheck::COOLDOWN
               ? sendError(request, STATUS_TOO_MANY, "ADMIN_UNLOCK_COOLDOWN",
                           "Too many failed unlock attempts. Try again shortly.")
               : sendError(request, STATUS_UNAUTHORIZED, "DEVICE_PASSWORD_INVALID",
                           "Device password is incorrect.");
  }

  self.grantAdminUnlock(clientId, millis());
  memset(clientId, 0, sizeof(clientId));
  return sendJson(request, STATUS_OK, "{\"unlocked\":true}");
}

esp_err_t OpenBrewByWeightNetwork::adminLockHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  self.unlockJsonBody();
  self.clearAdminUnlock();
  return sendJson(request, STATUS_OK, "{\"unlocked\":false}");
}
#endif

bool OpenBrewByWeightNetwork::requireActiveWebUiClient(httpd_req_t *request) {
  if (!webAccessSessionAllowed(request)) {
    sendError(request, STATUS_UNAUTHORIZED, "AUTH_REQUIRED",
              "Sign in with the device password.");
    return false;
  }
  char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
  if (!readWebUiClientId(request, clientId, sizeof(clientId))) {
    sendError(request, STATUS_CONFLICT, "UI_CLAIM_REQUIRED",
              "Claim WebUI control before using this API.");
    return false;
  }
  bool active = false;
  dataMux_.lock();
  active = activeWebUiClientId_[0] != '\0' &&
           strcmp(activeWebUiClientId_, clientId) == 0;
  // An accepted request also proves the page alive to its stream session.
  for (auto &session : uiStreams_)
    if (active && session.bound && strcmp(session.clientId, clientId) == 0)
      session.heardAtMs = millis();
  dataMux_.unlock();
  memset(clientId, 0, sizeof(clientId));
  if (!active) {
    sendError(request, STATUS_CONFLICT, "UI_TAKEN_OVER",
              "This WebUI window is inactive. Reactivate to continue.");
    return false;
  }
  char activity[3] = {};
  if (httpd_req_get_hdr_value_str(request, "X-WebUI-Activity", activity,
                                  sizeof(activity)) == ESP_OK) {
    char *end = nullptr;
    const unsigned long seconds = strtoul(activity, &end, 10);
    if (end != activity && *end == '\0' && seconds <= 30)
      notePowerWebActivity(millis(), static_cast<uint32_t>(seconds));
  }
  return true;
}

esp_err_t OpenBrewByWeightNetwork::ownedApiHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  struct Route {
    const char *uri = nullptr;
    httpd_method_t method = HTTP_GET;
    esp_err_t (*handler)(httpd_req_t *) = nullptr;
    bool setup = false;  // also served claim-less to SoftAP peers (/setup)
    bool open = false;   // served before any claim or sign-in (login page)
    bool diagnostic = false;  // also needs Admin or the public Diagnostic page
  };
  static const Route routes[] = {
      {"/api/v1/status/admin", HTTP_GET, statusHandler, true},
      {"/api/v1/debug/export", HTTP_GET, debugExportHandler},
      {"/api/v1/diagnostic/crashes", HTTP_GET, crashArchiveDownloadHandler},
      {"/api/v1/diagnostic/crashes/empty", HTTP_POST, crashArchiveClearHandler},
      {"/api/v1/diagnostic/audit/empty", HTTP_POST, auditLogClearHandler},
      {"/api/v1/diagnostic/audit", HTTP_GET, auditLogExportHandler},
      {"/api/v1/diagnostic/heap-map", HTTP_GET, heapMapDownloadHandler, false, false, true},
      {"/api/v1/diagnostic/audit/verbose", HTTP_POST, auditVerboseHandler, false, false,
       true},
      {"/api/v1/stats/clear", HTTP_POST, shotsClearHandler},
      {"/api/v1/stats/delete", HTTP_POST, shotsDeleteHandler},
      {"/api/v1/stats/rate", HTTP_POST, shotsRateHandler},
      {"/api/v1/history/clear", HTTP_POST, historyClearHandler},
      {"/api/v1/history/delete", HTTP_POST, historyDeleteHandler},
      {"/api/v1/last-shot/clear", HTTP_POST, lastShotClearHandler},
      {"/api/v1/time/sync", HTTP_POST, timeSyncHandler},
      {"/api/v1/time/zones", HTTP_GET, timeZoneCatalogHandler},
      {"/api/v1/time/preview", HTTP_POST, timeZonePreviewHandler},
      {"/api/v1/config", HTTP_POST, configHandler},
      {"/api/v1/webhooks", HTTP_POST, webhookHandler},
      {"/api/v1/machine/linea-micra", HTTP_POST, lineaMicraHandler},
      {"/api/v1/bullseye/test", HTTP_POST, bullseyeTestHandler},
      {"/api/v1/scale/preferred/clear", HTTP_POST, preferredScaleClearHandler},
      {"/api/v1/scale/preferred/select", HTTP_POST, preferredScaleSelectHandler},
      {"/api/v1/scale/friendly-name", HTTP_POST, scaleFriendlyNameHandler},
      {"/api/v1/presets", HTTP_POST, presetsHandler},
      {"/api/v1/calibration/reset", HTTP_POST, resetCalibrationHandler},
      {"/api/v1/calibration/reset-guard-samples", HTTP_POST,
       resetGuardSamplesHandler},
      {"/api/v1/control/paddle", HTTP_POST, paddleHandler},
      {"/api/v1/control/rinse", HTTP_POST, rinseHandler},
      {"/api/v1/control/stop", HTTP_POST, stopHandler},
      {"/api/v1/control/force-pulse", HTTP_POST, forcePulseHandler},
      {"/api/v1/control/state-override", HTTP_POST, stateOverrideHandler},
      {"/api/v1/control/restart", HTTP_POST, restartHandler},
      {"/api/v1/firmware-mode", HTTP_POST, firmwareModeHandler},
      {"/api/v1/diagnostic/reset-history", HTTP_POST, clearResetHistoryHandler,
       false, false, true},
      {"/api/v1/factory-reset", HTTP_POST, factoryResetHandler},
      {"/api/v1/network", HTTP_POST, networkHandler, true},
      {"/api/v1/network/scan", HTTP_GET, wifiScanStatusHandler, true},
      {"/api/v1/network/scan", HTTP_POST, wifiScanStartHandler, true},
      {"/api/v1/device/password", HTTP_POST, devicePasswordHandler},
      {"/api/v1/auth", HTTP_GET, authStatusHandler, false, true},
      {"/api/v1/auth/login", HTTP_POST, authLoginHandler, false, true},
      {"/api/v1/auth/logout", HTTP_POST, authLogoutHandler, false, true},
      {"/api/v1/auth/access", HTTP_POST, authAccessHandler},
      {"/api/v1/auth/notice", HTTP_POST, authNoticeHandler},
      {"/api/v1/auth/pairing", HTTP_POST, authPairingHandler},
#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1
// Unlock routes are not served in development builds; administration is
// public there and the UI never calls these endpoints.
#else
      {"/api/v1/admin/unlock", HTTP_POST, adminUnlockHandler},
      {"/api/v1/admin/lock", HTTP_POST, adminLockHandler},
#endif
      {"/api/v1/admin/ble-scan", HTTP_PUT, bleScanHandler},
      {"/api/v1/diagnostic/profiler", HTTP_POST, taskProfilerHandler, false, false, true},
      {"/api/v1/diagnostic/scale-profile", HTTP_POST, scaleProfileHandler, false, false,
       true},
      {"/api/v1/diagnostic/scale-profile/download", HTTP_GET,
       scaleProfileDownloadHandler},
      {"/api/v1/diagnostic/loop-max/reset", HTTP_POST, loopMaxResetHandler, false, false,
       true},
      {"/api/v1/diagnostic/scale-gap-max/reset", HTTP_POST, scaleGapMaxResetHandler,
       false, false, true},
      {"/api/v1/diagnostic/tare", HTTP_POST, scaleTareHandler, false, false, true},
  };
  const Route *matched = nullptr;
  for (const Route &route : routes) {
    if (request->method == route.method &&
        apiUriMatches(request->uri, route.uri)) {
      matched = &route;
      break;
    }
  }
  if (matched == nullptr) {
    return sendError(request, STATUS_NOT_FOUND, "NOT_FOUND",
                     "Unknown API route.");
  }
  if (matched->open) return matched->handler(request);
  // /setup holds no UI claim: joining the SoftAP already proved the device
  // password, the same trust as the admin bypass, so its own routes skip it.
  const bool setupPeer = matched->setup && self.snapshot().apActive &&
                         socketServedBySoftAp(httpd_req_to_sockfd(request));
  if (!setupPeer && !self.requireActiveWebUiClient(request)) {
    return ESP_OK;
  }
  if (matched->diagnostic && !self.diagnosticPageEnabled() &&
      !self.requireAdminUnlock(request)) {
    static RefusalLog log;
    if (const uint32_t count = log.due())
      self.lifecycleLogf("Refused %lu Diagnostic action(s): Admin is locked and the page is hidden",
                         static_cast<unsigned long>(count));
    return ESP_OK;
  }
  return matched->handler(request);
}

bool OpenBrewByWeightNetwork::requireJsonContentType(httpd_req_t *request) {
  const size_t length =
      httpd_req_get_hdr_value_len(request, "Content-Type");
  if (length == 0 || length >= 64) {
    return false;
  }
  char contentType[64] = {};
  if (httpd_req_get_hdr_value_str(request, "Content-Type", contentType,
                                   sizeof(contentType)) != ESP_OK) {
    return false;
  }
  const size_t jsonLength = strlen(JSON_CONTENT_TYPE);
  if (strncmp(contentType, JSON_CONTENT_TYPE, jsonLength) != 0) {
    return false;
  }
  const char suffix = contentType[jsonLength];
  return suffix == '\0' || suffix == ';' || suffix == ' ' || suffix == '\t';
}

bool OpenBrewByWeightNetwork::readJsonBody(
    httpd_req_t *request, char body[REQUEST_BODY_CAPACITY]) {
  if (!requireJsonContentType(request) || request->content_len <= 0 ||
      request->content_len >= REQUEST_BODY_CAPACITY) {
    return false;
  }
  // Callers hold the shared work buffer while the body arrives, so a
  // trickling peer gets one total deadline (plus at most one receive timeout).
  static RefusalLog slowBodies;
  const uint32_t startedAtMs = millis();
  size_t received = 0;
  while (received < static_cast<size_t>(request->content_len)) {
    if (millis() - startedAtMs >= 5000) {
      if (const uint32_t count = slowBodies.due())
        serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                             "http: refused %lu request body(s) that arrived too slowly",
                             static_cast<unsigned long>(count));
      return false;
    }
    const int result = httpd_req_recv(
        request, body + received,
        static_cast<size_t>(request->content_len) - received);
    if (result <= 0) {
      return false;
    }
    received += static_cast<size_t>(result);
  }
  body[received] = '\0';
  return true;
}

static bool ifNoneMatchEquals(httpd_req_t *request, const char *etag) {
  const size_t length = httpd_req_get_hdr_value_len(request, "If-None-Match");
  if (length == 0 || length >= IF_NONE_MATCH_CAPACITY) {
    return false;
  }
  char value[IF_NONE_MATCH_CAPACITY] = {};
  if (httpd_req_get_hdr_value_str(request, "If-None-Match", value,
                                  sizeof(value)) != ESP_OK) {
    return false;
  }
  return strcmp(value, etag) == 0;
}

// Generated URLs carry exactly ?v=<WEB_UI_ASSET_TAG>, the hash of this
// build's asset bytes; httpd matches handlers on the path alone.
static bool requestsCurrentAssetTag(httpd_req_t *request) {
  const char *query = strchr(request->uri, '?');
  return query != nullptr && strncmp(query, "?v=", 3) == 0 &&
         strcmp(query + 3, WEB_UI_ASSET_TAG) == 0;
}

static esp_err_t serveWebAsset(httpd_req_t *request, const WebAsset &asset) {
  // Only the URL naming these exact bytes may be cached as immutable. Any
  // other ?v= (a stale shell after OTA or rollback) or a bare probe path gets
  // the current bytes uncached, so no cache key ever holds a body it does
  // not name. The shell and assets keep the connection open so a page load
  // reuses its handshakes; the bound ui-stream refreshes its LRU slot, so
  // lru_purge evicts idle asset sockets first. API JSON and OTA responses
  // still close.
  const bool current = requestsCurrentAssetTag(request);
  const char *cacheControl =
      current ? "public, max-age=31536000, immutable" : "no-store";
  if (current && ifNoneMatchEquals(request, WEB_UI_ETAG)) {
    httpd_resp_set_status(request, STATUS_NOT_MODIFIED);
    httpd_resp_set_hdr(request, "Cache-Control", cacheControl);
    httpd_resp_set_hdr(request, "ETag", WEB_UI_ETAG);
    return httpd_resp_send(request, nullptr, 0);
  }
  httpd_resp_set_type(request, asset.contentType);
  if (asset.gzip) {
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
  }
  httpd_resp_set_hdr(request, "Cache-Control", cacheControl);
  if (current) {
    httpd_resp_set_hdr(request, "ETag", WEB_UI_ETAG);
  }
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  return sendBody(request, asset.data, asset.length);
}

esp_err_t OpenBrewByWeightNetwork::rootHandler(httpd_req_t *request) {
  // The shell stays cacheable so an installed home-screen app can open from
  // the browser cache while the controller is unreachable. The reliable grace
  // window is the fresh max-age period for a fresh open (verified on
  // Chromium); stale-if-error extends it only on engines that apply it to
  // navigations. Reload and the 304 path still revalidate against
  // WEB_UI_ETAG, and the UI navigates to a ?fw=<version> URL when the
  // status firmwareVersion differs from the baked build so the stale
  // fresh-cached copy cannot mask the new one.
  if (ifNoneMatchEquals(request, WEB_UI_ETAG)) {
    httpd_resp_set_status(request, STATUS_NOT_MODIFIED);
    httpd_resp_set_hdr(request, "Cache-Control",
                       "public, max-age=86400, stale-if-error=604800");
    httpd_resp_set_hdr(request, "ETag", WEB_UI_ETAG);
    return httpd_resp_send(request, nullptr, 0);
  }
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(request, "Cache-Control",
                     "public, max-age=86400, stale-if-error=604800");
  httpd_resp_set_hdr(request, "ETag", WEB_UI_ETAG);
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
  httpd_resp_set_hdr(
      request, "Content-Security-Policy",
      "default-src 'self'; script-src 'self'; style-src 'self'; "
      "connect-src 'self'; media-src data:; frame-ancestors 'none'");
  return sendBody(request, OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP,
                        OPEN_BREW_BY_WEIGHT_WEB_UI_GZIP_LEN);
}

// Every embedded asset except the shell is one row of the generated
// WEB_ASSETS table; a row ending in '*' prefix-matches its probe variants.
esp_err_t OpenBrewByWeightNetwork::assetHandler(httpd_req_t *request) {
  const size_t length = strcspn(request->uri, "?");
  for (const WebAsset &asset : WEB_ASSETS) {
    const size_t pathLength = strlen(asset.path);
    const bool prefix = asset.path[pathLength - 1] == '*';
    if (prefix ? strncmp(request->uri, asset.path, pathLength - 1) == 0
               : pathLength == length &&
                     strncmp(request->uri, asset.path, length) == 0) {
      return serveWebAsset(request, asset);
    }
  }
  return notFoundHandler(request, HTTPD_404_NOT_FOUND);
}

namespace {

// OS captive probes (frozen 2026-10): only these Hosts bounce to /setup, and
// only while the SoftAP is up, so ordinary typo paths keep the / redirect.
constexpr const char *const CAPTIVE_PROBE_HOSTS[] = {
    "captive.apple.com",
    "connectivitycheck.gstatic.com",
    "connectivitycheck.android.com",
    "clients3.google.com",
    "www.msftconnecttest.com",
    "www.msftncsi.com",
};

// Returns the matched probe host (nullptr for non-probe requests) so the
// portal-exit path can answer with the OS-specific success response.
const char *captiveProbeHost(httpd_req_t *request) {
  char host[64] = {};
  if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) !=
      ESP_OK) {
    return nullptr;
  }
  char *port = strchr(host, ':');
  if (port != nullptr) {
    *port = '\0';
  }
  for (const char *probe : CAPTIVE_PROBE_HOSTS) {
    if (strcasecmp(host, probe) == 0) {
      return probe;
    }
  }
  return nullptr;
}

// Portal-exit probe answers: iOS CNA dismisses on a 200 "Success" page,
// Android/Chrome on a bare 204, Windows NCSI on the 200 probe text body.
esp_err_t serveProbeSuccess(httpd_req_t *request, const char *host) {
  if (strcasecmp(host, "captive.apple.com") == 0) {
    httpd_resp_set_status(request, "200 OK");
    httpd_resp_set_type(request, "text/html");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request,
                           "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
                           "<BODY>Success</BODY></HTML>",
                           HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  if (strcasecmp(host, "www.msftconnecttest.com") != 0 &&
      strcasecmp(host, "www.msftncsi.com") != 0) {
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, nullptr, 0);
  }
  httpd_resp_set_status(request, "200 OK");
  httpd_resp_set_type(request, "text/plain");
  return httpd_resp_send(request,
                         strcasecmp(host, "www.msftncsi.com") == 0
                             ? "Microsoft NCSI"
                             : "Microsoft Connect Test",
                         HTTPD_RESP_USE_STRLEN);
}

// Host and stream-Origin refusals share one rate-limited Web UI log line.
void logRefusedHost(const char *name, const char *mdnsHost) {
  static RefusalLog log;
  if (const uint32_t count = log.due())
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "http: refused %lu request(s) for \"%.32s\"; use the IP or %.24s.local",
                         static_cast<unsigned long>(count), name, mdnsHost);
}

}  // namespace

esp_err_t OpenBrewByWeightNetwork::notFoundHandler(httpd_req_t *request,
                                              httpd_err_code_t) {
  // Keep API 404s as JSON; browser paths bounce to Home.
  if (strncmp(request->uri, "/api/", 5) == 0) {
    return sendError(request, STATUS_NOT_FOUND, "NOT_FOUND",
                     "The requested resource was not found.");
  }
  const char *probe =
      instance_->snapshot().apActive ? captiveProbeHost(request) : nullptr;
  if (probe != nullptr &&
      instance_->portalExitRequested_.load(std::memory_order_acquire)) {
    return serveProbeSuccess(request, probe);
  }
  if (probe != nullptr) {
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", CAPTIVE_PORTAL_URL);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, nullptr, 0);
  }
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location", "/");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, nullptr, 0);
}

bool OpenBrewByWeightNetwork::rejectForeignHost(httpd_req_t *request) {
  char host[96] = {};
  const size_t length = httpd_req_get_hdr_value_len(request, "Host");
  if (length == 0) return false;
  const NetworkStatusSnapshot network = instance_->snapshot();
  if (length < sizeof(host) &&
      httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) == ESP_OK &&
      (webAccessHostAllowed(host, network.mdnsHost) ||
       (network.apActive && captiveProbeHost(request) != nullptr)))
    return false;
  logRefusedHost(host, network.mdnsHost);
  if (strncmp(request->uri, "/api/", 5) == 0) {
    sendError(request, STATUS_FORBIDDEN, "HOST_NOT_ALLOWED",
              "Open the controller by its IP address or its .local name.");
    return true;
  }
  // Pages bounce to the address the browser actually reached.
  char location[24] = {};
  snprintf(location, sizeof(location), "http://%s/", network.iface.staIp);
  httpd_resp_set_status(request, "302 Found");
  httpd_resp_set_hdr(request, "Location",
                     instance_->softApPeer(request) ? CAPTIVE_PORTAL_URL : location);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_send(request, nullptr, 0);
  return true;
}

const char *OpenBrewByWeightNetwork::stateLabel(StopperState state) {
  const size_t index = static_cast<size_t>(state);
  return index < kStopperStateLabelCount
             ? kStopperStateLabels[index].humanLabel
             : "Unknown";
}

const char *OpenBrewByWeightNetwork::controlSourceName(ControlSource source) {
  switch (source) {
    case ControlSource::NONE: return "none";
    case ControlSource::PHYSICAL: return "physical";
    case ControlSource::WEB: return "web";
  }
  return "unknown";
}

const char *activeCycleShotTypeLabel(const ControlStatusSnapshot &control) {
  if (!control.activeCycle) {
    return "idle";
  }
  if (control.state == StopperState::RINSE) {
    return "rinse";
  }
  return shotLogTypeName(shotLogTypeFromCycle(
      control.state, control.cycleStartedWithScale, control.cycleTimerOnly,
      control.cycleAutomaticBrew));
}

const char *OpenBrewByWeightNetwork::endReasonName(EndReason reason) {
  const EndReasonLabel *label = findEndReasonLabel(reason);
  return label ? label->apiName : "UNKNOWN";
}

const char *OpenBrewByWeightNetwork::staStateName(StaState state) {
  switch (state) {
    case StaState::NOT_CONFIGURED: return "NOT_CONFIGURED";
    case StaState::CONNECTING: return "CONNECTING";
    case StaState::CONNECTED: return "CONNECTED";
    case StaState::FAILED: return "FAILED";
    case StaState::DISCONNECTED: return "DISCONNECTED";
  }
  return "UNKNOWN";
}

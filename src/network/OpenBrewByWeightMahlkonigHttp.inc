// Mahlkönig Grind-by-Sync over HTTP (docs/MAHLKONIG_GBS.md): /api/v2/* as an
// E64 WS polls it, and Settings' actions. The grinder's answers come from the
// network task's precomputed snapshot: no lock, no allocation, no flash, no
// log line per poll, also during a shot (an unanswered poll costs more
// airtime than an answer). A request is a signal only.

namespace openbrewbyweight {

esp_err_t OpenBrewByWeightNetwork::mahlkonigPullHandler(httpd_req_t *request) {
  if (!g_gbsCounters.serving.load(std::memory_order_acquire))
    return sendError(request, STATUS_NOT_FOUND, "NOT_FOUND", nullptr, "NOT_FOUND");
  char address[COMPANION_ADDRESS_TEXT];
  // The grinder sends no browser headers; a page that does is refused, as is
  // a peer outside the local network.
  if (!companionPeer(httpd_req_to_sockfd(request), address) ||
      httpd_req_get_hdr_value_len(request, "Origin") != 0 ||
      httpd_req_get_hdr_value_len(request, "Sec-Fetch-Site") != 0) {
    g_gbsCounters.refused.fetch_add(1, std::memory_order_relaxed);
    static RefusalLog refusals;
    if (const uint32_t count = refusals.due())
      lifecycleLogf("Mahlkönig: refused %lu grinder request(s) from a browser or another network",
                    static_cast<unsigned long>(count));
    return sendError(request, STATUS_FORBIDDEN, "MAHLKONIG_PEER", nullptr, "MAHLKONIG_PEER");
  }
  // /api/v2/<route>, query and a trailing slash ignored.
  const char *uri = request->uri + strlen("/api/v2");
  size_t length = strcspn(uri, "?");
  while (length > 0 && uri[length - 1] == '/') --length;
  const auto is = [&](const char *route) { return strlen(route) == length && strncmp(uri, route, length) == 0; };
  if (request->method == HTTP_GET) {
    char body[GBS_BODY_CAPACITY];
    const bool mako = is("/mako");
    const size_t n = mako ? g_gbsMako.read(body, sizeof(body) - 1)
                     : is("/machine") ? g_gbsMachine.read(body, sizeof(body) - 1)
                                      : 0;
    if (n == 0) memcpy(body, "{}", 3);
    else body[n] = '\0';
    if (mako) {
      g_gbsCounters.polls.fetch_add(1, std::memory_order_acq_rel);
      g_gbsCounters.seenAtMs.store(millis() | 1U, std::memory_order_release);
    }
    return sendJson(request, STATUS_OK, body);
  }
  // POST: a few dozen bytes; anything over the cap is not the grinder's.
  if (request->content_len > 1024) return sendError(request, "413 Payload Too Large", "PAYLOAD_TOO_LARGE",
                                                    nullptr, "MAHLKONIG_BODY_TOO_LARGE");
  char text[1025];
  size_t received = 0;
  while (received < request->content_len) {
    const int n = httpd_req_recv(request, text + received, request->content_len - received);
    if (n <= 0) return ESP_FAIL;
    received += static_cast<size_t>(n);
  }
  g_gbsCounters.posts.fetch_add(1, std::memory_order_relaxed);
  double value = 0;
  if (is("/scripts/execute") && gbsJsonNumber(text, received, "ID", value) && value == 9.0) {
    g_gbsCounters.executes.fetch_add(1, std::memory_order_acq_rel);
    if (instance_->taskHandle_ != nullptr) xTaskNotifyGive(instance_->taskHandle_);
  } else if (is("/brewratio") && gbsJsonNumber(text, received, "SYNC_BEVERAGE_WEIGHT", value) && value > 0 &&
             value < 1000) {
    g_gbsCounters.recipeDg.store(static_cast<uint32_t>(lround(value * 10)), std::memory_order_release);
  }
  return sendJson(request, STATUS_OK, "{}");
}

// POST /api/v1/mahlkonig from Settings: {"action":"pair","host":"…"},
// {"action":"pairing_id","pairingId":N} or {"action":"cancel"|"release"|
// "trust"|"forget"}. The network task applies it; pairing waits for the end
// of any shot.
esp_err_t OpenBrewByWeightNetwork::mahlkonigHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const esp_err_t bodyStatus = self.lockJsonBody(request, nullptr, "MAHLKONIG_ACTION_INVALID");
  if (bodyStatus != ESP_OK) return bodyStatus;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {"action", "pairingId", "host"};
  char action[16] = {}, host[GBS_HOST_CAPACITY] = {};
  uint32_t pairingId = 0;
  const bool parsed = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 3) &&
                      jsonString(root, "action", action, sizeof(action), false) &&
                      (strcmp(action, "pairing_id") == 0) == jsonFieldPresent(root, "pairingId") &&
                      (!jsonFieldPresent(root, "pairingId") || jsonUint32(root, "pairingId", pairingId));
  const bool hostSent = parsed && jsonFieldPresent(root, "host");
  const bool hostOk = hostSent && jsonString(root, "host", host, sizeof(host), false) && gbsHostValid(host);
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  static const struct {
    const char *name;
    GbsAction action;
  } actions[] = {{"pair", GbsAction::PAIR},       {"cancel", GbsAction::CANCEL},
                 {"pairing_id", GbsAction::PAIRING_ID}, {"release", GbsAction::RELEASE},
                 {"trust", GbsAction::TRUST},     {"forget", GbsAction::FORGET}};
  GbsAction chosen = GbsAction::NONE;
  for (const auto &entry : actions)
    if (parsed && strcmp(action, entry.name) == 0) chosen = entry.action;
  if (chosen == GbsAction::NONE)
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST", nullptr, "MAHLKONIG_ACTION_INVALID");
  if ((chosen == GbsAction::PAIR) != hostSent || (hostSent && !hostOk))
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST", nullptr, "MAHLKONIG_HOST_INVALID");
  if (chosen == GbsAction::PAIRING_ID && pairingId == 0)
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST", nullptr, "MAHLKONIG_PAIRING_ID_INVALID");
  bool enabled, pushReady;
  {
    TaskLockGuard lock(self.dataMux_);
    const RuntimeConfig &config = self.settings_.runtime;
    enabled = config.integrationMahlkonig && config.mahlkonigModel != GBS_MODEL_NONE;
    pushReady = gbsModelPush(config.mahlkonigModel);
    if (enabled && (pushReady || (chosen != GbsAction::PAIR && chosen != GbsAction::PAIRING_ID))) {
      g_gbsAction = chosen;
      g_gbsActionPairingId = pairingId;
      memcpy(g_gbsActionHost, host, sizeof(g_gbsActionHost));
    }
  }
  if (!enabled) return sendError(request, STATUS_CONFLICT, "MAHLKONIG_OFF", nullptr, "MAHLKONIG_OFF");
  if (!pushReady && (chosen == GbsAction::PAIR || chosen == GbsAction::PAIRING_ID))
    return sendError(request, STATUS_CONFLICT, "MAHLKONIG_NOT_READY", nullptr, "MAHLKONIG_NOT_READY");
  if (self.taskHandle_ != nullptr) xTaskNotifyGive(self.taskHandle_);
  return sendJson(request, STATUS_OK, "{\"ok\":true}");
}

}  // namespace openbrewbyweight

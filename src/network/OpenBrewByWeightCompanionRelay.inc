// Signed Web UI relay, controller side (docs/COMPANION_LINK.md#relay), every
// build: the handshake that derives Kr, the admission of each relayed request
// and the per-socket browser identity it records. httpd owns all of it;
// g_companion.relay and g_relayIdentities are guarded by dataMux_.

namespace openbrewbyweight {

// GET answers the challenge rb; POST proves the pairing secret over rb and the
// companion's nonce, and both sides derive Kr. Works without the link; rb
// rotates only after a successful POST.
esp_err_t OpenBrewByWeightNetwork::companionRelayHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  char address[COMPANION_ADDRESS_TEXT];
  if (!companionPeer(httpd_req_to_sockfd(request), address))
    return sendError(request, STATUS_FORBIDDEN, "COMPANION_PEER",
                     "Companions relay only from the local network.");
  uint8_t rb[COMPANION_NONCE_BYTES];
  char json[176] = {};
  size_t used = 0;
  if (request->method == HTTP_GET) {
    bool paired;
    {
      TaskLockGuard lock(self.dataMux_);
      CompanionRelay &relay = g_companion.relay;
      paired = g_companion.stored.paired != 0;
      if (paired && !relay.challengeSet) esp_fill_random(relay.challenge, sizeof(relay.challenge));
      relay.challengeSet |= paired;
      memcpy(rb, relay.challenge, sizeof(rb));
    }
    if (!paired)
      return sendError(request, STATUS_NOT_FOUND, "COMPANION_NOT_PAIRED",
                       "This controller is not paired with a companion.");
    used = static_cast<size_t>(snprintf(json, sizeof(json), "{\"challenge\":\""));
    (void)appendBase64(json, sizeof(json), used, rb, sizeof(rb));
    snprintf(json + used, sizeof(json) - used, "\",\"protocol\":%lu}",
             static_cast<unsigned long>(COMPANION_PROTOCOL));
    return sendJson(request, STATUS_OK, json);
  }
  if (!requireJsonContentType(request))
    return sendError(request, STATUS_UNSUPPORTED_MEDIA, "UNSUPPORTED_MEDIA_TYPE",
                     "Send the relay request as application/json.");
  const esp_err_t body = self.lockJsonBody(request, "A JSON request is required.");
  if (body != ESP_OK) return body;
  static const char *const fields[] = {"id", "nonce", "mac"};
  char id[COMPANION_ID_TEXT] = {}, nonceText[32] = {}, macText[48] = {};
  uint8_t nc[COMPANION_NONCE_BYTES], mac[32], expected[32];
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  const bool valid = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 3) &&
                     jsonString(root, "id", id, sizeof(id), false) && companionIdValid(id) &&
                     jsonString(root, "nonce", nonceText, sizeof(nonceText), false) &&
                     companionBase64Decode(nonceText, nc, sizeof(nc)) &&
                     jsonString(root, "mac", macText, sizeof(macText), false) &&
                     companionBase64Decode(macText, mac, sizeof(mac));
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!valid)
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST",
                     "The relay request fields are invalid.");
  uint8_t secret[COMPANION_SECRET_BYTES] = {};
  CompanionHello verdict;
  {
    TaskLockGuard lock(self.dataMux_);
    CompanionRelay &relay = g_companion.relay;
    verdict = companionHelloVerdict(g_companion.stored, id, COMPANION_PROTOCOL);
    if (verdict == CompanionHello::CHALLENGE && !relay.challengeSet) {
      esp_fill_random(relay.challenge, sizeof(relay.challenge));
      relay.challengeSet = true;
    }
    memcpy(rb, relay.challenge, sizeof(rb));
    memcpy(secret, verdict == CompanionHello::ERASE ? g_companion.stored.forgottenSecret
                                                    : g_companion.stored.secret,
           sizeof(secret));
  }
  if (verdict == CompanionHello::ERASE) {
    // A forgotten companion learns to delete its pairing, as on the link.
    const bool signedErase = companionHandshakeMac(secret, 'E', nc, nullptr, expected);
    memset(secret, 0, sizeof(secret));
    if (signedErase) {
      used = static_cast<size_t>(snprintf(
          json, sizeof(json),
          "{\"error\":\"COMPANION_NOT_PAIRED\",\"message\":\"This companion was forgotten.\","
          "\"erase\":\""));
      (void)appendBase64(json, sizeof(json), used, expected, sizeof(expected));
      snprintf(json + used, sizeof(json) - used, "\"}");
      return sendJson(request, STATUS_NOT_FOUND, json);
    }
  }
  if (verdict != CompanionHello::CHALLENGE) {
    memset(secret, 0, sizeof(secret));
    return sendError(request, STATUS_NOT_FOUND, "COMPANION_NOT_PAIRED",
                     "This controller is not paired with this companion.");
  }
  uint8_t key[32];
  const bool proven = companionHandshakeMac(secret, 'R', rb, nc, expected) &&
                      companionSame(mac, expected, sizeof(mac)) &&
                      companionHandshakeMac(secret, 'Q', rb, nc, mac) &&
                      companionSessionKey(secret, rb, nc, key, "obbw-relay-v1");
  memset(secret, 0, sizeof(secret));
  if (!proven) {
    memset(key, 0, sizeof(key));
    {
      TaskLockGuard lock(self.dataMux_);
      ++g_companion.authFailures;
    }
    static RefusalLog log;
    if (const uint32_t count = log.due())
      self.lifecycleLogf("Companion relay refused %lu handshake(s): authentication failed",
                         static_cast<unsigned long>(count));
    return sendError(request, STATUS_UNAUTHORIZED, "COMPANION_AUTH",
                     "The relay handshake did not authenticate.");
  }
  bool first, ui, link;
  {
    TaskLockGuard lock(self.dataMux_);
    CompanionRelay &relay = g_companion.relay;
    first = !relay.ready;
    memcpy(relay.key, key, sizeof(key));
    relay.ready = true;
    relay.window = CompanionRelayWindow{};
    ++relay.generation;
    esp_fill_random(relay.challenge, sizeof(relay.challenge));
    ui = g_companion.stored.uiOff == 0;
    // Q5: the link opens only for a feature that needs it.
    link = kCompanionLm && g_companion.stored.lmSwitch != 0;
  }
  memset(key, 0, sizeof(key));
  used = static_cast<size_t>(snprintf(json, sizeof(json), "{\"mac\":\""));
  (void)appendBase64(json, sizeof(json), used, mac, sizeof(mac));
  snprintf(json + used, sizeof(json) - used, "\",\"ui\":%s,\"link\":%s}", ui ? "true" : "false",
           link ? "true" : "false");
  if (first) self.lifecycleLogf("Companion relay ready at %s", address);
  return sendJson(request, STATUS_OK, json);
}

// Without X-OBBW-Relay the request is direct and only clears its socket's
// identity. With it: a local peer, a well-formed header, Kr's MAC over the
// request, a fresh q and the Web UI switch on; the socket's identity is then
// the header's browser until its next request. noinline: its frame is gone
// before the handler runs.
__attribute__((noinline)) bool OpenBrewByWeightNetwork::relayAdmit(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const int fd = httpd_req_to_sockfd(request);
  const size_t length = httpd_req_get_hdr_value_len(request, "X-OBBW-Relay");
  if (length == 0) {
    self.relayIdentityClear(fd);
    return true;
  }
  char text[COMPANION_RELAY_HEADER_TEXT] = {}, session[48] = {}, address[COMPANION_ADDRESS_TEXT];
  const size_t sessionLength = httpd_req_get_hdr_value_len(request, "X-WebUI-Session");
  CompanionRelayHeader header;
  const char *status = STATUS_BAD_REQUEST, *code = "RELAY_INVALID",
             *reason = "a malformed relay header";
  if (!companionPeer(fd, address)) {
    status = STATUS_FORBIDDEN;
    code = "COMPANION_PEER";
    reason = "a relay from outside the local network";
  } else if (length < sizeof(text) &&
             httpd_req_get_hdr_value_str(request, "X-OBBW-Relay", text, sizeof(text)) == ESP_OK &&
             companionRelayParse(text, header) && sessionLength < sizeof(session) &&
             (sessionLength == 0 || httpd_req_get_hdr_value_str(request, "X-WebUI-Session", session,
                                                                sizeof(session)) == ESP_OK)) {
    uint8_t key[32], mac[32];
    uint32_t generation;
    bool ready;
    {
      TaskLockGuard lock(self.dataMux_);
      ready = g_companion.relay.ready;
      generation = g_companion.relay.generation;
      memcpy(key, g_companion.relay.key, sizeof(key));
    }
    const bool signedOk =
        ready && companionRelayMac(key, http_method_str(static_cast<http_method>(request->method)),
                                   request->uri, header, session, mac) &&
        companionSame(mac, header.mac, COMPANION_RELAY_MAC_BYTES);
    memset(key, 0, sizeof(key));
    memset(session, 0, sizeof(session));
    TaskLockGuard lock(self.dataMux_);
    CompanionRelay &relay = g_companion.relay;
    status = STATUS_UNAUTHORIZED;
    code = "RELAY_STALE";
    reason = "a stale or unsigned relay";
    if (signedOk && relay.ready && relay.generation == generation) {
      if (!companionRelayAccept(relay.window, header.sequence)) {
        code = "RELAY_REPLAY";
        reason = "a replayed relay";
      } else if (g_companion.stored.uiOff != 0) {
        status = STATUS_FORBIDDEN;
        code = "RELAY_DISABLED";
        reason = "Web UI through the companion is off";
      } else {
        // This socket's entry, else a free one: closing sockets free theirs,
        // so httpd's seven sockets never need more.
        CompanionRelayIdentity *slot = nullptr;
        for (auto &entry : g_relayIdentities)
          if (entry.screen[0] != '\0' ? entry.fd == fd : slot == nullptr) slot = &entry;
        if (slot == nullptr) slot = &g_relayIdentities[0];
        else if (slot->screen[0] == '\0') self.relayIdentities_.fetch_add(1, std::memory_order_acq_rel);
        slot->fd = fd;
        snprintf(slot->screen, sizeof(slot->screen), "%s", header.screen);
        snprintf(slot->addr, sizeof(slot->addr), "%s", header.addr);
        return true;
      }
    }
  }
  {
    TaskLockGuard lock(self.dataMux_);
    ++g_companion.relay.rejects;
  }
  static RefusalLog log;
  if (const uint32_t count = log.due())
    self.lifecycleLogf("Companion relay refused %lu request(s): %s",
                       static_cast<unsigned long>(count), reason);
  sendError(request, status, code, "The companion's relay was refused.");
  return false;
}

void OpenBrewByWeightNetwork::relayIdentityClear(int fd) {
  if (relayIdentities_.load(std::memory_order_acquire) == 0) return;
  TaskLockGuard lock(dataMux_);
  for (auto &entry : g_relayIdentities) {
    if (entry.screen[0] == '\0' || entry.fd != fd) continue;
    entry = CompanionRelayIdentity{};
    relayIdentities_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

bool OpenBrewByWeightNetwork::relayIdentity(httpd_req_t *request,
                                            char (&screen)[COMPANION_RELAY_SCREEN_TEXT],
                                            char (&addr)[COMPANION_RELAY_ADDR_TEXT]) {
  if (relayIdentities_.load(std::memory_order_acquire) == 0) return false;
  const int fd = httpd_req_to_sockfd(request);
  TaskLockGuard lock(dataMux_);
  for (const auto &entry : g_relayIdentities) {
    if (entry.screen[0] == '\0' || entry.fd != fd) continue;
    memcpy(screen, entry.screen, sizeof(screen));
    memcpy(addr, entry.addr, sizeof(addr));
    return true;
  }
  return false;
}

bool OpenBrewByWeightNetwork::webUiPrincipal(httpd_req_t *request, CompanionPrincipal &principal) {
  char addr[COMPANION_RELAY_ADDR_TEXT];
  principal = CompanionPrincipal{};
  principal.relayed = relayIdentity(request, principal.id, addr);
  return principal.relayed || readWebUiClientId(request, principal.id, sizeof(principal.id));
}

}  // namespace openbrewbyweight

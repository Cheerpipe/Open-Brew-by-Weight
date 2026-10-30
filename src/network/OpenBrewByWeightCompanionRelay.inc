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
  uint8_t key[32], bodyKey[32];
  const bool proven = companionHandshakeMac(secret, 'R', rb, nc, expected) &&
                      companionSame(mac, expected, sizeof(mac)) &&
                      companionHandshakeMac(secret, 'Q', rb, nc, mac) &&
                      companionSessionKey(secret, rb, nc, key, "obbw-relay-v1") &&
                      companionBodyKey(key, bodyKey);
  memset(secret, 0, sizeof(secret));
  if (!proven) {
    memset(key, 0, sizeof(key));
    memset(bodyKey, 0, sizeof(bodyKey));
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
    memcpy(relay.bodyKey, bodyKey, sizeof(bodyKey));
    relay.ready = true;
    relay.window = CompanionRelayWindow{};
    ++relay.generation;
    esp_fill_random(relay.challenge, sizeof(relay.challenge));
    ui = g_companion.stored.uiOff == 0;
    // Q5: the link opens only for a feature that needs it: the La Marzocco
    // route (Linea Micra builds) or the diagnostic log (every build, 2B).
    link = (kCompanionLm && g_companion.stored.lmSwitch != 0) ||
           g_companion.stored.logSwitch != 0;
  }
  memset(key, 0, sizeof(key));
  memset(bodyKey, 0, sizeof(bodyKey));
  used = static_cast<size_t>(snprintf(json, sizeof(json), "{\"mac\":\""));
  (void)appendBase64(json, sizeof(json), used, mac, sizeof(mac));
  snprintf(json + used, sizeof(json) - used, "\",\"ui\":%s,\"link\":%s}", ui ? "true" : "false",
           link ? "true" : "false");
  if (first) self.lifecycleLogf("Companion relay ready at %s", address);
  return sendJson(request, STATUS_OK, json);
}

// Without X-OBBW-Relay the request is direct and only clears its socket's
// identity. With it: a local peer, a well-formed header, Kr's MAC over the
// request, a fresh q and the Web UI switch on; the seals on the session and
// device-password values open under Kb, and the socket's identity becomes the
// header's browser until its next request, with the opened credentials staged
// for this request's handlers. noinline: its frame is gone before the handler
// runs.
__attribute__((noinline)) bool OpenBrewByWeightNetwork::relayAdmit(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const int fd = httpd_req_to_sockfd(request);
  const size_t length = httpd_req_get_hdr_value_len(request, "X-OBBW-Relay");
  if (length == 0) {
    self.relayIdentityClear(fd);
    return true;
  }
  char text[COMPANION_RELAY_HEADER_TEXT] = {},
       session[COMPANION_RELAY_SECRET_TEXT] = {}, address[COMPANION_ADDRESS_TEXT];
  const size_t sessionLength = httpd_req_get_hdr_value_len(request, "X-WebUI-Session");
  size_t passwordLength = httpd_req_get_hdr_value_len(request, "X-Device-Password");
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
    char password[COMPANION_RELAY_SECRET_TEXT] = {};
    if (passwordLength >= sizeof(password) ||
        (passwordLength > 0 &&
         httpd_req_get_hdr_value_str(request, "X-Device-Password", password,
                                     sizeof(password)) != ESP_OK))
      passwordLength = sizeof(password);  // unreadable: refuse below
    uint8_t key[32], bodyKey[32], mac[32];
    uint32_t generation;
    bool ready;
    {
      TaskLockGuard lock(self.dataMux_);
      ready = g_companion.relay.ready;
      generation = g_companion.relay.generation;
      memcpy(key, g_companion.relay.key, sizeof(key));
      memcpy(bodyKey, g_companion.relay.bodyKey, sizeof(bodyKey));
    }
    // The seals open outside the lock; what they carry belongs to this
    // request's q, which the replay window admits exactly once. The key that
    // opened them stays with the request: an OTA job may run after a newer
    // relay handshake, and its body and answer still belong to these keys.
    char sessionText[WEB_ACCESS_TOKEN_TEXT] = {}, passwordText[WIFI_PASSWORD_CAPACITY] = {};
    uint8_t sessionToken[WEB_ACCESS_TOKEN_BYTES], admitted[32];
    memcpy(admitted, bodyKey, sizeof(admitted));
    const bool sessionSealed = sessionLength > 0, passwordSealed = passwordLength > 0;
    const bool signatureOk =
        ready && companionRelayMac(key, http_method_str(static_cast<http_method>(request->method)),
                                   request->uri, header, session, mac) &&
        companionSame(mac, header.mac, COMPANION_RELAY_MAC_BYTES);
    const bool sealsOk =
        !signatureOk ||
        ((!sessionSealed ||
          (companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_SESSION, header.sequence,
                               session, sessionText, sizeof(sessionText)) &&
           parseWebAccessToken(sessionText, sessionToken))) &&
         (!passwordSealed ||
          companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_PASSWORD, header.sequence,
                              password, passwordText, sizeof(passwordText))));
    memset(key, 0, sizeof(key));
    memset(bodyKey, 0, sizeof(bodyKey));
    memset(session, 0, sizeof(session));
    TaskLockGuard lock(self.dataMux_);
    CompanionRelay &relay = g_companion.relay;
    status = STATUS_UNAUTHORIZED;
    code = "RELAY_STALE";
    reason = "a stale or unsigned relay";
    if (signatureOk && relay.ready && relay.generation == generation) {
      if (!sealsOk) {
        status = STATUS_BAD_REQUEST;
        code = "RELAY_INVALID";
        reason = "a seal that did not open";
      } else if (!companionRelayAccept(relay.window, header.sequence)) {
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
        *slot = CompanionRelayIdentity{};
        slot->fd = fd;
        snprintf(slot->screen, sizeof(slot->screen), "%s", header.screen);
        snprintf(slot->addr, sizeof(slot->addr), "%s", header.addr);
        slot->sequence = header.sequence;
        memcpy(slot->bodyKey, admitted, sizeof(slot->bodyKey));
        if (sessionSealed) {
          slot->sessionValid = true;
          memcpy(slot->session, sessionToken, sizeof(slot->session));
        }
        if (passwordSealed) {
          slot->passwordValid = true;
          snprintf(slot->password, sizeof(slot->password), "%s", passwordText);
        }
        memset(sessionToken, 0, sizeof(sessionToken));
        memset(passwordText, 0, sizeof(passwordText));
        memset(admitted, 0, sizeof(admitted));
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

// What the admitted request's seals opened, staged per socket. relayIdentity
// above names the screen; these carry the credentials only within the
// request whose q sealed them.

bool OpenBrewByWeightNetwork::relaySessionToken(httpd_req_t *request,
                                                uint8_t token[WEB_ACCESS_TOKEN_BYTES],
                                                bool &relayed) {
  relayed = false;
  if (relayIdentities_.load(std::memory_order_acquire) == 0) return false;
  TaskLockGuard lock(dataMux_);
  for (const auto &entry : g_relayIdentities) {
    if (entry.screen[0] == '\0' || entry.fd != httpd_req_to_sockfd(request)) continue;
    relayed = true;
    if (!entry.sessionValid) return false;
    memcpy(token, entry.session, WEB_ACCESS_TOKEN_BYTES);
    return true;
  }
  return false;
}

bool OpenBrewByWeightNetwork::relayPassword(httpd_req_t *request, char *out,
                                            size_t capacity) {
  if (relayIdentities_.load(std::memory_order_acquire) == 0) return false;
  TaskLockGuard lock(dataMux_);
  for (const auto &entry : g_relayIdentities) {
    if (entry.screen[0] == '\0' || entry.fd != httpd_req_to_sockfd(request)) continue;
    if (entry.passwordValid) snprintf(out, capacity, "%s", entry.password);
    else out[0] = '\0';
    return true;
  }
  return false;
}

bool OpenBrewByWeightNetwork::relayBodyContext(httpd_req_t *request, uint8_t key[32],
                                               uint32_t &sequence) {
  if (relayIdentities_.load(std::memory_order_acquire) == 0) return false;
  TaskLockGuard lock(dataMux_);
  for (const auto &entry : g_relayIdentities) {
    if (entry.screen[0] == '\0' || entry.fd != httpd_req_to_sockfd(request)) continue;
    memcpy(key, entry.bodyKey, 32);  // the session that admitted this request
    sequence = entry.sequence;
    return true;
  }
  return false;
}

// One sealed response: the admitting session's Kb and the next response
// counter, which never repeats within a boot (see CompanionRelay).
bool OpenBrewByWeightNetwork::relayResponseKey(httpd_req_t *request, uint8_t key[32],
                                               uint32_t &counter) {
  TaskLockGuard lock(dataMux_);
  for (const auto &entry : g_relayIdentities) {
    if (entry.screen[0] == '\0' || entry.fd != httpd_req_to_sockfd(request)) continue;
    memcpy(key, entry.bodyKey, 32);
    counter = ++g_companion.relay.responses;
    return true;
  }
  return false;
}

// One sealed JSON response: X-OBBW-Enc names the counter it was sealed
// under. A seal that cannot be made is never sent opened.
esp_err_t OpenBrewByWeightNetwork::relaySendSealed(httpd_req_t *request,
                                                   const char *json, size_t length) {
  uint8_t key[32], nonce[12];
  uint32_t counter = 0;
  uint8_t *const cipher = static_cast<uint8_t *>(malloc(length + 16));
  const bool keyed = cipher != nullptr && relayResponseKey(request, key, counter);
  size_t sealedLength = 0;
  if (keyed) companionRelayNonce('f', COMPANION_RELAY_CTX_RESPONSE, counter, nonce);
  const bool sealed =
      keyed && companionAeadSealBytes(key, nonce,
                                      reinterpret_cast<const uint8_t *>(json), length,
                                      cipher, length + 16, sealedLength);
  memset(key, 0, sizeof(key));
  if (!sealed) {
    free(cipher);
    httpd_resp_set_status(request, STATUS_SERVER_ERROR);
    return sendBody(request, nullptr, 0);
  }
  char counterText[12];
  snprintf(counterText, sizeof(counterText), "%lu",
           static_cast<unsigned long>(counter));
  httpd_resp_set_hdr(request, "X-OBBW-Enc", counterText);
  const esp_err_t sent = sendBody(request, cipher, sealedLength);
  free(cipher);
  return sent;
}

// A body whose seal did not open: the same accounting as a refused relay.
esp_err_t OpenBrewByWeightNetwork::relayEnvelopeReject(httpd_req_t *request) {
  {
    TaskLockGuard lock(dataMux_);
    ++g_companion.relay.rejects;
  }
  static RefusalLog log;
  if (const uint32_t count = log.due())
    lifecycleLogf("Companion relay refused %lu request(s): a body seal that did not open",
                  static_cast<unsigned long>(count));
  return sendError(request, STATUS_BAD_REQUEST, "RELAY_INVALID",
                   "The companion's relay was refused.");
}

}  // namespace openbrewbyweight

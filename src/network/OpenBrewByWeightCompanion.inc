// Companion link, controller side (docs/COMPANION_LINK.md), every build; the
// La Marzocco route only on Linea Micra builds. httpd owns the link sockets,
// the frame codec, every send and the relay; the network task owns the
// request key pair, the pairing derivation, the NVS write, liveness and the
// outbox. g_companion is guarded by dataMux_. Until a companion asks, nothing
// here runs, connects, writes or logs: the controller only answers, and it
// never connects to the companion.

namespace openbrewbyweight {
namespace {

#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
constexpr bool kCompanionLm = true;
#else
// Without La Marzocco the route stays direct and the outbox empty, so the
// link shares one path and carries only the switches.
constexpr bool kCompanionLm = false;
MicraCompanionRouteStatus machineIntegrationCompanionRouteStatus() { return {}; }
MicraCompanionOutbox machineIntegrationCompanionOutbox() { return {}; }
MicraCompanionIngress machineIntegrationCompanionIngress() { return {}; }
void machineIntegrationCompanionLink(bool, bool) {}
#endif

// Network task only: the open request's key pair and the request it serves.
mbedtls_svc_key_id_t g_companionRequestKey = MBEDTLS_SVC_KEY_ID_INIT;
uint32_t g_companionRequestSerial = 0;
// Network task → httpd: unsigned frame bodies, NUL-separated; httpd owns it
// while companionSendPending_ is set.
struct CompanionSendJob {
  uint32_t generation;
  uint8_t count;
  uint8_t retries;  // clean aborts httpd deferred to the network task
  char bodies[COMPANION_FRAME_BYTES];
};
constexpr size_t kCompanionPlainBytes = 1408;  // every field escaped at worst
// A frame httpd could not start writing goes again from the next network pass,
// at most this often, before the link closes.
constexpr uint8_t kCompanionSendRetries = 5;
// Allocated from PSRAM by the first link handshake that claims a slot (only a
// paired or forgotten controller) and kept, so a controller that never pairs
// reserves nothing. frame: httpd only, one received frame, then each signed
// send. crypt: network task only, credentials plaintext, then ciphertext;
// wiped after use.
struct CompanionWorkspace {
  char frame[COMPANION_FRAME_BYTES + 1];
  CompanionSendJob job;
  uint8_t crypt[2 * kCompanionPlainBytes + 16];
};
std::atomic<CompanionWorkspace *> g_companionWorkspace{nullptr};
// Valid on every path after a handshake, the only place that allocates it.
CompanionWorkspace &companionWs() { return *g_companionWorkspace.load(std::memory_order_acquire); }
// A frame's JSON nests to d.widgets[].output.status; keep httpd recursion short.
constexpr size_t kCompanionFrameDepth = 8;
constexpr const char *STATUS_UNSUPPORTED_MEDIA = "415 Unsupported Media Type";

mbedtls_svc_key_id_t companionImportKey(const uint8_t *bytes, size_t size,
                                        psa_key_type_t type, psa_algorithm_t algorithm,
                                        psa_key_usage_t usage) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, type);
  psa_set_key_usage_flags(&attributes, usage);
  psa_set_key_algorithm(&attributes, algorithm);
  mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
  if (psa_import_key(&attributes, bytes, size, &key) != PSA_SUCCESS) key = MBEDTLS_SVC_KEY_ID_INIT;
  psa_reset_key_attributes(&attributes);
  return key;
}

// Import validates that the point lies on P-256; nothing is kept.
bool companionPublicKeyValid(const uint8_t key[COMPANION_KEY_BYTES]) {
  const mbedtls_svc_key_id_t id = companionImportKey(
      key, COMPANION_KEY_BYTES, PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1),
      PSA_ALG_ECDH, 0);
  if (mbedtls_svc_key_id_is_null(id)) return false;
  (void)psa_destroy_key(id);
  return true;
}

bool companionGenerateKey(mbedtls_svc_key_id_t &key, uint8_t publicKey[COMPANION_KEY_BYTES]) {
  psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&attributes, PSA_ALG_ECDH);
  size_t length = 0;
  const bool made = psa_generate_key(&attributes, &key) == PSA_SUCCESS &&
                    psa_export_public_key(key, publicKey, COMPANION_KEY_BYTES, &length) ==
                        PSA_SUCCESS &&
                    length == COMPANION_KEY_BYTES;
  psa_reset_key_attributes(&attributes);
  if (!made && !mbedtls_svc_key_id_is_null(key)) (void)psa_destroy_key(key);
  if (!made) key = MBEDTLS_SVC_KEY_ID_INIT;
  return made;
}

void companionDropRequestKey() {
  if (!mbedtls_svc_key_id_is_null(g_companionRequestKey)) (void)psa_destroy_key(g_companionRequestKey);
  g_companionRequestKey = MBEDTLS_SVC_KEY_ID_INIT;
}

// The credentials frame's sealing (Linea Micra builds).
[[maybe_unused]] bool companionAeadSeal(const uint8_t key[32], const uint8_t iv[12],
                                        const char *aad, const uint8_t *plain, size_t plainSize,
                                        uint8_t *out, size_t capacity, size_t &length) {
  const mbedtls_svc_key_id_t id =
      companionImportKey(key, 32, PSA_KEY_TYPE_AES, PSA_ALG_GCM, PSA_KEY_USAGE_ENCRYPT);
  if (mbedtls_svc_key_id_is_null(id)) return false;
  const bool sealed =
      psa_aead_encrypt(id, PSA_ALG_GCM, iv, 12, reinterpret_cast<const uint8_t *>(aad),
                       strlen(aad), plain, plainSize, out, capacity, &length) == PSA_SUCCESS;
  (void)psa_destroy_key(id);
  return sealed;
}

// The peer in text; false unless it is private, link-local or loopback.
bool companionPeer(int fd, char address[COMPANION_ADDRESS_TEXT]) {
  sockaddr_storage peer = {};
  socklen_t length = sizeof(peer);
  address[0] = '\0';
  if (getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &length) != 0) return false;
  if (peer.ss_family == AF_INET) {
    const in_addr &v4 = reinterpret_cast<const sockaddr_in &>(peer).sin_addr;
    (void)inet_ntop(AF_INET, &v4, address, COMPANION_ADDRESS_TEXT);
    return companionPeerAllowedV4(ntohl(v4.s_addr));
  }
  if (peer.ss_family != AF_INET6) return false;
  const in6_addr &v6 = reinterpret_cast<const sockaddr_in6 &>(peer).sin6_addr;
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&v6);
  static const uint8_t kV4Mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
  if (memcmp(bytes, kV4Mapped, sizeof(kV4Mapped)) == 0)
    (void)inet_ntop(AF_INET, bytes + 12, address, COMPANION_ADDRESS_TEXT);
  else (void)inet_ntop(AF_INET6, &v6, address, COMPANION_ADDRESS_TEXT);
  return companionPeerAllowedV6(bytes);
}

esp_err_t companionSendText(httpd_req_t *request, const char *text) {
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.final = true;
  frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
  frame.len = strlen(text);
  uiStreamSendKind = "companion";
  const esp_err_t result = httpd_ws_send_frame(request, &frame);
  uiStreamSendKind = "control";
  return result;
}

bool __attribute__((format(printf, 2, 3)))
companionAddBody(uint8_t &count, const char *format, ...) {
  size_t used = 0;
  for (uint8_t index = 0; index < count; ++index)
    used += strlen(companionWs().job.bodies + used) + 1;
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(companionWs().job.bodies + used, sizeof(companionWs().job.bodies) - used,
                               format, args);
  va_end(args);
  // Each body must also fit a signed frame: MAC, separator and sequence.
  if (length < 0 || used + static_cast<size_t>(length) >= sizeof(companionWs().job.bodies) ||
      static_cast<size_t>(length) + COMPANION_MAC_HEX + 20 > COMPANION_FRAME_BYTES) {
    companionWs().job.bodies[used] = '\0';
    return false;
  }
  ++count;
  return true;
}

const char *companionRouteName(MicraCompanionRoute route) {
  return route == MicraCompanionRoute::COMPANION ? "companion"
         : route == MicraCompanionRoute::PENDING ? "pending" : "direct";
}

[[maybe_unused]] const char *companionCommandName(MicraCompanionCommandOp op) {
  switch (op) {
    case MicraCompanionCommandOp::TEMPERATURE: return "temp";
    case MicraCompanionCommandOp::STANDBY: return "standby";
    case MicraCompanionCommandOp::POWER_ON: return "on";
    case MicraCompanionCommandOp::REFRESH: return "refresh";
    default: return nullptr;
  }
}

const CompanionLinkSlot *companionActiveLink() {
  for (const auto &slot : g_companion.links)
    if (slot.fd >= 0 && slot.phase == CompanionLinkPhase::READY &&
        slot.generation == g_companion.active)
      return &slot;
  return nullptr;
}

}  // namespace

// --- Hash primitives of OpenBrewByWeightCompanionLink.h (PSA) ----------------

bool companionSha256(const CompanionBytes *parts, size_t count, uint8_t out[32]) {
  psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
  bool ok = psa_hash_setup(&operation, PSA_ALG_SHA_256) == PSA_SUCCESS;
  for (size_t index = 0; ok && index < count; ++index)
    ok = psa_hash_update(&operation, static_cast<const uint8_t *>(parts[index].data),
                         parts[index].size) == PSA_SUCCESS;
  size_t length = 0;
  ok = ok && psa_hash_finish(&operation, out, 32, &length) == PSA_SUCCESS && length == 32;
  (void)psa_hash_abort(&operation);
  return ok;
}

bool companionHmacSha256(const uint8_t *key, size_t keySize, const CompanionBytes *parts,
                         size_t count, uint8_t out[32]) {
  const psa_algorithm_t algorithm = PSA_ALG_HMAC(PSA_ALG_SHA_256);
  const mbedtls_svc_key_id_t id =
      companionImportKey(key, keySize, PSA_KEY_TYPE_HMAC, algorithm, PSA_KEY_USAGE_SIGN_MESSAGE);
  if (mbedtls_svc_key_id_is_null(id)) return false;
  psa_mac_operation_t operation = PSA_MAC_OPERATION_INIT;
  bool ok = psa_mac_sign_setup(&operation, id, algorithm) == PSA_SUCCESS;
  for (size_t index = 0; ok && index < count; ++index)
    ok = psa_mac_update(&operation, static_cast<const uint8_t *>(parts[index].data),
                        parts[index].size) == PSA_SUCCESS;
  size_t length = 0;
  ok = ok && psa_mac_sign_finish(&operation, out, 32, &length) == PSA_SUCCESS && length == 32;
  (void)psa_mac_abort(&operation);
  (void)psa_destroy_key(id);
  return ok;
}

bool companionHkdfSha256(const uint8_t *ikm, size_t ikmSize, const uint8_t *salt,
                         size_t saltSize, const char *info, uint8_t out[32]) {
  psa_key_derivation_operation_t operation = PSA_KEY_DERIVATION_OPERATION_INIT;
  // An absent salt is HKDF's empty salt.
  const bool ok =
      psa_key_derivation_setup(&operation, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
      (saltSize == 0 || psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SALT,
                                                       salt, saltSize) == PSA_SUCCESS) &&
      psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_SECRET, ikm,
                                     ikmSize) == PSA_SUCCESS &&
      psa_key_derivation_input_bytes(&operation, PSA_KEY_DERIVATION_INPUT_INFO,
                                     reinterpret_cast<const uint8_t *>(info),
                                     strlen(info)) == PSA_SUCCESS &&
      psa_key_derivation_output_bytes(&operation, out, 32) == PSA_SUCCESS;
  (void)psa_key_derivation_abort(&operation);
  return ok;
}

// Relay protocol 2's seals: AES-256-GCM, no additional data, the tag riding
// with the bytes (16 appended on seal, expected on open).
bool companionAeadSealBytes(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *plain, size_t plainSize, uint8_t *out,
                            size_t capacity, size_t &length) {
  const mbedtls_svc_key_id_t id =
      companionImportKey(key, 32, PSA_KEY_TYPE_AES, PSA_ALG_GCM, PSA_KEY_USAGE_ENCRYPT);
  if (mbedtls_svc_key_id_is_null(id)) return false;
  const bool sealed = psa_aead_encrypt(id, PSA_ALG_GCM, nonce, 12, nullptr, 0, plain,
                                       plainSize, out, capacity, &length) == PSA_SUCCESS;
  (void)psa_destroy_key(id);
  return sealed;
}

bool companionAeadOpenBytes(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *sealed, size_t sealedSize, uint8_t *out,
                            size_t capacity, size_t &length) {
  const mbedtls_svc_key_id_t id =
      companionImportKey(key, 32, PSA_KEY_TYPE_AES, PSA_ALG_GCM, PSA_KEY_USAGE_DECRYPT);
  if (mbedtls_svc_key_id_is_null(id)) return false;
  const bool opened = psa_aead_decrypt(id, PSA_ALG_GCM, nonce, 12, nullptr, 0, sealed,
                                       sealedSize, out, capacity, &length) == PSA_SUCCESS;
  (void)psa_destroy_key(id);
  return opened;
}

// --- Lifecycle ------------------------------------------------------------------

void OpenBrewByWeightNetwork::beginCompanion() {
  CompanionStored stored;
  const CompanionLoad loaded = loadCompanionStored(stored);
  if (loaded == CompanionLoad::INVALID)
    lifecycleLog("Companion pairing unreadable; pair the companion again");
  g_companion = CompanionState{};
  g_companion.stored = stored;
  // A version 1 pairing keeps working; its next switch change writes v2.
  g_companion.dirty = loaded == CompanionLoad::PROMOTED;
  companionAwake_.store(stored.paired != 0 || stored.forgotten != 0, std::memory_order_release);
  memset(&stored, 0, sizeof(stored));
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  setMachineIntegrationCompanionNotify(companionOutboxChanged);
#endif
}

// The durable reset erased the record; drop the RAM copy and any link.
void OpenBrewByWeightNetwork::resetCompanion() {
  int fds[2] = {-1, -1};
  {
    TaskLockGuard lock(dataMux_);
    for (size_t index = 0; index < 2; ++index) {
      CompanionLinkSlot &slot = g_companion.links[index];
      if (slot.fd < 0 || slot.closing) continue;
      slot.closing = true;
      slot.closeReason = "factory reset";
      fds[index] = slot.fd;
    }
    g_companion.stored = CompanionStored{};
    g_companion.pairing = CompanionPairing{};
    companionRelayDrop(g_companion.relay);
    g_companion.dirty = false;
  }
  for (const int fd : fds)
    if (fd >= 0 && server_ != nullptr) (void)httpd_sess_trigger_close(server_, fd);
}

void OpenBrewByWeightNetwork::companionOutboxChanged() {
  OpenBrewByWeightNetwork *self = instance_;
  if (self == nullptr) return;
  self->companionNotified_.store(true, std::memory_order_release);
  // Without a link the next session sends the whole snapshot anyway.
  if (self->companionLinkUp_.load(std::memory_order_acquire) && self->taskHandle_ != nullptr)
    xTaskNotifyGive(self->taskHandle_);
}

// --- Pairing routes ------------------------------------------------------------

esp_err_t OpenBrewByWeightNetwork::companionPairHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  char address[COMPANION_ADDRESS_TEXT];
  if (!companionPeer(httpd_req_to_sockfd(request), address))
    return sendError(request, STATUS_FORBIDDEN, "COMPANION_PEER",
                     "Companions pair only from the local network.");
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
      return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "The id is invalid.");
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
                     "Send the pairing request as application/json.");
  const esp_err_t body = self.lockJsonBody(request, "A JSON request is required.");
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
                     "The pairing request fields are invalid.");
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
                     "The key does not match an open pairing request.");
  switch (result) {
    case CompanionPairResult::IGNORED:
      return sendError(request, STATUS_FORBIDDEN, "COMPANION_IGNORED",
                       "This companion was ignored on this controller.");
    case CompanionPairResult::ALREADY_PAIRED:
      return sendError(request, STATUS_CONFLICT, "ALREADY_PAIRED",
                       "This controller is already paired with a companion.");
    case CompanionPairResult::BUSY:
      return sendError(request, STATUS_CONFLICT, "PAIRING_BUSY",
                       "Another companion's request is waiting.");
    case CompanionPairResult::RATE_LIMITED:
      return sendError(request, STATUS_TOO_MANY, "PAIRING_RATE_LIMITED",
                       "Wait a few seconds before asking again.");
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
  const esp_err_t body = self.lockJsonBody(request, "A JSON request is required.");
  if (body != ESP_OK) return body;
  static const char *const fields[] = {"action", "code", "lm", "ui", "log"};
  char action[12] = {}, code[8] = {};
  bool lm = true, ui = true, log = true;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  bool valid = root != nullptr && jsonHasOnlyUniqueFields(root, fields, 5) &&
               jsonString(root, "action", action, sizeof(action), false);
  if (valid && jsonFieldPresent(root, "code")) valid = jsonString(root, "code", code, sizeof(code), false);
  const bool hasLm = valid && jsonFieldPresent(root, "lm");
  if (hasLm) valid = jsonBoolean(root, "lm", lm);
  const bool hasUi = valid && jsonFieldPresent(root, "ui");
  if (hasUi) valid = jsonBoolean(root, "ui", ui);
  const bool hasLog = valid && jsonFieldPresent(root, "log");
  if (hasLog) valid = jsonBoolean(root, "log", log);
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!valid)
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "The request fields are invalid.");
  const uint32_t now = millis();
  char name[COMPANION_NAME_TEXT] = {};
  if (!strcmp(action, "approve")) {
    bool digits = strlen(code) == 6;
    for (size_t index = 0; digits && index < 6; ++index) digits = code[index] >= '0' && code[index] <= '9';
    if (!digits)
      return sendError(request, STATUS_UNPROCESSABLE, "COMPANION_CODE",
                       "Enter the six-digit code the companion shows.");
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
                                   expected, lm, log, now)
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
                         "That is not the code the companion shows.");
      case CompanionApproveResult::DENIED:
        self.lifecycleLogf("Companion pairing request from '%s' denied after three wrong codes", name);
        return sendError(request, STATUS_UNPROCESSABLE, "COMPANION_CODE",
                         "Three wrong codes: the request was denied.");
      case CompanionApproveResult::NOT_READY:
        return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_READY",
                         "The controller is still preparing this request.");
      default:
        return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_PENDING",
                         "No companion request is waiting.");
    }
  }
  if (!strcmp(action, "ignore")) {
    bool ignored;
    {
      TaskLockGuard lock(self.dataMux_);
      ignored = companionPairIgnore(g_companion.pairing, now);
      memcpy(name, g_companion.pairing.request.name, sizeof(name));
    }
    if (!ignored)
      return sendError(request, STATUS_CONFLICT, "PAIRING_NOT_PENDING",
                       "No companion request is waiting.");
    self.lifecycleLogf("Companion pairing request from '%s' ignored", name);
  } else if (!strcmp(action, "unignore")) {
    {
      TaskLockGuard lock(self.dataMux_);
      companionUnignore(g_companion.pairing);
    }
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
                       "This controller is not paired with a companion.");
    for (const int fd : fds)
      if (fd >= 0) (void)httpd_sess_trigger_close(request->handle, fd);
    self.lifecycleLogf("Companion '%s' forgotten", name);
  } else if (!strcmp(action, "switches") && (hasLm || hasUi || hasLog) &&
             (kCompanionLm || !hasLm)) {
    bool paired, lmChanged = false, uiChanged = false, logChanged = false, linkUp = false;
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
        g_companion.dirty |= lmChanged || uiChanged || logChanged;
        linkUp = g_companion.active != 0;
        // Its next relayed request is stale, and the new handshake carries ui and link.
        if (lmChanged || uiChanged || logChanged) companionRelayDrop(g_companion.relay);
      }
    }
    if (!paired)
      return sendError(request, STATUS_CONFLICT, "COMPANION_NOT_PAIRED",
                       "This controller is not paired with a companion.");
    if (lmChanged) {
      machineIntegrationCompanionLink(linkUp, lm);
      self.lifecycleLog(lm ? "Companion: La Marzocco through the companion turned on"
                           : "Companion: La Marzocco through the companion turned off");
    }
    if (uiChanged) {
      // An open link learns it from the next cfg frame.
      companionOutboxChanged();
      self.lifecycleLog(ui ? "Companion: Web UI through the companion turned on"
                           : "Companion: Web UI through the companion turned off");
    }
    if (logChanged) {
      // An open link learns it from the next cfg frame; the relay handshake
      // re-answers whether the link is needed at all.
      companionOutboxChanged();
      self.lifecycleLog(log ? "Companion: Diagnostic log through the companion turned on"
                            : "Companion: Diagnostic log through the companion turned off");
    }
  } else {
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", "Unknown companion action.");
  }
  return sendJson(request, STATUS_OK, "{\"ok\":true}");
}

// --- Link socket ------------------------------------------------------------------

// Pre-handshake: Host allowlist, a local peer, and a pairing (or a forgotten
// one, which still receives `erase`). Not the ui-stream Origin check: the
// companion is not a browser page, and the pairing secret authenticates it.
esp_err_t OpenBrewByWeightNetwork::companionLinkHandshake(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (rejectForeignHost(request)) return ESP_FAIL;
  const int fd = httpd_req_to_sockfd(request);
  char address[COMPANION_ADDRESS_TEXT];
  if (!companionPeer(fd, address)) {
    static RefusalLog peer;
    if (const uint32_t count = peer.due())
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "companion link: refused %lu handshake(s) from outside the local network",
                           static_cast<unsigned long>(count));
    sendError(request, STATUS_FORBIDDEN, "COMPANION_PEER",
              "Companions connect only from the local network.");
    return ESP_FAIL;
  }
  CompanionLinkSlot *slot = nullptr;
  bool known;
  {
    TaskLockGuard lock(self.dataMux_);
    known = g_companion.stored.paired != 0 || g_companion.stored.forgotten != 0;
    for (auto &candidate : g_companion.links) {
      if (!known || candidate.fd >= 0) continue;
      candidate = CompanionLinkSlot{};
      candidate.fd = fd;
      candidate.openedAtMs = candidate.heardAtMs = millis();
      slot = &candidate;
      break;
    }
  }
  if (!known) {
    static RefusalLog unpaired;
    if (const uint32_t count = unpaired.due())
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "companion link: refused %lu handshake(s): not paired",
                           static_cast<unsigned long>(count));
    sendError(request, STATUS_NOT_FOUND, "COMPANION_NOT_PAIRED",
              "This controller is not paired with a companion.");
    return ESP_FAIL;
  }
  if (slot == nullptr) {
    static RefusalLog busy;
    if (const uint32_t count = busy.due())
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "companion link: refused %lu handshake(s): another link is being opened",
                           static_cast<unsigned long>(count));
    sendError(request, STATUS_CONFLICT, "COMPANION_BUSY",
              "Another companion link is being opened.");
    return ESP_FAIL;
  }
  // httpd is the only allocator, so this check and store cannot race.
  if (g_companionWorkspace.load(std::memory_order_acquire) == nullptr) {
    auto *workspace = static_cast<CompanionWorkspace *>(
        allocExternal(sizeof(CompanionWorkspace), AllocationOwner::NETWORK));
    if (workspace == nullptr) {
      {
        TaskLockGuard lock(self.dataMux_);
        *slot = CompanionLinkSlot{};
      }
      static RefusalLog memory;
      if (const uint32_t count = memory.due())
        serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                             "companion link: refused %lu handshake(s): no memory for a link",
                             static_cast<unsigned long>(count));
      sendError(request, STATUS_UNAVAILABLE, "COMPANION_MEMORY",
                "The controller has no memory for a companion link right now.");
      return ESP_FAIL;
    }
    memset(workspace, 0, sizeof(*workspace));
    g_companionWorkspace.store(workspace, std::memory_order_release);
  }
  request->sess_ctx = slot;
  request->free_ctx = companionLinkFree;
  // Bound incomplete frames, and send each frame without waiting for an ACK.
  const timeval timeout = {0, 100000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const int noDelay = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
  return ESP_OK;
}

void OpenBrewByWeightNetwork::companionLinkFree(void *context) {
  if (instance_ == nullptr || context == nullptr) return;
  OpenBrewByWeightNetwork &self = *instance_;
  auto &slot = *static_cast<CompanionLinkSlot *>(context);
  bool down, lm;
  const char *reason;
  {
    TaskLockGuard lock(self.dataMux_);
    down = slot.phase == CompanionLinkPhase::READY && slot.generation == g_companion.active;
    if (down) {
      g_companion.active = 0;
      g_companion.changedAtMs = millis() | 1U;
    }
    reason = slot.closeReason != nullptr ? slot.closeReason : "connection closed";
    lm = g_companion.stored.lmSwitch != 0;
    slot = CompanionLinkSlot{};
  }
  if (!down) return;
  self.companionLinkUp_.store(false, std::memory_order_release);
  machineIntegrationCompanionLink(false, lm);
  self.lifecycleLogf("Companion link down: %s", reason);
}

// Closes the socket (the caller returns ESP_FAIL). An authenticated link logs
// its reason when it goes down; refusals before that share one rate-limited line.
esp_err_t OpenBrewByWeightNetwork::companionLinkRefuse(CompanionLinkSlot &slot,
                                                       const char *reason,
                                                       bool authentication) {
  bool ready;
  {
    TaskLockGuard lock(dataMux_);
    slot.closing = true;
    slot.closeReason = reason;
    ready = slot.phase == CompanionLinkPhase::READY;
    if (authentication) ++g_companion.authFailures;
  }
  static RefusalLog log;
  if (!ready || authentication)
    if (const uint32_t count = log.due())
      lifecycleLogf("Companion link refused %lu time(s): %s", static_cast<unsigned long>(count), reason);
  return ESP_FAIL;
}

esp_err_t OpenBrewByWeightNetwork::companionLinkHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  auto *slot = static_cast<CompanionLinkSlot *>(request->sess_ctx);
  if (slot == nullptr) return ESP_FAIL;
  httpd_ws_frame_t frame = {};
  if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK || frame.type != HTTPD_WS_TYPE_TEXT ||
      !frame.final || frame.len == 0 || frame.len > COMPANION_FRAME_BYTES)
    return self.companionLinkRefuse(*slot, "a frame was not one text message of at most 4 KB", false);
  frame.payload = reinterpret_cast<uint8_t *>(companionWs().frame);
  if (httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK ||
      memchr(companionWs().frame, '\0', frame.len) != nullptr)
    return self.companionLinkRefuse(*slot, "a frame could not be read", false);
  companionWs().frame[frame.len] = '\0';
  const int fd = httpd_req_to_sockfd(request);
  // Every frame proves the link alive to the LRU purge and to liveness.
  httpd_sess_update_lru_counter(request->handle, fd);
  httpd_sess_set_send_override(request->handle, fd, uiStreamSend);
  return self.companionLinkFrame(request, *slot, frame.len);
}

esp_err_t OpenBrewByWeightNetwork::companionLinkFrame(httpd_req_t *request,
                                                      CompanionLinkSlot &slot, size_t length) {
  const uint32_t now = millis();
  CompanionLinkPhase phase;
  uint8_t key[32];
  {
    TaskLockGuard lock(dataMux_);
    slot.heardAtMs = now;
    phase = slot.phase;
    memcpy(key, slot.key, sizeof(key));
  }
  // A state frame's power intent is its arrival, as on the direct socket.
  const MicraCompanionIngress ingress = phase == CompanionLinkPhase::READY
                                            ? machineIntegrationCompanionIngress()
                                            : MicraCompanionIngress{};
  const char *json = companionWs().frame;
  if (phase == CompanionLinkPhase::READY) json = companionOpenFrame(key, companionWs().frame, length);
  memset(key, 0, sizeof(key));
  if (json == nullptr) return companionLinkRefuse(slot, "a frame signature did not match", true);
  cJSON *frame = parseJsonDocumentWithinLimits(json, COMPANION_FRAME_BYTES,
                                               JSON_DOCUMENT_MAX_VALUES, kCompanionFrameDepth);
  if (frame == nullptr) return companionLinkRefuse(slot, "a frame was not valid JSON", false);
  const cJSON *typeItem = cJSON_GetObjectItemCaseSensitive(frame, "t");
  const char *type = cJSON_IsString(typeItem) ? typeItem->valuestring : "";
  const esp_err_t result = phase == CompanionLinkPhase::HELLO   ? companionLinkHello(request, slot, frame, type)
                           : phase == CompanionLinkPhase::PROOF ? companionLinkProof(request, slot, frame, type)
                                                                : companionLinkInbound(slot, frame, type, ingress);
  cJSON_Delete(frame);
  return result;
}

esp_err_t OpenBrewByWeightNetwork::companionLinkHello(httpd_req_t *request,
                                                      CompanionLinkSlot &slot, cJSON *frame,
                                                      const char *type) {
  char id[COMPANION_ID_TEXT] = {}, name[COMPANION_NAME_TEXT] = {},
       version[COMPANION_VERSION_TEXT] = {}, nonce[32] = {};
  uint8_t nc[COMPANION_NONCE_BYTES];
  const cJSON *protocol = cJSON_GetObjectItemCaseSensitive(frame, "protocol");
  if (strcmp(type, "hello") != 0 || !jsonString(frame, "id", id, sizeof(id), false) ||
      !companionIdValid(id) || !jsonString(frame, "name", name, sizeof(name), false) ||
      !companionTextValid(name, 1, COMPANION_NAME_TEXT - 1, false) ||
      !jsonString(frame, "version", version, sizeof(version), true) ||
      !companionTextValid(version, 0, COMPANION_VERSION_TEXT - 1, true) ||
      !cJSON_IsNumber(protocol) || !jsonString(frame, "nonce", nonce, sizeof(nonce), false) ||
      !companionBase64Decode(nonce, nc, sizeof(nc)))
    return companionLinkRefuse(slot, "the opening hello was malformed", false);
  uint8_t secret[COMPANION_SECRET_BYTES] = {};
  CompanionHello verdict;
  {
    TaskLockGuard lock(dataMux_);
    verdict = companionHelloVerdict(g_companion.stored, id, protocol->valuedouble);
    if (verdict == CompanionHello::ERASE)
      memcpy(secret, g_companion.stored.forgottenSecret, sizeof(secret));
    // Admin and Diagnostic say which side needs an update.
    const double asked = protocol->valuedouble;
    if (verdict == CompanionHello::PROTOCOL)
      g_companion.peerProtocol = asked >= 0 && asked <= UINT16_MAX &&
                                         asked == static_cast<double>(static_cast<uint32_t>(asked))
                                     ? static_cast<uint32_t>(asked)
                                     : 0;
  }
  // Like the Micra socket's admission: no new link during a scaled shot or
  // a scale connection. The companion retries with its backoff.
  if (verdict == CompanionHello::CHALLENGE &&
      (outboundScaleInhibited() || scaleConnecting_.load(std::memory_order_acquire) ||
       (outboundScaleConnected.load(std::memory_order_acquire) &&
        outboundShotActive.load(std::memory_order_acquire)))) {
    (void)companionSendText(request, "{\"t\":\"error\",\"code\":\"RETRY_LATER\"}");
    TaskLockGuard lock(dataMux_);
    slot.closeReason = "retry later";
    return ESP_FAIL;
  }
  char reply[80] = {};
  size_t used = 0;
  if (verdict == CompanionHello::CHALLENGE) {
    uint8_t ns[COMPANION_NONCE_BYTES];
    esp_fill_random(ns, sizeof(ns));
    {
      TaskLockGuard lock(dataMux_);
      memcpy(slot.nc, nc, sizeof(nc));
      memcpy(slot.ns, ns, sizeof(ns));
      copyWebAccessLabel(slot.name, name);
      copyWebAccessLabel(slot.version, version);
      slot.protocol = static_cast<uint8_t>(COMPANION_PROTOCOL);
      slot.phase = CompanionLinkPhase::PROOF;
    }
    used = static_cast<size_t>(snprintf(reply, sizeof(reply), "{\"t\":\"challenge\",\"nonce\":\""));
    (void)appendBase64(reply, sizeof(reply), used, ns, sizeof(ns));
    snprintf(reply + used, sizeof(reply) - used, "\"}");
    return companionSendText(request, reply) == ESP_OK
               ? ESP_OK
               : companionLinkRefuse(slot, "the challenge could not be sent", false);
  }
  if (verdict == CompanionHello::ERASE) {
    uint8_t mac[32];
    const bool signedErase = companionHandshakeMac(secret, 'E', nc, nullptr, mac);
    memset(secret, 0, sizeof(secret));
    if (!signedErase) return companionLinkRefuse(slot, "the erase could not be signed", false);
    used = static_cast<size_t>(snprintf(reply, sizeof(reply), "{\"t\":\"erase\",\"mac\":\""));
    (void)appendBase64(reply, sizeof(reply), used, mac, sizeof(mac));
    snprintf(reply + used, sizeof(reply) - used, "\"}");
    (void)companionSendText(request, reply);
    return companionLinkRefuse(slot, "a forgotten companion was told to erase its pairing", false);
  }
  const bool protocolMismatch = verdict == CompanionHello::PROTOCOL;
  (void)companionSendText(request, protocolMismatch ? "{\"t\":\"error\",\"code\":\"PROTOCOL\"}"
                                                    : "{\"t\":\"error\",\"code\":\"NOT_PAIRED\"}");
  return companionLinkRefuse(slot, protocolMismatch ? "unsupported protocol version"
                                                    : "unknown companion", false);
}

esp_err_t OpenBrewByWeightNetwork::companionLinkProof(httpd_req_t *request,
                                                      CompanionLinkSlot &slot, cJSON *frame,
                                                      const char *type) {
  char macText[48] = {};
  uint8_t mac[32], expected[32], secret[COMPANION_SECRET_BYTES], nc[COMPANION_NONCE_BYTES],
      ns[COMPANION_NONCE_BYTES], key[32];
  if (strcmp(type, "proof") != 0 || !jsonString(frame, "mac", macText, sizeof(macText), false) ||
      !companionBase64Decode(macText, mac, sizeof(mac)))
    return companionLinkRefuse(slot, "the proof was malformed", true);
  bool paired;
  {
    TaskLockGuard lock(dataMux_);
    paired = g_companion.stored.paired != 0;
    memcpy(secret, g_companion.stored.secret, sizeof(secret));
    memcpy(nc, slot.nc, sizeof(nc));
    memcpy(ns, slot.ns, sizeof(ns));
  }
  const bool proven = paired && companionHandshakeMac(secret, 'C', nc, ns, expected) &&
                      companionSame(mac, expected, sizeof(mac)) &&
                      companionSessionKey(secret, nc, ns, key) &&
                      companionHandshakeMac(secret, 'F', nc, ns, mac);
  memset(secret, 0, sizeof(secret));
  if (!proven) {
    memset(key, 0, sizeof(key));
    return companionLinkRefuse(slot, "authentication failed", true);
  }
  char address[COMPANION_ADDRESS_TEXT];
  (void)companionPeer(slot.fd, address);
  char name[COMPANION_NAME_TEXT];
  int replaced = -1;
  bool lm;
  {
    TaskLockGuard lock(dataMux_);
    for (auto &other : g_companion.links) {
      if (&other == &slot || other.fd < 0 || other.phase != CompanionLinkPhase::READY) continue;
      other.closing = true;
      other.closeReason = "replaced by a new link";
      replaced = other.fd;
    }
    slot.phase = CompanionLinkPhase::READY;
    memcpy(slot.key, key, sizeof(key));
    slot.inSequence = slot.outSequence = 0;
    slot.readyAtMs = slot.heardAtMs = millis();
    slot.generation = ++g_companion.generation;
    g_companion.active = slot.generation;
    ++g_companion.ups;
    g_companion.changedAtMs = slot.readyAtMs | 1U;
    g_companion.peerProtocol = 0;
    // A new peer address, name or version is kept only once proven.
    CompanionStored &stored = g_companion.stored;
    if (strcmp(stored.address, address) != 0 || strcmp(stored.name, slot.name) != 0 ||
        strcmp(stored.companionVersion, slot.version) != 0) {
      snprintf(stored.address, sizeof(stored.address), "%s", address);
      memcpy(stored.name, slot.name, sizeof(stored.name));
      memcpy(stored.companionVersion, slot.version, sizeof(stored.companionVersion));
      g_companion.dirty = true;
    }
    memcpy(name, stored.name, sizeof(name));
    lm = stored.lmSwitch != 0;
  }
  memset(key, 0, sizeof(key));
  char reply[80] = {};
  size_t used = static_cast<size_t>(snprintf(reply, sizeof(reply), "{\"t\":\"ready\",\"mac\":\""));
  (void)appendBase64(reply, sizeof(reply), used, mac, sizeof(mac));
  snprintf(reply + used, sizeof(reply) - used, "\"}");
  if (companionSendText(request, reply) != ESP_OK)
    return companionLinkRefuse(slot, "the ready frame could not be sent", false);
  if (replaced >= 0) (void)httpd_sess_trigger_close(request->handle, replaced);
  companionLinkUp_.store(true, std::memory_order_release);
  machineIntegrationCompanionLink(true, lm);
  // The network task sends the whole outbox snapshot to the new session.
  companionNotified_.store(true, std::memory_order_release);
  if (taskHandle_ != nullptr) xTaskNotifyGive(taskHandle_);
  lifecycleLogf("Companion link up: '%s' at %s", name, address);
  return ESP_OK;
}

esp_err_t OpenBrewByWeightNetwork::companionLinkInbound(
    CompanionLinkSlot &slot, cJSON *frame, const char *type,
    [[maybe_unused]] MicraCompanionIngress ingress) {
  uint32_t expected;
  {
    TaskLockGuard lock(dataMux_);
    expected = slot.inSequence + 1;
  }
  if (!companionFrameSequence(frame, expected))
    return companionLinkRefuse(slot, "a frame arrived out of sequence", true);
  {
    TaskLockGuard lock(dataMux_);
    slot.inSequence = expected;
    // Every valid frame is a piggyback heartbeat for the log stream (2B).
    g_companion.logHeard = true;
  }
  // A type this controller does not know is a later addition: ignored, so a
  // newer companion keeps its link. Known types must be well formed; the La
  // Marzocco ones exist only on Linea Micra builds.
  if (strcmp(type, "ping") != 0 && strcmp(type, "logsub") != 0 &&
      (!kCompanionLm || (strcmp(type, "lm") != 0 && strcmp(type, "creds") != 0 &&
                         strcmp(type, "res") != 0 && strcmp(type, "st") != 0))) {
    static RefusalLog unknown;
    if (const uint32_t count = unknown.due())
      lifecycleLogf("Companion link ignored %lu frame(s) of a type this controller does not know",
                    static_cast<unsigned long>(count));
    return ESP_OK;
  }
  // The La Marzocco frames use the rest only on Linea Micra builds.
  [[maybe_unused]] bool flag = false, flag2 = false;
  [[maybe_unused]] uint32_t first = 0, second = 0, third = 0;
  [[maybe_unused]] char text[12] = {};
  if (!strcmp(type, "ping") && jsonBoolean(frame, "up", flag)) {
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
    machineIntegrationCompanionPing(flag);
#endif
  } else if (!strcmp(type, "logsub")) {
    if (!companionLinkLogSub(slot, frame))
      return companionLinkRefuse(slot, "a malformed frame arrived", false);
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  } else if (!strcmp(type, "lm") && jsonBoolean(frame, "sub", flag) &&
             jsonUint32(frame, "ep", first) && jsonUint32(frame, "tok", second)) {
    machineIntegrationCompanionUpstream(flag, first, second,
                                        jsonString(frame, "err", text, sizeof(text), false) &&
                                            !strcmp(text, "auth"));
  } else if (!strcmp(type, "creds") && jsonUint32(frame, "gen", first) &&
             jsonBoolean(frame, "ok", flag)) {
    machineIntegrationCompanionCredentials(first, flag);
  } else if (!strcmp(type, "res") && jsonUint32(frame, "id", first) &&
             jsonString(frame, "r", text, sizeof(text), false) &&
             ((flag = !strcmp(text, "accepted")) || !strcmp(text, "failed")) &&
             (!jsonFieldPresent(frame, "http") ||
              ((flag2 = jsonUint32(frame, "http", second)) && second <= UINT16_MAX))) {
    machineIntegrationCompanionCommandResult(first, flag, static_cast<uint16_t>(flag2 ? second : 0));
  } else if (!strcmp(type, "st") && jsonString(frame, "k", text, sizeof(text), false) &&
             (!strcmp(text, "full") || !strcmp(text, "push") || !strcmp(text, "read")) &&
             jsonUint32(frame, "ep", first) &&
             // A push takes its intent at ingress; reads order by `ac`.
             (!strcmp(text, "push") || jsonUint32(frame, "ac", third)) &&
             cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(frame, "d"))) {
    const MicraCompanionStateKind kind = !strcmp(text, "full")   ? MicraCompanionStateKind::FULL
                                         : !strcmp(text, "push") ? MicraCompanionStateKind::PUSH
                                                                 : MicraCompanionStateKind::READ;
    if (!machineIntegrationCompanionState(kind, first, third,
                                          cJSON_GetObjectItemCaseSensitive(frame, "d"), ingress))
      return companionLinkRefuse(slot, "a machine state push was rejected", false);
#endif
  } else {
    return companionLinkRefuse(slot, "a malformed frame arrived", false);
  }
  return ESP_OK;
}

// --- Network task --------------------------------------------------------------

void OpenBrewByWeightNetwork::serviceCompanion(uint32_t now) {
  // A controller that never paired has nothing to service: not even the lock.
  if (!companionAwake_.load(std::memory_order_acquire)) return;
  uint32_t serial = 0, makeKey = 0, derive = 0;
  bool pending = false, linkOpen = false, dirty = false;
  uint8_t companionKey[COMPANION_KEY_BYTES], controllerKey[COMPANION_KEY_BYTES];
  {
    TaskLockGuard lock(dataMux_);
    CompanionRequest &request = g_companion.pairing.request;
    companionPairTick(g_companion.pairing, now);
    pending = request.state == CompanionPairState::PENDING;
    serial = request.serial;
    if (pending && !request.keyReady && g_companionRequestSerial != serial) makeKey = serial;
    if (pending && request.approving) {
      derive = serial;
      memcpy(companionKey, request.companionKey, sizeof(companionKey));
      memcpy(controllerKey, request.controllerKey, sizeof(controllerKey));
    }
    for (const auto &slot : g_companion.links) linkOpen |= slot.fd >= 0;
    dirty = g_companion.dirty;
  }
  // The request key lives exactly as long as its request.
  if ((!pending || serial != g_companionRequestSerial) && makeKey == 0)
    companionDropRequestKey();
  if (makeKey != 0) {
    companionDropRequestKey();
    uint8_t publicKey[COMPANION_KEY_BYTES];
    const bool made = companionGenerateKey(g_companionRequestKey, publicKey);
    g_companionRequestSerial = makeKey;
    bool kept = false;
    {
      TaskLockGuard lock(dataMux_);
      CompanionRequest &request = g_companion.pairing.request;
      if (request.serial == makeKey && request.state == CompanionPairState::PENDING) {
        if (made) memcpy(request.controllerKey, publicKey, sizeof(publicKey));
        request.keyReady = kept = made;
        if (!made) request.state = CompanionPairState::DENIED;
      }
    }
    if (!kept) companionDropRequestKey();
    if (!made) lifecycleLog("Companion pairing request failed: the controller could not make its key");
  }
  if (derive != 0) {
    uint8_t shared[32], secret[COMPANION_SECRET_BYTES];
    size_t length = 0;
    const bool derived =
        derive == g_companionRequestSerial && !mbedtls_svc_key_id_is_null(g_companionRequestKey) &&
        psa_raw_key_agreement(PSA_ALG_ECDH, g_companionRequestKey, companionKey,
                              COMPANION_KEY_BYTES, shared, sizeof(shared), &length) == PSA_SUCCESS &&
        length == sizeof(shared) &&
        companionPairingSecret(shared, companionKey, controllerKey, secret);
    companionDropRequestKey();
    char name[COMPANION_NAME_TEXT] = {};
    {
      TaskLockGuard lock(dataMux_);
      CompanionRequest &request = g_companion.pairing.request;
      if (request.serial == derive && request.approving) {
        memcpy(name, request.name, sizeof(name));
        if (derived) {
          companionPairComplete(g_companion.pairing, g_companion.stored, secret);
          g_companion.dirty = true;
        } else {
          request.approving = false;
          request.state = CompanionPairState::DENIED;
        }
      }
    }
    memset(shared, 0, sizeof(shared));
    memset(secret, 0, sizeof(secret));
    if (name[0] != '\0')
      lifecycleLogf(derived ? "Companion '%s' paired" : "Companion '%s' could not be paired", name);
  }
  if (linkOpen && server_ != nullptr) {
    const MicraCompanionRoute route = machineIntegrationCompanionRouteStatus().route;
    int silent[2] = {-1, -1};
    {
      TaskLockGuard lock(dataMux_);
      for (size_t index = 0; index < 2; ++index) {
        CompanionLinkSlot &slot = g_companion.links[index];
        if (slot.fd < 0 || slot.closing ||
            now - slot.heardAtMs < companionSilenceLimitMs(slot, route))
          continue;
        slot.closing = true;
        slot.closeReason = "the companion went silent";
        silent[index] = slot.fd;
      }
    }
    for (const int fd : silent)
      if (fd >= 0) (void)httpd_sess_trigger_close(server_, fd);
    // As often as ui-stream's dispatch, keep the link the most recently used
    // session, so httpd's LRU purge evicts an idle asset socket instead.
    if (now - companionLruAtMs_ >= kUiStreamFallbackMs &&
        !companionLruPending_.exchange(true, std::memory_order_acq_rel)) {
      companionLruAtMs_ = now;
      if (httpd_queue_work(server_, companionLruWork, this) != ESP_OK)
        companionLruPending_.store(false, std::memory_order_release);
    }
    // Diagnostic log (2B): the due check runs every pass, so a hold's end
    // flushes at once instead of waiting for the next ping.
    companionLogService();
    if (companionNotified_.load(std::memory_order_acquire) &&
        !companionSendPending_.load(std::memory_order_acquire))
      companionPrepareSends();
  }
  // httpd deferred a send it could not start writing: it goes again from here
  // (and is dropped there if its link closed meanwhile).
  if (server_ != nullptr && companionSendRetry_.load(std::memory_order_acquire)) {
    companionSendRetry_.store(false, std::memory_order_release);
    if (httpd_queue_work(server_, companionSendWork, this) != ESP_OK)
      companionSendRetry_.store(true, std::memory_order_release);
  }
  if (dirty && (companionRetryAtMs_ == 0 || static_cast<int32_t>(now - companionRetryAtMs_) >= 0) &&
      networkFlashWriteAllowed()) {
    CompanionStored stored;
    {
      TaskLockGuard lock(dataMux_);
      stored = g_companion.stored;
      g_companion.dirty = false;
    }
    const bool saved = saveCompanionStored(stored);
    memset(&stored, 0, sizeof(stored));
    if (saved) {
      companionRetryAtMs_ = 0;
    } else {
      {
        TaskLockGuard lock(dataMux_);
        g_companion.dirty = true;
      }
      companionRetryAtMs_ = (now + 30000U) | 1U;
      lifecycleLog("Companion pairing could not be saved; retrying");
    }
  }
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
// The account for the creds frame, sealed for the companion and wiped here.
bool OpenBrewByWeightNetwork::companionCredentialsBody(uint8_t &count, uint32_t generation,
                                                       const uint8_t secret[COMPANION_SECRET_BYTES]) {
  char email[LINEA_MICRA_USERNAME_CAPACITY] = {}, password[LINEA_MICRA_PASSWORD_CAPACITY] = {},
       serial[LINEA_MICRA_SERIAL_CAPACITY] = {};
  bool ok = machineIntegrationCompanionCopyCredentials(generation, email, sizeof(email), password,
                                                       sizeof(password), serial, sizeof(serial));
  char *plain = reinterpret_cast<char *>(companionWs().crypt);
  uint8_t *sealed = companionWs().crypt + kCompanionPlainBytes;
  size_t used = 0;
  const auto put = [&](const char *text, bool escape) {
    if (!ok) return;
    ok = escape ? escapeJsonString(text, plain + used, kCompanionPlainBytes - used)
                : snprintf(plain + used, kCompanionPlainBytes - used, "%s", text) <
                      static_cast<int>(kCompanionPlainBytes - used);
    if (ok) used += strlen(plain + used);
  };
  put("{\"email\":\"", false);
  put(email, true);
  put("\",\"password\":\"", false);
  put(password, true);
  put("\",\"serial\":\"", false);
  put(serial, true);
  put("\"}", false);
  memset(email, 0, sizeof(email));
  memset(password, 0, sizeof(password));
  uint8_t key[32], iv[12];
  char aad[24], ivText[20] = {};
  size_t sealedLength = 0, ivUsed = 0;
  esp_fill_random(iv, sizeof(iv));
  snprintf(aad, sizeof(aad), "obbw-creds|%lu", static_cast<unsigned long>(generation));
  ok = ok && companionCredentialsKey(secret, key) &&
       companionAeadSeal(key, iv, aad, companionWs().crypt, used, sealed,
                         sizeof(companionWs().crypt) - kCompanionPlainBytes, sealedLength) &&
       appendBase64(ivText, sizeof(ivText), ivUsed, iv, sizeof(iv));
  memset(key, 0, sizeof(key));
  memset(companionWs().crypt, 0, kCompanionPlainBytes);
  // The ciphertext is base64'd straight into the body after its prefix.
  char *bodyStart = nullptr;
  if (ok) {
    size_t start = 0;
    for (uint8_t index = 0; index < count; ++index) start += strlen(companionWs().job.bodies + start) + 1;
    bodyStart = companionWs().job.bodies + start;
    size_t bodyUsed = static_cast<size_t>(snprintf(
        bodyStart, sizeof(companionWs().job.bodies) - start, "\"t\":\"creds\",\"gen\":%lu,\"iv\":\"%s\",\"ct\":\"",
        static_cast<unsigned long>(generation), ivText));
    ok = start + bodyUsed < sizeof(companionWs().job.bodies) &&
         appendBase64(bodyStart, sizeof(companionWs().job.bodies) - start - 3, bodyUsed, sealed,
                      sealedLength) &&
         bodyUsed + COMPANION_MAC_HEX + 20 <= COMPANION_FRAME_BYTES;
    if (ok) {
      memcpy(bodyStart + bodyUsed, "\"}", 3);
      ++count;
    } else {
      bodyStart[0] = '\0';
    }
  }
  memset(sealed, 0, sizeof(companionWs().crypt) - kCompanionPlainBytes);
  return ok;
}
#endif

// Network task: queue what changed in the outbox and the switches since this
// session last sent them (everything after a new session). httpd signs and sends.
void OpenBrewByWeightNetwork::companionPrepareSends() {
  companionNotified_.store(false, std::memory_order_release);
  const MicraCompanionOutbox outbox = machineIntegrationCompanionOutbox();
  uint32_t generation = 0;
  [[maybe_unused]] uint32_t lastCommand = 0;
  bool snapshot = false, ui = true, logSwitch = true;
  char deviceName[DEVICE_NAME_CAPACITY] = {};
  CompanionLinkSlot sent;
  uint8_t secret[COMPANION_SECRET_BYTES];
  {
    TaskLockGuard lock(dataMux_);
    // companionActiveLink's slot, mutable: the log subscription applies here.
    CompanionLinkSlot *link = nullptr;
    for (auto &slot : g_companion.links)
      if (slot.fd >= 0 && slot.phase == CompanionLinkPhase::READY &&
          slot.generation == g_companion.active)
        link = &slot;
    if (link == nullptr || link->closing) return;
    generation = link->generation;
    snapshot = !link->snapshotSent;
    companionLogApplyLocked(*link);  // the log cursor, against this boot's id
    sent = *link;
    lastCommand = g_companion.lastCommandSent;
    ui = g_companion.stored.uiOff == 0;
    logSwitch = g_companion.stored.logSwitch != 0;
    memcpy(deviceName, status_.deviceName, sizeof(deviceName));
    memcpy(secret, g_companion.stored.secret, sizeof(secret));
  }
  memset(sent.key, 0, sizeof(sent.key));
  memset(&companionWs().job, 0, sizeof(companionWs().job));
  uint8_t count = 0;
  const char *uiText = ui ? "true" : "false", *logText = logSwitch ? "true" : "false";
  const bool configDue = snapshot || outbox.configSequence != sent.sentConfig ||
                         ui != sent.sentUi || logSwitch != sent.sentLog;
  const bool config = configDue &&
      (kCompanionLm ? companionAddBody(count, "\"t\":\"cfg\",\"lm\":%s,\"conn\":\"%s\",\"observe\":%s,"
                                              "\"ui\":%s,\"log\":%s}",
                                       outbox.lmSwitch ? "true" : "false",
                                       outbox.websocketConnection ? "ws" : "api",
                                       outbox.observe ? "true" : "false", uiText, logText)
                    : companionAddBody(count, "\"t\":\"cfg\",\"ui\":%s,\"log\":%s}", uiText,
                                       logText));
  // The companion's label for this controller; validDeviceName's charset
  // (letters, digits, spaces, hyphens) needs no JSON escaping, and `id` is
  // its MAC text — the log store's device key (2B).
  const bool deviceDue = companionDeviceNameDue(sent, deviceName);
  char deviceIdText[24] = {};
  deviceId(deviceIdText, sizeof(deviceIdText));
  const bool device =
      deviceDue && companionAddBody(count, "\"t\":\"dev\",\"name\":\"%s\",\"id\":\"%s\"}",
                                    deviceName, deviceIdText);
  bool credentialsDue = false, credentials = false, routeDue = false, route = false,
       holdDue = false, hold = false, commandDue = false, command = false;
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  credentialsDue = outbox.credentialsGeneration != 0 &&
      (snapshot || outbox.credentialsGeneration != sent.sentCredentials);
  credentials = credentialsDue &&
      (outbox.credentialsErase
           ? companionAddBody(count, "\"t\":\"creds\",\"gen\":%lu,\"erase\":true}",
                              static_cast<unsigned long>(outbox.credentialsGeneration))
           : companionCredentialsBody(count, outbox.credentialsGeneration, secret));
  routeDue = snapshot || outbox.routeSequence != sent.sentRoute;
  route = routeDue && companionAddBody(count, "\"t\":\"route\",\"to\":\"%s\"}",
                                       outbox.routeCompanion ? "companion" : "direct");
  holdDue = snapshot || outbox.holdSequence != sent.sentHold;
  hold = holdDue && companionAddBody(count, "\"t\":\"hold\",\"on\":%s}",
                                     outbox.hold ? "true" : "false");
  const char *op = companionCommandName(outbox.commandOp);
  commandDue = outbox.commandId != 0 && outbox.commandId != lastCommand && op != nullptr;
  command = commandDue &&
      (outbox.commandOp == MicraCompanionCommandOp::TEMPERATURE
           ? companionAddBody(count, "\"t\":\"cmd\",\"id\":%lu,\"op\":\"%s\",\"v\":%u}",
                              static_cast<unsigned long>(outbox.commandId), op,
                              static_cast<unsigned>(outbox.commandValue))
           : companionAddBody(count, "\"t\":\"cmd\",\"id\":%lu,\"op\":\"%s\"}",
                              static_cast<unsigned long>(outbox.commandId), op));
#endif
  memset(secret, 0, sizeof(secret));
  // The diagnostic log stream (2B): one `log` body per pass; the rest pages
  // on the next pass.
  bool logMore = false;
  uint32_t logLast = sent.logSent, logMiss = 0;
  const bool logBuilt =
      companionLogBody(count, sent, logSwitch, logLast, logMore, logMiss);
  // A body that could not be built stays due and is tried on the next pass.
  const bool complete = config == configDue && device == deviceDue &&
                        credentials == credentialsDue &&
                        route == routeDue && hold == holdDue && command == commandDue;
  if (!complete || logMore) companionNotified_.store(true, std::memory_order_release);
  if (count == 0) return;
  companionWs().job.generation = generation;
  companionWs().job.count = count;
  companionSendPending_.store(true, std::memory_order_release);
  if (httpd_queue_work(server_, companionSendWork, this) != ESP_OK) {
    companionSendPending_.store(false, std::memory_order_release);
    companionNotified_.store(true, std::memory_order_release);
    return;
  }
  TaskLockGuard lock(dataMux_);
  for (auto &slot : g_companion.links) {
    if (slot.fd < 0 || slot.generation != generation) continue;
    slot.snapshotSent = slot.snapshotSent || complete;
    if (config) {
      slot.sentConfig = outbox.configSequence;
      slot.sentUi = ui;
      slot.sentLog = logSwitch;
    }
    if (device) memcpy(slot.sentDeviceName, deviceName, sizeof(slot.sentDeviceName));
    if (route) slot.sentRoute = outbox.routeSequence;
    if (hold) slot.sentHold = outbox.holdSequence;
    if (credentials) slot.sentCredentials = outbox.credentialsGeneration;
    if (logBuilt) {
      slot.logSent = logLast;
      slot.logCatchUp = logMore;
      g_companion.logHeard = false;  // this flush consumed the heartbeat
      g_companion.logFlushAtMs = millis();
      if (logMiss != 0) g_companion.logGaps += logMiss;
    }
  }
  if (command) g_companion.lastCommandSent = outbox.commandId;
}

// httpd: sign the queued bodies with the session key and send them in order.
void OpenBrewByWeightNetwork::companionSendWork(void *context) {
  auto &self = *static_cast<OpenBrewByWeightNetwork *>(context);
  int fd = -1;
  uint8_t key[32] = {};
  uint32_t sequence = 0;
  {
    TaskLockGuard lock(self.dataMux_);
    for (const auto &slot : g_companion.links) {
      if (slot.fd < 0 || slot.closing || slot.phase != CompanionLinkPhase::READY ||
          slot.generation != companionWs().job.generation)
        continue;
      fd = slot.fd;
      memcpy(key, slot.key, sizeof(key));
      sequence = slot.outSequence;
    }
  }
  CompanionSendJob &job = companionWs().job;
  const char *body = job.bodies;
  uint8_t done = 0;
  bool deferred = false;
  uiStreamSendKind = "companion";
  for (; fd >= 0 && done < job.count; ++done) {
    const size_t length =
        companionSealFrame(key, sequence + 1, body, companionWs().frame, sizeof(companionWs().frame));
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.final = true;
    frame.payload = reinterpret_cast<uint8_t *>(companionWs().frame);
    frame.len = length;
    if (length == 0 || httpd_ws_send_frame_async(self.server_, fd, &frame) != ESP_OK) {
      // Nothing of this frame reached the wire (the socket was not writable
      // yet): like ui-stream, defer it and the rest instead of closing.
      deferred = length != 0 && uiStreamSendCleanAbort && ++job.retries <= kCompanionSendRetries;
      break;
    }
    ++sequence;
    body += strlen(body) + 1;
  }
  uiStreamSendKind = "control";
  memset(key, 0, sizeof(key));
  const bool sent = fd >= 0 && done == job.count;
  if (fd >= 0) {
    {
      TaskLockGuard lock(self.dataMux_);
      for (auto &slot : g_companion.links) {
        if (slot.fd != fd) continue;
        if (sent || deferred) slot.outSequence = sequence;
        else {
          slot.closing = true;
          slot.closeReason = "a frame could not be sent";
        }
      }
    }
    if (done != 0) httpd_sess_update_lru_counter(self.server_, fd);
    if (!sent && !deferred) (void)httpd_sess_trigger_close(self.server_, fd);
  }
  if (deferred) {
    // The unsent bodies move to the front, in order; the job stays pending.
    size_t rest = 0;
    for (uint8_t index = done; index < job.count; ++index) rest += strlen(body + rest) + 1;
    memmove(job.bodies, body, rest);
    memset(job.bodies + rest, 0, sizeof(job.bodies) - rest);
    job.count = static_cast<uint8_t>(job.count - done);
    self.companionSendRetry_.store(true, std::memory_order_release);
    return;
  }
  memset(&job, 0, sizeof(job));
  self.companionSendPending_.store(false, std::memory_order_release);
}

// httpd: the ui-stream refresh, for the link sockets.
void OpenBrewByWeightNetwork::companionLruWork(void *context) {
  auto &self = *static_cast<OpenBrewByWeightNetwork *>(context);
  int fds[2] = {-1, -1};
  {
    TaskLockGuard lock(self.dataMux_);
    for (size_t index = 0; index < 2; ++index)
      if (!g_companion.links[index].closing) fds[index] = g_companion.links[index].fd;
  }
  for (const int fd : fds)
    if (fd >= 0) (void)httpd_sess_update_lru_counter(self.server_, fd);
  self.companionLruPending_.store(false, std::memory_order_release);
}

// httpd's close_fn: before the socket goes, its relay identity is dropped and
// an authenticated link closed by no decision of ours gets its cause for the
// Web UI log. httpd closes a peer that is still there only for its LRU purge.
void OpenBrewByWeightNetwork::companionSocketClose(httpd_handle_t, int fd) {
  OpenBrewByWeightNetwork *self = instance_;
  if (self != nullptr) self->relayIdentityClear(fd);
  bool unexplained = false;
  if (self != nullptr && self->companionLinkUp_.load(std::memory_order_acquire)) {
    TaskLockGuard lock(self->dataMux_);
    for (const auto &slot : g_companion.links)
      unexplained |= slot.fd == fd && slot.closeReason == nullptr;
  }
  if (unexplained) {
    char byte;
    const int got = static_cast<int>(recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT));
    const bool open = got > 0 || (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    const char *reason = got == 0 ? "the companion closed the connection"
                         : open   ? "evicted to make room for another connection"
                                  : "the connection was lost";
    TaskLockGuard lock(self->dataMux_);
    for (auto &slot : g_companion.links)
      if (slot.fd == fd && slot.closeReason == nullptr) slot.closeReason = reason;
  }
  close(fd);
}

}  // namespace openbrewbyweight

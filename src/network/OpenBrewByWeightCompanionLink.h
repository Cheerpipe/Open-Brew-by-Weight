#pragma once

// Companion link, controller side (docs/COMPANION_LINK.md), Linea Micra
// builds: the pairing request, the stored pairing, the link sessions and the
// handshake and frame codec. Pure state and arithmetic: the network owns it
// under dataMux_, feeds times, addresses and random bytes, and supplies the
// three hash primitives declared below (PSA on the controller, a reference
// implementation in the host test). Secrets never reach logs or JSON.

#include "OpenBrewByWeightShotCurveTypes.h"  // appendBase64
#include "OpenBrewByWeightWebAccess.h"       // webAccessFnv, copyWebAccessLabel
#include "machine/OpenBrewByWeightMicraCompanion.h"  // MicraCompanionRoute
#include <cJSON.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace openbrewbyweight {

constexpr uint32_t COMPANION_PROTOCOL = 1;
constexpr uint32_t COMPANION_REQUEST_MS = 600000;
constexpr uint32_t COMPANION_NEW_REQUEST_MS = 5000;
constexpr uint8_t COMPANION_CODE_ATTEMPTS = 3;
constexpr uint8_t COMPANION_IGNORED_IDS = 4;
// A whole WebSocket payload, MAC prefix included.
constexpr size_t COMPANION_FRAME_BYTES = 4096;
constexpr size_t COMPANION_KEY_BYTES = 65;  // uncompressed P-256 point
constexpr size_t COMPANION_SECRET_BYTES = 32;
constexpr size_t COMPANION_COMMITMENT_BYTES = 32;
constexpr size_t COMPANION_NONCE_BYTES = 16;
constexpr size_t COMPANION_MAC_HEX = 32;  // first 16 bytes of the frame HMAC
constexpr size_t COMPANION_ID_TEXT = 37, COMPANION_NAME_TEXT = 33,
                 COMPANION_VERSION_TEXT = 25, COMPANION_ADDRESS_TEXT = 48;
constexpr uint32_t COMPANION_MAGIC = 0x434D504EU;  // "CMPN"
constexpr uint16_t COMPANION_STORED_VERSION = 1;

// One NVS blob. Missing or invalid means "never paired".
struct CompanionStored {
  uint32_t magic = COMPANION_MAGIC;
  uint16_t version = COMPANION_STORED_VERSION;
  uint16_t structureSize = sizeof(CompanionStored);
  uint8_t paired = 0;
  uint8_t lmSwitch = 0;  // La Marzocco through the companion
  uint8_t forgotten = 0;  // forgottenId/forgottenSecret answer with `erase`
  uint8_t reserved = 0;
  uint8_t secret[COMPANION_SECRET_BYTES] = {};
  uint8_t publicKey[COMPANION_KEY_BYTES] = {};
  char id[COMPANION_ID_TEXT] = {};
  char name[COMPANION_NAME_TEXT] = {};  // sanitized for JSON and HTML
  char companionVersion[COMPANION_VERSION_TEXT] = {};
  char address[COMPANION_ADDRESS_TEXT] = {};  // peer of the last authenticated link
  char forgottenId[COMPANION_ID_TEXT] = {};
  uint8_t forgottenSecret[COMPANION_SECRET_BYTES] = {};
  uint32_t checksum = 0;
};
static_assert(sizeof(CompanionStored) == 328, "companion blob layout changed");

inline uint32_t companionChecksum(const CompanionStored &stored) {
  return webAccessFnv(&stored, offsetof(CompanionStored, checksum));
}

inline void finalizeCompanionStored(CompanionStored &stored) {
  stored.magic = COMPANION_MAGIC;
  stored.version = COMPANION_STORED_VERSION;
  stored.structureSize = sizeof(CompanionStored);
  stored.checksum = companionChecksum(stored);
}

inline bool validCompanionStored(const CompanionStored &stored) {
  return stored.magic == COMPANION_MAGIC && stored.version == COMPANION_STORED_VERSION &&
         stored.structureSize == sizeof(CompanionStored) && stored.paired <= 1 &&
         stored.lmSwitch <= 1 && stored.forgotten <= 1 &&
         stored.checksum == companionChecksum(stored);
}

// --- Request validation and peers -----------------------------------------

inline bool companionIdValid(const char *id) {
  if (id == nullptr || strnlen(id, COMPANION_ID_TEXT) != COMPANION_ID_TEXT - 1) return false;
  for (size_t index = 0; index < COMPANION_ID_TEXT - 1; ++index) {
    const char c = id[index];
    const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (dash ? c != '-' : !hex) return false;
  }
  return true;
}

// `ascii`: printable ASCII only (versions); otherwise UTF-8 without control
// characters (names). Display copies are sanitized by copyWebAccessLabel.
inline bool companionTextValid(const char *text, size_t minBytes, size_t maxBytes, bool ascii) {
  if (text == nullptr) return false;
  const size_t length = strnlen(text, maxBytes + 1);
  if (length < minBytes || length > maxBytes) return false;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char c = static_cast<unsigned char>(text[index]);
    if (c < 0x20 || c == 0x7F || (ascii && c > 0x7E)) return false;
  }
  return true;
}

// RFC 1918, 169.254/16 and loopback; IPv6 loopback and link-local. The
// dual-stack listener reports IPv4 peers as IPv4-mapped addresses.
inline bool companionPeerAllowedV4(uint32_t host) {
  return (host >> 24) == 10U || (host >> 20) == 0xAC1U || (host >> 16) == 0xC0A8U ||
         (host >> 16) == 0xA9FEU || (host >> 24) == 127U;
}

inline bool companionPeerAllowedV6(const uint8_t address[16]) {
  static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
  if (memcmp(address, kMapped, sizeof(kMapped)) == 0) {
    return companionPeerAllowedV4(static_cast<uint32_t>(address[12]) << 24 |
                                  static_cast<uint32_t>(address[13]) << 16 |
                                  static_cast<uint32_t>(address[14]) << 8 | address[15]);
  }
  bool loopback = address[15] == 1;
  for (size_t index = 0; index < 15; ++index) loopback = loopback && address[index] == 0;
  return loopback || (address[0] == 0xFE && (address[1] & 0xC0) == 0x80);
}

// --- Base64 (standard alphabet, padded, canonical) -------------------------

inline bool companionBase64Decode(const char *text, uint8_t *out, size_t size) {
  const size_t chars = 4 * ((size + 2) / 3);
  if (text == nullptr || size == 0 || size > 96 || strnlen(text, chars + 1) != chars) return false;
  uint32_t group = 0;
  size_t written = 0;
  for (size_t index = 0; index < chars; ++index) {
    const char c = text[index];
    const int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26
                    : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63
                    : c == '=' ? 0 : -1;
    if (digit < 0) return false;
    group = group << 6 | static_cast<uint32_t>(digit);
    if (index % 4 != 3) continue;
    for (int shift = 16; shift >= 0 && written < size; shift -= 8)
      out[written++] = static_cast<uint8_t>(group >> shift);
    group = 0;
  }
  // Only the exact encoding of these bytes is accepted (padding included).
  char again[132];
  size_t used = 0;
  return appendBase64(again, sizeof(again), used, out, size) && strcmp(again, text) == 0;
}

// --- Hash primitives (supplied by the owner) and protocol derivations ------

struct CompanionBytes {
  const void *data;
  size_t size;
};
bool companionSha256(const CompanionBytes *parts, size_t count, uint8_t out[32]);
bool companionHmacSha256(const uint8_t *key, size_t keySize, const CompanionBytes *parts,
                         size_t count, uint8_t out[32]);
bool companionHkdfSha256(const uint8_t *ikm, size_t ikmSize, const uint8_t *salt,
                         size_t saltSize, const char *info, uint8_t out[32]);

inline bool companionSame(const uint8_t *a, const uint8_t *b, size_t size) {
  uint8_t difference = 0;
  for (size_t index = 0; index < size; ++index) difference |= a[index] ^ b[index];
  return difference == 0;
}

// What the first pair request carries instead of the companion's key.
inline bool companionCommitment(const uint8_t companionKey[COMPANION_KEY_BYTES],
                                uint8_t commitment[COMPANION_COMMITMENT_BYTES]) {
  static const char kLabel[] = "obbw-companion-commit-v1";
  const CompanionBytes parts[] = {{kLabel, sizeof(kLabel) - 1}, {companionKey, COMPANION_KEY_BYTES}};
  return companionSha256(parts, 2, commitment);
}

// The six-digit verification code both sides show.
inline bool companionSas(const uint8_t companionKey[COMPANION_KEY_BYTES],
                         const uint8_t controllerKey[COMPANION_KEY_BYTES], uint32_t &code) {
  static const char kLabel[] = "obbw-companion-sas-v1";
  const CompanionBytes parts[] = {{kLabel, sizeof(kLabel) - 1},
                                  {companionKey, COMPANION_KEY_BYTES},
                                  {controllerKey, COMPANION_KEY_BYTES}};
  uint8_t digest[32];
  if (!companionSha256(parts, 3, digest)) return false;
  code = (static_cast<uint32_t>(digest[0]) << 24 | static_cast<uint32_t>(digest[1]) << 16 |
          static_cast<uint32_t>(digest[2]) << 8 | digest[3]) % 1000000U;
  return true;
}

inline bool companionPairingSecret(const uint8_t ecdhX[32],
                                   const uint8_t companionKey[COMPANION_KEY_BYTES],
                                   const uint8_t controllerKey[COMPANION_KEY_BYTES],
                                   uint8_t secret[COMPANION_SECRET_BYTES]) {
  const CompanionBytes keys[] = {{companionKey, COMPANION_KEY_BYTES},
                                 {controllerKey, COMPANION_KEY_BYTES}};
  uint8_t salt[32];
  return companionSha256(keys, 2, salt) &&
         companionHkdfSha256(ecdhX, 32, salt, sizeof(salt), "obbw-companion-pair-v1", secret);
}

inline bool companionSessionKey(const uint8_t secret[COMPANION_SECRET_BYTES],
                                const uint8_t nc[COMPANION_NONCE_BYTES],
                                const uint8_t ns[COMPANION_NONCE_BYTES], uint8_t key[32]) {
  uint8_t salt[2 * COMPANION_NONCE_BYTES];
  memcpy(salt, nc, COMPANION_NONCE_BYTES);
  memcpy(salt + COMPANION_NONCE_BYTES, ns, COMPANION_NONCE_BYTES);
  return companionHkdfSha256(secret, COMPANION_SECRET_BYTES, salt, sizeof(salt), "obbw-link-v1",
                             key);
}

// HMAC(S, tag ‖ nc ‖ ns): `C` proof, `F` ready; `E` erase omits ns.
inline bool companionHandshakeMac(const uint8_t secret[COMPANION_SECRET_BYTES], char tag,
                                  const uint8_t nc[COMPANION_NONCE_BYTES],
                                  const uint8_t *ns, uint8_t mac[32]) {
  const CompanionBytes parts[] = {{&tag, 1}, {nc, COMPANION_NONCE_BYTES},
                                  {ns, ns != nullptr ? COMPANION_NONCE_BYTES : 0}};
  return companionHmacSha256(secret, COMPANION_SECRET_BYTES, parts, 3, mac);
}

inline bool companionCredentialsKey(const uint8_t secret[COMPANION_SECRET_BYTES], uint8_t key[32]) {
  return companionHkdfSha256(secret, COMPANION_SECRET_BYTES, nullptr, 0, "obbw-creds-v1", key);
}

// --- Signed frames: `<mac>.<json>` -----------------------------------------

inline bool companionFrameMacHex(const uint8_t key[32], char direction, const char *json,
                                 size_t length, char hex[COMPANION_MAC_HEX]) {
  const CompanionBytes parts[] = {{&direction, 1}, {json, length}};
  uint8_t mac[32];
  if (!companionHmacSha256(key, 32, parts, 2, mac)) return false;
  static const char kDigits[] = "0123456789abcdef";
  for (size_t index = 0; index < COMPANION_MAC_HEX / 2; ++index) {
    hex[2 * index] = kDigits[mac[index] >> 4];
    hex[2 * index + 1] = kDigits[mac[index] & 15];
  }
  return true;
}

// Writes `<mac>.{"q":seq,` + body, where body continues the object after its
// brace (`"t":"cfg",…}`), signed for direction `f`. Returns the frame length,
// or 0 when it would exceed `capacity` or COMPANION_FRAME_BYTES.
inline size_t companionSealFrame(const uint8_t key[32], uint32_t sequence, const char *body,
                                 char *out, size_t capacity) {
  const size_t prefix = COMPANION_MAC_HEX + 1;
  if (capacity > COMPANION_FRAME_BYTES + 1) capacity = COMPANION_FRAME_BYTES + 1;
  if (capacity <= prefix) return 0;
  const int length = snprintf(out + prefix, capacity - prefix, "{\"q\":%lu,%s",
                              static_cast<unsigned long>(sequence), body);
  if (length < 0 || static_cast<size_t>(length) >= capacity - prefix ||
      !companionFrameMacHex(key, 'f', out + prefix, static_cast<size_t>(length), out))
    return 0;
  out[COMPANION_MAC_HEX] = '.';
  return prefix + static_cast<size_t>(length);
}

// Verifies a companion frame (direction `c`); returns its JSON or nullptr.
inline const char *companionOpenFrame(const uint8_t key[32], const char *text, size_t length) {
  const size_t prefix = COMPANION_MAC_HEX + 1;
  char expected[COMPANION_MAC_HEX];
  if (length <= prefix || length > COMPANION_FRAME_BYTES || text[COMPANION_MAC_HEX] != '.' ||
      !companionFrameMacHex(key, 'c', text + prefix, length - prefix, expected) ||
      !companionSame(reinterpret_cast<const uint8_t *>(expected),
                     reinterpret_cast<const uint8_t *>(text), COMPANION_MAC_HEX))
    return nullptr;
  return text + prefix;
}

inline bool companionFrameSequence(const cJSON *frame, uint32_t expected) {
  const cJSON *sequence = cJSON_GetObjectItemCaseSensitive(frame, "q");
  return cJSON_IsNumber(sequence) && sequence->valuedouble == static_cast<double>(expected);
}

// --- Pairing request (RAM only) ---------------------------------------------

enum class CompanionPairState : uint8_t { NONE, PENDING, APPROVED, DENIED, EXPIRED };
enum class CompanionPairResult : uint8_t { NEW, PENDING, IGNORED, ALREADY_PAIRED, BUSY, RATE_LIMITED };
enum class CompanionApproveResult : uint8_t { OK, WRONG_CODE, DENIED, NOT_PENDING, NOT_READY };

struct CompanionRequest {
  CompanionPairState state = CompanionPairState::NONE;
  bool keyReady = false;   // controllerKey holds this request's key
  bool keyIssued = false;  // controllerKey reached the companion in a response
  bool revealed = false;   // companionKey holds the key matching the commitment
  bool approving = false;  // the code matched; the network task derives S
  bool lmSwitch = true;    // the switch chosen in the approval panel
  uint8_t wrongCodes = 0;
  uint32_t serial = 0;  // names the request key the network task made
  uint32_t untilMs = 0;
  char id[COMPANION_ID_TEXT] = {};
  char name[COMPANION_NAME_TEXT] = {};
  char version[COMPANION_VERSION_TEXT] = {};
  char address[COMPANION_ADDRESS_TEXT] = {};
  uint8_t commitment[COMPANION_COMMITMENT_BYTES] = {};
  uint8_t companionKey[COMPANION_KEY_BYTES] = {};
  uint8_t controllerKey[COMPANION_KEY_BYTES] = {};
};

// Admin may take the code once both keys are fixed.
inline bool companionCodeReady(const CompanionRequest &request) {
  return request.state == CompanionPairState::PENDING && request.keyReady && request.revealed;
}

struct CompanionPairing {
  CompanionRequest request = {};
  uint32_t newAtMs = 0;
  bool newSeen = false;
  char ignored[COMPANION_IGNORED_IDS][COMPANION_ID_TEXT] = {};
  uint8_t ignoredNext = 0;
};

inline void companionPairTick(CompanionPairing &pairing, uint32_t now) {
  CompanionRequest &request = pairing.request;
  if (request.state == CompanionPairState::PENDING && !request.approving &&
      static_cast<int32_t>(now - request.untilMs) >= 0)
    request.state = CompanionPairState::EXPIRED;
}

inline bool companionIgnored(const CompanionPairing &pairing, const char *id) {
  for (const auto &ignored : pairing.ignored)
    if (ignored[0] != '\0' && strcmp(ignored, id) == 0) return true;
  return false;
}

inline uint8_t companionIgnoredCount(const CompanionPairing &pairing) {
  uint8_t count = 0;
  for (const auto &ignored : pairing.ignored) count += ignored[0] != '\0';
  return count;
}

// Fields are validated by the caller; `address` comes from the socket.
inline CompanionPairResult companionPairRequest(CompanionPairing &pairing, bool paired,
                                                const char *id, const char *name,
                                                const char *version, const char *address,
                                                const uint8_t commitment[COMPANION_COMMITMENT_BYTES],
                                                uint32_t now) {
  companionPairTick(pairing, now);
  CompanionRequest &request = pairing.request;
  if (companionIgnored(pairing, id)) return CompanionPairResult::IGNORED;
  if (paired) return CompanionPairResult::ALREADY_PAIRED;
  if (request.state == CompanionPairState::PENDING) {
    if (strcmp(request.id, id) != 0 || request.approving) return CompanionPairResult::BUSY;
    // The same companion asking again: its request stands unless its commitment changed.
    if (memcmp(request.commitment, commitment, COMPANION_COMMITMENT_BYTES) == 0)
      return CompanionPairResult::PENDING;
  }
  if (pairing.newSeen && now - pairing.newAtMs < COMPANION_NEW_REQUEST_MS)
    return CompanionPairResult::RATE_LIMITED;
  const uint32_t serial = request.serial + 1;
  request = CompanionRequest{};
  request.state = CompanionPairState::PENDING;
  request.serial = serial;
  request.untilMs = now + COMPANION_REQUEST_MS;
  memcpy(request.id, id, COMPANION_ID_TEXT - 1);
  copyWebAccessLabel(request.name, name);
  copyWebAccessLabel(request.version, version);
  snprintf(request.address, sizeof(request.address), "%s", address);
  memcpy(request.commitment, commitment, COMPANION_COMMITMENT_BYTES);
  pairing.newSeen = true;
  pairing.newAtMs = now;
  return CompanionPairResult::NEW;
}

// The companion's key, revealed once its request received controllerKey. A key
// that is not a P-256 point (`onCurve`, checked by the caller) or does not
// hash to the commitment denies the request; repeating the accepted key is
// harmless. False answers 400.
inline bool companionPairReveal(CompanionPairing &pairing, const char *id,
                                const uint8_t key[COMPANION_KEY_BYTES], bool onCurve,
                                uint32_t now) {
  companionPairTick(pairing, now);
  CompanionRequest &request = pairing.request;
  if (request.state != CompanionPairState::PENDING || strcmp(request.id, id) != 0) return false;
  if (request.revealed && companionSame(request.companionKey, key, COMPANION_KEY_BYTES)) return true;
  if (request.approving) return false;
  uint8_t digest[COMPANION_COMMITMENT_BYTES];
  if (!onCurve || request.revealed || !request.keyIssued || !companionCommitment(key, digest) ||
      !companionSame(digest, request.commitment, sizeof(digest))) {
    request.state = CompanionPairState::DENIED;
    return false;
  }
  memcpy(request.companionKey, key, COMPANION_KEY_BYTES);
  request.revealed = true;
  return true;
}

inline const char *companionPairPoll(CompanionPairing &pairing, const char *id, uint32_t now) {
  companionPairTick(pairing, now);
  const CompanionRequest &request = pairing.request;
  if (request.state == CompanionPairState::NONE || id == nullptr || strcmp(request.id, id) != 0)
    return "unknown";
  switch (request.state) {
    case CompanionPairState::PENDING: return "pending";
    case CompanionPairState::APPROVED: return "approved";
    case CompanionPairState::DENIED: return "denied";
    default: return "expired";
  }
}

// `expected` is the request's code (companionSas). Three wrong codes deny it.
inline CompanionApproveResult companionPairApprove(CompanionPairing &pairing, uint32_t code,
                                                   uint32_t expected, bool lmSwitch,
                                                   uint32_t now) {
  companionPairTick(pairing, now);
  CompanionRequest &request = pairing.request;
  if (request.state != CompanionPairState::PENDING || request.approving)
    return CompanionApproveResult::NOT_PENDING;
  if (!companionCodeReady(request)) return CompanionApproveResult::NOT_READY;
  if (code != expected) {
    if (++request.wrongCodes < COMPANION_CODE_ATTEMPTS) return CompanionApproveResult::WRONG_CODE;
    request.state = CompanionPairState::DENIED;
    return CompanionApproveResult::DENIED;
  }
  request.approving = true;
  request.lmSwitch = lmSwitch;
  return CompanionApproveResult::OK;
}

// Denies the open request and ignores its id (the oldest of a full list goes).
inline bool companionPairIgnore(CompanionPairing &pairing, uint32_t now) {
  companionPairTick(pairing, now);
  CompanionRequest &request = pairing.request;
  if (request.state != CompanionPairState::PENDING || request.approving) return false;
  if (!companionIgnored(pairing, request.id)) {
    memcpy(pairing.ignored[pairing.ignoredNext], request.id, COMPANION_ID_TEXT);
    pairing.ignoredNext = (pairing.ignoredNext + 1) % COMPANION_IGNORED_IDS;
  }
  request.state = CompanionPairState::DENIED;
  return true;
}

inline void companionUnignore(CompanionPairing &pairing) {
  memset(pairing.ignored, 0, sizeof(pairing.ignored));
  pairing.ignoredNext = 0;
}

// The derived secret makes the request the stored pairing.
inline void companionPairComplete(CompanionPairing &pairing, CompanionStored &stored,
                                  const uint8_t secret[COMPANION_SECRET_BYTES]) {
  CompanionRequest &request = pairing.request;
  if (stored.forgotten && strcmp(stored.forgottenId, request.id) == 0) {
    stored.forgotten = 0;
    memset(stored.forgottenId, 0, sizeof(stored.forgottenId));
    memset(stored.forgottenSecret, 0, sizeof(stored.forgottenSecret));
  }
  stored.paired = 1;
  stored.lmSwitch = request.lmSwitch ? 1 : 0;
  memcpy(stored.secret, secret, COMPANION_SECRET_BYTES);
  memcpy(stored.publicKey, request.companionKey, COMPANION_KEY_BYTES);
  memcpy(stored.id, request.id, sizeof(stored.id));
  memcpy(stored.name, request.name, sizeof(stored.name));
  memcpy(stored.companionVersion, request.version, sizeof(stored.companionVersion));
  memcpy(stored.address, request.address, sizeof(stored.address));
  request.approving = false;
  request.state = CompanionPairState::APPROVED;
}

// Forget keeps {id, secret} so the next link attempt receives a signed erase.
inline bool companionForget(CompanionStored &stored) {
  if (!stored.paired) return false;
  const CompanionStored kept = stored;
  stored = CompanionStored{};
  stored.forgotten = 1;
  memcpy(stored.forgottenId, kept.id, sizeof(stored.forgottenId));
  memcpy(stored.forgottenSecret, kept.secret, sizeof(stored.forgottenSecret));
  return true;
}

// --- Link sessions -----------------------------------------------------------

enum class CompanionHello : uint8_t { CHALLENGE, NOT_PAIRED, PROTOCOL, ERASE };

inline CompanionHello companionHelloVerdict(const CompanionStored &stored, const char *id,
                                            double protocol) {
  if (stored.paired && strcmp(stored.id, id) == 0)
    return protocol == COMPANION_PROTOCOL ? CompanionHello::CHALLENGE : CompanionHello::PROTOCOL;
  if (stored.forgotten && strcmp(stored.forgottenId, id) == 0) return CompanionHello::ERASE;
  return CompanionHello::NOT_PAIRED;
}

enum class CompanionLinkPhase : uint8_t { HELLO, PROOF, READY };

struct CompanionLinkSlot {
  int fd = -1;
  CompanionLinkPhase phase = CompanionLinkPhase::HELLO;
  bool closing = false;
  uint8_t protocol = 0;
  uint32_t generation = 0;  // names the authenticated session for queued sends
  uint32_t openedAtMs = 0, heardAtMs = 0, readyAtMs = 0;
  uint32_t inSequence = 0, outSequence = 0;
  uint8_t nc[COMPANION_NONCE_BYTES] = {}, ns[COMPANION_NONCE_BYTES] = {};
  uint8_t key[32] = {};
  char name[COMPANION_NAME_TEXT] = {}, version[COMPANION_VERSION_TEXT] = {};
  const char *closeReason = nullptr;
  // Outbox sequences already sent in this session; the first send is the
  // whole snapshot.
  bool snapshotSent = false;
  uint32_t sentConfig = 0, sentCredentials = 0, sentRoute = 0, sentHold = 0;
};

struct CompanionState {
  CompanionStored stored = {};
  CompanionPairing pairing = {};
  CompanionLinkSlot links[2] = {};  // the link, and a newcomer authenticating
  uint32_t generation = 0, active = 0;  // active: the ready session, 0 when down
  uint32_t ups = 0, authFailures = 0;
  uint32_t changedAtMs = 0;   // the link last came up or went down; 0: not since boot
  uint32_t peerProtocol = 0;  // the paired companion's refused protocol; 0: none
  uint32_t lastCommandSent = 0;  // a command id is sent once, across sessions
  bool dirty = false;
};

// Diagnostic → Companion; strings are sanitized on store.
struct CompanionSnapshot {
  bool paired = false, lmSwitch = false, linkOpen = false, upstream = false;
  uint8_t protocol = 0;   // the open link's; 0 while down
  int32_t linkAgeS = -1;  // since the link came up or went down; -1: not since boot
  uint32_t peerProtocol = 0, reconnects = 0, authFailures = 0, routeSwitches = 0, commands = 0;
  const char *route = "direct", *reason = "none";
  char name[COMPANION_NAME_TEXT] = {}, address[COMPANION_ADDRESS_TEXT] = {},
       version[COMPANION_VERSION_TEXT] = {};
};

// Handshake 10 s; then two ping intervals of silence, 15 s pings only once the
// route is the companion's. A route request (pending) keeps the long limit:
// the companion answers it on its own cadence, and the La Marzocco service
// gives up on it after 30 s without a ping or the full frame.
inline uint32_t companionSilenceLimitMs(const CompanionLinkSlot &slot, MicraCompanionRoute route) {
  return slot.phase != CompanionLinkPhase::READY ? 10000U
         : route == MicraCompanionRoute::COMPANION ? 35000U : 250000U;
}

// Seconds since the link came up or went down; -1 before either since boot.
inline int32_t companionLinkAgeS(const CompanionState &state, uint32_t now) {
  return state.changedAtMs == 0 ? -1 : static_cast<int32_t>((now - state.changedAtMs) / 1000U);
}

}  // namespace openbrewbyweight

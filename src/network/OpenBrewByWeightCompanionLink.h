#pragma once

// Companion link, controller side (docs/COMPANION_LINK.md), every build: the
// pairing request, the stored pairing, the link sessions, the handshake and
// frame codec, and the signed relay. Pure state and arithmetic: the network owns it
// under dataMux_, feeds times, addresses and random bytes, and supplies the
// three hash primitives declared below (PSA on the controller, a reference
// implementation in the host test). Secrets never reach logs or JSON.

#include "OpenBrewByWeightNetworkTypes.h"             // DEVICE_NAME_CAPACITY
#include "OpenBrewByWeightDomain.h"           // WIFI_PASSWORD_CAPACITY
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
constexpr uint16_t COMPANION_STORED_VERSION = 2;
constexpr size_t COMPANION_STORED_V1_BYTES = 328;  // version 1: no logSwitch tail

// One NVS blob. Missing or invalid means "never paired". The v2 tail
// (`logSwitch`) rides after the checksum: a v1 blob is the same bytes without
// it, so promotion is a version bump plus the default, and only that switch
// is outside the checksum's coverage (corruption there changes a switch, not
// the pairing).
struct CompanionStored {
  uint32_t magic = COMPANION_MAGIC;
  uint16_t version = COMPANION_STORED_VERSION;
  uint16_t structureSize = sizeof(CompanionStored);
  uint8_t paired = 0;
  uint8_t lmSwitch = 0;  // La Marzocco through the companion
  uint8_t forgotten = 0;  // forgottenId/forgottenSecret answer with `erase`
  uint8_t uiOff = 0;  // "Web UI through the companion" off; 0 (on) in older blobs
  uint8_t secret[COMPANION_SECRET_BYTES] = {};
  uint8_t publicKey[COMPANION_KEY_BYTES] = {};
  char id[COMPANION_ID_TEXT] = {};
  char name[COMPANION_NAME_TEXT] = {};  // sanitized for JSON and HTML
  char companionVersion[COMPANION_VERSION_TEXT] = {};
  char address[COMPANION_ADDRESS_TEXT] = {};  // peer of the last authenticated link
  char forgottenId[COMPANION_ID_TEXT] = {};
  uint8_t forgottenSecret[COMPANION_SECRET_BYTES] = {};
  uint32_t checksum = 0;
  uint8_t logSwitch = 1;  // "Diagnostic log through the companion" (v2)
};
static_assert(sizeof(CompanionStored) == 332, "companion blob layout changed");

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
         stored.lmSwitch <= 1 && stored.forgotten <= 1 && stored.uiOff <= 1 &&
         stored.logSwitch <= 1 && stored.checksum == companionChecksum(stored);
}

// A v1 blob read raw into the first COMPANION_STORED_V1_BYTES: the same
// fields, no tail. Promotion keeps the pairing and turns the new switch on
// (the switches default on, plan T); the caller persists the v2 layout.
inline bool validCompanionStoredV1(const CompanionStored &stored) {
  return stored.magic == COMPANION_MAGIC && stored.version == 1 &&
         stored.structureSize == COMPANION_STORED_V1_BYTES && stored.paired <= 1 &&
         stored.lmSwitch <= 1 && stored.forgotten <= 1 && stored.uiOff <= 1 &&
         stored.checksum == companionChecksum(stored);
}

inline void promoteCompanionStoredV1(CompanionStored &stored) {
  stored.version = COMPANION_STORED_VERSION;
  stored.structureSize = sizeof(CompanionStored);
  stored.logSwitch = 1;
  // version and structureSize sit inside the checksummed prefix.
  stored.checksum = companionChecksum(stored);
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
// AES-256-GCM with no additional data: `out` receives the plaintext plus the
// 16-byte tag (seal) or the plaintext without it (open); the same reference
// implementation contract as the hashes above.
bool companionAeadSealBytes(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *plain, size_t plainSize, uint8_t *out,
                            size_t capacity, size_t &length);
bool companionAeadOpenBytes(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *sealed, size_t sealedSize, uint8_t *out,
                            size_t capacity, size_t &length);

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

// The link's K (nc ‖ ns), or with "obbw-relay-v1" the relay's Kr (rb ‖ nc).
inline bool companionSessionKey(const uint8_t secret[COMPANION_SECRET_BYTES],
                                const uint8_t nc[COMPANION_NONCE_BYTES],
                                const uint8_t ns[COMPANION_NONCE_BYTES], uint8_t key[32],
                                const char *info = "obbw-link-v1") {
  uint8_t salt[2 * COMPANION_NONCE_BYTES];
  memcpy(salt, nc, COMPANION_NONCE_BYTES);
  memcpy(salt + COMPANION_NONCE_BYTES, ns, COMPANION_NONCE_BYTES);
  return companionHkdfSha256(secret, COMPANION_SECRET_BYTES, salt, sizeof(salt), info, key);
}

// HMAC(S, tag ‖ nc ‖ ns): `C` proof, `F` ready; `E` erase omits ns. The relay
// handshake signs `R` (request) and `Q` (reply) over rb ‖ nc.
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

// --- Signed relay: `X-OBBW-Relay: 2 <q> <addr> <screen> <mac>` -------------

constexpr size_t COMPANION_RELAY_HEADER_TEXT = 128;  // the longest header and its NUL
constexpr size_t COMPANION_RELAY_SCREEN_TEXT = 25, COMPANION_RELAY_ADDR_TEXT = 46;
constexpr size_t COMPANION_RELAY_MAC_BYTES = 16;
// A sealed X-WebUI-Session or X-Device-Password value in base64 and its NUL
// (63 password bytes seal to 108 characters).
constexpr size_t COMPANION_RELAY_SECRET_TEXT = 112;

struct CompanionRelayHeader {
  uint32_t sequence = 0;
  const char *q = nullptr, *addr = nullptr, *screen = nullptr;  // inside the parsed text
  uint8_t mac[COMPANION_RELAY_MAC_BYTES] = {};
};

inline bool companionLowerHex(const char *text, size_t minimum, size_t maximum) {
  const size_t length = strnlen(text, maximum + 1);
  if (length < minimum || length > maximum) return false;
  for (size_t index = 0; index < length; ++index)
    if (!((text[index] >= '0' && text[index] <= '9') || (text[index] >= 'a' && text[index] <= 'f')))
      return false;
  return true;
}

// Splits the header in place at single spaces. v: the relay protocol version,
// always 2; q: canonical decimal from 1; addr: the browser's IPv4 or IPv6
// text; screen: its 16-24 lowercase hex X-WebUI-Client; mac: 32 lowercase
// hex digits.
inline bool companionRelayParse(char *text, CompanionRelayHeader &out) {
  char *field[5] = {text};
  for (size_t index = 1; index < 5; ++index) {
    char *space = strchr(field[index - 1], ' ');
    if (space == nullptr) return false;
    *space = '\0';
    field[index] = space + 1;
  }
  const size_t qLength = strlen(field[1]), addrLength = strlen(field[2]);
  uint64_t sequence = 0;
  bool ok = strcmp(field[0], "2") == 0 && qLength >= 1 && qLength <= 10 && field[1][0] != '0' &&
            addrLength >= 2 && addrLength < COMPANION_RELAY_ADDR_TEXT &&
            strspn(field[2], "0123456789abcdefABCDEF:.") == addrLength &&
            companionLowerHex(field[3], 16, COMPANION_RELAY_SCREEN_TEXT - 1) &&
            companionLowerHex(field[4], 2 * COMPANION_RELAY_MAC_BYTES, 2 * COMPANION_RELAY_MAC_BYTES);
  for (size_t index = 0; ok && index < qLength; ++index) {
    ok = field[1][index] >= '0' && field[1][index] <= '9';
    sequence = sequence * 10 + static_cast<uint64_t>(field[1][index] - '0');
  }
  if (!ok || sequence > UINT32_MAX) return false;
  for (size_t index = 0; index < COMPANION_RELAY_MAC_BYTES; ++index) {
    const char *pair = field[4] + 2 * index;
    const auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
    out.mac[index] = static_cast<uint8_t>(nibble(pair[0]) << 4 | nibble(pair[1]));
  }
  out.sequence = static_cast<uint32_t>(sequence);
  out.q = field[1];
  out.addr = field[2];
  out.screen = field[3];
  return true;
}

// HMAC(Kr, "obbw-relay-v2\nMETHOD\nURI\nq\naddr\nscreen\nsession\n"); the
// header carries its first 16 bytes. `uri` is the path with its query and
// `session` the sealed X-WebUI-Session value as sent, or "".
inline bool companionRelayMac(const uint8_t key[32], const char *method, const char *uri,
                              const CompanionRelayHeader &header, const char *session,
                              uint8_t mac[32]) {
  const char *const fields[] = {"obbw-relay-v2", method, uri, header.q, header.addr,
                                header.screen, session};
  CompanionBytes parts[14];
  for (size_t index = 0; index < 7; ++index) {
    parts[2 * index] = {fields[index], strlen(fields[index])};
    parts[2 * index + 1] = {"\n", 1};
  }
  return companionHmacSha256(key, 32, parts, 14, mac);
}

// --- Sealed bodies and headers (protocol 2) ----------------------------------
//
// AES-256-GCM under Kb, the relay session's second key. The 12-byte nonce is
// the direction ('c' requests, 'f' responses), the context byte, two zeros
// and the counter little-endian; the counter is the request's q (contexts
// 0-2) or the response counter (context 3), so a nonce never repeats under
// one Kb and a seal names the one request it belongs to.

constexpr uint8_t COMPANION_RELAY_CTX_SESSION = 0, COMPANION_RELAY_CTX_PASSWORD = 1,
                  COMPANION_RELAY_CTX_REQUEST = 2, COMPANION_RELAY_CTX_RESPONSE = 3;

inline bool companionBodyKey(const uint8_t relayKey[32], uint8_t bodyKey[32]) {
  return companionHkdfSha256(relayKey, 32, nullptr, 0, "obbw-relay-body-v2", bodyKey);
}

inline void companionRelayNonce(char direction, uint8_t context, uint32_t counter,
                                uint8_t nonce[12]) {
  nonce[0] = static_cast<uint8_t>(direction);
  nonce[1] = context;
  nonce[2] = nonce[3] = 0;
  const uint64_t wide = counter;
  for (size_t index = 0; index < 8; ++index)
    nonce[4 + index] = static_cast<uint8_t>(wide >> (8 * index));
}

// Seals `text` (a header value) as canonical base64 ciphertext under the
// request's q; opens the same value. Opening fails on any tampering, wrong
// context, or a value sealed for another request.
inline bool companionSealSecret(const uint8_t bodyKey[32], uint8_t context, uint32_t q,
                                const char *text, char out[COMPANION_RELAY_SECRET_TEXT]) {
  // 81 sealed bytes are the most whose base64 fits the header value's 111.
  constexpr size_t kSealed = 3 * ((COMPANION_RELAY_SECRET_TEXT - 1) / 4);
  const size_t length = strnlen(text, COMPANION_RELAY_SECRET_TEXT);
  if (length == COMPANION_RELAY_SECRET_TEXT || length + 16 > kSealed) return false;
  uint8_t nonce[12], sealed[kSealed];
  companionRelayNonce('c', context, q, nonce);
  size_t used = 0;
  if (!companionAeadSealBytes(bodyKey, nonce, reinterpret_cast<const uint8_t *>(text),
                              length, sealed, sizeof(sealed), used))
    return false;
  size_t text2 = 0;
  return appendBase64(out, COMPANION_RELAY_SECRET_TEXT, text2, sealed, used);
}

inline bool companionOpenSecret(const uint8_t bodyKey[32], uint8_t context, uint32_t q,
                                const char *value, char *out, size_t capacity) {
  constexpr size_t kSealed = 3 * ((COMPANION_RELAY_SECRET_TEXT - 1) / 4);  // 81
  const size_t chars = strnlen(value, COMPANION_RELAY_SECRET_TEXT);
  if (capacity == 0 || chars == 0 || chars % 4 != 0 || chars > 4 * ((kSealed + 2) / 3))
    return false;
  uint8_t sealed[kSealed];
  const size_t pad = value[chars - 1] != '=' ? 0 : value[chars - 2] == '=' ? 2 : 1;
  const size_t sealedSize = 3 * (chars / 4) - pad;  // what the base64 names
  if (!companionBase64Decode(value, sealed, sealedSize)) return false;
  uint8_t nonce[12];
  companionRelayNonce('c', context, q, nonce);
  size_t opened = 0;
  if (sealedSize < 16 ||
      !companionAeadOpenBytes(bodyKey, nonce, sealed, sealedSize,
                              reinterpret_cast<uint8_t *>(out), capacity - 1, opened))
    return false;
  out[opened] = '\0';
  return true;
}

// Replay window: the highest q and a bitmap of the 64 below it (bit 0 = high).
struct CompanionRelayWindow {
  uint32_t high = 0;
  uint64_t mask = 0;
};

inline bool companionRelayAccept(CompanionRelayWindow &window, uint32_t q) {
  if (q == 0) return false;
  if (q > window.high) {
    const uint32_t shift = q - window.high;
    window.mask = (shift >= 64 ? 0 : window.mask << shift) | 1U;
    window.high = q;
    return true;
  }
  const uint32_t back = window.high - q;
  const uint64_t bit = back < 64 ? uint64_t{1} << back : 0;
  if (bit == 0 || (window.mask & bit) != 0) return false;
  window.mask |= bit;
  return true;
}

// Relay session: rb is the challenge, random per boot and rotated after each
// handshake; Kr signs relayed requests until the next one, and Kb seals their
// bodies, headers and responses. RAM only. `responses` never resets within a
// boot: a seal made with one session's Kb must never reuse a counter another
// session's sealed responses used, or GCM would repeat a nonce.
struct CompanionRelay {
  uint8_t challenge[COMPANION_NONCE_BYTES] = {};
  bool challengeSet = false, ready = false;
  uint8_t key[32] = {};
  uint8_t bodyKey[32] = {};
  uint32_t responses = 0;  // sealed-response counter since boot
  CompanionRelayWindow window = {};
  uint32_t generation = 0;  // names Kr for an admission that verified outside the lock
  uint32_t rejects = 0;     // signed-relay requests refused since boot
};

// Forget and the factory reset end the relay session: relayed requests are
// stale until a handshake with a new pairing. The response counter survives:
// it orders nonces, not the session.
inline void companionRelayDrop(CompanionRelay &relay) {
  relay.ready = false;
  memset(relay.key, 0, sizeof(relay.key));
  memset(relay.bodyKey, 0, sizeof(relay.bodyKey));
  relay.window = CompanionRelayWindow{};
}

// The browser behind a socket's current relayed request; one per httpd socket.
constexpr size_t COMPANION_RELAY_SOCKETS = 7;  // httpd's max_open_sockets
struct CompanionRelayIdentity {
  int fd = -1;
  char screen[COMPANION_RELAY_SCREEN_TEXT] = {};  // empty: a free entry
  char addr[COMPANION_RELAY_ADDR_TEXT] = {};
  // What the admitted request's seals opened or were made with, staged for its
  // handlers: the session token, the device password, the q, and the Kb of the
  // session that admitted it (an OTA job may outlive a relay handshake, and
  // its body and answer still belong to the keys that asked).
  uint32_t sequence = 0;
  uint8_t bodyKey[32] = {};
  bool sessionValid = false;
  uint8_t session[WEB_ACCESS_TOKEN_BYTES] = {};
  bool passwordValid = false;
  char password[WIFI_PASSWORD_CAPACITY] = {};
};

// Admin unlock and the unsafe override each belong to one principal: a direct
// browser's X-WebUI-Client, or a companion screen named by a signed relay.
struct CompanionPrincipal {
  char id[COMPANION_RELAY_SCREEN_TEXT] = {};
  bool relayed = false;
};

inline bool companionPrincipalHolds(const CompanionPrincipal &holder,
                                    const CompanionPrincipal &caller) {
  return holder.id[0] != '\0' && holder.relayed == caller.relayed &&
         strcmp(holder.id, caller.id) == 0;
}

// Home's relay.adminScreen/overrideScreen: the holder's screen as 8 hex
// digits of FNV-1a, or "" unless a companion screen holds it.
inline void companionScreenTag(const CompanionPrincipal &holder, char (&tag)[9]) {
  tag[0] = '\0';
  if (holder.relayed && holder.id[0] != '\0')
    snprintf(tag, sizeof(tag), "%08lx",
             static_cast<unsigned long>(webAccessFnv(holder.id, strlen(holder.id))));
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
  bool lmSwitch = true,    // the switches chosen in the approval panel
       logSwitch = true;
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
                                                   bool logSwitch, uint32_t now) {
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
  request.logSwitch = logSwitch;
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
  stored.logSwitch = request.logSwitch ? 1 : 0;
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
  bool snapshotSent = false, sentUi = true, sentLog = true;
  uint32_t sentConfig = 0, sentCredentials = 0, sentRoute = 0, sentHold = 0;
  char sentDeviceName[DEVICE_NAME_CAPACITY] = {};  // `dev` frame already sent
  // Diagnostic log stream (plan 2B). httpd records the companion's `logsub`
  // request verbatim (logWant*); the network task applies it against this
  // boot's id and owns everything after: whether the stream runs, the sent
  // cursor and whether a catch-up is paging a backlog across passes.
  bool logWant = false, logApplied = true, logSubscribed = false, logCatchUp = false;
  uint32_t logWantBoot = 0, logWantAfter = 0, logSent = 0;
};

// The device name frame is due on every new session (an empty sent copy) and
// again after a rename; an unpublished name (empty) is never sent.
inline bool companionDeviceNameDue(const CompanionLinkSlot &slot, const char *deviceName) {
  return deviceName[0] != '\0' &&
         strncmp(slot.sentDeviceName, deviceName, DEVICE_NAME_CAPACITY) != 0;
}

// The one-letter level of a link `log` frame event.
inline char companionLogLevelLetter(LogLevel level) {
  switch (level) {
    case LogLevel::CRITICAL: return 'c';
    case LogLevel::ERROR: return 'e';
    case LogLevel::WARNING: return 'w';
    case LogLevel::DEBUG: return 'd';
    default: return 'i';
  }
}

// The `log` frame's head: a member-list fragment continuing the object
// companionSealFrame opens with `{"q":N,` — never a `{` of its own, and every
// member (the optional `reset` and `miss` included) comma-separated; a shape
// slip here seals into a valid-MAC frame no JSON parser accepts.
inline size_t companionLogHead(char out[72], uint32_t boot, bool reset, uint32_t miss) {
  size_t used = static_cast<size_t>(snprintf(out, 72, "\"t\":\"log\",\"b\":%lu,",
                                             static_cast<unsigned long>(boot)));
  if (reset) used += static_cast<size_t>(snprintf(out + used, 72 - used, "\"reset\":true,"));
  if (miss != 0)
    used += static_cast<size_t>(snprintf(out + used, 72 - used, "\"miss\":%lu,",
                                         static_cast<unsigned long>(miss)));
  used += static_cast<size_t>(snprintf(out + used, 72 - used, "\"e\":["));
  return used;
}

// Whether events wait for the link: anything the companion has not got. A
// watermark that moved BACK counts too — the ring was cleared below the
// companion's cursor, and until the reset frame lands its view is wrong, so
// it is as urgent as a warning.
inline bool companionLogPending(uint32_t latest, uint32_t sent) {
  return latest != 0 && latest != sent;
}

inline bool companionLogUrgent(uint32_t urgent, uint32_t latest, uint32_t sent) {
  return (urgent != 0 && static_cast<int32_t>(urgent - sent) > 0) ||
         (latest != 0 && static_cast<int32_t>(latest - sent) < 0);
}

// Whether the network task may flush pending log events now (plan G and
// principle 9). A hold — a scaled shot, a scale connection or discovery, the
// BLE quiet interval — always defers: queue-and-flush, never drop. Outside a
// hold, an urgent event (CRITICAL/ERROR/WARNING, or a cleared ring) or a
// catch-up (subscribe, backlog, a hold that just ended) flushes at once;
// plain events wait for a frame to have arrived since the last flush, so
// they ride the companion's 15/120 s ping and add no radio wakeup of their
// own.
inline bool companionLogFlushDue(bool subscribed, bool pending, bool holdActive,
                                 bool urgentPending, bool catchUp, bool heardSinceFlush) {
  return subscribed && pending && !holdActive &&
         (urgentPending || catchUp || heardSinceFlush);
}

struct CompanionState {
  CompanionStored stored = {};
  CompanionPairing pairing = {};
  CompanionLinkSlot links[2] = {};  // the link, and a newcomer authenticating
  uint32_t generation = 0, active = 0;  // active: the ready session, 0 when down
  uint32_t ups = 0, authFailures = 0;
  uint32_t changedAtMs = 0;   // the link last came up or went down; 0: not since boot
  uint32_t peerProtocol = 0;  // the paired companion's refused protocol; 0: none
  uint32_t lastCommandSent = 0;  // a command id is sent once, across sessions
  // Diagnostic log stream: inbound frames since the last flush (the
  // piggyback heartbeat), when it last flushed, and the events this boot
  // reported missing to the companion.
  bool logHeard = false;
  uint32_t logFlushAtMs = 0, logGaps = 0;
  CompanionRelay relay = {};
  bool dirty = false;
};

// Diagnostic → Companion; strings are sanitized on store.
struct CompanionSnapshot {
  bool paired = false, lmSwitch = false, linkOpen = false, upstream = false, ui = true,
       relayReady = false, logSwitch = false;
  uint32_t relayRejects = 0;
  uint8_t protocol = 0;   // the open link's; 0 while down
  int32_t linkAgeS = -1;  // since the link came up or went down; -1: not since boot
  uint32_t peerProtocol = 0, reconnects = 0, authFailures = 0, routeSwitches = 0, commands = 0;
  // The diagnostic log stream: the switch, the sent cursor, the age of the
  // last flush (-1: never this boot) and the events reported missing.
  uint32_t logCursor = 0, logGaps = 0;
  int32_t logFlushS = -1;
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

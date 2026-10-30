// Companion link, controller side: pairing state machine, stored blob, frame
// and handshake codec, verified against docs/schemas/companion-link-v1.vectors.json.
// The hash primitives are a reference SHA-256/HMAC/HKDF here and PSA on the
// controller; ECDH and AES-GCM run only on the controller (not covered here).

#include "network/OpenBrewByWeightCompanionLink.h"

#include <cJSON.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace openbrewbyweight;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

namespace {

// --- Reference SHA-256 (FIPS 180-4) -----------------------------------------
struct Sha256 {
  uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                       0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t block[64] = {};
  size_t used = 0;
  uint64_t bits = 0;
  static uint32_t rotate(uint32_t value, int count) { return value >> count | value << (32 - count); }
  void compress() {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 |
             uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
    for (int i = 16; i < 64; ++i)
      w[i] = w[i - 16] + (rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3)) +
             w[i - 7] + (rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10));
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5],
             g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) +
                          ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) +
                          ((a & b) ^ (a & c) ^ (b & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
  }
  void update(const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
      block[used++] = bytes[i];
      if (used == 64) { compress(); used = 0; }
    }
    bits += uint64_t(size) * 8;
  }
  void finish(uint8_t out[32]) {
    const uint64_t total = bits;
    const uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (used != 56) update(&zero, 1);
    uint8_t length[8];
    for (int i = 0; i < 8; ++i) length[i] = uint8_t(total >> (56 - 8 * i));
    update(length, 8);
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j) out[4 * i + j] = uint8_t(state[i] >> (24 - 8 * j));
  }
};

std::vector<uint8_t> fromHex(const char *text) {
  std::vector<uint8_t> out;
  for (size_t i = 0; text[i] && text[i + 1]; i += 2) out.push_back(uint8_t(std::stoul(std::string(text + i, 2), nullptr, 16)));
  return out;
}

std::vector<uint8_t> fromBase64(const char *text, size_t size) {
  std::vector<uint8_t> out(size);
  CHECK(companionBase64Decode(text, out.data(), size));
  return out;
}

std::string toBase64(const uint8_t *data, size_t size) {
  char text[256];
  size_t used = 0;
  CHECK(appendBase64(text, sizeof(text), used, data, size));
  return text;
}

cJSON *loadVectors() {
  FILE *file = std::fopen(COMPANION_VECTORS, "rb");
  CHECK(file != nullptr);
  std::string text;
  char chunk[512];
  for (size_t read; (read = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, read);
  std::fclose(file);
  cJSON *root = cJSON_Parse(text.c_str());
  CHECK(root != nullptr);
  return root;
}

const char *field(const cJSON *object, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  CHECK(cJSON_IsString(item));
  return item->valuestring;
}

const char kCompanionId[] = "0b9f6a3c-2f4e-4d7a-9c1b-5e8f0a1d2c3b";
const char kOtherId[] = "1b9f6a3c-2f4e-4d7a-9c1b-5e8f0a1d2c3b";

}  // namespace

// The seam the controller fills with PSA.
namespace openbrewbyweight {
bool companionSha256(const CompanionBytes *parts, size_t count, uint8_t out[32]) {
  Sha256 hash;
  for (size_t i = 0; i < count; ++i) hash.update(parts[i].data, parts[i].size);
  hash.finish(out);
  return true;
}

bool companionHmacSha256(const uint8_t *key, size_t keySize, const CompanionBytes *parts,
                         size_t count, uint8_t out[32]) {
  uint8_t block[64] = {};
  if (keySize > 64) {
    const CompanionBytes whole = {key, keySize};
    companionSha256(&whole, 1, block);
  } else if (keySize > 0) {
    memcpy(block, key, keySize);
  }
  uint8_t pad[64], inner[32];
  for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x36;
  Sha256 first;
  first.update(pad, 64);
  for (size_t i = 0; i < count; ++i) first.update(parts[i].data, parts[i].size);
  first.finish(inner);
  for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x5c;
  Sha256 second;
  second.update(pad, 64);
  second.update(inner, 32);
  second.finish(out);
  return true;
}

bool companionHkdfSha256(const uint8_t *ikm, size_t ikmSize, const uint8_t *salt,
                         size_t saltSize, const char *info, uint8_t out[32]) {
  uint8_t prk[32];
  const CompanionBytes secret = {ikm, ikmSize};
  companionHmacSha256(salt, saltSize, &secret, 1, prk);
  const uint8_t counter = 1;
  const CompanionBytes expand[] = {{info, strlen(info)}, {&counter, 1}};
  return companionHmacSha256(prk, 32, expand, 2, out);
}

// Reference AEAD for the relay envelopes' logic only, where the controller
// runs PSA's AES-256-GCM: an HMAC-SHA256 keystream and tag keep the
// properties the envelopes rely on (key- and nonce-bound, tag-verified)
// without implementing AES here. No cross-implementation vectors exist for
// it; both ends of a real exchange use AES-256-GCM.
namespace {
void aeadKeystream(const uint8_t key[32], const uint8_t nonce[12], size_t block,
                   uint8_t out[32]) {
  const uint8_t index[4] = {uint8_t(block >> 24), uint8_t(block >> 16),
                            uint8_t(block >> 8), uint8_t(block)};
  const CompanionBytes parts[] = {{nonce, 12}, {"\x01", 1}, {index, 4}};
  companionHmacSha256(key, 32, parts, 3, out);
}
}  // namespace

bool companionAeadSealBytes(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *plain, size_t plainSize, uint8_t *out,
                            size_t capacity, size_t &length) {
  if (plainSize + 16 > capacity) return false;
  for (size_t base = 0; base < plainSize; base += 32) {
    uint8_t stream[32];
    aeadKeystream(key, nonce, base / 32, stream);
    for (size_t i = 0; i + base < plainSize && i < 32; ++i) out[base + i] = plain[base + i] ^ stream[i];
  }
  const CompanionBytes tag[] = {{nonce, 12}, {"\x02", 1}, {out, plainSize}};
  uint8_t mac[32];
  companionHmacSha256(key, 32, tag, 3, mac);
  memcpy(out + plainSize, mac, 16);
  length = plainSize + 16;
  return true;
}

bool companionAeadOpenBytes(const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *sealed, size_t sealedSize, uint8_t *out,
                            size_t capacity, size_t &length) {
  if (sealedSize < 16 || sealedSize - 16 > capacity) return false;
  const size_t plainSize = sealedSize - 16;
  const CompanionBytes tag[] = {{nonce, 12}, {"\x02", 1}, {sealed, plainSize}};
  uint8_t mac[32];
  companionHmacSha256(key, 32, tag, 3, mac);
  if (!companionSame(mac, sealed + plainSize, 16)) return false;
  for (size_t base = 0; base < plainSize; base += 32) {
    uint8_t stream[32];
    aeadKeystream(key, nonce, base / 32, stream);
    for (size_t i = 0; i + base < plainSize && i < 32; ++i) out[base + i] = sealed[base + i] ^ stream[i];
  }
  length = plainSize;
  return true;
}
}  // namespace openbrewbyweight

static void testVectors() {
  cJSON *vectors = loadVectors();
  const auto companionKey = fromBase64(field(vectors, "companionKey"), COMPANION_KEY_BYTES);
  const auto controllerKey = fromBase64(field(vectors, "controllerKey"), COMPANION_KEY_BYTES);
  uint8_t commitment[COMPANION_COMMITMENT_BYTES];
  CHECK(companionCommitment(companionKey.data(), commitment));
  CHECK(toBase64(commitment, sizeof(commitment)) == field(vectors, "commitment"));
  uint32_t code = 0;
  CHECK(companionSas(companionKey.data(), controllerKey.data(), code));
  char sas[8];
  std::snprintf(sas, sizeof(sas), "%06u", static_cast<unsigned>(code));
  CHECK(std::string(sas) == field(vectors, "sas"));

  const auto shared = fromHex(field(vectors, "ecdhX"));
  uint8_t secret[32];
  CHECK(companionPairingSecret(shared.data(), companionKey.data(), controllerKey.data(), secret));
  CHECK(fromHex(field(vectors, "pairingSecret")) == std::vector<uint8_t>(secret, secret + 32));

  const auto nc = fromBase64(field(vectors, "companionNonce"), 16);
  const auto ns = fromBase64(field(vectors, "controllerNonce"), 16);
  uint8_t key[32], mac[32];
  CHECK(companionSessionKey(secret, nc.data(), ns.data(), key));
  CHECK(fromHex(field(vectors, "sessionKey")) == std::vector<uint8_t>(key, key + 32));
  CHECK(companionHandshakeMac(secret, 'C', nc.data(), ns.data(), mac));
  CHECK(toBase64(mac, 32) == field(vectors, "proof"));
  CHECK(companionHandshakeMac(secret, 'F', nc.data(), ns.data(), mac));
  CHECK(toBase64(mac, 32) == field(vectors, "ready"));
  CHECK(companionHandshakeMac(secret, 'E', nc.data(), nullptr, mac));
  CHECK(toBase64(mac, 32) == field(vectors, "erase"));

  // Companion → controller: verified, then sequenced.
  const std::string inbound = field(vectors, "companionFrame");
  const char *json = companionOpenFrame(key, inbound.c_str(), inbound.size());
  CHECK(json != nullptr && std::string(json) == inbound.substr(COMPANION_MAC_HEX + 1));
  cJSON *frame = cJSON_Parse(json);
  CHECK(companionFrameSequence(frame, 1) && !companionFrameSequence(frame, 2));
  cJSON_Delete(frame);
  // Controller → companion: the same bytes as the reference.
  char out[COMPANION_FRAME_BYTES + 1];
  const size_t length = companionSealFrame(
      key, 1, "\"t\":\"cfg\",\"lm\":true,\"conn\":\"ws\",\"observe\":true}", out, sizeof(out));
  CHECK(length != 0 && std::string(out, length) == field(vectors, "controllerFrame"));
  const size_t devLength = companionSealFrame(
      key, 1, "\"t\":\"dev\",\"name\":\"openbrewbyweight\"}", out, sizeof(out));
  CHECK(devLength != 0 && std::string(out, devLength) == field(vectors, "deviceFrame"));
  // A frame signed for the other direction is not accepted back.
  CHECK(companionOpenFrame(key, out, length) == nullptr);

  uint8_t credentialsKey[32];
  CHECK(companionCredentialsKey(secret, credentialsKey));
  CHECK(fromHex(field(vectors, "credentialsKey")) ==
        std::vector<uint8_t>(credentialsKey, credentialsKey + 32));
  cJSON_Delete(vectors);
}

// The relay handshake, Kr and signed headers against the shared vectors.
static void testRelayVectors() {
  cJSON *vectors = loadVectors();
  const auto secret = fromHex(field(vectors, "pairingSecret"));
  const cJSON *relay = cJSON_GetObjectItemCaseSensitive(vectors, "relay");
  CHECK(cJSON_IsObject(relay));
  const auto rb = fromBase64(field(relay, "challenge"), 16);
  const auto nc = fromBase64(field(relay, "nonce"), 16);
  uint8_t mac[32], key[32];
  CHECK(companionHandshakeMac(secret.data(), 'R', rb.data(), nc.data(), mac));
  CHECK(toBase64(mac, 32) == field(relay, "request"));
  CHECK(companionHandshakeMac(secret.data(), 'Q', rb.data(), nc.data(), mac));
  CHECK(toBase64(mac, 32) == field(relay, "reply"));
  CHECK(companionHandshakeMac(secret.data(), 'E', nc.data(), nullptr, mac));
  CHECK(toBase64(mac, 32) == field(relay, "erase"));
  CHECK(companionSessionKey(secret.data(), rb.data(), nc.data(), key, "obbw-relay-v1"));
  CHECK(fromHex(field(relay, "key")) == std::vector<uint8_t>(key, key + 32));
  uint8_t bodyKey[32];
  CHECK(companionBodyKey(key, bodyKey));
  CHECK(fromHex(field(relay, "bodyKey")) == std::vector<uint8_t>(bodyKey, bodyKey + 32));
  const cJSON *requests = cJSON_GetObjectItemCaseSensitive(relay, "requests");
  // A screen's call, the companion's shared ui-stream (its relay id, 0.0.0.0)
  // and an IPv6 browser.
  CHECK(cJSON_GetArraySize(requests) == 3);
  for (const cJSON *item = requests->child; item != nullptr; item = item->next) {
    char text[COMPANION_RELAY_HEADER_TEXT];
    std::snprintf(text, sizeof(text), "%s", field(item, "header"));
    CompanionRelayHeader header;
    CHECK(companionRelayParse(text, header));
    CHECK(!strcmp(header.screen, field(item, "screen")));
    CHECK(companionRelayMac(key, field(item, "method"), field(item, "uri"), header,
                            field(item, "session"), mac));
    CHECK(companionSame(mac, header.mac, COMPANION_RELAY_MAC_BYTES));
    // The canonical string is exactly what the MAC covers.
    const std::string canonical = field(item, "canonical");
    const CompanionBytes whole = {canonical.data(), canonical.size()};
    uint8_t reference[32];
    CHECK(companionHmacSha256(key, 32, &whole, 1, reference) && !memcmp(reference, mac, 32));
    // Any other method, path, query or session is another request.
    CHECK(companionRelayMac(key, field(item, "method"), "/api/v1/control/stop", header,
                            field(item, "session"), mac));
    CHECK(!companionSame(mac, header.mac, COMPANION_RELAY_MAC_BYTES));
    CHECK(companionRelayMac(key, field(item, "method"), field(item, "uri"), header, "x", mac));
    CHECK(!companionSame(mac, header.mac, COMPANION_RELAY_MAC_BYTES));
  }
  cJSON_Delete(vectors);
}

static void testRelayHeader() {
  const auto parse = [](const char *value, CompanionRelayHeader &header) {
    char text[COMPANION_RELAY_HEADER_TEXT];
    std::snprintf(text, sizeof(text), "%s", value);
    return companionRelayParse(text, header);
  };
  const std::string mac(32, 'a'), screen = "0123456789abcdef";
  CompanionRelayHeader header;
  CHECK(parse(("2 4294967295 10.0.0.2 " + screen + " " + mac).c_str(), header) &&
        header.sequence == UINT32_MAX && header.mac[0] == 0xAA);
  CHECK(parse(("2 1 2001:db8::a:1 " + screen + " " + mac).c_str(), header) && header.sequence == 1);
  const std::string bad[] = {
      "1 1 10.0.0.2 " + screen + " " + mac,                 // version
      "2 0 10.0.0.2 " + screen + " " + mac,                 // q from 1
      "2 01 10.0.0.2 " + screen + " " + mac,                // canonical q
      "2 4294967296 10.0.0.2 " + screen + " " + mac,        // q is 32-bit
      "2 1x 10.0.0.2 " + screen + " " + mac,
      "2 1 10.0.0.2\" " + screen + " " + mac,               // address characters
      "2 1 1 " + screen + " " + mac,                        // address too short
      "2 1 " + std::string(46, '1') + " " + screen + " " + mac,
      "2 1 10.0.0.2 0123456789ABCDEF " + mac,               // lowercase screen
      "2 1 10.0.0.2 0123456789abcde " + mac,                // 15 digits
      "2 1 10.0.0.2 " + std::string(25, 'a') + " " + mac,   // 25 digits
      "2 1 10.0.0.2 " + screen + " " + mac.substr(1),       // short MAC
      "2 1 10.0.0.2 " + screen + " " + mac + "a",
      "2 1 10.0.0.2 " + screen + " " + mac + " ",           // trailing field
      "2  1 10.0.0.2 " + screen + " " + mac,                // double space
      "2 1 10.0.0.2 " + screen,
  };
  for (const std::string &value : bad) CHECK(!parse(value.c_str(), header));
}

// Relay protocol 2's seals: Kb, the nonce layout, and the envelope helpers
// (docs/COMPANION_LINK.md#sealed-bodies-and-headers-protocol-2).
static void testRelayEnvelope() {
  uint8_t relayKey[32], bodyKey[32], other[32];
  memset(relayKey, 7, sizeof(relayKey));
  CHECK(companionBodyKey(relayKey, bodyKey));
  CHECK(memcmp(bodyKey, relayKey, 32) != 0);
  CHECK(companionBodyKey(relayKey, other) && !memcmp(bodyKey, other, 32));  // deterministic
  relayKey[0] ^= 1;
  CHECK(companionBodyKey(relayKey, other) && memcmp(bodyKey, other, 32) != 0);

  uint8_t nonce[12];
  companionRelayNonce('c', COMPANION_RELAY_CTX_SESSION, 0x01020304, nonce);
  const uint8_t layout[12] = {'c', 0, 0, 0, 4, 3, 2, 1, 0, 0, 0, 0};
  CHECK(!memcmp(nonce, layout, 12));
  uint8_t response[12];
  companionRelayNonce('f', COMPANION_RELAY_CTX_RESPONSE, 1, response);
  CHECK(memcmp(nonce, response, 12) != 0);

  const std::string session = "0123456789abcdef0123456789abcdef";
  char sealed[COMPANION_RELAY_SECRET_TEXT], opened[64];
  CHECK(companionSealSecret(bodyKey, COMPANION_RELAY_CTX_SESSION, 42, session.c_str(), sealed));
  CHECK(companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_SESSION, 42, sealed, opened,
                            sizeof(opened)) &&
        session == opened);
  CHECK(!companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_PASSWORD, 42, sealed, opened,
                             sizeof(opened)));  // another context's seal
  CHECK(!companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_SESSION, 43, sealed, opened,
                             sizeof(opened)));  // another request's seal
  CHECK(!companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_SESSION, 42, "!!!!", opened,
                             sizeof(opened)));  // not a sealed value
  sealed[0] = sealed[0] == 'A' ? 'B' : 'A';
  CHECK(!companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_SESSION, 42, sealed, opened,
                             sizeof(opened)));  // tampered

  const std::string password(63, 'x');
  CHECK(companionSealSecret(bodyKey, COMPANION_RELAY_CTX_PASSWORD, 9, password.c_str(), sealed));
  CHECK(companionOpenSecret(bodyKey, COMPANION_RELAY_CTX_PASSWORD, 9, sealed, opened,
                            sizeof(opened)) &&
        password == opened);
  const std::string tooLong(66, 'x');
  CHECK(!companionSealSecret(bodyKey, COMPANION_RELAY_CTX_PASSWORD, 9, tooLong.c_str(), sealed));

  const std::string login = "{\"password\":\"ineedacoffee\"}";
  uint8_t sealedBody[96], openedBody[96], requestNonce[12];
  size_t sealedLength = 0, openedLength = 0;
  companionRelayNonce('c', COMPANION_RELAY_CTX_REQUEST, 5, requestNonce);
  CHECK(companionAeadSealBytes(bodyKey, requestNonce,
                               reinterpret_cast<const uint8_t *>(login.data()), login.size(),
                               sealedBody, sizeof(sealedBody), sealedLength) &&
        sealedLength == login.size() + 16);
  CHECK(companionAeadOpenBytes(bodyKey, requestNonce, sealedBody, sealedLength, openedBody,
                               sizeof(openedBody), openedLength) &&
        openedLength == login.size() && !memcmp(openedBody, login.data(), login.size()));
  sealedBody[0] ^= 1;
  CHECK(!companionAeadOpenBytes(bodyKey, requestNonce, sealedBody, sealedLength, openedBody,
                                sizeof(openedBody), openedLength));  // tampered range
  sealedBody[0] ^= 1;
  CHECK(!companionAeadOpenBytes(bodyKey, response, sealedBody, sealedLength, openedBody,
                                sizeof(openedBody), openedLength));  // another nonce

  CompanionRelay relay;
  relay.ready = true;
  relay.responses = 7;
  memset(relay.bodyKey, 9, sizeof(relay.bodyKey));
  companionRelayDrop(relay);
  // Keys drop with the session; the response counter orders nonces, never resets.
  bool cleared = true;
  for (const uint8_t byte : relay.bodyKey) cleared = cleared && byte == 0;
  CHECK(!relay.ready && cleared && relay.responses == 7);
}

static void testRelayWindow() {
  CompanionRelayWindow window;
  CHECK(!companionRelayAccept(window, 0));
  CHECK(companionRelayAccept(window, 1) && !companionRelayAccept(window, 1));
  CHECK(companionRelayAccept(window, 3) && companionRelayAccept(window, 2));  // out of order
  CHECK(!companionRelayAccept(window, 2) && !companionRelayAccept(window, 3));
  CHECK(companionRelayAccept(window, 66) && window.high == 66);
  CHECK(!companionRelayAccept(window, 2));  // older than the window
  CHECK(companionRelayAccept(window, 4));   // 66 - 62: still inside, never seen
  CHECK(!companionRelayAccept(window, 4) && !companionRelayAccept(window, 66));
  CHECK(companionRelayAccept(window, 1000) && window.mask == 1);  // a jump clears the bitmap
  CHECK(!companionRelayAccept(window, 936) && companionRelayAccept(window, 937));
  CHECK(companionRelayAccept(window, UINT32_MAX) && !companionRelayAccept(window, UINT32_MAX));
  CompanionRelay relay;
  relay.ready = true;
  relay.key[0] = 9;
  relay.window = window;
  relay.rejects = 4;
  companionRelayDrop(relay);
  CHECK(!relay.ready && relay.key[0] == 0 && relay.window.high == 0 && relay.rejects == 4);
}

// Admin unlock and the override belong to one principal; a companion screen
// and a direct window never stand in for each other, even with the same id.
static void testPrincipals() {
  CompanionPrincipal direct, screen, other, empty;
  std::snprintf(direct.id, sizeof(direct.id), "0123456789abcdef");
  screen = direct;
  screen.relayed = true;
  other = screen;
  other.id[0] = '1';
  CHECK(companionPrincipalHolds(direct, direct) && companionPrincipalHolds(screen, screen));
  CHECK(!companionPrincipalHolds(direct, screen) && !companionPrincipalHolds(screen, direct));
  CHECK(!companionPrincipalHolds(screen, other) && !companionPrincipalHolds(empty, empty));
  char tag[9];
  companionScreenTag(direct, tag);
  CHECK(tag[0] == '\0');
  companionScreenTag(empty, tag);
  CHECK(tag[0] == '\0');
  companionScreenTag(screen, tag);
  char expected[9];
  std::snprintf(expected, sizeof(expected), "%08lx",
                static_cast<unsigned long>(webAccessFnv(screen.id, strlen(screen.id))));
  CHECK(strlen(tag) == 8 && !strcmp(tag, expected));
}

static void testFrameCodec() {
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = uint8_t(i * 7 + 1);
  char out[COMPANION_FRAME_BYTES + 1];
  // Sign as the companion would (direction c) to exercise the receive side.
  const std::string json = "{\"q\":3,\"t\":\"ping\",\"up\":false}";
  char hex[COMPANION_MAC_HEX];
  CHECK(companionFrameMacHex(key, 'c', json.data(), json.size(), hex));
  std::string text = std::string(hex, COMPANION_MAC_HEX) + "." + json;
  CHECK(companionOpenFrame(key, text.c_str(), text.size()) != nullptr);
  std::string tampered = text;
  tampered[tampered.size() - 2] = 'u';  // body changed
  CHECK(companionOpenFrame(key, tampered.c_str(), tampered.size()) == nullptr);
  tampered = text;
  tampered[0] = tampered[0] == 'a' ? 'b' : 'a';  // MAC changed
  CHECK(companionOpenFrame(key, tampered.c_str(), tampered.size()) == nullptr);
  tampered = text;
  for (size_t i = 0; i < COMPANION_MAC_HEX; ++i)
    if (tampered[i] >= 'a' && tampered[i] <= 'f') tampered[i] = char(tampered[i] - 32);
  CHECK(tampered == text || companionOpenFrame(key, tampered.c_str(), tampered.size()) == nullptr);
  tampered = text;
  tampered[COMPANION_MAC_HEX] = ':';  // separator
  CHECK(companionOpenFrame(key, tampered.c_str(), tampered.size()) == nullptr);
  CHECK(companionOpenFrame(key, text.c_str(), COMPANION_MAC_HEX + 1) == nullptr);
  // Another session key rejects the frame.
  uint8_t other[32];
  memcpy(other, key, 32);
  other[31] ^= 1;
  CHECK(companionOpenFrame(other, text.c_str(), text.size()) == nullptr);
  // Ordering: the controller expects exactly the next sequence.
  cJSON *frame = cJSON_Parse(json.c_str());
  CHECK(companionFrameSequence(frame, 3));
  CHECK(!companionFrameSequence(frame, 2) && !companionFrameSequence(frame, 4));
  cJSON_Delete(frame);
  frame = cJSON_Parse("{\"q\":\"3\",\"t\":\"ping\"}");
  CHECK(!companionFrameSequence(frame, 3));
  cJSON_Delete(frame);
  // Oversize: a whole frame stays within 4096 bytes either way.
  std::string body = "\"t\":\"cmd\",\"pad\":\"" + std::string(4096, 'x') + "\"}";
  CHECK(companionSealFrame(key, 1, body.c_str(), out, sizeof(out)) == 0);
  body = "\"t\":\"x\",\"p\":\"" + std::string(COMPANION_FRAME_BYTES - 55, 'x') + "\"}";
  const size_t fits = companionSealFrame(key, 1, body.c_str(), out, sizeof(out));
  CHECK(fits == COMPANION_FRAME_BYTES);
  body.insert(body.size() - 2, "x");
  CHECK(companionSealFrame(key, 1, body.c_str(), out, sizeof(out)) == 0);
  CHECK(companionSealFrame(key, 1, "\"t\":\"x\"}", out, 40) == 0);
  std::string huge(COMPANION_FRAME_BYTES + 1, 'x');
  memcpy(&huge[0], text.data(), text.size());
  CHECK(companionOpenFrame(key, huge.c_str(), huge.size()) == nullptr);
}

// The `log` frame's body is a member-list fragment continuing the object the
// seal opens with `{"q":N,`. A head with a `{` of its own seals into a
// valid-MAC frame whose JSON no parser accepts — the shape this pins.
static void testLogFrameShape() {
  uint8_t key[32];
  for (int i = 0; i < 32; ++i) key[i] = uint8_t(i * 5 + 3);
  char out[COMPANION_FRAME_BYTES + 1];
  const struct {
    bool reset;
    uint32_t miss;
  } cases[] = {{false, 0}, {false, 12}, {true, 0}, {true, 34}};
  for (const auto &item : cases) {
    char head[72];
    CHECK(companionLogHead(head, 42, item.reset, item.miss) > 0);
    CHECK(head[0] == '"');  // continues the object; never a nested one
    std::string body = std::string(head) + "[7,89,1777000000,\"w\",\"network\",\"up\"]" + "]}";
    const size_t length = companionSealFrame(key, 5, body.c_str(), out, sizeof(out));
    CHECK(length > 0);
    // The companion's two steps on any frame: the MAC (direction f to it),
    // then the JSON. companionOpenFrame verifies direction c and cannot read
    // the controller's own sealing, so check the MAC directly.
    char mac[COMPANION_MAC_HEX];
    CHECK(companionFrameMacHex(key, 'f', out + COMPANION_MAC_HEX + 1,
                               length - COMPANION_MAC_HEX - 1, mac));
    CHECK(std::memcmp(out, mac, COMPANION_MAC_HEX) == 0);
    cJSON *frame = cJSON_Parse(out + COMPANION_MAC_HEX + 1);
    CHECK(frame != nullptr);
    CHECK(companionFrameSequence(frame, 5));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(frame, "t");
    CHECK(cJSON_IsString(type) && std::strcmp(type->valuestring, "log") == 0);
    const cJSON *boot = cJSON_GetObjectItemCaseSensitive(frame, "b");
    CHECK(cJSON_IsNumber(boot) && boot->valuedouble == 42);
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(frame, "e");
    CHECK(cJSON_IsArray(events) && cJSON_GetArraySize(events) == 1);
    const cJSON *reset = cJSON_GetObjectItemCaseSensitive(frame, "reset");
    CHECK(item.reset ? cJSON_IsBool(reset) : reset == nullptr);
    const cJSON *miss = cJSON_GetObjectItemCaseSensitive(frame, "miss");
    CHECK(item.miss != 0 ? (cJSON_IsNumber(miss) && miss->valuedouble == item.miss)
                         : miss == nullptr);
    cJSON_Delete(frame);
  }
}

// The dev frame is due on every fresh session (pairing, reconnect, reboot)
// and after a rename, and never for an unpublished (empty) name.
static void testDeviceNameFrame() {
  CompanionLinkSlot slot;
  CHECK(companionDeviceNameDue(slot, "openbrewbyweight"));
  std::snprintf(slot.sentDeviceName, sizeof(slot.sentDeviceName), "%s", "openbrewbyweight");
  CHECK(!companionDeviceNameDue(slot, "openbrewbyweight"));
  CHECK(companionDeviceNameDue(slot, "Kitchen Bar"));
  CHECK(!companionDeviceNameDue(slot, ""));
}

static void testBase64AndValidation() {
  uint8_t bytes[16];
  CHECK(companionBase64Decode("AAECAwQFBgcICQoLDA0ODw==", bytes, 16) && bytes[15] == 15);
  CHECK(!companionBase64Decode("AAECAwQFBgcICQoLDA0ODw=", bytes, 16));   // short
  CHECK(!companionBase64Decode("AAECAwQFBgcICQoLDA0ODx==", bytes, 16));  // non-canonical
  CHECK(!companionBase64Decode("AAECAwQFBgcICQoLDA0OD*==", bytes, 16));  // alphabet
  CHECK(!companionBase64Decode("AAECAwQFBgcICQoLDA0ODw==", bytes, 15));  // length
  CHECK(!companionBase64Decode(nullptr, bytes, 16));
  CHECK(companionIdValid(kCompanionId) && companionIdValid("0B9F6A3C-2F4E-4D7A-9C1B-5E8F0A1D2C3B"));
  CHECK(!companionIdValid("0b9f6a3c-2f4e-4d7a-9c1b-5e8f0a1d2c3") &&
        !companionIdValid("0b9f6a3c+2f4e-4d7a-9c1b-5e8f0a1d2c3b") &&
        !companionIdValid("0b9f6a3c-2f4e-4d7a-9c1b-5e8f0a1d2c3g") && !companionIdValid(nullptr));
  CHECK(companionTextValid("Kitchen \xC2\xB7 server", 1, 32, false));
  CHECK(!companionTextValid("", 1, 32, false) && !companionTextValid("a\nb", 1, 32, false));
  CHECK(!companionTextValid(std::string(33, 'n').c_str(), 1, 32, false));
  CHECK(companionTextValid("", 0, 24, true) && companionTextValid("1.0.0-rc.1", 0, 24, true));
  CHECK(!companionTextValid("1.0\xC2\xB7", 0, 24, true));
  // Peers: RFC 1918, link-local and loopback only.
  CHECK(companionPeerAllowedV4(0x0A000001) && companionPeerAllowedV4(0xAC1F0001) &&
        companionPeerAllowedV4(0xC0A80102) && companionPeerAllowedV4(0xA9FE0001) &&
        companionPeerAllowedV4(0x7F000001));
  CHECK(!companionPeerAllowedV4(0xAC200001) && !companionPeerAllowedV4(0x08080808) &&
        !companionPeerAllowedV4(0x64400001));
  uint8_t v6[16] = {0xFE, 0x80};
  CHECK(companionPeerAllowedV6(v6));
  const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(companionPeerAllowedV6(loopback));
  const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 192, 168, 1, 9};
  const uint8_t mappedPublic[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 8, 8, 8, 8};
  const uint8_t global[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  const uint8_t unique[16] = {0xFD, 0x12};
  CHECK(companionPeerAllowedV6(mapped) && !companionPeerAllowedV6(mappedPublic) &&
        !companionPeerAllowedV6(global) && !companionPeerAllowedV6(unique));
}

// The controller's key went out, then the companion revealed `key`.
static void reveal(CompanionPairing &pairing, const uint8_t key[COMPANION_KEY_BYTES], uint32_t now) {
  pairing.request.keyReady = pairing.request.keyIssued = true;
  CHECK(companionPairReveal(pairing, pairing.request.id, key, true, now));
}

static void testCommitThenReveal() {
  uint8_t key[COMPANION_KEY_BYTES] = {0x04, 7, 7, 7}, commitment[COMPANION_COMMITMENT_BYTES];
  CHECK(companionCommitment(key, commitment));
  uint32_t now = 1000;
  CompanionPairing pairing;
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now) == CompanionPairResult::NEW);
  // Nothing to reveal for another id; no code before the companion's key.
  CHECK(!companionPairReveal(pairing, kOtherId, key, true, now));
  pairing.request.keyReady = true;
  CHECK(!companionCodeReady(pairing.request));
  CHECK(companionPairApprove(pairing, 1, 1, true, true, now) == CompanionApproveResult::NOT_READY);
  // Revealing before controllerKey was issued shows a device in the middle.
  CHECK(!companionPairReveal(pairing, kCompanionId, key, true, now));
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "denied"));
  // A key that does not match the commitment, or is off the curve, denies too.
  for (int variant = 0; variant < 2; ++variant) {
    now += COMPANION_NEW_REQUEST_MS;
    CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                               commitment, now) == CompanionPairResult::NEW);
    pairing.request.keyReady = pairing.request.keyIssued = true;
    uint8_t other[COMPANION_KEY_BYTES];
    memcpy(other, key, sizeof(other));
    other[1] ^= variant == 0 ? 1 : 0;
    CHECK(!companionPairReveal(pairing, kCompanionId, other, variant == 0, now));
    CHECK(pairing.request.state == CompanionPairState::DENIED && !pairing.request.revealed);
  }
  // The matching key opens the code; asking or revealing again changes nothing.
  now += COMPANION_NEW_REQUEST_MS;
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now) == CompanionPairResult::NEW);
  reveal(pairing, key, now);
  CHECK(companionCodeReady(pairing.request) && !memcmp(pairing.request.companionKey, key, sizeof(key)));
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now + 1) == CompanionPairResult::PENDING);
  CHECK(companionPairReveal(pairing, kCompanionId, key, true, now + 2) && companionCodeReady(pairing.request));
  CHECK(companionPairApprove(pairing, 5, 5, true, true, now) == CompanionApproveResult::OK);
  // A retry while the secret is derived is harmless; another key is refused, not denied.
  CHECK(companionPairReveal(pairing, kCompanionId, key, true, now + 3));
  uint8_t other[COMPANION_KEY_BYTES];
  memcpy(other, key, sizeof(other));
  other[2] ^= 1;
  CHECK(!companionPairReveal(pairing, kCompanionId, other, true, now + 3));
  CHECK(pairing.request.state == CompanionPairState::PENDING && pairing.request.approving);
}

static void testPairing() {
  CompanionPairing pairing;
  CompanionStored stored;
  uint8_t key[COMPANION_KEY_BYTES] = {0x04, 1, 2, 3}, commitment[COMPANION_COMMITMENT_BYTES];
  CHECK(companionCommitment(key, commitment));
  uint32_t now = 1000;
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "unknown"));
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen \"server\"", "1.0.0",
                             "192.168.1.9", commitment, now) == CompanionPairResult::NEW);
  CHECK(!strcmp(pairing.request.name, "Kitchen server"));  // display copy is sanitized
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "pending"));
  CHECK(!strcmp(companionPairPoll(pairing, kOtherId, now), "unknown"));
  // The same companion polling with POST again keeps its request.
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now + 100) == CompanionPairResult::PENDING);
  CHECK(pairing.request.serial == 1);
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", commitment,
                             now + 6000) == CompanionPairResult::BUSY);
  // The code waits for both keys, then three wrong codes deny it.
  CHECK(companionPairApprove(pairing, 1, 1, true, true, now) == CompanionApproveResult::NOT_READY);
  reveal(pairing, key, now);
  CHECK(companionPairApprove(pairing, 1, 2, true, true, now) == CompanionApproveResult::WRONG_CODE);
  CHECK(companionPairApprove(pairing, 1, 2, true, true, now) == CompanionApproveResult::WRONG_CODE);
  CHECK(companionPairApprove(pairing, 1, 2, true, true, now) == CompanionApproveResult::DENIED);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "denied"));
  CHECK(companionPairApprove(pairing, 2, 2, true, true, now) == CompanionApproveResult::NOT_PENDING);
  // One new request per 5 s.
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", commitment,
                             now + 4999) == CompanionPairResult::RATE_LIMITED);
  now += 6000;
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", commitment,
                             now) == CompanionPairResult::NEW);
  CHECK(pairing.request.serial == 2 && !pairing.request.keyReady);
  // Ignore denies the request and refuses the id until Stop ignoring.
  CHECK(companionPairIgnore(pairing, now));
  CHECK(!companionPairIgnore(pairing, now));
  CHECK(companionIgnoredCount(pairing) == 1);
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", commitment,
                             now + 10000) == CompanionPairResult::IGNORED);
  companionUnignore(pairing);
  CHECK(companionIgnoredCount(pairing) == 0);
  // Expiry after ten minutes.
  now += 10000;
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now) == CompanionPairResult::NEW);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now + COMPANION_REQUEST_MS - 1), "pending"));
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now + COMPANION_REQUEST_MS), "expired"));
  now += COMPANION_REQUEST_MS + 1;
  // A changed commitment from the same companion is a new request.
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now) == CompanionPairResult::NEW);
  key[5] = 9;
  CHECK(companionCommitment(key, commitment));
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now + 1000) == CompanionPairResult::RATE_LIMITED);
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             commitment, now + 5000) == CompanionPairResult::NEW);
  now += 5000;
  // Approval: the network task derives S, which makes the pairing.
  reveal(pairing, key, now);
  CHECK(companionPairApprove(pairing, 7, 7, false, true, now) == CompanionApproveResult::OK);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now + COMPANION_REQUEST_MS + 5), "pending"));
  CHECK(companionPairApprove(pairing, 7, 7, false, true, now) == CompanionApproveResult::NOT_PENDING);
  CHECK(!companionPairIgnore(pairing, now));
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", commitment,
                             now + 6000) == CompanionPairResult::BUSY);
  uint8_t secret[COMPANION_SECRET_BYTES];
  memset(secret, 0x5A, sizeof(secret));
  stored.forgotten = 1;
  memcpy(stored.forgottenId, kCompanionId, sizeof(kCompanionId));
  companionPairComplete(pairing, stored, secret);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "approved"));
  CHECK(stored.paired == 1 && stored.lmSwitch == 0 && stored.forgotten == 0 &&
        !strcmp(stored.id, kCompanionId) && !strcmp(stored.address, "192.168.1.9") &&
        !strcmp(stored.name, "Kitchen") && stored.publicKey[5] == 9 && stored.secret[0] == 0x5A);
  CHECK(companionPairRequest(pairing, true, kOtherId, "Other", "", "192.168.1.10", commitment,
                             now + 60000) == CompanionPairResult::ALREADY_PAIRED);

  // Hello verdicts, then Forget leaves {id, secret} for the signed erase.
  CHECK(companionHelloVerdict(stored, kCompanionId, 1) == CompanionHello::CHALLENGE);
  CHECK(companionHelloVerdict(stored, kCompanionId, 2) == CompanionHello::PROTOCOL);
  CHECK(companionHelloVerdict(stored, kOtherId, 1) == CompanionHello::NOT_PAIRED);
  CHECK(companionForget(stored) && !companionForget(stored));
  CHECK(stored.paired == 0 && stored.secret[0] == 0 && stored.id[0] == '\0' &&
        stored.forgotten == 1 && stored.forgottenSecret[0] == 0x5A);
  CHECK(companionHelloVerdict(stored, kCompanionId, 1) == CompanionHello::ERASE);
  CHECK(companionHelloVerdict(stored, kOtherId, 1) == CompanionHello::NOT_PAIRED);
  // Pairing another companion keeps the erase for the forgotten one.
  CompanionPairing next;
  CHECK(companionPairRequest(next, false, kOtherId, "Other", "", "192.168.1.10", commitment, now) ==
        CompanionPairResult::NEW);
  reveal(next, key, now);
  CHECK(companionPairApprove(next, 3, 3, true, true, now) == CompanionApproveResult::OK);
  companionPairComplete(next, stored, secret);
  CHECK(stored.paired == 1 && stored.forgotten == 1 &&
        companionHelloVerdict(stored, kCompanionId, 1) == CompanionHello::ERASE &&
        companionHelloVerdict(stored, kOtherId, 1) == CompanionHello::CHALLENGE);

  // The ignore list keeps the newest ids.
  CompanionPairing many;
  char id[COMPANION_ID_TEXT];
  for (int i = 0; i < COMPANION_IGNORED_IDS + 1; ++i) {
    std::snprintf(id, sizeof(id), "%08d-2f4e-4d7a-9c1b-5e8f0a1d2c3b", i);
    CHECK(companionPairRequest(many, false, id, "n", "", "10.0.0.2", commitment, 100000u * (i + 1)) ==
          CompanionPairResult::NEW);
    CHECK(companionPairIgnore(many, 100000u * (i + 1)));
  }
  CHECK(companionIgnoredCount(many) == COMPANION_IGNORED_IDS);
  CHECK(!companionIgnored(many, "00000000-2f4e-4d7a-9c1b-5e8f0a1d2c3b"));
  CHECK(companionIgnored(many, id));
}

static void testStoredBlob() {
  CompanionStored stored;
  CHECK(!validCompanionStored(stored));  // never finalized
  stored.paired = 1;
  memcpy(stored.id, kCompanionId, sizeof(kCompanionId));
  finalizeCompanionStored(stored);
  CHECK(validCompanionStored(stored));
  CompanionStored copy = stored;
  copy.secret[3] ^= 1;
  CHECK(!validCompanionStored(copy));
  copy = stored;
  copy.version = COMPANION_STORED_VERSION + 1;
  CHECK(!validCompanionStored(copy));
  copy = stored;
  copy.magic ^= 1;
  CHECK(!validCompanionStored(copy));
  copy = stored;
  copy.paired = 2;
  copy.checksum = companionChecksum(copy);
  CHECK(!validCompanionStored(copy));
  copy = stored;
  copy.structureSize = 1;
  copy.checksum = companionChecksum(copy);
  CHECK(!validCompanionStored(copy));
  // The Web UI switch lives in the former reserved byte: older blobs read as on.
  CHECK(stored.uiOff == 0);
  copy = stored;
  copy.uiOff = 1;
  finalizeCompanionStored(copy);
  CHECK(validCompanionStored(copy));
  copy.uiOff = 2;
  copy.checksum = companionChecksum(copy);
  CHECK(!validCompanionStored(copy));
  CHECK(offsetof(CompanionStored, uiOff) == 11);
  // The log switch rides as the v2 tail, right after the checksum; the last
  // three bytes are alignment padding.
  CHECK(offsetof(CompanionStored, logSwitch) == 328);
  CHECK(sizeof(CompanionStored) == 332);
  copy = stored;
  copy.logSwitch = 2;
  copy.checksum = companionChecksum(copy);
  CHECK(!validCompanionStored(copy));
}

static void testStoredV1Promotion() {
  // A version 1 blob: the same fields, no logSwitch tail (328 bytes).
  CompanionStored v1;
  v1.paired = 1;
  memcpy(v1.id, kCompanionId, sizeof(kCompanionId));
  memcpy(v1.secret, "0123456789abcdef0123456789abcdef", 32);
  v1.version = 1;
  v1.structureSize = 328;
  v1.checksum = companionChecksum(v1);
  CHECK(validCompanionStoredV1(v1));
  CHECK(!validCompanionStored(v1));  // not v2 until promoted
  CompanionStored promoted = v1;
  promoteCompanionStoredV1(promoted);
  CHECK(validCompanionStored(promoted));  // re-finalized as v2
  CHECK(promoted.logSwitch == 1);        // the switches default on
  CHECK(promoted.paired == 1 && promoted.version == COMPANION_STORED_VERSION);
  CHECK(memcmp(promoted.secret, v1.secret, 32) == 0);  // the pairing survives
  // A corrupted v1 blob does not promote.
  CompanionStored bad = v1;
  bad.secret[7] ^= 0x40;
  CHECK(!validCompanionStoredV1(bad));
  // A v2 blob is not v1 either.
  CHECK(!validCompanionStoredV1(promoted));
}

static void testLogFlushDue() {
  // (subscribed, pending, hold, urgent, catchUp, heard)
  // Not subscribed: nothing flows even with everything else true.
  CHECK(!companionLogFlushDue(false, true, false, true, true, true));
  // Nothing pending: nothing to flush.
  CHECK(!companionLogFlushDue(true, false, false, true, true, true));
  // A hold always defers: queue-and-flush, never drop (plan G, principle 9).
  CHECK(!companionLogFlushDue(true, true, true, true, true, true));
  // Plain events wait for a frame since the last flush (the ping heartbeat).
  CHECK(!companionLogFlushDue(true, true, false, false, false, false));
  CHECK(companionLogFlushDue(true, true, false, false, false, true));
  // A catch-up (subscribe, backlog, a hold that just ended) flushes at once.
  CHECK(companionLogFlushDue(true, true, false, false, true, false));
  // An urgent event (CRITICAL/ERROR/WARNING) flushes at once outside holds.
  CHECK(companionLogFlushDue(true, true, false, true, false, false));
  // A watermark that moved back — the ring cleared below the companion's
  // cursor — is pending and urgent: without this, new events after a clear
  // never look newer than the stale cursor and the stream stays silent.
  CHECK(companionLogPending(3, 500));
  CHECK(companionLogUrgent(0, 3, 500));
  CHECK(companionLogUrgent(2, 3, 500));
  CHECK(!companionLogPending(500, 500));
  CHECK(!companionLogPending(0, 500));
  CHECK(companionLogPending(501, 500));
  CHECK(!companionLogUrgent(0, 501, 500));
  CHECK(companionLogUrgent(501, 501, 500));
  CHECK(companionLogLevelLetter(LogLevel::CRITICAL) == 'c');
  CHECK(companionLogLevelLetter(LogLevel::ERROR) == 'e');
  CHECK(companionLogLevelLetter(LogLevel::WARNING) == 'w');
  CHECK(companionLogLevelLetter(LogLevel::INFO) == 'i');
  CHECK(companionLogLevelLetter(LogLevel::DEBUG) == 'd');
  // The ring tracks the newest urgent event for that flush.
  DebugRingBuffer ring;
  CHECK(ring.latestAlarmSequence() == 0);
  ring.add(1000, 0, LogLevel::INFO, DebugCategory::STATE, DebugCode::STATE_TRANSITION);
  CHECK(ring.latestAlarmSequence() == 0);
  ring.add(2000, 0, LogLevel::WARNING, DebugCategory::SYSTEM, DebugCode::BOOT_BANNER);
  CHECK(ring.latestAlarmSequence() == 2);
  ring.add(3000, 0, LogLevel::ERROR, DebugCategory::NETWORK, DebugCode::STATE_TRANSITION);
  ring.add(4000, 0, LogLevel::DEBUG, DebugCategory::STATE, DebugCode::STATE_TRANSITION);
  CHECK(ring.latestAlarmSequence() == 3);
  ring.clear();
  CHECK(ring.latestAlarmSequence() == 0);
}

static void testLiveness() {
  CompanionLinkSlot slot;
  CHECK(companionSilenceLimitMs(slot, MicraCompanionRoute::DIRECT) == 10000);
  slot.phase = CompanionLinkPhase::READY;
  CHECK(companionSilenceLimitMs(slot, MicraCompanionRoute::DIRECT) == 250000);
  CHECK(companionSilenceLimitMs(slot, MicraCompanionRoute::COMPANION) == 35000);
  // A route request on a link idle for 100 s (pings every 120 s while direct)
  // keeps the link: only the companion route pings every 15 s.
  const uint32_t now = 500000;
  slot.heardAtMs = now - 100000;
  CHECK(now - slot.heardAtMs < companionSilenceLimitMs(slot, MicraCompanionRoute::PENDING));
  // "Since" covers both states, and is unknown until the first change.
  CompanionState state;
  CHECK(companionLinkAgeS(state, now) == -1);
  state.changedAtMs = now - 61000;
  CHECK(companionLinkAgeS(state, now) == 61);
}

int main() {
  testVectors();
  testRelayVectors();
  testRelayHeader();
  testRelayEnvelope();
  testRelayWindow();
  testPrincipals();
  testFrameCodec();
  testLogFrameShape();
  testDeviceNameFrame();
  testBase64AndValidation();
  testCommitThenReveal();
  testPairing();
  testStoredBlob();
  testStoredV1Promotion();
  testLogFlushDue();
  testLiveness();
  std::puts("companion_link_host_test: ok");
  return 0;
}

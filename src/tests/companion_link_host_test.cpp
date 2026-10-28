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
}  // namespace openbrewbyweight

static void testVectors() {
  cJSON *vectors = loadVectors();
  const auto companionKey = fromBase64(field(vectors, "companionKey"), COMPANION_KEY_BYTES);
  const auto controllerKey = fromBase64(field(vectors, "controllerKey"), COMPANION_KEY_BYTES);
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
  // A frame signed for the other direction is not accepted back.
  CHECK(companionOpenFrame(key, out, length) == nullptr);

  uint8_t credentialsKey[32];
  CHECK(companionCredentialsKey(secret, credentialsKey));
  CHECK(fromHex(field(vectors, "credentialsKey")) ==
        std::vector<uint8_t>(credentialsKey, credentialsKey + 32));
  cJSON_Delete(vectors);
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

static void testPairing() {
  CompanionPairing pairing;
  CompanionStored stored;
  uint8_t key[COMPANION_KEY_BYTES] = {0x04, 1, 2, 3};
  uint32_t now = 1000;
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "unknown"));
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen \"server\"", "1.0.0",
                             "192.168.1.9", key, now) == CompanionPairResult::NEW);
  CHECK(!strcmp(pairing.request.name, "Kitchen server"));  // display copy is sanitized
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "pending"));
  CHECK(!strcmp(companionPairPoll(pairing, kOtherId, now), "unknown"));
  // The same companion polling with POST again keeps its request.
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             key, now + 100) == CompanionPairResult::PENDING);
  CHECK(pairing.request.serial == 1);
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", key,
                             now + 6000) == CompanionPairResult::BUSY);
  // The code waits for the controller's key, then three wrong codes deny it.
  CHECK(companionPairApprove(pairing, 1, 1, true, now) == CompanionApproveResult::NOT_READY);
  pairing.request.keyReady = true;
  CHECK(companionPairApprove(pairing, 1, 2, true, now) == CompanionApproveResult::WRONG_CODE);
  CHECK(companionPairApprove(pairing, 1, 2, true, now) == CompanionApproveResult::WRONG_CODE);
  CHECK(companionPairApprove(pairing, 1, 2, true, now) == CompanionApproveResult::DENIED);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now), "denied"));
  CHECK(companionPairApprove(pairing, 2, 2, true, now) == CompanionApproveResult::NOT_PENDING);
  // One new request per 5 s.
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", key,
                             now + 4999) == CompanionPairResult::RATE_LIMITED);
  now += 6000;
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", key, now) ==
        CompanionPairResult::NEW);
  CHECK(pairing.request.serial == 2 && !pairing.request.keyReady);
  // Ignore denies the request and refuses the id until Stop ignoring.
  CHECK(companionPairIgnore(pairing, now));
  CHECK(!companionPairIgnore(pairing, now));
  CHECK(companionIgnoredCount(pairing) == 1);
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", key,
                             now + 10000) == CompanionPairResult::IGNORED);
  companionUnignore(pairing);
  CHECK(companionIgnoredCount(pairing) == 0);
  // Expiry after ten minutes.
  now += 10000;
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             key, now) == CompanionPairResult::NEW);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now + COMPANION_REQUEST_MS - 1), "pending"));
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now + COMPANION_REQUEST_MS), "expired"));
  now += COMPANION_REQUEST_MS + 1;
  // A changed key from the same companion is a new request.
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             key, now) == CompanionPairResult::NEW);
  key[5] = 9;
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             key, now + 1000) == CompanionPairResult::RATE_LIMITED);
  CHECK(companionPairRequest(pairing, false, kCompanionId, "Kitchen", "1.0.0", "192.168.1.9",
                             key, now + 5000) == CompanionPairResult::NEW);
  now += 5000;
  // Approval: the network task derives S, which makes the pairing.
  pairing.request.keyReady = true;
  CHECK(companionPairApprove(pairing, 7, 7, false, now) == CompanionApproveResult::OK);
  CHECK(!strcmp(companionPairPoll(pairing, kCompanionId, now + COMPANION_REQUEST_MS + 5), "pending"));
  CHECK(companionPairApprove(pairing, 7, 7, false, now) == CompanionApproveResult::NOT_PENDING);
  CHECK(!companionPairIgnore(pairing, now));
  CHECK(companionPairRequest(pairing, false, kOtherId, "Other", "", "192.168.1.10", key,
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
  CHECK(companionPairRequest(pairing, true, kOtherId, "Other", "", "192.168.1.10", key,
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
  CHECK(companionPairRequest(next, false, kOtherId, "Other", "", "192.168.1.10", key, now) ==
        CompanionPairResult::NEW);
  next.request.keyReady = true;
  CHECK(companionPairApprove(next, 3, 3, true, now) == CompanionApproveResult::OK);
  companionPairComplete(next, stored, secret);
  CHECK(stored.paired == 1 && stored.forgotten == 1 &&
        companionHelloVerdict(stored, kCompanionId, 1) == CompanionHello::ERASE &&
        companionHelloVerdict(stored, kOtherId, 1) == CompanionHello::CHALLENGE);

  // The ignore list keeps the newest ids.
  CompanionPairing many;
  char id[COMPANION_ID_TEXT];
  for (int i = 0; i < COMPANION_IGNORED_IDS + 1; ++i) {
    std::snprintf(id, sizeof(id), "%08d-2f4e-4d7a-9c1b-5e8f0a1d2c3b", i);
    CHECK(companionPairRequest(many, false, id, "n", "", "10.0.0.2", key, 100000u * (i + 1)) ==
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
}

static void testLiveness() {
  CompanionLinkSlot slot;
  CHECK(companionSilenceLimitMs(slot, true) == 10000);
  slot.phase = CompanionLinkPhase::READY;
  CHECK(companionSilenceLimitMs(slot, true) == 250000);
  CHECK(companionSilenceLimitMs(slot, false) == 35000);
}

int main() {
  testVectors();
  testFrameCodec();
  testBase64AndValidation();
  testPairing();
  testStoredBlob();
  testLiveness();
  std::puts("companion_link_host_test: ok");
  return 0;
}

#define OPEN_BREW_BY_WEIGHT_HOST_TEST
#define OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST
#define OPEN_BREW_BY_WEIGHT_OTA_HOST_TEST

#include "../OpenBrewByWeightOta.cpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace openbrewbyweight {
struct OtaHostTestAccess {
  static std::unique_ptr<OpenBrewByWeightOta> fresh() {
    std::unique_ptr<OpenBrewByWeightOta> ota(new OpenBrewByWeightOta);
    ota->begin();
    // Host tests must not regenerate the tracked version header to pick a board.
    std::strcpy(ota->runningTag_.arch, "n16r8");
    std::strcpy(ota->runningTag_.hardware, FW_HARDWARE_COMPAT_STRING);
    std::strcpy(ota->runningTag_.machine, FW_MACHINE_COMPAT_STRING);
    return ota;
  }
};
}  // namespace openbrewbyweight

using namespace openbrewbyweight;
const char *const kTransfer = "test-transfer-012345";

#define CHECK(condition) do { if (!(condition)) { \
  std::cerr << __func__ << ':' << __LINE__ << ": " << #condition << '\n'; \
  std::exit(EXIT_FAILURE); \
} } while (false)

void reset(esp_ota_img_states_t state = ESP_OTA_IMG_VALID) {
  ota_host::reset();
  persistence_host::reset();
  ota_host::state = state;
  g_hostFlashIoMutexAvailable = true;
  hostMillis = 100;
  otaHostFullSlotHashes = 0;
}

struct Image {
  std::vector<uint8_t> bytes;
  OtaSessionIdentity identity;
  explicit Image(size_t size = 65536) : bytes(size, 0xA5) {
    std::fill(bytes.begin(), bytes.begin() + OTA_IMAGE_PREFIX_BYTES, 0);
    bytes[0] = OTA_ESP_IMAGE_MAGIC;
    bytes[12] = 9;
    const uint32_t magic = OTA_APP_DESC_MAGIC;
    std::memcpy(bytes.data() + OTA_APP_DESC_OFFSET, &magic, sizeof(magic));
    std::strcpy(reinterpret_cast<char *>(bytes.data() + OTA_APP_DESC_PROJECT_NAME_OFFSET),
                OTA_EXPECTED_PROJECT_NAME);
    char prefix[OTA_TAG_PREFIX_CAPACITY];
    otaTagPrefix(prefix, sizeof(prefix));
    const std::string tag = std::string(prefix) + "arch=n16r8|hw=" +
        FW_HARDWARE_COMPAT_STRING + "|machine=" + FW_MACHINE_COMPAT_STRING +
        "|ver=" + FW_VERSION_STRING + "|packed=" +
        FW_VERSION_PACKED_STRING + "|END";
    std::memcpy(bytes.data() + 512, tag.data(), tag.size());
    identity.size = static_cast<uint32_t>(size);
    std::strcpy(identity.arch, "n16r8");
    std::strcpy(identity.hardware, FW_HARDWARE_COMPAT_STRING);
    std::strcpy(identity.machine, FW_MACHINE_COMPAT_STRING);
    std::strcpy(identity.version, FW_VERSION_STRING);
    std::strcpy(identity.transferId, "test-transfer-012345");
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    psa_hash_setup(&hash, PSA_ALG_SHA_256);
    psa_hash_update(&hash, bytes.data(), bytes.size());
    uint8_t digest[32];
    size_t digestLength = 0;
    psa_hash_finish(&hash, digest, sizeof(digest), &digestLength);
    sha256Hex(digest, identity.sha256);
  }
};

struct Stream {
  const std::vector<uint8_t> &bytes;
  size_t position = 0;
  size_t readLimit = 4096;
  size_t failAt = SIZE_MAX;
  bool safe = true;
  bool unsafeAfterRead = false;
  static int read(void *context, uint8_t *out, size_t capacity) {
    auto &s = *static_cast<Stream *>(context);
    if (s.position >= s.failAt) return 0;
    const size_t length = std::min({capacity, s.readLimit,
        s.bytes.size() - s.position, s.failAt - s.position});
    std::memcpy(out, s.bytes.data() + s.position, length);
    s.position += length;
    if (s.unsafeAfterRead) s.safe = false;
    return static_cast<int>(length);
  }
  static bool stillSafe(void *context) { return static_cast<Stream *>(context)->safe; }
  OtaStreamIo io() { return {read, stillSafe, nullptr, this}; }
};

void testFragmentedHeaderAndTail() {
  reset();
  auto ota = OtaHostTestAccess::fresh();
  Image image(65536 + 31);
  CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::OK);
  CHECK(ota->publishedState().busy && !ota->publishedState().imagePending);
  Stream stream{image.bytes};
  stream.readLimit = 100;
  CHECK(ota->writeRange(kTransfer, 0, 65536, stream.io(), hostMillis) == OtaResult::OK);
  CHECK(ota_host::writes == 16);
  // The id is rechecked under the OTA mutex: a foreign range writes nothing.
  CHECK(ota->writeRange("other-transfer-01234", 65536, 31, stream.io(), hostMillis) ==
        OtaResult::SESSION_IDENTITY_MISMATCH);
  CHECK(ota_host::writes == 16 && stream.position == 65536);
  CHECK(ota->writeRange(kTransfer, 65536, 31, stream.io(), hostMillis) == OtaResult::OK);
  CHECK(ota->snapshot().state == OtaState::STAGED);
  // A staged or committed image keeps a scheduled restart away.
  CHECK(!ota->publishedState().busy && ota->publishedState().imagePending);
  CHECK(ota_host::ends == 1 && ota_host::aborts == 0);
  CHECK(std::equal(image.bytes.begin(), image.bytes.end(), ota_host::flash.begin()));
  CHECK(ota->commit() == OtaResult::OK);
  CHECK(ota->snapshot().state == OtaState::COMMITTED);
  CHECK(ota->publishedState().imagePending);
  // After a commit only a restart remains; every retry path says so.
  CHECK(ota->commit() == OtaResult::COMMITTED);
  CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::COMMITTED);
  CHECK(ota->discard() == OtaResult::COMMITTED);
  CHECK(std::string(OpenBrewByWeightOta::resultName(OtaResult::COMMITTED)) == "OTA_ALREADY_FLASHED");
}

void testInterruptedRangeAndAlignment() {
  reset();
  auto ota = OtaHostTestAccess::fresh();
  Image image;
  CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::OK);
  Stream stream{image.bytes};
  CHECK(ota->writeRange(kTransfer, 0, 5000, stream.io(), hostMillis) == OtaResult::INVALID_RANGE);
  CHECK(ota_host::begins == 0 && stream.position == 0);
  stream.failAt = 4096 + 100;
  CHECK(ota->writeRange(kTransfer, 0, image.identity.size, stream.io(), hostMillis) == OtaResult::RECEIVE_FAILED);
  auto status = ota->snapshot();
  CHECK(status.sessionActive && status.nextOffset == 4096 && status.busy);
  CHECK(status.lastResult == OtaResult::RECEIVE_FAILED && ota_host::aborts == 0);
  CHECK(ota_host::flash[4096] == 0xFF);
  stream.position = status.nextOffset;
  stream.failAt = SIZE_MAX;
  CHECK(ota->writeRange(kTransfer, 4096, image.identity.size - 4096, stream.io(), hostMillis) == OtaResult::OK);
  CHECK(ota->snapshot().stagedValid);
}

void testSafetyAndFatalFailures() {
  for (int failure = 0; failure < 5; ++failure) {
    reset();
    auto ota = OtaHostTestAccess::fresh();
    Image image;
    if (failure == 3) image.identity.sha256[0] = image.identity.sha256[0] == 'a' ? 'b' : 'a';
    CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::OK);
    Stream stream{image.bytes};
    if (failure == 0) stream.unsafeAfterRead = true;
    if (failure == 1) ota_host::writeError = ESP_FAIL;
    if (failure == 2) ota_host::endError = ESP_FAIL;
    if (failure == 4) image.bytes[0] = 0;
    const OtaResult expected[] = {OtaResult::SAFETY_LOST, OtaResult::WRITE_FAILED,
        OtaResult::VERIFY_FAILED, OtaResult::HASH_MISMATCH, OtaResult::BAD_IMAGE};
    CHECK(ota->writeRange(kTransfer, 0, image.identity.size, stream.io(), hostMillis) == expected[failure]);
    CHECK(!ota->snapshot().sessionActive && !ota->snapshot().stagedValid);
    CHECK(!ota_host::active && ota_host::invalidHandles == 0);
    if (failure == 0 || failure == 4) CHECK(ota_host::begins == 0 && ota_host::writes == 0);
    if (failure == 1) CHECK(ota_host::aborts == 1);
    if (failure == 2 || failure == 3) CHECK(ota_host::ends == 1 && ota_host::aborts == 0);
  }
}

void testCheckpointRecovery() {
  for (bool corrupt : {false, true}) {
    reset();
    auto ota = OtaHostTestAccess::fresh();
    Image image(OTA_JOURNAL_CHECKPOINT_BYTES + 2 * OTA_TRANSFER_CHUNK_BYTES);
    CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::OK);
    Stream stream{image.bytes};
    for (uint32_t offset = 0; offset < OTA_JOURNAL_CHECKPOINT_BYTES + OTA_TRANSFER_CHUNK_BYTES;
         offset += OTA_TRANSFER_CHUNK_BYTES) {
      CHECK(ota->writeRange(kTransfer, offset, OTA_TRANSFER_CHUNK_BYTES, stream.io(), hostMillis) == OtaResult::OK);
    }
    ota.reset();  // Volatile state dies; flash and journal survive.
    if (corrupt) ota_host::flash[8000] ^= 1;
    else std::fill(ota_host::flash.begin() + OTA_JOURNAL_CHECKPOINT_BYTES,
                  ota_host::flash.begin() + image.bytes.size(), 0);
    ota = OtaHostTestAccess::fresh();
    if (corrupt) {
      CHECK(!ota->snapshot().sessionActive && ota_host::resumes == 0);
      CHECK(persistence_host::records.empty());
      continue;
    }
    CHECK(ota->snapshot().nextOffset == OTA_JOURNAL_CHECKPOINT_BYTES);
    CHECK(ota_host::resumes == 1 && ota_host::resumeMode == OTA_WITH_SEQUENTIAL_WRITES);
    stream.position = OTA_JOURNAL_CHECKPOINT_BYTES;
    for (uint32_t offset = OTA_JOURNAL_CHECKPOINT_BYTES; offset < image.bytes.size();
         offset += OTA_TRANSFER_CHUNK_BYTES) {
      CHECK(ota->writeRange(kTransfer, offset, OTA_TRANSFER_CHUNK_BYTES, stream.io(), hostMillis) == OtaResult::OK);
    }
    CHECK(ota->snapshot().stagedValid && ota_host::invalidHandles == 0);
    // Boot and staging read the appended digest instead of re-hashing slots.
    CHECK(otaHostFullSlotHashes == 0 && std::strlen(ota->snapshot().stagedImageSha256) == 64);
    CHECK(std::equal(image.bytes.begin(), image.bytes.end(), ota_host::flash.begin()));
    CHECK(persistence_host::records.empty());
  }
}

void testAbandonedSessionExpiresAndErasesLazily() {
  reset();
  auto ota = OtaHostTestAccess::fresh();
  Image image;
  CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::OK);
  CHECK(ota->busy() && !persistence_host::records.empty());
  const uint32_t expired = hostMillis + OTA_SESSION_TTL_MS;
  ota->expireSession(expired, false, false);  // A pour forbids the erase.
  CHECK(!ota->busy() && !persistence_host::records.empty());
  ota->expireSession(expired, true, false);
  CHECK(persistence_host::records.empty());
  CHECK(ota->createSession(image.identity, expired) == OtaResult::OK);
  ota->discard();  // Abort leaves the erase to the next allowed tick.
  CHECK(!ota->busy() && !persistence_host::records.empty());
  ota->expireSession(expired, true, false);
  CHECK(persistence_host::records.empty());
}

void testNewSessionClearsStaleJournal() {
  reset();
  auto ota = OtaHostTestAccess::fresh();
  Image first(OTA_JOURNAL_CHECKPOINT_BYTES + 2 * OTA_TRANSFER_CHUNK_BYTES);
  CHECK(ota->createSession(first.identity, hostMillis) == OtaResult::OK);
  Stream stream{first.bytes};
  for (uint32_t offset = 0; offset <= OTA_JOURNAL_CHECKPOINT_BYTES;
       offset += OTA_TRANSFER_CHUNK_BYTES) {
    CHECK(ota->writeRange(kTransfer, offset, OTA_TRANSFER_CHUNK_BYTES, stream.io(), hostMillis) == OtaResult::OK);
  }
  CHECK(persistence_host::records.size() == 2);
  ota.reset();  // The next boot cannot restore before confirmation.
  ota_host::state = ESP_OTA_IMG_PENDING_VERIFY;
  ota = OtaHostTestAccess::fresh();
  CHECK(ota->serviceBoot(15000, true, false, false, false, 15000, 180000).state ==
        ESP_OTA_IMG_VALID);
  Image second;
  std::strcpy(second.identity.transferId, "second-transfer-0123");
  CHECK(ota->createSession(second.identity, hostMillis) == OtaResult::OK);
  CHECK(persistence_host::records.size() == 1);
  ota.reset();
  ota = OtaHostTestAccess::fresh();
  CHECK(ota->snapshot().sessionActive);
  CHECK(std::string(ota->snapshot().session.transferId) == "second-transfer-0123");
}

OtaBootStatus service(OpenBrewByWeightOta &ota, uint32_t now, bool http = true,
                      bool cycle = false, bool relay = false, bool ble = false) {
  return ota.serviceBoot(now, http, cycle, relay, ble, 15000, 180000);
}

void testConfirmationGatesAndDiagnostics() {
  reset(ESP_OTA_IMG_PENDING_VERIFY);
  auto ota = OtaHostTestAccess::fresh();
  CHECK(ota->snapshot().pendingVerify && !ota->snapshot().confirmed);
  CHECK(!settingsSchemaWritesAdmitted.load());
  Image image;
  CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::PENDING_VERIFY);
  std::strcpy(image.identity.transferId, "different-transfer-012345");
  image.identity.sha256[0] = image.identity.sha256[0] == 'a' ? 'b' : 'a';
  CHECK(ota->createSession(image.identity, hostMillis) == OtaResult::PENDING_VERIFY);
  CHECK(std::string(service(*ota, 14999).reason) == "WAIT_UPTIME");
  CHECK(std::string(service(*ota, 15000, false).reason) == "WAIT_HTTP");
  CHECK(std::string(service(*ota, 15000, true, true).reason) == "WAIT_CYCLE");
  CHECK(std::string(service(*ota, 15000, true, false, true).reason) == "WAIT_RELAY");
  CHECK(std::string(service(*ota, 179999, true, false, false, true).reason) == "WAIT_BLE");
  CHECK(ota_host::confirms == 0);
  CHECK(service(*ota, 180000, true, true, false, true).attempts == 0);
  CHECK(service(*ota, 180000, true, false, false, true).state == ESP_OTA_IMG_VALID);
  const auto status = ota->snapshot();
  CHECK(status.confirmed && !status.pendingVerify && status.confirmAttempts == 1);
  CHECK(settingsSchemaWritesAdmitted.load());
  CHECK(status.confirmLastError == ESP_OK && std::string(status.confirmBlockReason) == "CONFIRMED");
  CHECK(std::strlen(status.runningImageSha256) == 64);
  CHECK(service(*ota, 200000).attempts == 1 && ota_host::confirms == 1);
  reset(ESP_OTA_IMG_PENDING_VERIFY);
  ota = OtaHostTestAccess::fresh();
  CHECK(service(*ota, 15000).state == ESP_OTA_IMG_VALID);
}

void testJournalFailureHasDistinctResult() {
  reset();
  auto ota = OtaHostTestAccess::fresh();
  Image image;
  persistence_host::failNextWrite = true;
  CHECK(ota->createSession(image.identity, hostMillis) ==
        OtaResult::JOURNAL_FAILED);
  CHECK(std::string(OpenBrewByWeightOta::resultName(OtaResult::JOURNAL_FAILED)) ==
        "OTA_JOURNAL_FAILED");
  const NvsDiagnosticSnapshot nvs = captureNvsDiagnostics();
  CHECK(nvs.lastFailure.present);
  CHECK(nvs.lastFailure.subsystem == NvsSubsystem::OTA_JOURNAL);
}

void testConfirmationRetryAndStateRecovery() {
  for (bool flashBusy : {false, true}) {
    reset(ESP_OTA_IMG_PENDING_VERIFY);
    auto ota = OtaHostTestAccess::fresh();
    if (flashBusy) g_hostFlashIoMutexAvailable = false;
    else ota_host::confirmError = ESP_FAIL;
    const auto first = service(*ota, 15000);
    CHECK(first.attempts == 1 && first.lastError == (flashBusy ? ESP_ERR_TIMEOUT : ESP_FAIL));
    CHECK(!ota->snapshot().confirmed && ota->snapshot().pendingVerify);
    CHECK(std::string(first.reason) == (flashBusy ? "FLASH_BUSY" : "CONFIRM_ERROR"));
    g_hostFlashIoMutexAvailable = true;
    ota_host::confirmError = ESP_OK;
    CHECK(service(*ota, 15999).attempts == 1);
    CHECK(service(*ota, 16000).attempts == 2);
    CHECK(ota->snapshot().confirmed && ota->snapshot().confirmLastError == ESP_OK);
  }
  reset(ESP_OTA_IMG_PENDING_VERIFY);
  ota_host::stateError = ESP_FAIL;
  auto ota = OtaHostTestAccess::fresh();
  CHECK(ota->snapshot().bootState == -2 && ota->snapshot().pendingVerify);
  CHECK(std::string(service(*ota, 15000).reason) == "STATE_ERROR");
  ota_host::stateError = ESP_OK;
  CHECK(!ota->snapshot().confirmed && service(*ota, 15999).state == -2);
  CHECK(service(*ota, 16000).state == ESP_OTA_IMG_VALID);
}

void testRollbackDoesNotConfirmOnFailure() {
  for (bool flashBusy : {false, true}) {
    reset(ESP_OTA_IMG_PENDING_VERIFY);
    auto ota = OtaHostTestAccess::fresh();
    if (flashBusy) g_hostFlashIoMutexAvailable = false;
    else ota_host::rollbackError = ESP_FAIL;
    const auto status = service(*ota, 180000, false);
    CHECK(std::string(status.reason) == (flashBusy ? "FLASH_BUSY" : "ROLLBACK_ERROR"));
    CHECK(ota_host::confirms == 0 && ota->snapshot().pendingVerify);
    g_hostFlashIoMutexAvailable = true;
    ota_host::rollbackError = ESP_OK;
    CHECK(service(*ota, 180999, false).attempts == 1);
    CHECK(service(*ota, 181000, false).state == ESP_OTA_IMG_INVALID);
    CHECK(ota->snapshot().rejected && !ota->confirmRunningImage());
    CHECK(ota_host::confirms == 0);
  }
  reset(ESP_OTA_IMG_PENDING_VERIFY);
  ota_host::rollbackPossible = false;
  auto ota = OtaHostTestAccess::fresh();
  CHECK(std::string(service(*ota, 180000, false).reason) == "NO_ROLLBACK");
  CHECK(ota_host::rollbacks == 0 && ota_host::confirms == 1);
  CHECK(ota->snapshot().confirmed);
}

int main() {
  testFragmentedHeaderAndTail();
  testInterruptedRangeAndAlignment();
  testSafetyAndFatalFailures();
  testCheckpointRecovery();
  testAbandonedSessionExpiresAndErasesLazily();
  testNewSessionClearsStaleJournal();
  testJournalFailureHasDistinctResult();
  testConfirmationGatesAndDiagnostics();
  testConfirmationRetryAndStateRecovery();
  testRollbackDoesNotConfirmOnFailure();
  std::cout << "OTA functional transfer, checkpoint and boot service tests passed\n";
}

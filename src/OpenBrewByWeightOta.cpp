#include "OpenBrewByWeightOta.h"

#if defined(OPEN_BREW_BY_WEIGHT_OTA_HOST_TEST)
#include "tests/ota_host_stubs.h"
#else
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <psa/crypto.h>
#endif

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightPreferences.h"
#include "OpenBrewByWeightSettingsWriteAdmission.h"
#include <OpenBrewByWeightVersion.h>

#include <string.h>

namespace openbrewbyweight {

// The identity marker every flashable Open Brew by Weight image carries. It has
// external linkage and is read back by begin(), which is what keeps the linker
// from garbage-collecting the one thing an OTA verify looks for.
extern "C" const char OPEN_BREW_BY_WEIGHT_FW_IMAGE_TAG[] = FW_IMAGE_TAG_STRING;

namespace {

// Transfer buffer size. Allocated from internal heap only for the duration of
// stage(): the flash driver cannot DMA from PSRAM, and a permanent 4 KiB BSS
// member is wasted between updates.
constexpr size_t OTA_CHUNK_BYTES = 4096;

struct OtaChunkBuffer {
  uint8_t *bytes = nullptr;

  explicit OtaChunkBuffer(size_t capacity)
      : bytes(static_cast<uint8_t *>(allocInternal(capacity, AllocationOwner::OTA))) {}

  ~OtaChunkBuffer() { heapCapsFree(bytes); }

  OtaChunkBuffer(const OtaChunkBuffer &) = delete;
  OtaChunkBuffer &operator=(const OtaChunkBuffer &) = delete;

  bool ok() const { return bytes != nullptr; }
};

struct FlashIoGuard {
  bool locked = false;

  explicit FlashIoGuard(uint32_t timeoutMs = FLASH_IO_LOCK_TIMEOUT_MS)
      : locked(tryLockFlashIo(timeoutMs)) {}
  ~FlashIoGuard() {
    if (locked) unlockFlashIo();
  }
  FlashIoGuard(const FlashIoGuard &) = delete;
  FlashIoGuard &operator=(const FlashIoGuard &) = delete;
  bool ok() const { return locked; }
};

// A real image is well over a megabyte; anything this small is not one.
constexpr uint32_t OTA_MIN_IMAGE_BYTES = 65536;

constexpr uint32_t OTA_SESSION_TTL_MS = 15U * 60U * 1000U;
constexpr uint32_t OTA_JOURNAL_MAGIC = 0x4f544a31U;  // OTJ1
constexpr uint16_t OTA_JOURNAL_VERSION = 1;
constexpr uint32_t OTA_PUBLISHED_AVAILABLE = 1U << 0;
constexpr uint32_t OTA_PUBLISHED_BUSY = 1U << 1;
constexpr uint32_t OTA_PUBLISHED_PENDING_VERIFY = 1U << 2;
constexpr uint32_t OTA_PUBLISHED_CONFIRMED = 1U << 3;
constexpr uint32_t OTA_PUBLISHED_REJECTED = 1U << 4;
constexpr uint32_t OTA_PUBLISHED_IMAGE_PENDING = 1U << 5;
std::atomic<uint32_t> otaAbortFailures{0};
std::atomic<uint32_t> otaJournalFailures{0};

// Two NVS records make a power cut during the metadata update harmless. The
// image itself remains in the inactive OTA partition; this journal only says
// which already-written prefix may be resumed.
struct OtaJournalRecord {
  uint32_t magic = OTA_JOURNAL_MAGIC;
  uint16_t version = OTA_JOURNAL_VERSION;
  uint16_t reserved = 0;
  uint32_t generation = 0;
  uint32_t received = 0;
  OtaSessionIdentity identity = {};
  char prefixSha256[OTA_SHA256_HEX_CAPACITY] = {};
  uint32_t checksum = 0;
};
static_assert(sizeof(OtaJournalRecord) == 416, "OTA journal layout changed");

constexpr size_t OTA_TAG_REREAD_BYTES =
    OTA_TAG_PREFIX_CAPACITY + OTA_TAG_BODY_CAPACITY;

bool sameTag(const OtaImageTag &left, const OtaImageTag &right) {
  return left.valid && right.valid && left.packed == right.packed &&
         strncmp(left.arch, right.arch, OTA_ARCH_CAPACITY) == 0 &&
         strncmp(left.hardware, right.hardware,
                 OTA_PROFILE_COMPAT_CAPACITY) == 0 &&
         strncmp(left.machine, right.machine,
                 OTA_PROFILE_COMPAT_CAPACITY) == 0 &&
         strncmp(left.version, right.version, OTA_VERSION_CAPACITY) == 0;
}

bool sameText(const char *left, const char *right, size_t capacity) {
  return strncmp(left, right, capacity) == 0;
}

bool sameSession(const OtaSessionIdentity &left,
                 const OtaSessionIdentity &right) {
  return left.size == right.size &&
         sameText(left.sha256, right.sha256, sizeof(left.sha256)) &&
         sameText(left.arch, right.arch, sizeof(left.arch)) &&
         sameText(left.hardware, right.hardware, sizeof(left.hardware)) &&
         sameText(left.machine, right.machine, sizeof(left.machine)) &&
         sameText(left.version, right.version, sizeof(left.version)) &&
         sameText(left.transferId, right.transferId,
                  sizeof(left.transferId));
}

void sha256Hex(const uint8_t digest[32], char output[OTA_SHA256_HEX_CAPACITY]) {
  static constexpr char HEX_DIGITS[] = "0123456789abcdef";
  for (size_t index = 0; index < 32; ++index) {
    output[index * 2] = HEX_DIGITS[digest[index] >> 4U];
    output[index * 2 + 1] = HEX_DIGITS[digest[index] & 0x0fU];
  }
  output[64] = '\0';
}

uint32_t journalChecksum(const OtaJournalRecord &record) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
  const size_t length = offsetof(OtaJournalRecord, checksum);
  uint32_t value = 2166136261U;
  for (size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    value *= 16777619U;
  }
  return value;
}

bool validJournal(const OtaJournalRecord &record) {
  return record.magic == OTA_JOURNAL_MAGIC &&
         record.version == OTA_JOURNAL_VERSION && record.received <= record.identity.size &&
         record.identity.size != 0 && record.checksum == journalChecksum(record);
}

bool validJournalIdentity(const OtaSessionIdentity &identity) {
  if (!otaArchIsUsable(identity.arch) ||
      !otaProfileIsUsable(identity.hardware) ||
      !otaProfileIsUsable(identity.machine) || identity.version[0] == '\0' ||
      identity.transferId[0] == '\0' || strlen(identity.sha256) != 64) {
    return false;
  }
  for (const char *cursor = identity.version; *cursor != '\0'; ++cursor) {
    if (!otaVersionCharAllowed(*cursor)) return false;
  }
  for (const char *cursor = identity.transferId; *cursor != '\0'; ++cursor) {
    const char c = *cursor;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  for (const char *cursor = identity.sha256; *cursor != '\0'; ++cursor) {
    if (!((*cursor >= '0' && *cursor <= '9') ||
          (*cursor >= 'a' && *cursor <= 'f'))) return false;
  }
  return true;
}

bool readJournal(const char *key, OtaJournalRecord &record) {
  if (!tryLockFlashIo()) return false;
  OpenBrewByWeightPreferences preferences(NvsSubsystem::OTA_JOURNAL);
  if (!preferences.begin("ota", true)) {
    unlockFlashIo();
    return false;
  }
  const size_t length = preferences.getBytes(key, &record, sizeof(record));
  preferences.end();
  unlockFlashIo();
  return length == sizeof(record) && validJournal(record);
}

bool writeJournal(const char *key, const OtaJournalRecord &record) {
  if (!tryLockFlashIo()) {
    otaJournalFailures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::OTA_JOURNAL);
  if (!preferences.begin("ota", false)) {
    unlockFlashIo();
    otaJournalFailures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const size_t length = preferences.putBytes(key, &record, sizeof(record));
  preferences.end();
  unlockFlashIo();
  const bool written = length == sizeof(record);
  if (!written) otaJournalFailures.fetch_add(1, std::memory_order_relaxed);
  return written;
}

bool clearJournal() {
  if (!tryLockFlashIo()) {
    otaJournalFailures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::OTA_JOURNAL);
  if (!preferences.begin("ota", false)) {
    unlockFlashIo();
    otaJournalFailures.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const bool removed =
      (!preferences.isKey("j0") || preferences.remove("j0")) &&
      (!preferences.isKey("j1") || preferences.remove("j1"));
  preferences.end();
  unlockFlashIo();
  if (!removed) otaJournalFailures.fetch_add(1, std::memory_order_relaxed);
  return removed;
}

// Same value esp_partition_get_sha256() returns for an app slot, without its
// full-slot re-hash: the bootloader verified the running image and
// esp_ota_end() the staged one. Images without an appended digest fall back.
bool readImageDigest(const esp_partition_t *partition, char *hex) {
  FlashIoGuard flash;
  if (!flash.ok()) return false;
  const esp_partition_pos_t position = {partition->address, partition->size};
  esp_image_metadata_t metadata = {};
  uint8_t digest[32] = {};
  if (esp_image_get_metadata(&position, &metadata) == ESP_OK &&
      metadata.image.hash_appended) {
    memcpy(digest, metadata.image_digest, sizeof(digest));
  } else if (esp_partition_get_sha256(partition, digest) != ESP_OK) {
    return false;
  }
  sha256Hex(digest, hex);
  return true;
}

bool abortOtaHandle(esp_ota_handle_t handle) {
  FlashIoGuard flash;
  const bool aborted = flash.ok() && esp_ota_abort(handle) == ESP_OK;
  if (!aborted) otaAbortFailures.fetch_add(1, std::memory_order_relaxed);
  return aborted;
}

}  // namespace

void OtaHandleAborter::operator()(uint32_t handle) const {
  abortOtaHandle(static_cast<esp_ota_handle_t>(handle));
}

void OtaSha256Deleter::operator()(void *context) const {
  if (context == nullptr) return;
  (void)psa_hash_abort(static_cast<psa_hash_operation_t *>(context));
  heapCapsFree(context);
}

std::atomic<uint32_t> OpenBrewByWeightOta::publishedFlags_{0};
TaskMutex OpenBrewByWeightOta::mutex_;
TaskMutex OpenBrewByWeightOta::publishedSnapshotMutex_;

OpenBrewByWeightOta &OpenBrewByWeightOta::instance() {
  // Task-only state, never a flash DMA source or destination (chunks and
  // re-reads use internal buffers). The scanner's runtime constructor keeps
  // the object dynamically initialised, so its non-zero defaults survive the
  // NOLOAD external BSS section.
  OPEN_BREW_BY_WEIGHT_PSRAM_BSS static OpenBrewByWeightOta shared;
  return shared;
}

bool OpenBrewByWeightOta::available() const {
  return publishedState().available;
}

bool OpenBrewByWeightOta::busy() const {
  return publishedState().busy;
}

uint32_t OpenBrewByWeightOta::slotBytes() const {
  TaskLockGuard lock(mutex_);
  return slotBytes_;
}

bool OpenBrewByWeightOta::runningImageConfirmed() const {
  return publishedState().confirmed;
}

bool OpenBrewByWeightOta::runningImageRejected() const {
  return publishedState().rejected;
}

OtaPublishedState OpenBrewByWeightOta::publishedState() const {
  const uint32_t flags = publishedFlags_.load(std::memory_order_acquire);
  OtaPublishedState state;
  state.available = (flags & OTA_PUBLISHED_AVAILABLE) != 0;
  state.busy = (flags & OTA_PUBLISHED_BUSY) != 0;
  state.pendingVerify = (flags & OTA_PUBLISHED_PENDING_VERIFY) != 0;
  state.confirmed = (flags & OTA_PUBLISHED_CONFIRMED) != 0;
  state.rejected = (flags & OTA_PUBLISHED_REJECTED) != 0;
  state.imagePending = (flags & OTA_PUBLISHED_IMAGE_PENDING) != 0;
  return state;
}

void OpenBrewByWeightOta::publishState() {
  uint32_t flags = 0;
  if (available_) flags |= OTA_PUBLISHED_AVAILABLE;
  if (busy_ || sessionActive_) flags |= OTA_PUBLISHED_BUSY;
  if (pendingVerify_) flags |= OTA_PUBLISHED_PENDING_VERIFY;
  if (confirmed_) flags |= OTA_PUBLISHED_CONFIRMED;
  if (rejected_) flags |= OTA_PUBLISHED_REJECTED;
  if (state_ == OtaState::STAGED || state_ == OtaState::COMMITTED) {
    flags |= OTA_PUBLISHED_IMAGE_PENDING;
  }
  publishedFlags_.store(flags, std::memory_order_release);
  publishSnapshotLocked();
}

OtaImageTag OpenBrewByWeightOta::runningTag() const {
  TaskLockGuard lock(mutex_);
  return runningTag_;
}

void OpenBrewByWeightOta::begin() {
  TaskLockGuard lock(mutex_);
  if (started_) {
    return;
  }
  started_ = true;

  OtaImageTagScanner scanner;
  scanner.feed(reinterpret_cast<const uint8_t *>(OPEN_BREW_BY_WEIGHT_FW_IMAGE_TAG),
               strlen(OPEN_BREW_BY_WEIGHT_FW_IMAGE_TAG));
  if (scanner.found()) {
    runningTag_ = scanner.tag();
  }

  const esp_partition_t *running = nullptr;
  const esp_partition_t *target = nullptr;
  {
    FlashIoGuard flash;
    if (flash.ok()) {
      running = esp_ota_get_running_partition();
      target = esp_ota_get_next_update_partition(nullptr);
    }
  }
  targetPartition_ = target;
  available_ = running != nullptr && target != nullptr && target != running;
  slotBytes_ = target != nullptr ? target->size : 0;
  state_ = available_ ? OtaState::IDLE : OtaState::UNAVAILABLE;

  readBootState();
  if (running != nullptr) (void)readImageDigest(running, runningImageSha256_);
  if (available_ && confirmed_) {
    restoreSessionJournal();
  }
  if (journalStale_) (void)removeSessionJournal();
  publishState();
}

bool OpenBrewByWeightOta::removeSessionJournal() {
  journalStale_ = !clearJournal();
  return !journalStale_;
}

bool OpenBrewByWeightOta::persistSession() {
  if (!sessionActive_) return true;
  if (!sessionSha256_) return false;
  // Zero padding first: the checksum and NVS blob cover every byte.
  OtaJournalRecord record;
  memset(&record, 0, sizeof(record));
  record.magic = OTA_JOURNAL_MAGIC;
  record.version = OTA_JOURNAL_VERSION;
  record.generation = ++sessionGeneration_;
  record.received = receivedBytes_;
  record.identity = session_;
  psa_hash_operation_t snapshot = PSA_HASH_OPERATION_INIT;
  const psa_status_t cloned = psa_hash_clone(
      static_cast<const psa_hash_operation_t *>(sessionSha256_.get()),
      &snapshot);
  uint8_t digest[32] = {};
  size_t digestLength = 0;
  const bool ok = cloned == PSA_SUCCESS &&
                  psa_hash_finish(&snapshot, digest, sizeof(digest),
                                  &digestLength) == PSA_SUCCESS &&
                  digestLength == sizeof(digest);
  (void)psa_hash_abort(&snapshot);
  if (!ok) return false;
  sha256Hex(digest, record.prefixSha256);
  record.checksum = journalChecksum(record);
  if (!writeJournal((record.generation & 1U) == 0 ? "j0" : "j1", record)) {
    return false;
  }
  journaledBytes_ = receivedBytes_;
  return true;
}

void OpenBrewByWeightOta::restoreSessionJournal() {
  OtaJournalRecord first;
  OtaJournalRecord second;
  const bool firstValid = readJournal("j0", first);
  const bool secondValid = readJournal("j1", second);
  if (!firstValid && !secondValid) return;
  const OtaJournalRecord &record =
      (!firstValid || (secondValid &&
       static_cast<int32_t>(second.generation - first.generation) > 0)) ? second : first;
  if (!validJournalIdentity(record.identity) || record.identity.size > slotBytes_ ||
      record.received % OTA_WRITE_ALIGNMENT != 0 ||
      record.received >= record.identity.size) {
    clearJournal();
    return;
  }
  const esp_partition_t *target =
      static_cast<const esp_partition_t *>(targetPartition_);
  OtaChunkBuffer chunk(OTA_CHUNK_BYTES);
  if (target == nullptr || !chunk.ok()) return;
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  bool ok = psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS;
  OtaImageTagScanner scanner;
  uint32_t offset = 0;
  bool headerChecked = record.received == 0;
  while (ok && offset < record.received) {
    const size_t length = record.received - offset < OTA_CHUNK_BYTES
                              ? record.received - offset
                              : OTA_CHUNK_BYTES;
    {
      FlashIoGuard flash;
      ok = flash.ok() &&
           esp_partition_read(target, offset, chunk.bytes, length) == ESP_OK;
    }
    ok = ok && psa_hash_update(&hash, chunk.bytes, length) == PSA_SUCCESS;
    if (!headerChecked && offset == 0) {
      headerChecked = length >= OTA_IMAGE_PREFIX_BYTES &&
                      validateOtaImageHeader(chunk.bytes, length) == OtaImageHeaderResult::OK;
      ok = ok && headerChecked;
    }
    scanner.feed(chunk.bytes, length);
    offset += static_cast<uint32_t>(length);
    yieldFlashIo();
    feedFlashIoWatchdog();
  }
  psa_hash_operation_t verification = PSA_HASH_OPERATION_INIT;
  ok = ok && psa_hash_clone(&hash, &verification) == PSA_SUCCESS;
  uint8_t digest[32] = {};
  size_t digestLength = 0;
  ok = ok && psa_hash_finish(&verification, digest, sizeof(digest),
                             &digestLength) == PSA_SUCCESS &&
       digestLength == sizeof(digest);
  (void)psa_hash_abort(&verification);
  char prefix[OTA_SHA256_HEX_CAPACITY] = {};
  sha256Hex(digest, prefix);
  if (!ok || !sameText(prefix, record.prefixSha256, sizeof(prefix))) {
    (void)psa_hash_abort(&hash);
    clearJournal();
    return;
  }
  session_ = record.identity;
  sessionGeneration_ = record.generation;
  journaledBytes_ = record.received;
  receivedBytes_ = record.received;
  expectedBytes_ = record.identity.size;
  scanner_ = scanner;
  sessionLastActivityMs_ = millis();
  sessionActive_ = true;
  state_ = OtaState::RECEIVING;
  if (!startSessionSha256()) {
    (void)psa_hash_abort(&hash);
    clearSession(false);
    state_ = OtaState::IDLE;
    return;
  }
  auto *sessionHash = static_cast<psa_hash_operation_t *>(sessionSha256_.get());
  (void)psa_hash_abort(sessionHash);
  *sessionHash = PSA_HASH_OPERATION_INIT;
  if (psa_hash_clone(&hash, sessionHash) != PSA_SUCCESS) {
    (void)psa_hash_abort(&hash);
    clearSession(false);
    state_ = OtaState::IDLE;
    return;
  }
  (void)psa_hash_abort(&hash);
  if (record.received != 0) {
    esp_ota_handle_t handle = 0;
    FlashIoGuard flash;
    if (!flash.ok() ||
        esp_ota_resume(target, OTA_WITH_SEQUENTIAL_WRITES, record.received,
                       &handle) != ESP_OK) {
      clearSession(false);
      clearJournal();
      receivedBytes_ = 0;
      expectedBytes_ = 0;
      state_ = OtaState::IDLE;
      return;
    }
    otaHandle_.reset(static_cast<uint32_t>(handle));
  }
}

OtaStatusSnapshot OpenBrewByWeightOta::snapshot(bool wait) const {
  if (wait) {
    mutex_.lock();
  } else if (!mutex_.tryLock()) {
    TaskLockGuard published(publishedSnapshotMutex_);
    return publishedSnapshot_;
  }
  publishSnapshotLocked();
  mutex_.unlock();
  TaskLockGuard published(publishedSnapshotMutex_);
  return publishedSnapshot_;
}

// Fills the published copy in place: progress updates from writeRange() must
// not stage a second ~0.5 KB OtaStatusSnapshot on the OTA task stack.
void OpenBrewByWeightOta::publishSnapshotLocked() const {
  TaskLockGuard published(publishedSnapshotMutex_);
  OtaStatusSnapshot &copy = publishedSnapshot_;
  copy.available = available_;
  copy.busy = busy_ || sessionActive_;
  copy.state = state_;
  copy.slotBytes = slotBytes_;
  copy.receivedBytes = receivedBytes_;
  copy.expectedBytes = expectedBytes_;
  copy.lastResult = lastResult_;
  copy.lastReceivedBytes = lastReceivedBytes_;
  copy.lastExpectedBytes = lastExpectedBytes_;
  copy.stagedValid = stagedValid_;
  copy.staged = stagedTag_;
  copy.pendingVerify = pendingVerify_;
  copy.confirmed = confirmed_;
  copy.rejected = rejected_;
  copy.running = runningTag_;
  copy.sessionActive = sessionActive_;
  copy.nextOffset = receivedBytes_;
  copy.abortFailures = otaAbortFailures.load(std::memory_order_relaxed);
  copy.journalFailures = otaJournalFailures.load(std::memory_order_relaxed);
  copy.session = session_;
  memcpy(copy.lastChunkSha256, lastChunkSha256_, sizeof(lastChunkSha256_));
  memcpy(copy.runningImageSha256, runningImageSha256_, sizeof(runningImageSha256_));
  memcpy(copy.stagedImageSha256, stagedImageSha256_, sizeof(stagedImageSha256_));
  copy.bootState = bootStatus_.state;
  copy.confirmBlockReason = bootStatus_.reason;
  copy.confirmAttempts = bootStatus_.attempts;
  copy.confirmLastError = bootStatus_.lastError;
  const uint32_t age = static_cast<uint32_t>(millis() - sessionLastActivityMs_);
  copy.sessionExpiresInMs =
      sessionActive_ && age < OTA_SESSION_TTL_MS ? OTA_SESSION_TTL_MS - age : 0;
}

OtaResult OpenBrewByWeightOta::finishFailure(OtaResult result) {
  // Keep this evidence after clearing the active transfer. The HTTP handler
  // reads its snapshot after stage() returns, so resetting first used to make
  // every failed upload look as though it had failed at byte zero.
  lastResult_ = result;
  lastReceivedBytes_ = receivedBytes_;
  lastExpectedBytes_ = expectedBytes_;
  clearSession(true);
  busy_ = false;
  receivedBytes_ = 0;
  expectedBytes_ = 0;
  // A failed transfer leaves a partial image in the inactive slot. It can never
  // boot: the selection is untouched and esp_ota_set_boot_partition would
  // refuse it anyway.
  stagedValid_ = false;
  stagedTag_ = OtaImageTag{};
  state_ = available_ ? OtaState::IDLE : OtaState::UNAVAILABLE;
  publishState();
  return result;
}

void OpenBrewByWeightOta::clearSession(bool abortHandle) {
  if (abortHandle) otaHandle_.reset();
  sessionActive_ = false;
  sessionLastActivityMs_ = 0;
  session_ = OtaSessionIdentity{};
  lastChunkSha256_[0] = '\0';
  lastChunkOffset_ = 0;
  lastChunkLength_ = 0;
  scanner_.reset();
  clearSessionSha256();
  journaledBytes_ = 0;
  journalStale_ = true;
}

bool OpenBrewByWeightOta::isDuplicateRange(uint32_t offset,
                                      uint32_t contentLength) const {
  TaskLockGuard lock(mutex_);
  return sessionActive_ && contentLength != 0 &&
         offset == lastChunkOffset_ && contentLength == lastChunkLength_ &&
         offset + contentLength == receivedBytes_;
}

void OpenBrewByWeightOta::expireSession(uint32_t now, bool flashAllowed, bool wait) {
  if (wait) mutex_.lock();
  else if (!mutex_.tryLock()) return;
  if (sessionActive_ &&
      static_cast<uint32_t>(now - sessionLastActivityMs_) >= OTA_SESSION_TTL_MS) {
    finishFailure(OtaResult::SESSION_EXPIRED);
  }
  if (flashAllowed && journalStale_ && !sessionActive_) (void)removeSessionJournal();
  mutex_.unlock();
}

OtaResult OpenBrewByWeightOta::createSession(const OtaSessionIdentity &identity,
                                        uint32_t now) {
  TaskLockGuard lock(mutex_);
  if (!started_ || !available_) {
    return OtaResult::UNAVAILABLE;
  }
  if (sessionActive_ &&
      static_cast<uint32_t>(now - sessionLastActivityMs_) >=
          OTA_SESSION_TTL_MS) {
    finishFailure(OtaResult::SESSION_EXPIRED);
  }
  if (state_ == OtaState::COMMITTED) return OtaResult::COMMITTED;
  if (busy_) {
    return OtaResult::BUSY;
  }
  // esp_ota_begin refuses to write while the running slot is still
  // ESP_OTA_IMG_PENDING_VERIFY, and rightly so: overwriting the only other
  // slot would leave nothing to roll back to.
  if (!confirmed_) {
    return OtaResult::PENDING_VERIFY;
  }
  if (!otaArchIsUsable(runningTag_.arch) ||
      !otaProfileIsUsable(runningTag_.hardware) ||
      !otaProfileIsUsable(runningTag_.machine)) {
    return OtaResult::NO_IDENTITY;
  }
  if (identity.size < OTA_MIN_IMAGE_BYTES) {
    return OtaResult::BAD_LENGTH;
  }
  if (identity.size > slotBytes_) {
    return OtaResult::TOO_LARGE;
  }
  if (!otaArchIsUsable(identity.arch) ||
      !otaProfileIsUsable(identity.hardware) ||
      !otaProfileIsUsable(identity.machine) || identity.sha256[0] == '\0' ||
      identity.transferId[0] == '\0' || identity.version[0] == '\0') {
    return OtaResult::SESSION_IDENTITY_MISMATCH;
  }
  if (!sameText(identity.arch, runningTag_.arch, OTA_ARCH_CAPACITY) ||
      !sameText(identity.hardware, runningTag_.hardware,
                OTA_PROFILE_COMPAT_CAPACITY) ||
      !sameText(identity.machine, runningTag_.machine,
                OTA_PROFILE_COMPAT_CAPACITY)) {
    return OtaResult::ARCH_MISMATCH;
  }
  if (sessionActive_ || stagedValid_) {
    return sameSession(session_, identity) ? OtaResult::OK
                                           : OtaResult::SESSION_CONFLICT;
  }

  // A record surviving an earlier discard or a failed boot-time read could
  // otherwise shadow this session's lower generation until its checkpoint.
  if (!removeSessionJournal()) return OtaResult::JOURNAL_FAILED;
  session_ = identity;
  ++sessionGeneration_;
  sessionActive_ = true;
  sessionLastActivityMs_ = now;
  scanner_.reset();
  lastChunkSha256_[0] = '\0';
  lastChunkOffset_ = 0;
  lastChunkLength_ = 0;
  busy_ = false;
  state_ = OtaState::RECEIVING;
  stagedValid_ = false;
  stagedTag_ = OtaImageTag{};
  receivedBytes_ = 0;
  expectedBytes_ = identity.size;
  lastResult_ = OtaResult::OK;
  if (!startSessionSha256()) return finishFailure(OtaResult::NO_MEMORY);
  if (!persistSession()) return finishFailure(OtaResult::JOURNAL_FAILED);
  publishState();
  return OtaResult::OK;
}

bool OpenBrewByWeightOta::verifySessionSha256() {
  if (!sessionSha256_ || session_.size == 0) {
    return false;
  }
  psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
  const psa_status_t cloned = psa_hash_clone(
      static_cast<const psa_hash_operation_t *>(sessionSha256_.get()), &hash);
  uint8_t digest[32] = {};
  size_t digestLength = 0;
  const bool ok = cloned == PSA_SUCCESS &&
                  psa_hash_finish(&hash, digest, sizeof(digest),
                                  &digestLength) == PSA_SUCCESS &&
                  digestLength == sizeof(digest);
  (void)psa_hash_abort(&hash);
  if (!ok) {
    return false;
  }
  char actual[OTA_SHA256_HEX_CAPACITY] = {};
  sha256Hex(digest, actual);
  return sameText(actual, session_.sha256, sizeof(actual));
}

bool OpenBrewByWeightOta::startSessionSha256() {
  clearSessionSha256();
  sessionSha256_.reset(allocInternal(sizeof(psa_hash_operation_t), AllocationOwner::OTA));
  if (!sessionSha256_) return false;
  auto *hash = static_cast<psa_hash_operation_t *>(sessionSha256_.get());
  *hash = PSA_HASH_OPERATION_INIT;
  if (psa_hash_setup(hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
    clearSessionSha256();
    return false;
  }
  return true;
}

bool OpenBrewByWeightOta::updateSessionSha256(const uint8_t *bytes,
                                         size_t length) {
  return sessionSha256_ && bytes != nullptr &&
         psa_hash_update(
             static_cast<psa_hash_operation_t *>(sessionSha256_.get()), bytes,
             length) == PSA_SUCCESS;
}

void OpenBrewByWeightOta::clearSessionSha256() {
  sessionSha256_.reset();
}

OtaResult OpenBrewByWeightOta::writeRange(const char *transferId, uint32_t offset,
                                     uint32_t contentLength,
                                     const OtaStreamIo &io, uint32_t now) {
  TaskLockGuard lock(mutex_);
  if (!sessionActive_) {
    return OtaResult::SESSION_REQUIRED;
  }
  if (transferId == nullptr ||
      strncmp(session_.transferId, transferId, OTA_TRANSFER_ID_CAPACITY) != 0) {
    return OtaResult::SESSION_IDENTITY_MISMATCH;
  }
  if (static_cast<uint32_t>(now - sessionLastActivityMs_) >=
      OTA_SESSION_TTL_MS) {
    finishFailure(OtaResult::SESSION_EXPIRED);
    return OtaResult::SESSION_EXPIRED;
  }
  if (busy_) {
    return OtaResult::BUSY;
  }
  if (io.read == nullptr || io.stillSafe == nullptr || contentLength == 0 ||
      contentLength > OTA_TRANSFER_CHUNK_BYTES || offset != receivedBytes_ ||
      contentLength > expectedBytes_ - receivedBytes_) {
    return offset != receivedBytes_ ? OtaResult::OFFSET_MISMATCH
                                    : OtaResult::INVALID_RANGE;
  }
  if (offset == 0 && contentLength < OTA_IMAGE_PREFIX_BYTES) {
    return OtaResult::INVALID_RANGE;
  }
  if (offset % OTA_WRITE_ALIGNMENT != 0 ||
      (contentLength != expectedBytes_ - receivedBytes_ &&
       contentLength % OTA_WRITE_ALIGNMENT != 0)) {
    return OtaResult::INVALID_RANGE;
  }

  busy_ = true;
  // XIP keeps cache available for most writes; mapping and driver exceptions
  // still require the buffer and hash state on the internal httpd stack.
  // The network reconciler already holds Wi-Fi at full power while busy.

  const esp_partition_t *target =
      static_cast<const esp_partition_t *>(targetPartition_);
  if (target == nullptr) {
    return finishFailure(OtaResult::UNAVAILABLE);
  }

  OtaChunkBuffer chunk(OTA_CHUNK_BYTES);
  if (!chunk.ok()) {
    return finishFailure(OtaResult::NO_MEMORY);
  }
  uint8_t *const buffer = chunk.bytes;

  psa_hash_operation_t chunkHash = PSA_HASH_OPERATION_INIT;
  bool hashStarted =
      psa_hash_setup(&chunkHash, PSA_ALG_SHA_256) == PSA_SUCCESS;
  uint32_t rangeReceived = 0;
  OtaResult failure = hashStarted ? OtaResult::OK : OtaResult::INTERNAL;

  while (failure == OtaResult::OK && rangeReceived < contentLength) {
    // Shot always wins: abort as soon as the paddle moves, a cycle starts, or
    // K1 closes. Do not wait for a byte interval — Wi-Fi would already be
    // competing with the scale.
    if (!io.stillSafe(io.context)) {
      failure = OtaResult::SAFETY_LOST;
      break;
    }
    const uint32_t remaining = contentLength - rangeReceived;
    const size_t length = remaining < OTA_CHUNK_BYTES ? remaining : OTA_CHUNK_BYTES;
    size_t filled = 0;
    // TCP may split even the image header across reads. Only a complete
    // sector (or the image's final tail) is handed to the flash driver.
    while (filled < length) {
      if (!io.stillSafe(io.context)) {
        failure = OtaResult::SAFETY_LOST;
        break;
      }
      const size_t want = length - filled;
      const int got = io.read(io.context, buffer + filled, want);
      if (!io.stillSafe(io.context)) {
        failure = OtaResult::SAFETY_LOST;
        break;
      }
      if (got <= 0) {
        failure = OtaResult::RECEIVE_FAILED;
        break;
      }
      if (static_cast<size_t>(got) > want) {
        failure = OtaResult::INTERNAL;
        break;
      }
      filled += static_cast<size_t>(got);
    }
    if (failure != OtaResult::OK) break;

    if (!otaHandle_) {
      if (rangeReceived != 0 || length < OTA_IMAGE_PREFIX_BYTES ||
          validateOtaImageHeader(buffer, length) != OtaImageHeaderResult::OK) {
        failure = OtaResult::BAD_IMAGE;
        break;
      }
      esp_ota_handle_t handle = 0;
      FlashIoGuard flash;
      if (!io.stillSafe(io.context)) {
        failure = OtaResult::SAFETY_LOST;
        break;
      }
      if (!flash.ok() ||
          esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        failure = OtaResult::WRITE_FAILED;
        break;
      }
      otaHandle_.reset(static_cast<uint32_t>(handle));
    }
    {
      FlashIoGuard flash;
      if (!io.stillSafe(io.context)) {
        failure = OtaResult::SAFETY_LOST;
        break;
      }
      if (!flash.ok() ||
          esp_ota_write(static_cast<esp_ota_handle_t>(otaHandle_.get()), buffer,
                        length) != ESP_OK) {
        failure = OtaResult::WRITE_FAILED;
        break;
      }
    }
    if (psa_hash_update(&chunkHash, buffer, length) != PSA_SUCCESS ||
        !updateSessionSha256(buffer, length)) {
      failure = OtaResult::WRITE_FAILED;
      break;
    }

    scanner_.feed(buffer, length);
    rangeReceived += static_cast<uint32_t>(length);
    receivedBytes_ += static_cast<uint32_t>(length);
    publishSnapshotLocked();
    yieldFlashIo();
    feedFlashIoWatchdog();
    // The marker is in the early read-only segment. Reject a wrong board or
    // a same-version-but-different build identity before it consumes a full
    // slot, while the final SHA-256 remains the authoritative whole-image
    // check.
    if (scanner_.found()) {
      const OtaImageTag &tag = scanner_.tag();
      if (!otaArchIsUsable(tag.arch)) {
        failure = OtaResult::NO_TAG;
        break;
      }
      if (!sameText(tag.arch, runningTag_.arch, OTA_ARCH_CAPACITY) ||
          !sameText(tag.hardware, runningTag_.hardware,
                    OTA_PROFILE_COMPAT_CAPACITY) ||
          !sameText(tag.machine, runningTag_.machine,
                    OTA_PROFILE_COMPAT_CAPACITY) ||
          !sameText(tag.arch, session_.arch, OTA_ARCH_CAPACITY)) {
        failure = OtaResult::ARCH_MISMATCH;
        break;
      }
      if (!sameText(tag.version, session_.version, OTA_VERSION_CAPACITY)) {
        failure = OtaResult::SESSION_IDENTITY_MISMATCH;
        break;
      }
      if (tag.packed < FW_VERSION_PACKED) {
        failure = OtaResult::DOWNGRADE;
        break;
      }
    }
  }
  if (io.progress != nullptr) io.progress(io.context, receivedBytes_, expectedBytes_);
  uint8_t chunkDigest[32] = {};
  size_t chunkDigestLength = 0;
  const bool hashFinished =
      psa_hash_finish(&chunkHash, chunkDigest, sizeof(chunkDigest),
                      &chunkDigestLength) == PSA_SUCCESS &&
      chunkDigestLength == sizeof(chunkDigest);
  (void)psa_hash_abort(&chunkHash);

  if ((failure == OtaResult::OK && (!otaHandle_ || rangeReceived != contentLength)) ||
      (!hashFinished && failure != OtaResult::SAFETY_LOST)) {
    failure = OtaResult::INTERNAL;
  }
  if (failure != OtaResult::OK) {
    if (failure == OtaResult::RECEIVE_FAILED) {
      // The incomplete RAM sector has never touched flash. The handle,
      // scanner and SHA already describe exactly this accepted prefix.
      lastResult_ = failure;
      lastReceivedBytes_ = receivedBytes_;
      lastExpectedBytes_ = expectedBytes_;
      lastChunkOffset_ = 0;
      lastChunkLength_ = 0;
      lastChunkSha256_[0] = '\0';
      sessionLastActivityMs_ = millis();
      if (otaJournalCheckpointDue(receivedBytes_, journaledBytes_, expectedBytes_) &&
          !persistSession()) return finishFailure(OtaResult::JOURNAL_FAILED);
      busy_ = false;
      publishState();
      return failure;
    }
    return finishFailure(failure);
  }
  sha256Hex(chunkDigest, lastChunkSha256_);
  lastChunkOffset_ = offset;
  lastChunkLength_ = contentLength;
  sessionLastActivityMs_ = millis();

  if (otaJournalCheckpointDue(receivedBytes_, journaledBytes_,
                              expectedBytes_) &&
      !persistSession()) {
    return finishFailure(OtaResult::JOURNAL_FAILED);
  }

  if (receivedBytes_ < expectedBytes_) {
    busy_ = false;
    lastResult_ = OtaResult::OK;
    return OtaResult::OK;
  }
  if (!scanner_.found()) return finishFailure(OtaResult::NO_TAG);
  const OtaImageTag &tag = scanner_.tag();
  if (!otaArchIsUsable(tag.arch)) return finishFailure(OtaResult::NO_TAG);
  if (!sameText(tag.arch, runningTag_.arch, OTA_ARCH_CAPACITY) ||
      !sameText(tag.hardware, runningTag_.hardware,
                OTA_PROFILE_COMPAT_CAPACITY) ||
      !sameText(tag.machine, runningTag_.machine,
                OTA_PROFILE_COMPAT_CAPACITY) ||
      !sameText(tag.arch, session_.arch, OTA_ARCH_CAPACITY)) {
    return finishFailure(OtaResult::ARCH_MISMATCH);
  }
  if (!sameText(tag.version, session_.version, OTA_VERSION_CAPACITY)) {
    return finishFailure(OtaResult::SESSION_IDENTITY_MISMATCH);
  }
  if (tag.packed < FW_VERSION_PACKED) return finishFailure(OtaResult::DOWNGRADE);

  // esp_ota_end re-reads the whole slot and verifies the appended SHA-256, so
  // a transfer that was silently corrupted in flight fails here.
  esp_err_t endStatus = ESP_FAIL;
  {
    FlashIoGuard flash;
    if (flash.ok()) {
      // IDF consumes the handle even when validation fails. Keep ownership
      // until the call actually happens (a lock failure still needs abort).
      endStatus = esp_ota_end(static_cast<esp_ota_handle_t>(otaHandle_.release()));
    }
  }
  if (endStatus != ESP_OK) {
    return finishFailure(OtaResult::VERIFY_FAILED);
  }
  if (!verifySessionSha256()) return finishFailure(OtaResult::HASH_MISMATCH);

  if (!readImageDigest(target, stagedImageSha256_)) {
    return finishFailure(OtaResult::VERIFY_FAILED);
  }

  // One last look at the machine before advertising the image as flashable.
  if (!io.stillSafe(io.context)) {
    return finishFailure(OtaResult::SAFETY_LOST);
  }

  stagedTag_ = tag;
  stagedTagOffset_ = scanner_.tagOffset();
  stagedValid_ = true;
  sessionActive_ = false;
  clearSessionSha256();
  journaledBytes_ = 0;
  removeSessionJournal();
  busy_ = false;
  state_ = OtaState::STAGED;
  lastResult_ = OtaResult::OK;
  lastReceivedBytes_ = receivedBytes_;
  lastExpectedBytes_ = expectedBytes_;
  publishState();
  return OtaResult::OK;
}

bool OpenBrewByWeightOta::reconfirmStagedTag() {
  const esp_partition_t *target =
      static_cast<const esp_partition_t *>(targetPartition_);
  if (target == nullptr || !stagedValid_) {
    return false;
  }
  if (stagedTagOffset_ >= target->size) {
    return false;
  }
  size_t length = OTA_TAG_REREAD_BYTES;
  if (stagedTagOffset_ + length > target->size) {
    length = target->size - stagedTagOffset_;
  }
  uint8_t bytes[OTA_TAG_REREAD_BYTES] = {};
  FlashIoGuard flash;
  if (!flash.ok() ||
      esp_partition_read(target, stagedTagOffset_, bytes, length) != ESP_OK) {
    return false;
  }
  OtaImageTagScanner scanner;
  scanner.feed(bytes, length);
  return scanner.found() && sameTag(scanner.tag(), stagedTag_);
}

OtaResult OpenBrewByWeightOta::commit() {
  TaskLockGuard lock(mutex_);
  if (!started_ || !available_) {
    return OtaResult::UNAVAILABLE;
  }
  if (busy_ || sessionActive_) {
    return OtaResult::BUSY;
  }
  if (state_ == OtaState::COMMITTED) return OtaResult::COMMITTED;
  if (!stagedValid_) {
    return OtaResult::NOTHING_STAGED;
  }
  // Re-read the identity straight from flash: it proves the slot still holds
  // the exact image that was verified, not just that a verify once happened.
  if (!reconfirmStagedTag()) {
    return finishFailure(OtaResult::VERIFY_FAILED);
  }
  const esp_partition_t *target =
      static_cast<const esp_partition_t *>(targetPartition_);
  // esp_ota_set_boot_partition runs a full image verification of its own and
  // refuses to select a slot that would not boot.
  FlashIoGuard flash;
  if (!flash.ok() || esp_ota_set_boot_partition(target) != ESP_OK) {
    return finishFailure(OtaResult::COMMIT_FAILED);
  }
  stagedValid_ = false;
  state_ = OtaState::COMMITTED;
  publishState();
  return OtaResult::OK;
}

OtaResult OpenBrewByWeightOta::discard() {
  TaskLockGuard lock(mutex_);
  if (state_ == OtaState::COMMITTED) return OtaResult::COMMITTED;
  if (busy_) return OtaResult::BUSY;
  clearSession(true);
  stagedValid_ = false;
  stagedTag_ = OtaImageTag{};
  stagedTagOffset_ = 0;
  receivedBytes_ = 0;
  expectedBytes_ = 0;
  state_ = available_ ? OtaState::IDLE : OtaState::UNAVAILABLE;
  publishState();
  return OtaResult::OK;
}

bool OpenBrewByWeightOta::confirmRunningImage() {
  TaskLockGuard lock(mutex_);
  return confirmRunningImageLocked();
}

bool OpenBrewByWeightOta::readBootState() {
  FlashIoGuard flash(0);
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const auto *running = esp_ota_get_running_partition();
  const esp_err_t result = flash.ok()
      ? esp_ota_get_state_partition(running, &state) : ESP_ERR_TIMEOUT;
  // An initial USB installation has no otadata record yet.
  if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) {
    bootStatus_.state = -2;
    bootStatus_.lastError = result;
    bootStatus_.reason = flash.ok() ? "STATE_ERROR" : "FLASH_BUSY";
    pendingVerify_ = true;
    confirmed_ = false;
    settingsSchemaWritesAdmitted.store(false, std::memory_order_release);
    return false;
  }
  bootStatus_.state = static_cast<int32_t>(state);
  pendingVerify_ = state == ESP_OTA_IMG_PENDING_VERIFY || state == ESP_OTA_IMG_NEW;
  confirmed_ = state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED;
  settingsSchemaWritesAdmitted.store(confirmed_, std::memory_order_release);
  rejected_ = state == ESP_OTA_IMG_INVALID || state == ESP_OTA_IMG_ABORTED;
  bootStatus_.reason = confirmed_ ? "CONFIRMED" : rejected_ ? "REJECTED" : "WAIT_UPTIME";
  bootStatus_.lastError = ESP_OK;
  return true;
}

bool OpenBrewByWeightOta::confirmRunningImageLocked() {
  if (confirmed_) {
    return true;
  }
  // A rejected image must stay rejected: flipping it back to VALID would
  // cancel the armed rollback and leave the bootloader targeting the same
  // slot that just failed to prove itself.
  if (rejected_) {
    return false;
  }
  ++bootStatus_.attempts;
  FlashIoGuard flash(0);
  bootStatus_.lastError = flash.ok()
      ? esp_ota_mark_app_valid_cancel_rollback() : ESP_ERR_TIMEOUT;
  if (bootStatus_.lastError != ESP_OK) {
    bootStatus_.reason = flash.ok() ? "CONFIRM_ERROR" : "FLASH_BUSY";
    return false;
  }
  bootStatus_.state = static_cast<int32_t>(ESP_OTA_IMG_VALID);
  bootStatus_.reason = "CONFIRMED";
  confirmed_ = true;
  pendingVerify_ = false;
  settingsSchemaWritesAdmitted.store(true, std::memory_order_release);
  publishState();
  return true;
}

bool OpenBrewByWeightOta::rejectRunningImage() {
  TaskLockGuard lock(mutex_);
  return rejectRunningImageLocked() == RejectResult::REJECTED;
}

OpenBrewByWeightOta::RejectResult OpenBrewByWeightOta::rejectRunningImageLocked() {
  if (rejected_) {
    return RejectResult::REJECTED;
  }
  if (confirmed_ || busy_) {
    return RejectResult::FAILED;
  }
  // Refusing here is the safe outcome: without a bootable alternative the
  // caller must keep running this image rather than restart into nothing.
  ++bootStatus_.attempts;
  FlashIoGuard flash(0);
  if (!flash.ok()) {
    bootStatus_.reason = "FLASH_BUSY";
    bootStatus_.lastError = ESP_ERR_TIMEOUT;
    return RejectResult::FAILED;
  }
  if (!esp_ota_check_rollback_is_possible()) {
    bootStatus_.reason = "NO_ROLLBACK";
    return RejectResult::NO_ALTERNATIVE;
  }
  bootStatus_.lastError = esp_ota_mark_app_invalid_rollback();
  if (bootStatus_.lastError != ESP_OK) {
    bootStatus_.reason = "ROLLBACK_ERROR";
    return RejectResult::FAILED;
  }
  bootStatus_.state = static_cast<int32_t>(ESP_OTA_IMG_INVALID);
  bootStatus_.reason = "REJECTED";
  rejected_ = true;
  pendingVerify_ = false;
  publishState();
  return RejectResult::REJECTED;
}

OtaBootStatus OpenBrewByWeightOta::serviceBoot(
    uint32_t now, bool httpReady, bool activeCycle, bool relayClosed,
    bool bleUp, uint32_t minUptimeMs, uint32_t deadlineMs) {
  TaskLockGuard lock(mutex_);
  if (confirmed_ || rejected_) return bootStatus_;
  // These gates always outrank the deadline and retry policy.
  if (activeCycle || relayClosed || busy_ || sessionActive_) {
    bootStatus_.reason = activeCycle ? "WAIT_CYCLE" : relayClosed ? "WAIT_RELAY" : "WAIT_TRANSFER";
    return bootStatus_;
  }
  if (bootAttempted_ && static_cast<uint32_t>(now - bootLastAttemptMs_) < 1000U) {
    return bootStatus_;
  }
  if (bootStatus_.state == -2) {
    bootLastAttemptMs_ = now;
    bootAttempted_ = true;
    ++bootStatus_.attempts;
    if (!readBootState()) return bootStatus_;
    publishState();
    if (confirmed_ || rejected_) return bootStatus_;
  }
  const auto action = decideOtaPendingVerify(pendingVerify_, false, httpReady,
      now, minUptimeMs, deadlineMs, true, !bleUp);
  if (action == OtaPendingVerifyAction::WAIT) {
    bootStatus_.reason = !httpReady ? "WAIT_HTTP" : now < minUptimeMs ? "WAIT_UPTIME" : "WAIT_BLE";
    return bootStatus_;
  }
  if (action == OtaPendingVerifyAction::NONE) return bootStatus_;
  bootLastAttemptMs_ = now;
  bootAttempted_ = true;
  if (action == OtaPendingVerifyAction::CONFIRM) {
    confirmRunningImageLocked();
  } else if (rejectRunningImageLocked() == RejectResult::NO_ALTERNATIVE) {
    if (confirmRunningImageLocked()) bootStatus_.reason = "NO_ROLLBACK";
  }
  return bootStatus_;
}

const char *OpenBrewByWeightOta::resultName(OtaResult result) {
  switch (result) {
    case OtaResult::OK: return "OK";
    case OtaResult::UNAVAILABLE: return "UNAVAILABLE";
    case OtaResult::BUSY: return "BUSY";
    case OtaResult::PENDING_VERIFY: return "PENDING_VERIFY";
    case OtaResult::BAD_LENGTH: return "BAD_LENGTH";
    case OtaResult::TOO_LARGE: return "TOO_LARGE";
    case OtaResult::RECEIVE_FAILED: return "RECEIVE_FAILED";
    case OtaResult::BAD_IMAGE: return "BAD_IMAGE";
    case OtaResult::NO_TAG: return "NO_TAG";
    case OtaResult::ARCH_MISMATCH: return "ARCH_MISMATCH";
    case OtaResult::DOWNGRADE: return "DOWNGRADE";
    case OtaResult::WRITE_FAILED: return "WRITE_FAILED";
    case OtaResult::VERIFY_FAILED: return "VERIFY_FAILED";
    case OtaResult::SAFETY_LOST: return "SAFETY_LOST";
    case OtaResult::NOTHING_STAGED: return "NOTHING_STAGED";
    case OtaResult::COMMIT_FAILED: return "COMMIT_FAILED";
    case OtaResult::NO_MEMORY: return "NO_MEMORY";
    case OtaResult::NO_IDENTITY: return "NO_IDENTITY";
    case OtaResult::SESSION_REQUIRED: return "SESSION_REQUIRED";
    case OtaResult::SESSION_CONFLICT: return "OTA_SESSION_CONFLICT";
    case OtaResult::SESSION_IDENTITY_MISMATCH:
      return "OTA_SESSION_IDENTITY_MISMATCH";
    case OtaResult::OFFSET_MISMATCH: return "OTA_OFFSET_MISMATCH";
    case OtaResult::INVALID_RANGE: return "OTA_INVALID_RANGE";
    case OtaResult::HASH_MISMATCH: return "OTA_SHA256_MISMATCH";
    case OtaResult::SESSION_EXPIRED: return "OTA_SESSION_EXPIRED";
    case OtaResult::JOURNAL_FAILED: return "OTA_JOURNAL_FAILED";
    case OtaResult::INTERNAL: return "INTERNAL";
    case OtaResult::COMMITTED: return "OTA_ALREADY_FLASHED";
  }
  return "UNKNOWN";
}

const char *OpenBrewByWeightOta::stateName(OtaState state) {
  switch (state) {
    case OtaState::UNAVAILABLE: return "unavailable";
    case OtaState::IDLE: return "idle";
    case OtaState::RECEIVING: return "receiving";
    case OtaState::STAGED: return "staged";
    case OtaState::COMMITTED: return "committed";
  }
  return "unknown";
}

}  // namespace openbrewbyweight

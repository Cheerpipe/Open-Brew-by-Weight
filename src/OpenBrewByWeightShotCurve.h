#pragma once

// One explicit curve owner; immutable worker images perform stepped writes.
// Each record owns an 8 KiB block; two sectors separately commit clear epochs.
#include "OpenBrewByWeightDualSlotFlashLog.h"
#include "OpenBrewByWeightShotCurveTypes.h"

namespace openbrewbyweight {

constexpr size_t SHOT_CURVE_BLOCK_BYTES = 8192;
constexpr size_t SHOT_CURVE_BLOCK_COUNT = SHOT_CURVE_CAPACITY + 1U;
constexpr size_t SHOT_CURVE_PARTITION_BYTES =
    2U * FLASH_IO_SECTOR_BYTES + SHOT_CURVE_BLOCK_COUNT * SHOT_CURVE_BLOCK_BYTES;

struct ShotCurveBlockHeader {
  uint32_t magic;
  uint16_t schema;
  uint16_t bytes;
  uint32_t epoch;
  uint32_t sequence;
  uint32_t floor;
  uint32_t shotId;
  uint16_t count;
  uint16_t flags;
  uint32_t checksum;
  uint32_t validity;
};
static_assert(sizeof(ShotCurveBlockHeader) == 36, "Explicit flash block header");

// Unpersisted curves a worker image carries per commit; any remainder stays
// dirty in the live log and is captured by the next image.
constexpr size_t SHOT_CURVE_PERSIST_BATCH = 4;
template <size_t RingCapacity> class BasicShotCurveLog;
using ShotCurveLog = BasicShotCurveLog<SHOT_CURVE_CAPACITY>;
using ShotCurvePersistImage = BasicShotCurveLog<SHOT_CURVE_PERSIST_BATCH>;

// Live log and worker image instances share one emulated partition.
class ShotCurveHostStorage {
 public:
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  static void resetHostStorage() {
    memset(hostFlash_, 0xff, sizeof(hostFlash_));
    hostSaveSucceeds_ = true;
    hostFailAfter_ = -1;
    hostEraseBytes_ = hostWriteBytes_ = 0;
  }
  static void setHostSaveSucceeds(bool succeeds) { hostSaveSucceeds_ = succeeds; }
  static void setHostFailAfter(int operations) { hostFailAfter_ = operations; }
  static size_t hostEraseBytes() { return hostEraseBytes_; }
  static size_t hostWriteBytes() { return hostWriteBytes_; }
  static void corruptHostByte(size_t offset) {
    if (offset < sizeof(hostFlash_)) hostFlash_[offset] ^= 0x80;
  }
  static uint8_t *hostFlashForTest() { return hostFlash_; }

 protected:
  inline static uint8_t hostFlash_[SHOT_CURVE_PARTITION_BYTES] = {};
  inline static bool hostSaveSucceeds_ = true;
  inline static int hostFailAfter_ = -1;
  inline static size_t hostEraseBytes_ = 0, hostWriteBytes_ = 0;
#endif
};

template <size_t RingCapacity>
class BasicShotCurveLog : public ShotCurveHostStorage {
 public:
  bool load() {
    resetShotCurveStore(store_);
    memset(blocks_, 0, sizeof(blocks_));
    epoch_ = durableEpoch_ = sequence_ = floor_ = 0;
    epochSlot_ = 0;
    tx_ = {};
    deletionCount_ = 0;
    dirty_ = false;
    if (!tryLockFlashIo()) return false;
    uint32_t meta[4] = {};
    for (uint8_t slot = 0; slot < 2; ++slot) {
      if (!read(slot * FLASH_IO_SECTOR_BYTES, meta, sizeof(meta))) {
        unlockFlashIo();
        return false;
      }
      if (meta[0] == kEpochMagic && meta[2] == epochChecksum(meta) &&
          meta[3] == 0 && meta[1] >= epoch_) {
        epoch_ = durableEpoch_ = meta[1];
        epochSlot_ = slot;
      }
    }
    for (size_t block = 0; block < SHOT_CURVE_BLOCK_COUNT; ++block) {
      ShotCurveBlockHeader header = {};
      if (!read(blockOffset(block), &header, sizeof(header))) {
        unlockFlashIo();
        return false;
      }
      if (!validHeader(header) || !read(blockOffset(block), disk_, header.bytes) ||
          blockChecksum(header.bytes) != header.checksum ||
          !decode(store_.records[0])) continue;
      // All current-epoch committed headers carry the retention floor,
      // including deleted records, so deletion cannot revive an older curve.
      if (header.epoch != epoch_) continue;
      if (header.sequence > sequence_) sequence_ = header.sequence;
      if (header.floor > floor_) floor_ = header.floor;
      blocks_[block] = header;
    }
    for (size_t n = 0; n < SHOT_CURVE_CAPACITY; ++n) {
      size_t best = SHOT_CURVE_BLOCK_COUNT;
      uint32_t last = n == 0 ? UINT32_MAX : loadSequence_;
      for (size_t block = 0; block < SHOT_CURVE_BLOCK_COUNT; ++block) {
        const auto &h = blocks_[block];
        if (!retained(h) || (n != 0 && h.sequence >= last) || containsShotId(h.shotId))
          continue;
        if (best == SHOT_CURVE_BLOCK_COUNT ||
            h.sequence > blocks_[best].sequence) best = block;
      }
      if (best == SHOT_CURVE_BLOCK_COUNT) break;
      const auto &h = blocks_[best];
      if (!read(blockOffset(best), disk_, h.bytes)) {
        unlockFlashIo();
        return false;
      }
      ShotCurveRecord &record = store_.records[store_.header.count];
      if (decode(record)) {
        ++store_.header.count;
        store_.header.writeIndex = store_.header.count % SHOT_CURVE_CAPACITY;
      }
      loadSequence_ = h.sequence;
    }
    // Boot selected newest first; restore the API's oldest-first RAM ring.
    for (size_t a = 0, b = store_.header.count; a < b / 2U; ++a) {
      memcpy(disk_, &store_.records[a], sizeof(ShotCurveRecord));
      store_.records[a] = store_.records[b - 1U - a];
      memcpy(&store_.records[b - 1U - a], disk_, sizeof(ShotCurveRecord));
    }
    unlockFlashIo();
    return true;
  }

  bool append(const ShotCurveRecord &record, bool persistNow = true) {
    if (!validShotCurveRecord(record)) return false;
    // Shot IDs are immutable sidecar identities; retrying an append is safe.
    if (containsShotId(record.shotId)) return true;
    copyShotCurve(store_.records[store_.header.writeIndex], record);
    store_.header.writeIndex = (store_.header.writeIndex + 1U) % SHOT_CURVE_CAPACITY;
    if (store_.header.count < SHOT_CURVE_CAPACITY) ++store_.header.count;
    dirty_ = true;
    return !persistNow || save();
  }

  bool removeById(uint32_t id, bool persistNow = true) {
    if (!containsShotId(id)) return false;
    if (deletionCount_ == SHOT_CURVE_CAPACITY) return false;
    deletions_[deletionCount_++] = id;
    compactShotCurveStore(store_);
    for (size_t i = 0; i < store_.header.count; ++i) {
      if (store_.records[i].shotId != id) continue;
      memmove(store_.records + i, store_.records + i + 1U,
              (store_.header.count - i - 1U) * sizeof(ShotCurveRecord));
      --store_.header.count;
      store_.header.writeIndex = store_.header.count % SHOT_CURVE_CAPACITY;
      dirty_ = true;
      return !persistNow || save();
    }
    return false;
  }

  bool clear(bool persistNow = true) {
    if (epoch_ == UINT32_MAX) return false;
    ++epoch_;
    deletionCount_ = 0;
    resetShotCurveStore(store_);
    dirty_ = true;
    return !persistNow || save();
  }

  bool save(uint32_t timeout = FLASH_IO_LOCK_TIMEOUT_MS) {
    FlashStoreStepResult result;
    do { result = flushStep(timeout); } while (result == FlashStoreStepResult::MORE);
    if (result == FlashStoreStepResult::FAILED) {
      // Immediate caller expects rollback; deferred worker retries its image.
      if (flashIoLockTimeouts() == lastLockTimeouts_) (void)load();
      return false;
    }
    return true;
  }
  bool flush(uint32_t timeout = FLASH_IO_CONTROL_LOCK_TIMEOUT_MS) {
    return save(timeout);
  }

  FlashStoreStepResult flushStep(uint32_t timeout = FLASH_IO_LOCK_TIMEOUT_MS) {
    lastLockTimeouts_ = flashIoLockTimeouts();
    if (!dirty_) return FlashStoreStepResult::COMPLETE;
    if (tx_.phase != FlashStoreTransaction::Phase::IDLE) {
      const auto result = step(timeout);
      if (result != FlashStoreStepResult::COMPLETE) return result;
      if (!tryLockFlashIo(timeout)) return FlashStoreStepResult::FAILED;
      const bool verified = read(tx_.targetOffset, verify_, tx_.totalBytes) &&
                            memcmp(disk_, verify_, tx_.totalBytes) == 0;
      unlockFlashIo();
      if (!verified) return FlashStoreStepResult::FAILED;
      if (writingEpoch_) {
        durableEpoch_ = epoch_;
        epochSlot_ = static_cast<uint8_t>(tx_.targetOffset / FLASH_IO_SECTOR_BYTES);
        floor_ = 0;
      } else {
        memcpy(&blocks_[targetBlock_], disk_, sizeof(ShotCurveBlockHeader));
        sequence_ = blocks_[targetBlock_].sequence;
        floor_ = blocks_[targetBlock_].floor;
      }
      return FlashStoreStepResult::MORE;
    }
    if (epoch_ != durableEpoch_) {
      uint32_t meta[4] = {kEpochMagic, epoch_, 0, 0};
      meta[2] = epochChecksum(meta);
      memcpy(disk_, meta, sizeof(meta));
      writingEpoch_ = true;
      beginFlashStoreTransaction(tx_, (1U - epochSlot_) * FLASH_IO_SECTOR_BYTES,
                                FLASH_IO_SECTOR_BYTES, 12, sizeof(meta));
      return step(timeout);
    }
    for (size_t block = 0; block < SHOT_CURVE_BLOCK_COUNT; ++block) {
      auto &h = blocks_[block];
      if (h.magic == 0 || h.epoch != epoch_ || h.validity != kCommitted ||
          !deletionPending(h.shotId)) continue;
      const uint32_t deleted = kDeleted;
      if (!tryLockFlashIo(timeout)) return FlashStoreStepResult::FAILED;
      const bool ok = write(blockOffset(block) + offsetof(ShotCurveBlockHeader, validity),
                            &deleted, sizeof(deleted));
      unlockFlashIo();
      if (!ok) return FlashStoreStepResult::FAILED;
      h.validity = deleted;
      return FlashStoreStepResult::MORE;
    }
    for (size_t n = 0; n < store_.header.count; ++n) {
      size_t index = (store_.header.writeIndex + RingCapacity -
                      store_.header.count + n) % RingCapacity;
      const auto &record = store_.records[index];
      if (persisted(record.shotId)) continue;
      if (sequence_ == UINT32_MAX) return FlashStoreStepResult::FAILED;
      targetBlock_ = SHOT_CURVE_BLOCK_COUNT;
      uint32_t oldest = UINT32_MAX;
      size_t retainedCount = 0;
      for (size_t block = 0; block < SHOT_CURVE_BLOCK_COUNT; ++block) {
        const auto &h = blocks_[block];
        if (retained(h)) {
          ++retainedCount;
          if (h.sequence < oldest) oldest = h.sequence;
        } else if (targetBlock_ == SHOT_CURVE_BLOCK_COUNT) targetBlock_ = block;
      }
      if (targetBlock_ == SHOT_CURVE_BLOCK_COUNT) {
        // The extra block always lies below the durable retention floor.
        return FlashStoreStepResult::FAILED;
      }
      encode(record, retainedCount >= SHOT_CURVE_CAPACITY ? oldest + 1U : floor_);
      writingEpoch_ = false;
      const size_t bytes = reinterpret_cast<ShotCurveBlockHeader *>(disk_)->bytes;
      beginFlashStoreTransaction(tx_, blockOffset(targetBlock_),
                                ((bytes + FLASH_IO_SECTOR_BYTES - 1U) /
                                 FLASH_IO_SECTOR_BYTES) * FLASH_IO_SECTOR_BYTES,
                                sizeof(ShotCurveBlockHeader), bytes);
      return step(timeout);
    }
    dirty_ = false;
    deletionCount_ = 0;
    return FlashStoreStepResult::COMPLETE;
  }

  void capturePersistenceImage(ShotCurvePersistImage &image) const {
    memcpy(image.blocks_, blocks_, sizeof(blocks_));
    image.epoch_ = epoch_;
    image.durableEpoch_ = durableEpoch_;
    image.sequence_ = sequence_;
    image.floor_ = floor_;
    image.epochSlot_ = epochSlot_;
    image.deletionCount_ = deletionCount_;
    memcpy(image.deletions_, deletions_, deletionCount_ * sizeof(deletions_[0]));
    image.tx_ = {};
    image.dirty_ = dirty_;
    image.store_.header = store_.header;
    image.store_.header.count = image.store_.header.writeIndex = 0;
    if (!dirty_) return;
    // Oldest-first commits leave only an uncommitted suffix. Measure it from
    // the newest end, then copy its oldest batch in commit order.
    size_t pending = 0;
    while (pending < store_.header.count && !persisted(newest(pending).shotId)) ++pending;
    const size_t batch =
        pending < SHOT_CURVE_PERSIST_BATCH ? pending : SHOT_CURVE_PERSIST_BATCH;
    for (size_t n = 0; n < batch; ++n)
      copyShotCurve(image.store_.records[n], newest(pending - 1U - n));
    image.store_.header.count = static_cast<uint16_t>(batch);
    image.store_.header.writeIndex =
        static_cast<uint16_t>(batch % SHOT_CURVE_PERSIST_BATCH);
  }

  void acknowledgePersisted(const ShotCurvePersistImage &image, bool clearDirty) {
    memcpy(blocks_, image.blocks_, sizeof(blocks_));
    sequence_ = image.sequence_;
    durableEpoch_ = image.durableEpoch_;
    epochSlot_ = image.epochSlot_;
    floor_ = image.floor_;
    tx_ = {};
    if (clearDirty) {
      // A capped batch leaves newer curves unpersisted for the next image.
      dirty_ = store_.header.count > 0 && !persisted(newest(0).shotId);
      deletionCount_ = 0;
    }
  }
  bool dirty() const { return dirty_; }
  size_t count() const { return store_.header.count; }
  const ShotCurveRecord *findNewestById(uint32_t id) const {
    if (id == 0) return nullptr;
    size_t index = store_.header.writeIndex;
    for (size_t n = 0; n < store_.header.count; ++n) {
      index = (index + RingCapacity - 1U) % RingCapacity;
      if (store_.records[index].shotId == id) return &store_.records[index];
    }
    return nullptr;
  }
  bool containsId(uint32_t id) const { return findNewestById(id) != nullptr; }
  bool containsShotId(uint32_t id) const { return containsId(id); }
  bool copyByShotId(uint32_t id, ShotCurveRecord &out) const {
    const auto *record = findNewestById(id);
    if (!record) return false;
    copyShotCurve(out, *record);
    return true;
  }
  size_t copyNewestFirst(ShotCurveRecord *out, size_t capacity) const {
    if (out == nullptr) return 0;
    size_t count = store_.header.count < capacity ? store_.header.count : capacity;
    size_t index = store_.header.writeIndex;
    for (size_t n = 0; n < count; ++n) {
      index = (index + RingCapacity - 1U) % RingCapacity;
      out[n] = store_.records[index];
    }
    return count;
  }

 private:
  static constexpr uint32_t kEpochMagic = 0x53434550;
  static constexpr uint32_t kCommitted = UINT32_MAX - 1U;
  static constexpr uint32_t kDeleted = UINT32_MAX - 3U;
  // Aligned schema-3 worst case of a valid record: 36 header + 18 fixed +
  // the record's stream bound + 2 x 151 bitmap bytes = 5630 -> 5632.
  static constexpr size_t kDiskCapacity =
      (sizeof(ShotCurveBlockHeader) + 18U + SHOT_CURVE_STREAM_BYTES +
       2U * SHOT_CURVE_BREAK_BYTES + 3U) & ~size_t(3U);
  static_assert(kDiskCapacity == 5632 && kDiskCapacity <= SHOT_CURVE_BLOCK_BYTES,
                "Every valid record fits one aligned block");
  static_assert(sizeof(ShotCurveRecord) <= kDiskCapacity,
                "The load-order swap borrows disk_ as one record of scratch");
  bool deletionPending(uint32_t id) const {
    for (size_t i = 0; i < deletionCount_; ++i)
      if (deletions_[i] == id) return true;
    return false;
  }
  static size_t blockOffset(size_t block) {
    return 2U * FLASH_IO_SECTOR_BYTES + block * SHOT_CURVE_BLOCK_BYTES;
  }
  static uint32_t epochChecksum(const uint32_t *meta) {
    return ~crc32Update(0xffffffffU, reinterpret_cast<const uint8_t *>(meta), 8);
  }
  static bool validHeader(const ShotCurveBlockHeader &h) {
    return h.magic == SHOT_CURVE_MAGIC &&
           h.schema == SHOT_CURVE_SCHEMA_VERSION && h.bytes >= sizeof(h) + 18U &&
           h.bytes <= kDiskCapacity && h.bytes % 4U == 0 &&
           h.shotId != 0 && h.sequence != 0 && h.floor <= h.sequence &&
           h.count <= SHOT_CURVE_MAX_POINTS && (h.flags & ~2U) == 0U &&
           (h.validity == kCommitted || h.validity == kDeleted);
  }
  bool retained(const ShotCurveBlockHeader &h) const {
    return h.magic == SHOT_CURVE_MAGIC && h.epoch == epoch_ &&
           h.validity == kCommitted && h.sequence >= floor_;
  }
  // n-th newest record of the ring (0 = newest); caller bounds n by count.
  const ShotCurveRecord &newest(size_t n) const {
    return store_.records[(store_.header.writeIndex + RingCapacity - 1U - n) %
                          RingCapacity];
  }
  bool persisted(uint32_t id) const {
    for (const auto &h : blocks_)
      if (retained(h) && h.shotId == id) return true;
    return false;
  }
  uint32_t blockChecksum(size_t bytes) const {
    uint32_t crc = crc32Update(0xffffffffU, disk_, offsetof(ShotCurveBlockHeader, checksum));
    return ~crc32Update(crc, disk_ + sizeof(ShotCurveBlockHeader),
                        bytes - sizeof(ShotCurveBlockHeader));
  }
  // The record's stream is the schema-3 payload as is: no re-encoding.
  void encode(const ShotCurveRecord &r, uint32_t floor) {
    memset(disk_, 0, sizeof(disk_));
    auto *h = reinterpret_cast<ShotCurveBlockHeader *>(disk_);
    uint8_t *p = disk_ + sizeof(*h);
    memcpy(p, &r.atmClearedMs, 18); p += 18;
    memcpy(p, r.stream, r.bytes); p += r.bytes;
    const size_t bits = (r.count + 7U) / 8U;
    memcpy(p, r.breakBefore, bits);
    memcpy(p + bits, r.held, bits);
    // Bits past the count are stale record bytes, never observations.
    if (r.count % 8U != 0U) {
      const uint8_t tail = static_cast<uint8_t>((1U << (r.count % 8U)) - 1U);
      p[bits - 1U] &= tail;
      p[2U * bits - 1U] &= tail;
    }
    p += 2U * bits;
    h->magic = SHOT_CURVE_MAGIC;
    h->schema = SHOT_CURVE_SCHEMA_VERSION;
    h->bytes = static_cast<uint16_t>((p - disk_ + 3U) & ~size_t(3U));
    h->epoch = epoch_;
    h->sequence = sequence_ + 1U;
    h->floor = floor;
    h->shotId = r.shotId;
    h->count = r.count;
    // Bit 1 marks truncation; the other bits stay zero.
    h->flags = r.truncated ? 2U : 0U;
    h->validity = kCommitted;
    h->checksum = blockChecksum(h->bytes);
  }
  bool decode(ShotCurveRecord &r) const {
    const auto *h = reinterpret_cast<const ShotCurveBlockHeader *>(disk_);
    resetShotCurveRecord(r);
    r.shotId = h->shotId;
    r.count = h->count;
    r.truncated = (h->flags & 2U) != 0;
    const size_t bits = (r.count + 7U) / 8U;
    const size_t fixed = sizeof(*h) + 18U;
    if (h->bytes < fixed + 2U * bits) return false;
    const uint8_t *p = disk_ + sizeof(*h);
    memcpy(&r.atmClearedMs, p, 18); p += 18;
    // The stream ends at its count-th point; the bitmaps and zero padding
    // follow, so the walk alone frames the block.
    const size_t room = h->bytes - fixed - 2U * bits;
    r.bytes = static_cast<uint16_t>(room < SHOT_CURVE_STREAM_BYTES ? room : SHOT_CURVE_STREAM_BYTES);
    memcpy(r.stream, p, r.bytes);
    ShotCurveReader walk;
    if (!shotCurveSeek(r, r.count, walk) ||
        ((fixed + walk.offset + 2U * bits + 3U) & ~size_t(3U)) != h->bytes) return false;
    r.bytes = static_cast<uint16_t>(walk.offset);
    r.lastAtMs = walk.atMs;
    r.lastWeightCg = walk.weightCg;
    p += walk.offset;
    memcpy(r.breakBefore, p, bits);
    memcpy(r.held, p + bits, bits);
    // Tail bits at or past count are padding noise, never observations.
    if (r.count % 8U != 0U) {
      const uint8_t tail = static_cast<uint8_t>((1U << (r.count % 8U)) - 1U);
      r.breakBefore[bits - 1U] &= tail;
      r.held[bits - 1U] &= tail;
    }
    for (const uint8_t *pad = p + 2U * bits; pad < disk_ + h->bytes; ++pad)
      if (*pad != 0U) return false;
    return validShotCurveRecord(r);
  }

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  bool read(size_t offset, void *out, size_t bytes) const {
    if (offset + bytes > sizeof(hostFlash_)) return false;
    memcpy(out, hostFlash_ + offset, bytes);
    return true;
  }
  static bool hostOperation() {
    if (!hostSaveSucceeds_ || hostFailAfter_ == 0) return false;
    if (hostFailAfter_ > 0) --hostFailAfter_;
    return true;
  }
  bool write(size_t offset, const void *source, size_t bytes) {
    if (!hostOperation() || offset + bytes > sizeof(hostFlash_)) return false;
    const auto *in = static_cast<const uint8_t *>(source);
    for (size_t i = 0; i < bytes; ++i) hostFlash_[offset + i] &= in[i];
    hostWriteBytes_ += bytes;
    return true;
  }
  FlashStoreStepResult step(uint32_t timeout) {
    if (!tryLockFlashIo(timeout)) return FlashStoreStepResult::FAILED;
    bool ok;
    if (tx_.phase == FlashStoreTransaction::Phase::ERASE) {
      ok = hostOperation();
      if (ok) {
        memset(hostFlash_ + tx_.targetOffset + tx_.offset, 0xff, FLASH_IO_SECTOR_BYTES);
        hostEraseBytes_ += FLASH_IO_SECTOR_BYTES;
        tx_.offset += FLASH_IO_SECTOR_BYTES;
        if (tx_.offset == tx_.eraseBytes) {
          tx_.phase = FlashStoreTransaction::Phase::BODY;
          tx_.offset = tx_.headerBytes;
        }
      }
    } else {
      const bool commit = tx_.phase == FlashStoreTransaction::Phase::COMMIT;
      const size_t offset = commit ? 0 : tx_.offset;
      const size_t remaining = commit ? tx_.headerBytes : tx_.totalBytes - offset;
      const size_t bytes = remaining < FLASH_IO_CHUNK_BYTES ? remaining : FLASH_IO_CHUNK_BYTES;
      ok = write(tx_.targetOffset + offset, disk_ + offset, bytes);
      if (ok) {
        if (commit) tx_.phase = FlashStoreTransaction::Phase::IDLE;
        else {
          tx_.offset += bytes;
          if (tx_.offset == tx_.totalBytes) tx_.phase = FlashStoreTransaction::Phase::COMMIT;
        }
      }
    }
    unlockFlashIo();
    if (!ok) { tx_.phase = FlashStoreTransaction::Phase::IDLE; return FlashStoreStepResult::FAILED; }
    return tx_.phase == FlashStoreTransaction::Phase::IDLE ?
        FlashStoreStepResult::COMPLETE : FlashStoreStepResult::MORE;
  }
#else
  static const esp_partition_t *partition() {
    const auto *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        static_cast<esp_partition_subtype_t>(0x40), "shotcurve");
    return part != nullptr && part->size == SHOT_CURVE_PARTITION_BYTES ? part : nullptr;
  }
  bool read(size_t offset, void *out, size_t bytes) const {
    return flashIoReadChunked(partition(), offset, out, bytes);
  }
  bool write(size_t offset, const void *source, size_t bytes) {
    return flashIoWriteChunked(partition(), offset, source, bytes);
  }
  FlashStoreStepResult step(uint32_t timeout) {
    return flashIoStoreStep(partition(), disk_, tx_, timeout);
  }
#endif

  ShotCurveRingStore<RingCapacity> store_{};
  ShotCurveBlockHeader blocks_[SHOT_CURVE_BLOCK_COUNT]{};
  alignas(4) uint8_t disk_[kDiskCapacity]{};
  alignas(4) uint8_t verify_[kDiskCapacity]{};
  FlashStoreTransaction tx_{};
  uint32_t epoch_ = 0, durableEpoch_ = 0, sequence_ = 0, floor_ = 0;
  uint32_t loadSequence_ = 0, lastLockTimeouts_ = 0;
  size_t targetBlock_ = 0;
  uint8_t epochSlot_ = 0;
  bool dirty_ = false, writingEpoch_ = false;
  uint32_t deletions_[SHOT_CURVE_CAPACITY]{};
  size_t deletionCount_ = 0;

  template <size_t> friend class BasicShotCurveLog;
};

}  // namespace openbrewbyweight

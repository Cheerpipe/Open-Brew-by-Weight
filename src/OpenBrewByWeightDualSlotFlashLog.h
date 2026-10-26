#pragma once

// Shared machinery for the deferred dual-slot flash ring stores (ShotLog,
// ShotCurveLog, HistoryLog): PSRAM working copy, generation flip through the
// inactive slot, chunked internal-SRAM-staged transfers while the flash cache
// is disabled, and the host-test double for deterministic persistence tests.
// Each store header specializes DualSlotFlashLogTraits (partition name, slot
// geometry, and the valid/compact/finalize/reset hooks) and keeps its own
// record-level API. Flash partitions, slot sizes, and on-disk schemas are
// owned by the store headers and stay byte-identical.

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightNvsDualSlot.h"

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <esp_partition.h>
#endif

namespace openbrewbyweight {

// One CRC over the header words every ring mutation moves (generation,
// nextRecordId, count). Shared by the stores' epoch() stream gates so the
// hashing cannot drift between stores.
template <typename Header>
uint32_t dualSlotHeaderEpoch(const Header &header) {
  uint32_t crc = crc32Update(0xFFFFFFFFU,
      reinterpret_cast<const uint8_t *>(&header.generation),
      sizeof(header.generation));
  crc = crc32Update(crc,
      reinterpret_cast<const uint8_t *>(&header.nextRecordId),
      sizeof(header.nextRecordId));
  return crc32Update(crc,
      reinterpret_cast<const uint8_t *>(&header.count),
      sizeof(header.count));
}

template <typename StoreT>
struct DualSlotFlashLogTraits;

template <typename StoreT, bool (*ValidFn)(const StoreT &),
          void (*CompactFn)(StoreT &), void (*FinalizeFn)(StoreT &)>
class DualSlotFlashLog {
  using Traits = DualSlotFlashLogTraits<StoreT>;
  using Record = typename Traits::Record;

 public:
  // A save erases only the sectors the store occupies, not the whole slot:
  // the slot stride and offsets are unchanged and readers read sizeof(store_).
  static constexpr size_t kEraseBytes =
      (sizeof(StoreT) + FLASH_IO_SECTOR_BYTES - 1) / FLASH_IO_SECTOR_BYTES *
      FLASH_IO_SECTOR_BYTES;
  static_assert(kEraseBytes <= Traits::kSlotBytes, "store exceeds its slot");

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  static void resetHostStorage() {
    memset(hostSlots_, 0, sizeof(hostSlots_));
    hostSlotValid_[0] = false;
    hostSlotValid_[1] = false;
    hostSaveSucceeds_ = true;
    hostEraseBytes_ = 0;
  }
  static void setHostSaveSucceeds(bool succeeds) { hostSaveSucceeds_ = succeeds; }
  static size_t hostEraseBytes() { return hostEraseBytes_; }
#endif

  bool load() {
    using Traits = DualSlotFlashLogTraits<StoreT>;
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    uint8_t bestSlot = 0;
    bool haveBest = false;
    for (uint8_t slot = 0; slot < 2; ++slot) {
      if (!hostSlotValid_[slot] || !ValidFn(hostSlots_[slot])) {
        continue;
      }
      if (!haveBest || secondRevisionIsNewer(hostSlots_[bestSlot].header.generation,
                                             hostSlots_[slot].header.generation)) {
        bestSlot = slot;
        haveBest = true;
      }
    }
    if (haveBest) {
      memcpy(&store_, &hostSlots_[bestSlot], sizeof(store_));
      activeSlot_ = bestSlot;
    } else {
      Traits::reset(store_);
      activeSlot_ = 0;
    }
    dirty_ = false;
    return true;
#else
    if (!lockFlashIo()) {
      // A busy lock must not regress a live store (generation, records):
      // keep RAM and retry the flush. Only a never-loaded (zeroed) store
      // takes the empty default.
      const bool live = store_.header.generation != 0;
      if (!live) Traits::reset(store_);
      dirty_ = live;
      return false;
    }
    const esp_partition_t *part = flashLogPartition();
    if (part == nullptr ||
        part->size < Traits::kSlotCount * Traits::kSlotBytes) {
      Traits::reset(store_);
      dirty_ = false;
      unlockFlashIo();
      return false;
    }

    // Headers first, then one full read: the newer header's slot, and the
    // other slot only when that one fails validation (a torn slot does: its
    // header is committed last). Same winner as validating both slots.
    static_assert(offsetof(StoreT, header) == 0, "slot header leads the store");
    decltype(store_.header) headers[2] = {};
    bool headerOk[2] = {};
    for (uint8_t slot = 0; slot < 2; ++slot)
      headerOk[slot] = flashIoReadChunked(part, slot * Traits::kSlotBytes,
                                          &headers[slot], sizeof(headers[slot]));
    const uint8_t newer =
        chooseNewerRevision(headerOk[0], headers[0].generation, headerOk[1],
                            headers[1].generation) == DualSlotChoice::SECOND ? 1 : 0;
    bool loaded = false;
    for (uint8_t attempt = 0; attempt < 2 && !loaded; ++attempt) {
      activeSlot_ = attempt == 0 ? newer : static_cast<uint8_t>(1U - newer);
      loaded = readSlot(part, activeSlot_ * Traits::kSlotBytes, store_) &&
               ValidFn(store_);
    }
    if (!loaded) {
      Traits::reset(store_);
      activeSlot_ = 0;
    }
    dirty_ = false;
    unlockFlashIo();
    return true;
#endif
  }

  bool save(uint32_t lockTimeoutMs = FLASH_IO_LOCK_TIMEOUT_MS) {
    if (!tryLockFlashIo(lockTimeoutMs)) {
      return false;
    }
    CompactFn(store_);
    bumpGeneration();
    FinalizeFn(store_);
    const uint8_t targetSlot = static_cast<uint8_t>(1U - (activeSlot_ & 1U));
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    if (!hostSaveSucceeds_) {
      unlockFlashIo();
      return false;
    }
    hostEraseBytes_ += kEraseBytes;
    memcpy(&hostSlots_[targetSlot], &store_, sizeof(store_));
    hostSlotValid_[targetSlot] = true;
    activeSlot_ = targetSlot;
    dirty_ = false;
    unlockFlashIo();
    return true;
#else
    const esp_partition_t *part = flashLogPartition();
    if (part == nullptr ||
        part->size < Traits::kSlotCount * Traits::kSlotBytes) {
      unlockFlashIo();
      return false;
    }
    yieldFlashIo();
    feedFlashIoWatchdog();
    const size_t targetOffset =
        static_cast<size_t>(targetSlot) * Traits::kSlotBytes;
    if (esp_partition_erase_range(part, targetOffset, kEraseBytes) != ESP_OK) {
      unlockFlashIo();
      return false;
    }
    yieldFlashIo();
    feedFlashIoWatchdog();
    // Chunked write: each 1 KiB step stages through the internal scratch
    // because the live store_ sits in PSRAM BSS, which may be unreachable
    // if a flash path disables cache despite XIP. Body first, header last,
    // like flushStep, so a torn save never presents a committed header.
    const auto *bytes = reinterpret_cast<const uint8_t *>(&store_);
    constexpr size_t kHeaderBytes = sizeof(store_.header);
    if (!flashIoWriteChunked(part, targetOffset + kHeaderBytes, bytes + kHeaderBytes,
                             sizeof(store_) - kHeaderBytes) ||
        !flashIoWriteChunked(part, targetOffset, bytes, kHeaderBytes)) {
      unlockFlashIo();
      return false;
    }
    activeSlot_ = targetSlot;
    dirty_ = false;
    unlockFlashIo();
    return true;
#endif
  }

  bool flush(uint32_t lockTimeoutMs = FLASH_IO_CONTROL_LOCK_TIMEOUT_MS) {
    if (!dirty_) {
      return true;
    }
    return save(lockTimeoutMs);
  }

  FlashStoreStepResult flushStep(
      uint32_t lockTimeoutMs = FLASH_IO_LOCK_TIMEOUT_MS) {
    if (!dirty_) return FlashStoreStepResult::COMPLETE;
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    return save(lockTimeoutMs) ? FlashStoreStepResult::COMPLETE
                               : FlashStoreStepResult::FAILED;
#else
    const esp_partition_t *part = flashLogPartition();
    if (part == nullptr ||
        part->size < Traits::kSlotCount * Traits::kSlotBytes) {
      return FlashStoreStepResult::FAILED;
    }
    if (persistProgress_.phase == FlashStoreTransaction::Phase::IDLE) {
      CompactFn(store_);
      bumpGeneration();
      FinalizeFn(store_);
      beginFlashStoreTransaction(
          persistProgress_,
          static_cast<size_t>(1U - (activeSlot_ & 1U)) * Traits::kSlotBytes,
          kEraseBytes, Traits::kHeaderBytes, sizeof(store_));
    }
    const FlashStoreStepResult result = flashIoStoreStep(
        part, &store_, persistProgress_, lockTimeoutMs);
    if (result == FlashStoreStepResult::COMPLETE) {
      activeSlot_ = static_cast<uint8_t>(1U - (activeSlot_ & 1U));
      dirty_ = false;
    }
    return result;
#endif
  }

  bool dirty() const { return dirty_; }

  void capturePersistenceImage(DualSlotFlashLog &image) const {
    if (dirty_) image = *this;
    image.activeSlot_ = activeSlot_;
    image.store_.header.generation = store_.header.generation;
    image.dirty_ = dirty_;
    image.persistProgress_ = {};
  }

  void acknowledgePersisted(const DualSlotFlashLog &image, bool clearDirty) {
    // Carry flash progress forward without replacing newer live RAM records.
    activeSlot_ = image.activeSlot_;
    store_.header.generation = image.store_.header.generation;
    if (clearDirty) dirty_ = false;
  }

  // Record-level ring API shared by the stores. The per-store traits supply
  // the record type, ring capacity, and the record field that carries the
  // record id (ShotLog/History use `id`, ShotCurve uses `shotId`).
  const Record *findNewestById(uint32_t id) const {
    if (id == 0 || store_.header.count == 0) {
      return nullptr;
    }
    size_t index = store_.header.writeIndex;
    for (size_t n = 0; n < store_.header.count; ++n) {
      if (index == 0) {
        index = Traits::kCapacity;
      }
      --index;
      if (Traits::recordIdOf(store_.records[index]) == id) {
        return &store_.records[index];
      }
    }
    return nullptr;
  }

  Record *findNewestById(uint32_t id) {
    return const_cast<Record *>(
        static_cast<const DualSlotFlashLog *>(this)->findNewestById(id));
  }

  bool containsId(uint32_t id) const { return findNewestById(id) != nullptr; }

  size_t copyNewestFirst(Record *output, size_t capacity) const {
    if (output == nullptr || capacity == 0 || store_.header.count == 0) {
      return 0;
    }
    const size_t toCopy =
        store_.header.count < capacity ? store_.header.count : capacity;
    size_t index = store_.header.writeIndex;
    for (size_t copied = 0; copied < toCopy; ++copied) {
      if (index == 0) {
        index = Traits::kCapacity;
      }
      --index;
      output[copied] = store_.records[index];
    }
    return toCopy;
  }

  // Assigns the next record id, advances the ring, and persists. On a failed
  // immediate save the store is rolled back when the failure was a lock
  // timeout, or reloaded from the last-good slot otherwise (compaction may
  // have moved records, making the pre-append snapshot stale).
  bool append(const Record &record, bool persistNow = true) {
    const uint32_t lockTimeoutsBefore = flashIoLockTimeouts();
    const uint16_t previousWriteIndex = store_.header.writeIndex;
    const uint16_t previousCount = store_.header.count;
    const uint32_t previousNextRecordId = store_.header.nextRecordId;
    const Record overwritten = store_.records[previousWriteIndex];

    Record stored = record;
    stored.id = store_.header.nextRecordId;
    if (store_.header.nextRecordId < UINT32_MAX) {
      ++store_.header.nextRecordId;
    }
    store_.records[store_.header.writeIndex] = stored;
    store_.header.writeIndex =
        static_cast<uint16_t>((store_.header.writeIndex + 1U) %
                              Traits::kCapacity);
    if (store_.header.count < Traits::kCapacity) {
      ++store_.header.count;
    }
    if (!persistNow) {
      dirty_ = true;
      return true;
    }
    if (save()) {
      return true;
    }
    if (flashIoLockTimeouts() == lockTimeoutsBefore) {
      load();
      return false;
    }
    store_.records[previousWriteIndex] = overwritten;
    store_.header.writeIndex = previousWriteIndex;
    store_.header.count = previousCount;
    store_.header.nextRecordId = previousNextRecordId;
    return false;
  }

  // Compacts to a linear prefix so deletion is a memmove, avoiding a second
  // capacity-sized record array on the loopTask stack.
  bool removeById(uint32_t id, bool persistNow = true) {
    if (id == 0 || store_.header.count == 0) {
      return false;
    }
    if (persistNow && !lockFlashIo()) {
      return false;
    }
    CompactFn(store_);
    bool found = false;
    size_t foundIndex = 0;
    for (size_t index = 0; index < store_.header.count; ++index) {
      if (Traits::recordIdOf(store_.records[index]) == id) {
        found = true;
        foundIndex = index;
        break;
      }
    }
    if (!found) {
      if (persistNow) unlockFlashIo();
      return false;
    }
    const uint16_t previousCount = store_.header.count;
    if (foundIndex + 1U < previousCount) {
      memmove(&store_.records[foundIndex], &store_.records[foundIndex + 1U],
              static_cast<size_t>(previousCount - foundIndex - 1U) *
                  sizeof(Record));
    }
    --store_.header.count;
    store_.header.writeIndex =
        static_cast<uint16_t>(store_.header.count % Traits::kCapacity);
    memset(&store_.records[store_.header.count], 0, sizeof(Record));
    if (!persistNow) {
      dirty_ = true;
      return true;
    }
    const bool saved = save();
    unlockFlashIo();
    if (saved) {
      return true;
    }
    load();
    return false;
  }

 protected:
  // Generations wrap like the settings revision (secondRevisionIsNewer picks
  // the newer slot across the wrap); 0 is reserved for a never-saved store.
  void bumpGeneration() {
    if (++store_.header.generation == 0) store_.header.generation = 1;
  }

  // Persists after a mutation: defer when allowed, otherwise save now and
  // reload the last-good slot when the immediate save fails.
  bool persistMutation(bool persistNow) {
    if (!persistNow) {
      dirty_ = true;
      return true;
    }
    if (save()) {
      return true;
    }
    load();
    return false;
  }

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  // Looked up once: the table is immutable and every flushStep needs it.
  static const esp_partition_t *flashLogPartition() {
    static const esp_partition_t *const part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
        DualSlotFlashLogTraits<StoreT>::kPartitionName);
    return part;
  }

  static bool readSlot(const esp_partition_t *part, size_t offset,
                       StoreT &dest) {
    memset(&dest, 0, sizeof(dest));
    if (part == nullptr) {
      return false;
    }
    return flashIoReadChunked(part, offset, &dest, sizeof(dest));
  }
#endif

  StoreT store_{};
  FlashStoreTransaction persistProgress_{};
  uint8_t activeSlot_ = 0;
  bool dirty_ = false;

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  inline static StoreT hostSlots_[2] = {};
  inline static bool hostSlotValid_[2] = {};
  inline static bool hostSaveSucceeds_ = true;
  inline static size_t hostEraseBytes_ = 0;
#endif
};

}  // namespace openbrewbyweight

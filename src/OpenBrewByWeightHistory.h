#pragma once

// Activation-history ring store: PSRAM working copy with deferred dual-slot
// flash persistence in the dedicated `history` data partition. The flash
// mechanics live in OpenBrewByWeightDualSlotFlashLog.h; this header owns the
// activation-history schema hooks and the record-level API.

#include "OpenBrewByWeightDualSlotFlashLog.h"
#include "OpenBrewByWeightHistoryTypes.h"
#include "OpenBrewByWeightNvsDualSlot.h"

namespace openbrewbyweight {

// Two 16 KiB slots fill the dedicated history data partition.
constexpr size_t HISTORY_FLASH_SLOT_BYTES = 16384;
constexpr size_t HISTORY_FLASH_SLOT_COUNT = 2;

inline uint32_t historyRecordId(const HistoryRecord &record) {
  return record.id;
}

template <>
struct DualSlotFlashLogTraits<HistoryStore> {
  static constexpr const char *kPartitionName = "history";
  static constexpr size_t kSlotBytes = HISTORY_FLASH_SLOT_BYTES;
  static constexpr size_t kSlotCount = HISTORY_FLASH_SLOT_COUNT;
  static constexpr size_t kHeaderBytes = sizeof(HistoryHeader);
  static constexpr void (*reset)(HistoryStore &) = resetHistoryStore;
  static constexpr void (*widen)(HistoryStore &) = nullptr;
  using Record = HistoryRecord;
  static constexpr size_t kCapacity = HISTORY_CAPACITY;
  static constexpr uint32_t (*recordIdOf)(const HistoryRecord &) =
      historyRecordId;
};

class HistoryLog
    : public DualSlotFlashLog<HistoryStore, validHistoryStore,
                              compactHistoryStore, finalizeHistoryStore> {
 public:

  bool clear(bool persistNow = true) {
    // Keep the monotonic generation: a regressed generation would make the
    // pre-clear slot look newer on the next load.
    const uint32_t generation = store_.header.generation;
    resetHistoryStore(store_);
    store_.header.generation = generation;
    return persistMutation(persistNow);
  }

  size_t count() const { return store_.header.count; }

  uint32_t nextRecordId() const { return store_.header.nextRecordId; }

  // Change epoch for stream gating: one CRC over the header words every
  // mutation moves. On a full ring an append only advances nextRecordId, so
  // generation+count alone would miss it. Caller holds the store mutex.
  uint32_t epoch() const { return dualSlotHeaderEpoch(store_.header); }

  // Fills one bounded page (see HistoryPage) of the records whose HistoryType
  // bit is set in `types` (0 = every type); total, offset, and hasMore count
  // those matches only. offset counts from the newest record for Desc and
  // from the oldest for Asc. Caller holds the store mutex.
  void copyPage(HistoryPage &page, size_t offset, size_t limit,
                ShotLogSortDir dir, uint8_t types = 0) const {
    const size_t count = store_.header.count;
    const size_t oldest =
        (store_.header.writeIndex + HISTORY_CAPACITY - count) % HISTORY_CAPACITY;
    limit = historyClampPageLimit(limit);
    page.total = page.count = 0;
    for (size_t step = 0; step < count; ++step) {
      const HistoryRecord &record = store_.records[
          (oldest + (dir == ShotLogSortDir::Desc ? count - 1U - step : step)) %
          HISTORY_CAPACITY];
      if (types && (record.type > 7 || !((types >> record.type) & 1U))) continue;
      if (page.total >= offset && page.count < limit)
        page.records[page.count++] = record;
      ++page.total;
    }
    page.start = offset < page.total ? offset : page.total;
    page.hasMore = page.start + page.count < page.total;
  }

  // Copies the newest record without materializing a HistoryPage; equivalent
  // to copyPage(page, 0, 1, Desc) without the page frame. Caller holds the
  // store mutex.
  bool copyNewest(HistoryRecord &record) const {
    if (store_.header.count == 0) {
      return false;
    }
    size_t index = store_.header.writeIndex;
    if (index == 0) {
      index = HISTORY_CAPACITY;
    }
    --index;
    record = store_.records[index];
    return true;
  }
};

}  // namespace openbrewbyweight

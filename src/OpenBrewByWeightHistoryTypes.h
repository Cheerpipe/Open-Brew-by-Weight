#pragma once

// Independent activation history: confirmed machine activations and physical
// attempts blocked by No Scale Guard, kept beside the metrics-rich stats shot
// log. Cycle classification is time based; no shot metrics are stored.

#include "OpenBrewByWeightShotLogTypes.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace openbrewbyweight {

constexpr uint32_t HISTORY_MAGIC = 0x41435448U;  // "ACTH"
constexpr uint16_t HISTORY_SCHEMA_VERSION = 0;
constexpr size_t HISTORY_CAPACITY = 1000;
constexpr size_t HISTORY_PAGE_DEFAULT = 20;
// Bounds one HTTP page copy; the handler pages through the ring by offset.
constexpr size_t HISTORY_PAGE_MAX = 120;

constexpr uint8_t HISTORY_FLAG_WALL_TIME = 0x01;
// Set when the activation ended without any registered scale weight; clear
// means weight was registered.
constexpr uint8_t HISTORY_FLAG_NO_WEIGHT = 0x02;

enum class HistoryType : uint8_t {
  SHOT = 0,
  RINSE = 1,
  OTHER = 2,
  POWER_ON = 3,
  NO_SCALE_GUARD_ABORTED = 4,
  BACKFLUSH = 5,
};

inline const char *historyTypeName(HistoryType type) {
  switch (type) {
    case HistoryType::SHOT: return "shot";
    case HistoryType::RINSE: return "rinse";
    case HistoryType::OTHER: return "other";
    case HistoryType::POWER_ON: return "power_on";
    case HistoryType::NO_SCALE_GUARD_ABORTED: return "no_scale_guard_aborted";
    case HistoryType::BACKFLUSH: return "backflush";
  }
  return "unknown";
}

// A rinse end is always `rinse`. Otherwise an activation strictly longer than
// the cycle's BBW protection window counts as a `shot`; anything shorter is
// `other`. This labels the activation's intent, independent of shot metrics.
inline HistoryType historyTypeFromCycle(bool rinseEnd, uint32_t durationMs,
                                        uint32_t protectionMs) {
  if (rinseEnd) {
    return HistoryType::RINSE;
  }
  return durationMs > protectionMs ? HistoryType::SHOT : HistoryType::OTHER;
}

struct HistoryRecord {
  uint32_t id;
  uint32_t endedAtUnixSec;
  uint32_t endedAtLocalSec;
  uint16_t durationDs;
  uint8_t type;   // HistoryType
  uint8_t flags;  // bit 0: hasWallTime, bit 1: no registered weight
};

static_assert(sizeof(HistoryRecord) == 16,
              "HistoryRecord packing is part of the flash partition schema");

struct HistoryHeader {
  uint32_t magic;
  uint16_t schemaVersion;
  uint16_t recordSize;
  uint32_t generation;
  uint32_t nextRecordId;
  uint16_t count;
  uint16_t writeIndex;
  uint32_t checksum;
};

static_assert(sizeof(HistoryHeader) == 24,
              "HistoryHeader packing is part of the flash partition schema");

struct HistoryStore {
  HistoryHeader header;
  HistoryRecord records[HISTORY_CAPACITY];
  RecordStoreMarks marks;
};

static_assert(sizeof(HistoryStore) == 16104,
              "HistoryStore must stay 4-byte aligned for chunked flash I/O and fit one 16 KiB slot");

inline size_t historyClampPageLimit(size_t limit) {
  if (limit < 1U) {
    return 1U;
  }
  if (limit > HISTORY_PAGE_MAX) {
    return HISTORY_PAGE_MAX;
  }
  return limit;
}

inline uint32_t historyChecksum(const HistoryStore &store) {
  uint32_t crc = crc32Update(
      0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&store.header),
      offsetof(HistoryHeader, checksum));
  if (store.header.count > 0) {
    crc = crc32Update(crc, reinterpret_cast<const uint8_t *>(store.records),
                      static_cast<size_t>(store.header.count) *
                          sizeof(HistoryRecord));
  }
  return ~crc;
}

inline void finalizeHistoryStore(HistoryStore &store) {
  store.header.magic = HISTORY_MAGIC;
  store.header.schemaVersion = HISTORY_SCHEMA_VERSION;
  store.header.recordSize = sizeof(HistoryRecord);
  store.header.checksum = 0;
  store.header.checksum = historyChecksum(store);
  store.marks.magic = RECORD_MARKS_MAGIC;
  store.marks.generation = store.header.generation;
}

// Marks an earlier image did not write read as none.
inline void loadedHistoryStore(HistoryStore &store) {
  recordMarksLoaded(store.marks, store.header.generation);
}

inline bool validHistoryStore(const HistoryStore &store) {
  if (store.header.magic != HISTORY_MAGIC ||
      store.header.schemaVersion != HISTORY_SCHEMA_VERSION ||
      store.header.recordSize != sizeof(HistoryRecord) ||
      store.header.count > HISTORY_CAPACITY ||
      store.header.writeIndex >= HISTORY_CAPACITY ||
      store.header.checksum != historyChecksum(store)) {
    return false;
  }
  return true;
}

inline void resetHistoryStore(HistoryStore &store) {
  memset(&store, 0, sizeof(store));
  store.header.generation = 1;
  store.header.nextRecordId = 1;
  finalizeHistoryStore(store);
}

// Pack the ring into records[0..count) (oldest first) in place, mirroring
// compactShotLogStore.
inline void compactHistoryStore(HistoryStore &store) {
  store.header.writeIndex = compactRecordRing(
      store.records, HISTORY_CAPACITY, sizeof(HistoryRecord),
      store.header.count, store.header.writeIndex);
}

// One bounded page out of the ring. `dir` selects the walk order: Desc pages
// from the newest record, Asc from the oldest, matching the shot-log query
// semantics where Asc reverses the whole listing. Trivial aggregate so it can
// share the network work-buffer union; copyPage fills every field.
struct HistoryPage {
  size_t total;
  size_t start;
  size_t count;
  bool hasMore;
  HistoryRecord records[HISTORY_PAGE_MAX];
};

static_assert(sizeof(HistoryPage) <= 2048,
              "HistoryPage lives in the shared network work buffer union");

}  // namespace openbrewbyweight

#pragma once

// Stats shot-log ring store: PSRAM working copy with deferred dual-slot flash
// persistence in the dedicated `shotlog` data partition. The flash mechanics
// (generation flip through the inactive slot, chunked internal-SRAM-staged
// transfers across flash paths that may disable cache) live in
// OpenBrewByWeightDualSlotFlashLog.h; this header owns the shot-log schema hooks
// and the record-level API.

#include "OpenBrewByWeightDualSlotFlashLog.h"
#include "OpenBrewByWeightShotLogTypes.h"

namespace openbrewbyweight {

// Two 504 KiB slots fill the dedicated shotlog data partition; each slot
// leaves room for 1000 records of up to 516 bytes, so later schema growth
// never touches the partition table again. A save erases and rewrites only
// the sectors the store occupies (32 at 1000 records of 128 bytes).
constexpr size_t SHOT_LOG_FLASH_SLOT_BYTES = 516096;
constexpr size_t SHOT_LOG_FLASH_SLOT_COUNT = 2;
static_assert(sizeof(ShotLogStore) <= SHOT_LOG_FLASH_SLOT_BYTES,
              "Two shot-log slots of 1000 records must fit the shotlog partition");

// Only the current schema is accepted; any older slot is discarded. Records
// of an earlier, shorter size are accepted and widened on load.
inline bool validShotLogStoreCurrent(const ShotLogStore &store) {
  return validShotLogStore(store);
}

inline uint32_t shotLogRecordId(const ShotLogRecord &record) {
  return record.id;
}

template <>
struct DualSlotFlashLogTraits<ShotLogStore> {
  static constexpr const char *kPartitionName = "shotlog";
  static constexpr size_t kSlotBytes = SHOT_LOG_FLASH_SLOT_BYTES;
  static constexpr size_t kSlotCount = SHOT_LOG_FLASH_SLOT_COUNT;
  static constexpr size_t kHeaderBytes = sizeof(ShotLogHeader);
  static constexpr void (*reset)(ShotLogStore &) = resetShotLogStore;
  static constexpr void (*widen)(ShotLogStore &) = widenShotLogRecords;
  using Record = ShotLogRecord;
  static constexpr size_t kCapacity = SHOT_LOG_CAPACITY;
  static constexpr uint32_t (*recordIdOf)(const ShotLogRecord &) =
      shotLogRecordId;
};

class ShotLog
    : public DualSlotFlashLog<ShotLogStore, validShotLogStoreCurrent,
                              compactShotLogStore, finalizeShotLogStore> {
 public:
  uint32_t nextRecordId() const { return store_.header.nextRecordId; }

  // Change epoch for stream gating, mirroring HistoryLog::epoch(): one CRC
  // over the header words every mutation moves (rating bumps generation —
  // see updateRating; a full-ring append only advances nextRecordId). Curve
  // stores mutate only alongside these words. Caller holds the store mutex.
  uint32_t epoch() const { return dualSlotHeaderEpoch(store_.header); }

  // The stored shots as corridor statistics rows, oldest first; a bad shot
  // reads as an empty row that no statistic admits.
  auto arrivalRows() const {
    const size_t first = (store_.header.writeIndex + SHOT_LOG_CAPACITY -
                          store_.header.count) % SHOT_LOG_CAPACITY;
    return [this, first](size_t i) {
      const ShotLogRecord &record = store_.records[(first + i) % SHOT_LOG_CAPACITY];
      return shotLogRecordEligible(record) ? shotLogArrivalRecord(record)
                                           : corridor::ArrivalRecord{};
    };
  }

  static corridor::StatsScope statsScope(const ShotPreset &preset) {
    corridor::StatsScope scope;
    scope.presetId = preset.id;
    scope.epoch = preset.learningEpoch;
    scope.fastBeforeMs = static_cast<int32_t>(preset.fastBeforeMs);
    scope.slowAfterMs = static_cast<int32_t>(preset.slowAfterMs);
    scope.goalDg = preset.goalWeightDg;
    return scope;
  }

  // Smooth-cut arrival statistics of one preset since its epoch, for its
  // current window and target (plan §4.8). Caller holds the store mutex.
  corridor::ArrivalStats arrivalStats(const ShotPreset &preset,
                                      int32_t *arrivalsOut = nullptr) const {
    return corridor::arrivalStatsOf(store_.header.count, arrivalRows(), statsScope(preset),
                                    arrivalsOut);
  }

  // The preset's reference plateau flow (rateX100) for the shot predictor's
  // stalled-shot physics gate. Caller holds the store mutex.
  int32_t plateauFlowX100(const ShotPreset &preset) const {
    return corridor::plateauFlowX100(store_.header.count, arrivalRows(),
                                     statsScope(preset));
  }

  // The preset's grind hint, replayed from the stored shots (nothing is
  // persisted). Caller holds the store mutex.
  corridor::GrindHintV2 grindHint(const ShotPreset &preset) const {
    return corridor::grindHintOf(store_.header.count, arrivalRows(), statsScope(preset));
  }

  // Median first drop of one preset since its epoch, or of the other presets.
  // Caller holds the store mutex.
  corridor::FirstDropStats firstDropStats(const ShotPreset &preset,
                                          bool otherPresets = false) const {
    return corridor::firstDropStatsOf(store_.header.count, arrivalRows(), preset.id,
                                      preset.learningEpoch, otherPresets);
  }

  // The integration's ten-shot aggregate. Caller holds the store mutex.
  ShotStatsView statsView() const {
    const size_t write = store_.header.writeIndex;
    return shotLogStatsView(store_.header.count, SHOT_LOG_STATS_WINDOW,
                            [&](size_t n) -> const ShotLogRecord & {
      return store_.records[(write + SHOT_LOG_CAPACITY - 1U - n) % SHOT_LOG_CAPACITY];
    });
  }

  // The drip each newest good shot of a preset needed (offset used + final −
  // cut line, cg), newest first; skipped when the shot did not train the drip.
  // Skipped shots keep their measured value so the chart can draw them
  // hatched; only shots without a computable value stay empty.
  size_t recentDrips(uint8_t presetId, uint32_t *ids, int16_t *neededCg,
                     uint16_t &skippedMask, size_t max) const {
    size_t found = 0;
    skippedMask = 0;
    size_t index = store_.header.writeIndex;
    for (size_t n = 0; n < store_.header.count && found < max; ++n) {
      if (index == 0) index = SHOT_LOG_CAPACITY;
      const ShotLogRecord &record = store_.records[--index];
      if (shotLogPresetId(record) != presetId || !shotLogRecordEligible(record)) continue;
      const bool trained = ((record.extractionExtended >> 5) & 3) == 2;
      const bool measurable = !shotLogWeightIsMissing(record.cutLineCg) &&
                              !shotLogWeightIsMissing(record.actualWeightCg);
      ids[found] = record.id;
      neededCg[found] = measurable
          ? static_cast<int16_t>(record.offsetUsedCg + record.actualWeightCg -
                                 record.cutLineCg)
          : 0;
      if (!trained) skippedMask |= static_cast<uint16_t>(1U << found);
      ++found;
    }
    return found;
  }
  // The newest record `match` accepts, newest first (a predicate that never
  // accepts visits them all). Caller holds the store mutex.
  template <typename Match>
  const ShotLogRecord *findNewest(Match &&match) const {
    size_t index = store_.header.writeIndex;
    for (size_t n = 0; n < store_.header.count; ++n) {
      if (index == 0) index = SHOT_LOG_CAPACITY;
      if (match(store_.records[--index])) return &store_.records[index];
    }
    return nullptr;
  }
  // The newest record with an id below `id`: ids grow with every append, so
  // a walk by id survives appends and deletions in between.
  bool copyNewestBefore(uint32_t id, ShotLogRecord &output) const {
    const ShotLogRecord *found = findNewest([id](const ShotLogRecord &r) { return r.id < id; });
    if (found != nullptr) output = *found;
    return found != nullptr;
  }
  bool copyNewestEligible(ShotLogRecord &output) const {
    const ShotLogRecord *found = findNewest(shotLogRecordEligible);
    if (found != nullptr) output = *found;
    return found != nullptr;
  }

  bool updateRating(uint32_t id, uint8_t rating, bool persistNow = true) {
    ShotLogRecord *found = findNewestById(id);
    if (found == nullptr || rating > SHOT_LOG_RATING_MAX) {
      return false;
    }
    found->sideFlags =
        shotLogPackRating(found->sideFlags, rating);
    // The record bytes changed without moving count or nextRecordId, so bump
    // generation: the modified slot content must win dual-slot selection and
    // stream epochs hash these header words.
    ++store_.header.generation;
    return persistMutation(persistNow);
  }

  bool copyRatingById(uint32_t id, uint8_t &rating) const {
    const ShotLogRecord *found = findNewestById(id);
    if (found == nullptr) {
      return false;
    }
    rating = shotLogRating(found->sideFlags);
    return true;
  }

  bool clear(bool persistNow = true) {
    // Keep the monotonic generation: a regressed generation would make the
    // pre-clear slot look newer on the next load.
    const uint32_t generation = store_.header.generation;
    resetShotLogStore(store_);
    store_.header.generation = generation;
    return persistMutation(persistNow);
  }

  size_t count() const { return store_.header.count; }
};

}  // namespace openbrewbyweight

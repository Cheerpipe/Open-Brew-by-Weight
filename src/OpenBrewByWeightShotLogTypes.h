#pragma once

// Fixed v1 shot-log schema and domain helpers. No Preferences / NVS:
// persistence lives in the dedicated shotlog flash partition (dual slot).

#include "OpenBrewByWeightCorridor.h"
#include "OpenBrewByWeightDomain.h"

#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace openbrewbyweight {

constexpr uint32_t SHOT_LOG_MAGIC = 0x534C4F47U;  // "SLOG"
// Schema 2 appends the machine target temp, preset dose, and brew ratio
// snapshots to the schema 1 record; older slots are discarded. Within schema
// 2 records grow only by appending fields: a store keeps its record size, a
// later image widens shorter records on load, and the schema changes only for
// a change appending cannot express.
constexpr uint16_t SHOT_LOG_SCHEMA_VERSION = 2;
// Record size of the first schema-2 image; never changes.
constexpr uint16_t SHOT_LOG_MIN_RECORD_BYTES = 124;
constexpr size_t SHOT_LOG_CAPACITY = 500;
constexpr size_t SHOT_LOG_PAGE_DEFAULT = 10;
// Maximum number of records in each independent read-time Stats window.
constexpr size_t SHOT_LOG_STATS_WINDOW = 10;
// Windows the Web UI Stats panel can select; the first is the default.
constexpr size_t SHOT_LOG_STATS_WINDOWS[] = {SHOT_LOG_STATS_WINDOW, 50, 100};
constexpr size_t SHOT_LOG_STATS_WINDOW_MAX = 100;
// Options per Shots-list filter group, and the facets the Stats header lists.
constexpr size_t SHOT_LIST_FILTER_MAX = 8;

enum class ShotLogSort : uint8_t { Date = 0, Rating = 1 };
enum class ShotLogSortDir : uint8_t { Desc = 0, Asc = 1 };

inline ShotLogSort shotLogSortFromName(const char *name) {
  if (name != nullptr && strcmp(name, "rating") == 0) {
    return ShotLogSort::Rating;
  }
  return ShotLogSort::Date;
}

inline ShotLogSortDir shotLogSortDirFromName(const char *name) {
  if (name != nullptr && strcmp(name, "asc") == 0) {
    return ShotLogSortDir::Asc;
  }
  return ShotLogSortDir::Desc;
}

inline size_t shotLogClampPageLimit(size_t limit) {
  if (limit < 1U) {
    return 1U;
  }
  if (limit > SHOT_LOG_CAPACITY) {
    return SHOT_LOG_CAPACITY;
  }
  return limit;
}

// Newest-first page into records[offset..). Returns hasMore.
inline bool shotLogPageSlice(size_t total, size_t offset, size_t limit,
                             size_t &start, size_t &pageCount) {
  limit = shotLogClampPageLimit(limit);
  if (offset > total) {
    offset = total;
  }
  start = offset;
  const size_t remaining = total - offset;
  pageCount = remaining < limit ? remaining : limit;
  return (start + pageCount) < total;
}

constexpr float MIN_SHOT_LOG_WEIGHT_G = 2.0f;
constexpr uint32_t MIN_SHOT_LOG_DURATION_MS = 12000U;
// INT16_MIN leaves the full positive int16 centigram range usable.
constexpr int16_t SHOT_LOG_WEIGHT_MISSING = INT16_MIN;
constexpr uint16_t SHOT_LOG_METRIC_MISSING = UINT16_MAX;

inline bool shotLogWeightIsMissing(int16_t centigrams) {
  return centigrams == SHOT_LOG_WEIGHT_MISSING;
}

enum class ShotLogType : uint8_t {
  AUTO = 0,
  TIMER_ONLY = 1,
  MANUAL = 2
};

enum class ShotLogCut : uint8_t {
  AUTO = 0,
  MANUAL = 1,
  LIMIT = 2
};

enum class ShotLogStopDetail : uint8_t {
  NORMAL_TARGET = 0,
  // 1 is retired (wire name "prediction"); never reuse it.
  EXTENDED_MAX_WEIGHT = 2,
  EXTENDED_MIN_TIME = 3,
  AUTO_TO_MANUAL = 4,
  SLOW_MAX_TIME = 5,
  SLOW_MIN_WEIGHT = 6,
  CUP_REMOVED = 7,
  ACTIVATOR = 8,
  WEB_STOP = 9,
  PHYSICAL_OVERRIDE = 10,
  WEB_HEARTBEAT = 11,
  HARD_LIMIT = 12,
  WALL_LIMIT = 13,
  RELAY_SAFETY = 14,
  WEIGHT_ANOMALY = 15,
  TOUCH_WEIGHT_FALLBACK = 16,
  OTHER = 255
};

constexpr uint8_t SHOT_LOG_FAST_GUARD_BIT = 0x01;
constexpr uint8_t SHOT_LOG_SLOW_GUARD_BIT = 0x02;
constexpr uint8_t SHOT_LOG_FAST_EXTENDED_BIT = 0x01;
constexpr uint8_t SHOT_LOG_SLOW_EXTENDED_BIT = 0x02;
// User rating 0–5 is packed into sideFlags. Bits 0–1 are the fast/slow side
// switches in force for the shot.
constexpr uint8_t SHOT_LOG_RATING_SHIFT = 2;
constexpr uint8_t SHOT_LOG_RATING_MASK = 0x1C;
constexpr uint8_t SHOT_LOG_RATING_MAX = 5;

inline uint8_t shotLogPackGuardFlags(bool fastEnabled, bool slowEnabled) {
  return static_cast<uint8_t>((fastEnabled ? SHOT_LOG_FAST_GUARD_BIT : 0) |
                              (slowEnabled ? SHOT_LOG_SLOW_GUARD_BIT : 0));
}

inline uint8_t shotLogPackExtendedFlags(bool fastExtended, bool slowExtended) {
  return static_cast<uint8_t>((fastExtended ? SHOT_LOG_FAST_EXTENDED_BIT : 0) |
                              (slowExtended ? SHOT_LOG_SLOW_EXTENDED_BIT : 0));
}

inline bool shotLogFastGuardEnabled(uint8_t flags) {
  return (flags & SHOT_LOG_FAST_GUARD_BIT) != 0;
}

inline bool shotLogSlowGuardEnabled(uint8_t flags) {
  return (flags & SHOT_LOG_SLOW_GUARD_BIT) != 0;
}

inline bool shotLogFastExtended(uint8_t flags) {
  return (flags & SHOT_LOG_FAST_EXTENDED_BIT) != 0;
}

inline bool shotLogSlowExtended(uint8_t flags) {
  return (flags & SHOT_LOG_SLOW_EXTENDED_BIT) != 0;
}

inline uint8_t shotLogRating(uint8_t flags) {
  return static_cast<uint8_t>((flags & SHOT_LOG_RATING_MASK) >>
                              SHOT_LOG_RATING_SHIFT);
}

inline uint8_t shotLogPackRating(uint8_t flags, uint8_t rating) {
  if (rating > SHOT_LOG_RATING_MAX) {
    rating = SHOT_LOG_RATING_MAX;
  }
  return static_cast<uint8_t>((flags & static_cast<uint8_t>(~SHOT_LOG_RATING_MASK)) |
                              (rating << SHOT_LOG_RATING_SHIFT));
}

inline const char *shotLogStopDetailName(ShotLogStopDetail detail) {
  switch (detail) {
    case ShotLogStopDetail::NORMAL_TARGET: return "normal_target";
    case ShotLogStopDetail::EXTENDED_MAX_WEIGHT: return "extended_max_weight";
    case ShotLogStopDetail::EXTENDED_MIN_TIME: return "extended_min_time";
    case ShotLogStopDetail::AUTO_TO_MANUAL: return "auto_to_manual";
    case ShotLogStopDetail::SLOW_MAX_TIME: return "slow_max_time";
    case ShotLogStopDetail::SLOW_MIN_WEIGHT: return "slow_min_weight";
    case ShotLogStopDetail::CUP_REMOVED: return "cup_removed";
    case ShotLogStopDetail::ACTIVATOR: return "activator";
    case ShotLogStopDetail::WEB_STOP: return "web_stop";
    case ShotLogStopDetail::PHYSICAL_OVERRIDE: return "physical_override";
    case ShotLogStopDetail::WEB_HEARTBEAT: return "web_heartbeat";
    case ShotLogStopDetail::HARD_LIMIT: return "hard_limit";
    case ShotLogStopDetail::WALL_LIMIT: return "wall_limit";
    case ShotLogStopDetail::RELAY_SAFETY: return "relay_safety";
    case ShotLogStopDetail::WEIGHT_ANOMALY: return "weight_anomaly";
    case ShotLogStopDetail::TOUCH_WEIGHT_FALLBACK: return "touch_weight_fallback";
    case ShotLogStopDetail::OTHER: return "other";
  }
  return "unknown";
}

// One preset's shot statistics for the Web UI (plan §4.10).
struct PresetShotStats {
  corridor::ArrivalStats stats;
  int32_t arrivalsMs[corridor::STATS_MAX_SHOTS] = {};
  // The preset's own first drops; without any, medianMs is the other presets'.
  corridor::FirstDropStats firstDrop;
  corridor::GrindHintV2 grindHint;  // NONE while the preset's hints are off
};

// JSON values (quoted, or null) of the packed corridor verdict byte.
inline const char *shotLogVerdictJson(uint8_t packed) {
  switch (packed & 3) {
    case 0: return "\"on_recipe\"";
    case 1: return "\"fast\"";
    case 2: return "\"slow\"";
  }
  return "null";
}

inline const char *shotLogScaleLossSourceJson(uint8_t packed) {
  switch ((packed >> 3) & 3) {
    case 0: return "\"projection\"";
    case 1: return "\"history\"";
    case 2: return "\"window\"";
  }
  return "\"fixed\"";
}

inline const char *actualWeightSourceName(ActualWeightSource source) {
  switch (source) {
    case ActualWeightSource::NONE: return "none";
    case ActualWeightSource::POST_DRIP: return "post_drip";
    case ActualWeightSource::LAST_KNOWN: return "last_known";
  }
  return "unknown";
}

inline ShotLogStopDetail shotLogStopDetailFromEndReason(
    EndReason reason, bool extractionGuardEnabled, bool extractionExtended) {
  (void)extractionGuardEnabled;
  (void)extractionExtended;
  switch (reason) {
    case EndReason::FAST_EXTRACTION_MAX_WEIGHT:
      return ShotLogStopDetail::EXTENDED_MAX_WEIGHT;
    case EndReason::FAST_EXTRACTION_MIN_TIME:
      return ShotLogStopDetail::EXTENDED_MIN_TIME;
    case EndReason::SLOW_EXTRACTION_MAX_TIME:
      return ShotLogStopDetail::SLOW_MAX_TIME;
    case EndReason::SLOW_EXTRACTION_MIN_WEIGHT:
      return ShotLogStopDetail::SLOW_MIN_WEIGHT;
    case EndReason::AUTO_TO_MANUAL_GUARD:
      return ShotLogStopDetail::AUTO_TO_MANUAL;
    case EndReason::CUP_REMOVED:
      return ShotLogStopDetail::CUP_REMOVED;
    case EndReason::SCALE_THRESHOLD:
      return ShotLogStopDetail::NORMAL_TARGET;
    case EndReason::WEIGHT_ANOMALY:
      return ShotLogStopDetail::WEIGHT_ANOMALY;
    case EndReason::TOUCH_WEIGHT_FALLBACK:
      return ShotLogStopDetail::TOUCH_WEIGHT_FALLBACK;
    case EndReason::ACTIVATOR:
      return ShotLogStopDetail::ACTIVATOR;
    case EndReason::WEB_STOP:
      return ShotLogStopDetail::WEB_STOP;
    case EndReason::PHYSICAL_OVERRIDE:
      return ShotLogStopDetail::PHYSICAL_OVERRIDE;
    case EndReason::WEB_HEARTBEAT_TIMEOUT:
      return ShotLogStopDetail::WEB_HEARTBEAT;
    case EndReason::GLOBAL_LIMIT:
      return ShotLogStopDetail::HARD_LIMIT;
    case EndReason::CONFIGURED_WALL_LIMIT:
      return ShotLogStopDetail::WALL_LIMIT;
    case EndReason::RELAY_SAFETY_FAILURE:
      return ShotLogStopDetail::RELAY_SAFETY;
    case EndReason::NONE:
    case EndReason::SHORT_SHOT:
    case EndReason::RINSE_COMPLETE:
    case EndReason::UNCONFIRMED_START:
      return ShotLogStopDetail::OTHER;
  }
  return ShotLogStopDetail::OTHER;
}

inline const char *shotLogTypeName(ShotLogType type) {
  switch (type) {
    case ShotLogType::AUTO: return "auto";
    case ShotLogType::TIMER_ONLY: return "timer_only";
    case ShotLogType::MANUAL: return "manual";
  }
  return "unknown";
}

inline const char *shotLogCutName(ShotLogCut cut) {
  switch (cut) {
    case ShotLogCut::AUTO: return "auto";
    case ShotLogCut::MANUAL: return "manual";
    case ShotLogCut::LIMIT: return "limit";
  }
  return "unknown";
}

inline ShotLogCut shotLogCutFromEndReason(EndReason reason) {
  switch (reason) {
    case EndReason::SCALE_THRESHOLD:
    case EndReason::WEIGHT_ANOMALY:
    case EndReason::TOUCH_WEIGHT_FALLBACK:
    case EndReason::FAST_EXTRACTION_MAX_WEIGHT:
    case EndReason::FAST_EXTRACTION_MIN_TIME:
    case EndReason::SLOW_EXTRACTION_MAX_TIME:
    case EndReason::SLOW_EXTRACTION_MIN_WEIGHT:
    case EndReason::CUP_REMOVED:
      return ShotLogCut::AUTO;
    case EndReason::ACTIVATOR:
    case EndReason::WEB_STOP:
    case EndReason::WEB_HEARTBEAT_TIMEOUT:
    case EndReason::PHYSICAL_OVERRIDE:
      return ShotLogCut::MANUAL;
    default:
      return ShotLogCut::LIMIT;
  }
}

inline bool shotLogEligible(EndReason reason, uint32_t durationMs) {
  if (reason == EndReason::SHORT_SHOT ||
      reason == EndReason::RINSE_COMPLETE ||
      brewEndIsAbandonedStart(reason)) {
    return false;
  }
  return durationMs > MIN_SHOT_LOG_DURATION_MS;
}

inline bool shotLogWeightEligible(float weightG, bool valid) {
  return valid && std::isfinite(weightG) && weightG > MIN_SHOT_LOG_WEIGHT_G;
}

inline bool shotLogBbwEligible(bool startedWithScale, bool timerOnly,
                               bool automaticBrew) {
  return startedWithScale && !timerOnly && automaticBrew;
}

inline ShotLogType shotLogTypeFromCycle(StopperState finalState,
                                        bool startedWithScale, bool timerOnly,
                                        bool automaticBrew) {
  if (finalState == StopperState::MANUAL_NO_SCALE || !startedWithScale) {
    return ShotLogType::MANUAL;
  }
  if (timerOnly) {
    return ShotLogType::TIMER_ONLY;
  }
  if (automaticBrew) {
    return ShotLogType::AUTO;
  }
  return ShotLogType::MANUAL;
}

struct ShotLogRecord {
  uint32_t id;
  uint32_t bootId;
  uint32_t endedAtMs;
  uint32_t endedAtUnixSec;
  uint32_t endedAtLocalSec;
  int16_t timezoneOffsetMinutesAtCommit;
  uint16_t durationDs;
  uint16_t goalWeightDg;
  uint8_t hasWallTime;
  uint8_t verdict;  // corridor::packVerdict: verdict, channeling, scale loss.
  int16_t actualWeightCg;
  int16_t errorCg;
  int16_t offsetUsedCg;
  uint16_t firstDropDs;
  uint16_t avgFlowCgS;
  uint8_t shotType;  // Bits 0–1: type; 2–7: preset ID low six bits.
  uint8_t cutType;   // Bits 0–1: cut; 2–3: preset ID high bits; 4–7: alpha high bits.
  // Bits 0–1: fast/slow side on; 2–4: rating; 5–7: BBW profile.
  uint8_t sideFlags;
  // Bits 0–1: cut line above/below the target at the cut; 2–4: alpha low
  // bits; 5–6: learning status.
  uint8_t extractionExtended;
  uint8_t stopDetail;
  uint8_t actualWeightSource;
  int16_t cutLineCg;     // Cut line f at the stop command; MISSING if none.
  uint16_t fastBeforeDs;  // Shot time window start t1.
  uint16_t arrivalDs;     // When the shot reached (or would reach) the target.
  // Deciseconds from the shot anchor to the late tare; MISSING = no tare.
  uint16_t tareAtDs;
  char presetName[SHOT_PRESET_NAME_CAPACITY];
  // Friendly name if set, else the BLE name, truncated; by value, empty = none.
  char scaleName[SCALE_FRIENDLY_NAME_MAX_LEN + 1];
  // The rest of the cut parameters in force for this shot: every record is a
  // self-contained snapshot, read without the (editable) preset.
  uint16_t slowAfterDs;   // Shot time window end t2.
  uint16_t delayDs;       // Weight-cut delay.
  uint16_t wallDs;        // Max shot time.
  uint8_t fastMarginDg;   // Relative to the target; meaningful with the side on.
  uint8_t slowMarginDg;
  uint8_t cutRampDs;      // Ramp length D (0 = step).
  // Drip delay + 1 ds; 0 = unknown.
  uint8_t dripDelayCode;
  // Machine integration's live coffee target at the shot's end; 0 = the
  // integration reported none.
  uint16_t targetTempDeciC;
  uint16_t doseDg;      // Active preset's dose; 0 = unknown.
  uint16_t ratioX100;   // Recorded yield / dose, hundredths; 0 = not computable.
};

// Appended fields read as zero in records widened from an earlier image, so
// zero must mean "missing" for them, or the widening must set their default.
static_assert(sizeof(ShotLogRecord) == 124, "Shot-log schema 2 record size");
static_assert(offsetof(ShotLogRecord, verdict) == 27, "schema 1 verdict byte");
static_assert(offsetof(ShotLogRecord, slowAfterDs) == 108, "schema 1 snapshot tail");
static_assert(offsetof(ShotLogRecord, dripDelayCode) == 117, "schema 1 drip delay byte");
static_assert(offsetof(ShotLogRecord, targetTempDeciC) == 118,
              "schema 2 snapshot tail");
static_assert(offsetof(ShotLogRecord, ratioX100) == 122, "schema 2 ratio");

inline ShotLogType shotLogType(const ShotLogRecord &record);

inline bool shotLogRecordEligible(const ShotLogRecord &record) {
  return record.durationDs > MIN_SHOT_LOG_DURATION_MS / 100U &&
         !shotLogWeightIsMissing(record.actualWeightCg) &&
         record.actualWeightCg > 200;
}

inline bool shotLogBbwErrorEligible(const ShotLogRecord &record) {
  const ShotLogStopDetail detail =
      static_cast<ShotLogStopDetail>(record.stopDetail);
  // Every weight cut counts: target, raised or lowered segment.
  return shotLogType(record) == ShotLogType::AUTO && record.goalWeightDg > 0 &&
         (detail == ShotLogStopDetail::NORMAL_TARGET ||
          detail == ShotLogStopDetail::EXTENDED_MAX_WEIGHT ||
          detail == ShotLogStopDetail::SLOW_MIN_WEIGHT);
}

// Error in tenths of a percent against the planned stop (the cut line).
inline uint32_t shotLogBbwErrorTenths(const ShotLogRecord &record) {
  const int32_t plannedCg = record.cutLineCg > 0
      ? record.cutLineCg : static_cast<int32_t>(record.goalWeightDg) * 10;
  const int32_t error = static_cast<int32_t>(record.actualWeightCg) - plannedCg;
  return static_cast<uint32_t>(error < 0 ? -error : error) * 1000U /
         static_cast<uint32_t>(plannedCg > 0 ? plannedCg : 1);
}

inline uint8_t shotLogPresetId(const ShotLogRecord &record);

// A stored shot as the corridor statistics see it: weight cuts carry their
// segment's EndReason; time limits and manual stops of automatic shots carry
// theirs for the grind hint's censored shots; everything else is NONE.
inline corridor::ArrivalRecord shotLogArrivalRecord(const ShotLogRecord &record) {
  corridor::ArrivalRecord out;
  out.recordId = record.id;
  out.presetId = shotLogPresetId(record);
  const auto detail = static_cast<ShotLogStopDetail>(record.stopDetail);
  const bool automatic = shotLogType(record) == ShotLogType::AUTO;
  out.endReason = static_cast<uint8_t>(
      detail == ShotLogStopDetail::NORMAL_TARGET ? EndReason::SCALE_THRESHOLD
      : detail == ShotLogStopDetail::EXTENDED_MAX_WEIGHT
          ? EndReason::FAST_EXTRACTION_MAX_WEIGHT
      : detail == ShotLogStopDetail::SLOW_MIN_WEIGHT
          ? EndReason::SLOW_EXTRACTION_MIN_WEIGHT
      : !automatic ? EndReason::NONE
      : detail == ShotLogStopDetail::WALL_LIMIT ? EndReason::CONFIGURED_WALL_LIMIT
      : detail == ShotLogStopDetail::HARD_LIMIT ? EndReason::GLOBAL_LIMIT
      : detail == ShotLogStopDetail::SLOW_MAX_TIME ? EndReason::SLOW_EXTRACTION_MAX_TIME
      : detail == ShotLogStopDetail::ACTIVATOR ? EndReason::ACTIVATOR
      : detail == ShotLogStopDetail::WEB_STOP ? EndReason::WEB_STOP
      : detail == ShotLogStopDetail::PHYSICAL_OVERRIDE ? EndReason::PHYSICAL_OVERRIDE
          : EndReason::NONE);
  out.durationMs = static_cast<int32_t>(record.durationDs) * 100;
  out.finalWeightDg = shotLogWeightIsMissing(record.actualWeightCg)
      ? 0 : record.actualWeightCg / 10;
  out.verdict = record.verdict;
  out.arrivalMs = record.arrivalDs == SHOT_LOG_METRIC_MISSING
      ? 0 : static_cast<int32_t>(record.arrivalDs) * 100;
  out.firstDropMs = record.firstDropDs == SHOT_LOG_METRIC_MISSING
      ? 0 : static_cast<int32_t>(record.firstDropDs) * 100;
  out.fastBeforeMs = static_cast<int32_t>(record.fastBeforeDs) * 100;
  out.slowAfterMs = static_cast<int32_t>(record.slowAfterDs) * 100;
  out.goalDg = record.goalWeightDg;
  out.avgFlowCgS = record.avgFlowCgS == SHOT_LOG_METRIC_MISSING
                       ? 0 : record.avgFlowCgS;
  return out;
}

inline PersistedLastShot shotLogProjectLastShot(
    const ShotLogRecord &record, const PersistedLastShot &linked) {
  PersistedLastShot shot = linked.shotLogId == record.id
                               ? linked : PersistedLastShot{};
  shot.valid = true;
  shot.shotLogId = record.id;
  shot.endedAtUptimeMs = record.endedAtMs;
  shot.endedAtUnixSec = record.endedAtUnixSec;
  shot.endedAtLocalSec = record.endedAtLocalSec;
  shot.hasWallTime = record.hasWallTime;
  shot.durationMs = static_cast<uint32_t>(record.durationDs) * 100U;
  shot.currentWeightG = record.actualWeightCg / 100.0f;
  shot.weightValid = true;
  // Only shots with a measured yield are logged, so every record had a scale.
  shot.scaleAvailable = true;
  shot.goalWeightDg = record.goalWeightDg;
  shot.presetId = shotLogPresetId(record);
  copyCString(shot.presetName, sizeof(shot.presetName), record.presetName);
  shot.shotType = static_cast<uint8_t>(shotLogType(record));
  shot.firstDropElapsedMs = record.firstDropDs == SHOT_LOG_METRIC_MISSING
      ? 0U : static_cast<uint32_t>(record.firstDropDs) * 100U;
  shot.tareElapsedMs = record.tareAtDs == SHOT_LOG_METRIC_MISSING
      ? 0U : static_cast<uint32_t>(record.tareAtDs) * 100U;
  copyCString(shot.scaleName, sizeof(shot.scaleName), record.scaleName);
  shot.averageFlowValid = record.avgFlowCgS != SHOT_LOG_METRIC_MISSING;
  shot.averageFlowGps = shot.averageFlowValid
      ? record.avgFlowCgS / 100.0f : 0.0f;
  shot.fastExtractionGuardEnabled =
      shotLogFastGuardEnabled(record.sideFlags);
  shot.slowExtractionGuardEnabled =
      shotLogSlowGuardEnabled(record.sideFlags);
  shot.extractionExtended = shotLogFastExtended(record.extractionExtended);
  shot.slowExtractionExtended = shotLogSlowExtended(record.extractionExtended);
  shot.rating = shotLogRating(record.sideFlags);
  shot.verdict = record.verdict & 3;
  shot.arrivalMs = record.arrivalDs == SHOT_LOG_METRIC_MISSING
      ? 0U : static_cast<uint32_t>(record.arrivalDs) * 100U;
  if (!shotLogWeightIsMissing(record.cutLineCg))
    shot.activeStopWeightG = record.cutLineCg / 100.0f;
  return shot;
}

inline ShotLogType shotLogType(const ShotLogRecord &record) {
  return static_cast<ShotLogType>(record.shotType & 3);
}
inline ShotLogCut shotLogCut(const ShotLogRecord &record) {
  return static_cast<ShotLogCut>(record.cutType & 3);
}
inline uint8_t shotLogPresetId(const ShotLogRecord &record) {
  return (record.shotType >> 2) | ((record.cutType & 12) << 4);
}
inline void shotLogSetPresetId(ShotLogRecord &record, uint8_t id) {
  record.shotType = (record.shotType & 3) | ((id & 63) << 2);
  record.cutType = (record.cutType & 0xf3) | ((id >> 6) << 2);
}

// The list's result: the verdict of a timed shot (0 on recipe, 1 fast,
// 2 slow); 3 = none, as for every shot without a time to target.
inline uint8_t shotLogResult(const ShotLogRecord &record) {
  return record.arrivalDs == 0 || record.arrivalDs == SHOT_LOG_METRIC_MISSING
             ? 3 : record.verdict & 3;
}

// A scale named by value, keyed for the list filter.
inline uint32_t shotLogScaleKey(const ShotLogRecord &record) {
  return crc32Update(0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(record.scaleName),
                     strnlen(record.scaleName, sizeof(record.scaleName)));
}

// Shots-list filter: an empty group does not restrict, groups combine with
// AND and the options of a group with OR. Results are shotLogResult bits.
// hideLowYield leaves out stored yields under 1 g, as the list always did;
// the CSV export reads every stored row.
struct ShotListFilter {
  uint8_t results = 0, presetCount = 0, scaleCount = 0;
  bool hideLowYield = false;
  uint8_t presets[SHOT_LIST_FILTER_MAX] = {};
  uint32_t scales[SHOT_LIST_FILTER_MAX] = {};
};

inline bool shotListFilterMatches(const ShotListFilter &filter,
                                  const ShotLogRecord &record) {
  const uint8_t result = shotLogResult(record);
  if (filter.results && (result > 2 || !((filter.results >> result) & 1U)))
    return false;
  if (filter.hideLowYield && !shotLogWeightIsMissing(record.actualWeightCg) &&
      record.actualWeightCg < 100)
    return false;
  bool preset = filter.presetCount == 0, scale = filter.scaleCount == 0;
  for (size_t i = 0; i < filter.presetCount; ++i)
    preset = preset || filter.presets[i] == shotLogPresetId(record);
  const uint32_t key = scale ? 0 : shotLogScaleKey(record);
  for (size_t i = 0; i < filter.scaleCount; ++i)
    scale = scale || filter.scales[i] == key;
  return preset && scale;
}

// Identity of a filter for the page cache and the stream fingerprint.
inline uint32_t shotListFilterHash(const ShotListFilter &filter) {
  const uint8_t head[4] = {filter.results, filter.presetCount, filter.scaleCount,
                           filter.hideLowYield};
  uint32_t hash = crc32Update(0xFFFFFFFFU, head, sizeof(head));
  hash = crc32Update(hash, filter.presets, filter.presetCount);
  return crc32Update(hash, reinterpret_cast<const uint8_t *>(filter.scales),
                     filter.scaleCount * sizeof(filter.scales[0]));
}

// Profile 0 unknown, 5 smooth cut (corridor) v1; 1–4 are retired and never
// written. Alpha 0 unknown, 1–100 hundredths.
inline uint8_t shotLogBbwProfile(const ShotLogRecord &record) {
  return record.sideFlags >> 5;
}
inline const char *shotLogBbwAlgorithm(const ShotLogRecord &record) {
  return shotLogBbwProfile(record) == 5 ? "corridor" : "unknown";
}
inline const char *shotLogBbwVersion(const ShotLogRecord &record) {
  return shotLogBbwProfile(record) == 5 ? "1" : "null";
}
inline uint8_t shotLogBbwAlpha(const ShotLogRecord &record) {
  const uint8_t alpha = ((record.extractionExtended >> 2) & 7) | ((record.cutType >> 4) << 3);
  return validBbwAlpha(alpha) ? alpha : 0;
}
inline const char *shotLogBbwLearningApplied(const ShotLogRecord &record) {
  const uint8_t code = (record.extractionExtended >> 5) & 3;
  return code == 1 ? "false" : code == 2 ? "true" : "null";
}
inline void shotLogSetBbw(ShotLogRecord &record, uint8_t algorithm,
                          uint8_t version, uint8_t alpha, bool applied) {
  if (!validBbwAlpha(alpha)) alpha = 0;
  const uint8_t profile = version == BBW_PROFILE_VERSION && algorithm == 1 ? 5 : 0;
  record.cutType = (record.cutType & 15) | ((alpha >> 3) << 4);
  record.sideFlags = (record.sideFlags & 0x1f) | (profile << 5);
  record.extractionExtended = (record.extractionExtended & 3) |
      ((alpha & 7) << 2) | ((applied ? 2 : 1) << 5);
}

// Input must be newest-first (copyNewestFirst). Date+desc is a no-op.
inline void shotLogReverseRecords(ShotLogRecord *records, size_t count) {
  if (records == nullptr || count < 2U) {
    return;
  }
  size_t i = 0;
  size_t j = count - 1U;
  while (i < j) {
    const ShotLogRecord tmp = records[i];
    records[i] = records[j];
    records[j] = tmp;
    ++i;
    --j;
  }
}

inline int shotLogCompareRatingOrder(const ShotLogRecord &left,
                                     const ShotLogRecord &right,
                                     ShotLogSortDir dir) {
  const uint8_t leftRating = shotLogRating(left.sideFlags);
  const uint8_t rightRating = shotLogRating(right.sideFlags);
  const bool leftUnrated = leftRating == 0;
  const bool rightUnrated = rightRating == 0;
  if (leftUnrated != rightUnrated) {
    return leftUnrated ? 1 : -1;
  }
  if (leftRating != rightRating) {
    if (dir == ShotLogSortDir::Desc) {
      return leftRating > rightRating ? -1 : 1;
    }
    return leftRating < rightRating ? -1 : 1;
  }
  if (left.id != right.id) {
    return left.id > right.id ? -1 : 1;
  }
  return 0;
}

inline void shotLogSortRecords(ShotLogRecord *records, size_t count,
                               ShotLogSort sort, ShotLogSortDir dir) {
  if (records == nullptr || count < 2U) {
    return;
  }
  if (sort != ShotLogSort::Rating) {
    if (dir == ShotLogSortDir::Asc) {
      shotLogReverseRecords(records, count);
    }
    return;
  }
  for (size_t i = 1; i < count; ++i) {
    const ShotLogRecord key = records[i];
    size_t j = i;
    while (j > 0 &&
           shotLogCompareRatingOrder(records[j - 1U], key, dir) > 0) {
      records[j] = records[j - 1U];
      --j;
    }
    records[j] = key;
  }
}

struct ShotLogHeader {
  uint32_t magic;
  uint16_t schemaVersion;
  uint16_t recordSize;
  uint32_t generation;
  uint32_t nextRecordId;
  uint16_t count;
  uint16_t writeIndex;
  uint32_t checksum;
};
static_assert(sizeof(ShotLogHeader) == 24, "shot-log header layout changed");

// Read-time view; stats are derived from shot records and never persisted.
struct ShotStatsView {
  uint32_t shotCount = 0;
  uint32_t flowCount = 0;
  uint32_t bbwCount = 0;
  uint32_t timedCount = 0;
  uint32_t durationDsSum = 0;
  uint32_t actualCgSum = 0;
  uint32_t errorPctTenthsSum = 0;
  uint32_t flowCgSx100Sum = 0;
  uint32_t daysSpan = 0;
  uint16_t durationsDs[SHOT_LOG_STATS_WINDOW] = {};
};

// The aggregate over the newest `window` eligible records of `count`, where
// at(n) is the n-th newest. The BBW error averages the newest `window` weight
// cuts, which may reach past that window. Only the first ten durations are
// kept: they feed the integration's ten-shot chart.
template <typename At>
inline ShotStatsView shotLogStatsView(size_t count, size_t window, At at) {
  ShotStatsView stats = {};
  int32_t newestDay = -1;
  int32_t oldestDay = -1;
  for (size_t n = 0;
       n < count && (stats.shotCount < window || stats.bbwCount < window); ++n) {
    const ShotLogRecord &record = at(n);
    if (!shotLogRecordEligible(record)) continue;
    if (stats.bbwCount < window && shotLogBbwErrorEligible(record)) {
      ++stats.bbwCount;
      stats.errorPctTenthsSum += shotLogBbwErrorTenths(record);
    }
    if (stats.shotCount >= window) continue;
    if (stats.shotCount < SHOT_LOG_STATS_WINDOW)
      stats.durationsDs[stats.shotCount] = record.durationDs;
    ++stats.shotCount;
    stats.durationDsSum += record.durationDs;
    stats.actualCgSum += static_cast<uint32_t>(record.actualWeightCg);
    if (record.avgFlowCgS != SHOT_LOG_METRIC_MISSING) {
      ++stats.flowCount;
      stats.flowCgSx100Sum += static_cast<uint32_t>(record.avgFlowCgS) * 100U;
    }
    if (record.hasWallTime && record.endedAtLocalSec != 0) {
      ++stats.timedCount;
      const int32_t day = static_cast<int32_t>(record.endedAtLocalSec / 86400U);
      if (newestDay < day) newestDay = day;
      if (oldestDay < 0 || oldestDay > day) oldestDay = day;
    }
  }
  if (newestDay >= 0 && oldestDay >= 0)
    stats.daysSpan = static_cast<uint32_t>(newestDay - oldestDay);
  return stats;
}

struct ShotLogStore {
  ShotLogHeader header;
  ShotLogRecord records[SHOT_LOG_CAPACITY];
};

inline uint32_t shotLogLocalSecFromUtc(uint32_t utcSec, int16_t offsetMinutes) {
  return utcSec + static_cast<int32_t>(offsetMinutes) * 60;
}

inline uint32_t shotLogChecksum(const ShotLogStore &store) {
  uint32_t crc = crc32Update(
      0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&store.header),
      offsetof(ShotLogHeader, checksum));
  if (store.header.count > 0) {
    crc = crc32Update(crc, reinterpret_cast<const uint8_t *>(store.records),
                      static_cast<size_t>(store.header.count) *
                          store.header.recordSize);
  }
  return ~crc;
}

inline void finalizeShotLogStore(ShotLogStore &store) {
  store.header.magic = SHOT_LOG_MAGIC;
  store.header.schemaVersion = SHOT_LOG_SCHEMA_VERSION;
  store.header.recordSize = sizeof(ShotLogRecord);
  store.header.checksum = 0;
  store.header.checksum = shotLogChecksum(store);
}

inline bool validShotLogStore(const ShotLogStore &store) {
  if (store.header.magic != SHOT_LOG_MAGIC ||
      store.header.schemaVersion != SHOT_LOG_SCHEMA_VERSION ||
      store.header.recordSize < SHOT_LOG_MIN_RECORD_BYTES ||
      store.header.recordSize > sizeof(ShotLogRecord) ||
      store.header.count > SHOT_LOG_CAPACITY ||
      store.header.writeIndex >= SHOT_LOG_CAPACITY ||
      store.header.checksum != shotLogChecksum(store)) {
    return false;
  }
  return true;
}

// Re-strides the records of a valid store written with shorter records to
// the current size; appended fields read as zero.
inline void widenShotLogRecords(ShotLogStore &store) {
  const size_t stored = store.header.recordSize;
  if (stored >= sizeof(ShotLogRecord)) return;
  const uint8_t *packed = reinterpret_cast<const uint8_t *>(store.records);
  for (size_t i = store.header.count; i-- > 0;) {
    auto *record = reinterpret_cast<uint8_t *>(&store.records[i]);
    memmove(record, packed + i * stored, stored);
    memset(record + stored, 0, sizeof(ShotLogRecord) - stored);
  }
  store.header.recordSize = sizeof(ShotLogRecord);
}

inline void resetShotLogStore(ShotLogStore &store) {
  memset(&store, 0, sizeof(store));
  store.header.generation = 1;
  store.header.nextRecordId = 1;
  finalizeShotLogStore(store);
}

// Rotate a record ring in place so the `count` live records (oldest first,
// ending just before `writeIndex`) occupy records[0..count) and the tail is
// zero-filled. Triple-reversal rotation: no workspace, safe for any overlap.
// Shared by the shot-log, shot-curve, and activation-history stores, whose
// live copies sit in PSRAM. Callers must guarantee count <= capacity and
// writeIndex < capacity (both are part of each store's validity check).
// Returns the compacted writeIndex (count % capacity).
inline uint16_t compactRecordRing(void *records, size_t capacity,
                                  size_t recordBytes, uint16_t count,
                                  uint16_t writeIndex) {
  uint8_t *base = static_cast<uint8_t *>(records);
  if (count == 0) {
    memset(base, 0, capacity * recordBytes);
    return 0;
  }
  const size_t shift = (static_cast<size_t>(writeIndex) + capacity - count) %
                       capacity;
  if (shift != 0) {
    auto reverse = [base, recordBytes](size_t first, size_t last) {
      while (first + 1U < last) {
        --last;
        uint8_t *a = base + first * recordBytes;
        uint8_t *b = base + last * recordBytes;
        for (size_t i = 0; i < recordBytes; ++i) {
          const uint8_t byte = a[i];
          a[i] = b[i];
          b[i] = byte;
        }
        ++first;
      }
    };
    reverse(0, shift);
    reverse(shift, capacity);
    reverse(0, capacity);
  }
  memset(base + count * recordBytes, 0, (capacity - count) * recordBytes);
  return static_cast<uint16_t>(count % capacity);
}

// Pack the ring into records[0..count) (oldest first) in place so the flash
// slot always holds the used prefix and deletion is a memmove.
inline void compactShotLogStore(ShotLogStore &store) {
  store.header.writeIndex = compactRecordRing(
      store.records, SHOT_LOG_CAPACITY, sizeof(ShotLogRecord),
      store.header.count, store.header.writeIndex);
}

inline int16_t shotLogWeightToCentigrams(float weightG) {
  if (!std::isfinite(weightG)) {
    return SHOT_LOG_WEIGHT_MISSING;
  }
  const int32_t centigrams = weightToCentigrams(weightG);
  // INT16_MIN is reserved as the missing sentinel.
  if (centigrams == INT32_MAX || centigrams <= INT16_MIN ||
      centigrams > INT16_MAX) {
    return SHOT_LOG_WEIGHT_MISSING;
  }
  return static_cast<int16_t>(centigrams);
}

}  // namespace openbrewbyweight

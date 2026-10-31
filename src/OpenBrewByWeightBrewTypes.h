#pragma once

#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OpenBrewByWeightMachineTypes.h"
#include "OpenBrewByWeightScaleTypes.h"

namespace openbrewbyweight {

// =============================================================================
// LAYER: Brew types (stopper workflow + guard contracts)
// =============================================================================
// WHAT: StopperState, EndReason, GuardInputs, and brew timing/weight constants.
//       The stopper orchestrates brew vs idle and *requests* circuit start/stop;
//       it does not detect rinse gestures, drive GPIO, or know paddle vs switch.
//       Machine publishes REQUEST_RINSE; OpenBrewByWeightRinse owns duration. The
//       activator translates GPIO into UserIntent before brew sees it.
//
// BOUNDARY: GuardInputs is a neutral snapshot the orchestrator fills once per
// loop. Brew/guards must not poll machine specializations, scale link, or cup
// presence themselves, and must not import paddle/momentary headers.

constexpr uint32_t DEFAULT_LAST_SHOT_COOLDOWN_MS = 60UL * 60UL * 1000UL;
constexpr uint32_t MIN_LAST_SHOT_COOLDOWN_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t MAX_LAST_SHOT_COOLDOWN_MS = 240UL * 60UL * 1000UL;
constexpr uint32_t DEFAULT_RETARE_WINDOW_MS = 4000;
constexpr uint32_t DEFAULT_BBW_PROTECTION_MS = 12000;
constexpr uint32_t MIN_BBW_PROTECTION_AFTER_RETARE_MS = 3000;
constexpr float DEFAULT_MINIMUM_CUP_WEIGHT_G = 10.0f;
constexpr float MIN_MINIMUM_CUP_WEIGHT_G = 1.0f;
constexpr float MAX_MINIMUM_CUP_WEIGHT_G = 500.0f;
constexpr uint32_t MIN_RETARE_WINDOW_MS = 500;
constexpr uint32_t MAX_RETARE_WINDOW_MS = 10000;
constexpr uint32_t MIN_BBW_PROTECTION_MS = 500;
constexpr uint32_t MAX_BBW_PROTECTION_MS = 30000;
constexpr float DEFAULT_RETARE_STABILITY_TOLERANCE_G = 2.0f;
constexpr uint8_t DEFAULT_RETARE_STABILITY_SAMPLES = 3;
constexpr uint32_t DEFAULT_RETARE_STABILITY_MAX_GAP_MS = 500;
constexpr uint32_t DEFAULT_RETARE_STABILITY_MIN_DURATION_MS = 300;
constexpr uint8_t MIN_RETARE_STABILITY_SAMPLES = 2;
constexpr uint8_t MAX_RETARE_STABILITY_SAMPLES = 10;
constexpr float MIN_RETARE_STABILITY_TOLERANCE_G = 0.1f;
constexpr float MAX_RETARE_STABILITY_TOLERANCE_G = 20.0f;
constexpr uint32_t MIN_RETARE_STABILITY_MAX_GAP_MS = 100;
constexpr uint32_t MAX_RETARE_STABILITY_MAX_GAP_MS = 5000;
constexpr uint32_t MIN_RETARE_STABILITY_MIN_DURATION_MS = 0;
constexpr uint32_t MAX_RETARE_STABILITY_MIN_DURATION_MS = 2000;
// Target weight lives in decigrams (100–2000 dg = 10.0–200.0 g); the whole
// pipeline — presets, runtime config, shot records, UI — speaks this unit.
constexpr uint16_t MIN_GOAL_WEIGHT_DG = 100;
constexpr uint16_t MAX_GOAL_WEIGHT_DG = 2000;
constexpr uint16_t DEFAULT_GOAL_WEIGHT_DG = 360;
inline float goalWeightDgToGrams(uint16_t goalWeightDg) {
  return static_cast<float>(goalWeightDg) * 0.1f;
}
// Smooth-cut recipe: a shot time window [t1, t2] and side margins relative to
// the target (decigrams). Factory Double · Adaptive values are the defaults.
constexpr uint32_t DEFAULT_FAST_BEFORE_MS = 27000;
constexpr uint32_t DEFAULT_SLOW_AFTER_MS = 36000;
constexpr uint16_t DEFAULT_FAST_MARGIN_DG = 60;
constexpr uint16_t DEFAULT_SLOW_MARGIN_DG = 30;
constexpr uint16_t DEFAULT_DOSE_DG = 180;
constexpr uint16_t MIN_SIDE_MARGIN_DG = 5;
constexpr uint16_t MAX_SIDE_MARGIN_DG = 100;
// The slow side never stops below 5 g; the fast side extends at most 25 %.
constexpr uint16_t MIN_SLOW_FLOOR_DG = 50;
constexpr uint16_t MIN_DOSE_DG = 50;
constexpr uint16_t MAX_DOSE_DG = 300;
constexpr uint32_t WINDOW_STEP_MS = 500;
constexpr uint32_t MIN_WINDOW_AFTER_DELAY_MS = 2000;
constexpr uint32_t MIN_WINDOW_SPAN_MS = 1000;
constexpr uint32_t MIN_WINDOW_BEFORE_WALL_MS = 1000;
// Cut-line transition per side: its length (0 = a step at the window edge)
// and a curve picked by where it is steepest and how hard it is
// (corridor::RAMP_CURVES). The struct default is 5 s Even + Soft
// (smoothstep); the factory Adaptive presets use 6 s Early (Fast) and Late
// (Slow), flat beside the window so near-recipe shots stay on target.
constexpr uint32_t DEFAULT_RAMP_MS = 5000;
constexpr uint32_t FACTORY_RAMP_MS = 6000;
constexpr uint32_t MAX_RAMP_MS = 10000;
constexpr uint32_t RAMP_STEP_MS = 500;
constexpr uint8_t RAMP_CURVE_EVEN = 0;
constexpr uint8_t RAMP_CURVE_EARLY = 1;
constexpr uint8_t RAMP_CURVE_LATE = 2;
constexpr uint8_t RAMP_CURVE_TYPES = 3;
constexpr uint8_t RAMP_HARDNESS_SOFT = 0;
constexpr uint8_t RAMP_HARDNESS_MEDIUM = 1;
constexpr uint8_t RAMP_HARDNESS_HARD = 2;
constexpr uint8_t RAMP_HARDNESS_LEVELS = 3;
// Continuous hardness: hundredths of hardness, 0..200 (soft 0, medium 100,
// hard 200). UNSET marks a value not yet migrated from the legacy enum.
constexpr uint8_t RAMP_HARDNESS_PCT_MAX = 200;
constexpr uint8_t RAMP_HARDNESS_PCT_UNSET = 0xFF;
inline uint8_t rampHardnessPctOf(uint8_t level) {
  return level < RAMP_HARDNESS_LEVELS ? static_cast<uint8_t>(level * 100) : 0;
}
// The legacy level nearest a valid pct (0..49 soft, 50..149 medium, 150..200 hard).
inline uint8_t rampNearestHardness(uint8_t pct) {
  const uint16_t level = (static_cast<uint16_t>(pct) + 50) / 100;
  return static_cast<uint8_t>(level < RAMP_HARDNESS_LEVELS ? level : RAMP_HARDNESS_HARD);
}
// Wire names of the transition curve type and hardness.
inline const char *rampCurveName(uint8_t type) {
  return type == RAMP_CURVE_EARLY ? "early" : type == RAMP_CURVE_LATE ? "late" : "even";
}
inline const char *rampHardnessName(uint8_t hardness) {
  return hardness == RAMP_HARDNESS_MEDIUM ? "medium"
         : hardness == RAMP_HARDNESS_HARD ? "hard"
                                          : "soft";
}
// The hardness the cut runs at: an UNSET pct (a runtime blob written before
// continuous hardness, whose sanitize only covers presets) reads as its
// legacy level's percent, so the corridor never sees the sentinel and the
// cut matches the level the wire still reports.
inline uint8_t rampHardnessPctEffective(uint8_t pct, uint8_t level) {
  return pct > RAMP_HARDNESS_PCT_MAX ? rampHardnessPctOf(level) : pct;
}
// A persisted transition choice: a known curve type, a known legacy level,
// and a pct that is valid or still UNSET (consumers derive UNSET from the
// level, so it is valid alongside a valid level).
inline bool rampChoiceValid(uint8_t type, uint8_t level, uint8_t pct) {
  return type < RAMP_CURVE_TYPES && level < RAMP_HARDNESS_LEVELS &&
         (pct <= RAMP_HARDNESS_PCT_MAX || pct == RAMP_HARDNESS_PCT_UNSET);
}
// The nearest legacy level of a pct/enum pair; an UNSET pct keeps its level.
inline uint8_t rampNearestHardnessOf(uint8_t pct, uint8_t level) {
  return pct > RAMP_HARDNESS_PCT_MAX ? level : rampNearestHardness(pct);
}
// Wire hardness of a pct/enum pair: percent 0..100 (one decimal), an UNSET
// pct falling back to its legacy level's percent.
inline double rampHardnessPercent(uint8_t pct, uint8_t level) {
  return (pct > RAMP_HARDNESS_PCT_MAX ? rampHardnessPctOf(level) : pct) / 2.0;
}
// Extra delay after the scale timer catches up to machine circuit time (whole seconds)
// before sending STOP_TIMER. 0 stops in that same instant.
constexpr uint32_t DEFAULT_SCALE_TIMER_STOP_EXTRA_DELAY_MS = 0;
constexpr uint32_t MIN_SCALE_TIMER_STOP_EXTRA_DELAY_MS = 0;
constexpr uint32_t MAX_SCALE_TIMER_STOP_EXTRA_DELAY_MS = 1000;
constexpr uint32_t DEFAULT_DRIP_DELAY_MS = 3000;
constexpr uint32_t MIN_DRIP_DELAY_MS = 0;
constexpr uint32_t MAX_DRIP_DELAY_MS = 10000;
constexpr uint32_t MAX_SCALE_TIMER_STOP_CATCHUP_MS = 2000;
constexpr float MAX_OFFSET_G = 5.0f;
constexpr float DEFAULT_WEIGHT_OFFSET_G = 0.5f;

enum class StopperState : uint8_t {
  REQUIRES_OFF,
  READY,
  BREW,
  RINSE,
  MANUAL_NO_SCALE,
  BACKFLUSH_CANDIDATE,
  BACKFLUSH_RUNNING
};

inline bool backflushActive(StopperState state) {
  return state == StopperState::BACKFLUSH_CANDIDATE ||
         state == StopperState::BACKFLUSH_RUNNING;
}

enum class ControlSource : uint8_t {
  NONE,
  PHYSICAL,
  WEB
};

// Snapshot the stopper builds once per loop. Brew/guards never poll machine,
// scale link, or cup presence themselves — and never see paddle/momentary
// internals (no PaddleMode, reed, pulse state, MachineType, or rinseGestureMs).
struct GuardInputs {
  bool holdActive = false;
  bool physicalOn = false;
  bool stablyOff = false;
  bool scaleAvailable = false;
  bool scaleUsable = false;
  CupPresenceState cup = CupPresenceState::ABSENT;
  float currentWeightG = 0.0f;
  // Orchestrator-composed hold timeout for blocked starts (no-scale / cup).
  uint32_t blockedHoldTimeoutMs = 0;
};

// Single source for StopperState renderings (API id and human label), indexed
// by StopperState; out-of-range values keep the previous "UNKNOWN" fallbacks.
inline constexpr struct {
  const char *apiName;
  const char *humanLabel;
} kStopperStateLabels[] = {
    {"REQUIRES_OFF", "Paddle release required"},
    {"READY", "Ready"},
    {"BREW", "Automatic brew"},
    {"RINSE", "Rinse in progress"},
    {"MANUAL_NO_SCALE", "Manual shot without scale"},
    {"BACKFLUSH_CANDIDATE", "Checking backflush"},
    {"BACKFLUSH_RUNNING", "Backflush in progress"},
};

constexpr size_t kStopperStateLabelCount =
    sizeof(kStopperStateLabels) / sizeof(kStopperStateLabels[0]);

static_assert(kStopperStateLabelCount == 7,
              "keep kStopperStateLabels in sync with StopperState");

inline const char *stopperStateName(StopperState state) {
  const size_t index = static_cast<size_t>(state);
  return index < kStopperStateLabelCount ? kStopperStateLabels[index].apiName
                                         : "UNKNOWN";
}

enum class EndReason : uint8_t {
  NONE = 0,
  ACTIVATOR = 1,
  // 2 is retired; ordinals are stored in shot and audit logs.
  SCALE_THRESHOLD = 3,
  WEIGHT_ANOMALY = 4,
  GLOBAL_LIMIT = 5,
  CONFIGURED_WALL_LIMIT = 6,
  SHORT_SHOT = 7,
  RINSE_COMPLETE = 8,
  WEB_STOP = 9,
  PHYSICAL_OVERRIDE = 10,
  WEB_HEARTBEAT_TIMEOUT = 11,
  RELAY_SAFETY_FAILURE = 12,
  FAST_EXTRACTION_MAX_WEIGHT = 13,
  FAST_EXTRACTION_MIN_TIME = 14,
  SLOW_EXTRACTION_MAX_TIME = 15,
  SLOW_EXTRACTION_MIN_WEIGHT = 16,
  AUTO_TO_MANUAL_GUARD = 17,
  CUP_REMOVED = 18,
  UNCONFIRMED_START = 19,
  TOUCH_WEIGHT_FALLBACK = 20
};
// Reasons in use (2 retired); a reason appended above must move this
// (asserted after kEndReasonLabels).
constexpr size_t END_REASON_COUNT = static_cast<size_t>(EndReason::TOUCH_WEIGHT_FALLBACK) + 1U;

constexpr uint32_t TOUCH_STOP_FALLBACK_MS = 1000;
struct TouchStopEvidence {
  uint32_t firstAtMs = 0;
  uint32_t lastAtMs = 0;
  uint32_t packetSequence = 0;
  uint32_t connectionGeneration = 0;
  float thresholdG = 0.0f;
  float weightG = 0.0f;
  EndReason policy = EndReason::NONE;
  uint8_t samples = 0;
};

// Single source for EndReason renderings: API UPPER_SNAKE and debug labels in
// one table so new enumerators are added once (row count is asserted below).
struct EndReasonLabel {
  EndReason reason;
  const char *apiName;
  const char *debugName;
};

inline constexpr EndReasonLabel kEndReasonLabels[] = {
    {EndReason::NONE, "NONE", "none"},
    {EndReason::ACTIVATOR, "ACTIVATOR", "activator"},
    {EndReason::SCALE_THRESHOLD, "SCALE_THRESHOLD", "scale threshold"},
    {EndReason::WEIGHT_ANOMALY, "WEIGHT_ANOMALY", "weight anomaly"},
    {EndReason::GLOBAL_LIMIT, "GLOBAL_LIMIT", "global machine circuit limit"},
    {EndReason::CONFIGURED_WALL_LIMIT, "CONFIGURED_WALL_LIMIT",
     "configured wall limit"},
    {EndReason::SHORT_SHOT, "SHORT_SHOT", "short shot"},
    {EndReason::RINSE_COMPLETE, "RINSE_COMPLETE", "rinse complete"},
    {EndReason::WEB_STOP, "WEB_STOP", "web stop"},
    {EndReason::PHYSICAL_OVERRIDE, "PHYSICAL_OVERRIDE", "physical override"},
    {EndReason::WEB_HEARTBEAT_TIMEOUT, "WEB_HEARTBEAT_TIMEOUT",
     "web heartbeat timeout"},
    {EndReason::RELAY_SAFETY_FAILURE, "RELAY_SAFETY_FAILURE",
     "relay safety failure"},
    {EndReason::FAST_EXTRACTION_MAX_WEIGHT, "FAST_EXTRACTION_MAX_WEIGHT",
     "fast extraction max weight"},
    {EndReason::FAST_EXTRACTION_MIN_TIME, "FAST_EXTRACTION_MIN_TIME",
     "fast extraction min time"},
    {EndReason::SLOW_EXTRACTION_MAX_TIME, "SLOW_EXTRACTION_MAX_TIME",
     "slow extraction max time"},
    {EndReason::SLOW_EXTRACTION_MIN_WEIGHT, "SLOW_EXTRACTION_MIN_WEIGHT",
     "slow extraction min weight"},
    {EndReason::AUTO_TO_MANUAL_GUARD, "AUTO_TO_MANUAL_GUARD",
     "auto-to-manual time guard"},
    {EndReason::CUP_REMOVED, "CUP_REMOVED", "cup removed"},
    {EndReason::UNCONFIRMED_START, "UNCONFIRMED_START", "unconfirmed start"},
    {EndReason::TOUCH_WEIGHT_FALLBACK, "TOUCH_WEIGHT_FALLBACK",
     "touch weight fallback"},
};

inline const EndReasonLabel *findEndReasonLabel(EndReason reason) {
  for (const auto &label : kEndReasonLabels) {
    if (label.reason == reason) {
      return &label;
    }
  }
  return nullptr;
}

constexpr size_t kEndReasonLabelCount =
    sizeof(kEndReasonLabels) / sizeof(kEndReasonLabels[0]);

// Compile-time completeness guard: one row per enumerator (20; ordinal 2 was
// retired). Adding an enumerator without a row breaks this assert.
static_assert(kEndReasonLabelCount == 20,
              "kEndReasonLabels must cover every EndReason enumerator");
static_assert(kEndReasonLabels[kEndReasonLabelCount - 1U].reason ==
                  static_cast<EndReason>(END_REASON_COUNT - 1U),
              "END_REASON_COUNT must follow the last EndReason row");

inline bool brewWeightCutSettlesMachineOff(EndReason reason) {
  switch (reason) {
    case EndReason::SCALE_THRESHOLD:
    case EndReason::FAST_EXTRACTION_MAX_WEIGHT:
    case EndReason::FAST_EXTRACTION_MIN_TIME:
    case EndReason::SLOW_EXTRACTION_MAX_TIME:
    case EndReason::SLOW_EXTRACTION_MIN_WEIGHT:
      return true;
    default:
      return false;
  }
}

inline bool brewEndIsAbandonedStart(EndReason reason) {
  return reason == EndReason::UNCONFIRMED_START;
}

enum class ActualWeightSource : uint8_t {
  NONE = 0,
  POST_DRIP = 1,
  LAST_KNOWN = 2
};
constexpr size_t ACTUAL_WEIGHT_SOURCE_COUNT = static_cast<size_t>(ActualWeightSource::LAST_KNOWN) + 1U;

}  // namespace openbrewbyweight

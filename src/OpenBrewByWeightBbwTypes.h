#pragma once

#include <stdint.h>
#include <string.h>
#include <cmath>

namespace openbrewbyweight {

// The single cutoff algorithm id (1). Its API name is "corridor" (the smooth
// cut with drip-offset learning).
enum class BbwAlgorithm : uint8_t { LINEAR_EWMA = 1 };
constexpr uint8_t DEFAULT_BBW_EWMA_ALPHA = 30;  // Hundredths, dimensionless.
// Profile 3: the drip-offset EWMA learning against the smooth-cut line.
constexpr uint8_t BBW_PROFILE_VERSION = 3;
constexpr uint8_t BBW_ALPHA_CANDIDATES[] = {10, 30, 50, 100};

// What a good shot did to the drip correction, kept with its record (5 bits).
// NONE: it trained (or an earlier record); TARGET_ONLY: it trained the
// correction, but the quick start counts only shots stopped at the target.
// Every other note names why the correction stayed. Append only.
// WEIGHT_DISTURBED is the cause-less disturbance of older records;
// LEVEL_SHIFT..SCALE_LOST name the first disturbance of the shot.
enum class DripLearnNote : uint8_t {
  NONE, MANUAL_BREW, TIMER_ONLY, WEIGHT_DISTURBED, CHANNELING, NO_FLOW_RATE,
  NOT_WEIGHT_CUT, NO_FINAL_WEIGHT, FINAL_TOO_LOW, NOT_FRESH, OUT_OF_RANGE,
  PRESET_GONE, OTHER_ALGORITHM, RESTARTED, IMPOSSIBLE, OUTLIER, TARGET_ONLY, CUT_SHORT,
  LEVEL_SHIFT, TOUCH_STOP, OVERLOAD, WEIGHT_JUMP, CUP_LIFTED, SCALE_LOST
};
constexpr uint8_t DRIP_LEARN_NOTE_COUNT = static_cast<uint8_t>(DripLearnNote::SCALE_LOST) + 1;
static_assert(DRIP_LEARN_NOTE_COUNT <= 32, "the record keeps the note in 5 bits");

inline bool dripLearnNoteIsDisturbance(uint8_t note) {
  return note == static_cast<uint8_t>(DripLearnNote::WEIGHT_DISTURBED) ||
         (note >= static_cast<uint8_t>(DripLearnNote::LEVEL_SHIFT) && note < DRIP_LEARN_NOTE_COUNT);
}

inline const char *dripLearnNoteText(uint8_t note) {
  static const char *const text[DRIP_LEARN_NOTE_COUNT] = {
      "trained", "manual brew", "timer-only shot", "weight disturbed during the shot",
      "channeling", "no valid flow rate at the stop", "not a weight cut",
      "final weight unavailable", "final weight below history threshold",
      "final weight not a fresh post-drip reading", "drip outside the safe range",
      "shot preset no longer exists", "shot used another drip algorithm version",
      "drip learning restarted during the shot", "drip impossible for the flow at the stop",
      "drip is an outlier against recent shots",
      "quick start counts only shots stopped at the target",
      "drip wait cut short before the weight settled",
      "weight shifted and stayed during the shot", "touch backup ended the shot",
      "scale reading out of range", "implausible weight jump", "cup lifted during the shot",
      "scale readings lost during the shot"};
  return note < DRIP_LEARN_NOTE_COUNT ? text[note] : "unknown";
}

inline bool validBbwAlpha(uint8_t alpha) {
  return alpha >= 1 && alpha <= 100;
}

inline uint8_t bbwAlgorithmVersion(uint8_t algorithm) {
  (void)algorithm;
  return BBW_PROFILE_VERSION;
}

inline bool parseBbwAlphaBaseline(double value, uint8_t &out) {
  if (!(value >= 0.01 && value <= 1.0)) return false;
  const unsigned hundredths = static_cast<unsigned>(std::lround(value * 100.0));
  if (fabs(value * 100.0 - hundredths) > 1e-9) return false;
  out = static_cast<uint8_t>(hundredths);
  return true;
}

inline bool parseBbwAlgorithm(const char *name, uint8_t &algorithm) {
  if (name == nullptr) return false;
  if (strcmp(name, "corridor") == 0) {
    algorithm = static_cast<uint8_t>(BbwAlgorithm::LINEAR_EWMA);
    return true;
  }
  return false;
}

}  // namespace openbrewbyweight

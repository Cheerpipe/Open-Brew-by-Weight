#pragma once

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightScaleTypes.h"

namespace openbrewbyweight {
namespace bbwEwma {

// Per-shot observation filter (design review 2026-10-29): physics bounds the
// drip by the flow at the stop (effective decay <= 0.6 s plus a splash and
// reporting allowance); statistics reject what disagrees with the recent
// accepted observations; two rejected shots that agree within
// DRIP_STEP_AGREE_G adopt their mean as a real change.
constexpr float DRIP_PHYSICS_FLOOR_G = -0.30f;   // Below: scale event, not drip.
constexpr float DRIP_PHYSICS_TAU_S = 0.6f;
constexpr float DRIP_PHYSICS_ALLOWANCE_G = 0.5f;
constexpr uint8_t DRIP_STAT_WINDOW = 5;
constexpr uint8_t DRIP_STAT_MIN_OBSERVATIONS = 3;
constexpr float DRIP_STAT_MIN_SIGMA_G = 0.10f;
constexpr float DRIP_STAT_MIN_GATE_G = 0.30f;
constexpr float DRIP_STEP_AGREE_G = 0.50f;

inline float clampOffset(float value) {
  return fmaxf(0.0f, fminf(MAX_OFFSET_G, value));
}

inline bool learn(float offsetG, float finalWeightG, float goalG, uint8_t alpha,
                  bool acceptedFreshObservation, float &updatedOffset) {
  const float observation = offsetG + (finalWeightG - goalG);
  if (!acceptedFreshObservation || !validBbwAlpha(alpha) ||
      !std::isfinite(offsetG) || !std::isfinite(observation) ||
      fabsf(observation) > MAX_OFFSET_G) return false;
  updatedOffset = clampOffset(offsetG + (alpha / 100.0f) * (finalWeightG - goalG));
  return true;
}

// True when the observation can be genuine drip: at least the floor, at most
// the tail the flow at the stop can leave behind (rateX100 is dg/s x 100,
// so g/s = x100/1000). No drip-delay term: a settled reading also carries
// in-flight coffee and scale lag, which the allowance covers.
inline bool physicsPlausibleDrip(float observationG, int32_t rateAtStopX100) {
  if (!std::isfinite(observationG) || rateAtStopX100 <= 0) return false;
  return observationG >= DRIP_PHYSICS_FLOOR_G &&
         observationG <= DRIP_PHYSICS_TAU_S * (rateAtStopX100 / 1000.0f) +
                             DRIP_PHYSICS_ALLOWANCE_G;
}

inline void sortWindow(float *values, uint8_t n) {
  for (uint8_t i = 1; i < n; ++i) {
    const float v = values[i];
    uint8_t j = i;
    while (j > 0 && values[j - 1] > v) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = v;
  }
}

// V2: replay bounded observations from the predictions before the oldest one.
// The fifth trajectory compares a custom incumbent; challengers stay unchanged.
struct Evidence {
  static constexpr uint8_t WINDOW = 20;
  float anchors[5] = {};
  float observations[WINDOW] = {};
  uint8_t initialAlpha = 0;  // Seeded by the first eligible observation.
  uint8_t count = 0;
  uint8_t position = 0;
  uint8_t cadence = 0;
  uint8_t sinceSwitch = 0;
  uint8_t challenger = 0;
  uint8_t wins = 0;

  uint8_t observe(float observation, float seed, uint8_t alpha) {
    if (!std::isfinite(observation) || fabsf(observation) > MAX_OFFSET_G ||
        !std::isfinite(seed) || !validBbwAlpha(alpha)) return alpha;
    if (count == 0) {
      for (float &anchor : anchors) anchor = seed;
      initialAlpha = alpha;
    }
    if (count == WINDOW) {
      for (uint8_t i = 0; i < 5; ++i) {
        const uint8_t gain = i < 4 ? BBW_ALPHA_CANDIDATES[i] : initialAlpha;
        anchors[i] = clampOffset(anchors[i] +
            (gain / 100.0f) * (observations[position] - anchors[i]));
      }
    }
    observations[position] = observation;
    position = (position + 1) % WINDOW;
    if (count < WINDOW) ++count;
    if (sinceSwitch < 10) ++sinceSwitch;
    cadence = (cadence + 1) % 5;
    if (count < WINDOW || cadence != 0) return alpha;
    float scores[5] = {};
    uint8_t incumbent = 4;
    for (uint8_t i = 0; i < 5; ++i) {
      const uint8_t gain = i < 4 ? BBW_ALPHA_CANDIDATES[i] : initialAlpha;
      float prediction = anchors[i];
      for (uint8_t j = 0; j < WINDOW; ++j) {
        const float error = observations[(position + j) % WINDOW] - prediction;
        scores[i] += error * error;  // Score before updating.
        prediction = clampOffset(prediction + (gain / 100.0f) * error);
      }
      if (!std::isfinite(scores[i])) { wins = 0; return alpha; }
      if (i < 4 && gain == alpha) incumbent = i;
    }
    uint8_t best = incumbent;
    bool tied = false;
    for (uint8_t i = 0; i < 4; ++i) {
      if (scores[i] < scores[best]) { best = i; tied = false; }
      else if (i != best && scores[i] == scores[best]) tied = true;
    }
    if (tied || best == incumbent || sinceSwitch < 10 ||
        !(scores[best] <= scores[incumbent] * 0.90f)) {
      wins = 0;
      return alpha;
    }
    wins = challenger == best ? wins + 1 : 1;
    challenger = best;
    if (wins < 2) return alpha;
    wins = 0;
    sinceSwitch = 0;
    return BBW_ALPHA_CANDIDATES[best];
  }
};
static_assert(sizeof(Evidence) <= 352, "BBW candidate RAM budget");

// Hampel identifier against the median of the newest DRIP_STAT_WINDOW
// accepted observations (MAD scaled to sigma, with a floor so quantized
// windows cannot collapse the gate to zero). Inactive below
// DRIP_STAT_MIN_OBSERVATIONS: the quick start median owns that range.
inline bool hampelOutlier(const Evidence &evidence, float observationG) {
  if (evidence.count < DRIP_STAT_MIN_OBSERVATIONS) return false;
  constexpr uint8_t W = Evidence::WINDOW;
  float window[DRIP_STAT_WINDOW];
  const uint8_t n = evidence.count < DRIP_STAT_WINDOW ? evidence.count
                                                      : DRIP_STAT_WINDOW;
  for (uint8_t k = 0; k < n; ++k)
    window[k] = evidence.observations[(evidence.position + W - 1 - k) % W];
  sortWindow(window, n);
  const float median = window[n / 2];
  for (uint8_t k = 0; k < n; ++k) window[k] = fabsf(window[k] - median);
  sortWindow(window, n);
  const float sigma = fmaxf(1.4826f * window[n / 2], DRIP_STAT_MIN_SIGMA_G);
  return fabsf(observationG - median) >
         fmaxf(DRIP_STAT_MIN_GATE_G, 3.0f * sigma);
}

}  // namespace bbwEwma
}  // namespace openbrewbyweight

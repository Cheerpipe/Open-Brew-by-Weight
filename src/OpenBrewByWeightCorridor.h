#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OpenBrewByWeightBrewTypes.h"

// =============================================================================
// Smooth cut ("corridor") kernel — pure integer math, no globals, no heap.
// =============================================================================
// Units: weights in decigrams (dg), times in milliseconds measured from the
// machine running start, flow rates in dg/s x 100. Every time is a stop-COMMAND
// time; machine stop latency lives only inside tau. The control task owns all
// state built from these functions.

namespace openbrewbyweight {
namespace corridor {

// Default ramp length D per side: the ceiling ramps to the target over D before
// t1, the target to the floor over D after t2. D = 0 is exactly the legacy
// Fast/Slow guard step shape.
constexpr int32_t RAMP_MS = DEFAULT_RAMP_MS;
// Bisection over [t_k, wall] in integer ms needs at most 16 halvings.
static_assert(HARD_MAX_CIRCUIT_CLOSED_MS < 65536, "solver iteration bound");
constexpr int32_t STOP_LATENCY_MS = 100;
constexpr int32_t RATE_WINDOW_MS = 3000;
constexpr uint8_t RATE_MIN_SAMPLES = 4;
constexpr int32_t RATE_MIN_SPAN_MS = 2000;
constexpr int32_t MAX_RATE_X100 = 4000;            // 40 dg/s
constexpr int32_t MIN_PREDICTION_RATE_X100 = 300;  // 3 dg/s
constexpr int32_t MAX_LEAD_DG = 40;
constexpr int32_t MAD_FLOOR_DG = 3;
constexpr uint8_t MAD_MIN_INNOVATIONS = 4;
constexpr uint8_t MAX_CONSECUTIVE_REJECTIONS = 2;
constexpr int32_t VERDICT_DEAD_BAND_MS = 500;
constexpr int32_t ARRIVAL_CLAMP_MS = 5000;
// Replay-tuned (W3.1): 30 dg/s catches late surges a 50 dg/s floor misses.
constexpr int32_t CHANNELING_RATE_X100 = 3000;
constexpr int32_t CHANNELING_BASE_RATE_X100 = 500;
constexpr int32_t CHANNELING_LOOKBACK_MS = 1000;
constexpr uint8_t CHANNELING_CONFIRM_SAMPLES = 2;
constexpr uint8_t STATS_MAX_SHOTS = 10;
constexpr int32_t STATS_SIGMA_FLOOR_MS = 750;
constexpr uint8_t SCALE_LOSS_MIN_SHOTS = 3;
constexpr uint8_t SHOT_STATS_MIN_SHOTS = 3;
constexpr int32_t PROJECTION_HORIZON_MS = 3000;
constexpr int32_t HISTORY_MARGIN_MS = 500;
// Grind hint (times in ms, ratios in permille): the level averages the newest
// HINT_LEVEL_SHOTS of a segment; F_n is its standard error per sigma and
// G_n = sqrt(1 + F_n^2) that of one new shot against it (n = 1..6).
constexpr uint8_t HINT_LEVEL_SHOTS = 6;
constexpr int16_t HINT_F[HINT_LEVEL_SHOTS] = {1000, 709, 669, 548, 478, 432};
constexpr int16_t HINT_G[HINT_LEVEL_SHOTS] = {1414, 1226, 1203, 1140, 1108, 1089};
constexpr uint8_t HINT_SIGMA_SHOTS = 10;
constexpr uint8_t HINT_SIGMA_MIN_DIFFERENCES = 6;
constexpr uint8_t HINT_SIGMA_SEGMENT_SHOTS = 5;
constexpr uint8_t HINT_DIAL_IN_SHOTS = 12;
constexpr uint8_t HINT_REPEAT_SHOTS = 3;
constexpr uint8_t HINT_VARY_SHOTS = 6;
constexpr uint8_t CP_ARMED_SHOTS = 3;
constexpr int32_t HINT_SIGMA_FLOOR_MS = 1000;
constexpr int32_t HINT_SIGMA_PRIOR_MS = 2000;
constexpr int32_t HINT_SIGMA_CAP_MS = 3000;
constexpr int32_t HINT_BAND_FLOOR_MS = 750;
constexpr int32_t HINT_FIRST_SHOT_MS = 3000;
constexpr int32_t Z_TENTATIVE = 1000;
constexpr int32_t Z_LIKELY = 1645;
constexpr int32_t Z_CONFIDENT = 2330;
constexpr int32_t R_MOVED = 1250;  // residuals in permille of sigma * G_n
constexpr int32_t R_SINGLE = 2000;
constexpr int32_t R_UNARMED = 2500;
constexpr int32_t HINT_CENSORED_KEY = 1 << 24;  // sorts a censored shot slowest

struct Params {
  int32_t goalDg = 0;
  int32_t fastMarginDg = 0;
  int32_t slowMarginDg = 0;
  bool fastOn = false;
  bool slowOn = false;
  int32_t delayMs = 0;
  int32_t fastBeforeMs = 0;  // t1
  int32_t slowAfterMs = 0;   // t2
  int32_t wallMs = 0;
  int32_t fastRampMs = RAMP_MS;  // D before t1
  int32_t slowRampMs = RAMP_MS;  // D after t2
  uint8_t fastCurve = 0;         // RAMP_CURVES index
  uint8_t slowCurve = 0;
};

// Transition curves e(u), u in [0, 1]: the regularized incomplete beta
// I(u; a, b) with integer a, b >= 2, so each is monotone (the line never
// rises, which crossingMs relies on) and flat at both ends. Index = type *
// RAMP_HARDNESS_LEVELS + hardness; index 0 (Even, Soft) is Beta(2,2), the
// original smoothstep 3u^2 - 2u^3.
struct RampCurve {
  uint8_t a;
  uint8_t b;
};
constexpr RampCurve RAMP_CURVES[RAMP_CURVE_TYPES * RAMP_HARDNESS_LEVELS] = {
    {2, 2}, {3, 3}, {4, 4},  // Even: smoothstep, smootherstep, smootheststep
    {2, 3}, {2, 4}, {2, 6},  // Steep early
    {3, 2}, {4, 2}, {6, 2}};  // Steep late
constexpr uint8_t RAMP_CURVE_MAX_DEGREE = 7;
constexpr bool rampCurvesValid() {
  for (const RampCurve &c : RAMP_CURVES)
    if (c.a < 2 || c.b < 2 || c.a + c.b - 1 > RAMP_CURVE_MAX_DEGREE) return false;
  return true;
}
static_assert(rampCurvesValid(), "curves must be flat at both ends and fit the evaluator");
constexpr double rampWideBound() {
  double v = MAX_SIDE_MARGIN_DG;
  for (uint8_t i = 0; i < RAMP_CURVE_MAX_DEGREE; ++i) v *= MAX_RAMP_MS;
  return v;
}
static_assert(rampWideBound() < 3.4e38, "margin * D^7 must fit the 128-bit evaluator");
static_assert(MAX_RAMP_MS < 65536, "D^2 must fit one 32-bit divisor");
inline uint8_t rampCurveIndex(uint8_t type, uint8_t hardness) {
  return type < RAMP_CURVE_TYPES && hardness < RAMP_HARDNESS_LEVELS
             ? static_cast<uint8_t>(type * RAMP_HARDNESS_LEVELS + hardness)
             : 0;
}
inline uint8_t rampCurveTypeOf(uint8_t index) { return index / RAMP_HARDNESS_LEVELS; }
inline uint8_t rampCurveHardnessOf(uint8_t index) { return index % RAMP_HARDNESS_LEVELS; }

// Unsigned 128-bit value in 32-bit limbs: products and quotients by small
// integers, all the exact ramp evaluation needs on any target.
struct RampWide {
  uint32_t w[4] = {};
  void mul(uint32_t k) {
    uint64_t c = 0;
    for (uint32_t &x : w) {
      c += static_cast<uint64_t>(x) * k;
      x = static_cast<uint32_t>(c);
      c >>= 32;
    }
  }
  void add(const RampWide &y) {
    uint64_t c = 0;
    for (int i = 0; i < 4; ++i) {
      c += static_cast<uint64_t>(w[i]) + y.w[i];
      w[i] = static_cast<uint32_t>(c);
      c >>= 32;
    }
  }
  void div(uint32_t k) {
    uint64_t r = 0;
    for (int i = 3; i >= 0; --i) {
      r = (r << 32) | w[i];
      w[i] = static_cast<uint32_t>(r / k);
      r -= static_cast<uint64_t>(w[i]) * k;
    }
  }
};

// Exact floor(margin * e(n / d)) when `reached`, else floor(margin * (1 - e)):
// the Bernstein sum of I over j >= a (or j < a) divided by d^N, N = a + b - 1.
// For Beta(2,2) these are the original smoothstep expressions bit for bit.
inline int32_t rampDg(int32_t marginDg, int32_t n, int32_t d, uint8_t curve, bool reached) {
  const RampCurve c = RAMP_CURVES[curve < RAMP_CURVE_TYPES * RAMP_HARDNESS_LEVELS ? curve : 0];
  const uint32_t degree = c.a + c.b - 1u;
  RampWide sum;
  uint32_t choose = 1;  // C(degree, j)
  for (uint32_t j = 0; j <= degree; ++j) {
    if ((j >= c.a) == reached) {
      RampWide term;
      term.w[0] = choose;
      for (uint32_t i = 0; i < j; ++i) term.mul(static_cast<uint32_t>(n));
      for (uint32_t i = j; i < degree; ++i) term.mul(static_cast<uint32_t>(d - n));
      sum.add(term);
    }
    choose = choose * (degree - j) / (j + 1);
  }
  sum.mul(static_cast<uint32_t>(marginDg));
  // floor(floor(x / D^2) / D) = floor(x / D^3): D^2 per step halves the divisions.
  const uint32_t ud = static_cast<uint32_t>(d);
  for (uint32_t i = 0; i < degree; i += 2) sum.div(i + 1 < degree ? ud * ud : ud);
  return static_cast<int32_t>(sum.w[0]);
}

inline int32_t ceilingDg(const Params &p) {
  return p.goalDg + (p.fastOn ? p.fastMarginDg : 0);
}
inline int32_t floorDg(const Params &p) {
  return p.goalDg - (p.slowOn ? p.slowMarginDg : 0);
}
inline int32_t fastRampStartMs(const Params &p) {
  const int32_t a = p.fastBeforeMs - p.fastRampMs > p.delayMs ? p.fastBeforeMs - p.fastRampMs
                                                              : p.delayMs;
  return a < p.fastBeforeMs ? a : p.fastBeforeMs;
}
inline int32_t slowRampEndMs(const Params &p) {
  const int32_t b = p.slowAfterMs + p.slowRampMs < p.wallMs ? p.slowAfterMs + p.slowRampMs
                                                            : p.wallMs;
  return b > p.slowAfterMs ? b : p.slowAfterMs;
}

// f(t): ceiling held to a, eased down to the target at t1 along the Fast curve,
// straight target through [t1, t2], eased down to the floor at b along the Slow
// curve, floor held. Ramps truncate toward the target; zero-length ramps are
// legacy steps.
inline int32_t cutLineDg(const Params &p, int32_t t) {
  const int32_t a = fastRampStartMs(p);
  if (t < p.fastBeforeMs) {
    if (t < a) return ceilingDg(p);
    return p.goalDg +
           rampDg(ceilingDg(p) - p.goalDg, t - a, p.fastBeforeMs - a, p.fastCurve, false);
  }
  if (t < p.slowAfterMs || (t == p.slowAfterMs && p.slowRampMs > 0)) return p.goalDg;
  const int32_t b = slowRampEndMs(p);
  if (t >= b) return floorDg(p);
  return p.goalDg -
         rampDg(p.goalDg - floorDg(p), t - p.slowAfterMs, b - p.slowAfterMs, p.slowCurve, true);
}

// EndReason of a weight cut = the segment of f that fired.
inline EndReason cutReason(const Params &p, int32_t lineDg) {
  if (lineDg > p.goalDg) return EndReason::FAST_EXTRACTION_MAX_WEIGHT;
  if (lineDg < p.goalDg) return EndReason::SLOW_EXTRACTION_MIN_WEIGHT;
  return EndReason::SCALE_THRESHOLD;
}

inline bool weightCutReason(uint8_t reason) {
  return reason == static_cast<uint8_t>(EndReason::SCALE_THRESHOLD) ||
         reason == static_cast<uint8_t>(EndReason::FAST_EXTRACTION_MAX_WEIGHT) ||
         reason == static_cast<uint8_t>(EndReason::SLOW_EXTRACTION_MIN_WEIGHT);
}

// tau = protocol sensor lag + valve stop latency + momentary stop pulse.
inline int32_t tauMs(uint16_t sensorLagMs, uint32_t momentaryStopPulseMs) {
  return static_cast<int32_t>(sensorLagMs) + STOP_LATENCY_MS +
         static_cast<int32_t>(momentaryStopPulseMs);
}

inline int32_t leadDg(int32_t rateX100, int32_t tau) {
  const int64_t lead = static_cast<int64_t>(rateX100) * tau / 100000;
  return lead > MAX_LEAD_DG ? MAX_LEAD_DG : static_cast<int32_t>(lead);
}

// Small in-place sort for <= 32 values (no heap).
inline void sortSmall(int32_t *values, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    const int32_t v = values[i];
    uint8_t j = i;
    for (; j > 0 && values[j - 1] > v; --j) values[j] = values[j - 1];
    values[j] = v;
  }
}
inline int32_t medianOfSorted(const int32_t *sorted, uint8_t count) {
  if (count == 0) return 0;
  return count % 2 ? sorted[count / 2]
                   : (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
}
inline int32_t madOf(const int32_t *values, uint8_t count, int32_t median) {
  int32_t deviations[32];
  for (uint8_t i = 0; i < count; ++i) {
    deviations[i] = values[i] > median ? values[i] - median : median - values[i];
  }
  sortSmall(deviations, count);
  return medianOfSorted(deviations, count);
}

// Time-window least-squares flow estimator with a MAD innovation gate.
struct RateEstimator {
  static constexpr uint8_t CAPACITY = 32;
  static constexpr int32_t NO_RATE = INT32_MIN;
  enum class Result : uint8_t { ACCEPTED, REJECTED, IGNORED };

  int32_t atMs[CAPACITY] = {};
  int32_t rawRateX100[CAPACITY] = {};
  int16_t weightDg[CAPACITY] = {};
  int16_t innovationDg[CAPACITY] = {};
  uint8_t head = 0;  // index of the newest sample
  uint8_t count = 0;
  uint8_t rejectedRun = 0;
  uint8_t channelingRun = 0;
  bool valid = false;
  bool channeling = false;  // latched for the rest of the shot
  int32_t rateX100 = 0;     // clamped q-hat
  int32_t rawX100 = 0;      // unclamped q-hat
  int32_t onsetMs = 0;      // first drop; surge baselines need a full window after it

  void reset() {
    const bool latched = channeling;
    const int32_t onset = onsetMs;
    *this = RateEstimator{};
    channeling = latched;
    onsetMs = onset;
  }

  uint8_t index(uint8_t age) const {
    return static_cast<uint8_t>((head + CAPACITY - age) % CAPACITY);
  }

  Result push(int32_t t, int32_t weight) {
    if (count > 0 && t <= atMs[head]) return Result::IGNORED;
    if (weight > INT16_MAX) weight = INT16_MAX;
    if (weight < INT16_MIN) weight = INT16_MIN;
    while (count > 0 && atMs[index(count - 1)] <= t - RATE_WINDOW_MS) --count;
    int32_t innovation = 0;
    if (count > 0) {
      const int32_t predicted = weightDg[head] +
          (valid ? static_cast<int32_t>(static_cast<int64_t>(rateX100) *
                                        (t - atMs[head]) / 100000)
                 : 0);
      innovation = weight - predicted;
    }
    if (count >= MAD_MIN_INNOVATIONS) {
      int32_t innovations[CAPACITY];
      uint8_t n = 0;
      for (uint8_t age = 0; age + 1 < count; ++age)
        innovations[n++] = innovationDg[index(age)];
      sortSmall(innovations, n);
      const int32_t median = medianOfSorted(innovations, n);
      const int32_t mad = madOf(innovations, n, median);
      const int64_t deviation = innovation > median ? innovation - median
                                                    : median - innovation;
      // 4.5 * max(1.4826 * MAD, 3 dg), in 1e-4 dg.
      const int64_t scaled = static_cast<int64_t>(mad) * 66717;
      const int64_t limit = scaled > 45000 * MAD_FLOOR_DG ? scaled : 45000 * MAD_FLOOR_DG;
      if (deviation * 10000 > limit) {
        if (++rejectedRun <= MAX_CONSECUTIVE_REJECTIONS) return Result::REJECTED;
        // The stream moved for good: relearn from this sample.
        reset();
        innovation = 0;
      }
    }
    rejectedRun = 0;
    head = static_cast<uint8_t>((head + 1) % CAPACITY);
    if (count < CAPACITY) ++count;
    atMs[head] = t;
    weightDg[head] = static_cast<int16_t>(weight);
    innovationDg[head] = static_cast<int16_t>(
        innovation > INT16_MAX ? INT16_MAX : innovation < INT16_MIN ? INT16_MIN : innovation);
    fit();
    rawRateX100[head] = valid ? rawX100 : NO_RATE;
    updateChanneling();
    return Result::ACCEPTED;
  }

  void fit() {
    valid = false;
    if (count < RATE_MIN_SAMPLES ||
        atMs[head] - atMs[index(count - 1)] < RATE_MIN_SPAN_MS) return;
    int64_t sumT = 0, sumW = 0;
    for (uint8_t age = 0; age < count; ++age) {
      sumT += atMs[index(age)] - atMs[head];
      sumW += weightDg[index(age)];
    }
    int64_t sxx = 0, sxy = 0;
    for (uint8_t age = 0; age < count; ++age) {
      const int64_t dt = (atMs[index(age)] - atMs[head]) * count - sumT;
      const int64_t dw = static_cast<int64_t>(weightDg[index(age)]) * count - sumW;
      sxx += dt * dt;
      sxy += dt * dw;
    }
    if (sxx <= 0) return;
    const int64_t raw = sxy * 100000 / sxx;
    const int64_t bounded = raw > INT32_MAX / 2 ? INT32_MAX / 2 : raw;
    rawX100 = static_cast<int32_t>(bounded < -INT32_MAX / 2 ? -INT32_MAX / 2 : bounded);
    rateX100 = rawX100 < 0 ? 0 : rawX100;
    if (rateX100 > MAX_RATE_X100) rateX100 = MAX_RATE_X100;
    valid = true;
  }

  // Predicate on consecutive accepted samples: q_raw >= 30 dg/s and >= 1.5x
  // the q_raw at least 1 s earlier, which itself was >= 5 dg/s over a window
  // wholly after the first drop (the onset ramp is not a surge).
  void updateChanneling() {
    bool surge = false;
    if (valid && rawX100 >= CHANNELING_RATE_X100) {
      for (uint8_t age = 1; age < count; ++age) {
        const uint8_t i = index(age);
        if (atMs[i] > atMs[head] - CHANNELING_LOOKBACK_MS) continue;
        surge = atMs[i] - RATE_WINDOW_MS >= onsetMs && rawRateX100[i] != NO_RATE &&
                rawRateX100[i] >= CHANNELING_BASE_RATE_X100 &&
                static_cast<int64_t>(rawX100) * 2 >= static_cast<int64_t>(rawRateX100[i]) * 3;
        break;
      }
    }
    channelingRun = surge ? static_cast<uint8_t>(channelingRun + 1) : 0;
    if (channelingRun >= CHANNELING_CONFIRM_SAMPLES) channeling = true;
  }
};

// Earliest t >= t_k where W_k + q(t - t_k) + q*tau + r >= f(t). Returns false
// ("none") when q < 3 dg/s; returns the wall when the line is never met.
inline bool crossingMs(const Params &p, int32_t tk, int32_t weightDgAtK,
                       int32_t rateX100, int32_t tau, int32_t offsetDg,
                       int32_t &out) {
  if (rateX100 < MIN_PREDICTION_RATE_X100) return false;
  const int32_t lead = leadDg(rateX100, tau);
  auto met = [&](int32_t t) {
    const int64_t ahead = static_cast<int64_t>(rateX100) * (t - tk) / 100000;
    return weightDgAtK + ahead + lead + offsetDg >= cutLineDg(p, t);
  };
  if (tk >= p.wallMs || met(tk)) {
    out = tk;
    return true;
  }
  if (!met(p.wallMs)) {
    out = p.wallMs;
    return true;
  }
  int32_t lo = tk, hi = p.wallMs;
  while (hi - lo > 1) {
    const int32_t mid = lo + (hi - lo) / 2;
    (met(mid) ? hi : lo) = mid;
  }
  out = hi;
  return true;
}

// Backstop direct-law threshold (no prediction lead).
inline int32_t backstopThresholdDg(const Params &p, int32_t t, int32_t offsetDg) {
  return cutLineDg(p, t) - offsetDg;
}

enum class Verdict : uint8_t { ON_RECIPE = 0, FAST = 1, SLOW = 2, UNCLASSIFIED = 3 };

struct ArrivalVerdict {
  Verdict verdict = Verdict::UNCLASSIFIED;
  int32_t arrivalMs = 0;
};

// t_arr = t_cut + (target - f(t_cut)) / q_cut, clamped to +-5 s.
inline ArrivalVerdict arrivalVerdict(const Params &p, int32_t cutMs, int32_t rateX100) {
  ArrivalVerdict out;
  const int32_t line = cutLineDg(p, cutMs);
  if (line == p.goalDg) {
    out.arrivalMs = cutMs;
  } else {
    if (rateX100 < MIN_PREDICTION_RATE_X100) return out;
    int64_t shift = static_cast<int64_t>(p.goalDg - line) * 100000 / rateX100;
    if (shift > ARRIVAL_CLAMP_MS) shift = ARRIVAL_CLAMP_MS;
    if (shift < -ARRIVAL_CLAMP_MS) shift = -ARRIVAL_CLAMP_MS;
    out.arrivalMs = cutMs + static_cast<int32_t>(shift);
    if (out.arrivalMs <= 0) return ArrivalVerdict{};  // before the shot: no arrival
  }
  out.verdict = out.arrivalMs < p.fastBeforeMs - VERDICT_DEAD_BAND_MS ? Verdict::FAST
              : out.arrivalMs > p.slowAfterMs + VERDICT_DEAD_BAND_MS ? Verdict::SLOW
                                                                     : Verdict::ON_RECIPE;
  return out;
}

// Persisted verdict byte: bits 0-1 verdict, bit 2 channeling, bits 3-4
// scale-loss source, bit 5 blind (scale-loss shot without a valid weight).
enum class LossSource : uint8_t { PROJECTION = 0, HISTORY = 1, WINDOW = 2, FIXED = 3 };
constexpr uint8_t VERDICT_CHANNELING_BIT = 0x04;
constexpr uint8_t VERDICT_BLIND_BIT = 0x20;
inline uint8_t packVerdict(Verdict verdict, bool channeling, LossSource source, bool blind) {
  return static_cast<uint8_t>(static_cast<uint8_t>(verdict) |
                              (channeling ? VERDICT_CHANNELING_BIT : 0) |
                              (static_cast<uint8_t>(source) << 3) |
                              (blind ? VERDICT_BLIND_BIT : 0));
}
inline Verdict verdictOf(uint8_t packed) { return static_cast<Verdict>(packed & 3); }
inline bool channelingOf(uint8_t packed) { return (packed & VERDICT_CHANNELING_BIT) != 0; }
inline LossSource lossSourceOf(uint8_t packed) {
  return static_cast<LossSource>((packed >> 3) & 3);
}
inline bool blindOf(uint8_t packed) { return (packed & VERDICT_BLIND_BIT) != 0; }

// One shot-log row as the statistics see it.
struct ArrivalRecord {
  uint32_t recordId = 0;
  uint8_t presetId = 0;
  uint8_t endReason = 0;
  uint8_t verdict = 0;  // packed
  int32_t arrivalMs = 0;
  int32_t firstDropMs = 0;  // 0 = not measured
  // The window and target the shot was judged against.
  int32_t fastBeforeMs = 0;
  int32_t slowAfterMs = 0;
  int32_t goalDg = 0;
  int32_t durationMs = 0;     // shot time at the stop
  int32_t finalWeightDg = 0;  // 0 = not measured
  uint16_t avgFlowCgS = 0;    // post-first-drop average flow; 0 = missing
};

// Which shots count for a preset: its own since its epoch (New coffee), judged
// against its current window and a target within STATS_GOAL_TOLERANCE_DG.
constexpr int32_t STATS_GOAL_TOLERANCE_DG = 10;
struct StatsScope {
  uint8_t presetId = 0;
  uint32_t epoch = 0;
  int32_t fastBeforeMs = 0;
  int32_t slowAfterMs = 0;
  int32_t goalDg = 0;
};

struct ArrivalStats {
  uint8_t n = 0;
  int32_t minMs = 0;
  int32_t maxMs = 0;
  int32_t medianMs = 0;
  int32_t sigmaMs = 0;
  uint16_t total = 0;  // admissible shots since the epoch
};

// Slot for recordId among the newest STATS_MAX_SHOTS kept ids: a free slot
// until full, then the oldest one if recordId is newer, else -1.
inline int newestSlot(uint32_t *ids, uint8_t &n, uint32_t recordId) {
  uint8_t slot = n;
  if (n == STATS_MAX_SHOTS) {
    slot = 0;
    for (uint8_t j = 1; j < n; ++j) if (ids[j] < ids[slot]) slot = j;
    if (ids[slot] >= recordId) return -1;
  } else {
    ++n;
  }
  ids[slot] = recordId;
  return slot;
}

// A slow shot cut short (design 3.3): max time or a manual stop after t2 with
// 10-100 % of the target in the cup. Slow evidence only: its arrival is at
// least the shot time, past t2.
inline bool censoredArrival(const ArrivalRecord &r) {
  const auto e = static_cast<EndReason>(r.endReason);
  return (e == EndReason::CONFIGURED_WALL_LIMIT || e == EndReason::GLOBAL_LIMIT ||
          e == EndReason::SLOW_EXTRACTION_MAX_TIME || e == EndReason::ACTIVATOR ||
          e == EndReason::WEB_STOP || e == EndReason::PHYSICAL_OVERRIDE) &&
         r.durationMs > r.slowAfterMs && r.finalWeightDg * 10 >= r.goalDg &&
         r.finalWeightDg < r.goalDg;
}
inline int32_t hintArrivalMs(const ArrivalRecord &r) {
  return censoredArrival(r) && r.durationMs > r.arrivalMs ? r.durationMs : r.arrivalMs;
}
// The projection hit the +-5 s clamp (both stored times truncate to 0.1 s).
inline bool clampedArrival(const ArrivalRecord &r) {
  const int32_t shift = r.arrivalMs - r.durationMs;
  return r.durationMs > 0 && r.arrivalMs > 0 &&
         (shift >= ARRIVAL_CLAMP_MS - 100 || -shift >= ARRIVAL_CLAMP_MS - 100);
}

// The grind hint reads the scope's shots before the epoch too (the spread
// belongs to the shooter, not the coffee) and admits censored shots.
inline bool hintAdmissible(const ArrivalRecord &r, const StatsScope &s) {
  return r.presetId == s.presetId && r.fastBeforeMs == s.fastBeforeMs &&
         r.slowAfterMs == s.slowAfterMs && r.goalDg - s.goalDg <= STATS_GOAL_TOLERANCE_DG &&
         s.goalDg - r.goalDg <= STATS_GOAL_TOLERANCE_DG && !channelingOf(r.verdict) &&
         !blindOf(r.verdict) &&
         (censoredArrival(r) || (weightCutReason(r.endReason) &&
                                 verdictOf(r.verdict) != Verdict::UNCLASSIFIED && r.arrivalMs > 0));
}
inline bool admissibleArrival(const ArrivalRecord &r, const StatsScope &s) {
  return r.recordId >= s.epoch && weightCutReason(r.endReason) && hintAdmissible(r, s);
}

// Statistics over the newest <= 10 admissible arrivals of one preset; rowAt(i)
// returns the i-th ArrivalRecord in any order (no copy of the whole log).
// arrivalsOut, when given, receives the n kept arrivals in ascending order.
// The preset's reference plateau flow q_p (rateX100) from the same
// admissible rows: median of avgFlow scaled by the ramp shape. avgFlow is
// the post-first-drop mean of a ramp-then-plateau curve, so the plateau it
// undershoots is ~4/3 of it (linear ramp over half the flow time; the gate
// that consumes q_p has multi-second margins). 0 when no row has a flow.
template <typename RowAt>
inline int32_t plateauFlowX100(size_t count, RowAt rowAt, const StatsScope &scope) {
  uint32_t ids[STATS_MAX_SHOTS] = {};
  int32_t flows[STATS_MAX_SHOTS] = {};
  uint8_t n = 0;
  for (size_t i = 0; i < count; ++i) {
    const ArrivalRecord r = rowAt(i);
    if (!hintAdmissible(r, scope) || r.avgFlowCgS == 0) continue;
    const int slot = newestSlot(ids, n, r.recordId);
    if (slot < 0) continue;
    flows[slot] = static_cast<int32_t>(r.avgFlowCgS) * 40 / 3;
  }
  if (n == 0) return 0;
  sortSmall(flows, n);
  return medianOfSorted(flows, n);
}

template <typename RowAt>
inline ArrivalStats arrivalStatsOf(size_t count, RowAt rowAt, const StatsScope &scope,
                                   int32_t *arrivalsOut = nullptr) {
  uint32_t ids[STATS_MAX_SHOTS] = {};
  int32_t times[STATS_MAX_SHOTS] = {};
  uint8_t n = 0;
  uint16_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    const ArrivalRecord r = rowAt(i);
    if (!admissibleArrival(r, scope)) continue;
    if (total < UINT16_MAX) ++total;
    const int slot = newestSlot(ids, n, r.recordId);
    if (slot >= 0) times[slot] = r.arrivalMs;
  }
  ArrivalStats out;
  out.n = n;
  out.total = total;
  if (n == 0) return out;
  int32_t sorted[STATS_MAX_SHOTS];
  memcpy(sorted, times, n * sizeof(int32_t));
  sortSmall(sorted, n);
  if (arrivalsOut) memcpy(arrivalsOut, sorted, n * sizeof(int32_t));
  out.minMs = sorted[0];
  out.maxMs = sorted[n - 1];
  out.medianMs = medianOfSorted(sorted, n);
  const int32_t sigma = static_cast<int32_t>(
      static_cast<int64_t>(madOf(sorted, n, out.medianMs)) * 14826 / 10000);
  out.sigmaMs = sigma > STATS_SIGMA_FLOOR_MS ? sigma : STATS_SIGMA_FLOOR_MS;
  return out;
}

inline ArrivalStats arrivalStats(const ArrivalRecord *records, size_t count,
                                 uint8_t presetId, uint32_t epoch) {
  StatsScope scope;
  scope.presetId = presetId;
  scope.epoch = epoch;
  return arrivalStatsOf(count, [records](size_t i) { return records[i]; }, scope);
}

// Median of the newest <= 10 measured first drops of one preset since its
// epoch, or with otherPresets of every other preset's shots (the fallback for
// a preset without any). Unlike arrivals it ignores the window, target and
// how the shot ended: none of them moves the first drop.
struct FirstDropStats {
  uint8_t n = 0;
  int32_t medianMs = 0;  // 0 = none
};

template <typename RowAt>
inline FirstDropStats firstDropStatsOf(size_t count, RowAt rowAt, uint8_t presetId,
                                       uint32_t epoch, bool otherPresets) {
  uint32_t ids[STATS_MAX_SHOTS] = {};
  int32_t drops[STATS_MAX_SHOTS] = {};
  uint8_t n = 0;
  for (size_t i = 0; i < count; ++i) {
    const ArrivalRecord r = rowAt(i);
    const bool own = r.presetId == presetId;
    if (r.firstDropMs <= 0 || (otherPresets ? own : !own || r.recordId < epoch)) continue;
    const int slot = newestSlot(ids, n, r.recordId);
    if (slot >= 0) drops[slot] = r.firstDropMs;
  }
  sortSmall(drops, n);
  FirstDropStats out;
  out.n = n;
  out.medianMs = medianOfSorted(drops, n);
  return out;
}

// Grind hint (design 2026-10-16): the trimmed level of the newest <= 6 shots
// on the current grind setting (a "segment") against the middle c of the
// window, in standard errors of the shooter's own spread. A pure replay of the
// preset's shot log; nothing is stored.
enum class HintState : uint8_t { NONE, DIAL_IN, MAINTENANCE, CHECKING, VARY };
enum class HintDirection : uint8_t { NONE, FINER, COARSER };
enum class HintTier : uint8_t { NONE, TENTATIVE, LIKELY, CONFIDENT };
enum class HintBin : uint8_t { NONE, SMALL, MEDIUM, LARGE };

inline HintTier tierOf(int32_t zPermille) {
  return zPermille >= Z_CONFIDENT ? HintTier::CONFIDENT
       : zPermille >= Z_LIKELY    ? HintTier::LIKELY
       : zPermille >= Z_TENTATIVE ? HintTier::TENTATIVE
                                  : HintTier::NONE;
}

// Comfort band half-width b = max(h/2, h - 2 sigma), at least 750 ms.
inline int32_t bandHalfMs(int32_t halfMs, int32_t sigmaMs) {
  const int32_t b = halfMs - 2 * sigmaMs > halfMs / 2 ? halfMs - 2 * sigmaMs : halfMs / 2;
  return b > HINT_BAND_FLOOR_MS ? b : HINT_BAND_FLOOR_MS;
}

// The window is tighter than the shooter's long-run spread (design 5.4):
// sigmaLong > 0.8 h. Never the recent spread, which an undetected step inflates.
inline bool hintVaries(int32_t sigmaLongMs, int32_t halfMs) { return 10 * sigmaLongMs > 8 * halfMs; }

// Size of a distance D from c: a touch below h, medium below max(2h, c/5).
inline HintBin binOf(int32_t distanceMs, int32_t middleMs, int32_t halfMs) {
  if (distanceMs < halfMs) return HintBin::SMALL;
  return distanceMs < (2 * halfMs > middleMs / 5 ? 2 * halfMs : middleMs / 5) ? HintBin::MEDIUM
                                                                               : HintBin::LARGE;
}

// Level of 1..6 sort keys: the shot, the mean of two, else the mean without
// the fastest and the slowest (a censored key sorts slowest).
inline int32_t trimmedLevelMs(int32_t *keys, uint8_t n) {
  sortSmall(keys, n);
  const uint8_t trim = n > 2 ? 1 : 0;
  int64_t sum = 0;
  for (uint8_t i = trim; i + trim < n; ++i) sum += keys[i] % HINT_CENSORED_KEY;
  return static_cast<int32_t>(sum / (n - 2 * trim));
}

// The one spread estimator (sigmaLong and sigma_recent): consecutive
// differences, 1.4826 MAD / sqrt(2). A step between two shots moves one
// difference, not the whole scale (sorted in place).
inline int32_t differenceSigmaMs(int32_t *differences, uint8_t n) {
  sortSmall(differences, n);
  const int64_t mad = madOf(differences, n, medianOfSorted(differences, n));
  return static_cast<int32_t>(mad * 1483 / 1000 * 7071 / 10000);
}

// One evaluation's inputs: the level of the newest n segment shots, the
// median of the segment's last three (0 below three), sigma_use and sigmaLong.
struct HintEvidence {
  bool dialIn = true;
  uint8_t n = 0;
  uint8_t censored = 0;  // censored shots in the level
  int32_t levelMs = 0;
  int32_t lastThreeMs = 0;
  int32_t sigmaMs = HINT_SIGMA_PRIOR_MS;
  int32_t sigmaLongMs = HINT_SIGMA_PRIOR_MS;
  HintDirection advised = HintDirection::NONE;  // announced in this segment
};

struct HintDecision {
  HintDirection direction = HintDirection::NONE;  // the level's side of c
  HintTier tier = HintTier::NONE;  // NONE: below the phase's threshold
  HintBin bin = HintBin::NONE;
  int32_t z = 0;  // level past the band on that side, permille SE
};

// Decision rules (design 4.3-5.4, 8.1) for a window [t1, t2].
inline HintDecision hintDecision(const HintEvidence &e, const StatsScope &w) {
  HintDecision d;
  if (e.n == 0) return d;
  const int32_t c = (w.fastBeforeMs + w.slowAfterMs) / 2, h = (w.slowAfterMs - w.fastBeforeMs) / 2;
  const int32_t se = e.sigmaMs * HINT_F[e.n - 1] / 1000;
  const bool slow = e.levelMs > c;
  const int32_t past = slow ? e.levelMs - c : c - e.levelMs;
  d.direction = slow ? HintDirection::COARSER : HintDirection::FINER;
  d.z = static_cast<int32_t>(static_cast<int64_t>(past - bandHalfMs(h, e.sigmaMs)) * 1000 / se);
  if (hintVaries(e.sigmaLongMs, h)) {
    if (static_cast<int64_t>(past - h) * 1000 / se >= Z_CONFIDENT) d.tier = HintTier::CONFIDENT;
  } else {
    if (!e.dialIn)
      d.tier = e.n >= 3 && d.z >= Z_LIKELY ? tierOf(d.z) : HintTier::NONE;
    else if (e.n == 1)
      d.tier = past >= h + (2 * e.sigmaMs > HINT_FIRST_SHOT_MS ? 2 * e.sigmaMs : HINT_FIRST_SHOT_MS)
                   ? HintTier::TENTATIVE : HintTier::NONE;
    else
      d.tier = e.n == 2 && d.z >= Z_TENTATIVE ? HintTier::TENTATIVE : tierOf(d.z);
    // Hard fallback: the last three's median outside the window by > SE.
    const int32_t se3 = e.sigmaMs * HINT_F[2] / 1000;
    if (e.lastThreeMs > 0 && d.tier < HintTier::LIKELY &&
        (slow ? e.lastThreeMs - w.slowAfterMs : w.fastBeforeMs - e.lastThreeMs) > se3)
      d.tier = HintTier::LIKELY;
  }
  // Two censored shots: coarser at likely (the level is then always slow).
  if (e.censored >= 2 && slow && d.tier < HintTier::LIKELY) d.tier = HintTier::LIKELY;
  if (e.advised != HintDirection::NONE && d.direction != e.advised) d.tier = HintTier::NONE;
  int32_t distance = past;
  if (!e.dialIn) distance -= Z_LIKELY * se / 1000;
  d.bin = binOf(distance > 0 ? distance : 0, c, h);
  return d;
}

struct GrindHintV2 {
  HintState state = HintState::NONE;
  HintDirection direction = HintDirection::NONE;
  HintTier tier = HintTier::NONE;
  HintBin bin = HintBin::NONE;
  uint8_t n = 0;          // shots in the level
  bool atLeast = false;   // a clamped or censored shot among them
  int32_t offsetMs = 0;   // level - c
  int32_t bandMs = 0;     // comfort band half-width b
  uint32_t segmentStartId = 0;
};

// Replays one preset's hint-admissible shots, oldest first, through change
// points, spread, decision and the hint lifecycle (design 4-7). It lives for
// one evaluation; the closed-loop test feeds it shot by shot.
struct GrindHintReplay {
  static constexpr uint8_t RING = 16;
  static constexpr uint32_t NO_SHOT = UINT32_MAX;
  struct Shot {
    uint32_t id;
    uint32_t segment;  // sequence number of its segment's first shot
    int32_t key;       // arrival, + HINT_CENSORED_KEY when censored
    bool clamped;
  };
  struct Level {
    uint8_t n = 0;
    uint8_t censored = 0;
    bool atLeast = false;
    int32_t ms = 0;
  };

  StatsScope scope;
  Shot shots[RING] = {};
  uint32_t pushed = 0;  // shot k sits at shots[k % RING]
  uint32_t segmentStart = 0;
  uint32_t segmentStartId = 0;
  uint32_t epochStart = NO_SHOT;  // first shot since the epoch
  uint32_t candidate = NO_SHOT;   // first shot after the latest announcement
  uint32_t announceId = 0;        // record of the latest announcement
  int32_t sigmaLongMs = HINT_SIGMA_PRIOR_MS;
  int32_t sigmaMs = HINT_SIGMA_PRIOR_MS;  // sigma_use after the latest shot
  int32_t movedPermille = 0;  // latest shot's residual, advised side
  bool sigmaSeeded = false;
  bool segmentCounted = false;  // this segment already fed sigmaLong
  bool dialIn = true;
  bool armedPending = false;
  int8_t unarmedSide = 0;
  uint8_t dialInShots = 0;
  uint8_t armedLeft = 0;
  uint8_t sinceAnnounce = 0;
  uint8_t repeats = 0;
  HintDirection hint = HintDirection::NONE;     // shown
  HintDirection advised = HintDirection::NONE;  // announced in this segment
  HintTier announcedTier = HintTier::NONE;
  HintBin announcedBin = HintBin::NONE;
  GrindHintV2 out;

  explicit GrindHintReplay(const StatsScope &s) : scope(s) {}

  const Shot &at(uint32_t k) const { return shots[k % RING]; }

  // Level of the newest <= 6 kept shots in [from, to], skipping one.
  Level levelOf(uint32_t from, uint32_t to, uint32_t skip = NO_SHOT) const {
    Level l;
    int32_t keys[HINT_LEVEL_SHOTS];
    for (uint32_t k = to + 1; k-- > from && k + RING >= pushed && l.n < HINT_LEVEL_SHOTS;) {
      if (k == skip) continue;
      keys[l.n++] = at(k).key;
      if (at(k).key >= HINT_CENSORED_KEY) ++l.censored;
      l.atLeast = l.atLeast || at(k).clamped || at(k).key >= HINT_CENSORED_KEY;
    }
    if (l.n > 0) l.ms = trimmedLevelMs(keys, l.n);
    return l;
  }

  // A new segment from shot first on: the advice and its arming end there.
  void startSegment(uint32_t first) {
    for (uint32_t k = first + RING < pushed ? pushed - RING : first; k < pushed; ++k)
      shots[k % RING].segment = first;
    segmentStart = first;
    candidate = NO_SHOT;
    hint = advised = HintDirection::NONE;
    segmentCounted = armedPending = false;
    unarmedSide = 0;
    armedLeft = repeats = 0;
  }

  // Change points (design 6.2) from the new shot's residual against the
  // segment, leaving out a pending first of a pair.
  void detectChange(int32_t arrivalMs) {
    const uint32_t k = pushed;
    const Level ref = levelOf(segmentStart, k - 1, armedPending || unarmedSide ? k - 1 : NO_SHOT);
    if (ref.n == 0) return;
    const int32_t r = static_cast<int32_t>(static_cast<int64_t>(arrivalMs - ref.ms) * 1000000 /
                                           (static_cast<int64_t>(sigmaMs) * HINT_G[ref.n - 1]));
    // Finer makes shots slower.
    const int32_t advisedR = advised == HintDirection::FINER ? r
                           : advised == HintDirection::COARSER ? -r : 0;
    if (hint != HintDirection::NONE) movedPermille = advisedR;
    if (armedLeft > 0 || movedPermille >= R_MOVED) {
      if (armedLeft > 0 && advisedR >= R_SINGLE) {
        startSegment(k);
        return;
      }
      if (armedPending && advisedR >= R_MOVED) {
        startSegment(k - 1);
        return;
      }
      armedPending = advisedR >= R_MOVED;
      armedLeft = static_cast<uint8_t>(armedLeft > 1 ? armedLeft - 1 : armedPending ? 1 : 0);
    }
    const int8_t side = ref.n < 4 ? 0 : r >= R_UNARMED ? 1 : r <= -R_UNARMED ? -1 : 0;
    if (side != 0 && side == unarmedSide) {
      startSegment(k - 1);
      return;
    }
    unarmedSide = side;
  }

  // sigma_use = max(1 s, sigmaLong, sigma_recent) (design 4.2).
  void updateSigma() {
    if (!segmentCounted) {
      // sigma_seg: differences of consecutive uncensored shots up to the
      // segment's fifth uncensored one.
      int32_t differences[HINT_SIGMA_SEGMENT_SHOTS];
      uint8_t n = 0, shots = 0;
      for (uint32_t k = segmentStart; k < pushed && shots < HINT_SIGMA_SEGMENT_SHOTS; ++k) {
        if (k + RING < pushed || at(k).key >= HINT_CENSORED_KEY) continue;
        if (shots++ > 0 && at(k - 1).key < HINT_CENSORED_KEY)
          differences[n++] = at(k).key - at(k - 1).key;
      }
      if (shots == HINT_SIGMA_SEGMENT_SHOTS) {
        const int32_t segment = differenceSigmaMs(differences, n);
        sigmaLongMs = sigmaSeeded ? (3 * sigmaLongMs + segment) / 4 : segment;
        if (sigmaLongMs > HINT_SIGMA_CAP_MS) sigmaLongMs = HINT_SIGMA_CAP_MS;
        sigmaSeeded = segmentCounted = true;
      }
    }
    // sigma_recent: differences of consecutive uncensored shots of one
    // segment of >= 3, over the newest 10 shots.
    int32_t differences[HINT_SIGMA_SHOTS];
    uint8_t n = 0;
    uint32_t end = pushed;
    for (uint32_t k = pushed; k-- > 1 && k + HINT_SIGMA_SHOTS > pushed;) {
      const Shot &a = at(k - 1), &b = at(k);
      if (k + 1 < pushed && at(k + 1).segment != b.segment) end = k + 1;
      if (a.segment == b.segment && end - b.segment >= 3 && a.key < HINT_CENSORED_KEY &&
          b.key < HINT_CENSORED_KEY)
        differences[n++] = b.key - a.key;
    }
    const int32_t recent = n >= HINT_SIGMA_MIN_DIFFERENCES ? differenceSigmaMs(differences, n) : 0;
    sigmaMs = sigmaLongMs > HINT_SIGMA_FLOOR_MS ? sigmaLongMs : HINT_SIGMA_FLOOR_MS;
    if (recent > sigmaMs) sigmaMs = recent;
  }

  HintDecision decide(const Level &level, int32_t lastThreeMs) const {
    HintEvidence e;
    e.dialIn = dialIn;
    e.n = level.n;
    e.censored = level.censored;
    e.levelMs = level.ms;
    e.lastThreeMs = lastThreeMs;
    e.sigmaMs = sigmaMs;
    e.sigmaLongMs = sigmaLongMs;
    e.advised = advised;
    return hintDecision(e, scope);
  }

  void push(const ArrivalRecord &r) {
    const int32_t arrival = hintArrivalMs(r);
    movedPermille = 0;
    if (pushed == 0 || (r.recordId >= scope.epoch && at(pushed - 1).id < scope.epoch)) {
      if (r.recordId >= scope.epoch) epochStart = pushed;
      startSegment(pushed);
      dialIn = true;
      dialInShots = 0;
    } else {
      detectChange(arrival);
    }
    shots[pushed % RING] = {r.recordId, segmentStart,
                            censoredArrival(r) ? arrival + HINT_CENSORED_KEY : arrival,
                            clampedArrival(r)};
    ++pushed;
    updateSigma();
    // Reversal (design 6.3): the shots since the latest announcement alone,
    // confidently on the other side, start a segment at the first of them.
    if (advised != HintDirection::NONE && candidate != NO_SHOT && pushed - candidate >= 3 &&
        candidate + RING >= pushed) {
      const HintDecision post = decide(levelOf(candidate, pushed - 1), 0);
      if (post.direction != advised && post.z >= Z_CONFIDENT) startSegment(candidate);
    }
    evaluate(r.recordId);
  }

  // Lifecycle (design 7): announce on reaching the phase's tier, again on a
  // higher tier or bin and once after 3 unmoved shots; stand while z >= 0;
  // never on a shot that moved the advised way, which shows "checking".
  void evaluate(uint32_t id) {
    const uint32_t last = pushed - 1, segmentShots = pushed - segmentStart;
    if (segmentStart + RING >= pushed) segmentStartId = at(segmentStart).id;
    const Level level = levelOf(segmentStart, last);
    const HintDecision d = decide(level, segmentShots >= 3 ? levelOf(last - 2, last).ms : 0);
    const bool moved = movedPermille >= R_MOVED;
    const bool checking = hint != HintDirection::NONE && moved;
    if (hint != HintDirection::NONE && !checking && (d.direction != hint || d.z < 0))
      hint = HintDirection::NONE;
    if (sinceAnnounce < UINT8_MAX) ++sinceAnnounce;
    if (d.tier != HintTier::NONE && !moved) {
      const bool fresh = hint == HintDirection::NONE;
      const bool escalated = !fresh && (d.tier > announcedTier || d.bin > announcedBin);
      const bool repeat = !fresh && !escalated && sinceAnnounce >= HINT_REPEAT_SHOTS &&
                          d.tier >= HintTier::LIKELY && repeats == 0;
      if (fresh || escalated || repeat) {
        if (repeat) ++repeats;
        if (fresh || d.tier > announcedTier) announcedTier = d.tier;
        if (fresh || d.bin > announcedBin) announcedBin = d.bin;
        hint = advised = d.direction;
        armedLeft = CP_ARMED_SHOTS;
        armedPending = false;
        sinceAnnounce = 0;
        candidate = pushed;
        announceId = id;
      }
    }
    if (dialInShots < UINT8_MAX) ++dialInShots;
    const int32_t c = (scope.fastBeforeMs + scope.slowAfterMs) / 2;
    const int32_t h = (scope.slowAfterMs - scope.fastBeforeMs) / 2;
    out = GrindHintV2{};
    if (epochStart != NO_SHOT) {
      out.n = level.n;
      out.atLeast = level.atLeast;
      out.offsetMs = level.ms - c;
      out.bandMs = bandHalfMs(h, sigmaMs);
      out.segmentStartId = segmentStartId;
      if (hint != HintDirection::NONE) {
        out.state = checking ? HintState::CHECKING
                  : dialIn   ? HintState::DIAL_IN
                             : HintState::MAINTENANCE;
        out.direction = hint;
        out.tier = HintTier::TENTATIVE;
        if (d.direction == hint) {
          if (level.n >= 3 && tierOf(d.z) > out.tier) out.tier = tierOf(d.z);
          if (d.tier > out.tier) out.tier = d.tier;
        }
        out.bin = d.bin;
      } else if (hintVaries(sigmaLongMs, h) && segmentShots >= HINT_VARY_SHOTS) {
        out.state = HintState::VARY;
      }
    }
    const int32_t fromMiddle = level.ms > c ? level.ms - c : c - level.ms;
    if (dialIn && (dialInShots >= HINT_DIAL_IN_SHOTS ||
                   (segmentShots >= 3 && fromMiddle <= bandHalfMs(h, sigmaMs))))
      dialIn = false;
  }
};

// The preset's grind hint: its shots replayed oldest first (rowAt in log
// order; a row not newer than the previous kept one is skipped).
template <typename RowAt>
inline GrindHintV2 grindHintOf(size_t count, RowAt rowAt, const StatsScope &scope) {
  GrindHintReplay replay(scope);
  uint32_t newest = 0;
  for (size_t i = 0; i < count; ++i) {
    const ArrivalRecord r = rowAt(i);
    if (!hintAdmissible(r, scope) || (replay.pushed > 0 && r.recordId <= newest)) continue;
    newest = r.recordId;
    replay.push(r);
  }
  return replay.out;
}

struct LossInputs {
  int32_t nowMs = 0;
  int32_t lastSampleMs = 0;  // t_p
  uint32_t fixedMs = 0;      // 0 = none
  bool beforeFirstDrops = false;
  bool crossingValid = false;
  int32_t crossingMs = 0;    // t* from the last accepted sample
  ArrivalStats history;
};

struct LossDeadline {
  int32_t deadlineMs = 0;
  LossSource source = LossSource::WINDOW;
  bool cutNow = false;
};

// Per-shot kernel state owned by the control task.
struct CorridorState {
  Params params;
  RateEstimator rate;
  int32_t tauMs = 0;           // sensor lag + stop latency (+ stop pulse)
  int32_t offsetDg = 0;        // learned drip offset r
  bool crossingValid = false;  // t* from the last accepted sample
  int32_t crossingMs = 0;
  int32_t stopMs = 0;          // the weight cut that fired: shot time and line
  int32_t stopLineDg = 0;
  ArrivalStats history;        // this preset's recent arrivals (shot start)
};
static_assert(sizeof(CorridorState) <= 512, "corridor state RAM budget");

// Scale-loss deadline, first applicable: fixed time, R0 cut now, R1 projection,
// R2 history, R3 window midpoint; clamped to [max(t_p, delay), wall].
inline LossDeadline lossDeadline(const Params &p, const LossInputs &in) {
  LossDeadline out;
  const bool project = !in.beforeFirstDrops && in.crossingValid;
  if (in.fixedMs != 0) {
    out.source = LossSource::FIXED;
    out.deadlineMs = static_cast<int32_t>(in.fixedMs);
  } else if (project && in.crossingMs <= in.nowMs) {
    out.source = LossSource::PROJECTION;
    out.deadlineMs = in.nowMs;
  } else if (project && in.crossingMs - in.lastSampleMs <= PROJECTION_HORIZON_MS) {
    out.source = LossSource::PROJECTION;
    out.deadlineMs = in.crossingMs;
  } else if (in.history.n >= SCALE_LOSS_MIN_SHOTS) {
    out.source = LossSource::HISTORY;
    const int32_t fromHistory =
        in.history.medianMs + 2 * in.history.sigmaMs / 3 + HISTORY_MARGIN_MS;
    out.deadlineMs = fromHistory < p.slowAfterMs ? fromHistory : p.slowAfterMs;
  } else {
    out.source = LossSource::WINDOW;
    out.deadlineMs = (p.fastBeforeMs + p.slowAfterMs) / 2;
  }
  const int32_t floor = in.lastSampleMs > p.delayMs ? in.lastSampleMs : p.delayMs;
  if (out.deadlineMs < floor) out.deadlineMs = floor;
  if (out.deadlineMs > p.wallMs) out.deadlineMs = p.wallMs;
  out.cutNow = out.deadlineMs <= in.nowMs;
  return out;
}

}  // namespace corridor

// Timed-law sentinel (s): past every wall, so only the direct backstop can cut.
constexpr float NO_CUT_PREDICTION_S = HARD_MAX_CIRCUIT_CLOSED_MS / 1000.0f + 1.0f;

}  // namespace openbrewbyweight

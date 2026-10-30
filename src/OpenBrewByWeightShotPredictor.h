#pragma once

// Shot Arrival Predictor (SAP) — read-only live prediction of how the shot
// will end, consumed by the Web UI hero tint and any future consumer
// (projected curve, projected-time indicator). It never actuates: the stop
// command path (corridor::crossingMs in the brew control) stays untouched.
//
// Units follow the corridor kernel: weights in dg, times in ms, rates in
// rateX100 units (dg/s * 100 == g/s * 1000), acceleration in rateX100 per
// second (g/s^2 * 1000). Products go through int64. The design and frozen
// tunables are validated in temp/ai_temp_hero_forecast_backtest
// (SPEC_v4.md, BLEND_FREEZE.md); this header is that spec in integer form.

#include "OpenBrewByWeightCorridor.h"

#include <stdint.h>

namespace openbrewbyweight {

// Hero tint class streamed to the Web UI ("tintClass" JSON field).
enum class ShotTint : uint8_t { NEUTRAL = 0, FAST = 1, ON_RECIPE = 2, SLOW = 3 };
// Hero note badge streamed alongside the tint ("tintNote").
enum class ShotTintNote : uint8_t { NONE = 0, CHANNELING = 1, RECOVERED = 2, RETARE = 3, STALLED = 4 };

inline const char *shotTintJson(ShotTint tint) {
  switch (tint) {
    case ShotTint::FAST: return "fast";
    case ShotTint::ON_RECIPE: return "on_recipe";
    case ShotTint::SLOW: return "slow";
    case ShotTint::NEUTRAL: break;
  }
  return "neutral";
}
inline const char *shotTintNoteJson(ShotTintNote note) {
  switch (note) {
    case ShotTintNote::CHANNELING: return "channeling";
    case ShotTintNote::RECOVERED: return "recovered";
    case ShotTintNote::RETARE: return "retare";
    case ShotTintNote::STALLED: return "stalled";
    case ShotTintNote::NONE: break;
  }
  return "none";
}

namespace predictor {

// Frozen tunables (BLEND_FREEZE.md; sweeps validated there).
constexpr int32_t A_CLAMP_LO_X100 = -50;       // -0.05 g/s^2
constexpr int32_t A_CLAMP_HI_X100 = 150;       // +0.15 g/s^2
constexpr int32_t LEVER_CAP_MS = 2500;
constexpr int32_t MONO_GUARD_DG = 5;           // 0.5 g
constexpr int32_t MONO_SPIKE_DG = 15;          // 1.5 g single-sample artifact
constexpr int32_t YOUNG_SPAN_MS = 2000;
constexpr int32_t PLATEAU_MIN_MS = 10000;      // absolute floor (bloom guard)
constexpr int32_t V_FLOOR_DGPS = 2;            // 0.2 g/s
constexpr int32_t SIGMA_DELTA_DGPS_X10 = 6;    // 0.06 g/s, dg/s x10
constexpr int32_t CENSORED_SIGMA_DIV = 4;
constexpr uint8_t PRIOR_MIN_N = 2;
constexpr int32_t STALENESS_Z_PERMILLE = 2500;
constexpr int32_t DB_R_MS = 500, DB_G_MS = 500, DB_B_MS = 1000;
constexpr int32_t T_LOCK_MS = 5000;
constexpr int32_t K_LO_PERMILLE = 500, K_HI_PERMILLE = 900;
constexpr int32_t SLEW_UP_PCT_S = 25, SLEW_EVIDENCE_PCT_S = 45;
constexpr int32_t SLEW_DOWN_PCT_S = 100;
constexpr int32_t WALL_WATCH_MS = 1000;        // non-censored t* near the wall
constexpr int32_t QP_GATE_MARGIN_MS = 500;
constexpr int32_t RATE_STEP_RATIO_X100 = 150;  // 1.5x within 2 s
constexpr int32_t RATE_STEP_WINDOW_MS = 2000;
constexpr int32_t RATE_STEP_MIN_BASE_X100 = 100;  // ignore steps off tiny rates
constexpr int32_t RECOVERY_NOTE_MS = 3000;

// floor(sqrt(v)) for v >= 0, integer Newton (converges from above).
inline int32_t isqrt64(int64_t v) {
  if (v <= 0) return 0;
  int64_t x = v;
  int64_t y = (x + 1) / 2;
  while (y < x) {
    x = y;
    y = (x + v / x) / 2;
  }
  return static_cast<int32_t>(x);
}

// Standard normal CDF in permille: table at z = 0.0..4.0 step 0.1 with
// linear interpolation (max error 0.65 permille, asserted in host tests);
// |z| >= 4 saturates. zMilli is z * 1000.
constexpr uint16_t PHI_PERMILLE[41] = {
    500, 540, 579, 618, 655, 691, 726, 758, 788, 816, 841, 864, 885, 903,
    919, 933, 945, 955, 964, 971, 977, 982, 986, 989, 992, 994, 995, 997,
    997, 998, 999, 999, 999, 1000, 1000, 1000, 1000, 1000, 1000, 1000,
    1000};
inline int32_t phiPermille(int32_t zMilli) {
  if (zMilli <= -4000) return 0;
  if (zMilli >= 4000) return 1000;
  const bool neg = zMilli < 0;
  if (neg) zMilli = -zMilli;
  const int32_t idx = zMilli / 100;            // 0..39
  const int32_t frac = zMilli - idx * 100;     // 0..99
  const int32_t lo = PHI_PERMILLE[idx];
  const int32_t hi = PHI_PERMILLE[idx + 1];
  const int32_t out = lo + (hi - lo) * frac / 100;
  return neg ? 1000 - out : out;
}

// Per-shot predictor state, owned beside corridor::CorridorState in the
// cycle session; reset at shot start by armShotPredictor.
struct ShotPredictor {
  // Guards and phase tracking.
  int32_t maxWDg = INT32_MIN;
  int32_t prevWDg = INT32_MIN;  // previous sample (spike guard)
  uint32_t guardOkSinceMs = 0;   // 0 = disturbance pending
  uint32_t lowASinceMs = 0;      // |a_raw| <= 50 streak (rate units per s)
  uint32_t lowQSinceMs = 0;      // rate-collapse streak
  uint32_t lastStepAtMs = 0;
  uint32_t blueSinceMs = 0;      // tint class == SLOW since (reversal note)
  uint32_t noteUntilMs = 0;      // RECOVERED note display window
  bool stalledCandidate = false;  // blue candidate came from the censored path
  int32_t aX100 = 0, aRawX100 = 0;
  bool priorSuspended = false;
  uint32_t lastSampleMs = 0;  // uptime of the last fed sample (slew dt)
  // Tint outputs (streamed); pct slews per BLEND_FREEZE (0..100).
  uint8_t tintPct = 0;
  ShotTint tintClass = ShotTint::NEUTRAL;
  ShotTintNote tintNote = ShotTintNote::NONE;
  // Latched live (mean-basis, acceleration-aware) crossing t* for display
  // consumers; -1 = no usable forecast this sample (young, censored, no rate).
  int32_t fcTms = -1;
};

// Inputs assembled by the brew integration per accepted scale sample.
struct PredictorInputs {
  const corridor::CorridorState *cut = nullptr;  // rate, params, history
  int32_t nowMs = 0;                // shot clock at the sample
  int32_t weightDg = 0;
  int32_t firstDropMs = 0;          // 0 = none yet
  int32_t qPX100 = 0;               // preset plateau flow; 0 = absent
  bool autoMode = false;            // BBW automatic shot
  bool scaleOk = false;
  bool retarePending = false;
  int32_t sampleDtMs = 100;         // slew integration step
};

inline void armShotPredictor(ShotPredictor &p) { p = ShotPredictor{}; }

// Raw rate ~ageMs behind now within tolerance; NO_RATE if none fits.
// When atMs is given it receives the matched sample's time (for dt).
inline int32_t rawRateAt(const corridor::RateEstimator &rate, int32_t nowMs,
                         int32_t ageMs, int32_t toleranceMs,
                         int32_t *atMs = nullptr) {
  const int32_t target = nowMs - ageMs;
  int32_t best = corridor::RateEstimator::NO_RATE;
  int32_t bestDist = INT32_MAX;
  int32_t bestAt = 0;
  for (uint8_t age = 0; age < rate.count; ++age) {
    const uint8_t i = rate.index(age);
    if (rate.rawRateX100[i] == corridor::RateEstimator::NO_RATE) continue;
    const int32_t d = rate.atMs[i] - target;
    const int32_t dist = d < 0 ? -d : d;
    if (dist <= toleranceMs && dist < bestDist) {
      best = rate.rawRateX100[i];
      bestAt = rate.atMs[i];
      bestDist = dist;
    }
  }
  if (atMs != nullptr) *atMs = bestAt;
  return best;
}

// sigma_W in dg from the estimator's innovation ring (matches the MAD gate).
inline int32_t sigmaWDg(const corridor::RateEstimator &rate) {
  if (rate.count < 2) return 3;
  int32_t values[corridor::RateEstimator::CAPACITY];
  uint8_t n = 0;
  for (uint8_t age = 0; age + 1 < rate.count; ++age)
    values[n++] = rate.innovationDg[rate.index(age)];
  if (n == 0) return 3;
  corridor::sortSmall(values, n);
  const int32_t med = corridor::medianOfSorted(values, n);
  const int32_t mad = corridor::madOf(values, n, med);
  const int32_t s = static_cast<int32_t>(static_cast<int64_t>(mad) * 14826 / 10000);
  return s < 3 ? 3 : s;
}

// Slope variance in (dg/ms)^2 scaled by 1e9, from the estimator's own LS
// window: Var = sigma_W^2 * n^2 / sxx (same algebra as fit()). sw is
// sigmaWDg(rate), which the caller already holds.
inline int64_t varQdgMs2X1e9(const corridor::RateEstimator &rate, int64_t sw) {
  if (rate.count < 2) return 0;
  int64_t sumT = 0;
  for (uint8_t age = 0; age < rate.count; ++age)
    sumT += rate.atMs[rate.index(age)];
  int64_t sxx = 0;
  for (uint8_t age = 0; age < rate.count; ++age) {
    const int64_t dt =
        static_cast<int64_t>(rate.atMs[rate.index(age)]) * rate.count - sumT;
    sxx += dt * dt;
  }
  if (sxx <= 0) return 0;
  return sw * sw * rate.count * rate.count * 1000000000LL / sxx;
}

// Cut-line slope f'(t) in dg/s (central difference over 100 ms).
inline int32_t cutLineSlopeDgS(const corridor::Params &p, int32_t tMs) {
  const int32_t hi = corridor::cutLineDg(p, tMs + 50);
  const int32_t lo = corridor::cutLineDg(p, tMs > 50 ? tMs - 50 : 0);
  return (hi - lo) * 10;
}

struct Forecast {
  int32_t tMs = -1;     // shot-clock crossing; -1 = no usable forecast
  int32_t sigmaMs = 0;
  bool censored = false;  // line never met before the wall (tMs = wall)
};

// One crossing forecast. basis 0 = mean (acceleration-aware), 1 = raw.
// aX100 is the clamped acceleration in rateX100/s; aRawX100 unclamped.
inline Forecast forecastBasis(const corridor::CorridorState &cut, int32_t nowMs,
                              int32_t weightDg, int basis, int32_t aX100,
                              int32_t aRawX100) {
  const corridor::RateEstimator &rate = cut.rate;
  const corridor::Params &p = cut.params;
  Forecast out;
  if (!rate.valid || rate.rateX100 < corridor::MIN_PREDICTION_RATE_X100)
    return out;
  int32_t tPlain = 0;
  if (!corridor::crossingMs(p, nowMs, weightDg, rate.rateX100, cut.tauMs,
                            cut.offsetDg, tPlain))
    return out;
  if (tPlain >= p.wallMs) {
    out.censored = true;
    out.tMs = p.wallMs;
    out.sigmaMs = (p.wallMs - nowMs) / CENSORED_SIGMA_DIV;
    return out;
  }
  int32_t qBasis = rate.rateX100;
  int32_t tStar = tPlain;
  const int32_t floorStart = corridor::slowRampEndMs(p);
  const bool goalFlat =
      p.fastBeforeMs <= tPlain &&
      (tPlain < p.slowAfterMs || (tPlain == p.slowAfterMs && p.slowRampMs > 0));
  const bool floorFlat = tPlain >= floorStart;
  if (basis == 0 && aX100 != 0 && (goalFlat || floorFlat)) {
    // Closed-form quadratic on the flat level: with qc = q + a*T_w/2 and
    // lead qc*tau, solve a*h^2 + 2000*qc*h - 2e8*D = 0 (h in ms; the mass
    // (qc/1e5)*h + (a/2e8)*h^2 is in dg, matching leadDg's scaling).
    const int32_t level = goalFlat ? p.goalDg : corridor::floorDg(p);
    const int32_t qc = qBasis + aX100 * 3 / 2;  // a * T_w/2, T_w = 3 s
    if (qc > 0) {
      const int64_t lead = static_cast<int64_t>(qc) * cut.tauMs / 100000;
      const int64_t D = static_cast<int64_t>(level) - cut.offsetDg - weightDg - lead;
      if (D <= 0) {
        tStar = nowMs;
        qBasis = qc;
      } else {
        const int64_t b = static_cast<int64_t>(qc) * 2000;
        const int64_t disc = b * b + 800000000LL * aX100 * D;
        if (disc < 0) {
          // Decelerating past the level: the projection never arrives.
          out.censored = true;
          out.tMs = p.wallMs;
          out.sigmaMs = (p.wallMs - nowMs) / CENSORED_SIGMA_DIV;
          return out;
        }
        const int64_t root = isqrt64(disc);
        const int64_t h = (root - b) / (2 * static_cast<int64_t>(aX100));
        const int32_t hMs = static_cast<int32_t>(
            h < 0 ? 0 : (h > 200000 ? 200000 : h));
        tStar = nowMs + hMs;
        qBasis = qc;
        if (tStar >= p.wallMs) {
          out.censored = true;
          out.tMs = p.wallMs;
          out.sigmaMs = (p.wallMs - nowMs) / CENSORED_SIGMA_DIV;
          return out;
        }
      }
    }
  } else if (basis == 0 && aX100 != 0) {
    // Ramp landing: one capped-lever pass (q_eff = q + a*min(T_w/2+h0/2, cap)).
    const int32_t h0 = tPlain - nowMs;
    const int32_t lever = 1500 + h0 / 2 < LEVER_CAP_MS ? 1500 + h0 / 2
                                                       : LEVER_CAP_MS;
    int32_t qEff = qBasis + static_cast<int64_t>(aX100) * lever / 1000;
    if (qEff > corridor::MAX_RATE_X100) qEff = corridor::MAX_RATE_X100;
    int32_t tEff = 0;
    if (qEff < corridor::MIN_PREDICTION_RATE_X100 ||
        !corridor::crossingMs(p, nowMs, weightDg, qEff, cut.tauMs,
                              cut.offsetDg, tEff) ||
        tEff >= p.wallMs) {
      // A deceleration that collapses the projected rate never arrives.
      out.censored = true;
      out.tMs = p.wallMs;
      out.sigmaMs = (p.wallMs - nowMs) / CENSORED_SIGMA_DIV;
      return out;
    }
    tStar = tEff;
    qBasis = qEff;
  }
  // sigma_t from the mass-variance identity, in dg^2 x100 then to ms.
  const int32_t h = tStar > nowMs ? tStar - nowMs : 0;
  int32_t v = qBasis / 100 - cutLineSlopeDgS(p, tStar);  // dg/s
  if (v < V_FLOOR_DGPS) v = V_FLOOR_DGPS;  // only the magnitude matters
  const int32_t swDg = sigmaWDg(rate);
  const int64_t varQ = varQdgMs2X1e9(rate, swDg);
  int64_t t100 = static_cast<int64_t>(swDg) * swDg * 100;
  const int64_t span = static_cast<int64_t>(h) + cut.tauMs;
  t100 += span * span * varQ / 1000000000LL * 100;
  // Drift term: (sigma_Delta^2 / 3) * h_s^3 in dg^2, x100.
  t100 += static_cast<int64_t>(SIGMA_DELTA_DGPS_X10) *
          SIGMA_DELTA_DGPS_X10 * h * h * h / 1000000000LL / 3;
  // Declining-flow bias (F3): a_raw [g/s^2] * h_s^2 / 2 in grams, i.e.
  // aRawX100 * h_ms^2 / 2e8 in dg (x100 in the variance units).
  if (aRawX100 < -20 && h > 0) {
    const int64_t bias = -static_cast<int64_t>(aRawX100) * h * h / 200000000LL;
    t100 += bias * bias * 100;
  }
  const int64_t sigma2Ms =
      1000000LL * (t100 / 100) / (static_cast<int64_t>(v) * v);
  out.sigmaMs = static_cast<int32_t>(isqrt64(sigma2Ms < 1 ? 1 : sigma2Ms));
  if (out.sigmaMs > 20000) out.sigmaMs = 20000;
  out.tMs = tStar;
  return out;
}

// Per-sample update: refreshes guards, phase, forecast, prior fusion, the
// guarded tint candidates and the slew-limited blend (BLEND_FREEZE.md).
inline void updateShotPredictor(ShotPredictor &pr, const PredictorInputs &in) {
  const corridor::CorridorState &cut = *in.cut;
  const corridor::RateEstimator &rate = cut.rate;
  const corridor::Params &p = cut.params;
  const int32_t now = in.nowMs;

  // --- Guards and phase ----------------------------------------------------
  // Mass cannot jump: a >1.5 g single-sample step is a scale artifact in
  // either direction (max physical flow is 0.4 g per 10 Hz sample).
  const bool spike =
      pr.prevWDg != INT32_MIN && in.weightDg - pr.prevWDg > MONO_SPIKE_DG;
  if (spike)
    pr.guardOkSinceMs = 0;  // artifact: young until 2 s clean
  else if (in.weightDg > pr.maxWDg)
    pr.maxWDg = in.weightDg;
  if (in.weightDg < pr.maxWDg - MONO_GUARD_DG) {
    pr.guardOkSinceMs = 0;  // disturbance: young until 2 s clean
  } else if (pr.guardOkSinceMs == 0 && !spike) {
    pr.guardOkSinceMs = static_cast<uint32_t>(now);
  }
  pr.prevWDg = in.weightDg;
  int32_t rawOldAt = 0;
  const int32_t rawOld = rawRateAt(rate, now, 1500, 400, &rawOldAt);
  pr.aRawX100 = (rawOld == corridor::RateEstimator::NO_RATE || !rate.valid ||
                 now - rawOldAt <= 0)
                    ? 0
                    : static_cast<int32_t>(
                          (static_cast<int64_t>(rate.rawX100 - rawOld) * 1000) /
                          (now - rawOldAt));
  pr.aX100 = pr.aRawX100 < A_CLAMP_LO_X100 ? A_CLAMP_LO_X100
            : pr.aRawX100 > A_CLAMP_HI_X100 ? A_CLAMP_HI_X100
                                            : pr.aRawX100;
  if (rate.valid) {
    const int32_t q2 = rawRateAt(rate, now, RATE_STEP_WINDOW_MS, 400);
    if (q2 != corridor::RateEstimator::NO_RATE && q2 >= RATE_STEP_MIN_BASE_X100 &&
        rate.rawX100 >= static_cast<int64_t>(q2) * RATE_STEP_RATIO_X100 / 100)
      pr.lastStepAtMs = static_cast<uint32_t>(now);
  }
  if (pr.aRawX100 <= 50 && pr.aRawX100 >= -50) {
    if (pr.lowASinceMs == 0) pr.lowASinceMs = static_cast<uint32_t>(now);
  } else {
    pr.lowASinceMs = 0;
  }
  const bool qOk = rate.valid && rate.rateX100 >= corridor::MIN_PREDICTION_RATE_X100;
  if (!qOk) {
    if (pr.lowQSinceMs == 0) pr.lowQSinceMs = static_cast<uint32_t>(now);
  } else {
    pr.lowQSinceMs = 0;
  }
  const bool plateau =
      in.firstDropMs > 0 &&
      (now >= in.firstDropMs + 8000 ||
       (pr.lowASinceMs != 0 && now - pr.lowASinceMs >= 3000 &&
        now >= in.firstDropMs + 4000)) &&
      now >= PLATEAU_MIN_MS;  // F11 absolute floor on both branches
  const bool estimatorYoung =
      rate.count < corridor::RATE_MIN_SAMPLES || !rate.valid;
  const bool young = in.firstDropMs == 0 || !in.autoMode || !in.scaleOk ||
                     in.retarePending || estimatorYoung ||
                     pr.guardOkSinceMs == 0 || now - pr.guardOkSinceMs < YOUNG_SPAN_MS;
  const bool stepRecent =
      pr.lastStepAtMs != 0 && now - pr.lastStepAtMs < RATE_STEP_WINDOW_MS;

  // --- Forecast + prior fusion ---------------------------------------------
  const Forecast mean =
      qOk ? forecastBasis(cut, now, in.weightDg, 0, pr.aX100, pr.aRawX100)
          : Forecast{};
  const bool collapse = !qOk && plateau && pr.lowQSinceMs != 0 &&
                        now - pr.lowQSinceMs >= 3000;
  Forecast live = mean;
  if (!qOk && collapse) {
    live.censored = true;
    live.tMs = p.wallMs;
    live.sigmaMs = (p.wallMs - now) / CENSORED_SIGMA_DIV;
  }
  pr.fcTms = !live.censored && live.tMs >= 0 && live.tMs < p.wallMs ? live.tMs : -1;
  const Forecast red = qOk ? forecastBasis(cut, now, in.weightDg, 1, 0, 0)
                           : Forecast{};
  const corridor::ArrivalStats &h = cut.history;
  int32_t tHat = live.tMs, sigmaF = live.sigmaMs;
  if (!live.censored && live.tMs >= 0 && h.n >= PRIOR_MIN_N &&
      !pr.priorSuspended && plateau) {
    const int32_t g = corridor::HINT_G[h.n < corridor::HINT_LEVEL_SHOTS
                                           ? h.n - 1
                                           : corridor::HINT_LEVEL_SHOTS - 1];
    const int32_t sP = h.sigmaMs > 750 ? h.sigmaMs : 750;
    const int64_t sp = static_cast<int64_t>(sP) * g / 1000;
    const int64_t diff = tHat > h.medianMs ? tHat - h.medianMs
                                           : h.medianMs - tHat;
    if (diff * 1000 > STALENESS_Z_PERMILLE *
                          isqrt64(static_cast<int64_t>(sigmaF) * sigmaF +
                                  sp * sp)) {
      pr.priorSuspended = true;  // grind shift: live only for this shot
    } else {
      const int64_t a = static_cast<int64_t>(sigmaF) * sigmaF;
      const int64_t b = sp * sp;
      tHat = static_cast<int32_t>(
          (static_cast<int64_t>(tHat) * b + static_cast<int64_t>(h.medianMs) * a) /
          (a + b));
      sigmaF = static_cast<int32_t>(isqrt64(a * b / (a + b)));
    }
  }

  // --- Guarded tint candidates (BLEND_FREEZE) -------------------------------
  enum Cand : uint8_t { C_NONE, C_RED, C_GREEN, C_BLUE };
  uint8_t cand = C_NONE;
  int32_t k = 0, tBlend = live.tMs;
  const bool chan = rate.channeling;
  if (!young && !collapse && qOk && !live.censored && red.tMs >= 0 &&
      red.tMs < p.wallMs - WALL_WATCH_MS &&
      red.tMs + 2 * red.sigmaMs < p.fastBeforeMs - DB_R_MS && !chan &&
      !stepRecent) {
    cand = C_RED;
    k = phiPermille((p.fastBeforeMs - DB_R_MS - red.tMs) * 1000 /
                    (red.sigmaMs > 0 ? red.sigmaMs : 1));
    tBlend = red.tMs;
  } else if (!young && live.tMs >= 0 && !live.censored &&
             live.tMs < p.wallMs - WALL_WATCH_MS) {
    const int32_t lo = tHat - 2 * sigmaF, hi = tHat + 2 * sigmaF;
    tBlend = tHat;
    if (p.fastBeforeMs + DB_G_MS <= lo && hi <= p.slowAfterMs - DB_G_MS) {
      cand = C_GREEN;
      k = phiPermille((p.slowAfterMs - DB_G_MS - tHat) * 1000 /
                      (sigmaF > 0 ? sigmaF : 1)) -
           phiPermille((p.fastBeforeMs + DB_G_MS - tHat) * 1000 /
                       (sigmaF > 0 ? sigmaF : 1));
    } else if (lo > p.slowAfterMs + DB_B_MS && !chan) {
      cand = C_BLUE;
      k = 1000 - phiPermille((p.slowAfterMs + DB_B_MS - tHat) * 1000 /
                             (sigmaF > 0 ? sigmaF : 1));
    }
  } else if (!young && live.censored && plateau && !chan) {
    // Pre-plateau censoring is the ramp bias, not slow evidence; post-plateau
    // it feeds F10: censored blue needs the preset's plateau-flow gate.
    const bool gate = h.n >= 1 && in.qPX100 > 0 && !pr.priorSuspended;
    if (gate) {
      const int64_t lead =
          static_cast<int64_t>(rate.rateX100) * cut.tauMs / 100000;
      const int64_t mass = p.goalDg + cut.offsetDg + lead - in.weightDg;
      const int64_t arrivalAtQp = now + mass * 100000 / in.qPX100;
      if (arrivalAtQp > p.slowAfterMs + QP_GATE_MARGIN_MS) {
        cand = C_BLUE;
        k = 1000;
        tBlend = p.wallMs;
        sigmaF = live.sigmaMs;
        pr.stalledCandidate = true;
      }
    }
  }

  // --- Slew-limited blend ----------------------------------------------------
  const uint32_t dt = in.sampleDtMs > 0 ? in.sampleDtMs : 100;
  pr.stalledCandidate = cand == C_BLUE && live.censored;
  pr.tintNote = in.retarePending
                    ? ShotTintNote::RETARE
                    : (pr.stalledCandidate
                           ? ShotTintNote::STALLED
                           : (pr.noteUntilMs != 0 &&
                                      static_cast<uint32_t>(now) < pr.noteUntilMs
                                  ? ShotTintNote::RECOVERED
                                  : (chan ? ShotTintNote::CHANNELING
                                          : ShotTintNote::NONE)));
  if (cand == C_NONE) {
    if (young || (!qOk && !collapse)) {
      const int32_t down = SLEW_DOWN_PCT_S * dt / 1000;
      pr.tintPct = pr.tintPct > down ? pr.tintPct - down : 0;
      if (pr.tintPct == 0) pr.tintClass = ShotTint::NEUTRAL;
    }  // else: valid forecast outside every region — hold class and pct
    return;
  }
  // ramp toward the T-5 s anchor; acceleration only post-plateau.
  int32_t rampX = static_cast<int64_t>(now) * 1000 /
                  (tBlend - T_LOCK_MS > 1000 ? tBlend - T_LOCK_MS : 1000);
  if (rampX > 1000) rampX = 1000;
  if (rampX < 0) rampX = 0;
  int32_t kk = plateau ? (k - K_LO_PERMILLE) * 1000 /
                             (K_HI_PERMILLE - K_LO_PERMILLE)
                       : 0;
  int32_t accelX = 0;
  if (kk > 0) {
    if (kk >= 1000) {
      accelX = 1000;
    } else {
      accelX = kk * kk * (3000 - 2 * kk) / 1000000;  // smoothstep, x1000
    }
  }
  const int32_t targetX = rampX > accelX ? rampX : accelX;  // 0..1000
  const int32_t rise =
      (k >= K_HI_PERMILLE ? SLEW_EVIDENCE_PCT_S : SLEW_UP_PCT_S) * 10 * dt / 1000;
  int32_t pct = pr.tintPct * 10;
  if (targetX >= pct) {
    pct += rise;
    if (pct > targetX) pct = targetX;
  } else {
    pct -= SLEW_DOWN_PCT_S * 10 * dt / 1000;
    if (pct < targetX) pct = targetX;
  }
  if (pct > 1000) pct = 1000;
  if (pct < 0) pct = 0;
  pr.tintPct = static_cast<uint8_t>((pct + 5) / 10);
  const ShotTint newClass =
      cand == C_RED ? ShotTint::FAST
                    : cand == C_GREEN ? ShotTint::ON_RECIPE : ShotTint::SLOW;
  if (newClass == ShotTint::SLOW && pr.tintClass != ShotTint::SLOW)
    pr.blueSinceMs = static_cast<uint32_t>(now);
  if (pr.tintClass == ShotTint::SLOW && newClass == ShotTint::ON_RECIPE &&
      stepRecent && pr.blueSinceMs != 0)
    pr.noteUntilMs = static_cast<uint32_t>(now + RECOVERY_NOTE_MS);
  pr.tintClass = newClass;
}

}  // namespace predictor
}  // namespace openbrewbyweight

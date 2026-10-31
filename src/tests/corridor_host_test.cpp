#include "../OpenBrewByWeightBrewTypes.h"
#include "../OpenBrewByWeightCorridor.h"

#include <cJSON.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace openbrewbyweight::corridor;
using openbrewbyweight::EndReason;
using namespace openbrewbyweight;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      if (++failures <= 40)                                                  \
        std::cerr << __func__ << ":" << __LINE__ << ": check failed: "       \
                  << #condition << "\n";                                     \
    }                                                                        \
  } while (false)

cJSON *loadVectors(const char *name = "corridor_vectors_v1.json") {
  std::ifstream file(std::string(CORRIDOR_FIXTURE_DIR "/") + name);
  std::stringstream text;
  text << file.rdbuf();
  return cJSON_Parse(text.str().c_str());
}

int32_t num(const cJSON *object, const char *key) {
  return static_cast<int32_t>(cJSON_GetObjectItemCaseSensitive(object, key)->valuedouble);
}
bool flag(const cJSON *object, const char *key) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, key));
}

Params paramsOf(const cJSON *presets, const char *name) {
  const cJSON *p = cJSON_GetObjectItemCaseSensitive(presets, name);
  Params out;
  out.goalDg = num(p, "goalDg");
  out.fastMarginDg = num(p, "fastMarginDg");
  out.slowMarginDg = num(p, "slowMarginDg");
  out.fastOn = flag(p, "fastOn");
  out.slowOn = flag(p, "slowOn");
  out.delayMs = num(p, "delayMs");
  out.fastBeforeMs = num(p, "fastBeforeMs");
  out.slowAfterMs = num(p, "slowAfterMs");
  out.wallMs = num(p, "wallMs");
  out.fastRampMs = out.slowRampMs = num(p, "rampMs");
  return out;
}

// f bounded, non-increasing, exact at the knots and close to straight ramps.
void checkCutLineInvariants(const Params &p) {
  const int32_t ceiling = ceilingDg(p), floor = floorDg(p);
  CHECK(cutLineDg(p, p.fastBeforeMs) == p.goalDg);
  CHECK(cutLineDg(p, p.slowAfterMs) == (p.slowRampMs > 0 ? p.goalDg : floor));
  CHECK(cutLineDg(p, fastRampStartMs(p)) == (fastRampStartMs(p) < p.fastBeforeMs ? ceiling : p.goalDg));
  CHECK(cutLineDg(p, slowRampEndMs(p)) == floor);
  const int32_t a = fastRampStartMs(p), b = slowRampEndMs(p);
  const int32_t fastBound = static_cast<int32_t>(std::ceil(0.0963 * (ceiling - p.goalDg)));
  const int32_t slowBound = static_cast<int32_t>(std::ceil(0.0963 * (p.goalDg - floor)));
  int32_t previous = cutLineDg(p, p.delayMs);
  for (int32_t t = p.delayMs; t <= p.wallMs; ++t) {
    const int32_t f = cutLineDg(p, t);
    CHECK(f <= previous);
    CHECK(f <= ceiling && f >= floor);
    previous = f;
    if (t >= a && t < p.fastBeforeMs && p.fastBeforeMs > a) {
      const double straight = p.goalDg + (ceiling - p.goalDg) *
          static_cast<double>(p.fastBeforeMs - t) / (p.fastBeforeMs - a);
      CHECK(std::fabs(f - straight) <= fastBound + 1);
    }
    if (t > p.slowAfterMs && t < b) {
      const double straight = p.goalDg - (p.goalDg - floor) *
          static_cast<double>(t - p.slowAfterMs) / (b - p.slowAfterMs);
      CHECK(std::fabs(f - straight) <= slowBound + 1);
    }
    if (t >= p.fastBeforeMs && t < p.slowAfterMs) CHECK(f == p.goalDg);
  }
}

int32_t bruteCrossing(const Params &p, int32_t tk, int32_t w, int32_t q, int32_t tau,
                      int32_t r) {
  const int32_t lead = leadDg(q, tau);
  for (int32_t t = tk; t <= p.wallMs; ++t) {
    if (w + static_cast<int64_t>(q) * (t - tk) / 100000 + lead + r >= cutLineDg(p, t)) return t;
  }
  return p.wallMs;
}

void testVectors() {
  cJSON *root = loadVectors();
  CHECK(root != nullptr);
  if (root == nullptr) return;
  CHECK(num(root, "rampMs") == RAMP_MS);
  const cJSON *presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
  const cJSON *preset = nullptr;
  cJSON_ArrayForEach(preset, presets) checkCutLineInvariants(paramsOf(presets, preset->string));

  const cJSON *lines = cJSON_GetObjectItemCaseSensitive(root, "cutLine");
  const cJSON *line = nullptr;
  cJSON_ArrayForEach(line, lines) {
    const Params p = paramsOf(presets, line->string);
    const cJSON *point = nullptr;
    cJSON_ArrayForEach(point, line) {
      CHECK(cutLineDg(p, cJSON_GetArrayItem(point, 0)->valueint) ==
            cJSON_GetArrayItem(point, 1)->valueint);
    }
  }

  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "solver")) {
    const Params p = paramsOf(presets, cJSON_GetObjectItemCaseSensitive(item, "preset")->valuestring);
    int32_t out = -1;
    const bool found = crossingMs(p, num(item, "tkMs"), num(item, "weightDg"), num(item, "rateX100"),
                                  num(item, "tauMs"), num(item, "offsetDg"), out);
    const cJSON *expected = cJSON_GetObjectItemCaseSensitive(item, "crossingMs");
    CHECK(found == !cJSON_IsNull(expected));
    if (found) {
      CHECK(out == expected->valueint);
      CHECK(out == bruteCrossing(p, num(item, "tkMs"), num(item, "weightDg"), num(item, "rateX100"),
                                 num(item, "tauMs"), num(item, "offsetDg")));
    }
  }

  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "verdict")) {
    const Params p = paramsOf(presets, cJSON_GetObjectItemCaseSensitive(item, "preset")->valuestring);
    const ArrivalVerdict v = arrivalVerdict(p, num(item, "cutMs"), num(item, "rateX100"));
    CHECK(static_cast<int32_t>(v.verdict) == num(item, "verdict"));
    CHECK(v.arrivalMs == num(item, "arrivalMs"));
  }

  ArrivalRecord records[32];
  size_t count = 0;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "statsRecords")) {
    ArrivalRecord &r = records[count++];
    r.recordId = static_cast<uint32_t>(cJSON_GetArrayItem(item, 0)->valueint);
    r.presetId = static_cast<uint8_t>(cJSON_GetArrayItem(item, 1)->valueint);
    r.endReason = static_cast<uint8_t>(cJSON_GetArrayItem(item, 2)->valueint);
    r.verdict = static_cast<uint8_t>(cJSON_GetArrayItem(item, 3)->valueint);
    r.arrivalMs = cJSON_GetArrayItem(item, 4)->valueint;
  }
  // Order must not matter: also run over the reversed array.
  ArrivalRecord reversed[32];
  for (size_t i = 0; i < count; ++i) reversed[i] = records[count - 1 - i];
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "stats")) {
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(item, "expected");
    for (const ArrivalRecord *source : {records, reversed}) {
      const ArrivalStats s = arrivalStats(source, count, static_cast<uint8_t>(num(item, "presetId")),
                                          static_cast<uint32_t>(num(item, "epoch")));
      CHECK(s.n == num(e, "n"));
      CHECK(s.minMs == num(e, "minMs"));
      CHECK(s.maxMs == num(e, "maxMs"));
      CHECK(s.medianMs == num(e, "medianMs"));
      CHECK(s.sigmaMs == num(e, "sigmaMs"));
    }
  }

  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "deadline")) {
    const Params p = paramsOf(presets, cJSON_GetObjectItemCaseSensitive(item, "preset")->valuestring);
    LossInputs in;
    in.nowMs = num(item, "nowMs");
    in.lastSampleMs = num(item, "lastSampleMs");
    in.fixedMs = static_cast<uint32_t>(num(item, "fixedMs"));
    in.beforeFirstDrops = flag(item, "beforeFirstDrops");
    in.crossingValid = flag(item, "crossingValid");
    in.crossingMs = num(item, "crossingMs");
    in.history.n = static_cast<uint8_t>(num(item, "historyN"));
    in.history.medianMs = num(item, "historyMedianMs");
    in.history.sigmaMs = num(item, "historySigmaMs");
    const LossDeadline d = lossDeadline(p, in);
    const cJSON *e = cJSON_GetObjectItemCaseSensitive(item, "expected");
    CHECK(d.deadlineMs == num(e, "deadlineMs"));
    CHECK(static_cast<int32_t>(d.source) == num(e, "source"));
    CHECK(d.cutNow == flag(e, "cutNow"));
    CHECK(d.deadlineMs >= p.delayMs && d.deadlineMs <= p.wallMs);
  }
  cJSON_Delete(root);
}

// D = 0 oracle: with steps instead of ramps and the prediction lead excluded,
// the kernel reproduces every legacy Fast/Slow decision captured from the 1.x
// firmware harness (legacy_guard_decisions_v1.json). The legacy min-time (14)
// and max-time (15) stops map to the target (3) and lowered (16) segments.
void testLegacyOracle() {
  std::ifstream file(CORRIDOR_FIXTURE_DIR "/legacy_guard_decisions_v1.json");
  std::stringstream text;
  text << file.rdbuf();
  cJSON *root = cJSON_Parse(text.str().c_str());
  CHECK(root != nullptr);
  if (root == nullptr) return;
  int rows = 0;
  const cJSON *row = nullptr;
  cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(root, "rows")) {
    ++rows;
    Params p;
    p.goalDg = num(row, "goalDg");
    p.fastMarginDg = num(row, "fastMarginDg");
    p.slowMarginDg = num(row, "slowMarginDg");
    p.fastOn = flag(row, "fastOn");
    p.slowOn = flag(row, "slowOn");
    p.delayMs = num(row, "delayMs");
    p.fastBeforeMs = num(row, "fastBeforeMs");
    p.slowAfterMs = num(row, "slowAfterMs");
    p.wallMs = num(row, "wallMs");
    p.fastRampMs = p.slowRampMs = 0;
    const int32_t r = num(row, "offsetDg"), first = num(row, "firstDropMs"),
                  rate = num(row, "rateDgPerS"), change = num(row, "changeAtMs"),
                  rate2 = num(row, "rate2DgPerS"), period = num(row, "periodMs");
    auto weightAt = [&](int32_t t) {
      if (t < first) return 0;
      if (change == 0 || t < change) return rate * (t - first) / 1000;
      return rate * (change - first) / 1000 + rate2 * (t - change) / 1000;
    };
    int32_t cutMs = -1, cutWeight = 0, lastW = 0;
    EndReason reason = EndReason::NONE;
    bool haveStar = false;
    int32_t star = 0;
    uint8_t confirmations = 0;
    for (int32_t t = num(row, "firstSampleMs"); cutMs < 0; t += period) {
      if (t >= p.wallMs) {
        cutMs = p.wallMs;
        cutWeight = lastW;
        reason = EndReason::CONFIGURED_WALL_LIMIT;
        break;
      }
      // Loop before the sample: timed law with zero rate and zero lead.
      if (t >= p.delayMs && haveStar && t >= star) {
        cutMs = t;
        cutWeight = lastW;
        reason = cutReason(p, cutLineDg(p, t));
        break;
      }
      const int32_t w = weightAt(t);
      if (t >= p.delayMs) {
        confirmations = w >= backstopThresholdDg(p, t, r) ? confirmations + 1 : 0;
        if (confirmations >= 2) {
          cutMs = t;
          cutWeight = w;
          reason = cutReason(p, cutLineDg(p, t));
          break;
        }
      }
      lastW = w;
      haveStar = false;
      // Prediction excluded: only a later step of f can meet a weight that
      // is below the line now (the legacy min/max-time stops).
      for (int32_t s = t + 1; w + r < cutLineDg(p, t) && s <= p.wallMs && !haveStar; ++s)
        if (w + r >= cutLineDg(p, s)) {
          haveStar = true;
          star = s;
        }
    }
    int32_t legacy = num(row, "endReason");
    if (legacy == static_cast<int32_t>(EndReason::FAST_EXTRACTION_MIN_TIME))
      legacy = static_cast<int32_t>(EndReason::SCALE_THRESHOLD);
    if (legacy == static_cast<int32_t>(EndReason::SLOW_EXTRACTION_MAX_TIME))
      legacy = static_cast<int32_t>(EndReason::SLOW_EXTRACTION_MIN_WEIGHT);
    // Legacy shared one confirmation counter between the target and ceiling
    // checks, so a ceiling cut could fire on the first ceiling sample after
    // an above-target one; the kernel backstop always needs two samples at
    // the line, so on the raised segment it may cut one sample later.
    const bool sameCut = cutMs == num(row, "cutMs") && cutWeight == num(row, "cutWeightDg");
    const bool ceilingQuirk =
        legacy == static_cast<int32_t>(EndReason::FAST_EXTRACTION_MAX_WEIGHT) &&
        cutMs == num(row, "cutMs") + period;
    const bool match = static_cast<int32_t>(reason) == legacy && (sameCut || ceilingQuirk);
    if (!match)
      std::cerr << cJSON_GetObjectItemCaseSensitive(row, "id")->valuestring << ": reason "
                << static_cast<int>(reason) << " cut " << cutMs << " w " << cutWeight << "\n";
    CHECK(match);
  }
  CHECK(rows == 14);
  cJSON_Delete(root);
}

Params doubleAdaptive() {
  Params p;
  p.goalDg = 360;
  p.fastMarginDg = 40;
  p.slowMarginDg = 30;
  p.fastOn = p.slowOn = true;
  p.delayMs = 12000;
  p.fastBeforeMs = 27000;
  p.slowAfterMs = 34000;
  p.wallMs = 50000;
  return p;
}

void testStepsAndZeroLengthSegments() {
  Params p = doubleAdaptive();
  p.fastRampMs = p.slowRampMs = 0;
  CHECK(cutLineDg(p, 26999) == 400);
  CHECK(cutLineDg(p, 27000) == 360);
  CHECK(cutLineDg(p, 33999) == 360);
  CHECK(cutLineDg(p, 34000) == 330);
  // Delay later than t1 - D clips the fast ramp start to the delay.
  p = doubleAdaptive();
  p.delayMs = 25000;
  CHECK(fastRampStartMs(p) == 25000);
  CHECK(cutLineDg(p, 25000) == 400);
  // Wall closer than t2 + D clips the slow ramp end to the wall.
  p = doubleAdaptive();
  p.wallMs = 36000;
  CHECK(slowRampEndMs(p) == 36000);
  CHECK(cutLineDg(p, 36000) == 330);
  // Sides off are the target line everywhere.
  p = doubleAdaptive();
  p.fastOn = p.slowOn = false;
  for (int32_t t = 0; t <= 50000; t += 250) CHECK(cutLineDg(p, t) == 360);
  CHECK(cutReason(doubleAdaptive(), 380) == EndReason::FAST_EXTRACTION_MAX_WEIGHT);
  CHECK(cutReason(doubleAdaptive(), 360) == EndReason::SCALE_THRESHOLD);
  CHECK(cutReason(doubleAdaptive(), 340) == EndReason::SLOW_EXTRACTION_MIN_WEIGHT);
}

void testSolverAgainstBruteForce() {
  const Params p = doubleAdaptive();
  int32_t none = 0;
  CHECK(!crossingMs(p, 20000, 200, 299, 790, 15, none));
  uint32_t seed = 12345;
  auto next = [&](uint32_t mod) {
    seed = seed * 1103515245u + 12345u;
    return static_cast<int32_t>((seed >> 8) % mod);
  };
  for (int i = 0; i < 400; ++i) {
    const int32_t tk = 12000 + next(36000), w = next(420), q = 300 + next(3800),
                  tau = 300 + next(700), r = next(50);
    int32_t out = 0;
    CHECK(crossingMs(p, tk, w, q, tau, r, out));
    CHECK(out == bruteCrossing(p, tk, w, q, tau, r));
    CHECK(out >= tk && out <= p.wallMs);
  }
}

void testEstimator() {
  // Exact on a clean 2.0 g/s ramp at 10 Hz once 4 samples span >= 2 s.
  RateEstimator e;
  for (int32_t t = 10000; t <= 11900; t += 100) e.push(t, (t - 10000) / 50);
  CHECK(!e.valid);
  e.push(12000, 40);
  CHECK(e.valid && e.rateX100 == 2000);
  for (int32_t t = 12100; t <= 20000; t += 100) e.push(t, (t - 10000) / 50);
  CHECK(e.valid && e.rateX100 == 2000);
  CHECK(e.count <= RateEstimator::CAPACITY);
  CHECK(e.atMs[e.index(e.count - 1)] > 20000 - RATE_WINDOW_MS);

  // Timestamp jitter (+-40 ms) at 1.5 g/s stays within 0.1 g/s.
  RateEstimator j;
  for (int i = 0; i <= 60; ++i) {
    const int32_t t = 10000 + i * 100 + ((i * 37) % 81) - 40;
    j.push(t, (i * 100 * 15) / 1000);
  }
  CHECK(j.valid && std::abs(j.rateX100 - 1500) <= 100);

  // A single spike is excluded; the fit is unchanged.
  RateEstimator s;
  for (int32_t t = 10000; t <= 14000; t += 100) s.push(t, (t - 10000) / 50);
  CHECK(s.push(14100, 300) == RateEstimator::Result::REJECTED);
  CHECK(s.rateX100 == 2000);
  CHECK(s.push(14200, 84) == RateEstimator::Result::ACCEPTED);
  // Three consecutive rejections relearn from the new level.
  CHECK(s.push(14300, 400) == RateEstimator::Result::REJECTED);
  CHECK(s.push(14400, 402) == RateEstimator::Result::REJECTED);
  CHECK(s.push(14500, 404) == RateEstimator::Result::ACCEPTED);
  CHECK(!s.valid && s.count == 1);
  // Duplicate or reordered timestamps are ignored.
  CHECK(s.push(14500, 404) == RateEstimator::Result::IGNORED);

  // Clamps: a falling stream reads 0, a gusher reads 40 dg/s (raw kept).
  RateEstimator down;
  for (int32_t t = 0; t <= 3000; t += 250) down.push(t, 300 - t / 100);
  CHECK(down.valid && down.rateX100 == 0 && down.rawX100 < 0);
  RateEstimator gusher;
  for (int32_t t = 0; t <= 3000; t += 100) gusher.push(t, t * 6 / 100);
  CHECK(gusher.valid && gusher.rateX100 == MAX_RATE_X100 && gusher.rawX100 == 6000);

  // int64 sums at 10 Hz over 3 s with the maximum weight stay exact.
  RateEstimator heavy;
  for (int32_t t = 0; t <= 6000; t += 100) heavy.push(t, 20000 + t / 10);
  CHECK(heavy.valid && heavy.rawX100 == 10000);

  // Reset keeps the channeling latch for the rest of the shot.
  heavy.channeling = true;
  heavy.reset();
  CHECK(!heavy.valid && heavy.count == 0 && heavy.channeling);
}

void testChanneling() {
  // 1.5 g/s at 5 Hz for 4 s, then a surge to 6 g/s: latched on the second
  // qualifying sample.
  RateEstimator e;
  int32_t w = 0;
  for (int32_t t = 0; t <= 4000; t += 200) e.push(t, w += 3);
  CHECK(!e.channeling);
  int32_t latchedAt = -1;
  for (int32_t t = 4200; t <= 8000; t += 200) {
    e.push(t, w += 12);
    if (e.channeling && latchedAt < 0) latchedAt = t;
  }
  CHECK(e.channeling);
  CHECK(latchedAt > 4100 && latchedAt <= 7000);
  // A steady fast shot (6 g/s from the start) is not channeling.
  RateEstimator steady;
  w = 0;
  for (int32_t t = 0; t <= 8000; t += 200) steady.push(t, w += 12);
  CHECK(!steady.channeling);
  // A 5 g/s onset after 3 s of empty pan looks like a surge to a window that
  // still holds the empty pan; baselines after the first drop do not.
  for (const int32_t onset : {0, 3000}) {
    RateEstimator fast;
    fast.onsetMs = onset;
    w = 0;
    for (int32_t t = 0; t <= 9000; t += 200) fast.push(t, t > 3000 ? w += 10 : w);
    CHECK(fast.channeling == (onset == 0));
  }
  // A real mid-shot surge after the onset is still latched.
  RateEstimator late;
  late.onsetMs = 2000;
  w = 0;
  for (int32_t t = 0; t <= 14000; t += 200)
    late.push(t, t > 2000 ? w += (t <= 8000 ? 3 : 12) : w);
  CHECK(late.channeling);
}

void testNegativeArrival() {
  // A raised-segment cut early in the shot projects its arrival before t = 0:
  // no arrival is recorded instead of a wrapped one.
  Params p;
  p.goalDg = 360;
  p.fastMarginDg = 60;
  p.fastOn = true;
  p.delayMs = 500;
  p.fastBeforeMs = 27000;
  p.slowAfterMs = 36000;
  p.wallMs = 50000;
  CHECK(arrivalVerdict(p, 2000, 2000).verdict == Verdict::UNCLASSIFIED);
  const ArrivalVerdict later = arrivalVerdict(p, 6000, 2000);
  CHECK(later.verdict == Verdict::FAST && later.arrivalMs == 3000);
}

void testVerdictPacking() {
  const uint8_t packed = packVerdict(Verdict::SLOW, true, LossSource::FIXED, true);
  CHECK(verdictOf(packed) == Verdict::SLOW);
  CHECK(channelingOf(packed));
  CHECK(lossSourceOf(packed) == LossSource::FIXED);
  CHECK(blindOf(packed));
  CHECK(packVerdict(Verdict::ON_RECIPE, false, LossSource::PROJECTION, false) == 0);
  CHECK(tauMs(690, 0) == 790);
  CHECK(tauMs(300, 300) == 700);
  CHECK(leadDg(4000, 2000) == MAX_LEAD_DG);
}

// Grind hint v2 vectors: every rule of design sections 4-8.
template <typename Enum, size_t N>
Enum enumOf(const cJSON *object, const char *key, const char *const (&names)[N]) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  for (size_t i = 0; item != nullptr && i < N; ++i)
    if (std::string(names[i]) == item->valuestring) return static_cast<Enum>(i);
  return static_cast<Enum>(0);
}
const char *const STATES[] = {"none", "dial_in", "maintenance", "checking", "vary"};
const char *const DIRECTIONS[] = {"none", "finer", "coarser"};
const char *const TIERS[] = {"none", "tentative", "likely", "confident"};
const char *const BINS[] = {"none", "small", "medium", "large"};

int32_t numOr(const cJSON *object, const char *key, int32_t fallback) {
  return cJSON_GetObjectItemCaseSensitive(object, key) ? num(object, key) : fallback;
}

StatsScope hintScope(const cJSON *root, const cJSON *item) {
  const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "window");
  StatsScope scope;
  scope.presetId = static_cast<uint8_t>(num(w, "presetId"));
  scope.goalDg = num(w, "goalDg");
  const cJSON *own = cJSON_GetObjectItemCaseSensitive(item, "window");
  scope.fastBeforeMs = own ? cJSON_GetArrayItem(own, 0)->valueint : num(w, "fastBeforeMs");
  scope.slowAfterMs = own ? cJSON_GetArrayItem(own, 1)->valueint : num(w, "slowAfterMs");
  scope.epoch = static_cast<uint32_t>(numOr(item, "epoch", 0));
  return scope;
}

// A weight cut at its arrival, or {censoredMs}: a max-time stop with 30 g of 36 g.
ArrivalRecord hintShot(const StatsScope &scope, uint32_t id, const cJSON *shot) {
  ArrivalRecord r;
  r.recordId = id;
  r.presetId = scope.presetId;
  r.fastBeforeMs = scope.fastBeforeMs;
  r.slowAfterMs = scope.slowAfterMs;
  r.goalDg = scope.goalDg;
  r.finalWeightDg = 300;
  if (cJSON_IsNumber(shot)) {
    r.endReason = static_cast<uint8_t>(EndReason::SCALE_THRESHOLD);
    r.verdict = static_cast<uint8_t>(Verdict::ON_RECIPE);
    r.arrivalMs = r.durationMs = shot->valueint;
  } else {
    r.endReason = static_cast<uint8_t>(EndReason::CONFIGURED_WALL_LIMIT);
    r.verdict = static_cast<uint8_t>(Verdict::UNCLASSIFIED);
    r.durationMs = num(shot, "censoredMs");
  }
  return r;
}

bool sameHint(const GrindHintV2 &a, const GrindHintV2 &b) {
  return a.state == b.state && a.direction == b.direction && a.tier == b.tier && a.bin == b.bin &&
         a.n == b.n && a.atLeast == b.atLeast && a.offsetMs == b.offsetMs &&
         a.segmentStartId == b.segmentStartId;
}

// Fields an expectation names; the enums default to none, atLeast to false.
void checkHint(const GrindHintV2 &o, const cJSON *e) {
  CHECK(o.state == enumOf<HintState>(e, "state", STATES));
  CHECK(o.direction == enumOf<HintDirection>(e, "direction", DIRECTIONS));
  if (cJSON_GetObjectItemCaseSensitive(e, "tier")) CHECK(o.tier == enumOf<HintTier>(e, "tier", TIERS));
  if (cJSON_GetObjectItemCaseSensitive(e, "bin")) CHECK(o.bin == enumOf<HintBin>(e, "bin", BINS));
  if (cJSON_GetObjectItemCaseSensitive(e, "n")) CHECK(o.n == num(e, "n"));
  if (cJSON_GetObjectItemCaseSensitive(e, "offsetMs")) CHECK(o.offsetMs == num(e, "offsetMs"));
  CHECK(o.atLeast == flag(e, "atLeast"));
  CHECK(o.segmentStartId == static_cast<uint32_t>(num(e, "segmentStartId")));
}

// Replays a vector's shots one by one (applying a white-box advice after
// adviseAfter shots); the pure function must agree after every shot.
template <typename AfterShot>
GrindHintReplay replayVector(const cJSON *root, const cJSON *item, AfterShot afterShot) {
  const StatsScope scope = hintScope(root, item);
  GrindHintReplay replay(scope);
  std::vector<ArrivalRecord> log;
  const cJSON *shot = nullptr;
  cJSON_ArrayForEach(shot, cJSON_GetObjectItemCaseSensitive(item, "shots")) {
    log.push_back(hintShot(scope, static_cast<uint32_t>(log.size() + 1), shot));
    replay.push(log.back());
    if (static_cast<int32_t>(log.size()) == numOr(item, "adviseAfter", -1)) {
      replay.advised = enumOf<HintDirection>(item, "advised", DIRECTIONS);
      replay.candidate = replay.pushed;
    }
    if (numOr(item, "adviseAfter", -1) < 0)
      CHECK(sameHint(replay.out,
                     grindHintOf(log.size(), [&log](size_t i) { return log[i]; }, scope)));
    afterShot(replay, log.size() - 1);
  }
  return replay;
}

void testHintVectors() {
  cJSON *root = loadVectors("corridor_vectors_v2.json");
  CHECK(root != nullptr);
  if (root == nullptr) return;
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "trimmed")) {
    int32_t keys[HINT_LEVEL_SHOTS];
    uint8_t n = 0;
    const cJSON *v = nullptr;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(item, "keys")) keys[n++] = v->valueint;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(item, "censored"))
      keys[n++] = v->valueint + HINT_CENSORED_KEY;
    CHECK(trimmedLevelMs(keys, n) == num(item, "levelMs"));
  }
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "band"))
    CHECK(bandHalfMs(num(item, "halfMs"), num(item, "sigmaMs")) == num(item, "bandMs"));
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "tiers"))
    CHECK(std::string(TIERS[static_cast<int>(tierOf(cJSON_GetArrayItem(item, 0)->valueint))]) ==
          cJSON_GetArrayItem(item, 1)->valuestring);
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "bins"))
    CHECK(binOf(num(item, "distanceMs"), num(item, "middleMs"), num(item, "halfMs")) ==
          enumOf<HintBin>(item, "bin", BINS));
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "rules")) {
    HintEvidence e;
    e.dialIn = flag(item, "dialIn");
    e.n = static_cast<uint8_t>(num(item, "n"));
    e.censored = static_cast<uint8_t>(numOr(item, "censored", 0));
    e.levelMs = num(item, "levelMs");
    e.lastThreeMs = numOr(item, "lastThreeMs", 0);
    e.sigmaMs = num(item, "sigmaMs");
    e.sigmaLongMs = numOr(item, "sigmaLongMs", e.sigmaMs);
    e.advised = enumOf<HintDirection>(item, "advised", DIRECTIONS);
    const HintDecision d = hintDecision(e, hintScope(root, item));
    const bool ok = d.direction == enumOf<HintDirection>(item, "direction", DIRECTIONS) &&
                    d.tier == enumOf<HintTier>(item, "tier", TIERS) &&
                    d.bin == enumOf<HintBin>(item, "bin", BINS) && d.z == num(item, "z");
    if (!ok) std::cerr << "rule: " << cJSON_GetObjectItemCaseSensitive(item, "case")->valuestring << "\n";
    CHECK(ok);
  }
  const StatsScope scope = hintScope(root, nullptr);
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "censoring")) {
    ArrivalRecord r;
    r.recordId = 1;
    r.presetId = scope.presetId;
    r.endReason = static_cast<uint8_t>(num(item, "endReason"));
    r.verdict = static_cast<uint8_t>(num(item, "verdict"));
    r.arrivalMs = num(item, "arrivalMs");
    r.durationMs = num(item, "durationMs");
    r.finalWeightDg = num(item, "finalWeightDg");
    r.fastBeforeMs = scope.fastBeforeMs;
    r.slowAfterMs = scope.slowAfterMs;
    r.goalDg = scope.goalDg;
    const bool ok = censoredArrival(r) == flag(item, "censored") &&
                    clampedArrival(r) == flag(item, "clamped") &&
                    hintAdmissible(r, scope) == flag(item, "admissible") &&
                    hintArrivalMs(r) == num(item, "hintArrivalMs");
    if (!ok) std::cerr << "censoring: " << cJSON_GetObjectItemCaseSensitive(item, "case")->valuestring << "\n";
    CHECK(ok);
  }
  for (const char *section : {"sigma", "changePoints"}) {
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, section)) {
      const GrindHintReplay replay = replayVector(root, item, [](const GrindHintReplay &, size_t) {});
      bool ok = numOr(item, "sigmaLongMs", replay.sigmaLongMs) == replay.sigmaLongMs &&
                numOr(item, "sigmaMs", replay.sigmaMs) == replay.sigmaMs &&
                numOr(item, "segmentStartId", static_cast<int32_t>(replay.out.segmentStartId)) ==
                    static_cast<int32_t>(replay.out.segmentStartId) &&
                numOr(item, "n", replay.out.n) == replay.out.n;
      if (cJSON_GetObjectItemCaseSensitive(item, "state")) {
        const int before = failures;
        checkHint(replay.out, item);
        ok = ok && failures == before;
      }
      if (!ok) std::cerr << section << ": " << cJSON_GetObjectItemCaseSensitive(item, "case")->valuestring << "\n";
      CHECK(ok);
    }
  }
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "lifecycle")) {
    const cJSON *expected = cJSON_GetObjectItemCaseSensitive(item, "expected");
    const int before = failures;
    replayVector(root, item, [expected](const GrindHintReplay &replay, size_t shot) {
      const cJSON *e = cJSON_GetArrayItem(expected, static_cast<int>(shot));
      checkHint(replay.out, e);
      CHECK((replay.announceId == shot + 1) == flag(e, "announced"));
    });
    if (failures != before)
      std::cerr << "lifecycle: " << cJSON_GetObjectItemCaseSensitive(item, "case")->valuestring << "\n";
  }
  cJSON_Delete(root);
}

void testGrindHintReplay() {
  // The hint reads only its preset's rows for the current window and target,
  // in log order: other presets, windows, channeling, blind or out-of-order
  // rows change nothing. A dialled-in coffee that turns fast is advised finer.
  const Params p = doubleAdaptive();
  StatsScope scope;
  scope.presetId = 1;
  scope.fastBeforeMs = p.fastBeforeMs;
  scope.slowAfterMs = p.slowAfterMs;
  scope.goalDg = p.goalDg;
  std::vector<ArrivalRecord> clean, mixed;
  int shown = -1;
  for (uint32_t id = 1; id <= 30; ++id) {
    ArrivalRecord r{id, 1, 3, 0, id <= 20 ? 30500 + static_cast<int32_t>(id * 377 % 1200) - 600 : 26000,
                    0, p.fastBeforeMs, p.slowAfterMs, p.goalDg};
    r.durationMs = r.arrivalMs;
    clean.push_back(r);
    ArrivalRecord other = r;
    other.presetId = 2;
    mixed.push_back(other);
    other = r;
    other.slowAfterMs += 1000;
    mixed.push_back(other);
    other = r;
    other.verdict = packVerdict(Verdict::FAST, true, LossSource::PROJECTION, false);
    mixed.push_back(other);
    mixed.push_back(r);
    other = r;
    other.verdict = packVerdict(Verdict::FAST, false, LossSource::HISTORY, true);
    mixed.push_back(other);
    if (id > 1) mixed.push_back(clean[id - 2]);
    const GrindHintV2 h = grindHintOf(clean.size(), [&clean](size_t i) { return clean[i]; }, scope);
    CHECK(sameHint(h, grindHintOf(mixed.size(), [&mixed](size_t i) { return mixed[i]; }, scope)));
    if (id <= 20) CHECK(h.state == HintState::NONE);
    if (shown < 0 && h.direction == HintDirection::FINER) shown = static_cast<int>(id - 20);
  }
  CHECK(shown > 0 && shown <= 5);
}

// e(u) = I(u; a, b) in long double, the reference for the exact evaluator.
long double betaReference(long double u, int a, int b) {
  const int n = a + b - 1;
  long double e = 0, choose = 1;
  for (int j = 0; j <= n; ++j) {
    if (j >= a) e += choose * std::pow(u, j) * std::pow(1 - u, n - j);
    choose = choose * (n - j) / (j + 1);
  }
  return e;
}

// Every transition curve, exhaustively over the ms grid of a ramp: the exact
// share never steps backwards (the cut line never rises), it is flat at both
// ends, the two shares of one point add up to the margin, and it matches the
// real-valued curve to within the floor.
void testRampCurvesExhaustive() {
  for (uint8_t c = 0; c < RAMP_CURVE_TYPES * RAMP_HARDNESS_LEVELS; ++c) {
    const RampCurve curve = RAMP_CURVES[c];
    for (const int32_t d : {500, 1000, 2500, 5000, 7500, 10000}) {
      for (const int32_t m : {5, 100}) {
        int32_t previous = 0;
        CHECK(rampDg(m, 0, d, c, true) == 0);
        CHECK(rampDg(m, 0, d, c, false) == m);
        CHECK(rampDg(m, d, d, c, true) == m);
        CHECK(rampDg(m, 1, d, c, true) <= 1);
        CHECK(m - rampDg(m, d - 1, d, c, true) <= 1);
        for (int32_t n = 0; n <= d; ++n) {
          const int32_t reached = rampDg(m, n, d, c, true);
          const int32_t left = rampDg(m, n, d, c, false);
          CHECK(reached >= previous);
          CHECK(reached + left == m || reached + left == m - 1);
          const long double ideal =
              m * betaReference(static_cast<long double>(n) / d, curve.a, curve.b);
          CHECK(reached <= ideal + 1e-9L && reached > ideal - 1 - 1e-9L);
          previous = reached;
        }
      }
    }
  }
  CHECK(rampCurveIndex(RAMP_CURVE_EVEN, RAMP_HARDNESS_SOFT) == 0);
  CHECK(rampCurveIndex(RAMP_CURVE_LATE, RAMP_HARDNESS_HARD) == 8);
  CHECK(rampCurveIndex(RAMP_CURVE_TYPES, RAMP_HARDNESS_SOFT) == 0);
  CHECK(rampCurveTypeOf(5) == RAMP_CURVE_EARLY && rampCurveHardnessOf(5) == RAMP_HARDNESS_HARD);
}

// The default curve keeps the original smoothstep law bit for bit.
void testSmoothstepIdentity() {
  for (const int64_t d : {1, 7, 500, 3000, 5000, 10000}) {
    for (const int64_t m : {5, 37, 100}) {
      for (int64_t n = 0; n < d; ++n) {
        CHECK(rampDg(static_cast<int32_t>(m), static_cast<int32_t>(n), static_cast<int32_t>(d), 0,
                     false) == m * (d * d * d - 3 * n * n * d + 2 * n * n * n) / (d * d * d));
        CHECK(rampDg(static_cast<int32_t>(m), static_cast<int32_t>(n), static_cast<int32_t>(d), 0,
                     true) == m * (3 * n * n * d - 2 * n * n * n) / (d * d * d));
      }
    }
  }
}

// Continuous hardness (design 2026-10-31): whole hardness (0/100/200) takes
// the exact legacy integer path for every type, polarity, margin and ramp
// length (A1); the blended cut line stays monotone non-increasing and its flat
// parts do not move (A2); and the blend never leaves 1 dg of the
// double-precision reference blend (A3).
void testContinuousHardnessBlend() {
  // An UNSET runtime pct (a blob written before continuous hardness) must
  // read as its legacy level: the corridor never sees the sentinel, so the
  // cut matches the level the wire still reports (armSmoothCut and
  // pendingCutParams apply this through rampHardnessPctEffective).
  CHECK(rampHardnessPctEffective(RAMP_HARDNESS_PCT_UNSET, RAMP_HARDNESS_SOFT) == 0);
  CHECK(rampHardnessPctEffective(RAMP_HARDNESS_PCT_UNSET, RAMP_HARDNESS_MEDIUM) == 100);
  CHECK(rampHardnessPctEffective(RAMP_HARDNESS_PCT_UNSET, RAMP_HARDNESS_HARD) == 200);
  CHECK(rampHardnessPctEffective(137, RAMP_HARDNESS_HARD) == 137);
  CHECK(rampHardnessPctEffective(0, RAMP_HARDNESS_HARD) == 0);
  const int32_t durations[] = {500, 1000, 2500, 5000, 7500, MAX_RAMP_MS};
  // A1: bit-identity at whole hardness, the crown property. n sweeps the ramp
  // grid including both endpoints.
  for (uint8_t type = 0; type < RAMP_CURVE_TYPES; ++type) {
    for (const uint16_t pct : {0, 100, 200}) {
      for (const bool reached : {false, true}) {
        for (const int32_t d : durations) {
          for (const int32_t m : {5, 37, 100}) {
            for (int i = 0; i <= 40; ++i) {
              const int32_t n = static_cast<int32_t>(static_cast<int64_t>(i) * d / 40);
              CHECK(rampDgBlend(m, n, d, type, pct, reached) ==
                    rampDg(m, n, d, rampCurveIndex(type, static_cast<uint8_t>(pct / 100)),
                           reached));
            }
          }
        }
      }
    }
  }
  // A2: the whole cut line stays monotone non-increasing for a sweep of
  // hardness values, and the held ceiling, window target and floor do not
  // depend on hardness at all.
  for (uint16_t pct = 0; pct <= 200; pct += 5) {
    Params p;
    p.goalDg = 360;
    p.fastMarginDg = 60;
    p.slowMarginDg = 30;
    p.fastOn = p.slowOn = true;
    p.delayMs = 3000;
    p.fastBeforeMs = 27000;
    p.slowAfterMs = 36000;
    p.wallMs = 60000;
    p.fastRampMs = p.slowRampMs = MAX_RAMP_MS;
    p.fastCurve = rampCurveIndex(RAMP_CURVE_LATE, RAMP_HARDNESS_MEDIUM);
    p.slowCurve = rampCurveIndex(RAMP_CURVE_EARLY, RAMP_HARDNESS_SOFT);
    p.fastCurveHardnessPct = static_cast<uint8_t>(pct);
    p.slowCurveHardnessPct = static_cast<uint8_t>(pct);
    int32_t previous = cutLineDg(p, p.delayMs);
    for (int32_t t = p.delayMs; t <= p.wallMs; ++t) {
      const int32_t f = cutLineDg(p, t);
      CHECK(f <= previous);
      previous = f;
    }
    for (const int32_t t : {p.delayMs, p.fastBeforeMs - p.fastRampMs - 1,
                            p.fastBeforeMs, (p.fastBeforeMs + p.slowAfterMs) / 2,
                            p.slowAfterMs, p.slowAfterMs + p.slowRampMs, p.wallMs}) {
      Params soft = p;
      soft.fastCurveHardnessPct = soft.slowCurveHardnessPct = 0;
      CHECK(cutLineDg(p, t) == cutLineDg(soft, t));
    }
  }
  // A3: the blend stays within 1 dg of the ideal blend on the decigram grid
  // (floor of the double-precision blend), and within 1.5 dg of the real
  // value itself (each term floors, one rounding).
  long double worstReal = 0;
  for (uint8_t type = 0; type < RAMP_CURVE_TYPES; ++type) {
    for (uint16_t pct = 0; pct <= 200; ++pct) {
      const int f = pct % 100;
      const RampCurve lo = RAMP_CURVES[rampCurveIndex(type, static_cast<uint8_t>(pct / 100))];
      const RampCurve hi = RAMP_CURVES[rampCurveIndex(type, static_cast<uint8_t>(pct / 100 + 1))];
      for (const bool reached : {false, true}) {
        for (const int32_t d : durations) {
          for (const int32_t m : {5, 37, 100}) {
            for (int i = 0; i <= 17; ++i) {
              const int32_t n = static_cast<int32_t>(static_cast<int64_t>(i) * d / 17);
              const long double u = static_cast<long double>(n) / d;
              const long double share =
                  betaReference(u, lo.a, lo.b) * (100 - f) / 100 +
                  betaReference(u, hi.a, hi.b) * f / 100;
              const long double ideal = m * (reached ? share : 1 - share);
              const int32_t blend = rampDgBlend(m, n, d, type, pct, reached);
              const long double real = blend > ideal ? blend - ideal : ideal - blend;
              if (real > worstReal) worstReal = real;
              CHECK(blend - static_cast<int32_t>(ideal) <= 1);
              CHECK(static_cast<int32_t>(ideal) - blend <= 1);
            }
          }
        }
      }
    }
  }
  CHECK(worstReal < 1.5L);
}

// Per-side lengths and curves: the kernel reproduces the recorded vectors (the
// Web UI checks the same file), the line stays monotone and bounded, and the
// solver still finds the first crossing.
void testTransitionVectors() {
  cJSON *root = loadVectors("corridor_transitions_v1.json");
  CHECK(root != nullptr);
  if (root == nullptr) return;
  const cJSON *curves = cJSON_GetObjectItemCaseSensitive(root, "curves");
  CHECK(cJSON_GetArraySize(curves) == RAMP_CURVE_TYPES * RAMP_HARDNESS_LEVELS);
  for (int c = 0; c < cJSON_GetArraySize(curves); ++c) {
    const cJSON *ab = cJSON_GetArrayItem(curves, c);
    CHECK(cJSON_GetArrayItem(ab, 0)->valueint == RAMP_CURVES[c].a);
    CHECK(cJSON_GetArrayItem(ab, 1)->valueint == RAMP_CURVES[c].b);
  }
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root, "cases")) {
    Params p;
    p.goalDg = num(item, "goalDg");
    p.fastMarginDg = num(item, "fastMarginDg");
    p.slowMarginDg = num(item, "slowMarginDg");
    p.fastOn = flag(item, "fastOn");
    p.slowOn = flag(item, "slowOn");
    p.delayMs = num(item, "delayMs");
    p.fastBeforeMs = num(item, "fastBeforeMs");
    p.slowAfterMs = num(item, "slowAfterMs");
    p.wallMs = num(item, "wallMs");
    p.fastRampMs = num(item, "fastRampMs");
    p.slowRampMs = num(item, "slowRampMs");
    p.fastCurve = static_cast<uint8_t>(num(item, "fastCurve"));
    p.slowCurve = static_cast<uint8_t>(num(item, "slowCurve"));
    // The stored index carries the case's hardness; the blend reads the pct.
    p.fastCurveHardnessPct = rampHardnessPctOf(rampCurveHardnessOf(p.fastCurve));
    p.slowCurveHardnessPct = rampHardnessPctOf(rampCurveHardnessOf(p.slowCurve));
    const cJSON *point = nullptr;
    cJSON_ArrayForEach(point, cJSON_GetObjectItemCaseSensitive(item, "cutLine")) {
      CHECK(cutLineDg(p, cJSON_GetArrayItem(point, 0)->valueint) ==
            cJSON_GetArrayItem(point, 1)->valueint);
    }
    int32_t previous = cutLineDg(p, p.delayMs);
    for (int32_t t = p.delayMs; t <= p.wallMs; ++t) {
      const int32_t f = cutLineDg(p, t);
      CHECK(f <= previous && f <= ceilingDg(p) && f >= floorDg(p));
      previous = f;
    }
    for (int32_t tk = p.delayMs; tk < p.wallMs; tk += 4000) {
      for (const int32_t q : {400, 1500, 3500}) {
        int32_t out = 0;
        CHECK(crossingMs(p, tk, p.goalDg / 2, q, 790, 15, out));
        CHECK(out == bruteCrossing(p, tk, p.goalDg / 2, q, 790, 15));
      }
    }
  }
  cJSON_Delete(root);
}

void testShotStatsSummary() {
  // The Web UI's shot statistics: every admissible shot since the epoch
  // counts in total, the newest ten feed the sorted arrivals, and the first
  // drop median ignores unmeasured shots.
  ArrivalRecord records[16];
  size_t count = 0;
  for (uint32_t id = 1; id <= 12; ++id)
    records[count++] = {id, 1, 3, 0, static_cast<int32_t>(26000 + id * 500),
                        id % 3 == 0 ? 0 : static_cast<int32_t>(5000 + id * 100)};
  records[count++] = {13, 1, 3, packVerdict(Verdict::FAST, true, LossSource::PROJECTION, false),
                      20000, 4000};  // channeling: never counted
  records[count++] = {14, 2, 3, 0, 30000, 6000};  // another preset
  // Same preset, other window or a target 1.1 g away: arrivals skip them,
  // first drops keep them.
  records[count++] = {15, 1, 3, 0, 31000, 9000, 1000};
  records[count++] = {16, 1, 3, 0, 31000, 9000, 0, 0, 11};
  const auto rows = [&](size_t i) { return records[i]; };
  int32_t arrivals[STATS_MAX_SHOTS] = {};
  StatsScope scope;
  scope.presetId = 1;
  const ArrivalStats s = arrivalStatsOf(count, rows, scope, arrivals);
  CHECK(s.n == STATS_MAX_SHOTS && s.total == 12);
  CHECK(arrivals[0] == 27500 && arrivals[STATS_MAX_SHOTS - 1] == 32000);
  for (uint8_t i = 1; i < s.n; ++i) CHECK(arrivals[i - 1] <= arrivals[i]);
  scope.goalDg = 10;  // 1 g away still counts
  CHECK(arrivalStatsOf(count, rows, scope).total == 13);
  CHECK(arrivalStats(records, count, 1, 13).total == 0);
  // First drops: the newest ten measured, whatever the window, target or
  // ending (ids 2,4,5,7,8,10,11,13,15,16): median of 5700 and 5800.
  const FirstDropStats own = firstDropStatsOf(count, rows, 1, 0, false);
  CHECK(own.n == STATS_MAX_SHOTS && own.medianMs == (5700 + 5800) / 2);
  // New coffee (epoch 15) keeps only the shots after it.
  const FirstDropStats fresh = firstDropStatsOf(count, rows, 1, 15, false);
  CHECK(fresh.n == 2 && fresh.medianMs == 9000);
  CHECK(firstDropStatsOf(count, rows, 1, 17, false).n == 0);
  // The fallback reads only the other presets, whatever their epoch.
  const FirstDropStats others = firstDropStatsOf(count, rows, 1, 99, true);
  CHECK(others.n == 1 && others.medianMs == 6000);
}

}  // namespace

int main() {
  testVectors();
  testRampCurvesExhaustive();
  testSmoothstepIdentity();
  testContinuousHardnessBlend();
  testTransitionVectors();
  testLegacyOracle();
  testStepsAndZeroLengthSegments();
  testSolverAgainstBruteForce();
  testEstimator();
  testChanneling();
  testNegativeArrival();
  testVerdictPacking();
  testHintVectors();
  testGrindHintReplay();
  testShotStatsSummary();
  if (failures != 0) {
    std::cerr << failures << " corridor kernel check(s) failed\n";
    return 1;
  }
  std::cout << "corridor kernel host test passed\n";
  return 0;
}

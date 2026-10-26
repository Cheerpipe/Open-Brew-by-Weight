// Uncensored synthetic replay of the smooth cut (plan W3.1). Each synthetic
// shot is a true weight curve; every candidate law is simulated against the
// same curve, so no alternative is ever scored against a censored outcome.
// With a path argument the binary instead replays an exported Stats CSV and
// prints, per shot, when each law would have commanded the stop.
#include "../OpenBrewByWeightCorridor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace openbrewbyweight::corridor;

namespace {

int failures = 0;
#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      ++failures;                                                             \
      std::cerr << __LINE__ << ": check failed: " << #condition << "\n";      \
    }                                                                         \
  } while (false)

struct Rng {
  uint32_t state;
  double uniform() {
    state = state * 1664525u + 1013904223u;
    return (state >> 8) / 16777216.0;
  }
  double range(double lo, double hi) { return lo + (hi - lo) * uniform(); }
  double normal() {
    double sum = 0;
    for (int i = 0; i < 12; ++i) sum += uniform();
    return sum - 6.0;
  }
};

struct ScaleKind {
  int32_t periodMs;
  uint16_t lagMs;  // protocol table value used by tau
};
constexpr ScaleKind kScales[] = {{100, 300}, {200, 500}, {500, 690}};

// One synthetic shot: true flow shape and its physical stop behavior.
struct Shot {
  int32_t firstDropMs;
  double steadyGps;
  double rampS;
  double declinePerS;
  int32_t channelAtMs;  // 0 = none
  ScaleKind scale;
  int32_t trueLagMs;
  int32_t valveMs;
  double dripBaseG;
  uint32_t jitterSeed;
  std::vector<double> weightG;  // true weight per ms

  double flowAt(int32_t t) const {
    if (t < firstDropMs) return 0;
    const double s = (t - firstDropMs) / 1000.0;
    double q = steadyGps * (1 - std::exp(-s / rampS)) * std::max(0.4, 1 - declinePerS * s);
    if (channelAtMs && t >= channelAtMs)
      q *= 1 + 2.2 * std::min(1.0, (t - channelAtMs) / 600.0);
    return q;
  }
  void integrate(int32_t untilMs) {
    weightG.assign(static_cast<size_t>(untilMs) + 1, 0.0);
    for (int32_t t = 1; t <= untilMs; ++t) weightG[t] = weightG[t - 1] + flowAt(t) / 1000.0;
  }
  double at(int32_t t) const {
    return weightG[std::max<int32_t>(0, std::min<int32_t>(t, static_cast<int32_t>(weightG.size()) - 1))];
  }
};

Shot makeShot(Rng &rng, const Params &p, double arrivalS, bool channel, int scaleIndex) {
  Shot s;
  s.firstDropMs = static_cast<int32_t>(rng.range(5000, 9000));
  s.rampS = rng.range(1.0, 2.5);
  s.declinePerS = rng.range(0.0, 0.012);
  s.scale = kScales[scaleIndex];
  s.trueLagMs = s.scale.lagMs + static_cast<int32_t>(rng.range(-60, 60));
  s.valveMs = static_cast<int32_t>(rng.range(80, 120));
  s.dripBaseG = rng.range(0.4, 0.9);
  s.jitterSeed = static_cast<uint32_t>(rng.uniform() * 1e9);
  s.channelAtMs = 0;
  s.steadyGps = 1.0;
  s.integrate(p.wallMs + 2000);
  // Scale the steady flow so the target is reached at arrivalS.
  const int32_t arrivalMs = static_cast<int32_t>(arrivalS * 1000);
  s.steadyGps = p.goalDg / 10.0 / s.at(arrivalMs);
  if (channel) s.channelAtMs = static_cast<int32_t>(rng.range(s.firstDropMs + 4000, arrivalMs - 2000));
  s.integrate(p.wallMs + 2000);
  return s;
}

enum class Law : uint8_t { TIMED, PER_SAMPLE };

struct Outcome {
  int32_t cutMs = 0;
  int32_t lineDg = 0;
  double finalG = 0;
  double errorG = 0;
  bool channeling = false;
  bool weightCut = false;
  int32_t rateAtCut = 0;
  int32_t arrivalMs = 0;
  bool arrivalValid = false;
};

struct Sample {
  int32_t atMs;
  int32_t weightDg;
};

std::vector<Sample> samplesOf(const Shot &s, int32_t untilMs) {
  std::vector<Sample> out;
  Rng jitter{s.jitterSeed};
  int32_t last = -1;
  for (int32_t tick = s.scale.periodMs; tick <= untilMs; tick += s.scale.periodMs) {
    int32_t received = tick + static_cast<int32_t>(jitter.range(0, 40));
    if (received <= last) received = last + 1;
    last = received;
    const double noise = jitter.uniform() < 0.15 ? (jitter.uniform() < 0.5 ? -0.1 : 0.1) : 0.0;
    out.push_back({received, static_cast<int32_t>(std::lround((s.at(tick - s.trueLagMs) + noise) * 10))});
  }
  return out;
}

// Simulates the control loop at 10 ms ticks. lossAtMs > 0 stops the stream.
Outcome simulate(const Shot &shot, Params p, Law law, double offsetG,
                 RateEstimator *stateAtLoss = nullptr, int32_t lossAtMs = 0,
                 Sample *lastBeforeLoss = nullptr) {
  const std::vector<Sample> samples = samplesOf(shot, p.wallMs);
  RateEstimator rate;
  const int32_t tau = tauMs(shot.scale.lagMs, 0);
  const int32_t offsetDg = static_cast<int32_t>(std::lround(offsetG * 10));
  bool haveStar = false;
  int32_t star = 0;
  uint8_t confirmations = 0;
  size_t next = 0;
  Outcome out;
  for (int32_t tick = 0; tick <= p.wallMs; tick += 10) {
    bool fire = tick >= p.wallMs;
    while (next < samples.size() && samples[next].atMs <= tick) {
      const Sample &k = samples[next++];
      if (lossAtMs && k.atMs >= lossAtMs) continue;
      if (rate.push(k.atMs, k.weightDg) != RateEstimator::Result::ACCEPTED) continue;
      if (lastBeforeLoss) *lastBeforeLoss = k;
      if (rate.channeling) p.fastOn = false;  // f := min(f, target)
      haveStar = rate.valid && crossingMs(p, k.atMs, k.weightDg, rate.rateX100, tau, offsetDg, star);
      const bool over = k.weightDg >= backstopThresholdDg(p, k.atMs, offsetDg);
      confirmations = over ? static_cast<uint8_t>(confirmations + 1) : 0;
      if (tick >= p.delayMs && (confirmations >= 2 ||
                                (law == Law::PER_SAMPLE && haveStar && star == k.atMs)))
        fire = true;
    }
    if (lossAtMs && tick >= lossAtMs) {
      if (stateAtLoss) *stateAtLoss = rate;
      out.cutMs = -1;
      return out;
    }
    if (law == Law::TIMED && haveStar && tick >= star && tick >= p.delayMs) fire = true;
    if (!fire) continue;
    out.cutMs = tick;
    out.weightCut = tick < p.wallMs;
    out.channeling = rate.channeling;
    out.lineDg = cutLineDg(p, tick);
    out.rateAtCut = rate.valid ? rate.rateX100 : 0;
    const double flow = shot.flowAt(tick + shot.valveMs);
    out.finalG = shot.at(tick + shot.valveMs) + shot.dripBaseG + 0.25 * flow;
    out.errorG = out.finalG - out.lineDg / 10.0;
    const ArrivalVerdict v = arrivalVerdict(p, tick, out.rateAtCut);
    out.arrivalValid = v.verdict != Verdict::UNCLASSIFIED;
    out.arrivalMs = v.arrivalMs;
    return out;
  }
  return out;
}

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5))];
}

Params factory(bool dbl) {
  Params p;
  p.goalDg = dbl ? 360 : 180;
  p.fastMarginDg = dbl ? 40 : 20;
  p.slowMarginDg = dbl ? 30 : 15;
  p.fastOn = p.slowOn = true;
  p.delayMs = 12000;
  p.fastBeforeMs = dbl ? 27000 : 23000;
  p.slowAfterMs = dbl ? 34000 : 32000;
  p.wallMs = 50000;
  return p;
}

struct CorpusShot {
  Shot shot;
  bool dbl;
  bool channel;
};

std::vector<CorpusShot> corpus() {
  Rng rng{20261014};
  std::vector<CorpusShot> out;
  for (int i = 0; i < 240; ++i) {
    const bool dbl = i % 2 == 0;
    const Params p = factory(dbl);
    const double t1 = p.fastBeforeMs / 1000.0, t2 = p.slowAfterMs / 1000.0;
    const double pick = rng.uniform();
    // Dialed-in barista: most shots on recipe, some fast and slow ones.
    const double arrival = pick < 0.7 ? (t1 + t2) / 2 + 1.4 * rng.normal()
                         : pick < 0.85 ? rng.range(t1 - 6, t1) : rng.range(t2, t2 + 6);
    const bool channel = rng.uniform() < 0.08;
    out.push_back({makeShot(rng, p, std::max(16.0, std::min(44.0, arrival)), channel, i % 3), dbl, channel});
  }
  return out;
}

struct LawResult {
  std::vector<double> absError;
  int outside = 0;
  int channelFlagged = 0, channelShots = 0, falseChannel = 0, normalShots = 0;
};

LawResult runLaw(const std::vector<CorpusShot> &shots, Law law, int32_t rampMs,
                 std::vector<Outcome> *outcomes = nullptr) {
  LawResult result;
  double offset[2] = {1.5, 1.5};
  for (const CorpusShot &c : shots) {
    Params p = factory(c.dbl);
    p.fastRampMs = p.slowRampMs = rampMs;
    const Outcome o = simulate(c.shot, p, law, offset[c.dbl]);
    if (outcomes) outcomes->push_back(o);
    if (o.cutMs < p.delayMs || o.cutMs > p.wallMs) ++result.outside;
    (c.channel ? result.channelShots : result.normalShots)++;
    if (o.channeling) (c.channel ? result.channelFlagged : result.falseChannel)++;
    if (!o.weightCut || o.channeling) continue;
    result.absError.push_back(std::fabs(o.errorG));
    // r = learned drip offset, one adaptive loop (alpha 0.3, [0, 5] g); like
    // bbwEwma::learn, an observation beyond the 5 g offset bound is rejected.
    if (std::fabs(offset[c.dbl] + o.errorG) <= 5.0)
      offset[c.dbl] = std::max(0.0, std::min(5.0, offset[c.dbl] + 0.3 * o.errorG));
  }
  return result;
}

void syntheticReplay() {
  const std::vector<CorpusShot> shots = corpus();
  std::vector<Outcome> timedOutcomes;
  const LawResult timed = runLaw(shots, Law::TIMED, RAMP_MS, &timedOutcomes);
  const LawResult perSample = runLaw(shots, Law::PER_SAMPLE, RAMP_MS);
  const double timedP95 = percentile(timed.absError, 0.95);
  const double perSampleP95 = percentile(perSample.absError, 0.95);
  std::printf("law: timed p95 |final - f(t_cut)| = %.2f g (median %.2f), per-sample %.2f g\n",
              timedP95, percentile(timed.absError, 0.5), perSampleP95);
  // The per-sample law replaces the timed law only if it beats it by >= 0.3 g.
  CHECK(!(perSampleP95 <= timedP95 - 0.3));
  CHECK(timedP95 <= 1.5);
  CHECK(timed.outside == 0 && perSample.outside == 0);

  // D (4-6 s): keep 5 s unless a neighbour beats it by >= 0.3 g.
  for (int32_t ramp : {4000, 6000}) {
    const double p95 = percentile(runLaw(shots, Law::TIMED, ramp).absError, 0.95);
    std::printf("D = %d ms: p95 %.2f g\n", ramp, p95);
    CHECK(!(p95 <= timedP95 - 0.3));
  }

  // Channeling: flagged on <= 2 % of normal shots, caught on most surges.
  std::printf("channeling: %d/%d surges flagged, %d/%d normal shots flagged\n",
              timed.channelFlagged, timed.channelShots, timed.falseChannel, timed.normalShots);
  CHECK(timed.falseChannel * 50 <= timed.normalShots);
  CHECK(timed.channelFlagged * 10 >= timed.channelShots * 3);

  // Injected scale losses: R1 projection vs R2 history against the
  // uncensored cut; the deadline the firmware would pick must be accurate.
  std::vector<ArrivalRecord> history;
  std::vector<double> r1Error, r2Error, chosenError, projectedError;
  Rng rng{77};
  int outside = 0;
  for (size_t i = 0; i < shots.size(); ++i) {
    const CorpusShot &c = shots[i];
    const Params p = factory(c.dbl);
    const Outcome &truth = timedOutcomes[i];
    if (!truth.weightCut || truth.channeling) continue;
    const uint8_t presetId = c.dbl ? 1 : 2;
    const ArrivalStats stats = arrivalStats(history.data(), history.size(), presetId, 0);
    const int32_t lossAt = truth.cutMs - static_cast<int32_t>(rng.range(300, 4000));
    if (lossAt > p.delayMs) {
      RateEstimator state;
      Sample last{0, 0};
      simulate(c.shot, p, Law::TIMED, 1.0, &state, lossAt, &last);
      LossInputs in;
      in.nowMs = lossAt;
      in.lastSampleMs = last.atMs;
      in.beforeFirstDrops = last.weightDg < 5;
      in.crossingValid = state.valid &&
          crossingMs(p, last.atMs, last.weightDg, state.rateX100, tauMs(c.shot.scale.lagMs, 0), 10,
                     in.crossingMs);
      in.history = stats;
      const LossDeadline chosen = lossDeadline(p, in);
      if (chosen.deadlineMs < p.delayMs || chosen.deadlineMs > p.wallMs) ++outside;
      chosenError.push_back(std::fabs(chosen.deadlineMs - truth.cutMs) / 1000.0);
      if (chosen.source == LossSource::PROJECTION) projectedError.push_back(chosenError.back());
      if (chosen.source == LossSource::PROJECTION && stats.n >= SCALE_LOSS_MIN_SHOTS) {
        LossInputs noProjection = in;
        noProjection.crossingValid = false;
        r1Error.push_back(std::fabs(chosen.deadlineMs - truth.cutMs) / 1000.0);
        r2Error.push_back(std::fabs(lossDeadline(p, noProjection).deadlineMs - truth.cutMs) / 1000.0);
      }
    }
    if (truth.arrivalValid)
      history.push_back({static_cast<uint32_t>(i + 1), presetId, 3, 0, truth.arrivalMs});
  }
  std::printf("scale loss: all regimes median %.2f s p95 %.2f s (n=%zu); projection median %.2f s "
              "p95 %.2f s (n=%zu); R1 median %.2f s vs R2 %.2f s (n=%zu)\n",
              percentile(chosenError, 0.5), percentile(chosenError, 0.95), chosenError.size(),
              percentile(projectedError, 0.5), percentile(projectedError, 0.95), projectedError.size(),
              percentile(r1Error, 0.5), percentile(r2Error, 0.5), r1Error.size());
  CHECK(outside == 0);
  CHECK(!r1Error.empty() && percentile(r1Error, 0.5) <= percentile(r2Error, 0.5));
  CHECK(percentile(chosenError, 0.5) <= 0.5);
  // Accuracy targets hold where the shot itself is observed (projection);
  // the history and window regimes cannot know an off-recipe shot's pace.
  CHECK(percentile(projectedError, 0.5) <= 0.5 && percentile(projectedError, 0.95) <= 1.5);
}

// CSV mode: replay exported curves (censored at the recorded stop).
int replayCsv(const char *path) {
  std::ifstream file(path);
  std::string line;
  if (!std::getline(file, line)) {
    std::cerr << "cannot read " << path << "\n";
    return 1;
  }
  std::vector<std::string> header;
  for (std::stringstream ss(line); std::getline(ss, line, ',');) header.push_back(line);
  auto column = [&](const std::string &name) {
    for (size_t i = 0; i < header.size(); ++i) if (header[i] == name) return static_cast<int>(i);
    return -1;
  };
  const int goal = column("goal_g"), duration = column("duration_s"), first = column("sample_1_time_s");
  if (goal < 0 || duration < 0 || first < 0) {
    std::cerr << "missing goal_g, duration_s or sample columns\n";
    return 1;
  }
  std::printf("id,recorded_stop_s,timed_cut_s,per_sample_cut_s,censored\n");
  while (std::getline(file, line)) {
    std::vector<std::string> cells;
    for (std::stringstream ss(line); std::getline(ss, line, ',');) cells.push_back(line);
    if (static_cast<int>(cells.size()) <= first) continue;
    Params p = factory(true);
    p.goalDg = static_cast<int32_t>(std::lround(std::atof(cells[goal].c_str()) * 10));
    const int32_t recordedMs = static_cast<int32_t>(std::atof(cells[duration].c_str()) * 1000);
    int32_t cut[2] = {-1, -1};
    for (int law = 0; law < 2; ++law) {
      RateEstimator rate;
      for (size_t i = first; i + 1 < cells.size(); i += 3) {
        if (cells[i].empty() || cells[i + 1].empty()) break;
        const int32_t t = static_cast<int32_t>(std::atof(cells[i].c_str()) * 1000);
        const int32_t w = static_cast<int32_t>(std::lround(std::atof(cells[i + 1].c_str()) * 10));
        if (rate.push(t, w) != RateEstimator::Result::ACCEPTED || !rate.valid || t < p.delayMs) continue;
        int32_t star = 0;
        if (crossingMs(p, t, w, rate.rateX100, tauMs(500, 0), 15, star) &&
            (law == 0 || star == t)) {
          cut[law] = std::max(star, t);
          break;
        }
      }
    }
    std::printf("%s,%.2f,%.2f,%.2f,%s\n", cells[0].c_str(), recordedMs / 1000.0, cut[0] / 1000.0,
                cut[1] / 1000.0, (cut[0] < 0 || cut[0] > recordedMs) ? "yes" : "no");
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1) return replayCsv(argv[1]);
  syntheticReplay();
  if (failures) {
    std::cerr << failures << " replay acceptance check(s) failed\n";
    return 1;
  }
  std::cout << "corridor replay host test passed\n";
  return 0;
}

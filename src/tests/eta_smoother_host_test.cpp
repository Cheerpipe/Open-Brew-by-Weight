// ETA smoother kernel: seeding, reset, inertia schedule and noise rejection.
#include "../OpenBrewByWeightEtaSmoother.h"

#include <cstdlib>
#include <iostream>

using namespace openbrewbyweight;

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __func__ << ":" << __LINE__ << ": check failed: "         \
                << #condition << "\n";                                       \
    }                                                                        \
  } while (false)

const eta::SmootherConfig kConfig{};

// Feeds etaAt(t) every dtMs over [fromMs, toMs]; returns the last output.
template <typename F>
int32_t feed(eta::Smoother &s, int32_t fromMs, int32_t toMs, int32_t dtMs, F etaAt) {
  int32_t out = -1;
  for (int32_t t = fromMs; t <= toMs; t += dtMs) out = eta::smooth(s, kConfig, t, etaAt(t), true, dtMs);
  return out;
}

void testNotReadyOrMissingResets() {
  eta::Smoother s;
  feed(s, 10000, 12000, 100, [](int32_t) { return 30000; });
  CHECK(s.shownMs == 30000);
  CHECK(eta::smooth(s, kConfig, 12100, 30000, false, 100) == -1);
  CHECK(s.shownMs == -1 && s.seedCount == 0);
  feed(s, 12200, 13200, 100, [](int32_t) { return 30000; });
  CHECK(s.shownMs == 30000);
  CHECK(eta::smooth(s, kConfig, 13300, -1, true, 100) == -1);
  CHECK(s.shownMs == -1);
}

void testSeedIsTheFirstSecondsMedian() {
  eta::Smoother s;
  // Hidden for the first second; one wild estimate does not move the seed.
  CHECK(eta::smooth(s, kConfig, 10000, 20000, true, 100) == -1);
  int32_t out = -1;
  for (int32_t t = 10100; t < 11000; t += 100) {
    out = eta::smooth(s, kConfig, t, 31000 + (t / 100) % 3 * 100, true, 100);
    CHECK(out == -1);
  }
  out = eta::smooth(s, kConfig, 11000, 31000, true, 100);
  CHECK(out >= 31000 && out <= 31200);
}

void testSeedCapacityAtFastFeeds() {
  eta::Smoother s;
  // 50 Hz: the buffer keeps its first SEED_CAPACITY estimates.
  const int32_t out = feed(s, 10000, 11000, 20, [](int32_t) { return 28000; });
  CHECK(s.seedCount == eta::SEED_CAPACITY);
  CHECK(out == 28000);
}

void testFarJumpGlidesNearJumpFollows() {
  eta::Smoother s;
  feed(s, 10000, 11000, 100, [](int32_t) { return 30000; });
  // 19 s from the end, a 9 s late jump moves the dot well under 1 s per second.
  const int32_t far = feed(s, 11100, 12000, 100, [](int32_t) { return 39000; });
  CHECK(far > 30000 && far < 31000);
  eta::Smoother n;
  feed(n, 25000, 26000, 100, [](int32_t) { return 30000; });
  // 3 s before the end, a second covers most of a move (inertia ~1 s).
  const int32_t near = feed(n, 26100, 27000, 100, [](int32_t) { return 29000; });
  CHECK(near < 29600);
}

void testEarlierEstimateIsFollowedFaster() {
  eta::Smoother a, b;
  feed(a, 10000, 11000, 100, [](int32_t) { return 30000; });
  feed(b, 10000, 11000, 100, [](int32_t) { return 30000; });
  const int32_t earlier = feed(a, 11100, 13000, 100, [](int32_t) { return 24000; });
  const int32_t later = feed(b, 11100, 13000, 100, [](int32_t) { return 36000; });
  CHECK(30000 - earlier > later - 30000);
}

void testAlternatingNoiseIsDamped() {
  eta::Smoother s;
  int64_t inputMove = 0, outputMove = 0;
  int32_t prevIn = 31000, prevOut = -1;
  for (int32_t t = 12000; t <= 26000; t += 100) {
    const int32_t in = 31000 + ((t / 100) % 2 ? 4000 : -4000);
    const int32_t out = eta::smooth(s, kConfig, t, in, true, 100);
    inputMove += std::abs(in - prevIn);
    if (out >= 0 && prevOut >= 0) outputMove += std::abs(out - prevOut);
    prevIn = in;
    if (out >= 0) prevOut = out;
  }
  CHECK(prevOut > 29000 && prevOut < 33000);
  CHECK(outputMove * 10 < inputMove);
}

void testConvergesOnASteadyEstimate() {
  eta::Smoother s;
  feed(s, 10000, 11000, 100, [](int32_t) { return 36000; });
  const int32_t out = feed(s, 11100, 30000, 100, [](int32_t) { return 31000; });
  CHECK(out >= 31000 && out <= 31100);
}

}  // namespace

int main() {
  testNotReadyOrMissingResets();
  testSeedIsTheFirstSecondsMedian();
  testSeedCapacityAtFastFeeds();
  testFarJumpGlidesNearJumpFollows();
  testEarlierEstimateIsFollowedFaster();
  testAlternatingNoiseIsDamped();
  testConvergesOnASteadyEstimate();
  if (failures != 0) {
    std::cerr << failures << " ETA smoother check(s) failed\n";
    return 1;
  }
  std::cout << "ETA smoother host test passed\n";
  return 0;
}

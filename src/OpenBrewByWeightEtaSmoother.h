#pragma once

// ETA smoother — a display post-filter for an estimated arrival time (an
// absolute time in ms on the caller's clock). It never changes the estimate
// itself: callers keep their raw value and show this one. Pure integer math,
// no globals, no heap; the caller owns the state and decides when its
// estimate is showable. First consumer: the SAP's live forecast dot.

#include "OpenBrewByWeightCorridor.h"

#include <stdint.h>

namespace openbrewbyweight {
namespace eta {

constexpr uint8_t SEED_CAPACITY = 12;  // one second at a 10 Hz feed

struct SmootherConfig {
  int32_t inertiaPermille = 500;  // inertia = this share of the time left
  int32_t minInertiaMs = 300;
  int32_t seedMs = 1000;          // the first second's median seeds the output
};

struct Smoother {
  int32_t shownMs = -1;  // -1 = nothing to show
  int32_t seedFromMs = 0;
  uint8_t seedCount = 0;
  int32_t seed[SEED_CAPACITY] = {};
};

// One step per new estimate at nowMs, dtMs after the previous one. A missing
// estimate (etaMs < 0) or a caller that is not ready resets the smoother, so
// the next showable estimate seeds afresh. Returns the value to show.
inline int32_t smooth(Smoother &s, const SmootherConfig &c, int32_t nowMs,
                      int32_t etaMs, bool ready, int32_t dtMs) {
  if (etaMs < 0 || !ready) {
    s = Smoother{};
    return -1;
  }
  if (s.shownMs < 0) {
    if (s.seedCount == 0) s.seedFromMs = nowMs;
    if (s.seedCount < SEED_CAPACITY) s.seed[s.seedCount++] = etaMs;
    if (nowMs - s.seedFromMs < c.seedMs) return -1;
    corridor::sortSmall(s.seed, s.seedCount);
    return s.shownMs = corridor::medianOfSorted(s.seed, s.seedCount);
  }
  // Inertia follows the more urgent of the shown and the new estimate: heavy
  // while the arrival is far, light as it nears, so the end is never shown late.
  const int32_t left = (etaMs < s.shownMs ? etaMs : s.shownMs) - nowMs;
  int64_t inertia = left > 0 ? static_cast<int64_t>(left) * c.inertiaPermille / 1000 : 0;
  if (inertia < c.minInertiaMs) inertia = c.minInertiaMs;
  const int64_t dt = dtMs > 0 ? dtMs : 1;
  s.shownMs += static_cast<int32_t>((static_cast<int64_t>(etaMs) - s.shownMs) * dt /
                                    (inertia + dt));
  return s.shownMs;
}

}  // namespace eta
}  // namespace openbrewbyweight

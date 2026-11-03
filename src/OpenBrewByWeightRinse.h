#pragma once

#include <stdint.h>

// =============================================================================
// LAYER: Rinse (generic duration)
// =============================================================================
// WHAT: Timed rinse clock. Starts when the stopper *accepts* a rinse and
//       reports when the supplied duration has elapsed. The caller anchors
//       the start: a rinse demoted from a running cycle passes that cycle's
//       start so pre-classification time counts as rinse time. Product rules
//       (beeps, no-scale BBW, history, locks) stay in the stopper.
//
// BOUNDARY: Duration only. Does not detect gestures, read GPIO, know paddle vs
// switch, call machine, or write CycleSession. The stopper copies
// rinseClock.startedAtMs into the cycle session if tests need a mirror.

// Water runs durationMs from startedAtMs.
struct RinseClock {
  uint32_t startedAtMs = 0;
  uint32_t durationMs = 0;
};

RinseClock rinseClock;
bool rinseTimerActive = false;

uint32_t rinseBegin(RinseClock clock) {
  rinseClock = clock;
  rinseTimerActive = true;
  return rinseClock.startedAtMs;
}

bool rinseDeadlineReached() {
  return rinseTimerActive &&
         elapsedMs(rinseClock.startedAtMs) >= rinseClock.durationMs;
}

void rinseClear() {
  rinseTimerActive = false;
  rinseClock = {};
}

#pragma once

#include <atomic>
#include <cstdint>

namespace openbrewbyweight {

// Wrap-safe ordering of 32-bit tick counters (millis() stamps, sequences,
// generations, revisions) that wrap modulo 2^32. Results are exact while the
// two values are less than 2^31 apart (about 24.8 days of millis()): a modular
// difference below TICK_HALF_RANGE is a non-negative signed one, so each
// comparison equals the int32_t cast of the difference. constexpr so ISR and
// IRAM callers inline them without a call into flash.
constexpr uint32_t TICK_HALF_RANGE = 0x80000000U;
constexpr int32_t ticksSince(uint32_t now, uint32_t then) {
  return static_cast<int32_t>(now - then);
}
constexpr bool tickReached(uint32_t now, uint32_t deadline) {
  return now - deadline < TICK_HALF_RANGE;
}
constexpr bool tickAfter(uint32_t a, uint32_t b) {
  return a != b && a - b < TICK_HALF_RANGE;
}
constexpr bool tickBefore(uint32_t a, uint32_t b) { return a - b >= TICK_HALF_RANGE; }

// One moment read from both device clocks: millis() uptime and the wall clock
// derived from it. utcSec is 0 while the wall clock is not synced.
struct Instant {
  uint32_t monotonicMs = 0;
  uint32_t utcSec = 0;
};

// Last/max acquisition (or hold) time of a blocking mutex, updated from the
// acquiring task; read by the task-profiler serializer.
struct MutexWaitStat {
  std::atomic<uint32_t> lastUs{0};
  std::atomic<uint32_t> maxUs{0};

  void note(uint32_t waitedUs) {
    lastUs.store(waitedUs, std::memory_order_relaxed);
    uint32_t known = maxUs.load(std::memory_order_relaxed);
    while (waitedUs > known &&
           !maxUs.compare_exchange_weak(known, waitedUs,
                                        std::memory_order_relaxed)) {
    }
  }
};

}  // namespace openbrewbyweight

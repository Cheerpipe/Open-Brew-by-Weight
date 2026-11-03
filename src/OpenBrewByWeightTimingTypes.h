#pragma once

#include <atomic>
#include <cstdint>

namespace openbrewbyweight {

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

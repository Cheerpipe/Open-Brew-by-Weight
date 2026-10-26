#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace openbrewbyweight {

// Lock-wait accounting for the Micra service's backflush poll (the control
// loop's input/backflush phase): the poll updates these atomics, the
// task-profiler snapshot reads them. Inline with function-local storage so
// every host-test harness that compiles only fragments links the same
// instance; this header is Micra-specific, keeping the concrete feature out
// of the common machine lifecycle.
inline std::atomic<uint32_t> &micraBackflushWaitLastUsStat() {
  static std::atomic<uint32_t> value{0};
  return value;
}
inline std::atomic<uint32_t> &micraBackflushWaitMaxUsStat() {
  static std::atomic<uint32_t> value{0};
  return value;
}
// Ownership time of the same mutex by the control task inside the poll.
inline std::atomic<uint32_t> &micraBackflushHoldLastUsStat() {
  static std::atomic<uint32_t> value{0};
  return value;
}
inline std::atomic<uint32_t> &micraBackflushHoldMaxUsStat() {
  static std::atomic<uint32_t> value{0};
  return value;
}

}  // namespace openbrewbyweight

namespace openbrewbyweight::micra_timing {

inline constexpr std::array<uint32_t, 3> kRetryDelaysMs{{3000, 6000, 9000}};
inline constexpr size_t kMaxAttempts = 1U + kRetryDelaysMs.size();
inline constexpr uint32_t kStatePollMs = 30000;
// Sample freshness tracks the poll cadence: a confirmed sample stays CURRENT
// until the next poll was due. The exhausted-cycle cooldown shares the
// optimistic overlay grace policy: two full poll intervals.
inline constexpr uint32_t kStateFreshnessMs = kStatePollMs;
inline constexpr uint32_t kOptimisticOverlayMs = 2U * kStatePollMs;
inline constexpr uint32_t kPostWakeObservationDelayMs = 15000;
inline constexpr uint32_t kExhaustedCooldownMs = kOptimisticOverlayMs;
// A machine command that exhausts this many cycles while the machine is online
// is dropped instead of re-reading the dashboard every cooldown forever.
inline constexpr uint8_t kMaxCommandCycles = 3;
inline constexpr uint32_t kGateRetryMs = 1000;
inline constexpr uint32_t kHttpTimeoutMs = 10000;
// The cloud silently drops keep-alive sockets idle for ~3-5 min (reuse
// observed fine at 189 s, dead from 309 s); reconnect before that window.
inline constexpr uint32_t kHttpReuseIdleMs = 180000;
inline constexpr uint32_t kAccessTokenLifetimeMs = 60U * 60U * 1000U;
inline constexpr uint32_t kAccessTokenRefreshAgeMs = 50U * 60U * 1000U;

inline constexpr bool accessTokenRefreshDue(uint32_t ageMs) {
  return ageMs >= kAccessTokenRefreshAgeMs;
}

inline constexpr bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

class ObservationSchedule {
 public:
  bool automaticDue(uint32_t now) const {
    return (periodic_ || postWakePending_) &&
        deadlineReached(now, automaticAtMs_) && observationAllowed(now);
  }

  bool observationAllowed(uint32_t now) const {
    return !postWakePending_ || deadlineReached(now, postWakeAtMs_);
  }

  void dueNow(uint32_t now) { automaticAtMs_ = now; periodic_ = true; }
  void suspendPeriodic() { periodic_ = false; }

  void armPostEvent(uint32_t now) {
    automaticAtMs_ = now;
    postWakeAtMs_ = now + kPostWakeObservationDelayMs;
    postWakePending_ = true;
  }

  bool observationStarted(uint32_t now) {
    if (postWakePending_ && deadlineReached(now, postWakeAtMs_)) {
      postWakePending_ = false;
      return true;
    }
    return false;
  }

  // A completed observation (including a manual refresh after rejected
  // credentials) resumes periodic polling.
  void scheduleNext(uint32_t now) {
    periodic_ = true;
    if (!postWakePending_) automaticAtMs_ = now + kStatePollMs;
  }

 private:
  uint32_t automaticAtMs_ = 0;
  uint32_t postWakeAtMs_ = 0;
  bool postWakePending_ = false;
  bool periodic_ = true;
};

static_assert(kRetryDelaysMs[0] < kRetryDelaysMs[1] &&
                  kRetryDelaysMs[1] < kRetryDelaysMs[2]);
static_assert(kStatePollMs > 0);
static_assert(kOptimisticOverlayMs == 2U * kStatePollMs);
static_assert(kPostWakeObservationDelayMs < kOptimisticOverlayMs);
static_assert(kAccessTokenRefreshAgeMs < kAccessTokenLifetimeMs);

}  // namespace openbrewbyweight::micra_timing

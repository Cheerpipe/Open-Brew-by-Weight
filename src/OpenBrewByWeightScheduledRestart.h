#pragma once

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightTimeZone.h"

namespace openbrewbyweight {

constexpr uint32_t SCHEDULED_RESTART_WINDOW_SEC = 30U * 60U;
constexpr uint32_t SCHEDULED_RESTART_QUIET_MS = 5U * 60U * 1000U;
constexpr uint32_t SCHEDULED_RESTART_MIN_UPTIME_MS = 30U * 60U * 1000U;

inline bool scheduledRestartOffsetAt(const char *zone, int64_t utcSec,
                                     int16_t &offsetMinutes,
                                     uint32_t *nextUtc = nullptr) {
  return utcSec > 0 && utcSec <= static_cast<int64_t>(UINT32_MAX) &&
         timeZoneOffsetAt(zone, static_cast<uint32_t>(utcSec), offsetMinutes,
                          nextUtc);
}

// UTC instant of a local date-time, or 0 outside the zone rules. A repeated
// local time (DST fall-back) maps to its first instant; a skipped one
// (spring-forward gap) to the transition, the first local minute that exists.
inline uint32_t scheduledRestartLocalToUtc(const char *zone, int64_t localSec) {
  int16_t before = 0, after = 0, actual = 0;
  uint32_t transition = 0;
  // Offsets span -12 h..+14 h and DST transitions are months apart, so the
  // two candidates bracket at most one transition.
  if (!scheduledRestartOffsetAt(zone, localSec - 15 * 3600, before) ||
      !scheduledRestartOffsetAt(zone, localSec + 13 * 3600, after)) {
    return 0;
  }
  const int64_t first = localSec - before * 60;
  const int64_t second = localSec - after * 60;
  if (scheduledRestartOffsetAt(zone, first, actual) && actual == before) {
    return static_cast<uint32_t>(first);
  }
  if (!scheduledRestartOffsetAt(zone, second, actual, &transition)) return 0;
  return actual == after ? static_cast<uint32_t>(second) : transition;
}

// First instant strictly after nowUtc whose local time is minuteOfDay.
inline uint32_t scheduledRestartNextUtc(const char *zone, uint32_t nowUtc,
                                        uint16_t minuteOfDay) {
  int16_t offset = 0;
  if (!scheduledRestartOffsetAt(zone, nowUtc, offset)) return 0;
  const int64_t local = static_cast<int64_t>(nowUtc) + offset * 60;
  const int64_t target = local - local % 86400 + minuteOfDay * 60;
  for (int64_t day = 0; day < 3; ++day) {
    const uint32_t utc = scheduledRestartLocalToUtc(zone, target + day * 86400);
    if (utc == 0 || utc > nowUtc) return utc;
  }
  return 0;
}

enum class ScheduledRestartState : uint8_t {
  OFF,
  NO_CLOCK,
  NO_TIMEZONE,
  ARMED,      // next occurrence known, seen ahead of it in this boot
  PENDING,    // inside the window, waiting for a quiet controller
  FIRING,     // FIRED logged; the caller drains the audit log, then submits
  SUBMITTED   // planned restart handed to the maintenance lease
};

inline const char *scheduledRestartStateName(ScheduledRestartState state) {
  switch (state) {
    case ScheduledRestartState::OFF: return "off";
    case ScheduledRestartState::NO_CLOCK: return "no_clock";
    case ScheduledRestartState::NO_TIMEZONE: return "no_timezone";
    case ScheduledRestartState::ARMED: return "armed";
    case ScheduledRestartState::PENDING: return "pending";
    case ScheduledRestartState::FIRING:
    case ScheduledRestartState::SUBMITTED: return "restarting";
  }
  return "unknown";
}

struct ScheduledRestartInput {
  bool enabled = false;
  uint16_t minuteOfDay = 0;
  const char *zone = nullptr;
  uint32_t utcSec = 0;  // 0 when the clock is unusable
  bool clockStale = false;
  uint32_t nowMs = 0;   // uptime
  bool shotActive = false;
  bool updateActive = false;
  bool imageConfirmed = true;
  bool busy = false;
};

struct ScheduledRestartLog {
  bool emit = false;
  DebugCode code = DebugCode::SCHEDULED_RESTART_ARMED;
  ScheduledRestartCause cause = ScheduledRestartCause::NONE;
};

// Control-task owned. Fires at most once per occurrence and only for an
// occurrence this boot saw coming, so a boot or a first clock sync inside the
// window never restarts. It also needs 30 min of uptime at the target, so a
// clock corrected backwards after a restart cannot fire twice in one window.
class ScheduledRestartPlanner {
 public:
  ScheduledRestartLog step(const ScheduledRestartInput &in) {
    track(in.shotActive, in.nowMs, shotSeen_, shotAtMs_);
    track(in.updateActive, in.nowMs, updateSeen_, updateAtMs_);
    longUptime_ = longUptime_ ||
                  in.nowMs >= SCHEDULED_RESTART_MIN_UPTIME_MS +
                                  SCHEDULED_RESTART_WINDOW_SEC * 1000U;
    const char *zone = in.zone != nullptr ? in.zone : "";
    if (!in.enabled || in.minuteOfDay != minute_ || strcmp(zone, zone_) != 0) {
      state_ = ScheduledRestartState::OFF;
      targetUtc_ = 0;
      minute_ = in.minuteOfDay;
      copyCString(zone_, sizeof(zone_), zone);
      if (!in.enabled) return {};
    }
    if (in.utcSec == 0) {
      if (targetUtc_ == 0) {
        state_ = ScheduledRestartState::NO_CLOCK;
        return {};
      }
      return enter(ScheduledRestartState::NO_CLOCK,
                   ScheduledRestartCause::NO_CLOCK);
    }
    if (targetUtc_ == 0) {
      targetUtc_ = scheduledRestartNextUtc(zone_, in.utcSec, minute_);
      if (targetUtc_ == 0) {
        return enter(ScheduledRestartState::NO_TIMEZONE,
                     ScheduledRestartCause::NO_TIMEZONE);
      }
      state_ = ScheduledRestartState::ARMED;
      cause_ = ScheduledRestartCause::NONE;
      return {true, DebugCode::SCHEDULED_RESTART_ARMED};
    }
    if (in.utcSec < targetUtc_) {
      state_ = ScheduledRestartState::ARMED;
      cause_ = ScheduledRestartCause::NONE;
      return {};
    }
    const uint32_t lateSec = in.utcSec - targetUtc_;
    if (lateSec >= SCHEDULED_RESTART_WINDOW_SEC) {
      // PENDING keeps its blocking cause; a lease that never restarted is
      // BUSY; otherwise the window passed unseen, without a clock.
      ScheduledRestartCause cause = ScheduledRestartCause::NO_CLOCK;
      if (state_ == ScheduledRestartState::PENDING) cause = cause_;
      if (state_ == ScheduledRestartState::FIRING ||
          state_ == ScheduledRestartState::SUBMITTED) {
        cause = ScheduledRestartCause::BUSY;
      }
      return skip(in, cause);
    }
    if (state_ == ScheduledRestartState::SUBMITTED) return {};
    if (!longUptime_ && in.nowMs / 1000U < SCHEDULED_RESTART_MIN_UPTIME_MS / 1000U +
                                               lateSec) {
      return skip(in, ScheduledRestartCause::RECENT_BOOT);
    }
    const ScheduledRestartCause blocking = blockingCause(in);
    if (blocking != ScheduledRestartCause::NONE) {
      return enter(ScheduledRestartState::PENDING, blocking);
    }
    if (state_ == ScheduledRestartState::FIRING) return {};
    state_ = ScheduledRestartState::FIRING;
    cause_ = in.clockStale ? ScheduledRestartCause::CLOCK_STALE
                           : ScheduledRestartCause::NONE;
    return {true, DebugCode::SCHEDULED_RESTART_FIRED, cause_};
  }

  void submitted() {
    if (state_ == ScheduledRestartState::FIRING) {
      state_ = ScheduledRestartState::SUBMITTED;
    }
  }

  // A shot took the lease back: it counts as shot activity, and the next
  // step re-evaluates (and logs) the deferral.
  void aborted(uint32_t nowMs) {
    if (state_ == ScheduledRestartState::FIRING ||
        state_ == ScheduledRestartState::SUBMITTED) {
      state_ = ScheduledRestartState::PENDING;
      cause_ = ScheduledRestartCause::NONE;
      track(true, nowMs, shotSeen_, shotAtMs_);
    }
  }

  ScheduledRestartState state() const { return state_; }
  ScheduledRestartCause cause() const { return cause_; }

 private:
  static void track(bool active, uint32_t nowMs, bool &seen, uint32_t &atMs) {
    if (active) {
      seen = true;
      atMs = nowMs;
    } else if (seen && nowMs - atMs >= SCHEDULED_RESTART_QUIET_MS) {
      seen = false;
    }
  }

  ScheduledRestartCause blockingCause(const ScheduledRestartInput &in) const {
    if (shotSeen_) return ScheduledRestartCause::SHOT;
    if (updateSeen_) return ScheduledRestartCause::UPDATE;
    if (!in.imageConfirmed) return ScheduledRestartCause::UPDATE_UNCONFIRMED;
    if (in.busy) return ScheduledRestartCause::BUSY;
    return ScheduledRestartCause::NONE;
  }

  // Gives up on this occurrence and arms the next one.
  ScheduledRestartLog skip(const ScheduledRestartInput &in,
                           ScheduledRestartCause cause) {
    targetUtc_ = scheduledRestartNextUtc(zone_, in.utcSec, minute_);
    state_ = targetUtc_ != 0 ? ScheduledRestartState::ARMED
                             : ScheduledRestartState::NO_TIMEZONE;
    cause_ = ScheduledRestartCause::NONE;
    return {true, DebugCode::SCHEDULED_RESTART_SKIPPED, cause};
  }

  // Logs DEFERRED once per new state or cause.
  ScheduledRestartLog enter(ScheduledRestartState state,
                            ScheduledRestartCause cause) {
    const bool changed = state_ != state || cause_ != cause;
    state_ = state;
    cause_ = cause;
    return {changed, DebugCode::SCHEDULED_RESTART_DEFERRED, cause};
  }

  ScheduledRestartState state_ = ScheduledRestartState::OFF;
  ScheduledRestartCause cause_ = ScheduledRestartCause::NONE;
  uint32_t targetUtc_ = 0;
  uint16_t minute_ = 0;
  char zone_[TIMEZONE_ID_CAPACITY] = {};
  bool shotSeen_ = false;
  bool updateSeen_ = false;
  bool longUptime_ = false;  // latched: survives the 49-day millis() wrap
  uint32_t shotAtMs_ = 0;
  uint32_t updateAtMs_ = 0;
};

}  // namespace openbrewbyweight

#include "../OpenBrewByWeightScheduledRestart.h"

#include <cassert>

using namespace openbrewbyweight;

namespace {

constexpr uint32_t JAN10 = 1768003200U;  // 2026-01-10 00:00 UTC
constexpr uint32_t HOUR = 3600U, MINUTE = 60U;
constexpr uint32_t NOON = JAN10 + 12 * HOUR;

// One controller boot: uptime follows the clock from bootUtc.
struct Boot {
  ScheduledRestartPlanner planner;
  ScheduledRestartInput in;
  uint32_t bootUtc;
  explicit Boot(uint32_t bootAtUtc, const char *zone = "Etc/UTC",
                uint16_t minute = 720)
      : bootUtc(bootAtUtc) {
    in.enabled = true;
    in.minuteOfDay = minute;
    in.zone = zone;
  }
  ScheduledRestartLog at(uint32_t utc, bool clock = true) {
    in.utcSec = clock ? utc : 0;
    in.nowMs = (utc - bootUtc) * 1000U;
    return planner.step(in);
  }
  // Steps every minute in [from, to) and returns the first log emitted.
  ScheduledRestartLog firstLog(uint32_t from, uint32_t to) {
    for (uint32_t utc = from; utc < to; utc += MINUTE) {
      const ScheduledRestartLog log = at(utc);
      if (log.emit) return log;
    }
    return {};
  }
};

bool is(const ScheduledRestartLog &log, DebugCode code,
        ScheduledRestartCause cause = ScheduledRestartCause::NONE) {
  return log.emit && log.code == code && log.cause == cause;
}

void next_occurrence_follows_local_time() {
  assert(scheduledRestartNextUtc("Etc/UTC", NOON - MINUTE, 720) == NOON);
  assert(scheduledRestartNextUtc("Etc/UTC", NOON, 720) == NOON + 24 * HOUR);
  assert(scheduledRestartNextUtc("Etc/UTC", NOON, 0) == JAN10 + 24 * HOUR);
  // Santiago is UTC-3 in January.
  assert(scheduledRestartNextUtc("America/Santiago", JAN10, 720) ==
         NOON + 3 * HOUR);
  assert(scheduledRestartNextUtc("", NOON, 720) == 0);
  assert(scheduledRestartNextUtc("Invalid/Zone", NOON, 720) == 0);
  // Spring forward: 02:30 does not exist, the transition (03:00) stands in.
  assert(scheduledRestartNextUtc("America/New_York", 1772946000U, 150) ==
         1772953200U);
  // Fall back: 01:30 happens twice; only the first counts, and the second
  // pass already looks ahead to tomorrow.
  assert(scheduledRestartNextUtc("America/New_York", 1793509200U, 90) ==
         1793511000U);
  assert(scheduledRestartNextUtc("America/New_York", 1793513100U, 90) ==
         1793601000U);
}

void idle_controller_restarts_at_the_configured_time() {
  Boot boot(NOON - 2 * HOUR);
  assert(is(boot.at(NOON - 2 * MINUTE), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(!boot.at(NOON - MINUTE).emit);
  assert(is(boot.at(NOON), DebugCode::SCHEDULED_RESTART_FIRED));
  assert(boot.planner.state() == ScheduledRestartState::FIRING);
  assert(!boot.at(NOON + 1).emit);
  boot.planner.submitted();
  boot.in.busy = true;  // the lease itself must not defer the restart
  assert(!boot.at(NOON + 2).emit);
  assert(boot.planner.state() == ScheduledRestartState::SUBMITTED);
  // A lease that never restarts gives up at the deadline.
  assert(is(boot.at(NOON + 30 * MINUTE), DebugCode::SCHEDULED_RESTART_SKIPPED,
            ScheduledRestartCause::BUSY));
  assert(boot.planner.state() == ScheduledRestartState::ARMED);
}

void a_boot_or_first_sync_inside_the_window_never_fires() {
  Boot late(NOON - 2 * HOUR);
  assert(is(late.at(NOON + 5 * MINUTE), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(!late.firstLog(NOON + 6 * MINUTE, NOON + 40 * MINUTE).emit);

  Boot noClock(NOON - 2 * HOUR);
  assert(!noClock.at(NOON - 2 * MINUTE, false).emit);
  assert(noClock.planner.state() == ScheduledRestartState::NO_CLOCK);
  assert(!noClock.at(NOON, false).emit);
  assert(is(noClock.at(NOON + 2 * MINUTE), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(!noClock.firstLog(NOON + 3 * MINUTE, NOON + 40 * MINUTE).emit);
}

void clock_lost_after_arming() {
  Boot regained(NOON - 2 * HOUR);
  assert(is(regained.at(NOON - MINUTE), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(is(regained.at(NOON, false), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::NO_CLOCK));
  assert(!regained.at(NOON + MINUTE, false).emit);
  assert(is(regained.at(NOON + 10 * MINUTE), DebugCode::SCHEDULED_RESTART_FIRED));

  Boot late(NOON - 2 * HOUR);
  assert(late.at(NOON - MINUTE).emit);
  assert(late.at(NOON, false).emit);
  assert(is(late.at(NOON + 31 * MINUTE), DebugCode::SCHEDULED_RESTART_SKIPPED,
            ScheduledRestartCause::NO_CLOCK));
  assert(late.planner.state() == ScheduledRestartState::ARMED);
}

void shots_defer_until_five_quiet_minutes() {
  Boot boot(NOON - 2 * HOUR);
  assert(boot.at(NOON - MINUTE).emit);
  boot.in.shotActive = true;
  assert(is(boot.at(NOON), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::SHOT));
  assert(!boot.at(NOON + 3 * MINUTE).emit);
  boot.in.shotActive = false;
  assert(!boot.firstLog(NOON + 4 * MINUTE, NOON + 8 * MINUTE).emit);
  assert(is(boot.at(NOON + 8 * MINUTE), DebugCode::SCHEDULED_RESTART_FIRED));

  // Activity just before the target also counts.
  Boot before(NOON - 2 * HOUR);
  assert(before.at(NOON - 3 * MINUTE).emit);
  before.in.shotActive = true;
  assert(!before.at(NOON - 2 * MINUTE).emit);
  before.in.shotActive = false;
  assert(is(before.at(NOON), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::SHOT));
  assert(is(before.firstLog(NOON + MINUTE, NOON + 10 * MINUTE),
            DebugCode::SCHEDULED_RESTART_FIRED));

  // A shot ending at +27 min leaves no quiet window: skip for the day.
  Boot busy(NOON - 2 * HOUR);
  assert(busy.at(NOON - MINUTE).emit);
  busy.in.shotActive = true;
  assert(busy.at(NOON).emit);
  assert(!busy.at(NOON + 27 * MINUTE).emit);
  busy.in.shotActive = false;
  assert(!busy.firstLog(NOON + 28 * MINUTE, NOON + 30 * MINUTE).emit);
  assert(is(busy.at(NOON + 30 * MINUTE), DebugCode::SCHEDULED_RESTART_SKIPPED,
            ScheduledRestartCause::SHOT));
}

void updates_and_unconfirmed_images_defer() {
  Boot boot(NOON - 2 * HOUR);
  assert(boot.at(NOON - MINUTE).emit);
  boot.in.updateActive = true;
  assert(is(boot.at(NOON), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::UPDATE));
  boot.in.updateActive = false;
  boot.in.imageConfirmed = false;
  assert(!boot.at(NOON + 4 * MINUTE).emit);
  assert(is(boot.at(NOON + 5 * MINUTE), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::UPDATE_UNCONFIRMED));
  boot.in.imageConfirmed = true;
  boot.in.busy = true;
  assert(is(boot.at(NOON + 6 * MINUTE), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::BUSY));
  boot.in.busy = false;
  assert(is(boot.at(NOON + 7 * MINUTE), DebugCode::SCHEDULED_RESTART_FIRED));
}

void recent_boot_skips_the_day() {
  Boot boot(NOON - 30 * MINUTE);
  assert(boot.at(NOON - 29 * MINUTE).emit);
  assert(is(boot.at(NOON), DebugCode::SCHEDULED_RESTART_FIRED));

  Boot recent(NOON - 10 * MINUTE);
  assert(recent.at(NOON - 9 * MINUTE).emit);
  assert(is(recent.at(NOON), DebugCode::SCHEDULED_RESTART_SKIPPED,
            ScheduledRestartCause::RECENT_BOOT));
  assert(!recent.firstLog(NOON + MINUTE, NOON + HOUR).emit);

  // A clock running ahead fired early; NTP then corrects it backwards. The
  // next boot sees the target again but never reaches it with 30 min uptime.
  Boot again(NOON - 2 * MINUTE);
  assert(is(again.at(NOON - MINUTE), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(is(again.at(NOON), DebugCode::SCHEDULED_RESTART_SKIPPED,
            ScheduledRestartCause::RECENT_BOOT));
  assert(!again.firstLog(NOON + MINUTE, NOON + 31 * MINUTE).emit);

  // Late in the window the uptime bar is measured at the target, not now.
  Boot shot(NOON - 31 * MINUTE);
  assert(shot.at(NOON - 30 * MINUTE).emit);
  shot.in.shotActive = true;
  assert(is(shot.at(NOON), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::SHOT));
  shot.in.shotActive = false;
  assert(is(shot.firstLog(NOON + MINUTE, NOON + 10 * MINUTE),
            DebugCode::SCHEDULED_RESTART_FIRED));
}

void stale_clock_still_fires_and_says_so() {
  Boot boot(NOON - 2 * HOUR);
  boot.in.clockStale = true;
  assert(boot.at(NOON - MINUTE).emit);
  assert(is(boot.at(NOON), DebugCode::SCHEDULED_RESTART_FIRED,
            ScheduledRestartCause::CLOCK_STALE));
}

void missing_time_zone_keeps_the_feature_dormant() {
  Boot boot(NOON - 2 * HOUR, "");
  assert(is(boot.at(NOON - MINUTE), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::NO_TIMEZONE));
  assert(boot.planner.state() == ScheduledRestartState::NO_TIMEZONE);
  assert(!boot.firstLog(NOON, NOON + HOUR).emit);
  boot.in.zone = "Etc/UTC";
  assert(is(boot.at(NOON + HOUR), DebugCode::SCHEDULED_RESTART_ARMED));
}

void settings_changes_never_fire_retroactively() {
  Boot boot(NOON - 2 * HOUR);
  boot.in.enabled = false;
  assert(!boot.firstLog(NOON - 5 * MINUTE, NOON + 10 * MINUTE).emit);
  assert(boot.planner.state() == ScheduledRestartState::OFF);
  boot.in.enabled = true;
  assert(is(boot.at(NOON + 10 * MINUTE), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(!boot.firstLog(NOON + 11 * MINUTE, NOON + HOUR).emit);
  // Moving the time just ahead re-arms for today.
  boot.in.minuteOfDay = 13 * 60 + 5;
  assert(is(boot.at(NOON + HOUR), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(is(boot.at(NOON + HOUR + 5 * MINUTE), DebugCode::SCHEDULED_RESTART_FIRED));
}

void a_shot_taking_the_lease_back_reevaluates() {
  Boot boot(NOON - 2 * HOUR);
  assert(boot.at(NOON - MINUTE).emit);
  assert(is(boot.at(NOON), DebugCode::SCHEDULED_RESTART_FIRED));
  boot.planner.submitted();
  // A paddle tap between two steps still counts as shot activity.
  boot.planner.aborted(boot.in.nowMs);
  assert(is(boot.at(NOON + 1), DebugCode::SCHEDULED_RESTART_DEFERRED,
            ScheduledRestartCause::SHOT));
  assert(!boot.firstLog(NOON + MINUTE, NOON + 5 * MINUTE).emit);
  assert(is(boot.at(NOON + 5 * MINUTE), DebugCode::SCHEDULED_RESTART_FIRED));
}

void the_window_may_cross_midnight() {
  const uint32_t target = JAN10 + 23 * HOUR + 50 * MINUTE;
  Boot boot(JAN10 + 20 * HOUR, "Etc/UTC", 23 * 60 + 50);
  assert(boot.at(target - 5 * MINUTE).emit);
  boot.in.shotActive = true;
  assert(boot.at(target).emit);
  assert(!boot.at(target + 15 * MINUTE).emit);  // 00:05 next day
  boot.in.shotActive = false;
  assert(is(boot.firstLog(target + 16 * MINUTE, target + 30 * MINUTE),
            DebugCode::SCHEDULED_RESTART_FIRED));
}

void fall_back_repeat_does_not_fire_twice() {
  // 01:30 New York on 2026-11-01: first pass 05:30 UTC (EDT), second 06:30.
  Boot first(1793509200U - 2 * HOUR, "America/New_York", 90);
  assert(first.at(1793511000U - MINUTE).emit);
  assert(is(first.at(1793511000U), DebugCode::SCHEDULED_RESTART_FIRED));
  // After that restart the controller boots during the second pass.
  Boot second(1793511000U + MINUTE, "America/New_York", 90);
  assert(is(second.at(1793513100U), DebugCode::SCHEDULED_RESTART_ARMED));
  assert(!second.firstLog(1793513100U + MINUTE, 1793513100U + 2 * HOUR).emit);
}

}  // namespace

int main() {
  next_occurrence_follows_local_time();
  idle_controller_restarts_at_the_configured_time();
  a_boot_or_first_sync_inside_the_window_never_fires();
  clock_lost_after_arming();
  shots_defer_until_five_quiet_minutes();
  updates_and_unconfirmed_images_defer();
  recent_boot_skips_the_day();
  stale_clock_still_fires_and_says_so();
  missing_time_zone_keeps_the_feature_dormant();
  settings_changes_never_fire_retroactively();
  a_shot_taking_the_lease_back_reevaluates();
  the_window_may_cross_midnight();
  fall_back_repeat_does_not_fire_twice();
}

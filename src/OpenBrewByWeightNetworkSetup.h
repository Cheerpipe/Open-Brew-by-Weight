#pragma once

// /setup onboarding transaction: one explicit phase, owned by the network
// task and published in NetworkStatusSnapshot, instead of a set of booleans.
// Pure: no I/O, no clock; the network task feeds events and applies the
// returned decisions.

#include <cstdint>

namespace openbrewbyweight {

enum class SetupPhase : uint8_t {
  NONE,        // no onboarding session; the SoftAP follows boot/recovery rules
  KEEP,        // USB AP_START keeps the SoftAP until AP_STOP or a confirm
  QUEUED,      // /setup saved a network with live apply; not associating yet
  ATTEMPTING,  // associating with the saved (PENDING) network
  FAILED,      // that attempt failed; reconnects held until the page retries
  JOINED,      // this attempt got a lease on the saved SSID; awaits "Save Wi-Fi"
  CONFIRMED,   // saved network persisted; the SoftAP closes after a grace
};

enum class SetupEvent : uint8_t {
  KEEP_REQUESTED,   // AP_START succeeded
  KEEP_RELEASED,    // AP_STOP, or AP_START failed
  APPLY_SAVED,      // /setup save with apply:"live" persisted as PENDING
  ATTEMPT_STARTED,  // the association for the saved network began
  ATTEMPT_FAILED,   // terminal driver failure, aged attempt, or RF abort
  JOINED,           // STA lease on the saved SSID
  JOINED_OTHER,     // STA lease, but not on the saved SSID
  CONFIRMED,        // PENDING persisted as CONFIRMED
  REVERTED,         // PENDING rolled back to the last known good network
  APPLY_RESTARTS,   // the AP left before a queued apply: save-and-restart
  AP_STOPPED,       // the SoftAP went down
};

constexpr SetupPhase nextSetupPhase(SetupPhase phase, SetupEvent event) {
  switch (event) {
    case SetupEvent::KEEP_REQUESTED:
      return phase == SetupPhase::NONE || phase == SetupPhase::CONFIRMED
                 ? SetupPhase::KEEP : phase;
    case SetupEvent::KEEP_RELEASED:
      // A queued apply survives: the network task falls back to the
      // save-and-restart path once it sees the AP gone.
      return phase == SetupPhase::QUEUED ? phase : SetupPhase::NONE;
    case SetupEvent::APPLY_SAVED:
      return SetupPhase::QUEUED;
    case SetupEvent::ATTEMPT_STARTED:
      return phase == SetupPhase::QUEUED ? SetupPhase::ATTEMPTING : phase;
    case SetupEvent::ATTEMPT_FAILED:
      return phase == SetupPhase::ATTEMPTING ? SetupPhase::FAILED : phase;
    case SetupEvent::JOINED:
      // A held attempt the driver completed late still proves the network.
      return phase == SetupPhase::ATTEMPTING || phase == SetupPhase::FAILED
                 ? SetupPhase::JOINED : phase;
    case SetupEvent::JOINED_OTHER:
      return phase == SetupPhase::ATTEMPTING ? SetupPhase::FAILED : phase;
    case SetupEvent::CONFIRMED:
      return phase == SetupPhase::NONE ? phase : SetupPhase::CONFIRMED;
    case SetupEvent::REVERTED:
    case SetupEvent::APPLY_RESTARTS:
      return SetupPhase::NONE;
    case SetupEvent::AP_STOPPED:
      return phase == SetupPhase::CONFIRMED ? SetupPhase::NONE : phase;
  }
  return phase;
}

// The SoftAP survives STA connect and the idle timeout.
constexpr bool setupKeepsSoftAp(SetupPhase phase) {
  return phase != SetupPhase::NONE && phase != SetupPhase::CONFIRMED;
}

// The page owns the next attempt; background reconnects would abort its scans.
constexpr bool setupHoldsReconnect(SetupPhase phase) {
  return phase == SetupPhase::FAILED;
}

// A PENDING network may be persisted only once this attempt joined it (or
// outside onboarding, with the SoftAP down). Untested credentials and an old
// association that outlived the save never confirm.
constexpr bool setupConfirmAllowed(SetupPhase phase, bool apActive) {
  return phase == SetupPhase::JOINED || phase == SetupPhase::KEEP ||
         ((phase == SetupPhase::NONE || phase == SetupPhase::CONFIRMED) &&
          !apActive);
}

// A joined onboarding attempt is saved only by the owner's "Save Wi-Fi" (the
// confirm action); status polls and the UI stream do not confirm it.
constexpr bool setupAwaitsOwnerSave(SetupPhase phase, bool apActive) {
  return apActive && phase == SetupPhase::JOINED;
}

// "Keep" or "Continue without Wi-Fi" drops this session's unsaved test now
// instead of at its window's end, once the attempt has begun.
constexpr bool setupDiscardAllowed(SetupPhase phase) {
  return phase == SetupPhase::ATTEMPTING || phase == SetupPhase::FAILED ||
         phase == SetupPhase::JOINED;
}

// "finish" from the summary: valid while the saved network's teardown
// deadline is still ahead; an unsaved joined attempt is not finished.
constexpr bool setupFinishAllowed(SetupPhase phase, bool apActive,
                                  uint32_t teardownAtMs, uint32_t now) {
  return apActive && phase != SetupPhase::JOINED && teardownAtMs != 0 &&
         static_cast<int32_t>(teardownAtMs - now) > 0;
}

enum class SoftApTeardown : uint8_t { KEEP, ARM_GRACE, STOP };

// Station is up and the SoftAP is still raised. Without a deadline, a phone
// still associated gets the grace window instead of losing the page mid-step.
constexpr SoftApTeardown softApTeardownStep(SetupPhase phase, uint8_t apClients,
                                            uint32_t teardownAtMs,
                                            uint32_t now) {
  return setupKeepsSoftAp(phase) ? SoftApTeardown::KEEP
         : teardownAtMs == 0
             ? (apClients > 0 ? SoftApTeardown::ARM_GRACE : SoftApTeardown::STOP)
         : static_cast<int32_t>(now - teardownAtMs) >= 0 ? SoftApTeardown::STOP
                                                          : SoftApTeardown::KEEP;
}

constexpr const char *setupPhaseName(SetupPhase phase) {
  switch (phase) {
    case SetupPhase::NONE: return "none";
    case SetupPhase::KEEP: return "keep";
    case SetupPhase::QUEUED: return "queued";
    case SetupPhase::ATTEMPTING: return "connecting";
    case SetupPhase::FAILED: return "failed";
    case SetupPhase::JOINED: return "joined";
    case SetupPhase::CONFIRMED: return "confirmed";
  }
  return "none";
}

// Onboarding brewing choice, offered only while the firmware is off: saved by
// part one of setup (the "brew" action) and published so the page can resume.
enum class SetupBrew : uint8_t { NONE, USUAL, BY_WEIGHT };

constexpr const char *setupBrewName(SetupBrew brew) {
  return brew == SetupBrew::USUAL       ? "usual"
         : brew == SetupBrew::BY_WEIGHT ? "weight"
                                        : "none";
}

}  // namespace openbrewbyweight

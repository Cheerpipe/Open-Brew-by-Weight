#pragma once

#include <stdint.h>

namespace openbrewbyweight {

constexpr uint32_t RECOVERY_MODE_WINDOW_MS = 60000;
constexpr uint32_t RECOVERY_GESTURE_WINDOW_MS = 5000;
constexpr uint32_t RECOVERY_CONFIRMATION_MS = 3000;
constexpr uint8_t RECOVERY_NETWORK_CYCLES = 3;
constexpr uint8_t RECOVERY_FACTORY_CYCLES = 5;
constexpr uint32_t RECOVERY_ENTRY_GRACE_MS = 30000;
constexpr uint32_t RECOVERY_GRACE_BEEP_INTERVAL_MS = 5000;
static_assert(RECOVERY_ENTRY_GRACE_MS % RECOVERY_GRACE_BEEP_INTERVAL_MS == 0,
              "Grace expiry must fall on a beep boundary without beeping");

enum class RecoveryGestureResult : uint8_t {
  NONE = 0,
  NETWORK_ACCESS_RESET,
  FACTORY_RESET,
  TIMED_OUT,
};

inline bool recoveryGestureEntryAllowed(bool powerOnReset,
                                        bool activatorStablyOn) {
  return powerOnReset && activatorStablyOn;
}

enum class RecoveryGraceResult : uint8_t {
  HOLDING = 0,
  BEEP_DUE,
  ABORTED,
  ELAPSED,
};

// Entry grace between the power-on hold detection and the irreversible
// gesture recognizer: the activator must stay held for the whole window.
// Each interval boundary sounds one reminder cue; boundaries crossed while
// the caller was late are skipped, never replayed as a burst. Unsigned
// subtraction keeps deadlines safe across millis() wraparound, and release
// is checked before expiry so a coincident tick cancels the entry instead
// of starting recovery.
struct RecoveryGraceWindow {
  bool active = false;
  uint8_t beepsEmitted = 0;
  uint32_t startedAtMs = 0;

  void begin(uint32_t nowMs) {
    active = true;
    beepsEmitted = 0;
    startedAtMs = nowMs;
  }

  RecoveryGraceResult update(uint32_t nowMs, bool activatorHeld) {
    if (!active) {
      return RecoveryGraceResult::HOLDING;
    }
    if (!activatorHeld) {
      active = false;
      return RecoveryGraceResult::ABORTED;
    }
    const uint32_t elapsed = nowMs - startedAtMs;
    if (elapsed >= RECOVERY_ENTRY_GRACE_MS) {
      active = false;
      return RecoveryGraceResult::ELAPSED;
    }
    const uint32_t boundariesCrossed =
        elapsed / RECOVERY_GRACE_BEEP_INTERVAL_MS;
    if (boundariesCrossed >= beepsEmitted) {
      beepsEmitted = static_cast<uint8_t>(boundariesCrossed) + 1;
      return RecoveryGraceResult::BEEP_DUE;
    }
    return RecoveryGraceResult::HOLDING;
  }
};

// Boot-local recognizer. Inputs are already debounced by the firmware. A
// gesture begins on the first OFF edge and counts complete OFF->ON cycles.
// Unsigned subtraction deliberately keeps all deadlines safe across millis()
// wraparound.
struct RecoveryGestureRecognizer {
  bool active = false;
  bool attemptActive = false;
  uint8_t completedCycles = 0;
  uint32_t modeStartedAtMs = 0;
  uint32_t attemptStartedAtMs = 0;
  uint32_t lastTransitionAtMs = 0;

  void begin(uint32_t nowMs) {
    active = true;
    attemptActive = false;
    completedCycles = 0;
    modeStartedAtMs = nowMs;
    attemptStartedAtMs = 0;
    lastTransitionAtMs = nowMs;
  }

  void resetAttempt() {
    attemptActive = false;
    completedCycles = 0;
    attemptStartedAtMs = 0;
  }

  RecoveryGestureResult update(uint32_t nowMs, bool activatorHeld,
                               bool turnedOn, bool turnedOff) {
    if (!active) {
      return RecoveryGestureResult::NONE;
    }
    if (static_cast<uint32_t>(nowMs - modeStartedAtMs) >=
        RECOVERY_MODE_WINDOW_MS) {
      active = false;
      resetAttempt();
      return RecoveryGestureResult::TIMED_OUT;
    }

    const bool transitioned = turnedOn || turnedOff;
    if (transitioned && !attemptActive) {
      if (!turnedOff) {
        return RecoveryGestureResult::NONE;
      }
      attemptActive = true;
      completedCycles = 0;
      attemptStartedAtMs = nowMs;
      lastTransitionAtMs = nowMs;
      return RecoveryGestureResult::NONE;
    }
    if (!attemptActive) {
      return RecoveryGestureResult::NONE;
    }

    if (transitioned) {
      const bool insideGestureWindow =
          static_cast<uint32_t>(nowMs - attemptStartedAtMs) <=
          RECOVERY_GESTURE_WINDOW_MS;
      if (!insideGestureWindow) {
        resetAttempt();
        // An OFF edge can also be the first edge of a fresh attempt.
        if (turnedOff) {
          attemptActive = true;
          attemptStartedAtMs = nowMs;
          lastTransitionAtMs = nowMs;
        }
        return RecoveryGestureResult::NONE;
      }
      lastTransitionAtMs = nowMs;
      if (turnedOn) {
        ++completedCycles;
        if (completedCycles > RECOVERY_FACTORY_CYCLES) {
          resetAttempt();
        }
      }
      return RecoveryGestureResult::NONE;
    }

    const bool candidate = activatorHeld &&
                           (completedCycles == RECOVERY_NETWORK_CYCLES ||
                            completedCycles == RECOVERY_FACTORY_CYCLES);
    if (candidate &&
        static_cast<uint32_t>(nowMs - lastTransitionAtMs) >=
            RECOVERY_CONFIRMATION_MS) {
      const RecoveryGestureResult result =
          completedCycles == RECOVERY_FACTORY_CYCLES
              ? RecoveryGestureResult::FACTORY_RESET
              : RecoveryGestureResult::NETWORK_ACCESS_RESET;
      active = false;
      resetAttempt();
      return result;
    }

    // Non-candidates expire when their five-second movement window closes.
    // Valid 3/5-cycle candidates may finish their separate confirmation wait
    // after the movement window, but still before the 60-second mode deadline.
    if (!candidate &&
        static_cast<uint32_t>(nowMs - attemptStartedAtMs) >
            RECOVERY_GESTURE_WINDOW_MS) {
      resetAttempt();
    }
    return RecoveryGestureResult::NONE;
  }
};

}  // namespace openbrewbyweight

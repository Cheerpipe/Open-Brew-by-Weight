#pragma once

#include "OpenBrewByWeightHardware.h"
#include "OpenBrewByWeightMachineTypes.h"
#include "OpenBrewByWeightResourceOwner.h"
#include "OpenBrewByWeightSafety.h"
#include "OpenBrewByWeightCrashArchive.h"

// K1 electrical driver and independent deadline/feedback safety.
// Included from OpenBrewByWeightMachine.h in the openBrewByWeight.cpp translation unit.
// State is file-scope BSS — no heap.
//
// BOUNDARY: Shared electrical layer under the machine façade. Paddle and
// momentary specializations request close/open through this driver; brew and
// guards do not call it directly (except via machineRequestStart/Stop). Safety
// may open the circuit without waiting for brew policy.

// Logging owns no control state. Closing K1 conservatively latches the atomic
// publication consumed by cross-core log producers; control clears it only
// after observing both the session and relay idle.
void latchControlCriticalLogging();

bool readCircuitFeedbackClosed() {
  return EXTERNAL_SAFETY_HARDWARE_PRESENT &&
         digitalRead(CIRCUIT_FEEDBACK_GPIO) == CIRCUIT_FEEDBACK_CLOSED_LEVEL;
}

void stopRelayDeadlineTimers() {
  independentSafetyTimer.stop();
  if (relaySafetyTimer != nullptr) {
    (void)esp_timer_stop(relaySafetyTimer);
  }
  if (operationalLimitTimer != nullptr) {
    (void)esp_timer_stop(operationalLimitTimer);
  }
}

void tripRelaySafetyLocked(RelaySafetyFault fault, bool hardLimit,
                           bool operationalLimit, bool lockout) {
  // The electrical action is deliberately first. State publication, logging,
  // timer cleanup and recovery all happen after the relay is de-energized.
  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
  if (circuitClosed) relayElectricalOpenedAtMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  circuitClosed = false;
  ++relaySafetyGeneration;
  relaySafetyState = lockout ? RelaySafetyState::LOCKOUT
                             : RelaySafetyState::TRIPPED;
  relaySafetyFault = fault;
  relaySafetyTripped = relaySafetyTripped || hardLimit;
  operationalLimitTripped =
      operationalLimitTripped || operationalLimit;
  feedbackExpectedClosed = false;
  feedbackTransitionPending = EXTERNAL_SAFETY_HARDWARE_PRESENT;
  feedbackTransitionStartedAtMs = millis();
  feedbackTransitionStampPending = false;
}

void tripRelaySafety(RelaySafetyFault fault, bool hardLimit = false,
                     bool operationalLimit = false, bool lockout = true) {
  portENTER_CRITICAL(&relayMux);
  tripRelaySafetyLocked(fault, hardLimit, operationalLimit, lockout);
  portEXIT_CRITICAL(&relayMux);
  // RTC validation/checksum is task work and must never extend the interrupt-
  // disabled relay critical section. A reset before this OPEN publication
  // is only classified as a reset during close in the reset history.
  recordRelayCommandedClosed(false);
  stopRelayDeadlineTimers();
}

void relaySafetyTimerCallback(void *) {
  const uint32_t callbackAtMs = millis();
  bool tripped = false;
  portENTER_CRITICAL(&relayMux);
  // Ignore a callback queued by a previous generation. ARMING is included so
  // a timeout that races the close transaction cancels that transaction.
  if (relayHardLimitMs != 0 &&
      (relaySafetyState == RelaySafetyState::ARMING ||
       relaySafetyState == RelaySafetyState::CLOSED) &&
      static_cast<uint32_t>(callbackAtMs - circuitClosedAtMs) >=
          relayHardLimitMs) {
    tripRelaySafetyLocked(RelaySafetyFault::HARD_LIMIT, true, false, false);
    tripped = true;
  }
  portEXIT_CRITICAL(&relayMux);
  if (tripped) {
    recordRelayCommandedClosed(false);
  }
}

void operationalLimitTimerCallback(void *) {
  const uint32_t callbackAtMs = millis();
  bool tripped = false;
  portENTER_CRITICAL(&relayMux);
  if (operationalLimitAtArmMs != 0 &&
      (relaySafetyState == RelaySafetyState::ARMING ||
       relaySafetyState == RelaySafetyState::CLOSED) &&
      operationalLimitAtArmMs < relayHardLimitMs &&
      static_cast<uint32_t>(callbackAtMs - circuitClosedAtMs) >=
          operationalLimitAtArmMs) {
    tripRelaySafetyLocked(RelaySafetyFault::OPERATIONAL_LIMIT, false, true,
                          false);
    tripped = true;
  }
  portEXIT_CRITICAL(&relayMux);
  if (tripped) {
    recordRelayCommandedClosed(false);
  }
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
void IRAM_ATTR writeGpioFromIsr(uint8_t pin, uint8_t level) {
  const uint32_t mask = (pin < 32) ? (uint32_t{1} << pin)
                                   : (uint32_t{1} << (pin - 32));
  if (level == LOW) {
    if (pin < 32) {
      REG_WRITE(GPIO_OUT_W1TC_REG, mask);
    }
#ifdef GPIO_OUT1_W1TC_REG
    else {
      REG_WRITE(GPIO_OUT1_W1TC_REG, mask);
    }
#endif
  } else {
    if (pin < 32) {
      REG_WRITE(GPIO_OUT_W1TS_REG, mask);
    }
#ifdef GPIO_OUT1_W1TS_REG
    else {
      REG_WRITE(GPIO_OUT1_W1TS_REG, mask);
    }
#endif
  }
}

void IRAM_ATTR openRelayElectricalFromIsr() {
  // Arduino-ESP32 does not place digitalWrite()/gpio_set_level() in IRAM in
  // the standard board configurations. Write the GPIO register directly so
  // the GPTimer deadline can de-energize K1 while flash cache is disabled.
  // Supports both active-HIGH and active-LOW relay modules: the correct
  // register (W1TS or W1TC) is selected at compile time via RELAY_OPEN_LEVEL.
  writeGpioFromIsr(RELAY_GPIO, RELAY_OPEN_LEVEL);
}
#else
void openRelayElectricalFromIsr() {
  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
}
#endif

// Replaces ESP-IDF's weak hook, which only aborts, so this one must abort too:
// returning would keep running on a corrupted stack. Open the machine circuit
// first (IRAM GPIO path) and clear the RTC close marker; avoid heap or Serial.
extern "C" void vApplicationStackOverflowHook(TaskHandle_t, char *) {
  openRelayElectricalFromIsr();
  recordRelayCommandedClosed(false);
  esp_system_abort("stack overflow");
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
void IRAM_ATTR openBrewByWeightPanicHandler(arduino_panic_info_t *info, void *) {
  // The Arduino core invokes this before its normal panic/reboot path. Keep
  // it allocation-free and flash-independent: only de-energize K1 through
  // the same direct-register path used by the independent safety timer.
  openRelayElectricalFromIsr();
  openbrewbyweight::crashArchiveRecordPanic(info);
  openbrewbyweight::shotTraceWriteBackForPanic();
}
#endif

void IRAM_ATTR independentSafetyTimerCallback(void *) {
  portENTER_CRITICAL_ISR(&relayMux);
  if (relayHardLimitMs != 0 &&
      (relaySafetyState == RelaySafetyState::ARMING ||
       relaySafetyState == RelaySafetyState::CLOSED)) {
    const bool operational =
        operationalLimitAtArmMs < relayHardLimitMs;
    // Keep this ISR minimal and IRAM-safe. The control task performs timer
    // cleanup, RTC OPEN publication and logging when it consumes the latched
    // trip flags. If reset wins that race, the retained CLOSE marker is only
    // classified as a reset during close.
    openRelayElectricalFromIsr();
    if (circuitClosed) relayElectricalOpenedAtMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    circuitClosed = false;
    ++relaySafetyGeneration;
    relaySafetyState = RelaySafetyState::TRIPPED;
    relaySafetyFault = operational ? RelaySafetyFault::OPERATIONAL_LIMIT
                                   : RelaySafetyFault::HARD_LIMIT;
    relaySafetyTripped = !operational;
    operationalLimitTripped = operational;
    feedbackExpectedClosed = false;
    feedbackTransitionPending = EXTERNAL_SAFETY_HARDWARE_PRESENT;
    feedbackTransitionStampPending = EXTERNAL_SAFETY_HARDWARE_PRESENT;
  }
  portEXIT_CRITICAL_ISR(&relayMux);
}

bool initializeRelaySafetyTimer() {
  if (relaySafetyTimer != nullptr || operationalLimitTimer != nullptr) {
    return false;
  }
  esp_timer_create_args_t hardArgs = {};
  hardArgs.callback = &relaySafetyTimerCallback;
  hardArgs.arg = nullptr;
  hardArgs.dispatch_method = ESP_TIMER_TASK;
  hardArgs.name = "circuit_hard_limit";
  esp_timer_handle_t hardHandle = nullptr;
  if (esp_timer_create(&hardArgs, &hardHandle) != ESP_OK) {
    return false;
  }
  struct TimerRollbackDeleter {
    void operator()(esp_timer_handle_t handle) const {
      (void)esp_timer_stop(handle);
      (void)esp_timer_delete(handle);
    }
  };
  using TimerRollbackOwner =
      openbrewbyweight::UniqueResource<esp_timer_handle_t, TimerRollbackDeleter>;
  TimerRollbackOwner hardOwner(hardHandle);

  esp_timer_create_args_t operationalArgs = {};
  operationalArgs.callback = &operationalLimitTimerCallback;
  operationalArgs.arg = nullptr;
  operationalArgs.dispatch_method = ESP_TIMER_TASK;
  operationalArgs.name = "circuit_oper_limit";
  esp_timer_handle_t operationalHandle = nullptr;
  if (esp_timer_create(&operationalArgs, &operationalHandle) != ESP_OK) {
    return false;
  }
  TimerRollbackOwner operationalOwner(operationalHandle);
  if (!independentSafetyTimer.begin(&independentSafetyTimerCallback,
                                    nullptr)) {
    return false;
  }
  relaySafetyTimer = hardOwner.release();
  operationalLimitTimer = operationalOwner.release();
  return true;
}

// The one field most callers need, without the full snapshot's reset-history
// copy and feedback GPIO read; same lock, same value as snapshot.closed.
bool relayCircuitClosed() {
  portENTER_CRITICAL(&relayMux);
  const bool closed = circuitClosed;
  portEXIT_CRITICAL(&relayMux);
  return closed;
}

RelaySafetySnapshot getRelaySafetySnapshot() {
  RelaySafetySnapshot snapshot;
  portENTER_CRITICAL(&relayMux);
  snapshot.state = relaySafetyState;
  snapshot.fault = relaySafetyFault;
  snapshot.closed = circuitClosed;
  snapshot.commandedClosed = relaySafetyState == RelaySafetyState::ARMING ||
                             relaySafetyState == RelaySafetyState::CLOSED;
  snapshot.feedbackAvailable = EXTERNAL_SAFETY_HARDWARE_PRESENT;
  snapshot.externalSafetyPresent = EXTERNAL_SAFETY_HARDWARE_PRESENT;
  snapshot.watchdogReady = taskWatchdogReady;
  snapshot.timersReady = relaySafetyTimersReady;
  snapshot.tripped = relaySafetyTripped;
  snapshot.operationalTripped = operationalLimitTripped;
  snapshot.generation = relaySafetyGeneration;
  snapshot.closedAtMs = circuitClosedAtMs;
  snapshot.operationalLimitMs = operationalLimitAtArmMs;
  snapshot.hardLimitMs = relayHardLimitMs;
  snapshot.purpose = relayPurpose;
  snapshot.electricalClosedAtMs = relayElectricalClosedAtMs;
  snapshot.electricalOpenedAtMs = relayElectricalOpenedAtMs;
  portEXIT_CRITICAL(&relayMux);
  // Reset history is immutable after boot. Keep the bounded array copy out of
  // the ISR-shared spinlock.
  snapshot.resetReasonCode = safetyResetStatus.reasonCode;
  snapshot.unsafeResetCount = safetyResetStatus.unsafeResetCount;
  snapshot.bootLoopDetected = safetyResetStatus.bootLoopDetected;
  snapshot.resetHistoryCount = safetyResetStatus.resetHistoryCount;
  for (uint8_t i = 0; i < snapshot.resetHistoryCount; ++i)
    snapshot.resetHistory[i] = safetyResetStatus.resetHistory[i];
  snapshot.feedbackClosed = readCircuitFeedbackClosed();
  return snapshot;
}

bool relayOutputIsClosed() {
  return digitalRead(RELAY_GPIO) == RELAY_CLOSED_LEVEL;
}

void writeRelayClosedOutput() {
  digitalWrite(RELAY_GPIO, RELAY_CLOSED_LEVEL);
}

bool reassertCommandedRelayClosedPin() {
  portENTER_CRITICAL(&relayMux);
  const bool expired = circuitClosed && operationalLimitAtArmMs != 0 &&
      elapsedMs(circuitClosedAtMs) >= operationalLimitAtArmMs;
  if (expired) {
    const bool operational = operationalLimitAtArmMs < relayHardLimitMs;
    tripRelaySafetyLocked(operational ? RelaySafetyFault::OPERATIONAL_LIMIT : RelaySafetyFault::HARD_LIMIT,
                          !operational, operational, false);
  }
  const bool live = circuitClosed && relaySafetyState == RelaySafetyState::CLOSED &&
      (operationalLimitAtArmMs == 0 || elapsedMs(circuitClosedAtMs) < operationalLimitAtArmMs);
  const bool desync = live && !relayOutputIsClosed();
  if (desync) writeRelayClosedOutput();
  const bool closed = live && relayOutputIsClosed();
  portEXIT_CRITICAL(&relayMux);
  if (expired) { recordRelayCommandedClosed(false); stopRelayDeadlineTimers(); }
  if (desync) addDebugEvent(DebugCategory::RELAY, DebugCode::RELAY_GPIO_DESYNC);
  if (live && !closed) tripRelaySafety(RelaySafetyFault::GPIO_DESYNC, true, false, false);
  return closed;
}

bool setMachineCircuitClosed(bool closed,
                  uint32_t operationalLimitMs = HARD_MAX_CIRCUIT_CLOSED_MS,
                  RelayPurpose purpose = RelayPurpose::NORMAL) {
  if (closed) {
    latchControlCriticalLogging();
    const RelaySafetySnapshot before = getRelaySafetySnapshot();
    if (before.closed) {
      return reassertCommandedRelayClosedPin();
    }

    if (operationalLimitMs < 1 ||
        operationalLimitMs > HARD_MAX_CIRCUIT_CLOSED_MS ||
        purpose == RelayPurpose::BACKFLUSH_CONFIRMED ||
        (purpose == RelayPurpose::BACKFLUSH_CANDIDATE &&
         operationalLimitMs != HARD_MAX_CIRCUIT_CLOSED_MS)) {
      tripRelaySafety(RelaySafetyFault::INVALID_LIMIT);
      addDebugEvent(DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
                    static_cast<int32_t>(CircuitArmFailReason::INVALID_LIMIT));
      return false;
    }
    if (before.state == RelaySafetyState::LOCKOUT) {
      addDebugEvent(DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
                    static_cast<int32_t>(CircuitArmFailReason::SAFETY_LOCKOUT));
      return false;
    }
    const bool watchdogUnavailable =
        !taskWatchdogReady || criticalTaskWatchdogFaulted();
    if (!firmwareInitializationComplete || !platformClockReady ||
        !relaySafetyTimersReady ||
        watchdogUnavailable ||
        relaySafetyTimer == nullptr || operationalLimitTimer == nullptr ||
        !independentSafetyTimer.ready()) {
      tripRelaySafety(watchdogUnavailable
                          ? RelaySafetyFault::WATCHDOG_UNAVAILABLE
                          : RelaySafetyFault::INITIALIZATION_FAILED);
      addDebugEvent(
          DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
          static_cast<int32_t>(CircuitArmFailReason::SUPERVISOR_UNAVAILABLE));
      return false;
    }
    if (EXTERNAL_SAFETY_HARDWARE_PRESENT && readCircuitFeedbackClosed()) {
      tripRelaySafety(RelaySafetyFault::FEEDBACK_STUCK_CLOSED);
      addDebugEvent(
          DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
          static_cast<int32_t>(CircuitArmFailReason::FEEDBACK_STUCK_CLOSED));
      return false;
    }

    stopRelayDeadlineTimers();
    const bool unlimited = firmwareCompatibilityMode() && purpose == RelayPurpose::NORMAL;
    const uint32_t closingAtMs = millis();
    uint32_t generation;
    // A momentary stop pulse may re-close K1 after tripRelaySafety opened it.
    // Keep the trip flags so stateMachineTask still finalizes the brew cycle.
    const bool preserveTripFlags =
        before.tripped || before.operationalTripped;
    portENTER_CRITICAL(&relayMux);
    generation = ++relaySafetyGeneration;
    circuitClosedAtMs = closingAtMs;
    relayPurpose = purpose;
    relayHardLimitMs = unlimited ? 0 : HARD_MAX_CIRCUIT_CLOSED_MS;
    operationalLimitAtArmMs = unlimited ? 0 : operationalLimitMs;
    if (!preserveTripFlags) {
      relaySafetyTripped = false;
      operationalLimitTripped = false;
      relaySafetyFault = RelaySafetyFault::NONE;
    }
    relaySafetyState = RelaySafetyState::ARMING;
    circuitClosed = false;
    portEXIT_CRITICAL(&relayMux);

    bool armed = true;
    if (!unlimited) {
      armed = independentSafetyTimer.arm(operationalLimitMs);
      if (esp_timer_start_once(
              relaySafetyTimer,
              static_cast<uint64_t>(HARD_MAX_CIRCUIT_CLOSED_MS) * 1000ULL) != ESP_OK) {
        armed = false;
      }
      if (operationalLimitMs < HARD_MAX_CIRCUIT_CLOSED_MS &&
          esp_timer_start_once(
              operationalLimitTimer,
              static_cast<uint64_t>(operationalLimitMs) * 1000ULL) != ESP_OK) {
        armed = false;
      }
    }

    if (!armed) {
      tripRelaySafety(RelaySafetyFault::TIMER_ARM_FAILED);
      addDebugEvent(DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
                    static_cast<int32_t>(CircuitArmFailReason::TIMER_ARM_FAILED));
      return false;
    }

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    if (hostCircuitArmBeforeCommitHook != nullptr) {
      hostCircuitArmBeforeCommitHook();
    }
#endif

    // Publish the conservative RTC CLOSE marker before entering the short
    // commit section. If the transaction is canceled below it is cleared
    // after the relay has remained/opened safe.
    recordRelayCommandedClosed(true);
    bool committed = false;
    portENTER_CRITICAL(&relayMux);
    // A timer callback may have run while the timers were being armed. It
    // increments the generation and changes ARMING to TRIPPED, so this stale
    // continuation can no longer energize the relay.
    if (relaySafetyGeneration == generation &&
        relaySafetyState == RelaySafetyState::ARMING &&
        (unlimited || static_cast<uint32_t>(millis() - closingAtMs) <
            operationalLimitMs)) {
      digitalWrite(RELAY_GPIO, RELAY_CLOSED_LEVEL);
      relayElectricalClosedAtMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
      relayElectricalOpenedAtMs = relayElectricalClosedAtMs;
      circuitClosed = true;
      relaySafetyState = RelaySafetyState::CLOSED;
      feedbackExpectedClosed = true;
      feedbackTransitionPending = EXTERNAL_SAFETY_HARDWARE_PRESENT;
      feedbackTransitionStartedAtMs = millis();
      feedbackTransitionStampPending = false;
      committed = true;
    }
    portEXIT_CRITICAL(&relayMux);
    if (!committed) {
      recordRelayCommandedClosed(false);
      stopRelayDeadlineTimers();
      addDebugEvent(DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
                    static_cast<int32_t>(CircuitArmFailReason::ARM_CANCELED));
      return false;
    }
    addDebugEvent(DebugCategory::RELAY, DebugCode::RELAY_CLOSED,
                  static_cast<int32_t>(unlimited ? 0 : operationalLimitMs));
    return true;
  }

  portENTER_CRITICAL(&relayMux);
  const bool wasClosed = circuitClosed ||
                         relaySafetyState == RelaySafetyState::ARMING;
  // Open before stopping either timer. A preemption after this write is safe.
  const bool alreadyOpenedBySafety =
      !circuitClosed && (relaySafetyState == RelaySafetyState::TRIPPED ||
                     relaySafetyState == RelaySafetyState::LOCKOUT);
  if (!alreadyOpenedBySafety) {
    digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
    if (circuitClosed) relayElectricalOpenedAtMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  }
  circuitClosed = false;
  ++relaySafetyGeneration;
  if (relaySafetyState != RelaySafetyState::TRIPPED &&
      relaySafetyState != RelaySafetyState::LOCKOUT) {
    relaySafetyState = RelaySafetyState::OPEN;
  }
  feedbackExpectedClosed = false;
  feedbackTransitionPending = EXTERNAL_SAFETY_HARDWARE_PRESENT;
  feedbackTransitionStartedAtMs = millis();
  feedbackTransitionStampPending = false;
  portEXIT_CRITICAL(&relayMux);
  recordRelayCommandedClosed(false);
  stopRelayDeadlineTimers();
  // BLE claim is recomputed on the scale worker (connecting / GATT / closed).
  // Do not force BALANCE here: a live scale link must keep PREFER_BT.
  if (wasClosed) {
    addDebugEvent(DebugCategory::RELAY, DebugCode::RELAY_OPENED);
  }
  return true;
}

// Control is the sole caller through the machine facade. Keep the old task
// deadline armed until independent replacement succeeds. GPIO never closes here.
bool promoteRelayBackflush(uint32_t generation) {
  const auto before = getRelaySafetySnapshot();
  if (!before.closed || before.state != RelaySafetyState::CLOSED ||
      before.generation != generation || before.tripped || before.operationalTripped ||
      before.purpose != RelayPurpose::BACKFLUSH_CANDIDATE ||
      elapsedMs(before.closedAtMs) >= HARD_MAX_CIRCUIT_CLOSED_MS) return false;
  bool armed = independentSafetyTimer.arm(
      BACKFLUSH_MAX_CIRCUIT_CLOSED_MS - elapsedMs(before.closedAtMs));
  if (armed) armed = esp_timer_stop(relaySafetyTimer) == ESP_OK;
  if (armed) {
    const uint32_t elapsed = elapsedMs(before.closedAtMs);
    armed = elapsed < HARD_MAX_CIRCUIT_CLOSED_MS &&
        esp_timer_start_once(relaySafetyTimer,
            static_cast<uint64_t>(BACKFLUSH_MAX_CIRCUIT_CLOSED_MS - elapsed) * 1000) == ESP_OK;
  }
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (hostCircuitArmBeforeCommitHook) hostCircuitArmBeforeCommitHook();
#endif
  portENTER_CRITICAL(&relayMux);
  const bool committed = armed && circuitClosed &&
      relaySafetyState == RelaySafetyState::CLOSED &&
      relaySafetyGeneration == generation &&
      relayPurpose == RelayPurpose::BACKFLUSH_CANDIDATE &&
      !relaySafetyTripped && !operationalLimitTripped &&
      elapsedMs(before.closedAtMs) < HARD_MAX_CIRCUIT_CLOSED_MS;
  if (committed) {
    relayPurpose = RelayPurpose::BACKFLUSH_CONFIRMED;
    relayHardLimitMs = operationalLimitAtArmMs = BACKFLUSH_MAX_CIRCUIT_CLOSED_MS;
  }
  portEXIT_CRITICAL(&relayMux);
  if (!committed) tripRelaySafety(RelaySafetyFault::TIMER_ARM_FAILED);
  return committed;
}

// The deadline ISR only opens K1; consuming its latched trip completes the
// task-side work (RTC OPEN marker, timer cleanup) even with no session to end.
bool consumeTripFlag(bool &flag) {
  portENTER_CRITICAL(&relayMux);
  const bool tripped = flag;
  const bool open = !circuitClosed;
  flag = false;
  portEXIT_CRITICAL(&relayMux);
  if (tripped && open) {
    recordRelayCommandedClosed(false);
    stopRelayDeadlineTimers();
  }
  return tripped;
}

bool consumeRelaySafetyTrip() { return consumeTripFlag(relaySafetyTripped); }

bool consumeOperationalLimitTrip() {
  return consumeTripFlag(operationalLimitTripped);
}

void serviceRelaySafety() {
  if (criticalTaskWatchdogFaulted()) {
    tripRelaySafety(RelaySafetyFault::TASK_WATCHDOG_FAILURE);
    requestSafeRestart();
    return;
  }

  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  if (relay.closed &&
      relay.state != RelaySafetyState::TRIPPED &&
      relay.state != RelaySafetyState::LOCKOUT) {
    if (!reassertCommandedRelayClosedPin()) {
      return;
    }
  }
  if (relay.operationalLimitMs != 0 &&
      (relay.state == RelaySafetyState::ARMING || relay.closed) &&
      elapsedMs(relay.closedAtMs) >= relay.operationalLimitMs) {
    const bool operational =
        relay.operationalLimitMs < relay.hardLimitMs;
    tripRelaySafety(operational ? RelaySafetyFault::OPERATIONAL_LIMIT
                               : RelaySafetyFault::HARD_LIMIT,
                    !operational, operational, false);
    return;
  }

  if (!EXTERNAL_SAFETY_HARDWARE_PRESENT) {
    return;
  }

  const bool feedbackClosed = readCircuitFeedbackClosed();
  bool pending = false;
  bool expectedClosed = false;
  uint32_t settleStartedAtMs = 0;
  portENTER_CRITICAL(&relayMux);
  if (feedbackTransitionStampPending) {
    feedbackTransitionStartedAtMs = millis();
    feedbackTransitionStampPending = false;
  }
  pending = feedbackTransitionPending;
  expectedClosed = feedbackExpectedClosed;
  settleStartedAtMs = feedbackTransitionStartedAtMs;
  portEXIT_CRITICAL(&relayMux);

  if (pending) {
    if (elapsedMs(settleStartedAtMs) < CIRCUIT_FEEDBACK_SETTLE_MS) {
      return;
    }
    portENTER_CRITICAL(&relayMux);
    const bool stillPending = feedbackTransitionPending &&
        elapsedMs(feedbackTransitionStartedAtMs) >= CIRCUIT_FEEDBACK_SETTLE_MS;
    expectedClosed = feedbackExpectedClosed;
    if (stillPending) {
      feedbackTransitionPending = false;
    }
    portEXIT_CRITICAL(&relayMux);
    if (stillPending && feedbackClosed != expectedClosed) {
      tripRelaySafety(expectedClosed
                          ? RelaySafetyFault::FEEDBACK_FAILED_TO_CLOSE
                          : RelaySafetyFault::FEEDBACK_STUCK_CLOSED);
    }
    return;
  }

  if (relay.closed != feedbackClosed) {
    tripRelaySafety(relay.closed
                        ? RelaySafetyFault::FEEDBACK_CHANGED_UNEXPECTEDLY
                        : RelaySafetyFault::FEEDBACK_STUCK_CLOSED);
  }
}

void serviceSafetyHeartbeat(bool healthyLoopCompleted) {
  if (!EXTERNAL_SAFETY_HARDWARE_PRESENT) {
    return;
  }
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  const bool healthy = healthyLoopCompleted && relay.watchdogReady &&
                       relay.timersReady && !criticalTaskWatchdogFaulted() &&
                       relay.state != RelaySafetyState::LOCKOUT &&
                       relay.state != RelaySafetyState::TRIPPED;
  if (!healthy) {
    safetyHeartbeatLevel = false;
    digitalWrite(SAFETY_HEARTBEAT_GPIO, SAFETY_HEARTBEAT_IDLE_LEVEL);
    return;
  }
  if (elapsedMs(safetyHeartbeatToggledAtMs) >=
      SAFETY_HEARTBEAT_TOGGLE_MS) {
    safetyHeartbeatLevel = !safetyHeartbeatLevel;
    digitalWrite(SAFETY_HEARTBEAT_GPIO,
                 safetyHeartbeatLevel ? HIGH : LOW);
    safetyHeartbeatToggledAtMs = millis();
  }
}

void initializeRelaySafetyStateAfterBoot() {
  portENTER_CRITICAL(&relayMux);
  relaySafetyState = platformClockReady && relaySafetyTimersReady &&
                             taskWatchdogReady
                         ? RelaySafetyState::OPEN
                         : RelaySafetyState::LOCKOUT;
  relaySafetyFault =
      !platformClockReady || !relaySafetyTimersReady
          ? RelaySafetyFault::INITIALIZATION_FAILED
          : (!taskWatchdogReady ? RelaySafetyFault::WATCHDOG_UNAVAILABLE
                                : RelaySafetyFault::NONE);
  portEXIT_CRITICAL(&relayMux);
}

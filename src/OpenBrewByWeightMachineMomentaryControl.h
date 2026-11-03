#pragma once

#include "OpenBrewByWeightMachineMomentaryConfig.h"

// =============================================================================
// SPECIALIZATION: Momentary switch — actuator (control)
// =============================================================================
// WHAT: K1 mirrors the physical switch 1:1 when the façade allows activator
//       drive. Firmware uses synthetic pulses for Web controls and for a stop
//       (weight cut / walls), aborted if the user presses. Logical run walls
//       reuse tripRelaySafety so brew sees existing flags.
//
// BOUNDARY: Momentary-only. No paddle latch/PaddleMode policy. Stopper/brew
// call machineRequestStart/Stop on the façade and must not know about pulses
// or 1:1 mirroring. Run state lives in MomentaryOnlyState / MomentaryReedState.

void abortFirmwarePulseIfActive() {
  firmwarePulsePending = false;
  firmwarePulsePendingReadyAtMs = 0;
  if (pulseOutputActive) {
    pulseOutputActive = false;
    if (!momentaryPhysicalOn && relayCircuitClosed()) {
      (void)setMachineCircuitClosed(false);
    }
  }
}

bool emitFirmwarePulse(FirmwarePulseKind kind);

void serviceFirmwarePulseDrive() {
  if (!pulseOutputActive) {
    if (relayCircuitClosed()) {
      (void)setMachineCircuitClosed(false);
    }
    if (!firmwarePulsePending || momentaryPhysicalOn ||
        tickBefore(millis(), firmwarePulsePendingReadyAtMs)) {
      return;
    }
    const FirmwarePulseKind pendingKind = firmwarePulsePendingKind;
    firmwarePulsePending = false;
    firmwarePulsePendingReadyAtMs = 0;
    (void)emitFirmwarePulse(pendingKind);
    return;
  }
  if (tickReached(millis(), pulseOutputEndsAtMs)) {
    (void)setMachineCircuitClosed(false);
    pulseOutputActive = false;
    if (firmwarePulsePending) {
      firmwarePulsePendingReadyAtMs = millis() + ACTIVATOR_DEBOUNCE_MS;
    }
  } else if (!relayCircuitClosed()) {
    (void)setMachineCircuitClosed(true, HARD_MAX_CIRCUIT_CLOSED_MS);
  }
}

void applyMomentaryRelayDrive() {
  if (rinseActuationActive) {
    serviceFirmwarePulseDrive();
    return;
  }
  if (momentaryPhysicalOn) {
    if (!machineActivatorDriveSuppressedThisHold) {
      abortFirmwarePulseIfActive();
    } else if (pulseOutputActive) {
      serviceFirmwarePulseDrive();
      return;
    }
    if (!machineMayForwardActivatorOn()) {
      if (relayCircuitClosed()) {
        (void)setMachineCircuitClosed(false);
      }
      return;
    }
    if (!relayCircuitClosed()) {
      (void)setMachineCircuitClosed(true, HARD_MAX_CIRCUIT_CLOSED_MS);
    }
    return;
  }
  machineNoteActivatorReleased();
  if (pulseOutputActive || firmwarePulsePending) {
    serviceFirmwarePulseDrive();
    return;
  }
  if (relayCircuitClosed()) {
    (void)setMachineCircuitClosed(false);
  }
}

bool emitFirmwarePulse(FirmwarePulseKind kind) {
  if (!rinseActuationActive && momentaryPhysicalOn) {
    return true;
  }
  if (pulseOutputActive) {
    return true;
  }
  uint32_t durationMs = runtimeStopPulseMs(runtimeConfig);
  if (durationMs < 50U) {
    durationMs = 50U;
  }
  if (!setMachineCircuitClosed(true, HARD_MAX_CIRCUIT_CLOSED_MS)) {
    return false;
  }
  pulseOutputActive = true;
  pulseOutputKind = kind;
  pulseOutputIsStart = kind == FirmwarePulseKind::START;
  pulseOutputEndsAtMs = millis() + durationMs;
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
  if (kind == FirmwarePulseKind::STOP) {
    momentaryFirmwareCutPending = true;
  }
#endif
  return true;
}

bool queueWebFirmwarePulse(FirmwarePulseKind kind) {
  if (momentaryPhysicalOn) {
    return false;
  }
  if (pulseOutputActive) {
    if (kind == FirmwarePulseKind::STOP &&
        pulseOutputKind == FirmwarePulseKind::STOP) {
      return true;
    }
    if (firmwarePulsePending) {
      return false;
    }
    firmwarePulsePending = true;
    firmwarePulsePendingKind = kind;
    firmwarePulsePendingReadyAtMs = 0;
    return true;
  }
  if (firmwarePulsePending) {
    return false;
  }
  return emitFirmwarePulse(kind);
}

bool emitFirmwareStopPulse() {
  return emitFirmwarePulse(FirmwarePulseKind::STOP);
}

void maybeEmitFirmwareStopPulse() {
  if (momentaryPhysicalOn) {
    abortFirmwarePulseIfActive();
    return;
  }
  if (!machineAllowsFirmwareStopPulse()) {
    abortFirmwarePulseIfActive();
    return;
  }
  (void)emitFirmwareStopPulse();
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
void endMomentaryLogicalRunForWall() {
  const bool cut = machineAllowsFirmwareStopPulse();
  latchMomentaryElapsed();
  momentaryLogicalRunActive = false;
  if (cut) {
    maybeEmitFirmwareStopPulse();
    noteMomentaryLogicalStop();
    return;
  }
  if (momentaryInferredState != MachineRunState::IDLE) {
    momentaryOrphanRun = true;
  }
}
#endif

void serviceLogicalRunWalls() {
  if (!momentaryLogicalRunActive) {
    return;
  }
  const uint32_t elapsed = elapsedMs(momentaryLogicalRunStartedAtMs);
  if (elapsed >= HARD_MAX_CIRCUIT_CLOSED_MS) {
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
    if (machineAllowsFirmwareStopPulse()) {
      tripRelaySafety(RelaySafetyFault::HARD_LIMIT, true, false, false);
      endMomentaryLogicalRunForWall();
    } else if (!momentarySawScale && !machineSense.weightFresh) {
      latchMomentaryElapsed();
      settleMomentaryInferredOff();
      momentaryNoFlowIdlePending = true;
    } else {
      tripRelaySafety(RelaySafetyFault::HARD_LIMIT, true, false, false);
      endMomentaryLogicalRunForWall();
    }
#else
    tripRelaySafety(RelaySafetyFault::HARD_LIMIT, true, false, false);
    latchMomentaryElapsed();
    momentaryLogicalRunActive = false;
    if (machineAllowsFirmwareStopPulse()) {
      maybeEmitFirmwareStopPulse();
      noteMomentaryLogicalStop();
    }
#endif
    return;
  }
  if (momentaryLogicalOperationalLimitMs < HARD_MAX_CIRCUIT_CLOSED_MS &&
      elapsed >= momentaryLogicalOperationalLimitMs) {
    tripRelaySafety(RelaySafetyFault::OPERATIONAL_LIMIT, false, true, false);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
    endMomentaryLogicalRunForWall();
#else
    latchMomentaryElapsed();
    momentaryLogicalRunActive = false;
    if (machineAllowsFirmwareStopPulse()) {
      maybeEmitFirmwareStopPulse();
      noteMomentaryLogicalStop();
    }
#endif
  }
}

inline bool machineRequestStart(uint32_t operationalLimitMs,
                                bool remoteActuation) {
  clearMomentaryElapsedLatch();
  noteMomentaryLogicalStart();
  momentaryLogicalRunActive = true;
  momentaryLogicalRunStartedAtMs = millis();
  momentaryLogicalOperationalLimitMs = operationalLimitMs;
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  if (relay.state == RelaySafetyState::LOCKOUT ||
      relay.state == RelaySafetyState::TRIPPED) {
    return false;
  }
  // A physical press already toggles the machine circuit. In particular, a
  // release-edge start must not add a second pulse, which would toggle it back
  // off. A Web start has no physical edge, so it must synthesize that pulse.
  return !remoteActuation || emitFirmwarePulse(FirmwarePulseKind::START);
}

inline bool machineRequestStop() {
  latchMomentaryElapsed();
  momentaryLogicalRunActive = false;
  const bool skipPulse =
      momentaryPhysicalOn || momentarySkipFirmwareStopPulse;
  momentarySkipFirmwareStopPulse = false;
  if (skipPulse) {
    abortFirmwarePulseIfActive();
    noteMomentaryLogicalStop();
    applyMomentaryRelayDrive();
    return true;
  }
  if (pulseOutputActive) {
    noteMomentaryLogicalStop();
    return true;
  }
  if (!machineAllowsFirmwareStopPulse()) {
    noteMomentaryLogicalStop();
    applyMomentaryRelayDrive();
    return true;
  }
  noteMomentaryLogicalStop();
  return emitFirmwareStopPulse();
}

inline bool machineRequestWebStop() {
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
  if (momentaryPhysicalOn) {
    return machineRequestStop();
  }
  latchMomentaryElapsed();
  momentaryLogicalRunActive = false;
  momentarySkipFirmwareStopPulse = false;
  noteMomentaryLogicalStop();
  return queueWebFirmwarePulse(FirmwarePulseKind::STOP);
#else
  return machineRequestStop();
#endif
}

inline bool machineRequestForcedPulse() {
  return queueWebFirmwarePulse(FirmwarePulseKind::FORCED);
}

// continueSessionRun keeps an already-running logical run's start timestamp
// (a demoted rinse counts the pre-classification time). A fresh rinse, or a
// rinse over a run that predates the current session, restarts the clock.
inline bool machineBeginRinse(uint32_t operationalLimitMs,
                              bool continueSessionRun) {
  if (rinseActuationActive) {
    return true;
  }
  rinseActuationActive = true;
  machineActivatorDriveSuppressedThisHold = true;
  abortFirmwarePulseIfActive();
  if (relayCircuitClosed()) {
    (void)setMachineCircuitClosed(false);
  }
  clearMomentaryElapsedLatch();
  noteMomentaryLogicalStart();
  momentaryLogicalRunActive = true;
  if (!continueSessionRun) {
    momentaryLogicalRunStartedAtMs = millis();
  }
  momentaryLogicalOperationalLimitMs = operationalLimitMs;
  return emitFirmwarePulse(FirmwarePulseKind::START);
}

inline bool machineEndRinse() {
  latchMomentaryElapsed();
  momentaryLogicalRunActive = false;
  machineActivatorDriveSuppressedThisHold = true;
  noteMomentaryLogicalStop();
  abortFirmwarePulseIfActive();
  const bool ok = emitFirmwarePulse(FirmwarePulseKind::STOP);
  rinseActuationActive = false;
  return ok;
}

inline void machineApplyWorkflowConfig(RuntimeConfig &dst,
                                       const RuntimeConfig &src) {
  dst.stopPulseTenMs = src.stopPulseTenMs;
  dst.maxSinglePressHundredMs = src.maxSinglePressHundredMs;
  dst.momentaryStartOnPress = src.momentaryStartOnPress;
  dst.reedConfirmTimeoutHundredMs = src.reedConfirmTimeoutHundredMs;
  dst.assumeIdleWhenScaleConnects = src.assumeIdleWhenScaleConnects;
  dst.shotReactTimeoutS = src.shotReactTimeoutS;
  dst.rinseGestureMs = src.rinseGestureMs;
}

inline void serviceMachine() {
  serviceMomentaryRunSensors();
  applyMomentaryRelayDrive();
  serviceLogicalRunWalls();
  if (momentaryStopRetryPending) {
    momentaryStopRetryPending = false;
    maybeEmitFirmwareStopPulse();
  }
}

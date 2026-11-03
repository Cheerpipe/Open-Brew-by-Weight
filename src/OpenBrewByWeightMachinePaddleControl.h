#pragma once

// =============================================================================
// SPECIALIZATION: Paddle / latch-switch — actuator (control)
// =============================================================================
// WHAT: Start latches the circuit closed; stop opens it (via relay safety).
//       Each loop, ON reasserts K1 if already commanded closed. OFF never
//       opens here — only machineRequestStop does (asymmetric mirror).
//
// BOUNDARY: Paddle-only K1 requests. No momentary pulse/mirror logic. Upper
// layers call machineRequestStart/Stop on the façade, not this header.

void applyPaddleRelayDrive() {
  const bool held = activatorOn || rawActivatorOn;
  if (!held) {
    machineNoteActivatorReleased();
    return;
  }
  if (!machineMayForwardActivatorOn()) {
    if (relayCircuitClosed()) {
      (void)setMachineCircuitClosed(false);
    }
    return;
  }
  if (relayCircuitClosed()) {
    (void)setMachineCircuitClosed(true, HARD_MAX_CIRCUIT_CLOSED_MS);
  }
}

inline bool machineRequestStart(uint32_t operationalLimitMs,
                                bool remoteActuation) {
  (void)remoteActuation;
  return setMachineCircuitClosed(true, operationalLimitMs);
}

inline bool machineRequestStop() {
  if (activatorOn || rawActivatorOn) {
    machineActivatorDriveSuppressedThisHold = true;
  }
  return setMachineCircuitClosed(false);
}

inline bool machineRequestWebStop() { return machineRequestStop(); }

constexpr bool machineRequestForcedPulse() { return false; }

// continueSessionRun has no paddle effect: an already-closed circuit keeps
// its original close timestamp (a demoted rinse counts the pre-classification
// time), and an open circuit starts fresh either way.
inline bool machineBeginRinse(uint32_t operationalLimitMs,
                              bool continueSessionRun) {
  (void)continueSessionRun;
  if (rinseActuationActive) {
    return true;
  }
  rinseActuationActive = true;
  if (relayCircuitClosed()) {
    return true;
  }
  return machineRequestStart(operationalLimitMs);
}

inline bool machineEndRinse() {
  rinseActuationActive = false;
  return machineRequestStop();
}

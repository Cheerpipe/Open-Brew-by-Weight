#pragma once

#include "OpenBrewByWeightHardware.h"
#include "OpenBrewByWeightMachineTypes.h"
#include "OpenBrewByWeightSafety.h"

// =============================================================================
// LAYER: Machine (façade)
// =============================================================================
// WHAT: Generic machine API. The activator reads user intention on
//       ACTIVATOR_GPIO (paddle latch, momentary switch, or another compatible
//       mechanism) and translates that signal into UserIntent /
//       MachineIntention. Compile-time selects the specialization behind one
//       surface: start/stop, run state, cycle policy, MachineSense.
//
// BOUNDARY (hard rules — do not cross):
// - The activator owns GPIO sampling, debounce, edge interpretation, and
//   translation to UserIntent. Stopper / brew / cup / scale / guards
//   talk ONLY to this abstract machine. They must NOT include paddle or
//   momentary headers, branch on machine type, read paddleMode / reed /
//   momentary GPIO, or know how a specialization drives K1.
// - Paddle code lives only under OpenBrewByWeightMachinePaddle*. Momentary under
//   OpenBrewByWeightMachineMomentary*. Never mix paddle logic into momentary files
//   or the reverse. Run state for each type is owned solely by that type's
//   *State* header (PaddleState vs MomentaryOnlyState / MomentaryReedState).
// - Rinse *detection* is machine-owned (gesture → REQUEST_RINSE). Rinse
//   *duration* lives in OpenBrewByWeightRinse. The stopper orchestrates: it does
//   not classify ON→OFF windows or long-press. machineBeginRinse/EndRinse
//   latch rinse actuation (not a second rinse FSM). Brew/scale/cup never
//   call actuators here.
// - Activator→K1 forwarding permission is a one-way stopper push
//   (machineSetActivatorDriveAllowed), like preferBleAirtime. Guards never
//   write it. machineRequestStart/Stop and the relay driver must not consult
//   it (rinse, web, firmware stop pulse still close K1).

#include "OpenBrewByWeightMachineRelay.h"
#include "OpenBrewByWeightMachineActivatorSample.h"

struct MachineIntention {
  UserIntent intent = UserIntent::NONE;
  bool holdActive = false;
  bool physicalOn = false;
  bool turnedOn = false;
  bool turnedOff = false;
  bool stablyOff = false;
};

MachineIntention lastPolledIntention;
MachineSense machineSense;

inline MachineIntention machineCaptureIntention(const MachineIntention &intention) {
  lastPolledIntention = intention;
  return intention;
}

inline MachineIntention machineLastIntention() { return lastPolledIntention; }

inline void machineObserveSense(const MachineSense &sense) {
  machineSense = sense;
}

inline void machineSetPreferBleAirtime(bool prefer) {
  machinePreferBleAirtime = prefer;
}

// Dumb drive bit + hold-continuity latch. Specializations must not know why
// the stopper cleared the bit (no-scale BBW, cup-start, or a future guard).
bool machineActivatorDriveAllowed = true;
bool machineActivatorDriveSuppressedThisHold = false;
bool rinseActuationActive = false;

inline void machineSetActivatorDriveAllowed(bool allowed) {
  machineActivatorDriveAllowed = allowed;
}

inline void machineAllowCurrentActivatorHold() {
  machineActivatorDriveSuppressedThisHold = false;
}

inline bool machineMayForwardActivatorOn() {
  if (!machineActivatorDriveAllowed) {
    machineActivatorDriveSuppressedThisHold = true;
    return false;
  }
  if (machineActivatorDriveSuppressedThisHold) {
    return false;
  }
  return true;
}

inline void machineNoteActivatorReleased() {
  machineActivatorDriveSuppressedThisHold = false;
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0
#include "OpenBrewByWeightMachinePaddleInput.h"
#include "OpenBrewByWeightMachinePaddleControl.h"
#include "OpenBrewByWeightMachinePaddleState.h"
#include "OpenBrewByWeightMachinePaddlePolicy.h"

inline void machineOnActivatorReady() {}
inline void machineOnBrewOutcome(bool) {}
// Time from a stop command to the machine acting on it (none for a paddle).
constexpr uint32_t machineStopCommandLatencyMs() { return 0; }
inline bool machineSupportsRinse() { return runtimeConfig.rinseEnabled; }
// A paddle ON may still classify as a short ON->OFF rinse. Keep a rejected
// start pending until that gesture window has elapsed.
constexpr bool machineBlockedStartNeedsHoldQualification() { return true; }
inline void machineNoteFirmwareStop() {
  if (activatorOn || rawActivatorOn) {
    machineActivatorDriveSuppressedThisHold = true;
  }
}
inline void machineSampleInput() {
  updateActivatorInput();
  applyPaddleRelayDrive();
}
// Compatibility-mode transparent mirror: close while the activator is held,
// open when it is released. Relay safety disables duration deadlines for the
// boot-scoped compatibility mode while retaining fault supervision. The
// debounced hold drives the mirror — a raw input glitch must never toggle K1.
inline bool machineServiceCompatibilityDrive(
    const MachineIntention &intent) {
  const bool held = intent.physicalOn;
  const bool closed = relayCircuitClosed();
  if (held && !closed) {
    return machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS, false);
  }
  if (!held && closed) {
    return machineRequestStop();
  }
  return true;
}
inline void serviceMachine() {
  machineServiceReminders();
}
inline void machineOverrideInferredOff() {}
constexpr bool machineOverrideInferredOn() { return true; }
inline void machineArmSettledWeightCutOff() {}
inline void machineNoteSettledWeightCutOff() {}
inline void machineCancelSettledWeightCutOff() {}
#else
#include "OpenBrewByWeightMachineMomentaryInput.h"
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
#include "OpenBrewByWeightMachineMomentaryReedState.h"
#else
#include "OpenBrewByWeightMachineMomentaryOnlyState.h"
#endif
#include "OpenBrewByWeightMachineMomentaryControl.h"

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
inline void machineOverrideInferredOff() {}
constexpr bool machineOverrideInferredOn() { return true; }
inline void machineArmSettledWeightCutOff() {}
inline void machineNoteSettledWeightCutOff() {}
inline void machineCancelSettledWeightCutOff() {}
#endif

inline bool machineSupportsRinse() { return runtimeConfig.rinseEnabled; }
// A momentary stop is a timed press: the machine acts after the pulse.
inline uint32_t machineStopCommandLatencyMs() {
  return runtimeStopPulseMs(runtimeConfig);
}
// A momentary start edge is already a complete attempt. In particular,
// release mode has no active hold left by the time REQUEST_START is emitted.
constexpr bool machineBlockedStartNeedsHoldQualification() { return false; }
inline void machineBeginCycle(bool) {}
inline void machineEndCycle() { rinseActuationActive = false; }
constexpr bool machineHidesPhysicalStop() { return false; }
constexpr bool machineAllowsAutomationStop() { return true; }
inline uint32_t machineCloseLimitMs(uint32_t operationalWallMs) {
  return operationalWallMs;
}
constexpr bool machineCycleHardMaxArmedForTest() { return false; }
constexpr bool machineCyclePromotedToNaturalForTest() { return false; }
inline void machineNoteFirmwareStop() {
  momentarySkipFirmwareStopPulse = false;
}
inline void machineServiceReminders() {}
inline void machineSampleInput() {
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  sampleReed();
#endif
  updateActivatorInput();
}

inline MachineIntention machinePollIntention() {
  MachineIntention out;
  out.turnedOn = activatorTurnedOn;
  out.turnedOff = activatorTurnedOff;
  out.holdActive = momentaryPhysicalOn || rawActivatorOn;
  out.physicalOn = momentaryPhysicalOn;
  out.stablyOff = activatorIsStablyOff();
  if (momentaryRinseRequested) {
    out.intent = UserIntent::REQUEST_RINSE;
  } else if (activatorTurnedOn) {
    out.intent = UserIntent::REQUEST_START;
  } else if (activatorTurnedOff) {
    out.intent = UserIntent::REQUEST_STOP;
  } else if (out.holdActive) {
    out.intent = UserIntent::HOLD_ACTIVE;
  } else if (out.stablyOff) {
    out.intent = UserIntent::STABLE_IDLE;
  }
  return machineCaptureIntention(out);
}
// Compatibility mode is already transparent here: applyMomentaryRelayDrive
// mirrors the switch 1:1, and every synthetic pulse source (orchestrator,
// web commands) is gated upstream of the machine layer.
constexpr bool machineServiceCompatibilityDrive(const MachineIntention &) {
  return true;
}
#endif

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE != 1
constexpr bool machineTakeNoFlowIdle() { return false; }
#endif

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0
inline bool machineStartBackflush() {
  return !relayCircuitClosed() &&
      setMachineCircuitClosed(true, HARD_MAX_CIRCUIT_CLOSED_MS, RelayPurpose::BACKFLUSH_CANDIDATE);
}

inline bool machineConfirmBackflush(uint32_t generation) {
  return promoteRelayBackflush(generation);
}
#else
constexpr bool machineStartBackflush() { return false; }
constexpr bool machineConfirmBackflush(uint32_t) { return false; }
#endif

inline void machineInitialize() {
  // Latch OPEN before enabling the driver: an active-LOW relay must not see
  // the reset latch level as CLOSED.
  openRelayElectricalFromIsr();
  pinMode(RELAY_GPIO, OUTPUT);
  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
  if (EXTERNAL_SAFETY_HARDWARE_PRESENT) {
    pinMode(SAFETY_HEARTBEAT_GPIO, OUTPUT);
    digitalWrite(SAFETY_HEARTBEAT_GPIO, SAFETY_HEARTBEAT_IDLE_LEVEL);
    pinMode(CIRCUIT_FEEDBACK_GPIO, INPUT_PULLUP);
  }
  initializeActivatorInput();
  // No activator drive until loop() grants it: boot-time sampling (recovery
  // gesture) must never close K1 before initialization completes.
  machineSetActivatorDriveAllowed(false);
  machineNoteActivatorReleased();
}

inline bool machineBootActivatorHeldStably() {
  if (!readRawActivatorOn()) {
    return false;
  }
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const uint32_t startedAtMs = millis();
  while (static_cast<uint32_t>(millis() - startedAtMs) <
         ACTIVATOR_DEBOUNCE_MS) {
    if (!readRawActivatorOn()) {
      return false;
    }
    serviceBootRecoverySafety();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
#endif
  return readRawActivatorOn();
}

struct MachineStatusSample {
  bool rawOn;
  bool running;
  uint32_t circuitElapsedMs;
  MachineRunState runState;
};

// Sample hardware/relay state before the caller locks snapshot publication.
inline MachineStatusSample machineSampleStatus() {
  const bool running = machineIsRunning();
  const bool rawOn = readRawActivatorOn();
  const uint32_t circuitElapsedMs = machineElapsedMs();
  const MachineRunState runState = machineRunState();
  return {rawOn, running, circuitElapsedMs, runState};
}

inline void machineFillStatus(ControlStatusFields &status,
                              const MachineStatusSample &sample) {
  status.rawActivatorOn = sample.rawOn;
  status.physicalActivatorOn = sample.rawOn;
  status.machineRunning = sample.running;
  status.circuitElapsedMs = sample.circuitElapsedMs;
  machineFillInferenceStatus(status);
}

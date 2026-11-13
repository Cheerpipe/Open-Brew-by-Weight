// One physical dispatch before any brew/scale effects; providers own evidence,
// control owns activity, and every electrical action uses the machine facade.
void endBackflush(bool confirmed, const char *reason) {
  machineRequestStop();
  const auto relay = getRelaySafetySnapshot();
  appendHistoryRecord(confirmed ? HistoryType::BACKFLUSH : HistoryType::OTHER,
      relay.electricalOpenedAtMs - relay.electricalClosedAtMs, true);
  finishMachineIntegrationBackflush(backflushPermit.attempt);
  backflushPermit = {};
  backflushStopReason = !confirmed && strcmp(reason, "mode_ended") == 0 ? "unconfirmed_end" : reason;
  transitionTo(StopperState::REQUIRES_OFF);
}

bool backflushProvenanceCurrent(const MachineBackflushSnapshot &observation) {
  return observation.valid &&
      (observation.phase == MachineBackflushPhase::AWAITING ||
       observation.phase == MachineBackflushPhase::ACTIVE) &&
      observation.permit.continuity == backflushPermit.continuity &&
      observation.permit.episode == backflushPermit.episode &&
      observation.permit.attempt == backflushPermit.attempt;
}

// Home's "Backflush ready · paddle to ON": paddle ON now admits a supervised
// backflush that keeps the circuit on for the whole program.
bool backflushReadyForPaddle() {
  return !session.active && stopperState == StopperState::READY &&
      backflushObservation.valid && backflushObservation.phase == MachineBackflushPhase::AWAITING &&
      backflushObservation.permit.extendable && backflushObservation.ready &&
      firmwareInitializationComplete && !maintenanceLease.active;
}

// One cue per awaiting episode, even if readiness flickers within it.
void serviceBackflushReadyAlert() {
  if (!backflushReadyForPaddle() ||
      backflushObservation.permit.episode == backflushReadyAlertEpisode) return;
  backflushReadyAlertEpisode = backflushObservation.permit.episode;
  if (runtimeConfig.buzzerBackflushReadyBeep) emitAlert(AlertEvent::BACKFLUSH_READY);
}

void serviceBackflush(const MachineIntention &intent) {
  const auto relay = getRelaySafetySnapshot();
  bool confirmed = stopperState == StopperState::BACKFLUSH_RUNNING;
  if (intent.turnedOff || intent.stablyOff || !intent.holdActive) {
    endBackflush(confirmed, "physical_off");
    return;
  }
  if (!relay.closed || relay.tripped || relay.operationalTripped ||
      elapsedMs(relay.closedAtMs) >= relay.hardLimitMs) {
    endBackflush(confirmed, "safety_stop");
    return;
  }
  if (!backflushObservation.valid ||
      backflushObservation.permit.continuity != backflushPermit.continuity ||
      backflushObservation.permit.attempt != backflushPermit.attempt) {
    endBackflush(confirmed, "supervision_lost");
    return;
  }
  // Reduce the entire ordered batch before promotion. ACTIVE then INACTIVE
  // closes once as Backflush without touching either armed deadline.
  for (uint8_t i = 0; i < backflushObservation.count; ++i) {
    const auto &change = backflushObservation.changes[i];
    const uint32_t expected = backflushSequence == UINT32_MAX ? 1 : backflushSequence + 1;
    if (change.sequence != expected ||
        change.permit.continuity != backflushPermit.continuity ||
        change.permit.episode != backflushPermit.episode) {
      endBackflush(confirmed, "observation_gap");
      return;
    }
    backflushSequence = change.sequence;
    if (change.phase == MachineBackflushPhase::ACTIVE) {
      if (!backflushPermit.extendable ||
          change.permit.attempt != backflushPermit.attempt ||
          (!confirmed && elapsedMs(relay.closedAtMs) >= HARD_MAX_CIRCUIT_CLOSED_MS)) {
        endBackflush(confirmed, "ambiguous_confirmation");
        return;
      }
      confirmed = true;
    } else if (change.phase == MachineBackflushPhase::INACTIVE || confirmed) {
      endBackflush(confirmed, change.phase == MachineBackflushPhase::INACTIVE
          ? "mode_ended" : "state_changed");
      return;
    }
  }
  if (confirmed && stopperState == StopperState::BACKFLUSH_CANDIDATE) {
    if (!backflushProvenanceCurrent(machineIntegrationBackflush()) ||
        // cppcheck-suppress knownConditionTrueFalse ; momentary machine builds define machineConfirmBackflush() as constexpr false; paddle builds promote the relay
        !machineConfirmBackflush(backflushRelayGeneration) ||
        !backflushProvenanceCurrent(machineIntegrationBackflush())) {
      endBackflush(true, "promotion_failed");
      return;
    }
    transitionTo(StopperState::BACKFLUSH_RUNNING);
  }
}

bool serviceMachineWakeGesture(const MachineIntention &intent) {
  machineWakeGestureConsumedThisLoop = false;
  if (backflushActive(stopperState)) {
    machineWakeGestureConsumedThisLoop = true;
    serviceBackflush(intent);
    return true;
  }
  if (machineWakePassthroughActive) {
    machineWakeGestureConsumedThisLoop = true;
    if (intent.turnedOff || intent.stablyOff || !intent.holdActive) {
      machineRequestStop();
      machineWakePassthroughActive = false;
      appendHistoryRecord(HistoryType::POWER_ON,
                          elapsedMs(machineWakeStartedAtMs), true);
    }
    return true;
  }
  if (!intent.turnedOn) return backflushObservation.busy() && !session.active;

  MachineBackflushPermit permit;
  const MachinePhysicalStartDisposition disposition =
      machineIntegrationPhysicalStart(&permit);
  if (disposition == MachinePhysicalStartDisposition::BLOCKED_CLEANING ||
      disposition == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE) {
    machineWakeGestureConsumedThisLoop = true;
    if (disposition == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE &&
        firmwareInitializationComplete && stopperState == StopperState::READY &&
        !session.active && !maintenanceLease.active) {
      machineSetActivatorDriveAllowed(true);
      machineAllowCurrentActivatorHold();
      backflushPermit = permit;
      if (servicePowerManagement() && machineStartBackflush()) {
        const auto admitted = machineIntegrationBackflush();
        backflushRelayGeneration = getRelaySafetySnapshot().generation;
        backflushSequence = permit.sequence;
        backflushStopReason = "none";
        transitionTo(StopperState::BACKFLUSH_CANDIDATE);
        if (!backflushProvenanceCurrent(admitted)) endBackflush(false, "admission_lost");
        return true;
      }
    }
    finishMachineIntegrationBackflush(permit.attempt);
    backflushPermit = {};
    if (!session.active) transitionTo(StopperState::REQUIRES_OFF);
    return true;
  }
  if (disposition != MachinePhysicalStartDisposition::WAKE_PASSTHROUGH) {
    return false;
  }

  machineWakeGestureConsumedThisLoop = true;
  if (!firmwareInitializationComplete || stopperState != StopperState::READY ||
      session.active || maintenanceLease.active) {
    return true;
  }
  machineSetActivatorDriveAllowed(true);
  machineAllowCurrentActivatorHold();
  if (!servicePowerManagement() ||
      !machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS, false)) {
    transitionTo(StopperState::REQUIRES_OFF);
    return true;
  }
  machineWakeStartedAtMs = millis();
  backflushStopReason = "none";
  machineWakePassthroughActive = true;
  return true;
}

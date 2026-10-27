// Placement policy and request provenance stay on the control task. The cup
// detector still owns all presence/stability decisions; the worker owns ATT.
struct IdleTareRuntime {
  uint32_t cupWeightRequestId = 0;
  uint32_t connectionGeneration = 0;
  uint32_t configRevision = 0;
  uint32_t settingsGeneration = 0;
  uint32_t lastSampleAtMs = 0;
  uint32_t lastPacketSequence = 0;
  uint32_t requestId = 0;
  uint32_t requestedAtMs = 0;
  uint32_t placementId = 0;
  uint32_t lastCaptureSequence = 0;
  uint32_t effectAtMs = 0;
  uint32_t lastTerminalRequestId = 0;
  uint32_t lastTerminalAtMs = 0;
  uint32_t lastStartedAtMs = 0;
  uint32_t lastWrittenAtMs = 0;
  uint32_t lastCaptureBoundary = 0;
  uint32_t rejectedSamples = 0;
  uint32_t accessoryPlacementId = 0;
  uint32_t accessoryCycleId = 0;
  uint32_t accessoryStartedAtMs = 0;
  uint32_t accessoryLastAtMs = 0;
  uint32_t accessoryLowAtMs = 0;  // Last reading at or below half the minimum cup.
  uint32_t accessoryCrossedAtMs = 0;
  uint32_t unloadStartedAtMs = 0;
  uint32_t unloadLastAtMs = 0;
  float previousReferenceG = 0.0f;
  float requestMinimumG = 0.0f;
  float requestMaximumG = 0.0f;
  float requestPlacementThresholdG = 0.0f;
  float lastWeightG = 0.0f;
  float accessoryMinimumG = 0.0f;
  float accessoryMaximumG = 0.0f;
  float accessoryPeakG = 0.0f;
  float knownAccessoryMassG = 0.0f;
  float unloadMinimumG = 0.0f;
  float unloadMaximumG = 0.0f;
  uint8_t accessorySamples = 0;
  uint8_t unloadSamples = 0;
  bool accessoryUsed = false;
  bool accessoryRemovalAttempted = false;
  bool accessoryRequest = false;
  bool accessoryAbove = false;  // At or above the minimum since the last crossing.
  bool accessoryStep = false;   // That crossing was fast enough to be a placement.
  bool unchangedAfterWrite = false;
  IdleTareReason eligibilityReason = IdleTareReason::NONE;
  IdleTareReason lastReason = IdleTareReason::NONE;
  IdleTareReason deferredReason = IdleTareReason::NONE;
  bool eligible = false;
  bool absentObserved = false;
  bool cupStillPresent = false;
  bool tareNotified = false;
  bool previouslyTared = false;
};
IdleTareRuntime idleTare;
uint32_t nextIdleTareRequestId = 1;

void cancelPendingFinalize(const char *reason, uint32_t profileReason);

IdleTareReason idleTareEligibilityReason() {
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  const bool endedAwaitingRelease =
      stopperState == StopperState::REQUIRES_OFF && lastCycle.valid &&
      lastCycle.cycleId == session.id &&
      !brewEndIsAbandonedStart(lastCycle.endReason) &&
      lastCycle.endReason != EndReason::RINSE_COMPLETE &&
      lastCycle.endReason != EndReason::GLOBAL_LIMIT &&
      lastCycle.endReason != EndReason::CONFIGURED_WALL_LIMIT &&
      lastCycle.endReason != EndReason::RELAY_SAFETY_FAILURE;
  if (!runtimeConfig.autoTareOutsideBrew) return IdleTareReason::FEATURE_DISABLED;
  if (!firmwareInitializationComplete) return IdleTareReason::NOT_READY;
  if (session.active || backflushActive(stopperState) || backflushObservation.busy())
    return IdleTareReason::ACTIVE_CYCLE;
  if (maintenanceLease.active) return IdleTareReason::MAINTENANCE;
  if (relay.closed || relay.tripped || relay.operationalTripped)
    return IdleTareReason::RELAY_BLOCKED;
  if (stopperState != StopperState::READY && !endedAwaitingRelease)
    return IdleTareReason::STOP_STATE;
  if (machineRunState() != MachineRunState::IDLE)
    return IdleTareReason::MACHINE_NOT_OFF;
  if (!scaleLinkAvailable(getScaleLinkSnapshot()))
    return IdleTareReason::SCALE_UNAVAILABLE;
  return IdleTareReason::NONE;
}

void releaseIdleTareRequest(IdleTareReason reason = IdleTareReason::NONE) {
  const ProfileControlObservation profile;
  IdleTareStatus released;
  if (!cancelIdleScaleTare(idleTare.requestId, &released)) {
    if (idleTare.deferredReason == IdleTareReason::NONE)
      idleTare.deferredReason = reason;
    return;
  }
  finishCupWeight(idleTare.cupWeightRequestId,
      released.phase == IdleTarePhase::SUCCEEDED && idleTare.tareNotified &&
          idleTare.cupStillPresent,
      released.phase == IdleTarePhase::SUCCEEDED ||
          released.reason == IdleTareReason::WRITE_FAILED,
      released.preTareWeightG);
  if (idleTare.accessoryRequest && released.phase == IdleTarePhase::SUCCEEDED &&
      idleTare.tareNotified && cupPresence.weight.valid &&
      std::isfinite(cupPresence.emptyAnchorG)) {
    cupPresence.weight.weightG = -cupPresence.emptyAnchorG;
    idleTare.knownAccessoryMassG = released.preTareWeightG > 0.0f
        ? released.preTareWeightG : 0.0f;
    if (released.preTareWeightG < 0.0f) idleTare.accessoryUsed = false;
    else idleTare.accessoryRemovalAttempted = false;
  }
  if (idleTare.accessoryRequest && released.phase == IdleTarePhase::QUEUED &&
      reason == IdleTareReason::UNSTABLE) {  // Nothing was written; allow a retry.
    if (idleTare.accessoryMinimumG < 0.0f) idleTare.accessoryRemovalAttempted = false;
    else idleTare.accessoryUsed = false;
  }
  if (idleTare.cupStillPresent &&
      cupPresencePlacementId() == idleTare.placementId) {
    if (released.phase == IdleTarePhase::SUCCEEDED) {
      if (idleTare.tareNotified) {
        notifyCupPresenceTare();
        if (!idleTare.accessoryRequest && idleTare.eligible && !session.active) {
          idleTare.accessoryPlacementId = idleTare.placementId;
          idleTare.accessoryCycleId = session.id;
          idleTare.accessoryUsed = false;
          idleTare.accessoryRemovalAttempted = false;
          idleTare.knownAccessoryMassG = 0.0f;
        }
        reason = IdleTareReason::EFFECT_CONFIRMED;
      } else {
        // A still-observed unchanged reference is usable. Missing or different
        // evidence cannot certify either physical tare or empty pan.
        if (!idleTare.unchangedAfterWrite ||
            elapsedMs(idleTare.lastSampleAtMs) > MAX_AUTOMATION_WEIGHT_AGE_MS) {
          markCupTareReferenceUncertain();
        }
        reason = IdleTareReason::EFFECT_UNCONFIRMED;
      }
    } else if (idleTare.tareNotified) {
      restoreCupTareReference(idleTare.previouslyTared,
                              idleTare.previousReferenceG);
    }
  }
  idleTare.lastTerminalRequestId = idleTare.requestId;
  idleTare.lastTerminalAtMs = millis();
  idleTare.lastStartedAtMs = released.startedAtMs;
  idleTare.lastWrittenAtMs = released.writtenAtMs;
  idleTare.lastCaptureBoundary = released.captureBoundary;
  if (idleTare.deferredReason != IdleTareReason::NONE)
    reason = idleTare.deferredReason;
  idleTare.lastReason = reason;
  scaleProfileNoteEvent(ScaleProfileEvent::TARE_RELEASED, millis(),
                        idleTare.connectionGeneration,
                        idleTare.lastCaptureSequence, NAN,
                        idleTare.requestId, static_cast<uint32_t>(reason));
  // A lift or a shot start supersedes the request; any other end is a miss.
  if (reason != IdleTareReason::EFFECT_CONFIRMED &&
      reason != IdleTareReason::REMOVED &&
      reason != IdleTareReason::START_REQUEST) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE, "%s tare %s: %s",
                         idleTare.accessoryRequest ? "Accessory" : "Cup",
                         released.phase == IdleTarePhase::SUCCEEDED ? "not confirmed"
                             : released.phase == IdleTarePhase::FAILED ? "failed"
                                                                      : "cancelled",
                         idleTareReasonName(static_cast<uint8_t>(reason)));
  }
  idleTare.requestId = 0;
  idleTare.tareNotified = false;
  idleTare.cupStillPresent = false;
  idleTare.accessoryRequest = false;
}

void serviceIdleTare() {
  const ProfileControlObservation profile;
  if (!runtimeConfig.autoTareOutsideBrew && idleTare.requestId == 0 &&
      !idleTare.eligible) {
    idleTare.eligibilityReason = IdleTareReason::FEATURE_DISABLED;
    return;
  }
  const uint32_t generation = scaleConnectionGenerationNow();
  idleTare.eligibilityReason = idleTareEligibilityReason();
  const bool eligible = idleTare.eligibilityReason == IdleTareReason::NONE;
  if (eligible != idleTare.eligible ||
      generation != idleTare.connectionGeneration ||
      idleTareSettingsGeneration != idleTare.settingsGeneration) {
    idleTare.absentObserved = false;
    idleTare.accessoryPlacementId = 0;
    idleTare.accessorySamples = 0;
    idleTare.unloadSamples = 0;
    idleTare.accessoryRemovalAttempted = false;
    idleTare.knownAccessoryMassG = 0.0f;
    resetCupPlaceStabilityStreak();
    cupPresence.weight.unloadQualified = false;
    cupPresence.weight.unloadSamples = 0;
    const bool changedConnection = generation != idleTare.connectionGeneration;
    const bool changedConfig =
        idleTareSettingsGeneration != idleTare.settingsGeneration;
    if (changedConnection) {
      idleTare.lastSampleAtMs = 0;
      idleTare.lastPacketSequence = 0;
      idleTare.lastCaptureSequence = 0;
      idleTare.cupStillPresent = false;
    }
    idleTare.eligible = eligible;
    idleTare.connectionGeneration = generation;
    idleTare.configRevision = runtimeConfig.revision;
    idleTare.settingsGeneration = idleTareSettingsGeneration;
    if (idleTare.requestId != 0 &&
        (changedConnection || idleScaleTareStatus().phase == IdleTarePhase::QUEUED)) {
      releaseIdleTareRequest(changedConnection ? IdleTareReason::CONNECTION_CHANGED
                            : changedConfig ? IdleTareReason::CONFIG_CHANGED
                                            : idleTare.eligibilityReason);
    }
  }
  if (idleTare.requestId == 0) return;
  if (cupPresenceState() != CupPresenceState::PRESENT ||
      cupPresencePlacementId() != idleTare.placementId) {
    idleTare.cupStillPresent = false;
  }
  const IdleTareStatus status = idleScaleTareStatus();
  const bool failed = status.requestId != idleTare.requestId ||
                      status.phase == IdleTarePhase::NONE ||
                      status.phase == IdleTarePhase::FAILED;
  if (failed) {
    releaseIdleTareRequest(status.reason == IdleTareReason::NONE
                              ? IdleTareReason::WRITE_FAILED : status.reason);
  } else if (!idleTare.cupStillPresent) {
    releaseIdleTareRequest(IdleTareReason::REMOVED);
  } else if (status.phase == IdleTarePhase::SUCCEEDED && idleTare.tareNotified) {
    releaseIdleTareRequest(IdleTareReason::EFFECT_CONFIRMED);
  } else if (status.phase == IdleTarePhase::QUEUED &&
             elapsedMs(idleTare.lastSampleAtMs) > runtimeConfig.retareStabilityMaxGapMs) {
    releaseIdleTareRequest(IdleTareReason::STALE_SAMPLE);
  } else if (
      elapsedMs(idleTare.requestedAtMs) >=
          SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs) {
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_BASELINE_TIMEOUT, millis(),
                          idleTare.connectionGeneration, idleTare.lastCaptureSequence,
                          NAN, idleTare.requestId,
                          static_cast<uint32_t>(IdleTareReason::EFFECT_UNCONFIRMED));
    releaseIdleTareRequest(IdleTareReason::EFFECT_UNCONFIRMED);
  }
}

bool idleTareBlocksStart(bool preempt = false) {
  if (preempt && idleTare.requestId != 0) {
    idleTare.deferredReason = IdleTareReason::START_REQUEST;
    localBuzzer.stopIfCue(BuzzerCue::TARE);
  }
  serviceIdleTare();
  if (idleTare.requestId == 0) return false;
  if (preempt) {
    releaseIdleTareRequest(IdleTareReason::START_REQUEST);
    return false;
  }
  if (idleScaleTareStatus().phase == IdleTarePhase::QUEUED)
    releaseIdleTareRequest(IdleTareReason::START_REQUEST);
  return true;
}

bool observeIdleTareSample(float weight, uint32_t atMs, uint32_t sequence,
                           uint32_t generation, uint32_t captureSequence) {
  serviceIdleTare();
  if (generation != idleTare.connectionGeneration ||
      static_cast<int32_t>(millis() - atMs) < 0 ||
      static_cast<int32_t>(millis() - atMs) >
          static_cast<int32_t>(MAX_AUTOMATION_WEIGHT_AGE_MS) ||
      (idleTare.lastPacketSequence != 0 &&
       (static_cast<int32_t>(sequence - idleTare.lastPacketSequence) <= 0 ||
        static_cast<int32_t>(atMs - idleTare.lastSampleAtMs) < 0))) {
    resetCupSampleEvidence();
    idleTare.unloadSamples = 0;
    ++idleTare.rejectedSamples;
    if (idleTare.requestId != 0 &&
        idleScaleTareStatus().phase == IdleTarePhase::QUEUED)
      releaseIdleTareRequest(IdleTareReason::STALE_SAMPLE);
    return false;
  }
  idleTare.lastSampleAtMs = atMs;
  idleTare.lastPacketSequence = sequence;
  idleTare.lastCaptureSequence = captureSequence;
  idleTare.lastWeightG = weight;
  // Hold a partial negative ramp until it settles; a full unload keeps the
  // cup detector's existing fast path through the empty-pan anchor.
  if (idleTare.knownAccessoryMassG >= runtimeConfig.minimumCupWeightG &&
      weight <= runtimeConfig.cupRemovedWeightG &&
      std::isfinite(cupPresence.emptyAnchorG) &&
      weight > cupPresence.emptyAnchorG + runtimeConfig.retareStabilityToleranceG) {
    if (idleTare.unloadSamples == 0 ||
        atMs - idleTare.unloadLastAtMs > runtimeConfig.retareStabilityMaxGapMs ||
        fmaxf(weight, idleTare.unloadMaximumG) -
            fminf(weight, idleTare.unloadMinimumG) >
            runtimeConfig.retareStabilityToleranceG) {
      idleTare.unloadSamples = 0;
      idleTare.unloadStartedAtMs = atMs;
      idleTare.unloadMinimumG = idleTare.unloadMaximumG = weight;
    }
    idleTare.unloadMinimumG = fminf(weight, idleTare.unloadMinimumG);
    idleTare.unloadMaximumG = fmaxf(weight, idleTare.unloadMaximumG);
    idleTare.unloadLastAtMs = atMs;
    if (idleTare.unloadSamples < UINT8_MAX) ++idleTare.unloadSamples;
  } else {
    idleTare.unloadSamples = 0;
  }
  if (idleTare.requestId != 0 && idleTare.cupStillPresent) {
    const IdleTareStatus status = idleScaleTareStatus();
    if (status.requestId == idleTare.requestId && status.phase == IdleTarePhase::QUEUED) {
      const float low = fminf(idleTare.requestMinimumG, weight);
      const float high = fmaxf(idleTare.requestMaximumG, weight);
      if (weight < MIN_AUTOMATION_WEIGHT_G || weight > MAX_AUTOMATION_WEIGHT_G ||
          weight < idleTare.requestPlacementThresholdG ||
          high - low > runtimeConfig.retareStabilityToleranceG) {
        releaseIdleTareRequest(IdleTareReason::UNSTABLE);
      } else {
        idleTare.requestMinimumG = low;
        idleTare.requestMaximumG = high;
      }
    } else if (status.requestId == idleTare.requestId &&
        (status.phase == IdleTarePhase::WRITING ||
         status.phase == IdleTarePhase::SUCCEEDED)) {
      const bool afterBoundary = captureSequence != 0
          ? static_cast<int32_t>(captureSequence - status.captureBoundary) > 0
          : static_cast<int32_t>(atMs - status.startedAtMs) > 0;
      if (afterBoundary) {
        idleTare.unchangedAfterWrite =
            fmaxf(weight, idleTare.requestMaximumG) -
                fminf(weight, idleTare.requestMinimumG) <=
            runtimeConfig.retareStabilityToleranceG;
        if (!idleTare.tareNotified && fabsf(weight) <= FIRST_DROP_BASELINE_SETTLE_G) {
          notifyCupPresenceTare();
          idleTare.tareNotified = true;
          idleTare.effectAtMs = atMs;
          scaleProfileNoteEvent(ScaleProfileEvent::TARE_BASELINE_CONFIRMED,
                                millis(), generation, captureSequence, weight,
                                idleTare.requestId, 0);
        }
        if (status.phase == IdleTarePhase::SUCCEEDED && idleTare.tareNotified) {
          releaseIdleTareRequest(IdleTareReason::EFFECT_CONFIRMED);
        }
      }
    }
  }
  return true;
}

// Field diagnostics: a placement that is not tared must reach the Web UI log.
// An accessory retries on every sample, so it logs only a changed reason.
void logIdlePlacementNotTared(IdleTareReason reason, bool accessory,
                              IdleTareReason previous = IdleTareReason::NONE) {
  if (accessory && reason == previous) return;
  serialTraceCategoryf(
      LogLevel::INFO, DebugCategory::SCALE,
      "%s placed, not tared: %s (empty ref %s, %.1fg)",
      accessory ? "Accessory" : "Cup",
      idleTareReasonName(static_cast<uint8_t>(reason)),
      std::isfinite(cupPresence.emptyAnchorG) ? "known" : "unknown",
      static_cast<double>(idleTare.lastWeightG));
}

void handleIdleCupEvent(CupPresenceEvent event, bool accessory = false) {
  const ProfileControlObservation profile;
  if (event == CupPresenceEvent::REMOVED) {
    idleTare.cupStillPresent = false;
    idleTare.accessoryPlacementId = 0;
    idleTare.accessorySamples = 0;
    idleTare.unloadSamples = 0;
    idleTare.accessoryRemovalAttempted = false;
    idleTare.knownAccessoryMassG = 0.0f;
    if (idleTare.requestId != 0) releaseIdleTareRequest(IdleTareReason::REMOVED);
  }
  if (!idleTare.eligible) {
    if (event == CupPresenceEvent::PLACED) {
      scaleProfileNoteEvent(ScaleProfileEvent::TARE_REJECTED, millis(),
                            idleTare.connectionGeneration, idleTare.lastPacketSequence,
                            idleTare.lastWeightG, cupPresencePlacementId(),
                            static_cast<uint32_t>(idleTare.eligibilityReason));
      logIdlePlacementNotTared(idleTare.eligibilityReason, false);
    }
    return;
  }
  if (cupPresenceState() == CupPresenceState::ABSENT) {
    idleTare.absentObserved = cupPresence.weight.emptyValid || cupPresence.weight.unloadQualified;
  }
  if (event != CupPresenceEvent::PLACED && !accessory) return;
  const bool placedInIdle = accessory || idleTare.absentObserved;
  idleTare.absentObserved = false;
  const IdleTareReason previousReason = idleTare.lastReason;
  if (!placedInIdle || idleTare.requestId != 0 ||
      !getScaleLinkSnapshot().features.has(ScaleFeatureTare)) {
    idleTare.lastReason = !placedInIdle ? IdleTareReason::NO_ABSENCE
        : idleTare.requestId != 0 ? IdleTareReason::SLOT_BUSY : IdleTareReason::UNSUPPORTED;
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_REJECTED, millis(),
                          idleTare.connectionGeneration, idleTare.lastPacketSequence,
                          idleTare.lastWeightG, cupPresencePlacementId(),
                          static_cast<uint32_t>(idleTare.lastReason));
    logIdlePlacementNotTared(idleTare.lastReason, accessory, previousReason);
    return;
  }
  ScaleCommand command;
  command.type = ScaleCommandType::TARE_ONLY;
  command.autoTare = true;
  command.idleTareRequestId = nextIdleTareRequestId++;
  command.cupWeightRequestId = allocateCupWeightRequestId();
  if (nextIdleTareRequestId == 0) nextIdleTareRequestId = 1;
  command.connectionGeneration = idleTare.connectionGeneration;
  command.expiresAtMs = millis() + MAX_AUTOMATION_WEIGHT_AGE_MS;
  command.qualifiedPacketSequence = idleTare.lastPacketSequence;
  command.commandFeedbackExpected =
      getScaleLinkSnapshot().features.has(ScaleFeatureCommandAudibleFeedback);
  if (!enqueueScaleCommand(command)) {
    idleTare.lastReason = IdleTareReason::QUEUE_FULL;
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_REJECTED, millis(),
                          idleTare.connectionGeneration, idleTare.lastPacketSequence,
                          idleTare.lastWeightG, cupPresencePlacementId(),
                          static_cast<uint32_t>(idleTare.lastReason));
    logIdlePlacementNotTared(idleTare.lastReason, accessory, previousReason);
    return;
  }
  idleTare.requestId = command.idleTareRequestId;
  idleTare.cupWeightRequestId = command.cupWeightRequestId;
  trackCupWeightTare(command.cupWeightRequestId);
  idleTare.requestedAtMs = millis();
  idleTare.cupStillPresent = true;
  idleTare.previouslyTared = cupPresenceIsTared();
  idleTare.previousReferenceG = cupPresenceReferenceG();
  scaleProfileNoteEvent(ScaleProfileEvent::TARE_REQUEST, millis(),
                        command.connectionGeneration,
                        command.qualifiedPacketSequence, NAN,
                        command.idleTareRequestId,
                        static_cast<uint32_t>(accessory
                                                  ? ScaleProfileTareOrigin::ACCESSORY
                                                  : ScaleProfileTareOrigin::IDLE_CUP));
  idleTare.requestMinimumG = accessory ? idleTare.accessoryMinimumG : cupPresenceMinimumG();
  idleTare.requestMaximumG = accessory ? idleTare.accessoryMaximumG : cupPresenceMaximumG();
  idleTare.requestPlacementThresholdG = accessory ? runtimeConfig.minimumCupWeightG
                                                : cupPresencePlacementThresholdG();
  if (accessory && idleTare.accessoryMinimumG < 0.0f)
    idleTare.requestPlacementThresholdG =
        cupPresence.emptyAnchorG + runtimeConfig.minimumCupWeightG;
  idleTare.placementId = cupPresencePlacementId();
  idleTare.accessoryRequest = accessory;
  if (accessory) {
    if (idleTare.accessoryMinimumG < 0.0f) idleTare.accessoryRemovalAttempted = true;
    else idleTare.accessoryUsed = true;
  }
  idleTare.unchangedAfterWrite = false;
  idleTare.effectAtMs = 0;
  idleTare.deferredReason = IdleTareReason::NONE;
  idleTare.tareNotified = false;
  emitImmediateCommandAlertIfBuzzer(AlertEvent::TARE);
}

bool idleKnownAccessoryRemoval(float weight) {
  const float toleranceG = runtimeConfig.retareStabilityToleranceG;
  const float cupMassG = -cupPresence.emptyAnchorG - idleTare.knownAccessoryMassG;
  return idleTare.eligible && idleAccessoryRetareEnabled(runtimeConfig.noScaleBbwMode) &&
      idleTare.knownAccessoryMassG >= runtimeConfig.minimumCupWeightG &&
      idleTare.accessoryPlacementId == cupPresencePlacementId() &&
      idleTare.accessoryCycleId == session.id && cupPresenceIsTared() &&
      cupPresence.weight.valid && std::isfinite(cupPresence.emptyAnchorG) &&
      cupMassG >= runtimeConfig.minimumCupWeightG &&
      fabsf(cupMassG - idleTare.knownAccessoryMassG) > 2.0f * toleranceG &&
      weight >= MIN_AUTOMATION_WEIGHT_G &&
      fabsf(weight + idleTare.knownAccessoryMassG) <= toleranceG;
}

bool idleAccessoryUnloadUnsettled(float weight, uint32_t atMs) {
  return idleTare.eligible && idleAccessoryRetareEnabled(runtimeConfig.noScaleBbwMode) &&
      idleTare.knownAccessoryMassG >= runtimeConfig.minimumCupWeightG &&
      idleTare.accessoryPlacementId == cupPresencePlacementId() &&
      idleTare.accessoryCycleId == session.id && cupPresenceIsTared() &&
      cupPresence.weight.valid && std::isfinite(cupPresence.emptyAnchorG) &&
      weight <= runtimeConfig.cupRemovedWeightG &&
      weight > cupPresence.emptyAnchorG + runtimeConfig.retareStabilityToleranceG &&
      (idleTare.unloadSamples < runtimeConfig.retareStabilitySamples ||
       atMs - idleTare.unloadStartedAtMs < runtimeConfig.retareStabilityMinDurationMs);
}

AccessoryState idleAccessoryState() {
  if (!runtimeConfig.autoTareOutsideBrew ||
      !idleAccessoryRetareEnabled(runtimeConfig.noScaleBbwMode)) return AccessoryState::OFF;
  if (!scaleLinkAvailable(getScaleLinkSnapshot())) return AccessoryState::UNKNOWN;
  if (cupPresenceState() != CupPresenceState::PRESENT) return AccessoryState::ABSENT;
  if ((idleTare.requestId != 0 && idleTare.accessoryRequest) || idleTare.accessorySamples != 0)
    return AccessoryState::SETTLING;
  if (!idleTare.eligible || idleTare.accessoryPlacementId != cupPresencePlacementId() ||
      idleTare.accessoryCycleId != session.id || !cupPresenceIsTared() ||
      !cupPresence.weight.valid) return AccessoryState::NOT_TRACKED;
  if (idleTare.knownAccessoryMassG >= runtimeConfig.minimumCupWeightG)
    return AccessoryState::PRESENT;
  return idleTare.accessoryUsed ? AccessoryState::NOT_TRACKED : AccessoryState::ABSENT;
}

// A hand-set item may overshoot and rock before it settles; bound that settle.
constexpr uint32_t ACCESSORY_SETTLE_MAX_MS = 3000;

void observeIdleAccessory(float weight, uint32_t atMs) {
  const float minimumG = runtimeConfig.minimumCupWeightG;
  const uint32_t gapMs = runtimeConfig.retareStabilityMaxGapMs;
  // An accessory lands as a step: it crosses from half the minimum to the
  // minimum within one sample gap. A pour (milk, syrup, coffee) ramps across
  // it. Decide once per crossing and track the landing peak.
  if (weight < minimumG) {
    if (weight <= minimumG / 2.0f) idleTare.accessoryLowAtMs = atMs;
    idleTare.accessoryAbove = idleTare.accessoryStep = false;
  } else if (!idleTare.accessoryAbove) {
    idleTare.accessoryAbove = true;
    idleTare.accessoryStep = atMs - idleTare.accessoryLowAtMs <= gapMs;
    idleTare.accessoryCrossedAtMs = atMs;
    idleTare.accessoryPeakG = weight;
  } else if (atMs - idleTare.accessoryCrossedAtMs <= gapMs) {
    idleTare.accessoryPeakG = fmaxf(weight, idleTare.accessoryPeakG);
  } else if (atMs - idleTare.accessoryCrossedAtMs > ACCESSORY_SETTLE_MAX_MS) {
    idleTare.accessoryStep = false;
  }
  if (idleTare.requestId == 0 && cupPresence.weight.valid &&
      fabsf(weight) <= FIRST_DROP_BASELINE_SETTLE_G) {
    if (idleTare.knownAccessoryMassG == 0.0f) idleTare.accessoryUsed = false;
    idleTare.accessoryRemovalAttempted = false;
  }
  if (!idleTare.eligible || !idleAccessoryRetareEnabled(runtimeConfig.noScaleBbwMode) ||
      idleTare.requestId != 0 ||
      idleTare.accessoryPlacementId != cupPresencePlacementId() ||
      idleTare.accessoryCycleId != session.id || !cupPresenceIsTared() ||
      !cupPresence.weight.valid ||
      !((weight >= minimumG && !idleTare.accessoryUsed) ||
        (weight <= -minimumG &&
         !idleTare.accessoryRemovalAttempted && idleKnownAccessoryRemoval(weight))) ||
      weight > MAX_AUTOMATION_WEIGHT_G || (weight > 0.0f && !idleTare.accessoryStep)) {
    idleTare.accessorySamples = 0;
    return;
  }
  if (idleTare.accessorySamples == 0 || atMs - idleTare.accessoryLastAtMs > gapMs ||
      fmaxf(weight, idleTare.accessoryMaximumG) -
          fminf(weight, idleTare.accessoryMinimumG) >
          runtimeConfig.retareStabilityToleranceG) {
    if (idleTare.accessorySamples != 0)
      scaleProfileNoteEvent(ScaleProfileEvent::CUP_QUALIFICATION_RESET, millis(),
                            idleTare.connectionGeneration, idleTare.lastPacketSequence, weight, 2,
                            static_cast<uint32_t>(atMs - idleTare.accessoryLastAtMs > gapMs
                                ? IdleTareReason::SAMPLE_GAP : IdleTareReason::UNSTABLE));
    idleTare.accessorySamples = 0;
    idleTare.accessoryStartedAtMs = atMs;
    idleTare.accessoryMinimumG = idleTare.accessoryMaximumG = weight;
  }
  idleTare.accessoryMinimumG = fminf(weight, idleTare.accessoryMinimumG);
  idleTare.accessoryMaximumG = fmaxf(weight, idleTare.accessoryMaximumG);
  idleTare.accessoryLastAtMs = atMs;
  if (idleTare.accessorySamples < UINT8_MAX) ++idleTare.accessorySamples;
  if (idleTare.accessorySamples < runtimeConfig.retareStabilitySamples ||
      atMs - idleTare.accessoryStartedAtMs < runtimeConfig.retareStabilityMinDurationMs) return;
  // Settling above the landing peak means the load kept growing: a pour.
  if (weight > 0.0f && idleTare.accessoryMaximumG > idleTare.accessoryPeakG + minimumG / 2.0f) {
    idleTare.accessoryStep = false;
    idleTare.accessorySamples = 0;
    return;
  }
  handleIdleCupEvent(CupPresenceEvent::NONE, true);
}

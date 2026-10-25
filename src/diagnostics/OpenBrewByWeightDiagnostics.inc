void profileControlState(uint32_t captureSequence, uint32_t evidenceConnection) {
  if (!scaleProfiler().recording()) return;
  const auto link = getScaleLinkSnapshot();
  const auto &cup = cupPresence;
  const IdleTareStatus command = idleScaleTareStatus();
  CupTareDiagnostics presentation;
  presentation.eligibilityReason = static_cast<uint8_t>(idleTare.eligibilityReason);
  presentation.requestId = idleTare.requestId;
  presentation.referenceKnown = cupPresenceIsKnown();
  presentation.emptyReferenceBlocked = !session.active && cup.state == CupPresenceState::ABSENT &&
      cup.inNegativeHole && !std::isfinite(cup.emptyAnchorG) && !cup.holdTransitions &&
      cup.weight.pendingId == 0 && idleTare.lastWeightG >= cup.holeWeightG + runtimeConfig.minimumCupWeightG;
  presentation.tared = cupPresenceIsTared();
  presentation.requestPlacementId = idleTare.placementId;
  presentation.placementId = cup.placementId;
  presentation.lastTerminalRequestId = idleTare.lastTerminalRequestId;
  presentation.absentObserved = idleTare.absentObserved;
  using S = ScaleProfileSignal;
  const auto cg = [](float value) -> uint32_t {
    return std::isfinite(value) ? static_cast<uint32_t>(weightToCentigrams(value))
                              : static_cast<uint32_t>(INT32_MIN);
  };
  const ScaleProfileValue values[] = {
      {S::CUP_PRESENT, cup.state == CupPresenceState::PRESENT},
      {S::CUP_HOLD, cup.holdTransitions}, {S::CUP_REFERENCE_KNOWN, cupPresenceIsKnown()},
      {S::CUP_TARED, cupPresenceIsTared()}, {S::EMPTY_READY, cup.weight.emptyValid},
      {S::EMPTY_SETTLING, cup.weight.emptySamples != 0 && !cup.weight.emptyValid},
      {S::EMPTY_REFERENCE_BLOCKED, presentation.emptyReferenceBlocked},
      {S::CUP_NEGATIVE_HOLE, cup.inNegativeHole},
      {S::UNLOAD_QUALIFIED, cup.weight.unloadQualified},
      {S::CUP_SETTLING, cup.placeStabilitySamples != 0},
      {S::CUP_REMOVAL_PENDING, cup.removedConfirmations != 0},
      {S::CUP_MASS_VALID, cup.weight.valid}, {S::EMPTY_ANCHOR_CG, cg(cup.emptyAnchorG)},
      {S::OCCUPIED_REFERENCE_CG, cg(cup.occupiedReferenceG)}, {S::PLACEMENT_ID, cup.placementId},
      {S::IDLE_ELIGIBILITY, static_cast<uint32_t>(idleTare.eligibilityReason)},
      {S::IDLE_STATUS, idleTarePresentationCode(presentation, cup.state == CupPresenceState::PRESENT)},
      {S::IDLE_DEFERRED_REASON, static_cast<uint32_t>(idleTare.deferredReason)},
      {S::IDLE_READY_FOR_CUP, idleTare.absentObserved}, {S::IDLE_REQUEST_ID, idleTare.requestId},
      {S::IDLE_LAST_REASON, static_cast<uint32_t>(idleTare.lastReason)},
      {S::IDLE_COMMAND_PHASE, static_cast<uint32_t>(command.phase)},
      {S::ACCESSORY_SETTLING, idleTare.accessorySamples != 0},
      {S::CYCLE_ID, session.id}, {S::CYCLE_ACTIVE, session.active},
      {S::WEIGHT_CONTROL, static_cast<uint32_t>(session.weightControlState)},
      {S::STREAM, static_cast<uint32_t>(observedWeightStreamStateAt(millis(), link))},
      {S::FIRST_FLOW_PHASE, static_cast<uint32_t>(session.firstFlow.phase)},
      {S::TOUCH_PHASE, static_cast<uint32_t>(session.accidentalTouchPhase)},
      {S::TOUCH_CLASS, static_cast<uint32_t>(session.accidentalTouchClass)},
      {S::BASELINE_WAITING, session.awaitingPostTareBaseline},
      {S::RETARE_OPEN, retareWindowOpen()}, {S::RETARE_PERFORMED, session.retarePerformed},
      {S::RETARE_EFFECT_PENDING, session.retareEffectPending},
      {S::BBW_PROTECTION, bbwProtectionActive()}, {S::SCALE_LOSS_GUARD, session.autoToManualGuardEnforced},
      {S::FINALIZE_CYCLE_ID, pendingFinalize.pending ? pendingFinalize.cycleId : 0},
      {S::CONFIG_REVISION, runtimeConfig.revision}, {S::AUTO_IDLE_TARE, runtimeConfig.autoTareOutsideBrew},
      {S::AUTO_TARE, session.active ? session.config.autoTare : runtimeConfig.autoTare},
      {S::AUTO_RETARE, session.active ? session.config.autoRetare : runtimeConfig.autoRetare},
      {S::CUP_PROTECTION_ENABLED, session.active ? session.config.cupProtectionEnabled : runtimeConfig.cupProtectionEnabled},
      {S::STOP_IF_REMOVED, session.active ? session.config.stopIfCupRemoved : runtimeConfig.stopIfCupRemoved},
      {S::REQUIRE_CUP, session.active ? session.config.requireCupToStart : runtimeConfig.requireCupToStart},
      {S::TOUCH_ENABLED, session.active ? session.config.avoidAccidentalTouchEnabled : runtimeConfig.avoidAccidentalTouchEnabled},
      {S::GOAL_CG, cg(goalWeightDgToGrams(
          session.active ? session.config.goalWeightDg : runtimeConfig.goalWeightDg))},
      {S::OFFSET_CG, cg(session.active ? session.config.weightOffsetG : runtimeConfig.weightOffsetG)},
      {S::NO_SCALE_BBW_MODE, runtimeConfig.noScaleBbwMode},
      {S::MIN_CUP_CG, cg(runtimeConfig.minimumCupWeightG)},
      {S::REMOVED_CG, cg(runtimeConfig.cupRemovedWeightG)},
      {S::STABILITY_TOLERANCE_CG, cg(runtimeConfig.retareStabilityToleranceG)},
      {S::STABILITY_SAMPLES, runtimeConfig.retareStabilitySamples},
      {S::STABILITY_DURATION_MS, runtimeConfig.retareStabilityMinDurationMs},
      {S::STABILITY_GAP_MS, runtimeConfig.retareStabilityMaxGapMs},
      {S::BASELINE_GRACE_MS, session.active ? session.config.postTareBaselineGraceMs
                                           : runtimeConfig.postTareBaselineGraceMs},
      {S::RETARE_WINDOW_MS, session.active ? session.config.retareWindowMs : runtimeConfig.retareWindowMs},
      {S::IDLE_PLACEMENT_ID, idleTare.placementId},
      {S::IDLE_ORIGIN, static_cast<uint32_t>(idleTare.accessoryRequest
          ? ScaleProfileTareOrigin::ACCESSORY : ScaleProfileTareOrigin::IDLE_CUP)},
      {S::SHOT_TARE_REQUEST_ID, session.appliedTareRequestId},
      {S::RETARE_REQUEST_ID, session.retareRequestId},
  };
  scaleProfiler().noteValues(values, sizeof(values) / sizeof(values[0]), millis(),
                             link.connectionGeneration,
                             evidenceConnection == link.connectionGeneration ? captureSequence : 0);
}

bool liveConfigPersistPending() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  settingsPersistMux.lock();
  const bool pending = runtimePersistPending || settingsPersistInFlight ||
                       settingsPersistResultReady;
  settingsPersistMux.unlock();
  return pending;
#else
  return runtimePersistPending;
#endif
}

void publishControlGate() {
  ControlGateSnapshot next;
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  BootCapabilities effectiveBoot = bootCapabilities;
  effectiveBoot.criticalFaultLatched =
      effectiveBoot.criticalFaultLatched || criticalTaskWatchdogFaulted() ||
      relay.state == RelaySafetyState::LOCKOUT;
  next.bootState = effectiveBoot.state();
  next.state = stopperState;
  next.activeCycle = session.active || backflushActive(stopperState);
  next.relayClosed = relay.closed;
  next.machineRunning = machineIsRunning();
  next.physicalActivatorOn = rawActivatorOn;
  next.maintenanceLeaseActive = maintenanceLease.active;
  next.maintenanceLeaseId = maintenanceLease.active ? maintenanceLease.id : 0;
  next.source = session.active ? session.source : ControlSource::NONE;
  // Single writer on the control task: an unchanged gate needs no commit —
  // readers keep the identical value and the RF gate setter is idempotent.
  if (publishedControlGate.bootState == next.bootState &&
      publishedControlGate.state == next.state &&
      publishedControlGate.activeCycle == next.activeCycle &&
      publishedControlGate.relayClosed == next.relayClosed &&
      publishedControlGate.machineRunning == next.machineRunning &&
      publishedControlGate.physicalActivatorOn == next.physicalActivatorOn &&
      publishedControlGate.maintenanceLeaseActive ==
          next.maintenanceLeaseActive &&
      publishedControlGate.maintenanceLeaseId == next.maintenanceLeaseId &&
      publishedControlGate.source == next.source) {
    return;
  }
  {
    TaskLockGuard lock(controlGateMutex);
    publishedControlGate = next;
  }
  const bool actualShot = session.active &&
      (stopperState == StopperState::BREW || stopperState == StopperState::MANUAL_NO_SCALE);
  const bool wasShot = outboundShotActive.exchange(actualShot, std::memory_order_acq_rel);
  if (actualShot && !wasShot) inhibitMachineIntegrationCloud();
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  networkManager.syncControlCriticalRf(next.activeCycle || next.relayClosed, true);
#endif
}

// Publish-side gather cache; single writer is the control task, so the
// guard fields need no atomics of their own. While the store generation and
// the good-history mode are unchanged, the publish reuses the cached gather
// and skips the shotStoreMutex round trip. The generation is read BEFORE the fetch so
// a mutation landing mid-fetch makes the cached generation stale and the
// next publish re-fetches. Every wrapper that mutates the shot/history/
// curve stores or the last-shot snapshot bumps shotStoreDirtyGeneration;
// the memory_order_relaxed bump sites are same-task (control task) today,
// while network-task wrappers bump with release and pair with the acquire
// load here. Direct store mutation without a wrapper does not propagate
// while the guard holds (pinned by host test h11). The factory-reset path
// bumps the generation too, so its wiped stores re-fetch immediately
// (host builds aside: that wrapper is firmware-only).
bool statusStoreCacheValid = false;
uint32_t statusStoreCacheGeneration = 0;
bool statusStoreCacheIncludeGoodHistory = false;
uint32_t statusShotBootId = 0;
PersistedLastShot statusLastShot = {};
PersistedLastShot statusLastGoodShot = {};
bool statusLastGoodLinked = false;
HistoryRecord statusNewestActivation = {};
bool statusHasNewestActivation = false;
// The live or just-ended activity's curve; a committed shot's curve reaches
// readers from the curve store instead.
static OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotCurveRecord statusLiveCurve;
// Drip chart gather, re-read only when the store or the active preset moves.
bool statusDripCacheValid = false;
uint32_t statusDripCacheGeneration = 0;
uint8_t statusDripCachePreset = 0;
uint8_t statusDripCount = 0;
uint16_t statusDripSkipped = 0;
uint32_t statusDripId[10] = {};
int16_t statusDripCg[10] = {};

void publishControlStatus() {
  const uint32_t now = millis();
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  const ScaleLinkSnapshot scaleLink = getScaleLinkSnapshot();
  const MachineStatusSample machine = machineSampleStatus();
  const bool weightFresh =
      scaleLinkAvailable(scaleLink) && currentWeightIsFresh(now, scaleLink.connectionGeneration);
  // Gather other owners before locking publication. Waiting for one producer
  // must not prevent readers from using the last committed control snapshot.
  char preferredMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char preferredName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  copyPreferredScaleMac(preferredMac, sizeof(preferredMac));
  copyPreferredScaleName(preferredName, sizeof(preferredName));
  const uint32_t macPauseRemainingMs = scaleMacCachePauseRemainingMs(now);
  const bool persistPending = liveConfigPersistPending();
  const CupTareDiagnostics cupTare = captureCupTareDiagnostics();
  // Sub-phases ride the loop's shared phase timeline; boot-time publishes
  // run before the first loop iteration and skip recording.
  const bool recordStatusPhases = loopPhaseStartedAtUs != 0;
  if (recordStatusPhases) finishLoopPhase(LoopPhase::HOUSEKEEPING_STATUS_GATHER);
  const bool includeGoodHistory = !session.active;
  const uint32_t gatherGeneration =
      shotStoreDirtyGeneration.load(std::memory_order_acquire);
  if (!statusStoreCacheValid ||
      statusStoreCacheGeneration != gatherGeneration ||
      statusStoreCacheIncludeGoodHistory != includeGoodHistory) {
    statusLastGoodLinked = copyShotStoreStatus(
        statusShotBootId, statusLastShot, statusLastGoodShot,
        statusNewestActivation, statusHasNewestActivation, includeGoodHistory);
    statusStoreCacheGeneration = gatherGeneration;
    statusStoreCacheIncludeGoodHistory = includeGoodHistory;
    statusStoreCacheValid = true;
  }
  if (!statusDripCacheValid || statusDripCacheGeneration != gatherGeneration ||
      statusDripCachePreset != presetBank.activeId) {
    TaskLockGuard lock(shotStoreMutex);
    statusDripCount = static_cast<uint8_t>(shotLog.recentDrips(
        presetBank.activeId, statusDripId, statusDripCg, statusDripSkipped, 10));
    statusDripCacheGeneration = gatherGeneration;
    statusDripCachePreset = presetBank.activeId;
    statusDripCacheValid = true;
  }
  // Reads the shot store (cached): never under the publication lock.
  const corridor::LossDeadline scaleLossProjection =
      idleScaleLossDeadline(activeShotPreset(presetBank));
  // The curve source is control-task owned: refresh it outside the lock.
  if (session.active) shotCurveSampler.snapshot(statusLiveCurve);
  else if (!homeCycleResult.valid) resetShotCurveRecord(statusLiveCurve);
  else if (pendingFinalize.cycleId == homeCycleResult.cycleId)
    copyShotCurve(statusLiveCurve, pendingFinalize.curve);
  else shotCurveSampler.snapshot(statusLiveCurve);
  if (recordStatusPhases)
    finishLoopPhase(LoopPhase::HOUSEKEEPING_STATUS_STORE_COPY);
  const uint32_t statusLockStartedUs = micros();
  TaskLockGuard lock(controlStatusMutex);
  noteMutexWaitUs(controlStatusWaitLastUs, controlStatusWaitMaxUs,
                  micros() - statusLockStartedUs);
  ControlStatusFields &next = publishedControlStatus;
  // The signal level's hysteresis needs the published level the reset clears.
  const uint8_t scaleSignalLevel = next.scaleSignalLevel;
  new (&next) ControlStatusFields{};
  next.cupTare = cupTare;
  next.snapshotVersion = ++controlStatusVersion;
  if (controlStatusVersion == 0) {
    controlStatusVersion = 1;
    next.snapshotVersion = controlStatusVersion;
  }
  next.state = stopperState;
  next.activeCycle = session.active;
  next.relayClosed = relay.closed;
  machineFillStatus(next, machine);
  next.virtualHoldOn = virtualHoldOn;
  next.remoteControlEnabled = REMOTE_MACHINE_CONTROL_ENABLED;
  next.source = session.active ? session.source : ControlSource::NONE;
  next.cycleId = session.active ? session.id : 0;
  next.bootId = statusShotBootId;
  next.maintenanceLeaseActive = maintenanceLease.active;
  next.maintenanceLeaseId = maintenanceLease.active ? maintenanceLease.id : 0;
  next.maintenanceStartedAtMs =
      maintenanceLease.active ? maintenanceLease.startedAtMs : 0;
  next.scheduledRestartState =
      static_cast<uint8_t>(scheduledRestartPlanner.state());
  next.scheduledRestartCause =
      static_cast<uint8_t>(scheduledRestartPlanner.cause());
  next.quickRevertPolicy = static_cast<uint8_t>(sessionQuickSettings.revertPolicy);
  next.quickOverrideMask = sessionQuickSettings.mask;
  next.quickRevertRemainingMs = quickRevertRemainingMs(sessionQuickSettings, millis());
  next.safetyState = relay.state;
  next.safetyFault = relay.fault;
  next.safetyGeneration = relay.generation;
  next.safetyHardLimitMs = relay.hardLimitMs;
  const uint32_t electricalElapsed = elapsedMs(relay.closedAtMs);
  next.safetyRemainingMs = relay.closed && electricalElapsed < relay.hardLimitMs
      ? relay.hardLimitMs - electricalElapsed : 0;
  next.backflushWaiting = !session.active && !backflushActive(stopperState) &&
      backflushObservation.valid && backflushObservation.phase == MachineBackflushPhase::AWAITING;
  next.backflushReady = next.backflushWaiting && backflushObservation.permit.extendable &&
      backflushObservation.ready && stopperState == StopperState::READY &&
      firmwareInitializationComplete && !maintenanceLease.active;
  next.backflushUnresolved = backflushObservation.unresolved;
  copyCString(next.backflushStopReason, sizeof(next.backflushStopReason), backflushStopReason);
  next.safetyTimersReady = relay.timersReady;
  next.taskWatchdogReady = relay.watchdogReady;
  next.externalSafetyPresent = relay.externalSafetyPresent;
  next.circuitFeedbackClosed = relay.feedbackClosed;
  next.resetReasonCode = relay.resetReasonCode;
  next.unsafeResetCount = relay.unsafeResetCount;
  next.bootLoopDetected = relay.bootLoopDetected;
  next.resetHistoryCount = relay.resetHistoryCount;
  for (uint8_t i = 0; i < next.resetHistoryCount; ++i)
    next.resetHistory[i] = relay.resetHistory[i];
  next.scaleAvailable = scaleLinkAvailable(scaleLink);
  next.weightControlState = session.active
                                ? session.weightControlState
                                : WeightControlState::INACTIVE;
  next.weightStreamState = telemetryWeightStreamState;
  next.scaleRecoveredStaleCount = scaleRecoveredStaleCount;
  next.scaleRecoveredStaleMs = scaleRecoveredStaleMs;
  next.currentWeightValid = weightFresh;
  next.currentWeightG = next.currentWeightValid ? currentWeight : 0.0f;
  next.currentWeightAgeMs =
      next.currentWeightValid
          ? static_cast<uint32_t>(now - currentWeightReceivedAtMs)
          : 0;
  next.observedWeightValid = observedWeightSequence > 0 &&
                             std::isfinite(observedWeight) &&
                             fabsf(observedWeight) <= MAX_PARSED_WEIGHT_G;
  next.observedWeightG = next.observedWeightValid ? observedWeight : 0.0f;
  next.observedWeightAgeMs = next.observedWeightValid
                                 ? elapsedMs(observedWeightReceivedAtMs)
                                 : 0;
  next.currentTimerValid = next.scaleAvailable && scaleLink.timerValid;
  next.currentTimerMs = next.currentTimerValid ? scaleLink.timerMs : 0;
  next.currentTimerAgeMs = next.currentTimerValid ? scaleLink.timerAgeMs : 0;
  next.currentFlowRateValid = weightFresh && scaleLink.flowRateValid &&
                              scaleLink.features.has(ScaleFeatureFlowRate) &&
                              std::isfinite(scaleLink.flowRateGps) &&
                              elapsedMs(scaleLink.flowRateAtMs) <= MAX_AUTOMATION_WEIGHT_AGE_MS;
  next.currentFlowRateCgS = next.currentFlowRateValid
      ? static_cast<int16_t>(lroundf(std::fmax(-32767.0f,
            std::fmin(32767.0f, scaleLink.flowRateGps * 100.0f))))
      : 0;
  next.scaleConnectionGeneration = scaleLink.connectionGeneration;
  next.scalePacketSequence = scaleLink.packetSequence;
  next.scalePacketGaps = scaleLink.packetGaps;
  next.scaleMaxPacketGapMs = scaleLink.maxPacketGapMs;
  next.scaleWeightUpdateIntervalMs = scaleLink.weightUpdateIntervalMs;
  next.scaleRejectedPackets = scaleLink.rejectedPackets;
  next.scaleReconnects = scaleLink.reconnects;
  next.scaleLastDisconnectReason = scaleLink.lastDisconnectReason;
  next.scaleBleDiagnostics = scaleLink.bleDiagnostics;
  next.scaleRssiValid = scaleLink.rssiValid;
  next.scaleRssi = scaleLink.rssi;
  next.scaleSignalLevel = connectionSignalLevel(scaleSignalLevel, scaleLink.rssiValid, scaleLink.rssi);
  next.uptimeMs = elapsedMs(bootStartedAtMs);
  next.loopIntervalGapMs =
      healthIntervalMaxGapMs > loopIntervalGapMs ? healthIntervalMaxGapMs
                                                 : loopIntervalGapMs;
  next.loopMaxGapMs = loopMaxGapMs;
  next.loopDeadlineMisses = loopDeadlineMisses;
  next.loopMaxExecutionUs = loopMaxExecutionUs;
  next.scaleWorkerMaxGapMs = scaleWorkerMaxGapMsValue();
  next.scaleWorkerDeadlineMisses = scaleWorkerDeadlineMissCount();
  next.scaleWorkerMaxExecutionUs = scaleWorkerMaxExecutionUsValue();
  next.healthSnapshotVersion = healthSnapshotVersion;
  next.healthSnapshotAgeMs = healthSnapshotVersion == 0
                                 ? UINT32_MAX
                                 : elapsedMs(healthSnapshotAtMs);
  next.healthSnapshotValid = healthSnapshotValid;
  next.loopStackMinBytes = loopStackMinBytes;
  next.scaleStackMinBytes = scaleWorkerStackMinBytesValue();
  next.freeHeapBytes = freeHeapBytes;
  next.minimumFreeHeapBytes = minimumFreeHeapBytes;
  next.largestFreeHeapBlockBytes = largestFreeHeapBlockBytes;
  next.internalHeapAllocatedBlocks = internalHeapAllocatedBlocks;
  next.internalHeapFreeBlocks = internalHeapFreeBlocks;
  next.internalHeapFragmentationPermille = internalHeapFragmentationPermille;
  next.psramSizeBytes = psramSizeBytes;
  next.psramFreeBytes = psramFreeBytes;
  next.psramMinimumFreeBytes = psramMinimumFreeBytes;
  next.psramLargestFreeBlockBytes = psramLargestFreeBlockBytes;
  next.bleBackend = 2;
  next.bleRuntimeState = bleRuntimeState;
  next.bleRuntimeLastError = bleRuntimeLastError;
  next.bleRuntimeLastResetReason = bleRuntimeLastResetReason;
  next.bleRuntimeSyncGeneration = bleRuntimeSyncGeneration;
  next.bleRuntimeResetCount = bleRuntimeResetCount;
  next.bleRuntimeHostStackMinBytes = bleRuntimeHostStackMinBytes;
  next.workBufExternal = workBufIsExternal();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  next.jsonArenaExternal = jsonArenaIsExternal();
#else
  next.jsonArenaExternal = false;
#endif
  next.allocExternalFallbackCount = allocExternalFallbackCount();
  next.hwmon = hwmonSnapshot;
  next.scaleEventsDropped = scaleWorkerDroppedEventCount();
  next.config = effectiveRuntimeConfig();
  const ShotPreset &bbwPreset = activeShotPreset(presetBank);
  next.bbwPresetId = bbwPreset.id;
  next.bbwAlpha = bbwPreset.bbwEwmaAlpha;
  next.bbwEwmaOffsetG = bbwPreset.bbwEwmaOffsetG;
  next.bbwRecentCount = statusDripCount;
  next.bbwRecentSkipped = statusDripSkipped;
  memcpy(next.bbwRecentCg, statusDripCg, sizeof(statusDripCg));
  if (const QuickStartEntry *quick =
          quickStartEntry(bbwQuickStart, presetBank, bbwPreset.id, false)) {
    next.bbwQuickState = quick->state;
    next.bbwQuickCount = quick->count;
    next.bbwQuickSetCg = quick->setCg;
    next.bbwQuickCg = quick->quickCg;
    uint8_t afterSet = 0, afterQuick = 0;
    for (uint8_t i = 0; i < statusDripCount; ++i) {
      afterSet += statusDripId[i] >= quick->setAtId;
      afterQuick += statusDripId[i] > quick->quickAtId;
    }
    if (quick->setAtId != 0) next.bbwRecentAfterSet = afterSet;
    if (quick->quickAtId != 0) next.bbwRecentAfterQuick = afterQuick;
  }
  next.lastCycle = lastCycle;
  next.lastShot = statusLastShot;
  next.lastGoodShot = statusLastGoodShot;
  next.homeCycle = homeCycleResult;
  next.homeResolvedAtMs = homeCycleResolvedAtMs;
  next.homePending = pendingFinalize.pending && !session.active &&
                     pendingFinalize.cycleId == next.homeCycle.cycleId;
  if (next.homePending && pendingFinalize.curve.count > 0) {
    next.homeCycle.currentWeightG = pendingFinalize.curve.lastWeightCg / 100.0f;
    next.homeCycle.weightValid = weightFresh;
  }
  next.lastGoodShotHistoryLinked = statusLastGoodLinked;
  if (statusHasNewestActivation) {
    const HistoryRecord &newest = statusNewestActivation;
    next.lastActivationId = newest.id;
    next.lastActivationEndedAtUnixSec = newest.endedAtUnixSec;
    next.lastActivationEndedAtLocalSec = newest.endedAtLocalSec;
    next.lastActivationDurationDs = newest.durationDs;
    next.lastActivationType = newest.type;
    next.lastActivationFlags = newest.flags;
  }
  // Only the used stream and bitmap bytes move under the lock.
  copyShotCurve(publishedControlCurve.shotCurve, statusLiveCurve);
  copyCString(next.scaleProtocol, sizeof(next.scaleProtocol),
              scaleLink.protocolName);
  if (next.scaleAvailable) {
    copyCString(next.connectedScaleMac, sizeof(next.connectedScaleMac),
                scaleLink.connectedMac);
  }
  next.scaleModel = scaleLink.state == ScaleLinkState::CONNECTED
                        ? scaleLink.model : ScaleModel::Unknown;
  copyCString(next.preferredScaleMac, sizeof(next.preferredScaleMac), preferredMac);
  copyCString(next.preferredScaleName, sizeof(next.preferredScaleName), preferredName);
  next.scaleMacCachePauseRemainingMs = macPauseRemainingMs;
  if (session.active) {
    next.cycleFlowDuringRetare = session.flowDuringRetare;
    next.cycleRetarePerformed = session.retarePerformed;
    next.cycleRetareAtMs = session.retareAtMs;
    copyCString(next.cycleScaleName, sizeof(next.cycleScaleName), session.scaleName);
    next.cycleStartedWithScale = session.startedWithScale;
    next.cycleAutomaticBrew = shot.automaticBrew;
    next.cycleTimerOnly = session.config.timerOnly;
    next.cycleFirstDropMs = session.firstDropMs;
    next.cycleRetareFlowFirstDetectedAtMs =
        session.retareFlowFirstDetectedAtMs;
    next.cycleStartedAtMs = session.circuitClosedAtMs != 0U
                                ? session.circuitClosedAtMs
                                : session.startedAtMs;
    next.cycleElapsedMs = machine.circuitElapsedMs;
    PersistedLastShot &home = next.homeCycle;
    home = PersistedLastShot{};
    home.valid = true;
    home.cycleId = session.id;
    home.durationMs = next.cycleElapsedMs;
    home.goalWeightDg = session.config.goalWeightDg;
    home.presetId = session.activePresetId;
    copyCString(home.presetName, sizeof(home.presetName), session.activePresetName);
    copyCString(home.scaleName, sizeof(home.scaleName), session.scaleName);
    copyCString(home.scaleProtocol, sizeof(home.scaleProtocol), scaleLink.protocolName);
    home.shotType = static_cast<uint8_t>(lastShotTypeFromCycle(
        stopperState, session.startedWithScale, session.config.timerOnly, shot.automaticBrew));
    home.scaleAvailable = session.startedWithScale;
    home.weightValid = weightFresh && session.hasWeightAnchor;
    home.currentWeightG = session.lastAcceptedWeightG;
    home.retarePerformed = session.retarePerformed;
    home.firstDropElapsedMs = session.firstDropMs != 0
        ? session.firstDropMs - next.cycleStartedAtMs : 0;
    home.tareElapsedMs = session.retareAtMs != 0
        ? session.retareAtMs - next.cycleStartedAtMs : 0;
    if (home.weightValid && session.scaleBaselineReady && session.firstDropMs != 0 &&
        home.durationMs > home.firstDropElapsedMs + 500U) {
      const float delta = home.currentWeightG - session.scaleBaselineG;
      home.averageFlowGps = delta * 1000.0f /
                           static_cast<float>(home.durationMs - home.firstDropElapsedMs);
      home.averageFlowValid = delta > 0 && std::isfinite(home.averageFlowGps);
    }
    home.extractionExtended = session.extractionExtended;
    home.slowExtractionExtended = session.slowExtractionExtended;
    next.cycleExtractionExtended = session.extractionExtended;
    next.cycleSlowExtractionExtended = session.slowExtractionExtended;
    next.cycleTargetReachedEarly = session.targetReachedEarly;
    // The live cut line f(t) (the planned stop weight before the drip offset).
    next.cycleActiveStopWeightG = cutLineNowDg() / 10.0f;
    next.cycleExpectedEndMs = shot.expectedEndS < NO_CUT_PREDICTION_S
        ? static_cast<uint32_t>(shot.expectedEndS * 1000.0f) : 0U;
    next.cycleTintClass = static_cast<uint8_t>(session.predictor.tintClass);
    next.cycleTintPct = session.predictor.tintPct;
    next.cycleTintNote = static_cast<uint8_t>(session.predictor.tintNote);
    next.cycleMinBbwBrewTimeRemainingMs =
        next.cycleElapsedMs >= session.config.fastBeforeMs
            ? 0U : session.config.fastBeforeMs - next.cycleElapsedMs;
    next.cycleAutoToManualGuardArmed = session.autoToManualGuardArmed;
    next.cycleAutoToManualGuardEnforced = session.autoToManualGuardEnforced;
    next.cycleAccidentalTouchHolding = session.accidentalTouchHolding;
    next.cycleAccidentalTouchPhase =
        static_cast<uint8_t>(session.accidentalTouchPhase);
    next.cycleAccidentalTouchClass =
        static_cast<uint8_t>(session.accidentalTouchClass);
    next.cycleAccidentalTouchPendingCount = session.accidentalTouchPendingCount;
    next.cycleCupRemovedPending = session.cupRemovedPending;
    next.cycleBbwProtectionEnabled = session.bbwProtectionEnabled;
    next.cycleBbwProtectionEnded = session.bbwProtectionEnded;
    if (session.autoToManualGuardEnforced)
      next.cycleAutoToManualGuardRemainingMs = scaleLossBackupRemainingMs();
  }
  next.autoToManualGuardTrendMs =
      static_cast<uint32_t>(scaleLossProjection.deadlineMs);
  next.scaleLossProjectedSource = static_cast<uint8_t>(scaleLossProjection.source);
  next.noScaleShotGuardEnabled = noScaleBbwEnabled(runtimeConfig.noScaleBbwMode);
  next.noScaleShotGuardArmed = noScaleShotGuardArmed;
  next.noScaleShotGuardHold = noScaleShotGuardHold;
  next.noScaleShotGuardScaleWasAvailable = noScaleShotGuardScaleWasAvailable;
  next.noScaleShotGuardCooldownRemainingMs = 0;
  if (noScaleBbwEnabled(runtimeConfig.noScaleBbwMode) &&
      !noScaleShotGuardArmed && noScaleShotGuardActivityAtMs != 0) {
    const uint32_t elapsed = elapsedMs(noScaleShotGuardActivityAtMs);
    next.noScaleShotGuardCooldownRemainingMs =
        elapsed >= runtimeConfig.lastShotCooldownMs
            ? 0U
            : runtimeConfig.lastShotCooldownMs - elapsed;
  }
  next.cupStartGuardHold = cupStartGuardHold;
  next.machineRunState = machine.runState;
  next.cupPresenceState = cupPresenceState();
  next.cupPresent = cupPresenceState() == CupPresenceState::PRESENT;
  next.configPersistPending = persistPending;
  next.configPersistFailed = runtimePersistFailed;
  next.bootCapabilities = bootCapabilities;
  next.bootCapabilities.criticalFaultLatched =
      next.bootCapabilities.criticalFaultLatched ||
      criticalTaskWatchdogFaulted() ||
      relay.state == RelaySafetyState::LOCKOUT;
  next.bootState = next.bootCapabilities.state();
  next.bootComplete = next.bootState == BootState::READY;
  next.bootDegraded = next.bootState == BootState::DEGRADED_SAFE ||
                      next.bootState == BootState::FAULT_LATCHED;
  next.scaleWorkerReady = scaleWorkerReady();
  next.usbConsoleIo4Closed = usbConsoleJumperPresent();
  next.usbSerialEnableSource = usbSerialEnableSource;
  next.bleScanIntensity = static_cast<uint8_t>(liveBleScanIntensity());
  next.bleScanEnabled = liveBleEnabled();
  next.debugEventsDropped =
      __atomic_load_n(&debugLogDroppedSnapshot, __ATOMIC_RELAXED) +
      serialLogDroppedCount();
  lock.unlock();
  publishControlGate();
  if (recordStatusPhases) finishLoopPhase(LoopPhase::HOUSEKEEPING_STATUS_COMMIT);
}

void refreshControlStatus() {
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  return;
#else
  // Waits until loop commits the requested status snapshot, not merely until
  // the request is noticed. Timeout leaves the request flag set.
  // is noticed. Timeout copies the last snapshot and leaves the flag set.
  __atomic_store_n(&controlStatusPublishRequested, true, __ATOMIC_RELEASE);
  const uint32_t startedAtMs = millis();
  while (__atomic_load_n(&controlStatusPublishRequested, __ATOMIC_ACQUIRE)) {
    if (static_cast<uint32_t>(millis() - startedAtMs) >=
        CONTROL_STATUS_REFRESH_WAIT_MS) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
#endif
}

void serviceControlStatusPublish() {
  const RelaySafetySnapshot relaySnap = getRelaySafetySnapshot();
  const bool requested =
      __atomic_load_n(&controlStatusPublishRequested, __ATOMIC_ACQUIRE);
  const bool forcePublish =
      publishedControlStatus.state != stopperState ||
      publishedControlStatus.activeCycle != session.active ||
      publishedControlStatus.relayClosed != relaySnap.closed ||
      publishedControlStatus.machineRunning != machineIsRunning() ||
      publishedControlStatus.safetyGeneration != relaySnap.generation ||
      publishedControlStatus.safetyFault != relaySnap.fault ||
      publishedControlStatus.configPersistPending !=
          liveConfigPersistPending() ||
      publishedControlStatus.configPersistFailed != runtimePersistFailed;
  if (requested || forcePublish) {
    publishControlStatus();
  }
  // Clear after the mutex-protected commit so httpd's wait cannot copy the
  // previous blob. A GET that arrives mid-publish blocks until that snapshot
  // is complete.
  if (requested) {
    __atomic_store_n(&controlStatusPublishRequested, false, __ATOMIC_RELEASE);
  }
}

SerialCliParser serialCliParser;

void resetSerialCliState() {
  serialCliResetParser(serialCliParser);
}

void serialCliReply(const char *message) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  serialCliOutput().println(message);
#else
  Serial.println(message);
#endif
}

void serialCliWipeCommandSecrets(WebCommand &command) {
  command.wipePassword();
}

void serialCliRejectUnsafe() {
  serialCliReply(
      "ERR not ready: paddle OFF, machine circuit open, Ready, no active cycle");
}

void serialCliQueueCommand(WebCommand &command, SerialCliVerb verb) {
  command.requestId = millis();
  if (!enqueueWebCommand(command)) {
    serialCliWipeCommandSecrets(command);
    serialCliReply("ERR control queue full");
    return;
  }
  serialCliWipeCommandSecrets(command);
  char line[48] = {};
  snprintf(line, sizeof(line), "OK queued %s", serialCliVerbName(verb));
  serialCliReply(line);
}

void serialCliQueueIfSafe(WebCommand &command, SerialCliVerb verb) {
  if (!controlAllowsConfigurationNow()) {
    serialCliWipeCommandSecrets(command);
    serialCliRejectUnsafe();
    return;
  }
  serialCliQueueCommand(command, verb);
}

void serialCliWarnIfCycleActive() {
  if (session.active) {
    serialCliReply("WARN cycle active; proceeding");
  }
}

void serialCliQueueNetworkAction(WebCommandType type, SerialCliVerb verb) {
  serialCliWarnIfCycleActive();
  WebCommand command;
  command.type = type;
  command.requestId = millis();
  if (!forwardAcceptedNetworkCommand(command)) {
    serialCliReply("ERR network queue full");
    return;
  }
  char line[48] = {};
  snprintf(line, sizeof(line), "OK queued %s", serialCliVerbName(verb));
  serialCliReply(line);
}

void serialCliFillNetworkDump(SerialCliNetworkDump &dump) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const NetworkStatusSnapshot snap = networkManager.snapshot();
  dump.networkActive = snap.networkActive;
  dump.apActive = snap.apActive;
  dump.httpActive = snap.httpActive;
  dump.wifiConfigured = snap.wifiConfigured;
  dump.staOpen = snap.staOpen;
  dump.staLinkMetricsValid = snap.staLinkMetricsValid;
  dump.staReconnectHeld = snap.staReconnectHeld;
  dump.apStartHeld = snap.apStartHeld;
  dump.httpStartHeld = snap.httpStartHeld;
  dump.devicePasswordFactory = snap.devicePasswordFactory;
  dump.wifiPs = snap.wifiPs;
  dump.ntpMayArm = snap.ntpMayArm;
  dump.apClients = snap.apClients;
  dump.staState = static_cast<uint8_t>(snap.staState);
  dump.staIpMode = snap.staIpMode;
  dump.staConfigState = snap.staConfigState;
  dump.wifiMode = snap.wifiMode;
  dump.channel = snap.channel;
  dump.scanState = snap.scanState;
  dump.ntpState = snap.ntpState;
  dump.staRssi = snap.staRssi;
  dump.staSignalQualityPct = snap.staSignalQualityPct;
  dump.wifiStatus = snap.wifiStatus;
  dump.confirmRemainingMs = snap.confirmRemainingMs;
  dump.taskAgeMs = snap.taskAgeMs;
  dump.taskStackMinBytes = snap.taskStackMinBytes;
  dump.startupFailures = snap.startupFailures;
  dump.lastCommandRequestId = snap.lastCommandRequestId;
  dump.staConnectAgeMs = snap.staConnectAgeMs;
  dump.staReconnectAgeMs = snap.staReconnectAgeMs;
  dump.staReconnectDelayMs = snap.staReconnectDelayMs;
  dump.lastCommandState = snap.lastCommandState;
  dump.iface = snap.iface;
#else
  (void)dump;
#endif
}

void serialCliPrintLiveNetworkStatus(SerialCliVerb verb) {
  SerialCliNetworkDump dump;
  serialCliFillNetworkDump(dump);
  if (verb == SerialCliVerb::WIFI_STATUS) {
    serialCliPrintWifiStatus(dump);
  } else if (verb == SerialCliVerb::AP_STATUS) {
    serialCliPrintApStatus(dump);
  } else if (verb == SerialCliVerb::WEBUI_STATUS) {
    serialCliPrintWebuiStatus(dump);
  } else {
    serialCliPrintNetStatus(dump);
  }
}

// Audit CLI: newest-first bounded dump of the merged latch + durable window.
// The audit surface is small (curated events), so direct printing keeps the
// command synchronous without the deferred LOG_DUMP pipeline.
void serialCliPrintAuditDump(const char *countArg) {
  const uint32_t latest = auditLogLatestSequence();
  long want = 24;
  if (countArg != nullptr && countArg[0] != '\0') {
    want = strtol(countArg, nullptr, 10);
    if (want <= 0) {
      serialCliReply("ERR AUDIT_DUMP count must be positive");
      return;
    }
    if (want > 128) want = 128;
  }
  const uint32_t after = latest > static_cast<uint32_t>(want)
                             ? latest - static_cast<uint32_t>(want)
                             : 0;
  static OPEN_BREW_BY_WEIGHT_PSRAM_BSS AuditRecord auditCliBatch[128];
  uint32_t dropped = 0, rateLimited = 0;
  const size_t count =
      copyAuditEvents(after, auditCliBatch, 128, &dropped, &rateLimited);
  const AuditStoreStatus store = auditStoreStatus();
  char line[160];
  snprintf(line, sizeof(line), "audit: state=%s retained=%lu seqHigh=%lu",
           store.state == AuditStoreState::READY ? "ready"
               : store.state == AuditStoreState::UNSUPPORTED ? "ram-only"
                                                             : "io-error",
           static_cast<unsigned long>(store.retained),
           static_cast<unsigned long>(store.seqHigh));
  serialCliReply(line);
  snprintf(line, sizeof(line), "audit: records=%u dropped=%lu rateLimited=%lu",
           static_cast<unsigned>(count),
           static_cast<unsigned long>(dropped),
           static_cast<unsigned long>(rateLimited));
  serialCliReply(line);
  for (size_t index = count; index > 0; --index) {
    const AuditRecord &record = auditCliBatch[index - 1];
    snprintf(line, sizeof(line),
             "seq=%lu wall=%lu level=%s code=%s arg1=%ld arg2=%ld %.24s",
             static_cast<unsigned long>(record.sequence),
             static_cast<unsigned long>(record.wallSec),
             logLevelName(static_cast<LogLevel>(record.level)),
             debugCodeName(static_cast<DebugCode>(record.code)),
             static_cast<long>(record.argument1),
             static_cast<long>(record.argument2), record.text);
    serialCliReply(line);
  }
}

void serialCliAuditVerbose(const char *modeArg) {
  const bool on =
      modeArg == nullptr || modeArg[0] == '\0' || serialCliEqualsIgnoreCase(modeArg, "on");
  if (!setAuditVerbose(on)) {
    serialCliReply("ERR audit verbose needs a synchronized clock");
    return;
  }
  serialCliReply(on ? "OK audit verbose session armed for 1800 s"
                    : "OK audit verbose session stopped");
}

// Task stacks/CPU and control-loop phase peaks from the existing profiler
// (5 min cap). The copy lands in PSRAM BSS: the loop stack cannot hold it.
void serialCliTaskProfile(const char *modeArg) {
  if (modeArg != nullptr && modeArg[0] != '\0') {
    const bool on = serialCliEqualsIgnoreCase(modeArg, "on");
    healthProfilerRequest.store(on ? HealthProfilerRequest::START : HealthProfilerRequest::STOP,
                                std::memory_order_release);
    serialCliReply(on ? "OK task profiler start requested" : "OK task profiler stop requested");
    return;
  }
  static OPEN_BREW_BY_WEIGHT_PSRAM_BSS TaskProfilerSnapshot cliProfile;
  copyTaskProfiler(cliProfile);
  const LoopPhaseProfilerSnapshot &loop = cliProfile.loopPhases;
  char line[128];
  snprintf(line, sizeof(line), "TASK_PROFILE state=%s stop=%s elapsedMs=%lu samples=%lu truncated=%d",
           taskProfilerStateName(cliProfile.state), taskProfilerStopReasonName(cliProfile.stopReason),
           static_cast<unsigned long>(cliProfile.elapsedMs),
           static_cast<unsigned long>(cliProfile.sampleCount), cliProfile.truncated ? 1 : 0);
  Serial.println(line);
  Serial.println("task,core,stackMinBytes,cpuNowPct,cpuAvgPct");
  for (uint8_t i = 0; i < cliProfile.rowCount && i < TASK_PROFILER_MAX_ROWS; ++i) {
    const TaskProfilerRow &row = cliProfile.rows[i];
    snprintf(line, sizeof(line), "%s,%d,%lu,%.2f,%.2f", row.name, static_cast<int>(row.core),
             static_cast<unsigned long>(row.stackMinBytes), row.currentCpuPct, row.averageCpuPct);
    Serial.println(line);
  }
  snprintf(line, sizeof(line), "loopPeakGapUs=%lu peakDelayUs=%lu peakDispatchUs=%lu peakGapCpuUs=%lu",
           static_cast<unsigned long>(loop.peakGapUs), static_cast<unsigned long>(loop.peakDelayUs),
           static_cast<unsigned long>(loop.peakDispatchUs), static_cast<unsigned long>(loop.peakGapCpuUs));
  Serial.println(line);
  Serial.println("phase,maxUs,maxCpuUs,avgUs,peakGapUs");
  for (uint8_t i = 0; i < loop.rowCount && i < LOOP_PHASE_COUNT; ++i) {
    const LoopPhaseProfilerRow &row = loop.rows[i];
    snprintf(line, sizeof(line), "%s,%lu,%lu,%lu,%lu", row.name != nullptr ? row.name : "-",
             static_cast<unsigned long>(row.maxExecutionUs), static_cast<unsigned long>(row.maxCpuUs),
             static_cast<unsigned long>(row.averageExecutionUs),
             static_cast<unsigned long>(row.peakGapExecutionUs));
    Serial.println(line);
  }
}

void serialCliPrintLiveHealth() {
  SerialCliHealthDump dump;
  dump.freeHeapBytes = freeHeapBytes;
  dump.minimumFreeHeapBytes = minimumFreeHeapBytes;
  dump.largestFreeHeapBlockBytes = largestFreeHeapBlockBytes;
  dump.internalHeapAllocatedBlocks = internalHeapAllocatedBlocks;
  dump.internalHeapFreeBlocks = internalHeapFreeBlocks;
  dump.internalHeapFragmentationPermille = internalHeapFragmentationPermille;
  dump.psramSizeBytes = psramSizeBytes;
  dump.psramFreeBytes = psramFreeBytes;
  dump.psramLargestFreeBlockBytes = psramLargestFreeBlockBytes;
  dump.bleBackend = 2;
  dump.bleRuntimeState = bleRuntimeState;
  dump.bleRuntimeLastError = bleRuntimeLastError;
  dump.bleRuntimeLastResetReason = bleRuntimeLastResetReason;
  dump.bleRuntimeSyncGeneration = bleRuntimeSyncGeneration;
  dump.bleRuntimeResetCount = bleRuntimeResetCount;
  dump.bleRuntimeHostStackMinBytes = bleRuntimeHostStackMinBytes;
  dump.workBufExternal = workBufIsExternal();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  dump.jsonArenaExternal = jsonArenaIsExternal();
#else
  dump.jsonArenaExternal = false;
#endif
  dump.allocExternalFallbackCount = allocExternalFallbackCount();
  dump.loopMaxGapMs = loopMaxGapMs;
  dump.healthIntervalMaxGapMs =
      healthIntervalMaxGapMs > loopIntervalGapMs ? healthIntervalMaxGapMs
                                                 : loopIntervalGapMs;
  dump.loopStackMinBytes = loopStackMinBytes;
  dump.scaleWorkerStackMinBytes = scaleWorkerStackMinBytesValue();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  dump.networkStackMinBytes = networkManager.snapshot().taskStackMinBytes;
#endif
  dump.heapAlertLatched = healthHeapAlertLatched;
  dump.stackAlertLatched = healthStackAlertLatched;
  dump.loopGapAlertLatched = healthLoopGapAlertLatched;
  dump.cpuLoadValid = hwmonSnapshot.cpuLoadValid;
  dump.cpuMhz = hwmonSnapshot.cpuMhz;
  dump.cpuLoad5s = hwmonSnapshot.cpuLoad5s;
  dump.cpuLoad1m = hwmonSnapshot.cpuLoad1m;
  dump.cpuLoad5m = hwmonSnapshot.cpuLoad5m;
  dump.cpu0Busy = hwmonSnapshot.cpu0Busy;
  dump.cpu1Busy = hwmonSnapshot.cpu1Busy;
  dump.tempValid = hwmonSnapshot.tempValid;
  dump.tempC = hwmonSnapshot.tempC;
  dump.tempPeakC = hwmonSnapshot.tempPeakC;
  serialCliPrintHealth(dump);
}

void serialCliPrintLiveHeap() {
  serialCliPrintHeap(sampleHeapCaps(), sampleInternalFreeBlocks());
}

void serialCliPrintLiveScaleStatus() {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  SerialCliScaleDump dump;
  dump.state = link.state == ScaleLinkState::CONNECTED ? "CONNECTED"
                                                       : "DISCONNECTED";
  dump.protocolName = link.protocolName;
  copyPreferredScaleMac(dump.preferredMac, sizeof(dump.preferredMac));
  copyPreferredScaleName(dump.preferredName, sizeof(dump.preferredName));
  dump.disconnectSequence = link.disconnectSequence;
  dump.connectionGeneration = link.connectionGeneration;
  dump.packetSequence = link.packetSequence;
  dump.packetGaps = link.packetGaps;
  dump.rejectedPackets = link.rejectedPackets;
  dump.reconnects = link.reconnects;
  dump.recoveredStaleCount = scaleRecoveredStaleCount;
  dump.recoveredStaleMs = scaleRecoveredStaleMs;
  dump.lastDisconnectReason = link.lastDisconnectReason;
  dump.rssiValid = link.rssiValid;
  dump.rssi = link.rssi;
  dump.workerAgeMs = elapsedMs(link.workerProgressAtMs);
  dump.timerValid = link.timerValid;
  dump.timerMs = link.timerMs;
  dump.timerAgeMs = link.timerAgeMs;
  dump.weightFresh = currentWeightIsFresh();
  dump.currentWeightG = currentWeight;
  dump.scanIntensity = bleScanIntensityName(liveBleScanIntensity());
  serialCliPrintScaleStatus(dump);
}

void serialCliPrintLiveNtpStatus() {
  const TimeStatusSnapshot time = g_wallClock.snapshot(millis());
  SerialCliNtpDump dump;
  dump.state = time.state;
  dump.utcSec = time.utcSec;
  dump.lastSyncAgeMs = time.lastSyncAgeMs;
  dump.nextRetryInMs = time.nextRetryInMs;
  dump.consecutiveFailures = time.consecutiveFailures;
  copyCString(dump.activeServer, sizeof(dump.activeServer), time.activeServer);
  dump.ntpServerPreset = runtimeConfig.ntpServerPreset;
  copyCString(dump.ntpServerCustom, sizeof(dump.ntpServerCustom),
              runtimeConfig.ntpServerCustom);
  copyCString(dump.timezoneId, sizeof(dump.timezoneId),
              runtimeConfig.timezoneId);
  dump.timezoneOffsetMinutes = 0;
  (void)timeZoneOffsetAtOrUtc(runtimeConfig.timezoneId, time.utcSec,
                                dump.timezoneOffsetMinutes);
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  dump.staUp = networkManager.snapshot().staState == StaState::CONNECTED;
#else
  dump.staUp = false;
#endif
  serialCliPrintNtpStatus(dump);
}

void serialCliPrintLiveLogDump() {
  if (session.active || circuitClosed) {
    serialCliReply("ERR LOG dump deferred; circuit/cycle active");
    return;
  }
  serialLogDumpCount = copyDebugEvents(
      0, serialLogDumpSnapshot, DEBUG_EVENT_CAPACITY, nullptr);
  serialLogDumpIndex = 0;
  serialCliPrintLogDumpPreamble(serialLogDumpCount,
                                currentRingRetainLogLevel());
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // The host sink is an in-memory stub and cannot block. Preserve the
  // synchronous CLI contract used by portable tests.
  while (serialLogDumpIndex < serialLogDumpCount) {
    writeSerialLogLine(serialLogDumpSnapshot[serialLogDumpIndex++]);
  }
#endif
}

void serviceSerialLogDump() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // One bounded record per control iteration. Formatting is local and the
  // sink only attempts a zero-timeout queue send; USB I/O belongs to
  // serial_log on core 0.
  if (serialLogDumpIndex < serialLogDumpCount) {
    writeSerialLogLine(serialLogDumpSnapshot[serialLogDumpIndex++]);
  }
#endif
}

void serialCliApplyDebugPersist(bool serialOn, LogLevel ringLevel,
                                const char *okMessage) {
  if (!settingsPersistenceAvailable()) {
    serialCliReply("ERR settings persistence unavailable");
    return;
  }
  if (!serialOn) {
    serialCliReply(okMessage);
  }
  RuntimeConfig candidate = runtimeConfig;
  setSerialLogLevel(candidate,
                    serialOn ? (ringLevel == LogLevel::DEBUG ? LogLevel::DEBUG
                                                              : LogLevel::INFO)
                             : LogLevel::NONE);
  candidate.ringRetainLogLevel = static_cast<uint8_t>(ringLevel);
  ++candidate.revision;
  if (candidate.revision == 0) {
    candidate.revision = 1;
  }
  commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
  if (serialOn) {
    serialCliReply(okMessage);
  }
}

void dispatchSerialCliRequest(SerialCliRequest &request) {
  switch (request.verb) {
    case SerialCliVerb::NONE:
      return;
    case SerialCliVerb::HELP:
      serialCliPrintHelp();
      return;
    case SerialCliVerb::HELLO:
      serialCliReply("how are you");
      return;
    case SerialCliVerb::REBOOT: {
      WebCommand command;
      command.type = WebCommandType::RESTART;
      serialCliQueueCommand(command, request.verb);
      return;
    }
    case SerialCliVerb::UNKNOWN:
    case SerialCliVerb::LINE_TOO_LONG:
    case SerialCliVerb::INVALID_ARGS:
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
      serialCliOutput().print("ERR ");
      serialCliOutput().println(request.error != nullptr ? request.error
                                                        : "invalid arguments");
#else
      Serial.print("ERR ");
      Serial.println(request.error != nullptr ? request.error
                                              : "invalid arguments");
#endif
      return;
    case SerialCliVerb::FACTORY_RESET: {
      WebCommand command;
      command.type = WebCommandType::FACTORY_RESET;
      serialCliQueueIfSafe(command, request.verb);
      return;
    }
    case SerialCliVerb::RESET_DEVICE_PASSWORD: {
      WebCommand command;
      command.type = WebCommandType::RESET_DEVICE_PASSWORD;
      serialCliQueueIfSafe(command, request.verb);
      return;
    }
    case SerialCliVerb::SET_DEVICE_PASSWORD: {
      WebCommand command;
      command.setNetworkType(WebCommandType::CHANGE_DEVICE_PASSWORD);
      copyCString(command.network.password, sizeof(command.network.password), request.arg1);
      serialCliQueueIfSafe(command, request.verb);
      memset(request.arg1, 0, sizeof(request.arg1));
      return;
    }
    case SerialCliVerb::SET_WIFI: {
      WebCommand command;
      command.setNetworkType(WebCommandType::SAVE_NETWORK);
      copyCString(command.network.ssid, sizeof(command.network.ssid), request.arg1);
      copyCString(command.network.password, sizeof(command.network.password), request.arg2);
      command.network.openNetwork = request.openNetwork;
      command.network.commitConfirmed = true;
      serialCliQueueIfSafe(command, request.verb);
      memset(request.arg1, 0, sizeof(request.arg1));
      memset(request.arg2, 0, sizeof(request.arg2));
      return;
    }
    case SerialCliVerb::CLEAR_WIFI: {
      WebCommand command;
      command.type = WebCommandType::FORGET_NETWORK;
      serialCliQueueIfSafe(command, request.verb);
      return;
    }
    case SerialCliVerb::RESET_NETWORK_AP: {
      WebCommand command;
      command.type = WebCommandType::RESET_NETWORK_AP;
      serialCliQueueIfSafe(command, request.verb);
      return;
    }
    case SerialCliVerb::CLEAR_SHOTS: {
      if (!controlAllowsConfigurationNow()) {
        serialCliRejectUnsafe();
        return;
      }
      if (!clearShotLog()) {
        serialCliReply("ERR shot history clear failed");
        return;
      }
      serialCliReply("OK shots cleared");
      return;
    }
    case SerialCliVerb::SERIAL_DEBUG_ON:
    case SerialCliVerb::SERIAL_DEBUG_OFF: {
      if (!settingsPersistenceAvailable()) {
        serialCliReply("ERR settings persistence unavailable");
        return;
      }
      const bool enable = request.verb == SerialCliVerb::SERIAL_DEBUG_ON;
      if (!enable) {
        serialCliReply("OK serial debug off");
      }
      RuntimeConfig candidate = runtimeConfig;
      setSerialLogLevel(candidate, enable ? LogLevel::INFO : LogLevel::NONE);
      ++candidate.revision;
      if (candidate.revision == 0) {
        candidate.revision = 1;
      }
      commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
      if (enable) {
        serialCliReply("OK serial debug on");
      }
      return;
    }
    case SerialCliVerb::DEBUG_FULL:
      serialCliApplyDebugPersist(true, LogLevel::DEBUG, "OK debug full");
      return;
    case SerialCliVerb::DEBUG_OFF:
      serialCliApplyDebugPersist(false, LogLevel::NONE, "OK debug off");
      return;
    case SerialCliVerb::DEBUG_STATUS:
      serialCliPrintDebugStatus(serialLogLevel, ringRetainLogLevel);
      return;
    case SerialCliVerb::WIFI_CONNECT:
      serialCliQueueNetworkAction(WebCommandType::WIFI_CONNECT, request.verb);
      return;
    case SerialCliVerb::WIFI_DISCONNECT:
      serialCliQueueNetworkAction(WebCommandType::WIFI_DISCONNECT,
                                  request.verb);
      return;
    case SerialCliVerb::WIFI_RESTART:
      serialCliQueueNetworkAction(WebCommandType::WIFI_RESTART, request.verb);
      return;
    case SerialCliVerb::AP_START:
      serialCliQueueNetworkAction(WebCommandType::AP_START, request.verb);
      return;
    case SerialCliVerb::AP_STOP:
      serialCliQueueNetworkAction(WebCommandType::AP_STOP, request.verb);
      return;
    case SerialCliVerb::WEBUI_START:
      serialCliQueueNetworkAction(WebCommandType::WEBUI_START, request.verb);
      return;
    case SerialCliVerb::WEBUI_STOP:
      serialCliQueueNetworkAction(WebCommandType::WEBUI_STOP, request.verb);
      return;
    case SerialCliVerb::WEBUI_RESTART:
      serialCliQueueNetworkAction(WebCommandType::WEBUI_RESTART, request.verb);
      return;
    case SerialCliVerb::WIFI_STATUS:
    case SerialCliVerb::AP_STATUS:
    case SerialCliVerb::WEBUI_STATUS:
    case SerialCliVerb::NET_STATUS:
      serialCliPrintLiveNetworkStatus(request.verb);
      return;
    case SerialCliVerb::LOG_DUMP:
      serialCliPrintLiveLogDump();
      return;
    case SerialCliVerb::HEALTH:
      serialCliPrintLiveHealth();
      return;
    case SerialCliVerb::SCALE_STATUS:
      serialCliPrintLiveScaleStatus();
      return;
    case SerialCliVerb::NTP_STATUS:
      serialCliPrintLiveNtpStatus();
      return;
    case SerialCliVerb::HEAP:
      serialCliPrintLiveHeap();
      return;
    case SerialCliVerb::BOOT_HEAP:
      serialCliPrintBootHeap();
      return;
    case SerialCliVerb::TASK_PROFILE:
      serialCliTaskProfile(request.arg1);
      return;
    case SerialCliVerb::AUDIT_DUMP:
      serialCliPrintAuditDump(request.arg1);
      return;
    case SerialCliVerb::AUDIT_CLEAR:
      // The control loop must not take the flash lock; the persistence
      // worker owns the erase (same queue contract as REBOOT).
      serialCliReply(dispatchAuditClearWork()
                         ? "OK queued AUDIT_CLEAR"
                         : "ERR audit clear could not be queued");
      return;
    case SerialCliVerb::AUDIT_VERBOSE:
      serialCliAuditVerbose(request.arg1);
      return;
  }
}

void serviceSerialCli() {
  size_t consumed = 0;
  while (Serial.available() > 0 &&
         consumed < SERIAL_CLI_MAX_BYTES_PER_LOOP) {
    const int incoming = Serial.read();
    ++consumed;
    if (incoming < 0) {
      break;
    }
    SerialCliRequest request;
    if (serialCliFeed(serialCliParser, static_cast<char>(incoming),
                      request)) {
      (void)beginSerialCliOutput();
      dispatchSerialCliRequest(request);
      endSerialCliOutput();
    }
  }
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
void serviceBootRecoverySafety() {
  serviceRelaySafety();
  localBuzzer.service();
  if (!feedCurrentTaskWatchdog()) {
    reportTaskWatchdogFault();
    tripRelaySafety(RelaySafetyFault::TASK_WATCHDOG_FAILURE);
  }
  serviceSafetyHeartbeat(false);
}

void waitForRecoveryBuzzer() {
  while (localBuzzer.busy()) {
    serviceBootRecoverySafety();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void completeBootRecovery(RecoveryOperation operation);

void abandonFailedBootRecovery(RecoveryOperation operation) {
  (void)playRecoveryCue(BuzzerCue::RECOVERY_ERROR);
  waitForRecoveryBuzzer();
  if (operation == RecoveryOperation::FACTORY_RESET) {
    (void)resetPersistedSettingsToFactory(persistedSettings);
  } else if (operation == RecoveryOperation::NETWORK_ACCESS_RESET) {
    (void)resetPersistedNetworkAccess(persistedSettings);
  }
  (void)abandonRecoveryIntent();
}

void abandonMalformedRecoveryIntent() {
  (void)playRecoveryCue(BuzzerCue::RECOVERY_ERROR);
  waitForRecoveryBuzzer();
  (void)resetPersistedSettingsToFactory(persistedSettings);
  (void)abandonRecoveryIntent();
}

bool applyBootRecoveryOperation(RecoveryOperation operation) {
  if (operation == RecoveryOperation::NETWORK_ACCESS_RESET) {
    return resetPersistedNetworkAccess(persistedSettings);
  }
  if (operation != RecoveryOperation::FACTORY_RESET) {
    return false;
  }

  return resetAllDurableStoresForNetwork(persistedSettings);
}

void completeBootRecovery(RecoveryOperation operation) {
  // Do not rewrite a valid latch: a failed put on full NVS can tear it.
  (void)ensureRecoveryIntent(operation);
  if (!applyBootRecoveryOperation(operation)) {
    abandonFailedBootRecovery(operation);
    return;
  }
  (void)clearRecoveryIntent();

  const BuzzerCue success =
      operation == RecoveryOperation::FACTORY_RESET
          ? BuzzerCue::FACTORY_RESET_OK
          : BuzzerCue::NETWORK_RESET_OK;
  (void)playRecoveryCue(success);
  waitForRecoveryBuzzer();
  // Preserve the final 50 ms pause specified by the recovery cue before the
  // reset cuts power to the buzzer output.
  const uint32_t pauseStartedAtMs = millis();
  while (static_cast<uint32_t>(millis() - pauseStartedAtMs) <
         BUZZER_RECOVERY_PULSE_GAP_MS) {
    serviceBootRecoverySafety();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  // If the latch could not be dropped, continue this boot instead of
  // restarting into the same pending operation forever.
  if (!bootRecoveryShouldRestartAfterSuccess()) {
    return;
  }
  recordResetUptime(millis(), true);
  ESP.restart();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void resumePendingBootRecovery() {
  RecoveryIntent intent;
  const PendingRecoveryKind kind = inspectPendingRecovery(intent);
  if (kind == PendingRecoveryKind::VALID) {
    completeBootRecovery(static_cast<RecoveryOperation>(intent.operation));
    return;
  }
  if (kind == PendingRecoveryKind::MALFORMED) {
    abandonMalformedRecoveryIntent();
  }
}

void maybeRunBootRecoveryGesture() {
  const bool powerOnReset = currentSafetyResetIsPowerOn();
  const bool switchStablyOn = powerOnReset && machineBootActivatorHeldStably();
  if (!recoveryGestureEntryAllowed(powerOnReset, switchStablyOn)) {
    return;
  }

  // The activator held at power-on is not by itself a recovery intent: it
  // must stay held through a 30 s grace, announced by the paddle-return
  // double beep, before the irreversible gesture window opens. Releasing it
  // cancels the entry and normal startup continues.
  RecoveryGraceWindow grace;
  grace.begin(millis());
  for (;;) {
    machineSampleInput();
    const MachineIntention graceIntent = machinePollIntention();
    const RecoveryGraceResult graceResult =
        grace.update(millis(), graceIntent.holdActive);
    if (graceResult == RecoveryGraceResult::ABORTED) {
      return;
    }
    if (graceResult == RecoveryGraceResult::BEEP_DUE) {
      (void)playRecoveryCue(BuzzerCue::PADDLE_REMINDER);
    }
    if (graceResult == RecoveryGraceResult::ELAPSED) {
      break;
    }
    serviceBootRecoverySafety();
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  RecoveryGestureRecognizer recognizer;
  recognizer.begin(millis());
  (void)playRecoveryCue(BuzzerCue::RECOVERY_START);

  for (;;) {
    machineSampleInput();
    const MachineIntention intent = machinePollIntention();
    const RecoveryGestureResult result = recognizer.update(
        millis(), intent.holdActive, intent.turnedOn, intent.turnedOff);
    if (result == RecoveryGestureResult::NETWORK_ACCESS_RESET) {
      completeBootRecovery(RecoveryOperation::NETWORK_ACCESS_RESET);
      return;
    }
    if (result == RecoveryGestureResult::FACTORY_RESET) {
      completeBootRecovery(RecoveryOperation::FACTORY_RESET);
      return;
    }
    if (result == RecoveryGestureResult::TIMED_OUT) {
      (void)playRecoveryCue(BuzzerCue::RECOVERY_START);
      waitForRecoveryBuzzer();
      return;
    }
    serviceBootRecoverySafety();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
#endif

// ---------------------------------------------------------------------------

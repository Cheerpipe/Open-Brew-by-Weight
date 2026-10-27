// State machine
// ---------------------------------------------------------------------------

void resetSessionForNewCycle(ControlSource source) {
  backflushStopReason = "none";
  bullseyeTracker.clear();
  session = CycleSession{};
  session.id = nextCycleId++;
  setScaleCommandCycle(session.id);
  if (nextCycleId == 0) {
    nextCycleId = 1;
  }
  session.active = true;
  session.source = source;
  noScaleGuardHistoryPending = false;
  serviceIdleTare();
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Publish the RF gate before any deferred NTP request or webhook can wake a
  // network worker. This is an atomic store/notify only; control never waits.
  networkManager.syncControlCriticalRf(true);
#endif
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Host tests mutate RuntimeConfig brew fields directly; keep the active
  // preset (compose source of truth) aligned before snapshotting a cycle.
  ensureShotPresetBank(presetBank, runtimeConfig.retareWindowMs,
                       runtimeConfig.autoRetare);
  {
    ShotPreset &preset = mutableActiveShotPreset(presetBank);
    copyUserRecipeFromConfig(runtimeConfig, preset);
    preset.bbwEwmaOffsetG = runtimeConfig.weightOffsetG;
  }
#endif
  session.config = snapshotConfig(effectiveRuntimeConfig());
  session.activePresetId = presetBank.activeId;
  const ShotPreset &preset = activeShotPreset(presetBank);
  copyCString(session.activePresetName, sizeof(session.activePresetName), preset.name);
  session.activePresetDoseDg = preset.doseDg;
  session.config.bbwAlpha = preset.bbwEwmaAlpha;
  session.config.bbwLearningGeneration = bbwLearningBank.forPreset(
      preset.id, presetBank).generation;
  armSmoothCut(getScaleLinkSnapshot().features.sensorLagMs);
  predictor::armShotPredictor(session.predictor);
  {
    // Recent arrivals of this preset, for the scale-loss history regime and
    // the shot predictor's reference plateau flow.
    TaskLockGuard lock(shotStoreMutex);
    session.cut.history = shotLog.arrivalStats(preset);
    session.predictorQpX100 = shotLog.plateauFlowX100(preset);
  }
  session.endReason = EndReason::NONE;
}

void maybeRequestNtpSyncOnActivity();
bool beginRinseCycle(ControlSource source);
bool enterRinse();

// Completes the opening of a shot trace once the machine started; the
// snapshot holds only what the shot log summary lacks (docs/SHOT_TRACE.md).
void attachShotTraceStart(const ScaleLinkSnapshot &link) {
  const auto cg = [](float grams) {
    return std::isfinite(grams) ? weightToCentigrams(grams) : INT32_MIN;
  };
  ShotTraceSnapshot snapshot = {};
  snapshot.freeHeapBytes = freeHeapBytes;
  snapshot.freePsramBytes = psramFreeBytes;
  snapshot.postTareGraceMs = session.config.postTareBaselineGraceMs;
  snapshot.retareWindowMs = session.config.retareWindowMs;
  snapshot.stabilityDurationMs = runtimeConfig.retareStabilityMinDurationMs;
  snapshot.stabilityGapMs = runtimeConfig.retareStabilityMaxGapMs;
  snapshot.stabilitySamples = runtimeConfig.retareStabilitySamples;
  snapshot.minimumCupCg = cg(runtimeConfig.minimumCupWeightG);
  snapshot.cupRemovedCg = cg(runtimeConfig.cupRemovedWeightG);
  snapshot.stabilityToleranceCg = cg(runtimeConfig.retareStabilityToleranceG);
  snapshot.emptyReferenceCg = cg(cupPresence.emptyAnchorG);
  snapshot.occupiedReferenceCg = cupPresence.state == CupPresenceState::PRESENT
                                     ? cg(cupPresence.occupiedReferenceG) : INT32_MIN;
  snapshot.preTareWeightCg = cg(currentWeight);
  snapshot.paddleMode = runtimeConfig.paddleMode;
  snapshot.noScaleBbwMode = runtimeConfig.noScaleBbwMode;
  snapshot.idleEligibility = static_cast<uint8_t>(idleTare.eligibilityReason);
  snapshot.idleLastReason = static_cast<uint8_t>(idleTare.lastReason);
  const std::pair<bool, uint16_t> flags[] = {
      {session.config.autoTare, SHOT_TRACE_SNAP_AUTO_TARE},
      {session.config.autoRetare, SHOT_TRACE_SNAP_AUTO_RETARE},
      {runtimeConfig.autoTareOutsideBrew, SHOT_TRACE_SNAP_IDLE_TARE},
      {idleAccessoryRetareEnabled(runtimeConfig.noScaleBbwMode),
       SHOT_TRACE_SNAP_ACCESSORY_RETARE},
      {session.config.avoidAccidentalTouchEnabled, SHOT_TRACE_SNAP_TOUCH_AVOIDANCE},
      {session.config.requireCupToStart, SHOT_TRACE_SNAP_REQUIRE_CUP},
      {session.config.stopIfCupRemoved, SHOT_TRACE_SNAP_STOP_IF_REMOVED},
      {session.config.cupProtectionEnabled, SHOT_TRACE_SNAP_CUP_PROTECTION},
      {cupPresence.state == CupPresenceState::PRESENT, SHOT_TRACE_SNAP_CUP_PRESENT},
      {cupPresenceIsTared(), SHOT_TRACE_SNAP_CUP_TARED},
      {cupPresence.weight.emptyValid, SHOT_TRACE_SNAP_EMPTY_VALID},
      {idleTare.eligible, SHOT_TRACE_SNAP_IDLE_ELIGIBLE},
      {link.features.has(ScaleFeatureTare), SHOT_TRACE_SNAP_SCALE_TARE}};
  for (const auto &flag : flags) {
    if (flag.first) snapshot.flags |= flag.second;
  }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  const auto network = networkManager.snapshot();
  if (network.staLinkMetricsValid) {
    snapshot.wifiRssi = network.staRssi;
    snapshot.flags |= SHOT_TRACE_SNAP_RSSI_VALID;
  }
#endif
  copyCString(snapshot.scaleName, sizeof(snapshot.scaleName), link.shotScaleName);
  copyCString(snapshot.scaleProtocol, sizeof(snapshot.scaleProtocol), link.protocolName);
  shotTrace().attachStart(snapshot);
}

void beginCycle(ControlSource source = ControlSource::PHYSICAL) {
  if (backflushObservation.busy() || backflushActive(stopperState)) return;
  const ProfileControlObservation profile;
  refreshLoopGuardsFromLastIntention();
  if (source == ControlSource::PHYSICAL &&
      noScaleShotGuardNeedsFreshActivator) {
    if (!machineLastIntention().turnedOn) {
      return;
    }
    noScaleShotGuardNeedsFreshActivator = false;
  }
  maybeEmitManualNoScaleBeep(loopGuardInputs);
  if (noScaleShotGuardWouldBlock(loopGuardInputs)) {
    if (source == ControlSource::PHYSICAL) {
      noScaleShotGuardHold = true;
      noScaleShotGuardHoldAtMs = millis();
      noScaleGuardHistoryPending = true;
      noScaleShotGuardNeedsFreshActivator = true;
      if (!noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode) &&
          !machineBlockedStartNeedsHoldQualification()) {
        // WARN_ONCE is consumed by the first rejected start, including a
        // normal momentary tap. Keep the hold latched until release so the
        // same physical activation cannot close K1 after Armed clears.
        consumeNoScaleShotGuard();
        addDebugEvent(DebugCategory::STATE,
                      DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED);
      }
      return;
    }
    blockNoScaleShotGuard();
    return;
  }
  noScaleShotGuardHold = false;

  if (cupStartGuardWouldBlock(loopGuardInputs)) {
    if (source == ControlSource::PHYSICAL) {
      cupStartGuardHold = true;
      cupStartGuardHoldAtMs = millis();
      return;
    }
    cupStartGuardHold = false;
    addDebugEvent(DebugCategory::STATE, DebugCode::CUP_START_GUARD_BLOCKED,
                  weightToCentigrams(currentWeight));
    emitAlert(AlertEvent::CUP_START_BLOCKED);
    return;
  }
  cupStartGuardHold = false;

  if (idleTareBlocksStart(source == ControlSource::PHYSICAL)) return;

  flushPendingScaleTimerStopNow();
  cancelPendingFinalize("Previous drip analysis cancelled by a new cycle", 2);

  resetSessionForNewCycle(source);
  session.startedAtMs = millis();
  session.circuitClosedAtMs = 0;
  session.firstDropMs = 0;
  session.retareFlowFirstDetectedAtMs = 0;
  session.scaleBaselineReady = false;
  session.scaleBaselineG = 0.0f;
  session.weightSequenceAtStart = currentWeightSequence;
  const ScaleLinkSnapshot scaleLinkAtStart = getScaleLinkSnapshot();
  session.startedWithScale =
      scaleLinkAvailable(scaleLinkAtStart) && currentWeightIsFresh();
  session.scaleDisconnectSequenceAtStart =
      scaleLinkAtStart.disconnectSequence;
  session.connectionGenerationAtStart =
      scaleLinkAtStart.connectionGeneration;
  session.ownedConnectionGeneration =
      scaleLinkAtStart.connectionGeneration;
  // The scale name is captured while the scale is connected; a mid-shot
  // disconnect must not blank the stored snapshot.
  if (session.startedWithScale)
    copyCString(session.scaleName, sizeof(session.scaleName),
                scaleLinkAtStart.shotScaleName);
  session.weightControlState =
      session.startedWithScale && !session.config.timerOnly
          ? WeightControlState::ACTIVE
          : WeightControlState::INACTIVE;
  session.automaticEnabled =
      session.weightControlState == WeightControlState::ACTIVE;
  session.calibrationEligible = session.automaticEnabled;
  if (session.startedWithScale) {
    resyncCupPresenceIfPanEmpty(currentWeight);
    if (!session.config.autoTare) {
      session.hasWeightAnchor = true;
      session.lastAcceptedWeightG = currentWeight;
      session.lastAcceptedWeightAtMs = currentWeightReceivedAtMs;
      session.lastAcceptedPacketSequence = currentWeightSequence;
      session.scaleBaselineG = currentWeight;
      session.scaleBaselineReady = true;
    }
    shotTrace().begin(session.startedAtMs);  // Before the relay closes.
  }
  resetShotTrajectory(session.startedAtMs);
  initializeBbwProtection();

  const bool automaticBbw =
      session.startedWithScale && !session.config.timerOnly;
  machineSetPreferBleAirtime(automaticBbw);
  machineBeginCycle(automaticBbw);
  const uint32_t closeLimitMs =
      automaticBbw ? machineCloseLimitMs(session.config.operationalWallMs)
                   : HARD_MAX_CIRCUIT_CLOSED_MS;
  outboundShotActive.store(true, std::memory_order_release);
  inhibitMachineIntegrationCloud();
  if (!servicePowerManagement() ||
      !machineRequestStart(closeLimitMs, source == ControlSource::WEB)) {
    shotTrace().discard(static_cast<uint8_t>(EndReason::RELAY_SAFETY_FAILURE), millis());
    setScaleCommandCycle(0);
    session.active = false;
    outboundShotActive.store(false, std::memory_order_release);
    session.endReason = EndReason::RELAY_SAFETY_FAILURE;
    machineSetPreferBleAirtime(false);
    machineEndCycle();
    transitionTo(StopperState::REQUIRES_OFF);
    return;
  }
  captureCycleCircuitStart();
  if (session.startedWithScale) attachShotTraceStart(scaleLinkAtStart);
  // Arm the tare baseline only for a started cycle: a refused start must not
  // leave the cup FSM held or believing an unsent tare happened.
  if (session.startedWithScale && session.config.autoTare) {
    session.preTareWeightG = currentWeight;
    orchestratePostTareBaselineArm();
    markTareZeroReady();
  }

  emitCircuitCycleAlert(session.startedWithScale && session.config.autoTare &&
                            session.config.canTareStartTimer
                        ? AlertEvent::TARE_START
                        : AlertEvent::START_TIMER,
                    true);
  if (session.startedWithScale) {
    if (!requestRemoteTimerStart()) {
      session.automaticEnabled = false;
      session.scaleWasLost = true;
      serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                          "Scale start command unavailable; cycle marked manual");
    }
  }

  enterBrewOrManualFromStart();

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  webhookBrewStartPending = true;
  // With real-time delivery selected, do not wait for the normal fresh-scale
  // sample window: the circuit edge itself is the requested start event.
  if (!networkManager.webhookConfig().deferDuringShot) {
    queueWebhookBrewStart();
  }
#endif

  addDebugEvent(DebugCategory::STATE, DebugCode::CYCLE_STARTED,
                weightToCentigrams(
                    goalWeightDgToGrams(session.config.goalWeightDg)),
                weightToCentigrams(session.config.weightOffsetG));
  scaleProfileNoteEvent(
      ScaleProfileEvent::SHOT_START, millis(),
      scaleConnectionGenerationNow(), 0, NAN, session.id,
      static_cast<uint32_t>(
          static_cast<int32_t>(weightToCentigrams(
              goalWeightDgToGrams(session.config.goalWeightDg)))));
  maybeRequestNtpSyncOnActivity();
}

void enqueueActivationHistoryWebhook(uint32_t id, const HistoryRecord &record);

void appendHistoryRecord(HistoryType type, uint32_t durationMs,
                         bool noWeight, const CycleWallTime *end) {
  HistoryRecord record = {};
  record.durationDs = static_cast<uint16_t>(
      durationMs / 100U > UINT16_MAX ? UINT16_MAX : durationMs / 100U);
  record.type = static_cast<uint8_t>(type);
  const CycleWallTime wall = end != nullptr ? *end : captureCycleWallTime();
  if (wall.available) {
    record.endedAtUnixSec = wall.utcSec;
    record.endedAtLocalSec =
        shotLogLocalSecFromUtc(wall.utcSec, wall.offsetMinutes);
    record.flags = HISTORY_FLAG_WALL_TIME;
  }
  if (noWeight) {
    record.flags |= HISTORY_FLAG_NO_WEIGHT;
  }
  // O(1) PSRAM append under the existing store mutex; the deferred flush in
  // ActivationStores::service() performs the flash write outside the loop.
  uint32_t activationId = 0;
  {
    TaskLockGuard lock(shotStoreMutex);
    if (!historyLog.append(record, false)) {
      addDebugEvent(DebugCategory::CONFIG, DebugCode::SHOT_LOG_PERSIST_FAILED,
                    static_cast<int32_t>(sizeof(HistoryStore)),
                    static_cast<int32_t>(historyLog.count()));
      return;
    }
    shotStoreDirtyGeneration.fetch_add(1, std::memory_order_relaxed);
    activationId = historyLog.nextRecordId() - 1U;
  }
  enqueueActivationHistoryWebhook(activationId, record);
}

// Every confirmed machine activation lands in the independent history log:
// its `shot` label describes intent from the protection window, regardless
// of whether a settled weight qualifies for the separate shot log.
void appendActivationHistory(EndReason reason, bool rinseEnd,
                             uint32_t durationMs,
                             const CycleWallTime &end = captureCycleWallTime()) {
  if (brewEndIsAbandonedStart(reason)) {
    return;
  }
  appendHistoryRecord(historyTypeFromCycle(
                          rinseEnd, durationMs,
                          session.config.bbwProtectionMs),
                      durationMs, !endedCycleWeightValid(), &end);
}

bool finalizeCycle(EndReason reason, StopperState nextState,
                   bool webStopActuation = false) {
  const ProfileControlObservation profile;
  setScaleCommandCycle(0);
  const uint32_t durationMs = endedCycleDurationMs();

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (brewEndIsAbandonedStart(reason) || endingRinseCycle(reason)) {
    webhookBrewStartPending = false;
  } else {
    queueWebhookBrewStart();
  }
#endif

  stopPulseTrains();
  cancelScaleBrewBeep(session.id);
  cancelScaleCompletionBeep();
  // GPIO first, then the machine circuit-open cue. BLE RF coexistence is
  // owned by the BLE worker (syncScaleRadioCoex).
  if (brewWeightCutSettlesMachineOff(reason)) {
    machineArmSettledWeightCutOff();
  } else {
    machineCancelSettledWeightCutOff();
  }
  const bool rinseEnd = endingRinseCycle(reason);
  bool machineStopAccepted = true;
  if (rinseEnd) {
    machineStopAccepted = machineEndRinse();
  } else if (webStopActuation) {
    machineStopAccepted = machineRequestWebStop();
  } else {
    machineStopAccepted = machineRequestStop();
  }
  shotTrace().note(ShotTraceKind::STOP_REQUESTED, millis(), static_cast<int16_t>(reason),
                   (webStopActuation ? SHOT_TRACE_STOP_WEB : 0) |
                       (machineStopAccepted ? 0 : SHOT_TRACE_STOP_REFUSED));
  const CycleWallTime cycleEndTime = captureCycleWallTime();
  rinseClear();
  session.rinseStartedAtMs = 0;
  session.endReason = reason;
  if (!brewEndIsAbandonedStart(reason)) {
    if (shotCompletionGetsLongBeep(reason)) {
      // Completion LONG replaces the stop-timer SINGLE so ends are one cue.
      requestCompletionAlert();
    } else {
      emitCircuitCycleAlert(AlertEvent::STOP_TIMER, true);
    }
  }
  if (reason == EndReason::AUTO_TO_MANUAL_GUARD &&
      runtimeConfig.buzzerAutoToManualGuardEndBeep) {
    emitAlert(AlertEvent::ATM_END);
  }
  scheduleScaleTimerStopAfterCycle(durationMs);

  // Bullseye celebrates an On recipe weight cut only.
  if (!rinseEnd && corridor::weightCutReason(static_cast<uint8_t>(reason)) &&
      corridor::arrivalVerdict(session.cut.params, static_cast<int32_t>(durationMs),
                               session.cut.rate.valid ? session.cut.rate.rateX100 : 0)
              .verdict == corridor::Verdict::ON_RECIPE &&
      bullseyeMelodyConfig.enabled && soundAlertsEnabled() &&
      currentAlertOutputChannel() == AlertOutputChannel::BUZZER_ONLY &&
      localBuzzer.ready) {
    bullseyeTracker.arm(session.config.goalWeightDg, millis(),
                         session.config.dripDelayMs, currentWeightSequence,
                         cupPresencePlacementId());
  } else {
    bullseyeTracker.clear();
  }

  schedulePendingShotFinalize(reason, durationMs, cycleEndTime);
  appendActivationHistory(reason, rinseEnd, durationMs, cycleEndTime);
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  {
    const WebhookConfig webhook = networkManager.webhookConfig();
    if (webhook.enabled && webhook.brewState &&
        !endingRinseCycle(reason) && !brewEndIsAbandonedStart(reason)) {
      WebhookEvent event = baseWebhookEvent(WebhookEventType::IDLE, session.id,
                                           cycleEndTime.endedAtMs);
      event.unixSec = cycleEndTime.available ? cycleEndTime.utcSec : 0;
      event.durationMs = durationMs;
      event.targetWeightG = goalWeightDgToGrams(session.config.goalWeightDg);
      event.presetId = session.activePresetId;
      snprintf(event.stopDetail, sizeof(event.stopDetail), "%s",
               shotLogStopDetailName(shotLogStopDetailFromEndReason(
                   reason, session.config.fastSideEnabled,
                   session.extractionExtended)));
      (void)networkManager.enqueueWebhook(event);
    }
  }
#endif
  // Home status must show the last shot immediately (during drip delay),
  // including empty / sub-1 g results. Rinses (including early abort) and
  // abandoned starts do not overwrite last shot.
  homeCycleResult = lastShotFromEndedCycle(reason, durationMs, &cycleEndTime);
  homeCycleResolvedAtMs = cycleEndTime.endedAtMs;
  if (!rinseEnd && !brewEndIsAbandonedStart(reason))
    persistLastShotSnapshot(homeCycleResult);

  lastCycle.valid = true;
  lastCycle.cycleId = session.id;
  lastCycle.durationMs = durationMs;
  lastCycle.endedAtMs = cycleEndTime.endedAtMs;
  lastCycle.endReason = reason;
  lastCycle.source = session.source;
  lastCycle.weightValid = endedCycleWeightValid();
  lastCycle.lastWeightG = lastCycle.weightValid ? currentWeight : 0.0f;
  lastCycle.weightAgeAtEndMs =
      lastCycle.weightValid ? elapsedMs(currentWeightReceivedAtMs) : 0;
  lastCycle.weightControlState = session.weightControlState;
  lastCycle.calibrationEligible = session.calibrationEligible;
  if (!rinseEnd && !brewEndIsAbandonedStart(reason)) {
    noScaleShotGuardActivityAtMs = millis();
  }
  session.active = false;
  outboundShotActive.store(false, std::memory_order_release);
  session.awaitingPostTareBaseline = false;
  holdCupPresenceTransitions(false);
  virtualHoldOn = false;
  machineSetPreferBleAirtime(false);
  machineEndCycle();
  addDebugEvent(DebugCategory::STATE, DebugCode::CYCLE_ENDED,
                static_cast<int32_t>(reason));
  scaleProfileNoteEvent(ScaleProfileEvent::SHOT_END, millis(),
                        scaleConnectionGenerationNow(), 0, NAN,
                        session.id, static_cast<uint32_t>(reason));
  transitionTo(nextState);
  serviceIdleTare();
  return machineStopAccepted;
}

bool noScaleArmedRinseIsBlocked(ControlSource source) {
  if (noScaleAllowRinseWhileArmed(runtimeConfig.noScaleBbwMode) ||
      noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode)) {
    return false;
  }
  if (noScaleShotGuardWouldBlock(loopGuardInputs)) {
    return true;
  }
  // Momentary press-mode consumes Armed on REQUEST_START, then emits
  // REQUEST_RINSE while the blocked hold is still latched.
  return source == ControlSource::PHYSICAL && noScaleShotGuardHold;
}

bool beginRinseCycle(ControlSource source) {
  if (backflushObservation.busy() || backflushActive(stopperState)) return false;
  const ProfileControlObservation profile;
  if (idleTareBlocksStart(source == ControlSource::PHYSICAL)) return false;
  refreshLoopGuardsFromLastIntention();
  if (source == ControlSource::PHYSICAL && !noScaleGuardHistoryPending &&
      (noScaleShotGuardWouldBlock(loopGuardInputs) || noScaleShotGuardHold)) {
    noScaleGuardHistoryPending = true;
    noScaleShotGuardHoldAtMs = millis();
  }
  if (noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode) &&
      noScaleShotGuardWouldBlock(loopGuardInputs)) {
    if (source == ControlSource::WEB) {
      maybeEmitManualNoScaleBeep(loopGuardInputs);
    }
    addDebugEvent(DebugCategory::STATE, DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED);
    return false;
  }
  if (noScaleArmedRinseIsBlocked(source)) {
    // Web rinse has no prior physical cue. Momentary release-mode's first
    // stopper event is REQUEST_RINSE while still held and still Armed.
    // Paddle/press already sounded at ON/START; do not retrigger.
    if (source == ControlSource::WEB ||
        (noScaleShotGuardArmed && loopGuardInputs.holdActive)) {
      maybeEmitManualNoScaleBeep(loopGuardInputs);
    }
    if (noScaleShotGuardArmed) {
      consumeNoScaleShotGuard();
    }
    if (source == ControlSource::PHYSICAL && loopGuardInputs.holdActive) {
      noScaleShotGuardHold = true;
      noScaleShotGuardHoldAtMs = millis();
    }
    return false;
  }
  noScaleShotGuardNeedsFreshActivator = false;
  flushPendingScaleTimerStopNow();
  cancelPendingFinalize(source == ControlSource::WEB
                            ? "Previous drip analysis cancelled by a web rinse"
                            : "Previous drip analysis cancelled by a rinse", 3);
  resetSessionForNewCycle(source);
  session.startedAtMs = millis();
  session.circuitClosedAtMs = 0;
  session.firstDropMs = 0;
  session.retareFlowFirstDetectedAtMs = 0;
  session.scaleBaselineReady = false;
  session.scaleBaselineG = 0.0f;
  session.weightSequenceAtStart = currentWeightSequence;
  session.startedWithScale = scaleAvailable();
  session.automaticEnabled = false;
  virtualHoldOn = false;
  resetShotTrajectory(session.startedAtMs);
  machineSetPreferBleAirtime(false);
  machineBeginCycle(false);
  // Fresh rinse: never inherit a run that predates this new session.
  if (!servicePowerManagement() ||
      !machineBeginRinse(session.config.operationalWallMs, false)) {
    setScaleCommandCycle(0);
    session.active = false;
    session.endReason = EndReason::RELAY_SAFETY_FAILURE;
    machineEndCycle();
    transitionTo(StopperState::REQUIRES_OFF);
    return false;
  }
  captureCycleCircuitStart();
  emitCircuitCycleAlert(AlertEvent::START_TIMER, true);
  // Only an Armed no-scale rinse clears the latch; keep Armed when the scale
  // is usable (e.g. web rinse with a connected scale).
  if (noScaleShotGuardArmed) {
    const ScaleLinkSnapshot scaleLink = getScaleLinkSnapshot();
    const bool scaleUsable =
        scaleLinkAvailable(scaleLink) && currentWeightIsFresh();
    if (!scaleUsable &&
        !noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode)) {
      consumeNoScaleShotGuard();
    }
  }
  if (session.startedWithScale) {
    if (!requestRemoteTimerStart()) {
      session.startedWithScale = false;
      session.automaticEnabled = false;
      session.scaleWasLost = true;
      serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                          "Scale start command unavailable; rinse marked manual");
    }
  }
  return true;
}

bool enterRinse() {
  // A rinse classified from the session's own running cycle keeps that run's
  // start: pre-classification time counts as rinse time, so the deadline and
  // the reported duration both anchor to the original activation. A run that
  // predates this session (web rinse over a running shot) does not. The
  // elapsed-based comparison is wrap-safe where an absolute timestamp
  // comparison would break when millis() wraps between the two starts.
  const uint32_t runningElapsedMs = machineElapsedMs();
  const uint32_t runStartedAtMs =
      runningElapsedMs != 0U ? millis() - runningElapsedMs : millis();
  const bool continueSessionRun =
      elapsedMs(session.startedAtMs) >= elapsedMs(runStartedAtMs);
  if (!machineBeginRinse(session.config.operationalWallMs,
                         continueSessionRun)) {
    // Do not machineEndRinse(): that would double-pulse on momentary.
    // BREW/MANUAL already get machineRequestStop + machineEndCycle here.
    if (stopperState == StopperState::BREW ||
        stopperState == StopperState::MANUAL_NO_SCALE) {
      finalizeCycle(EndReason::RELAY_SAFETY_FAILURE,
                    StopperState::REQUIRES_OFF);
    } else {
      setScaleCommandCycle(0);
      session.active = false;
      session.endReason = EndReason::RELAY_SAFETY_FAILURE;
      machineEndCycle();
      transitionTo(StopperState::REQUIRES_OFF);
    }
    return false;
  }
  const uint32_t rinseStartMs = continueSessionRun ? runStartedAtMs : millis();
  session.rinseStartedAtMs = rinseBegin(session.config.rinseDurationMs,
                                        rinseStartMs);
  session.automaticEnabled = false;
  shot.automaticBrew = false;
  session.calibrationEligible = false;
  session.autoToManualGuardArmed = false;
  session.autoToManualGuardEnforced = false;
  session.awaitingPostTareBaseline = false;
  holdCupPresenceTransitions(false);
  addDebugEvent(DebugCategory::STATE, DebugCode::RINSE_CLASSIFIED);
  // A rinse is never a shot; the next trace notes the discarded one.
  shotTrace().discard(static_cast<uint8_t>(EndReason::RINSE_COMPLETE), millis());
  transitionTo(StopperState::RINSE);
  outboundShotActive.store(false, std::memory_order_release);
  maybeRequestNtpSyncOnActivity();
  return true;
}

void enterBrewOrManualFromStart() {
  if (session.config.timerOnly && session.startedWithScale) {
    addDebugEvent(DebugCategory::STATE, DebugCode::TIMER_ONLY_BREW_STARTED);
    transitionTo(StopperState::BREW);
    return;
  }
  if (session.automaticEnabled) {
    shot.automaticBrew = true;
    armAutoToManualGuardForAutomaticBrew();
    addDebugEvent(DebugCategory::STATE, DebugCode::BREW_STARTED);
    transitionTo(StopperState::BREW);
    return;
  }
  session.automaticEnabled = false;
  session.retareEnded = true;
  session.bbwProtectionEnded = true;
  addDebugEvent(DebugCategory::STATE, DebugCode::MANUAL_CYCLE_STARTED);
  transitionTo(StopperState::MANUAL_NO_SCALE);
}

void handleGlobalLimitTrip() {
  const bool endedWakeGesture = machineWakePassthroughActive;
  const uint32_t wakeDurationMs = elapsedMs(machineWakeStartedAtMs);
  machineWakePassthroughActive = false;
  const bool wasAlreadyOpenedByTimer = consumeRelaySafetyTrip();
  if (wasAlreadyOpenedByTimer) {
    addDebugEvent(DebugCategory::RELAY, DebugCode::RELAY_OPENED);
  }
  if (!wasAlreadyOpenedByTimer && relayCircuitClosed()) {
    machineRequestStop();
  }

  if (!session.active) {
    if (endedWakeGesture) {
      appendHistoryRecord(HistoryType::POWER_ON, wakeDurationMs, true);
    }
    transitionTo(StopperState::REQUIRES_OFF);
    return;
  }

  finalizeCycle(EndReason::GLOBAL_LIMIT, StopperState::REQUIRES_OFF);
}

void handleOperationalLimitTrip() {
  machineWakePassthroughActive = false;
  const bool wasAlreadyOpenedByTimer = consumeOperationalLimitTrip();
  if (wasAlreadyOpenedByTimer) {
    addDebugEvent(DebugCategory::RELAY, DebugCode::RELAY_OPENED);
  }
  addDebugEvent(DebugCategory::SECURITY, DebugCode::OPERATIONAL_LIMIT);
  if (!session.active) {
    transitionTo(StopperState::REQUIRES_OFF);
    return;
  }
  finalizeCycle(EndReason::CONFIGURED_WALL_LIMIT,
                StopperState::REQUIRES_OFF);
}

// Compatibility mode: the stopper degrades to a transparent activator mirror
// with no brew intelligence or duration cutoff. Safety stays authoritative:
// maintenance leases were serviced above and every close still passes relay
// supervision. A REQUIRES_OFF park (safety trip or failed close) holds until the
// activator returns idle: the mirror must not re-close on its own.
void serviceCompatibilityMirror(const MachineIntention &intent) {
  if (stopperState == StopperState::REQUIRES_OFF) {
    if (intent.intent == UserIntent::STABLE_IDLE || intent.stablyOff) {
      transitionTo(StopperState::READY);
    }
    return;
  }
  machineSetActivatorDriveAllowed(true);
  machineAllowCurrentActivatorHold();
  if (!servicePowerManagement() ||
      !machineServiceCompatibilityDrive(intent)) {
    transitionTo(StopperState::REQUIRES_OFF);
  }
}

void stateMachineTask() {
  const ProfileControlObservation profile;
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  if (relay.tripped ||
      (relay.closed && relay.hardLimitMs != 0 &&
       elapsedMs(relay.closedAtMs) >= relay.hardLimitMs)) {
    addDebugEvent(DebugCategory::SECURITY, DebugCode::HARD_LIMIT);
    handleGlobalLimitTrip();
    return;
  }
  if (relay.operationalTripped ||
      (relay.closed && relay.operationalLimitMs < relay.hardLimitMs &&
       elapsedMs(relay.closedAtMs) >= relay.operationalLimitMs)) {
    handleOperationalLimitTrip();
    return;
  }
  // A fault lockout opens K1 without the limit flags; still end the session.
  if (session.active && relay.state == RelaySafetyState::LOCKOUT) {
    finalizeCycle(EndReason::RELAY_SAFETY_FAILURE, StopperState::REQUIRES_OFF);
    return;
  }
  if (relay.closed && session.active &&
      machineCloseLimitMs(session.config.operationalWallMs) ==
          HARD_MAX_CIRCUIT_CLOSED_MS &&
      machineAllowsAutomationStop() &&
      elapsedMs(relay.closedAtMs) >= session.config.operationalWallMs) {
    handleOperationalLimitTrip();
    return;
  }

  if (machineWakePassthroughActive || machineWakeGestureConsumedThisLoop) {
    return;
  }

  const MachineIntention intent = machineLastIntention();

  if (stopperState == StopperState::READY && relay.closed &&
      intent.holdActive && idleTare.requestId != 0) {
    (void)idleTareBlocksStart(true);
  }

  // The OFF edge that completes the emergency gesture is confirmation only.
  // This also covers momentary builds configured to start on button release.
  if (noScaleRequireBypassCompletedThisLoop) {
    noScaleRequireBypassCompletedThisLoop = false;
    if (noScaleGuardHistoryPending && !intent.holdActive) {
      appendHistoryRecord(HistoryType::NO_SCALE_GUARD_ABORTED,
                          elapsedMs(noScaleShotGuardHoldAtMs), true);
      noScaleGuardHistoryPending = false;
    }
    return;
  }

  if (maintenanceLease.active) {
    if (relay.closed) {
      machineRequestStop();
    }
    if (intent.holdActive) {
      if (!maintenanceLease.forwarded) {
        if (maintenanceLease.command.type == WebCommandType::RESTART) {
          // Shot wins: keep the planned restart and let the paddle proceed.
          // Do not report CANCELED — the reboot still happens after idle. A
          // scheduled restart goes back to its planner (quiet time, window).
          if (scheduledRestartRequestId != 0 &&
              maintenanceLease.command.requestId == scheduledRestartRequestId) {
            scheduledRestartPlanner.aborted(millis());
          } else {
            pendingPlannedRestart = maintenanceLease.command;
            plannedRestartHeld = true;
          }
          maintenanceLease = MaintenanceLease{};
          outboundMaintenance.store(false, std::memory_order_release);
        } else {
          addDebugEvent(DebugCategory::SECURITY,
                        DebugCode::MAINTENANCE_CANCELED,
                        static_cast<int32_t>(maintenanceLease.id));
          maintenanceCancellationCommand = WebCommand{};
          maintenanceCancellationCommand.type =
              WebCommandType::MAINTENANCE_COMPLETE;
          maintenanceCancellationCommand.requestId =
              maintenanceLease.command.requestId;
          maintenanceCancellationCommand.succeeded = false;
          maintenanceCancellationCommand.resultState =
              CommandResultState::CANCELED;
          maintenanceCancellationPending = true;
          maintenanceLease = MaintenanceLease{};
          outboundMaintenance.store(false, std::memory_order_release);
        }
      }
      transitionTo(StopperState::REQUIRES_OFF);
    }
    return;
  }

  if (firmwareCompatibilityMode()) {
    serviceCompatibilityMirror(intent);
    return;
  }

  if (session.active && session.source == ControlSource::WEB &&
      (intent.turnedOn || intent.turnedOff)) {
    finalizeCycle(EndReason::PHYSICAL_OVERRIDE, nextStateForUserHold(intent));
    return;
  }

  switch (stopperState) {
    case StopperState::BACKFLUSH_CANDIDATE:
    case StopperState::BACKFLUSH_RUNNING:
      return;  // Serviced before brew guards and scale effects.
    case StopperState::REQUIRES_OFF:
      if (intent.intent == UserIntent::STABLE_IDLE || intent.stablyOff) {
        transitionTo(StopperState::READY);
      }
      return;

    case StopperState::READY:
      if (intent.intent == UserIntent::REQUEST_START) {
        beginCycle(ControlSource::PHYSICAL);
      }
      if (intent.intent == UserIntent::REQUEST_RINSE && machineSupportsRinse() &&
          !(noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode) &&
            noScaleShotGuardNeedsFreshActivator) &&
          beginRinseCycle(ControlSource::PHYSICAL)) {
        noScaleShotGuardHold = false;
        cupStartGuardHold = false;
        (void)enterRinse();
      }
      if (noScaleGuardHistoryPending && !intent.holdActive && !session.active) {
        appendHistoryRecord(HistoryType::NO_SCALE_GUARD_ABORTED,
                            elapsedMs(noScaleShotGuardHoldAtMs), true);
        noScaleGuardHistoryPending = false;
      }
      return;

    case StopperState::RINSE:
      // Physical edges are consumed by the machine while rinsing.
      if (rinseDeadlineReached()) {
        finalizeCycle(EndReason::RINSE_COMPLETE, nextStateForUserHold(intent));
      }
      return;

    case StopperState::BREW:
      if (intent.intent == UserIntent::REQUEST_RINSE && machineSupportsRinse()) {
        (void)enterRinse();
        return;
      }
      if (intent.intent == UserIntent::REQUEST_STOP) {
        if (session.automaticEnabled) {
          shot.automaticBrew = true;
        }
        finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
        if (!session.active || stopperState != StopperState::BREW) {
          return;
        }
      }

      expirePostTareBaseline();
      serviceBbwProtectionPhases();

      if (session.cupRemovedPending) {
        finalizeCycle(EndReason::CUP_REMOVED, nextStateForUserHold(intent));
        return;
      }
      if (machineTakeNoFlowIdle()) {
        finalizeCycle(EndReason::UNCONFIRMED_START,
                      nextStateForUserHold(intent));
        return;
      }

      if ((session.weightControlState == WeightControlState::ACTIVE ||
           session.weightControlState == WeightControlState::VALIDATING) &&
          scaleAutomationUnavailableForSession()) {
        suspendWeightControl();
        weightStreamState = WeightStreamState::STALE;
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_STREAM_STALE);
        serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                            "Scale stream suspended during brew");
      }

      // Paddle original mode holds automation stops while the paddle is on.
      if (machineAllowsAutomationStop() == ((session.traceLatches & TRACE_LATCH_PADDLE) != 0)) {
        session.traceLatches ^= TRACE_LATCH_PADDLE;
        shotTrace().note(ShotTraceKind::STOP_BLOCKED, millis(),
                         (session.traceLatches & TRACE_LATCH_PADDLE) != 0,
                         SHOT_TRACE_BLOCKED_PADDLE |
                             (session.directStopPending ? SHOT_TRACE_BLOCKED_PENDING : 0));
      }
      if (machineAllowsAutomationStop()) {
        const StopperState afterAutomation = nextStateForUserHold(intent);
        if (autoToManualGuardDeadlineDue()) {
          addDebugEvent(DebugCategory::SCALE,
                        DebugCode::AUTO_TO_MANUAL_GUARD_FIRED,
                        static_cast<int32_t>(elapsedMs(session.startedAtMs)));
          finalizeCycle(EndReason::AUTO_TO_MANUAL_GUARD, afterAutomation);
          return;
        }

        if (automaticScaleStopDue()) {
          const EndReason reason = session.directStopPending
                                       ? session.directStopReason
                                       : EndReason::SCALE_THRESHOLD;
          finalizeCycle(reason, afterAutomation);
        }
      }
      return;

    case StopperState::MANUAL_NO_SCALE:
      if (intent.intent == UserIntent::REQUEST_RINSE && machineSupportsRinse()) {
        (void)enterRinse();
        return;
      }
      if (intent.intent == UserIntent::REQUEST_STOP) {
        if (session.automaticEnabled) {
          shot.automaticBrew = true;
        }
        finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
        return;
      }
      if (machineTakeNoFlowIdle()) {
        finalizeCycle(EndReason::UNCONFIRMED_START,
                      nextStateForUserHold(intent));
        return;
      }
      return;
  }
}

// ---------------------------------------------------------------------------

// Shot finalization pipeline: late-tare capture, pending-shot scheduling,
// weight tails, log commit, outcome webhooks, and the finalize task. Split
// from OpenBrewByWeightCycleRuntime.inc at its original position; included
// directly after it so the translation-unit order is unchanged.

bool endingRinseCycle(EndReason reason) {
  return stopperState == StopperState::RINSE ||
         reason == EndReason::RINSE_COMPLETE;
}

bool pendingFinalizeIsRinse(const PendingShotFinalize &snapshot) {
  return snapshot.endReason == EndReason::RINSE_COMPLETE ||
         snapshot.finalState == StopperState::RINSE;
}

void captureLateTareCompletion(uint32_t throughMs = millis()) {
  if (!session.retarePerformed) return;
  const ScaleEvent result = copyScaleShotTareResult();
  if (!result.writeSucceeded || result.cycleId != session.id ||
      result.cupWeightRequestId != session.retareRequestId) return;
  const uint32_t anchor = session.circuitClosedAtMs != 0U
                              ? session.circuitClosedAtMs : session.startedAtMs;
  if (static_cast<int32_t>(result.receivedAtMs - anchor) < 0 ||
      static_cast<int32_t>(result.receivedAtMs - throughMs) > 0) return;
  if (!session.active &&
      (!pendingFinalize.pending || pendingFinalize.cycleId != session.id ||
       static_cast<int32_t>(result.receivedAtMs - pendingFinalize.endedAtMs) > 0)) return;
  applyShotTareResult(result);
  if (session.retareAtMs != 0) return;
  session.retareAtMs = result.receivedAtMs;
  if (pendingFinalize.pending && pendingFinalize.cycleId == session.id) {
    pendingFinalize.tareAtDs = static_cast<uint16_t>((result.receivedAtMs - anchor) / 100U);
    if (homeCycleResult.cycleId == session.id) homeCycleResult.tareElapsedMs = result.receivedAtMs - anchor;
    PersistedLastShot last;
    {
      const TaskLockGuard lock(shotStoreMutex);
      last = persistedLastShot;
    }
    if (last.valid && last.cycleId == session.id) {
      last.tareElapsedMs = result.receivedAtMs - anchor;
      persistLastShotSnapshot(last);
    }
  }
}

// Field diagnostics: a brew cycle started with a scale that does not reach
// shot history says why. Manual cycles without a scale never have a yield.
void logShotNotStoredShort(EndReason reason, uint32_t durationMs) {
  if (!session.startedWithScale) return;
  serialTraceCategoryf(
      LogLevel::INFO, DebugCategory::STATE, "Shot not stored: short (%.2fs, stop %s)",
      static_cast<double>(durationMs) / 1000.0,
      shotLogStopDetailName(shotLogStopDetailFromEndReason(
          reason, session.config.fastSideEnabled, session.extractionExtended)));
}

void logShotNotStoredYield(const PendingShotFinalize &snapshot, float weightG,
                           bool valid, ActualWeightSource source) {
  if (!snapshot.startedWithScale) return;
  const bool measured = valid && std::isfinite(weightG);
  serialTraceCategoryf(
      LogLevel::INFO, DebugCategory::STATE,
      "Shot not stored: %s (%.1fs, yield %.1fg %s, stop %s, zero %.1fg)",
      measured ? "low_yield" : "no_yield",
      static_cast<double>(snapshot.durationDs) / 10.0,
      static_cast<double>(measured ? weightG : 0.0f),
      actualWeightSourceName(source),
      shotLogStopDetailName(shotLogStopDetailFromEndReason(
          snapshot.endReason, snapshot.extractionGuardEnabled,
          snapshot.extractionExtended)),
      static_cast<double>(snapshot.softwareZeroG));
}

void schedulePendingShotFinalize(EndReason reason, uint32_t durationMs,
                                 const CycleWallTime &end = captureCycleWallTime()) {
  captureLateTareCompletion(end.endedAtMs);
  if (session.hasWeightAnchor) {
    shotCurveSampler.captureEnd(end.endedAtMs, session.lastAcceptedWeightG);
  } else {
    shotCurveSampler.captureEnd(end.endedAtMs);
  }
  if (brewEndIsAbandonedStart(reason) || endingRinseCycle(reason)) {
    return;
  }
  sessionQuickSettings.shotEnded = sessionQuickSettings.mask != 0;
  const bool logEligible = shotLogEligible(reason, durationMs);
  if (!logEligible) logShotNotStoredShort(reason, durationMs);
  const bool offsetAnalysis = shot.automaticBrew && !session.config.timerOnly &&
                              session.calibrationEligible;
  bool webhookEligible = false;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const WebhookConfig webhook = networkManager.webhookConfig();
  webhookEligible = webhook.enabled && webhook.end;
#endif
  if (!logEligible && !offsetAnalysis && !webhookEligible) {
    return;
  }

  pendingFinalize = PendingShotFinalize{};
  pendingFinalize.pending = true;
  pendingFinalize.offsetAnalysis = offsetAnalysis;
  pendingFinalize.logEligible = logEligible;
  pendingFinalize.endedAtMs = end.endedAtMs;
  pendingFinalize.endedAtUtcSec = end.utcSec;
  pendingFinalize.endedAtOffsetMinutes = end.offsetMinutes;
  pendingFinalize.endedAtHasWallTime = end.available;
  pendingFinalize.dripDelayMs = session.config.dripDelayMs;
  pendingFinalize.endedWeightSequence = currentWeightSequence;
  pendingFinalize.curveLastSequence = currentWeightSequence;
  pendingFinalize.cycleStartedAtMs =
      session.circuitClosedAtMs != 0U ? session.circuitClosedAtMs : session.startedAtMs;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.durationDs =
      static_cast<uint16_t>(durationMs / 100U);
  const uint32_t shotAnchorMs = pendingFinalize.cycleStartedAtMs;
  if (session.firstDropMs != 0 &&
      static_cast<int32_t>(session.firstDropMs - shotAnchorMs) >= 0) {
    pendingFinalize.firstDropDs = static_cast<uint16_t>(
        (session.firstDropMs - shotAnchorMs) / 100U);
  }
  if (session.retareAtMs != 0 &&
      static_cast<int32_t>(session.retareAtMs - shotAnchorMs) >= 0) {
    pendingFinalize.tareAtDs = static_cast<uint16_t>(
        (session.retareAtMs - shotAnchorMs) / 100U);
  }
  pendingFinalize.goalWeightDg = session.config.goalWeightDg;
  pendingFinalize.weightOffsetG = session.config.weightOffsetG;
  pendingFinalize.bbwAlgorithm = session.config.bbwAlgorithm;
  pendingFinalize.bbwAlpha = session.config.bbwAlpha;
  pendingFinalize.bbwProfileVersion = session.config.bbwProfileVersion;
  pendingFinalize.bbwLearningGeneration = session.config.bbwLearningGeneration;
  pendingFinalize.scaleConnectionGeneration = session.connectionGenerationAtStart;
  pendingFinalize.scaleBaselineReady = session.scaleBaselineReady;
  pendingFinalize.scaleBaselineG =
      session.scaleBaselineReady ? session.scaleBaselineG : 0.0f;
  pendingFinalize.softwareZeroG = session.softwareZeroG;
  pendingFinalize.startedWithScale = session.startedWithScale;
  pendingFinalize.timerOnly = session.config.timerOnly;
  pendingFinalize.automaticBrew = shot.automaticBrew;
  pendingFinalize.finalState = stopperState;
  pendingFinalize.endReason = reason;
  pendingFinalize.extractionGuardEnabled =
      session.config.fastSideEnabled;
  pendingFinalize.extractionExtended = session.extractionExtended;
  pendingFinalize.slowExtractionGuardEnabled =
      session.config.slowSideEnabled;
  pendingFinalize.fastMarginDg = session.config.fastMarginDg;
  pendingFinalize.slowMarginDg = session.config.slowMarginDg;
  pendingFinalize.fastBeforeMs = session.config.fastBeforeMs;
  pendingFinalize.slowAfterMs = session.config.slowAfterMs;
  pendingFinalize.wallMs = session.config.operationalWallMs;
  pendingFinalize.fastRampMs = static_cast<uint32_t>(session.cut.params.fastRampMs);
  pendingFinalize.slowRampMs = static_cast<uint32_t>(session.cut.params.slowRampMs);
  pendingFinalize.fastCurve = session.cut.params.fastCurve;
  pendingFinalize.slowCurve = session.cut.params.slowCurve;
  pendingFinalize.rateAtStopX100 =
      session.cut.rate.valid ? session.cut.rate.rateX100 : 0;
  pendingFinalize.channeling = session.cut.rate.channeling;
  pendingFinalize.scaleLossSource = session.scaleLossSource;
  pendingFinalize.cutStopMs = session.cut.stopMs;
  pendingFinalize.cutLineDg = session.cut.stopMs > 0 ? session.cut.stopLineDg : 0;
  pendingFinalize.scaleLossBlind = session.scaleLossBlind;
  pendingFinalize.protectionMs = session.config.bbwProtectionMs;
  pendingFinalize.lastKnownWeightValid =
      session.hasWeightAnchor && std::isfinite(session.lastAcceptedWeightG);
  pendingFinalize.lastKnownWeightG = session.lastAcceptedWeightG;
  pendingFinalize.activePresetId = session.activePresetId != 0
                                       ? session.activePresetId
                                       : presetBank.activeId;
  copyCString(pendingFinalize.activePresetName,
              sizeof(pendingFinalize.activePresetName), session.activePresetName);
  pendingFinalize.targetTempDeciC = machineIntegrationShotTargetDeciC();
  pendingFinalize.activePresetDoseDg = session.activePresetDoseDg != 0
                                           ? session.activePresetDoseDg
                                           : activeShotPreset(presetBank).doseDg;
  pendingFinalize.cycleId = session.id;
  pendingFinalize.retarePerformed = session.retarePerformed;
  pendingFinalize.autoToManualGuardEnabled =
      session.config.scaleLossEnabled;
  pendingFinalize.autoToManualGuardEnforced = session.autoToManualGuardEnforced;
  pendingFinalize.autoToManualGuardArmed = session.autoToManualGuardArmed;
  if (session.autoToManualGuardEnforced)
    pendingFinalize.autoToManualGuardRemainingMs = scaleLossBackupRemainingMs();
  pendingFinalize.noScaleShotGuardEnabled =
      noScaleBbwEnabled(runtimeConfig.noScaleBbwMode);
  pendingFinalize.noScaleShotGuardArmed = noScaleShotGuardArmed;
  pendingFinalize.noScaleBbwMode = runtimeConfig.noScaleBbwMode;
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  copyCString(pendingFinalize.scaleProtocol,
              sizeof(pendingFinalize.scaleProtocol), link.protocolName);
  copyCString(pendingFinalize.scaleName, sizeof(pendingFinalize.scaleName),
              session.scaleName);
  shotCurveSampler.snapshot(pendingFinalize.curve);
}

// The cut parameters the shot ran with, as the kernel sees them.
corridor::Params pendingCutParams(const PendingShotFinalize &snapshot) {
  corridor::Params p;
  p.goalDg = snapshot.goalWeightDg;
  p.fastMarginDg = snapshot.fastMarginDg;
  p.slowMarginDg = snapshot.slowMarginDg;
  p.fastOn = snapshot.extractionGuardEnabled && !snapshot.channeling;
  p.slowOn = snapshot.slowExtractionGuardEnabled;
  p.delayMs = static_cast<int32_t>(snapshot.protectionMs);
  p.fastBeforeMs = static_cast<int32_t>(snapshot.fastBeforeMs);
  p.slowAfterMs = static_cast<int32_t>(snapshot.slowAfterMs);
  p.wallMs = static_cast<int32_t>(snapshot.wallMs);
  p.fastRampMs = static_cast<int32_t>(snapshot.fastRampMs);
  p.slowRampMs = static_cast<int32_t>(snapshot.slowRampMs);
  p.fastCurve = snapshot.fastCurve;
  p.slowCurve = snapshot.slowCurve;
  return p;
}

bool pendingShotScaleCut(const PendingShotFinalize &snapshot) {
  return shotLogBbwEligible(snapshot.startedWithScale, snapshot.timerOnly,
                            snapshot.automaticBrew);
}

// The cut moment and line: exact when a weight cut latched them, otherwise
// the end of the shot (time and scale-loss stops).
int32_t pendingCutMs(const PendingShotFinalize &snapshot) {
  return snapshot.cutLineDg > 0 ? snapshot.cutStopMs
                                : static_cast<int32_t>(snapshot.durationDs) * 100;
}

int32_t pendingCutLineDg(const PendingShotFinalize &snapshot) {
  return snapshot.cutLineDg > 0
      ? snapshot.cutLineDg
      : corridor::cutLineDg(pendingCutParams(snapshot), pendingCutMs(snapshot));
}

// Verdict of a weight cut by its arrival time (plan §4.6); every other end
// is not classified.
corridor::ArrivalVerdict pendingShotVerdict(const PendingShotFinalize &snapshot,
                                            int32_t rateX100) {
  if (!pendingShotScaleCut(snapshot) ||
      !brewWeightCutSettlesMachineOff(snapshot.endReason))
    return corridor::ArrivalVerdict{};
  return corridor::arrivalVerdict(pendingCutParams(snapshot), pendingCutMs(snapshot),
                                  rateX100);
}

void persistLastShotFromFinalize(const PendingShotFinalize &snapshot,
                                 float finalWeightG, bool finalWeightValid) {
  PersistedLastShot last = {};
  last.valid = true;
  last.cycleId = snapshot.cycleId;
  last.endedAtUptimeMs = snapshot.endedAtMs;
  last.presetId = snapshot.activePresetId;
  copyCString(last.presetName, sizeof(last.presetName), snapshot.activePresetName);
  last.durationMs = static_cast<uint32_t>(snapshot.durationDs) * 100U;
  last.endReason = snapshot.endReason;
  if (snapshot.endedAtHasWallTime) {
    last.endedAtUnixSec = snapshot.endedAtUtcSec;
    last.endedAtLocalSec =
        shotLogLocalSecFromUtc(snapshot.endedAtUtcSec,
                               snapshot.endedAtOffsetMinutes);
    last.hasWallTime = 1;
  }
  last.weightValid = finalWeightValid;
  last.currentWeightG = finalWeightValid ? finalWeightG : 0.0f;
  last.goalWeightDg = snapshot.goalWeightDg;
  last.extractionExtended =
      snapshot.endReason == EndReason::FAST_EXTRACTION_MAX_WEIGHT;
  last.slowExtractionExtended =
      snapshot.endReason == EndReason::SLOW_EXTRACTION_MIN_WEIGHT;
  last.activeStopWeightG = goalWeightDgToGrams(
      last.extractionExtended ? snapshot.goalWeightDg + snapshot.fastMarginDg
      : last.slowExtractionExtended ? snapshot.goalWeightDg - snapshot.slowMarginDg
                                    : snapshot.goalWeightDg);
  if (snapshot.firstDropDs != SHOT_LOG_METRIC_MISSING) {
    last.firstDropElapsedMs =
        static_cast<uint32_t>(snapshot.firstDropDs) * 100U;
  }
  if (snapshot.tareAtDs != SHOT_LOG_METRIC_MISSING) {
    last.tareElapsedMs = static_cast<uint32_t>(snapshot.tareAtDs) * 100U;
  }
  copyCString(last.scaleName, sizeof(last.scaleName), snapshot.scaleName);
  if (last.weightValid && last.firstDropElapsedMs != 0 &&
      last.durationMs > last.firstDropElapsedMs + 500U &&
      snapshot.scaleBaselineReady) {
    const float delta = last.currentWeightG - snapshot.scaleBaselineG;
    last.averageFlowGps =
        delta * 1000.0f /
        static_cast<float>(last.durationMs - last.firstDropElapsedMs);
    last.averageFlowValid =
        delta > 0.0f && std::isfinite(last.averageFlowGps);
  }
  last.retarePerformed = snapshot.retarePerformed;
  last.shotType = static_cast<uint8_t>(lastShotTypeFromCycle(
      snapshot.finalState, snapshot.startedWithScale, snapshot.timerOnly,
      snapshot.automaticBrew));
  last.scaleAvailable = snapshot.startedWithScale;
  last.fastExtractionGuardEnabled = snapshot.extractionGuardEnabled;
  last.slowExtractionGuardEnabled = snapshot.slowExtractionGuardEnabled;
  last.autoToManualGuardEnabled = snapshot.autoToManualGuardEnabled;
  last.autoToManualGuardEnforced = snapshot.autoToManualGuardEnforced;
  last.autoToManualGuardArmed = snapshot.autoToManualGuardArmed;
  last.autoToManualGuardRemainingMs = snapshot.autoToManualGuardRemainingMs;
  last.noScaleShotGuardEnabled = snapshot.noScaleShotGuardEnabled;
  last.noScaleShotGuardArmed = snapshot.noScaleShotGuardArmed;
  last.noScaleBbwMode = snapshot.noScaleBbwMode;
  copyCString(last.scaleProtocol, sizeof(last.scaleProtocol),
              snapshot.scaleProtocol);
  if (last.extractionExtended && last.durationMs < snapshot.fastBeforeMs)
    last.minBbwBrewTimeRemainingMs = snapshot.fastBeforeMs - last.durationMs;
  const corridor::ArrivalVerdict verdict =
      pendingShotVerdict(snapshot, snapshot.rateAtStopX100);
  last.verdict = static_cast<uint8_t>(verdict.verdict);
  last.arrivalMs = verdict.verdict == corridor::Verdict::UNCLASSIFIED
                       ? 0U : static_cast<uint32_t>(verdict.arrivalMs);
  persistLastShotSnapshot(last);
  if (homeCycleResult.cycleId == snapshot.cycleId) {
    last.durationMs = homeCycleResult.durationMs;
    last.firstDropElapsedMs = homeCycleResult.firstDropElapsedMs;
    last.tareElapsedMs = homeCycleResult.tareElapsedMs;
    homeCycleResult = last;
    homeCycleResolvedAtMs = millis();
  }
}

// Weight counts as registered for the ended cycle only when the scale
// delivered a finite reading after the cycle started. Shared by the persisted
// last shot, lastCycle, and the activation-history no-weight flag.
bool endedCycleWeightValid() {
  return currentWeightSequence != session.weightSequenceAtStart &&
         std::isfinite(currentWeight) &&
         static_cast<int32_t>(currentWeightReceivedAtMs - session.startedAtMs) >=
             0;
}

PersistedLastShot lastShotFromEndedCycle(EndReason reason, uint32_t durationMs,
                                   const CycleWallTime *end = nullptr) {
  const CycleWallTime wall = end != nullptr ? *end : captureCycleWallTime();
  PersistedLastShot last = {};
  last.valid = true;
  last.cycleId = session.id;
  last.endedAtUptimeMs = wall.endedAtMs;
  last.presetId = session.activePresetId;
  copyCString(last.presetName, sizeof(last.presetName), session.activePresetName);
  last.durationMs = durationMs;
  last.endReason = reason;
  if (wall.available) {
    last.endedAtUnixSec = wall.utcSec;
    last.endedAtLocalSec =
        shotLogLocalSecFromUtc(wall.utcSec, wall.offsetMinutes);
    last.hasWallTime = 1;
  }
  last.weightValid = endedCycleWeightValid();
  last.currentWeightG = last.weightValid ? currentWeight : 0.0f;
  const bool lastAcceptedValid =
      session.hasWeightAnchor && std::isfinite(session.lastAcceptedWeightG);
  if (lastAcceptedValid &&
      (!last.weightValid ||
       !plausibleSettledBrewWeight(last.currentWeightG,
                                   session.lastAcceptedWeightG, true))) {
    last.weightValid = true;
    last.currentWeightG = session.lastAcceptedWeightG;
  }
  last.goalWeightDg = session.config.goalWeightDg;
  last.extractionExtended =
      session.extractionExtended && session.config.fastSideEnabled;
  last.slowExtractionExtended =
      session.slowExtractionExtended &&
      session.config.slowSideEnabled;
  last.activeStopWeightG =
      last.extractionExtended
          ? goalWeightDgToGrams(session.config.goalWeightDg + session.config.fastMarginDg)
          : (last.slowExtractionExtended
                 ? goalWeightDgToGrams(session.config.goalWeightDg - session.config.slowMarginDg)
                 : goalWeightDgToGrams(session.config.goalWeightDg));
  const uint32_t startMs =
      session.circuitClosedAtMs != 0U ? session.circuitClosedAtMs : session.startedAtMs;
  if (session.firstDropMs != 0 &&
      static_cast<int32_t>(session.firstDropMs - startMs) >= 0) {
    last.firstDropElapsedMs = session.firstDropMs - startMs;
  }
  if (session.retareAtMs != 0 &&
      static_cast<int32_t>(session.retareAtMs - startMs) >= 0) {
    last.tareElapsedMs = session.retareAtMs - startMs;
  }
  last.retarePerformed = session.retarePerformed;
  last.shotType = static_cast<uint8_t>(lastShotTypeFromCycle(
      stopperState, session.startedWithScale, session.config.timerOnly,
      shot.automaticBrew));
  last.scaleAvailable = session.startedWithScale;
  last.fastExtractionGuardEnabled = session.config.fastSideEnabled;
  last.slowExtractionGuardEnabled = session.config.slowSideEnabled;
  last.autoToManualGuardEnabled = session.config.scaleLossEnabled;
  last.autoToManualGuardEnforced = session.autoToManualGuardEnforced;
  last.autoToManualGuardArmed = session.autoToManualGuardArmed;
  last.noScaleShotGuardEnabled = noScaleBbwEnabled(runtimeConfig.noScaleBbwMode);
  last.noScaleShotGuardArmed = noScaleShotGuardArmed;
  last.noScaleBbwMode = runtimeConfig.noScaleBbwMode;
  if (session.autoToManualGuardEnforced)
    last.autoToManualGuardRemainingMs = scaleLossBackupRemainingMs();
  if (last.extractionExtended && durationMs < session.config.fastBeforeMs)
    last.minBbwBrewTimeRemainingMs = session.config.fastBeforeMs - durationMs;
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  copyCString(last.scaleProtocol, sizeof(last.scaleProtocol),
              link.protocolName);
  copyCString(last.scaleName, sizeof(last.scaleName), session.scaleName);
  return last;
}

void commitPendingShotLog(const PendingShotFinalize &snapshot, float finalWeightG,
                          bool finalWeightValid,
                          ActualWeightSource weightSource) {
  TaskLockGuard lock(shotStoreMutex);
  ShotLogRecord record;
  memset(&record, 0, sizeof(record));
  record.actualWeightCg = SHOT_LOG_WEIGHT_MISSING;
  record.errorCg = SHOT_LOG_WEIGHT_MISSING;
  record.firstDropDs = snapshot.firstDropDs;
  record.tareAtDs = snapshot.tareAtDs;
  record.avgFlowCgS = SHOT_LOG_METRIC_MISSING;
  record.bootId = snapshot.bootId;
  record.endedAtMs = snapshot.endedAtMs;
  if (snapshot.endedAtHasWallTime) {
    record.endedAtUnixSec = snapshot.endedAtUtcSec;
    record.endedAtLocalSec = shotLogLocalSecFromUtc(
        snapshot.endedAtUtcSec, snapshot.endedAtOffsetMinutes);
    record.timezoneOffsetMinutesAtCommit = snapshot.endedAtOffsetMinutes;
    record.hasWallTime = 1;
  } else {
    record.endedAtUnixSec = 0;
    record.endedAtLocalSec = 0;
    record.timezoneOffsetMinutesAtCommit = 0;
    record.hasWallTime = 0;
  }
  // The persisted duration has decisecond precision; keep a just-over-12 s
  // shot distinguishable from an old row recorded at exactly 12.0 s.
  record.durationDs = snapshot.durationDs > 120U ? snapshot.durationDs : 121U;
  record.goalWeightDg = snapshot.goalWeightDg;
  record.offsetUsedCg = shotLogWeightToCentigrams(snapshot.weightOffsetG);
  record.shotType = static_cast<uint8_t>(shotLogTypeFromCycle(
      snapshot.finalState, snapshot.startedWithScale, snapshot.timerOnly,
      snapshot.automaticBrew));
  record.cutType = static_cast<uint8_t>(
      shotLogCutFromEndReason(snapshot.endReason));
  shotLogSetPresetId(record, snapshot.activePresetId);
  copyCString(record.presetName, sizeof(record.presetName),
              snapshot.activePresetName);
  copyCString(record.scaleName, sizeof(record.scaleName), snapshot.scaleName);
  record.sideFlags = shotLogPackGuardFlags(
      snapshot.extractionGuardEnabled, snapshot.slowExtractionGuardEnabled);
  // Cut line above / below the target at the stop: the segment that fired.
  record.extractionExtended = shotLogPackExtendedFlags(
      snapshot.endReason == EndReason::FAST_EXTRACTION_MAX_WEIGHT,
      snapshot.endReason == EndReason::SLOW_EXTRACTION_MIN_WEIGHT);
  record.stopDetail = static_cast<uint8_t>(shotLogStopDetailFromEndReason(
      snapshot.endReason, snapshot.extractionGuardEnabled,
      snapshot.extractionExtended));
  record.cutLineCg = pendingShotScaleCut(snapshot)
      ? static_cast<int16_t>(pendingCutLineDg(snapshot) * 10)
      : SHOT_LOG_WEIGHT_MISSING;
  record.fastBeforeDs = static_cast<uint16_t>(snapshot.fastBeforeMs / 100U);
  record.slowAfterDs = static_cast<uint16_t>(snapshot.slowAfterMs / 100U);
  record.delayDs = static_cast<uint16_t>(snapshot.protectionMs / 100U);
  record.wallDs = static_cast<uint16_t>(snapshot.wallMs / 100U);
  record.fastMarginDg = static_cast<uint8_t>(
      snapshot.fastMarginDg > UINT8_MAX ? UINT8_MAX : snapshot.fastMarginDg);
  record.slowMarginDg = static_cast<uint8_t>(
      snapshot.slowMarginDg > UINT8_MAX ? UINT8_MAX : snapshot.slowMarginDg);
  record.cutRampDs = static_cast<uint8_t>(snapshot.fastRampMs / 100U);
  record.slowRampCode = static_cast<uint16_t>(snapshot.slowRampMs / 100U + 1U);
  record.fastCurve = snapshot.fastCurve;
  record.slowCurve = snapshot.slowCurve;
  record.dripDelayCode = static_cast<uint8_t>(snapshot.dripDelayMs / 100U + 1U);
  record.targetTempDeciC = snapshot.targetTempDeciC;
  record.doseDg = snapshot.activePresetDoseDg;
  record.arrivalDs = SHOT_LOG_METRIC_MISSING;
  record.verdict = static_cast<uint8_t>(corridor::Verdict::UNCLASSIFIED);
  record.actualWeightSource = static_cast<uint8_t>(weightSource);
  shotLogSetBbw(record, snapshot.bbwAlgorithm, snapshot.bbwProfileVersion,
                 snapshot.bbwAlpha, snapshot.bbwLearningApplied);

  if (finalWeightValid) {
    record.actualWeightCg = shotLogWeightToCentigrams(finalWeightG);
    if (finalWeightG > 2.0f && record.actualWeightCg <= 200)
      record.actualWeightCg = 201;
    // Error against the planned stop (the cut line), else the target.
    record.errorCg = shotLogWeightToCentigrams(
        finalWeightG - (shotLogWeightIsMissing(record.cutLineCg)
                            ? goalWeightDgToGrams(snapshot.goalWeightDg)
                            : record.cutLineCg / 100.0f));
    if (snapshot.firstDropDs != SHOT_LOG_METRIC_MISSING &&
        (weightSource == ActualWeightSource::POST_DRIP ||
         weightSource == ActualWeightSource::LAST_KNOWN)) {
      const float durationS = snapshot.durationDs / 10.0f;
      const float firstDropS = snapshot.firstDropDs / 10.0f;
      const float brewS = durationS - firstDropS;
      const float deltaG = finalWeightG - snapshot.scaleBaselineG;
      if (brewS > 0.5f && deltaG > 0.0f) {
        const float flowGs = deltaG / brewS;
        if (std::isfinite(flowGs) && flowGs >= 0.0f && flowGs <= 655.35f) {
          record.avgFlowCgS =
              static_cast<uint16_t>(flowGs * 100.0f + 0.5f);
        }
      }
    }
  }

  // Ratio snapshot: the recorded yield over the snapshotted dose. The
  // positive-weight guard keeps the uint16 cast from wrapping on a
  // non-positive recorded yield.
  if (record.doseDg >= MIN_DOSE_DG && record.actualWeightCg > 0 &&
      !shotLogWeightIsMissing(record.actualWeightCg)) {
    record.ratioX100 = static_cast<uint16_t>(
        static_cast<int32_t>(record.actualWeightCg) * 10 / record.doseDg);
  }

  const corridor::ArrivalVerdict verdict =
      pendingShotVerdict(snapshot, snapshot.rateAtStopX100);
  const bool scaleLoss = snapshot.endReason == EndReason::AUTO_TO_MANUAL_GUARD;
  record.verdict = corridor::packVerdict(
      verdict.verdict, snapshot.channeling,
      static_cast<corridor::LossSource>(scaleLoss ? snapshot.scaleLossSource : 0),
      scaleLoss && snapshot.scaleLossBlind);
  if (verdict.verdict != corridor::Verdict::UNCLASSIFIED)
    record.arrivalDs = static_cast<uint16_t>(verdict.arrivalMs / 100);
  const uint32_t newId = shotLog.nextRecordId();
  if (!shotLog.append(record, false)) {
    const size_t nextCount =
        shotLog.count() < SHOT_LOG_CAPACITY ? shotLog.count() + 1U
                                            : SHOT_LOG_CAPACITY;
    const size_t blobBytes =
        sizeof(ShotLogHeader) + nextCount * sizeof(ShotLogRecord);
    addDebugEvent(DebugCategory::CONFIG, DebugCode::SHOT_LOG_PERSIST_FAILED,
                  static_cast<int32_t>(blobBytes),
                  static_cast<int32_t>(shotLog.count()));
    return;
  }
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_relaxed);
  if (homeCycleResult.cycleId == snapshot.cycleId) {
    record.id = newId;
    homeCycleResult.shotLogId = newId;
    homeCycleResult = shotLogProjectLastShot(record, homeCycleResult);
  }
  if (persistedLastShot.valid && persistedLastShot.cycleId == snapshot.cycleId) {
    PersistedLastShot linked = persistedLastShot;
    linked.shotLogId = newId;
    lastShotStore.advance(linked, runtimeConfig.bbwProtectionMs);
    lastShotNvsDirty = true;
  }
  if (snapshot.curve.count > 0) {
    static OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotCurveRecord curve;
    copyShotCurve(curve, snapshot.curve);
    curve.shotId = newId;
    if (!shotCurves.append(curve, false)) {
      addDebugEvent(DebugCategory::CONFIG, DebugCode::SHOT_LOG_PERSIST_FAILED,
                    static_cast<int32_t>(sizeof(ShotCurveStore)),
                    static_cast<int32_t>(shotCurves.count()));
    }
  }
}

void queueWebhookEnd(const PendingShotFinalize &snapshot, float finalWeightG,
                     bool finalWeightValid) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const WebhookConfig config = networkManager.webhookConfig();
  if (!config.enabled || !config.end || pendingFinalizeIsRinse(snapshot) ||
      brewEndIsAbandonedStart(snapshot.endReason)) {
    return;
  }
#endif
  WebhookEvent event = baseWebhookEvent(WebhookEventType::END, snapshot.cycleId,
                                        snapshot.endedAtMs);
  event.unixSec = snapshot.endedAtHasWallTime ? snapshot.endedAtUtcSec : 0;
  event.durationMs = static_cast<uint32_t>(snapshot.durationDs) * 100U;
  event.firstDropValid = snapshot.firstDropDs != SHOT_LOG_METRIC_MISSING;
  event.firstDropMs = event.firstDropValid
                          ? static_cast<uint32_t>(snapshot.firstDropDs) * 100U
                          : 0U;
  event.weightValid = finalWeightValid && std::isfinite(finalWeightG);
  event.weightG = event.weightValid ? finalWeightG : 0.0f;
  event.targetWeightG = goalWeightDgToGrams(snapshot.goalWeightDg);
  event.presetId = snapshot.activePresetId;
  copyCString(event.presetName, sizeof(event.presetName),
              snapshot.activePresetName);
  const ShotLogType type = shotLogTypeFromCycle(
      snapshot.finalState, snapshot.startedWithScale, snapshot.timerOnly,
      snapshot.automaticBrew);
  snprintf(event.shotType, sizeof(event.shotType), "%s", shotLogTypeName(type));
  snprintf(event.stopDetail, sizeof(event.stopDetail), "%s",
           shotLogStopDetailName(shotLogStopDetailFromEndReason(
               snapshot.endReason, snapshot.extractionGuardEnabled,
               snapshot.extractionExtended)));
  if (event.weightValid && event.firstDropValid &&
      event.durationMs > event.firstDropMs + 500U) {
    const float brewSeconds =
        static_cast<float>(event.durationMs - event.firstDropMs) / 1000.0f;
    const float delta = finalWeightG - snapshot.scaleBaselineG;
    if (delta > 0.0f && std::isfinite(delta)) {
      event.averageFlowGps = delta / brewSeconds;
      event.averageFlowValid = std::isfinite(event.averageFlowGps);
    }
  }
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  (void)networkManager.enqueueWebhook(event);
#else
  hostWebhookEndEvent = event;
#endif
}

void queueWebhookPresetsChanged() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const WebhookConfig config = networkManager.webhookConfig();
  if (!config.enabled || !config.presetChanges) return;
  WebhookEvent event = baseWebhookEvent(WebhookEventType::PRESETS_CHANGED, 0);
  event.presetId = presetBank.activeId;
  event.presetRevision = runtimeConfig.revision;
  event.presetCount = presetBank.count < 8 ? presetBank.count : 8;
  for (uint8_t i = 0; i < event.presetCount; ++i) {
    event.presets[i].id = presetBank.presets[i].id;
    event.presets[i].isFactory = presetBank.presets[i].isFactory;
    copyCString(event.presets[i].name, sizeof(event.presets[i].name),
                presetBank.presets[i].name);
  }
  (void)networkManager.enqueueWebhook(event);
#else
  ++hostPresetWebhookCount;
#endif
}

void queueWebhookQuickSettingsChanged() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const WebhookConfig config = networkManager.webhookConfig();
  if (!config.enabled || !config.presetChanges) return;
  RecipeSnapshot recipe;
  recipe.presets = presetBank;
  recipe.runtime = runtimeConfig;
  WebhookEvent event =
      baseWebhookEvent(WebhookEventType::QUICK_SETTINGS_CHANGED, 0);
  event.quickSettings = quickSettingsSnapshot(recipe);
  (void)networkManager.enqueueWebhook(event);
#else
  ++hostQuickSettingsWebhookCount;
#endif
}

void cancelPendingFinalize(const char *reason, uint32_t profileReason) {
  const ProfileControlObservation profile;
  if (!pendingFinalize.pending) {
    return;
  }
  captureLateTareCompletion();
  scaleProfileNoteEvent(ScaleProfileEvent::FINALIZE_CANCELLED, millis(),
                        scaleConnectionGenerationNow(), 0, NAN,
                        pendingFinalize.cycleId, profileReason);
  static OPEN_BREW_BY_WEIGHT_PSRAM_BSS PendingShotFinalize snapshot;
  snapshot = pendingFinalize;
  pendingFinalize.pending = false;
  serialTrace(LogLevel::INFO, reason != nullptr
                                  ? reason
                                  : "Previous drip analysis cancelled");
  ActualWeightSource source = ActualWeightSource::NONE;
  float weightG = 0.0f;
  bool valid = false;
  if (snapshot.lastKnownWeightValid && std::isfinite(snapshot.lastKnownWeightG)) {
    source = ActualWeightSource::LAST_KNOWN;
    weightG = snapshot.lastKnownWeightG;
    valid = true;
  }
  const bool persistEligible =
      snapshot.logEligible && shotLogWeightEligible(weightG, valid);
  // Keep the internal completed-cycle snapshot for linked metadata.
  if (!pendingFinalizeIsRinse(snapshot) &&
      !brewEndIsAbandonedStart(snapshot.endReason)) {
    persistLastShotFromFinalize(snapshot, weightG, valid);
  }
  if (persistEligible && !pendingFinalizeIsRinse(snapshot))
    commitPendingShotLog(snapshot, weightG, valid, source);
  else if (snapshot.logEligible && !pendingFinalizeIsRinse(snapshot))
    logShotNotStoredYield(snapshot, weightG, valid, source);
  queueWebhookEnd(snapshot, weightG, valid);
}

#include "OpenBrewByWeightBbwFinalize.inc"

void pendingShotFinalizeTask() {
  if (!pendingFinalize.pending ||
      elapsedMs(pendingFinalize.endedAtMs) < pendingFinalize.dripDelayMs) {
    return;
  }

  captureLateTareCompletion();
  const ProfileControlObservation profile;
  static OPEN_BREW_BY_WEIGHT_PSRAM_BSS PendingShotFinalize snapshot;
  snapshot = pendingFinalize;
  pendingFinalize.pending = false;

  const float finalWeightG = postStopFinalWeightG(snapshot, currentWeight);
  bool postDripWeightValid = false;
  if (scaleAvailable() && std::isfinite(currentWeight) &&
      currentWeightSequence != snapshot.endedWeightSequence &&
      static_cast<int32_t>(currentWeightReceivedAtMs - snapshot.endedAtMs) >
          0 &&
      std::isfinite(finalWeightG) &&
      plausibleSettledBrewWeight(finalWeightG, snapshot.lastKnownWeightG,
                                 snapshot.lastKnownWeightValid)) {
    postDripWeightValid = snapshot.startedWithScale;
  }

  if (brewWeightCutSettlesMachineOff(snapshot.endReason)) {
    if (postDripWeightValid) {
      machineNoteSettledWeightCutOff();
    } else {
      machineCancelSettledWeightCutOff();
    }
  }

  ActualWeightSource weightSource = ActualWeightSource::NONE;
  bool logWeightValid = false;
  float logWeightG = 0.0f;
  if (postDripWeightValid) {
    weightSource = ActualWeightSource::POST_DRIP;
    logWeightValid = true;
    logWeightG = finalWeightG;
  } else if (snapshot.lastKnownWeightValid) {
    weightSource = ActualWeightSource::LAST_KNOWN;
    logWeightValid = true;
    logWeightG = snapshot.lastKnownWeightG;
  }

  // Keep the cutoff time and settled-yield annotation separate from observations.
  if (logWeightValid) {
    (void)settleShotCurveEndWeight(snapshot.curve, logWeightG);
    pendingFinalize.curve.ended = snapshot.curve.ended;
  }

  snapshot.bbwLearningApplied =
      learnPendingBbw(snapshot, finalWeightG, postDripWeightValid);

  const bool persistEligible =
      snapshot.logEligible && shotLogWeightEligible(logWeightG, logWeightValid);

  // Keep the internal completed-cycle snapshot for linked metadata.
  if (!pendingFinalizeIsRinse(snapshot) &&
      !brewEndIsAbandonedStart(snapshot.endReason)) {
    persistLastShotFromFinalize(snapshot, logWeightG, logWeightValid);
  }
  if (persistEligible && !pendingFinalizeIsRinse(snapshot))
    commitPendingShotLog(snapshot, logWeightG, logWeightValid, weightSource);
  else if (snapshot.logEligible && !pendingFinalizeIsRinse(snapshot))
    logShotNotStoredYield(snapshot, logWeightG, logWeightValid, weightSource);
  queueWebhookEnd(snapshot, logWeightG, logWeightValid);


}

void serviceBullseyeMelody() {
  if (!bullseyeTracker.pending) {
    return;
  }
  const uint32_t nowMs = millis();
  if (!bullseyeMelodyConfig.enabled || !soundAlertsEnabled() ||
      currentAlertOutputChannel() != AlertOutputChannel::BUZZER_ONLY ||
      !localBuzzer.ready || session.active ||
      cupPresenceState() != CupPresenceState::PRESENT ||
      cupPresencePlacementId() != bullseyeTracker.cupPlacementId) {
    bullseyeTracker.clear();
    return;
  }
  if (currentWeightSequence == bullseyeTracker.processedWeightSequence ||
      !scaleAvailable() || !currentWeightIsFresh(nowMs)) {
    return;
  }
  if (bullseyeTracker.accept(currentWeight, currentWeightReceivedAtMs,
                              currentWeightSequence,
                              MAX_AUTOMATION_WEIGHT_AGE_MS)) {
    (void)localBuzzer.requestBullseye();
    serialTracef(LogLevel::INFO,
                 "Bullseye: %.2fg stable at target for %lums",
                 static_cast<double>(currentWeight),
                 static_cast<unsigned long>(BULLSEYE_STABILITY_MS));
  }
}

// ---------------------------------------------------------------------------

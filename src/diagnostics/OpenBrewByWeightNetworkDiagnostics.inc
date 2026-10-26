bool debugExportChunk(httpd_req_t *request, const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return true;
  }
  return sendChunk(request, text, strlen(text)) == ESP_OK;
}
bool debugExportChunkf(httpd_req_t *request, char *buf, size_t cap,
                       const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

bool __attribute__((format(printf, 4, 5)))
debugExportChunkf(httpd_req_t *request, char *buf, size_t cap,
                  const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, cap, fmt, args);
  va_end(args);
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    return false;
  }
  return debugExportChunk(request, buf);
}

}  // namespace

esp_err_t OpenBrewByWeightNetwork::debugExportHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  // Public with the Diagnostic page (it carries no secrets).
  // DEBUG EXPORT MAINTENANCE: extend sections below when adding diagnostically
  // relevant settings / state. Bump DEBUG_EXPORT_SCHEMA_VERSION on material
  // schema changes. Never include secrets.
  if (!self.lockWorkBufForStatus()) {
    return self.workBufBusy(request);
  }
  NetworkWorkBuf &work = *self.workBuf_;
  self.loadControlStatus(work.control);
  if (self.callbacks_.copyDebugExportExtras != nullptr) {
    self.callbacks_.copyDebugExportExtras(work.debugExport, work.control);
  } else {
    work.debugExport = DebugExportExtras{};
  }
  const ControlStatusSnapshot &c = work.control;
  const DebugExportExtras &x = work.debugExport;
  const NetworkStatusSnapshot network = self.snapshot();
  const TimeStatusSnapshot timeStatus = g_wallClock.snapshot(millis());
  int16_t appliedTimezoneOffset = 0;
  const bool timezoneOffsetKnown = timeStatus.utcSec != 0 &&
      timeZoneOffsetAtOrUtc(c.config.timezoneId, timeStatus.utcSec,
                              appliedTimezoneOffset);
  const bool configMutable = controlAllowsConfiguration(c);
  char *buf = work.statusJson;
  constexpr size_t cap = NetworkWorkBuf::kStatusJson;

  httpd_resp_set_type(request, JSON_CONTENT_TYPE);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  httpd_resp_set_hdr(request, "Connection", "close");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "attachment; filename=\"openbrewbyweight-debug.json\"");

  bool ok = debugExportChunkf(
      request, buf, cap,
      "{\"exportSchemaVersion\":%lu,\"exportedAtUtcSec\":%lu,"
      "\"firmwareVersion\":\"%s\",\"bootId\":%lu,\"machineType\":\"%s\","
      "\"boardArch\":\"%s\",\"hardwareProfile\":\"%s\","
      "\"machineProfile\":\"%s\",\"machineBrand\":\"%s\","
      "\"machineModel\":\"%s\",\"sections\":{",
      static_cast<unsigned long>(DEBUG_EXPORT_SCHEMA_VERSION),
      static_cast<unsigned long>(timeStatus.utcSec), FW_VERSION,
      static_cast<unsigned long>(c.bootId), compiledMachineTypeId(),
      FW_BOARD_ARCH_STRING, OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID,
      OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID, OPEN_BREW_BY_WEIGHT_MACHINE_BRAND,
      OPEN_BREW_BY_WEIGHT_MACHINE_MODEL);

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"envelope\":{\"configMutable\":%s,\"liveShot\":%s,"
           "\"snapshotStale\":%s,"
           "\"configRevision\":%lu,\"ringRetainLogLevel\":\"%s\","
           "\"serialLogLevel\":\"%s\","
           "\"bootState\":\"%s\",\"bootComplete\":%s,\"bootDegraded\":%s,"
           "\"settingsPersistenceReady\":%s,"
           "\"scaleWorkerReady\":%s},",
           configMutable ? "true" : "false",
           (c.activeCycle || c.machineRunning || c.relayClosed) ? "true"
                                                                : "false",
           c.snapshotStale ? "true" : "false",
           static_cast<unsigned long>(c.config.revision),
           logLevelName(
               static_cast<LogLevel>(c.config.ringRetainLogLevel)),
           logLevelName(serialLogLevelFromRuntime(c.config)),
           bootStateName(c.bootState),
           c.bootComplete ? "true" : "false",
           c.bootDegraded ? "true" : "false",
           c.bootCapabilities.settingsPersistenceWorker ? "true" : "false",
           c.scaleWorkerReady ? "true" : "false");

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"runtimeConfig\":{\"revision\":%lu,\"goalWeightG\":%.1f,"
           "\"weightOffsetG\":%.2f,\"weightOffsetBaselineG\":%.2f,"
           "\"autoTare\":%s,\"postTareBaselineGraceMs\":%lu,"
           "\"timerOnly\":%s,\"brewByWeight\":%s,\"canTareStartTimer\":%s,"
           "\"scaleTimerStopExtraDelayMs\":%lu,\"dripDelayMs\":%lu,"
           "\"soundAlertsMuted\":%s,\"firstDropBeep\":%s,"
           "\"paddleReturnReminderBeep\":%s,"
           "\"paddleReturnReminderIntervalMs\":%lu,"
           "\"paddleReturnReminderMaxDurationMs\":%lu,"
           "\"bbwProtectionMs\":%lu,\"operationalWallMs\":%lu,"
           "\"fastExtractionGuardEnabled\":%s,\"fastMarginG\":%.1f,"
           "\"fastBeforeMs\":%lu,\"slowExtractionGuardEnabled\":%s,"
           "\"slowMarginG\":%.1f,\"slowAfterMs\":%lu,"
           "\"fastRampMs\":%u,\"slowRampMs\":%u,\"fastCurve\":\"%s\","
           "\"fastCurveHardness\":\"%s\",\"slowCurve\":\"%s\",\"slowCurveHardness\":\"%s\","
           "\"autoToManualGuardEnabled\":%s,\"scaleLossFixedMs\":%lu,"
           "\"cupProtectionEnabled\":%s,\"stopIfCupRemoved\":%s,"
           "\"requireCupToStart\":%s,\"avoidAccidentalTouchEnabled\":%s,"
           "\"cupRemovedWeightG\":%.1f,"
           "\"noScaleBbwMode\":\"%s\","
           "\"noScaleAllowRinseWhileArmed\":%s,\"autoRetare\":%s,"
           "\"retareWindowMs\":%lu,\"minimumCupWeightG\":%.1f,"
           "\"lastShotCooldownMs\":%lu,\"timezoneId\":\"%s\","
           "\"appliedTimezoneOffsetMinutes\":%d,\"timezoneOffsetKnown\":%s,"
           "\"tzdbVersion\":\"%s\",\"timezoneResolution\":\"%s\","
           "\"serialLogLevel\":\"%s\","
           "\"powerManagementEnabled\":%s},",
           static_cast<unsigned long>(c.config.revision),
           static_cast<double>(goalWeightDgToGrams(c.config.goalWeightDg)),
           static_cast<double>(c.config.weightOffsetG),
           static_cast<double>(c.config.weightOffsetBaselineG),
           c.config.autoTare ? "true" : "false",
           static_cast<unsigned long>(c.config.postTareBaselineGraceMs),
           c.config.timerOnly ? "true" : "false",
           c.config.timerOnly ? "false" : "true",
           c.config.canTareStartTimer ? "true" : "false",
           static_cast<unsigned long>(c.config.scaleTimerStopExtraDelayMs),
           static_cast<unsigned long>(c.config.dripDelayMs),
           c.config.soundAlertsMuted ? "true" : "false",
           c.config.firstDropBeep ? "true" : "false",
           c.config.paddleReturnReminderBeep ? "true" : "false",
           static_cast<unsigned long>(c.config.paddleReturnReminderIntervalMs),
           static_cast<unsigned long>(
               c.config.paddleReturnReminderMaxDurationMs),
           static_cast<unsigned long>(c.config.bbwProtectionMs),
           static_cast<unsigned long>(c.config.operationalWallMs),
           c.config.fastSideEnabled ? "true" : "false",
           static_cast<double>(goalWeightDgToGrams(c.config.fastMarginDg)),
           static_cast<unsigned long>(c.config.fastBeforeMs),
           c.config.slowSideEnabled ? "true" : "false",
           static_cast<double>(goalWeightDgToGrams(c.config.slowMarginDg)),
           static_cast<unsigned long>(c.config.slowAfterMs),
           static_cast<unsigned>(c.config.fastRampMs), static_cast<unsigned>(c.config.slowRampMs),
           rampCurveName(c.config.fastCurve), rampHardnessName(c.config.fastCurveHardness),
           rampCurveName(c.config.slowCurve), rampHardnessName(c.config.slowCurveHardness),
           c.config.scaleLossEnabled ? "true" : "false",
           static_cast<unsigned long>(c.config.scaleLossFixedMs),
           c.config.cupProtectionEnabled ? "true" : "false",
           c.config.stopIfCupRemoved ? "true" : "false",
           c.config.requireCupToStart ? "true" : "false",
           c.config.avoidAccidentalTouchEnabled ? "true" : "false",
           static_cast<double>(c.config.cupRemovedWeightG),
           noScaleBbwModeId(c.config.noScaleBbwMode),
           noScaleAllowRinseWhileArmed(c.config.noScaleBbwMode) ? "true"
                                                              : "false",
           c.config.autoRetare ? "true" : "false",
           static_cast<unsigned long>(c.config.retareWindowMs),
           static_cast<double>(c.config.minimumCupWeightG),
           static_cast<unsigned long>(c.config.lastShotCooldownMs),
           c.config.timezoneId, static_cast<int>(appliedTimezoneOffset),
           timezoneOffsetKnown ? "true" : "false", tzdb::kVersion,
           timeZoneResolution(c.config.timezoneId, timeStatus.utcSec),
           logLevelName(serialLogLevelFromRuntime(c.config)),
           c.config.powerManagementEnabled ? "true" : "false");

  if (self.callbacks_.copyPresetBank != nullptr) {
    self.callbacks_.copyPresetBank(&work.presetBank);
  } else {
    memset(&work.presetBank, 0, sizeof(work.presetBank));
  }
  ok = ok && debugExportChunk(request, "\"presets\":{\"activeId\":");
  ok = ok &&
       debugExportChunkf(request, buf, cap, "%u,\"items\":[",
                         static_cast<unsigned>(work.presetBank.activeId));
  for (uint8_t i = 0;
       ok && i < work.presetBank.count && i < MAX_SHOT_PRESETS; ++i) {
    const ShotPreset &p = work.presetBank.presets[i];
    char safeName[SHOT_PRESET_NAME_CAPACITY * 6] = {};
    escapeJsonString(p.name, safeName, sizeof(safeName));
    ok = debugExportChunkf(
        request, buf, cap,
        "%s{\"id\":%u,\"name\":\"%s\",\"isFactory\":%s,\"brewByWeight\":%s,"
        "\"goalWeightG\":%.1f,\"doseG\":%.1f,\"fastBeforeMs\":%lu,"
        "\"slowAfterMs\":%lu,\"fastSideEnabled\":%s,\"fastMarginG\":%.1f,"
        "\"slowSideEnabled\":%s,\"slowMarginG\":%.1f,"
        "\"scaleLossFixedMs\":%lu,\"learningEpoch\":%lu,"
        "\"grindHintsEnabled\":%s,\"fastRampMs\":%u,\"slowRampMs\":%u,"
        "\"fastCurve\":\"%s\",\"fastCurveHardness\":\"%s\",\"slowCurve\":\"%s\","
        "\"slowCurveHardness\":\"%s\"}",
        i == 0 ? "" : ",", static_cast<unsigned>(p.id), safeName,
        p.isFactory ? "true" : "false", p.brewByWeight ? "true" : "false",
        static_cast<double>(goalWeightDgToGrams(p.goalWeightDg)),
        static_cast<double>(goalWeightDgToGrams(p.doseDg)),
        static_cast<unsigned long>(p.fastBeforeMs),
        static_cast<unsigned long>(p.slowAfterMs),
        p.fastSideEnabled ? "true" : "false",
        static_cast<double>(goalWeightDgToGrams(p.fastMarginDg)),
        p.slowSideEnabled ? "true" : "false",
        static_cast<double>(goalWeightDgToGrams(p.slowMarginDg)),
        static_cast<unsigned long>(p.scaleLossFixedMs),
        static_cast<unsigned long>(p.learningEpoch),
        p.grindHintsEnabled ? "true" : "false", static_cast<unsigned>(p.fastRampMs),
        static_cast<unsigned>(p.slowRampMs), rampCurveName(p.fastCurve),
        rampHardnessName(p.fastCurveHardness), rampCurveName(p.slowCurve),
        rampHardnessName(p.slowCurveHardness));
  }
  ok = ok && debugExportChunk(request, "]},");

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"controlSnapshot\":{\"state\":\"%s\",\"activeCycle\":%s,"
           "\"relayClosed\":%s,\"machineRunning\":%s,\"reedOn\":%s,"
           "\"physicalActivatorOn\":%s,\"controlSource\":\"%s\","
           "\"weightStreamState\":\"%s\",\"weightControlState\":\"%s\","
           "\"scaleAvailable\":%s,\"currentWeightValid\":%s,"
           "\"currentWeightG\":%.2f,\"observedWeightValid\":%s,"
           "\"observedWeightG\":%.2f,\"cycleId\":%lu,\"uptimeMs\":%lu,"
           "\"snapshotVersion\":%lu,\"snapshotStale\":%s,"
           "\"loopIntervalGapMs\":%lu,\"loopMaxGapMs\":%lu,"
           "\"loopDeadlineMisses\":%lu,\"scaleWorkerMaxGapMs\":%lu,"
           "\"scaleWorkerDeadlineMisses\":%lu,"
           "\"freeHeapBytes\":%lu,\"minimumFreeHeapBytes\":%lu,"
           "\"noScaleShotGuardEnabled\":%s,\"noScaleShotGuardArmed\":%s,"
           "\"noScaleShotGuardHold\":%s,\"cupStartGuardHold\":%s,"
           "\"cycleExtractionExtended\":%s,\"cycleSlowExtractionExtended\":%s,"
           "\"cycleTargetReachedEarly\":%s,\"cycleAutoToManualGuardArmed\":%s,"
           "\"cycleAutoToManualGuardEnforced\":%s,"
           "\"cycleAccidentalTouchHolding\":%s,"
           "\"cycleAccidentalTouchPhase\":\"%s\","
           "\"cycleAccidentalTouchClass\":\"%s\","
           "\"cycleCupRemovedPending\":%s,\"cycleBbwProtectionEnabled\":%s,"
           "\"cycleBbwProtectionEnded\":%s},",
           stopperStateName(c.state), c.activeCycle ? "true" : "false",
           c.relayClosed ? "true" : "false",
           c.machineRunning ? "true" : "false", c.reedOn ? "true" : "false",
           c.physicalActivatorOn ? "true" : "false",
           controlSourceName(c.source),
           weightStreamStateName(c.weightStreamState),
           weightControlStateName(c.weightControlState),
           c.scaleAvailable ? "true" : "false",
           c.currentWeightValid ? "true" : "false",
           static_cast<double>(c.currentWeightG),
           c.observedWeightValid ? "true" : "false",
           static_cast<double>(c.observedWeightG),
           static_cast<unsigned long>(c.cycleId),
           static_cast<unsigned long>(c.uptimeMs),
           static_cast<unsigned long>(c.snapshotVersion),
           c.snapshotStale ? "true" : "false",
           static_cast<unsigned long>(c.loopIntervalGapMs),
           static_cast<unsigned long>(c.loopMaxGapMs),
           static_cast<unsigned long>(c.loopDeadlineMisses),
           static_cast<unsigned long>(c.scaleWorkerMaxGapMs),
           static_cast<unsigned long>(c.scaleWorkerDeadlineMisses),
           static_cast<unsigned long>(c.freeHeapBytes),
           static_cast<unsigned long>(c.minimumFreeHeapBytes),
           c.noScaleShotGuardEnabled ? "true" : "false",
           c.noScaleShotGuardArmed ? "true" : "false",
           c.noScaleShotGuardHold ? "true" : "false",
           c.cupStartGuardHold ? "true" : "false",
           c.cycleExtractionExtended ? "true" : "false",
           c.cycleSlowExtractionExtended ? "true" : "false",
           c.cycleTargetReachedEarly ? "true" : "false",
           c.cycleAutoToManualGuardArmed ? "true" : "false",
           c.cycleAutoToManualGuardEnforced ? "true" : "false",
           c.cycleAccidentalTouchHolding ? "true" : "false",
           accidentalTouchPhaseName(static_cast<AccidentalTouchPhase>(
               c.cycleAccidentalTouchPhase)),
           accidentalTouchClassName(static_cast<AccidentalTouchClass>(
               c.cycleAccidentalTouchClass)),
           c.cycleCupRemovedPending ? "true" : "false",
           c.cycleBbwProtectionEnabled ? "true" : "false",
           c.cycleBbwProtectionEnded ? "true" : "false");

  if (x.sessionActive) {
    ok = ok &&
         debugExportChunkf(
             request, buf, cap,
             "\"session\":{\"active\":true,\"automaticEnabled\":%s,"
             "\"bbwProtectionEnabled\":%s,\"bbwProtectionEnded\":%s,"
             "\"startedWithScale\":%s,\"scaleWasLost\":%s,"
             "\"cupRemovedPending\":%s,\"extractionExtended\":%s,"
             "\"slowExtractionExtended\":%s,\"targetReachedEarly\":%s,"
             "\"autoToManualGuardArmed\":%s,\"autoToManualGuardEnforced\":%s,"
             "\"accidentalTouchHolding\":%s,\"accidentalTouchPhase\":\"%s\","
             "\"accidentalTouchClass\":\"%s\",\"accidentalTouchPendingCount\":%u,"
             "\"activePresetId\":%u,\"weightControlState\":\"%s\","
             "\"id\":%lu,\"startedAtMs\":%lu,\"firstDropMs\":%lu,"
             "\"autoToManualGuardDeadlineAtMs\":%lu,\"targetReachedAtMs\":%lu},",
             x.sessionAutomaticEnabled ? "true" : "false",
             x.sessionBbwProtectionEnabled ? "true" : "false",
             x.sessionBbwProtectionEnded ? "true" : "false",
             x.sessionStartedWithScale ? "true" : "false",
             x.sessionScaleWasLost ? "true" : "false",
             x.sessionCupRemovedPending ? "true" : "false",
             x.sessionExtractionExtended ? "true" : "false",
             x.sessionSlowExtractionExtended ? "true" : "false",
             x.sessionTargetReachedEarly ? "true" : "false",
             x.sessionAutoToManualGuardArmed ? "true" : "false",
             x.sessionAutoToManualGuardEnforced ? "true" : "false",
             x.sessionAccidentalTouchHolding ? "true" : "false",
             accidentalTouchPhaseName(static_cast<AccidentalTouchPhase>(
                 x.sessionAccidentalTouchPhase)),
             accidentalTouchClassName(static_cast<AccidentalTouchClass>(
                 x.sessionAccidentalTouchClass)),
             static_cast<unsigned>(x.sessionAccidentalTouchPendingCount),
             static_cast<unsigned>(x.sessionActivePresetId),
             weightControlStateName(static_cast<WeightControlState>(
                 x.sessionWeightControlState)),
             static_cast<unsigned long>(x.sessionId),
             static_cast<unsigned long>(x.sessionStartedAtMs),
             static_cast<unsigned long>(x.sessionFirstDropMs),
             static_cast<unsigned long>(x.sessionAutoToManualGuardDeadlineAtMs),
             static_cast<unsigned long>(x.sessionTargetReachedAtMs));
  } else {
    ok = ok && debugExportChunk(request, "\"session\":null,");
  }

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"relay\":{\"state\":\"%s\",\"fault\":\"%s\",\"closed\":%s,"
           "\"commandedClosed\":%s,\"feedbackClosed\":%s,"
           "\"feedbackAvailable\":%s,\"externalSafetyPresent\":%s,"
           "\"watchdogReady\":%s,\"timersReady\":%s,\"tripped\":%s,"
           "\"operationalTripped\":%s,\"generation\":%lu,\"closedAtMs\":%lu,"
           "\"operationalLimitMs\":%lu,\"resetReasonCode\":%lu,"
           "\"unsafeResetCount\":%lu,"
           "\"bootLoopDetected\":%s,\"resetHistory\":[",
           relaySafetyStateName(x.relay.state),
           relaySafetyFaultName(x.relay.fault),
           x.relay.closed ? "true" : "false",
           x.relay.commandedClosed ? "true" : "false",
           x.relay.feedbackClosed ? "true" : "false",
           x.relay.feedbackAvailable ? "true" : "false",
           x.relay.externalSafetyPresent ? "true" : "false",
           x.relay.watchdogReady ? "true" : "false",
           x.relay.timersReady ? "true" : "false",
           x.relay.tripped ? "true" : "false",
           x.relay.operationalTripped ? "true" : "false",
           static_cast<unsigned long>(x.relay.generation),
           static_cast<unsigned long>(x.relay.closedAtMs),
           static_cast<unsigned long>(x.relay.operationalLimitMs),
           static_cast<unsigned long>(x.relay.resetReasonCode),
           static_cast<unsigned long>(x.relay.unsafeResetCount),
           x.relay.bootLoopDetected ? "true" : "false");
  for (uint8_t i = 0; i < x.relay.resetHistoryCount; ++i) {
    const ResetHistoryEntry &entry = x.relay.resetHistory[i];
    ok = ok && debugExportChunkf(
                   request, buf, cap,
                   "%s{\"reason\":\"%s\",\"uptimeMs\":%lu}",
                   i ? "," : "", safetyResetReasonName(entry.reasonCode),
                   static_cast<unsigned long>(entry.uptimeMs));
  }
  ok = ok && debugExportChunk(request, "]},");

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"machine\":{\"rawActivatorOn\":%s,\"physicalActivatorOn\":%s,"
           "\"machineRunState\":\"%s\",\"machineStartAckPending\":%s,"
           "\"machineStopAckPending\":%s,\"machineOrphanRun\":%s},",
           x.rawActivatorOn ? "true" : "false",
           x.physicalActivatorOn ? "true" : "false",
           machineRunStateName(static_cast<MachineRunState>(x.machineRunState)),
           x.machineStartAckPending ? "true" : "false",
           x.machineStopAckPending ? "true" : "false",
           x.machineOrphanRun ? "true" : "false");

  {
    const bool bbwEnabled = !c.config.timerOnly;
    const bool scaleUsable =
        c.scaleAvailable && c.weightStreamState == WeightStreamState::FRESH;
    const bool lastShotCupRemoved =
        c.lastShot.valid && c.lastShot.endReason == EndReason::CUP_REMOVED;
    ok = ok &&
         debugExportChunkf(
             request, buf, cap,
             "\"guards\":{\"bbwEnabled\":%s,\"scaleUsable\":%s,"
             "\"lastShotCupRemoved\":%s,"
             "\"noScale\":{\"enabled\":%s,\"armed\":%s,\"hold\":%s,"
             "\"mode\":\"%s\",\"cooldownRemainingMs\":%lu,"
             "\"scaleWasAvailable\":%s},"
             "\"atm\":{\"enabled\":%s,\"armed\":%s,\"enforced\":%s,"
             "\"remainingMs\":%lu},"
             "\"slowExtraction\":{\"enabled\":%s,\"extended\":%s,"
             "\"targetReachedEarly\":%s,\"activeStopWeightG\":%.1f},"
             "\"fastExtraction\":{\"enabled\":%s,\"extended\":%s,"
             "\"targetReachedEarly\":%s,\"activeStopWeightG\":%.1f,"
             "\"minBbwBrewTimeRemainingMs\":%lu},"
             "\"accidentalTouch\":{\"enabled\":%s,\"holding\":%s,"
             "\"phase\":\"%s\",\"class\":\"%s\",\"pendingCount\":%u},"
             "\"cupProtection\":{\"enabled\":%s,\"stopIfRemoved\":%s,"
             "\"requireCupToStart\":%s,\"present\":%s,\"startHold\":%s,"
             "\"removedPending\":%s,\"bbwProtectionActive\":%s,"
             "\"bbwProtectionEnded\":%s}},",
             bbwEnabled ? "true" : "false", scaleUsable ? "true" : "false",
             lastShotCupRemoved ? "true" : "false",
             c.noScaleShotGuardEnabled ? "true" : "false",
             c.noScaleShotGuardArmed ? "true" : "false",
             c.noScaleShotGuardHold ? "true" : "false",
             noScaleBbwModeId(c.config.noScaleBbwMode),
             static_cast<unsigned long>(
                 c.noScaleShotGuardCooldownRemainingMs),
             c.noScaleShotGuardScaleWasAvailable ? "true" : "false",
             c.config.scaleLossEnabled ? "true" : "false",
             c.cycleAutoToManualGuardArmed ? "true" : "false",
             c.cycleAutoToManualGuardEnforced ? "true" : "false",
             static_cast<unsigned long>(c.cycleAutoToManualGuardRemainingMs),
             c.config.slowSideEnabled ? "true" : "false",
             c.cycleSlowExtractionExtended ? "true" : "false",
             c.cycleTargetReachedEarly ? "true" : "false",
             static_cast<double>(c.cycleActiveStopWeightG),
             c.config.fastSideEnabled ? "true" : "false",
             c.cycleExtractionExtended ? "true" : "false",
             c.cycleTargetReachedEarly ? "true" : "false",
             static_cast<double>(c.cycleActiveStopWeightG),
             static_cast<unsigned long>(c.cycleMinBbwBrewTimeRemainingMs),
             c.config.avoidAccidentalTouchEnabled ? "true" : "false",
             c.cycleAccidentalTouchHolding ? "true" : "false",
             accidentalTouchPhaseName(static_cast<AccidentalTouchPhase>(
                 c.cycleAccidentalTouchPhase)),
             accidentalTouchClassName(static_cast<AccidentalTouchClass>(
                 c.cycleAccidentalTouchClass)),
             static_cast<unsigned>(c.cycleAccidentalTouchPendingCount),
             c.config.cupProtectionEnabled ? "true" : "false",
             c.config.stopIfCupRemoved ? "true" : "false",
             c.config.requireCupToStart ? "true" : "false",
             c.cupPresent ? "true" : "false",
             c.cupStartGuardHold ? "true" : "false",
             c.cycleCupRemovedPending ? "true" : "false",
             c.cycleBbwProtectionEnabled && !c.cycleBbwProtectionEnded ? "true"
                                                                       : "false",
             c.cycleBbwProtectionEnded ? "true" : "false");
  }

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"cupPresence\":{\"state\":\"%s\",\"holdTransitions\":%s,"
           "\"inNegativeHole\":%s,"
           "\"removedConfirmations\":%u,\"placeStabilitySamples\":%u,"
           "\"holeWeightG\":%.2f,\"placeCandidateWeightG\":%.2f},",
           cupPresenceStateName(static_cast<CupPresenceState>(x.cupState)),
           x.cupHoldTransitions ? "true" : "false",
           x.cupInNegativeHole ? "true" : "false",
           static_cast<unsigned>(x.cupRemovedConfirmations),
           static_cast<unsigned>(x.cupPlaceStabilitySamples),
           static_cast<double>(x.cupHoleWeightG),
           static_cast<double>(x.cupPlaceCandidateWeightG));

  char scaleLinkRssiJson[16] = "null";
  const CupTareDiagnostics &tare = x.cupTare;
  char anchorDeltaJson[16] = "null";
  if (std::isfinite(tare.anchorDeltaG))
    snprintf(anchorDeltaJson, sizeof(anchorDeltaJson), "%.2f",
             static_cast<double>(tare.anchorDeltaG));
  ok = ok && debugExportChunkf(
      request, buf, cap,
      "\"idleTare\":{\"requestId\":%lu,\"placementId\":%lu,\"requestPlacementId\":%lu,\"phase\":%u,"
      "\"eligibilityReason\":\"%s\",\"lastReason\":\"%s\","
      "\"referenceKnown\":%s,\"tared\":%s,\"absentObserved\":%s,"
      "\"referenceG\":%.2f,\"qualificationMinG\":%.2f,\"qualificationMaxG\":%.2f,\"lastWeightG\":%.2f,"
      "\"anchorDeltaG\":%s,\"anchorWaitMs\":%lu,",
      static_cast<unsigned long>(tare.requestId),
      static_cast<unsigned long>(tare.placementId),
      static_cast<unsigned long>(tare.requestPlacementId), static_cast<unsigned>(tare.phase),
      idleTareReasonName(tare.eligibilityReason), idleTareReasonName(tare.lastReason),
      tare.referenceKnown ? "true" : "false", tare.tared ? "true" : "false",
      tare.absentObserved ? "true" : "false",
      static_cast<double>(tare.occupiedReferenceG),
      static_cast<double>(tare.requestMinimumG), static_cast<double>(tare.requestMaximumG),
      static_cast<double>(tare.lastWeightG), anchorDeltaJson,
      static_cast<unsigned long>(tare.anchorWaitMs));
  ok = ok && debugExportChunkf(
      request, buf, cap,
      "\"connectionGeneration\":%lu,\"configRevision\":%lu,\"requestedAtMs\":%lu,"
      "\"startedAtMs\":%lu,\"writtenAtMs\":%lu,\"effectAtMs\":%lu,"
      "\"captureBoundary\":%lu,\"lastSampleAtMs\":%lu,\"lastPacketSequence\":%lu,"
      "\"lastCaptureSequence\":%lu,\"lastTerminalRequestId\":%lu,"
      "\"lastTerminalAtMs\":%lu,\"rejectedSamples\":%lu,\"droppedSamples\":%lu},",
      static_cast<unsigned long>(tare.connectionGeneration),
      static_cast<unsigned long>(tare.configRevision),
      static_cast<unsigned long>(tare.requestedAtMs),
      static_cast<unsigned long>(tare.startedAtMs), static_cast<unsigned long>(tare.writtenAtMs),
      static_cast<unsigned long>(tare.effectAtMs), static_cast<unsigned long>(tare.captureBoundary),
      static_cast<unsigned long>(tare.lastSampleAtMs),
      static_cast<unsigned long>(tare.lastPacketSequence),
      static_cast<unsigned long>(tare.lastCaptureSequence),
      static_cast<unsigned long>(tare.lastTerminalRequestId),
      static_cast<unsigned long>(tare.lastTerminalAtMs),
      static_cast<unsigned long>(tare.rejectedSamples),
      static_cast<unsigned long>(tare.droppedSamples));
  if (c.scaleRssiValid) {
    snprintf(scaleLinkRssiJson, sizeof(scaleLinkRssiJson), "%d",
             static_cast<int>(c.scaleRssi));
  }

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"scaleLink\":{\"state\":\"%s\",\"disconnectSequence\":%lu,"
           "\"connectionGeneration\":%lu,\"packetSequence\":%lu,"
           "\"packetGaps\":%lu,\"rejectedPackets\":%lu,\"reconnects\":%lu,"
           "\"recoveredStaleCount\":%lu,\"recoveredStaleMs\":%lu,"
           "\"weightUpdateIntervalMs\":%lu,"
           "\"workerProgressAtMs\":%lu,\"timerValid\":%s,\"timerMs\":%lu,"
           "\"timerAgeMs\":%lu,\"protocol\":\"%s\","
           "\"lastDisconnectReasonName\":\"%s\",\"rssi\":%s},",
           debugExportScaleLinkStateName(x.scaleLinkState),
           static_cast<unsigned long>(x.scaleDisconnectSequence),
           static_cast<unsigned long>(x.scaleConnectionGeneration),
           static_cast<unsigned long>(x.scalePacketSequence),
           static_cast<unsigned long>(c.scalePacketGaps),
           static_cast<unsigned long>(c.scaleRejectedPackets),
           static_cast<unsigned long>(c.scaleReconnects),
           static_cast<unsigned long>(c.scaleRecoveredStaleCount),
           static_cast<unsigned long>(c.scaleRecoveredStaleMs),
           static_cast<unsigned long>(c.scaleWeightUpdateIntervalMs),
           static_cast<unsigned long>(x.scaleWorkerProgressAtMs),
           x.scaleTimerValid ? "true" : "false",
           static_cast<unsigned long>(x.scaleTimerMs),
           static_cast<unsigned long>(x.scaleTimerAgeMs), x.scaleProtocolName,
           scaleDisconnectReasonName(c.scaleLastDisconnectReason),
           scaleLinkRssiJson);

  char safeStaSsid[WIFI_SSID_CAPACITY] = {};
  char safeApSsid[WIFI_SSID_CAPACITY] = {};
  sanitizeJsonEmbed(network.iface.staSsid, safeStaSsid, sizeof(safeStaSsid));
  sanitizeJsonEmbed(network.iface.apSsid, safeApSsid, sizeof(safeApSsid));
  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"network\":{\"networkActive\":%s,\"apActive\":%s,\"apSsid\":\"%s\","
           "\"wifiConfigured\":%s,\"staState\":\"%s\",\"staIp\":\"%s\","
           "\"ssid\":\"%s\",\"apIp\":\"%s\",\"apClients\":%u,"
           "\"httpActive\":%s,\"staMac\":\"%s\",\"staBssid\":\"%s\","
           "\"apMac\":\"%s\",\"staReconnectHeld\":%s,\"apStartHeld\":%s,"
           "\"httpStartHeld\":%s,\"taskAgeMs\":%lu,\"taskStackMinWords\":%lu,"
           "\"startupFailures\":%lu,\"channel\":%u,\"rssi\":%d,"
           "\"signalQualityPct\":%u,\"configState\":\"%s\","
           "\"ipMode\":\"%s\",\"confirmRemainingMs\":%lu},",
           network.networkActive ? "true" : "false",
           network.apActive ? "true" : "false", safeApSsid,
           network.wifiConfigured ? "true" : "false",
           staStateName(network.staState), network.iface.staIp, safeStaSsid,
           network.iface.apIp, static_cast<unsigned>(network.apClients),
           network.httpActive ? "true" : "false", network.iface.staMac,
           network.iface.staBssid, network.iface.apMac,
           network.staReconnectHeld ? "true" : "false",
           network.apStartHeld ? "true" : "false",
           network.httpStartHeld ? "true" : "false",
           static_cast<unsigned long>(network.taskAgeMs),
           static_cast<unsigned long>(network.taskStackMinBytes),
           static_cast<unsigned long>(network.startupFailures),
           static_cast<unsigned>(network.channel),
           static_cast<int>(network.staRssi),
           static_cast<unsigned>(network.staSignalQualityPct),
           staConfigStateName(network.staConfigState),
           staIpModeName(network.staIpMode),
           static_cast<unsigned long>(network.confirmRemainingMs));

  char safeActiveServer[NTP_SERVER_HOST_CAPACITY] = {};
  sanitizeJsonEmbed(timeStatus.activeServer, safeActiveServer,
                    sizeof(safeActiveServer));
  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"time\":{\"state\":\"%s\",\"utcSec\":%lu,\"lastSyncAgeMs\":%lu,"
           "\"nextRetryInMs\":%lu,\"activeServer\":\"%s\"},",
           timeSyncStateName(timeStatus.state),
           static_cast<unsigned long>(timeStatus.utcSec),
           static_cast<unsigned long>(timeStatus.lastSyncAgeMs),
           static_cast<unsigned long>(timeStatus.nextRetryInMs),
           safeActiveServer);

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"health\":{\"stackUnit\":\"bytes\",\"stackUnavailable\":4294967295,"
           "\"uptimeMs\":%lu,\"snapshotVersion\":%lu,"
           "\"snapshotStale\":%s,\"loopIntervalGapMs\":%lu,"
           "\"loopMaxGapMs\":%lu,\"loopDeadlineMisses\":%lu,"
           "\"loopMaxExecutionUs\":%lu,"
           "\"scaleWorkerMaxGapMs\":%lu,\"scaleWorkerDeadlineMisses\":%lu,"
           "\"scaleWorkerMaxExecutionUs\":%lu,"
           "\"healthSnapshotVersion\":%lu,\"healthSnapshotAgeMs\":%lu,"
           "\"healthSnapshotValid\":%s,"
           "\"loopStackMinWords\":%lu,"
           "\"scaleStackMinWords\":%lu,\"freeHeapBytes\":%lu,"
           "\"minimumFreeHeapBytes\":%lu,"
           "\"largestFreeHeapBlockBytes\":%lu,"
           "\"internalHeapAllocatedBlocks\":%lu,"
           "\"internalHeapFreeBlocks\":%lu,\"internalHeapTotalBlocks\":%lu,"
           "\"internalHeapFragmentationPermille\":%u,"
           "\"psramSizeBytes\":%lu,\"psramFreeBytes\":%lu,"
           "\"psramMinimumFreeBytes\":%lu,"
           "\"psramLargestFreeBlockBytes\":%lu,\"bleBackend\":\"%s\","
           "\"bleRuntimeState\":%u,\"bleRuntimeLastError\":%ld,"
           "\"bleRuntimeLastResetReason\":%ld,"
           "\"bleRuntimeSyncGeneration\":%lu,\"bleRuntimeResetCount\":%lu,"
           "\"bleRuntimeHostStackMinWords\":%lu,\"workBufExternal\":%s,"
           "\"jsonArenaExternal\":%s,\"allocExternalFallback\":%lu,"
           "\"heapAlertLatched\":%s,"
           "\"stackAlertLatched\":%s,\"loopGapAlertLatched\":%s,"
           "\"cpuLoad5s\":%.2f,\"cpuLoad1m\":%.2f,\"cpuLoad5m\":%.2f,"
           "\"cpuMhz\":%lu,\"tempC\":%.1f,\"tempPeakC\":%.1f},",
           static_cast<unsigned long>(c.uptimeMs),
           static_cast<unsigned long>(c.snapshotVersion),
           c.snapshotStale ? "true" : "false",
           static_cast<unsigned long>(c.loopIntervalGapMs),
           static_cast<unsigned long>(c.loopMaxGapMs),
           static_cast<unsigned long>(c.loopDeadlineMisses),
           static_cast<unsigned long>(c.loopMaxExecutionUs),
           static_cast<unsigned long>(c.scaleWorkerMaxGapMs),
           static_cast<unsigned long>(c.scaleWorkerDeadlineMisses),
           static_cast<unsigned long>(c.scaleWorkerMaxExecutionUs),
           static_cast<unsigned long>(c.healthSnapshotVersion),
           static_cast<unsigned long>(c.healthSnapshotAgeMs),
           c.healthSnapshotValid ? "true" : "false",
           static_cast<unsigned long>(c.loopStackMinBytes),
           static_cast<unsigned long>(c.scaleStackMinBytes),
           static_cast<unsigned long>(c.freeHeapBytes),
           static_cast<unsigned long>(c.minimumFreeHeapBytes),
           static_cast<unsigned long>(c.largestFreeHeapBlockBytes),
           static_cast<unsigned long>(c.internalHeapAllocatedBlocks),
           static_cast<unsigned long>(c.internalHeapFreeBlocks),
           static_cast<unsigned long>(c.internalHeapAllocatedBlocks +
                                      c.internalHeapFreeBlocks),
           static_cast<unsigned>(c.internalHeapFragmentationPermille),
           static_cast<unsigned long>(c.psramSizeBytes),
           static_cast<unsigned long>(c.psramFreeBytes),
           static_cast<unsigned long>(c.psramMinimumFreeBytes),
           static_cast<unsigned long>(c.psramLargestFreeBlockBytes),
           "nimble",
           static_cast<unsigned>(c.bleRuntimeState),
           static_cast<long>(c.bleRuntimeLastError),
           static_cast<long>(c.bleRuntimeLastResetReason),
           static_cast<unsigned long>(c.bleRuntimeSyncGeneration),
           static_cast<unsigned long>(c.bleRuntimeResetCount),
           static_cast<unsigned long>(c.bleRuntimeHostStackMinBytes),
           c.workBufExternal ? "true" : "false",
           c.jsonArenaExternal ? "true" : "false",
           static_cast<unsigned long>(c.allocExternalFallbackCount),
           x.healthHeapAlertLatched ? "true" : "false",
           x.healthStackAlertLatched ? "true" : "false",
           x.healthLoopGapAlertLatched ? "true" : "false",
           static_cast<double>(c.hwmon.cpuLoad5s),
           static_cast<double>(c.hwmon.cpuLoad1m),
           static_cast<double>(c.hwmon.cpuLoad5m),
           static_cast<unsigned long>(c.hwmon.cpuMhz),
           static_cast<double>(c.hwmon.tempC),
           static_cast<double>(c.hwmon.tempPeakC));

  if (self.callbacks_.copyTaskProfiler != nullptr) {
    self.callbacks_.copyTaskProfiler(work.taskProfiler);
  } else {
    work.taskProfiler = TaskProfilerSnapshot{};
  }
  {
    size_t used = 0;
    buf[0] = '\0';
    ok = ok && jsonScratchAppend(buf, cap, &used, "\"tasks\":");
    ok = ok &&
         formatTaskProfilerObject(buf, cap, &used, work.taskProfiler);
    ok = ok && jsonScratchAppend(buf, cap, &used, ",");
    ok = ok && debugExportChunk(request, buf);
  }

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"safety\":{\"state\":\"%s\",\"fault\":\"%s\","
           "\"taskWatchdogReady\":%s,\"externalHardware\":%s,"
           "\"resetReasonCode\":%lu,"
           "\"unsafeResetCount\":%lu,\"bootLoopDetected\":%s,"
           "\"safetyGeneration\":%lu,\"safetyTimersReady\":%s},",
           relaySafetyStateName(c.safetyState),
           relaySafetyFaultName(c.safetyFault),
           c.taskWatchdogReady ? "true" : "false",
           c.externalSafetyPresent ? "true" : "false",
           static_cast<unsigned long>(c.resetReasonCode),
           static_cast<unsigned long>(c.unsafeResetCount),
           c.bootLoopDetected ? "true" : "false",
           static_cast<unsigned long>(c.safetyGeneration),
           c.safetyTimersReady ? "true" : "false");

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"bleScan\":{\"scanIntensity\":\"%s\"},",
           bleScanIntensityName(clampBleScanIntensity(c.bleScanIntensity)));

  self.buildOtaJson(work.otaJson, NetworkWorkBuf::kOtaJson, controlGateOf(c), c.bootId);
  ok = ok && debugExportChunk(request, "\"ota\":");
  ok = ok && debugExportChunk(request, work.otaJson);
  ok = ok && debugExportChunk(request, ",");

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"lastShot\":{\"valid\":%s,\"goalWeightG\":%.1f,\"durationMs\":%lu,"
           "\"endReason\":\"%s\",\"shotType\":\"%s\","
           "\"extractionExtended\":%s,\"slowExtractionExtended\":%s},"
           "\"compileFlags\":{\"buzzer\":\"%s\",\"remoteMachineControl\":%s,"
           "\"arch\":\"%s\",\"hardwareProfile\":\"%s\","
           "\"machineProfile\":\"%s\",\"machineBrand\":\"%s\","
           "\"machineModel\":\"%s\",\"machineType\":\"%s\",\"stopPulseMs\":%lu,"
           "\"maxSinglePressMs\":%lu,\"development\":%s},",
           c.lastShot.valid ? "true" : "false",
           static_cast<double>(goalWeightDgToGrams(c.lastShot.goalWeightDg)),
           static_cast<unsigned long>(c.lastShot.durationMs),
           self.endReasonName(c.lastShot.endReason),
           lastShotTypeName(static_cast<LastShotType>(c.lastShot.shotType)),
           c.lastShot.extractionExtended ? "true" : "false",
           c.lastShot.slowExtractionExtended ? "true" : "false",
           compiledBuzzerModeId(),
           REMOTE_MACHINE_CONTROL_ENABLED ? "true" : "false",
           FW_BOARD_ARCH_STRING, OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID,
           OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID, OPEN_BREW_BY_WEIGHT_MACHINE_BRAND,
           OPEN_BREW_BY_WEIGHT_MACHINE_MODEL, compiledMachineTypeId(),
           static_cast<unsigned long>(COMPILED_STOP_PULSE_MS),
           static_cast<unsigned long>(COMPILED_MAX_SINGLE_PRESS_MS),
           DEVELOPMENT_BUILD ? "true" : "false");

  ok = ok &&
       debugExportChunkf(
           request, buf, cap,
           "\"events\":{\"dropped\":%lu,\"items\":[",
           static_cast<unsigned long>(c.debugEventsDropped));
  if (ok && self.callbacks_.copyDebugEvents != nullptr) {
    uint32_t after = 0;
    bool first = true;
    for (;;) {
      const size_t count = self.callbacks_.copyDebugEvents(
          after, work.logBatch, kNetworkLogBatchSize, nullptr);
      if (count == 0) {
        break;
      }
      for (size_t index = 0; ok && index < count; ++index) {
        const DebugEvent &event = work.logBatch[index];
        char safeEventText[DEBUG_EVENT_TEXT_CAPACITY * 6] = {};
        escapeJsonString(event.text, safeEventText, sizeof(safeEventText));
        ok = debugExportChunkf(
            request, buf, cap,
            "%s{\"sequence\":%lu,\"atMs\":%lu,\"wallSec\":%lu,"
            "\"level\":\"%s\",\"category\":\"%s\",\"code\":\"%s\","
            "\"argument1\":%ld,\"argument2\":%ld,\"text\":\"%s\"}",
            first ? "" : ",", static_cast<unsigned long>(event.sequence),
            static_cast<unsigned long>(event.atMs),
            static_cast<unsigned long>(event.wallSec),
            logLevelName(event.level), debugCategoryName(event.category),
            debugCodeName(event.code), static_cast<long>(event.argument1),
            static_cast<long>(event.argument2), safeEventText);
        first = false;
        after = event.sequence;
      }
      if (count < kNetworkLogBatchSize) {
        break;
      }
    }
  }
  ok = ok && debugExportChunk(request, "]},");

  // Curated audit latch: rare, classified events that outlive ring rotation.
  uint32_t auditDropped = 0, auditRateLimited = 0;
  if (ok && self.callbacks_.copyAuditEvents != nullptr) {
    (void)self.callbacks_.copyAuditEvents(0, nullptr, 0, &auditDropped,
                                          &auditRateLimited);
  }
  ok = ok && debugExportChunkf(request, buf, cap,
                               "\"audit\":{\"dropped\":%lu,\"rateLimited\":%lu,"
                               "\"items\":[",
                               static_cast<unsigned long>(auditDropped),
                               static_cast<unsigned long>(auditRateLimited));
  if (ok && self.callbacks_.copyAuditEvents != nullptr) {
    uint32_t after = 0;
    bool first = true;
    for (;;) {
      AuditRecord *batch = work.auditBatch;
      const size_t count = self.callbacks_.copyAuditEvents(
          after, batch, NetworkWorkBuf::kAuditBatchSize, nullptr, nullptr);
      if (count == 0) {
        break;
      }
      for (size_t index = 0; ok && index < count; ++index) {
        const AuditRecord &record = batch[index];
        char detail[128] = {};
        char safeDetail[sizeof(detail) * 6] = {};
        formatAuditRecordDetail(record, detail, sizeof(detail));
        escapeJsonString(detail, safeDetail, sizeof(safeDetail));
        ok = debugExportChunkf(
            request, buf, cap,
            "%s{\"sequence\":%lu,\"atMs\":%lu,\"wallSec\":%lu,"
            "\"level\":\"%s\",\"category\":\"%s\",\"code\":\"%s\","
            "\"argument1\":%ld,\"argument2\":%ld,\"text\":\"%s\"}",
            first ? "" : ",", static_cast<unsigned long>(record.sequence),
            static_cast<unsigned long>(record.atMs),
            static_cast<unsigned long>(record.wallSec),
            logLevelName(static_cast<LogLevel>(record.level)),
            debugCategoryName(static_cast<DebugCategory>(record.category)),
            debugCodeName(static_cast<DebugCode>(record.code)),
            static_cast<long>(record.argument1),
            static_cast<long>(record.argument2), safeDetail);
        first = false;
        after = record.sequence;
      }
      if (count < NetworkWorkBuf::kAuditBatchSize) {
        break;
      }
    }
  }
  ok = ok && debugExportChunk(request, "]},");

#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  {
    const HeapLifecycleAggregate micraHeap = machineIntegrationHeapTelemetry();
    const MicraWebSocketStatus ws = machineIntegrationWebSocketStatus(false);
    ok = ok &&
         debugExportChunkf(
             request, buf, cap,
             "\"machineIntegrationHeap\":{"
             "\"service\":{\"cycles\":%lu,\"staleReplacements\":%lu,"
             "\"lastEvent\":%u,\"lastResult\":%u,"
             "\"lastFreeDelta\":%ld,\"lastLargestDelta\":%ld,"
             "\"worstFreeDelta\":%ld,\"worstLargestDelta\":%ld},"
             "\"websocket\":{\"cycles\":%lu,"
             "\"lastFreeDelta\":%ld,\"lastLargestDelta\":%ld,"
             "\"worstFreeDelta\":%ld,\"worstLargestDelta\":%ld,"
             "\"stop\":{\"lastFreeDelta\":%ld,\"lastLargestDelta\":%ld,"
             "\"worstFreeDelta\":%ld,\"worstLargestDelta\":%ld},"
             "\"allocationFailures\":%lu}},",
             static_cast<unsigned long>(micraHeap.cycles),
             static_cast<unsigned long>(micraHeap.staleReplacements),
             static_cast<unsigned>(micraHeap.lastEvent),
             static_cast<unsigned>(micraHeap.lastResult),
             static_cast<long>(micraHeap.lastDelta.freeBytes),
             static_cast<long>(micraHeap.lastDelta.largestBlock),
             static_cast<long>(micraHeap.worstFreeDelta),
             static_cast<long>(micraHeap.worstLargestDelta),
             static_cast<unsigned long>(ws.heap.cycles),
             static_cast<long>(ws.heap.lastDelta.freeBytes),
             static_cast<long>(ws.heap.lastDelta.largestBlock),
             static_cast<long>(ws.heap.worstFreeDelta),
             static_cast<long>(ws.heap.worstLargestDelta),
             static_cast<long>(ws.stopHeap.lastDelta.freeBytes),
             static_cast<long>(ws.stopHeap.lastDelta.largestBlock),
             static_cast<long>(ws.stopHeap.worstFreeDelta),
             static_cast<long>(ws.stopHeap.worstLargestDelta),
             static_cast<unsigned long>(ws.allocationFailures));
  }
#endif

  {
    // Boot heap capture (the serial BOOT_HEAP report): stages as
    // [stage,result,ms,free,largest], then post/ws [samples,free,largest].
    // Copy under the lock piecewise; never hold it across a socket send.
    uint32_t stages;
    BootHeapMinimum minima[2];
    {
      TaskLockGuard lock(bootHeapMutex);
      stages = bootHeapCapture.stages;
      minima[0] = bootHeapCapture.post;
      minima[1] = bootHeapCapture.ws;
    }
    ok = ok && debugExportChunk(request, "\"bootHeap\":{\"stages\":[");
    const char *separator = "";
    for (size_t i = 0; i < static_cast<size_t>(BootHeapStage::COUNT); ++i) {
      if (!(stages & (1U << i))) continue;
      BootHeapRecord r;
      {
        TaskLockGuard lock(bootHeapMutex);
        r = bootHeapCapture.records[i];
      }
      ok = ok && debugExportChunkf(request, buf, cap, "%s[\"%s\",\"%s\",%lu,%lu,%lu]", separator,
                                   bootHeapStageName(static_cast<BootHeapStage>(i)),
                                   bootHeapResultName(r.result), static_cast<unsigned long>(r.atMs),
                                   static_cast<unsigned long>(r.internal.freeBytes),
                                   static_cast<unsigned long>(r.internal.largestBlock));
      separator = ",";
    }
    ok = ok && debugExportChunk(request, "],\"minima\":[");
    for (size_t i = 0; i < 2; ++i) {
      ok = ok && debugExportChunkf(request, buf, cap, "%s[%lu,%lu,%lu]", i ? "," : "",
                                   static_cast<unsigned long>(minima[i].samples),
                                   static_cast<unsigned long>(minima[i].free),
                                   static_cast<unsigned long>(minima[i].largest));
    }
    ok = ok && debugExportChunk(request, "]},");
  }

  const NvsDiagnosticSnapshot nvs = captureNvsDiagnostics();
  ok = ok && debugExportChunkf(
      request, buf, cap,
      "\"nvs\":{\"statsValid\":%s,\"statsErrorCode\":%ld,"
      "\"statsErrorName\":\"%s\",\"partitionBytes\":%lu,"
      "\"expectedPartitionBytes\":%lu,\"layoutExpected\":%s,"
      "\"usedEntries\":%lu,\"freeEntries\":%lu,"
      "\"availableEntries\":%lu,\"totalEntries\":%lu,"
      "\"namespaces\":%lu,\"failures\":%lu,"
      "\"flashIoLockTimeouts\":%lu,\"lastFailure\":",
      nvs.statsValid ? "true" : "false",
      static_cast<long>(nvs.statsErrorCode), nvsErrorName(nvs.statsErrorCode),
      static_cast<unsigned long>(nvs.partitionBytes),
      static_cast<unsigned long>(EXPECTED_NVS_PARTITION_BYTES),
      nvs.partitionBytes == EXPECTED_NVS_PARTITION_BYTES ? "true" : "false",
      static_cast<unsigned long>(nvs.usedEntries),
      static_cast<unsigned long>(nvs.freeEntries),
      static_cast<unsigned long>(nvs.availableEntries),
      static_cast<unsigned long>(nvs.totalEntries),
      static_cast<unsigned long>(nvs.namespaceCount),
      static_cast<unsigned long>(nvs.failureCount),
      static_cast<unsigned long>(flashIoLockTimeouts()));
  if (ok && nvs.lastFailure.present) {
    ok = debugExportChunkf(
        request, buf, cap,
        "{\"subsystem\":\"%s\",\"operation\":\"%s\","
        "\"errorCode\":%ld,\"errorName\":\"%s\","
        "\"requiredEntries\":%lu,\"availableEntriesAtFailure\":%lu}},",
        nvsSubsystemName(nvs.lastFailure.subsystem),
        nvsOperationName(nvs.lastFailure.operation),
        static_cast<long>(nvs.lastFailure.errorCode),
        nvsErrorName(nvs.lastFailure.errorCode),
        static_cast<unsigned long>(nvs.lastFailure.requiredEntries),
        static_cast<unsigned long>(nvs.lastFailure.availableEntriesAtFailure));
  } else if (ok) {
    ok = debugExportChunk(request, "null},");
  }

  size_t shotTotal = 0;
  work.statsCache.valid = false;  // Debug export borrows the same record array.
  if (self.callbacks_.copyShotRecords != nullptr) {
    shotTotal =
        self.callbacks_.copyShotRecords(work.statsCache.snapshot.records, SHOT_LOG_CAPACITY);
  }
  const size_t recent =
      shotTotal < DEBUG_EXPORT_SHOT_SUMMARY_LIMIT
          ? shotTotal
          : DEBUG_EXPORT_SHOT_SUMMARY_LIMIT;
  ok = ok &&
       debugExportChunkf(request, buf, cap,
                         "\"shotLogSummary\":{\"total\":%lu,\"recent\":[",
                         static_cast<unsigned long>(shotTotal));
  for (size_t index = 0; ok && index < recent; ++index) {
    const ShotLogRecord &record = work.statsCache.snapshot.records[index];
    ok = debugExportChunkf(
        request, buf, cap,
        "%s{\"id\":%lu,\"bootId\":%lu,\"durationDs\":%u,\"goalWeightG\":%.1f,"
        "\"stopDetail\":\"%s\"}",
        index == 0 ? "" : ",", static_cast<unsigned long>(record.id),
        static_cast<unsigned long>(record.bootId),
        static_cast<unsigned>(record.durationDs),
        static_cast<double>(goalWeightDgToGrams(record.goalWeightDg)),
        shotLogStopDetailName(
            static_cast<ShotLogStopDetail>(record.stopDetail)));
  }
  ok = ok && debugExportChunk(request, "]}}}");

  if (!ok) {
    self.unlockWorkBuf();
    return ESP_FAIL;
  }
  const esp_err_t finished = httpd_resp_send_chunk(request, nullptr, 0);
  self.unlockWorkBuf();
  return finished;
}


esp_err_t OpenBrewByWeightNetwork::shotsClearHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  return self.runConfirmedClear(
      request,
      "Stop the cycle, switch the physical activator OFF, and wait for Ready "
      "before clearing shot history.",
      "CLEAR_SHOT_LOG", "SHOT_LOG_CLEAR_NOT_CONFIRMED",
      "The shot-history clear was not explicitly confirmed.",
      self.callbacks_.clearShotLog, "SHOT_LOG_CLEAR_FAILED",
      "Shot history could not be cleared. Check Diagnostic → NVS for "
      "capacity and the last storage error.");
}

esp_err_t OpenBrewByWeightNetwork::lastShotClearHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  return self.runConfirmedClear(
      request,
      "Stop the cycle, switch the physical activator OFF, and wait for Ready "
      "before clearing the last shot.",
      "CLEAR_LAST_SHOT", "LAST_SHOT_CLEAR_NOT_CONFIRMED",
      "The last-shot clear was not explicitly confirmed.",
      self.callbacks_.clearLastShot, "LAST_SHOT_CLEAR_FAILED",
      "Last shot could not be cleared.");
}

esp_err_t OpenBrewByWeightNetwork::shotsDeleteHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  return self.runDeleteById(
      request,
      "Stop the cycle, switch the physical activator OFF, and wait for Ready "
      "before deleting shot records.",
      self.callbacks_.deleteShotRecord,
      "A non-zero shot id is required.", "SHOT_NOT_FOUND",
      "The requested shot record was not found.");
}

esp_err_t OpenBrewByWeightNetwork::shotsRateHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const ControlGateSnapshot status = self.controlGate();
  if (!self.historyMutationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     "Stop the cycle, switch the physical activator OFF, and wait for Ready before rating shots.");
  }

  const esp_err_t bodyStatus = self.lockJsonBody(
      request, "A bounded JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {"id", "lastShot", "rating"};
  uint8_t rating = 0;
  uint32_t shotId = 0;
  bool lastShot = false;
  const bool hasId = jsonFieldPresent(root, "id");
  const bool hasLastShot = jsonFieldPresent(root, "lastShot");
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 3) &&
      jsonUint8(root, "rating", rating) && rating <= SHOT_LOG_RATING_MAX &&
      hasId != hasLastShot &&
      (!hasId || (jsonUint32(root, "id", shotId) && shotId != 0)) &&
      (!hasLastShot || (jsonBoolean(root, "lastShot", lastShot) && lastShot));
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD",
                     "A rating 0-5 and either a shot id or lastShot is required.");
  }

  if (hasLastShot) {
    if (self.callbacks_.rateLastShot == nullptr ||
        !self.callbacks_.rateLastShot(rating)) {
      return sendError(request, STATUS_NOT_FOUND, "LAST_SHOT_NOT_FOUND",
                       "There is no last shot to rate.");
    }
  } else if (self.callbacks_.rateShotRecord == nullptr ||
             !self.callbacks_.rateShotRecord(shotId, rating)) {
    return sendError(request, STATUS_NOT_FOUND, "SHOT_NOT_FOUND",
                     "The requested shot record was not found.");
  }
  char json[48] = {};
  snprintf(json, sizeof(json), "{\"rated\":true,\"rating\":%u}",
           static_cast<unsigned>(rating));
  return sendJson(request, STATUS_OK, json);
}

esp_err_t OpenBrewByWeightNetwork::historyClearHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  return self.runConfirmedClear(
      request,
      "Stop the cycle, switch the physical activator OFF, and wait for Ready "
      "before clearing the activation history.",
      "CLEAR_HISTORY", "HISTORY_CLEAR_NOT_CONFIRMED",
      "The activation-history clear was not explicitly confirmed.",
      self.callbacks_.clearHistoryLog, "HISTORY_CLEAR_FAILED",
      "Activation history could not be cleared. Check the last storage "
      "error in Diagnostic.");
}

esp_err_t OpenBrewByWeightNetwork::historyDeleteHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  return self.runDeleteById(
      request,
      "Stop the cycle, switch the physical activator OFF, and wait for Ready "
      "before deleting history records.",
      self.callbacks_.deleteHistoryRecord,
      "A non-zero history record id is required.", "HISTORY_RECORD_NOT_FOUND",
      "The requested history record was not found.");
}

// Verbose audit hunt session toggle: marked DEBUG codes join the latch for
// thirty minutes. Needs a synced clock (the deadline is wall-clock based).
esp_err_t OpenBrewByWeightNetwork::auditVerboseHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  bool enabled = false;
  static const char *const fields[] = {"enabled"};
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonBoolean(root, "enabled", enabled);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_AUDIT_VERBOSE",
                     "enabled must be a boolean.");
  }
  if (self.callbacks_.setAuditVerbose == nullptr ||
      !self.callbacks_.setAuditVerbose(enabled)) {
    return sendError(request, STATUS_CONFLICT, "AUDIT_VERBOSE_UNAVAILABLE",
                     "The wall clock is not synchronized yet.");
  }
  return sendJson(request, STATUS_OK, "{\"accepted\":true}");
}

esp_err_t OpenBrewByWeightNetwork::taskProfilerHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, "A JSON request is required.");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  bool enabled = false;
  static const char *const fields[] = {"enabled"};
  const bool parsed =
      root != nullptr && jsonHasOnlyUniqueFields(root, fields, 1) &&
      jsonBoolean(root, "enabled", enabled);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_PROFILER_CONFIG",
                     "enabled must be a boolean.");
  }
  WebCommand command;
  command.type = enabled ? WebCommandType::TASK_PROFILER_START
                         : WebCommandType::TASK_PROFILER_STOP;
  command.requestId = self.allocateRequestId();
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     "Task profiler state was not changed.");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::loopMaxResetHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (self.callbacks_.requestLoopMaxReset == nullptr) {
    return sendError(request, STATUS_UNAVAILABLE, "LOOP_TIMING_UNAVAILABLE",
                     "Loop timing could not be reset.");
  }
  self.callbacks_.requestLoopMaxReset();
  return sendJson(request, STATUS_OK, "{\"reset\":true}");
}

esp_err_t OpenBrewByWeightNetwork::scaleGapMaxResetHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (self.callbacks_.requestScaleGapMaxReset == nullptr) {
    return sendError(request, STATUS_UNAVAILABLE, "SCALE_TIMING_UNAVAILABLE",
                     "Scale gap maximum could not be reset.");
  }
  self.callbacks_.requestScaleGapMaxReset();
  return sendJson(request, STATUS_OK, "{\"reset\":true}");
}

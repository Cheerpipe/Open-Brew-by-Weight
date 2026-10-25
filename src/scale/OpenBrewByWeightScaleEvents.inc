// Scale connection and weight packets
// ---------------------------------------------------------------------------

AlertChannelContext currentAlertChannelContext() {
  AlertChannelContext ctx;
  ctx.channel = currentAlertOutputChannel();
  ctx.soundAlertsEnabled = soundAlertsEnabled();
  ctx.buzzerSupportEnabled = BUZZER_SUPPORT_ENABLED;
  ctx.buzzerReady = BUZZER_SUPPORT_ENABLED && localBuzzer.ready;
  ctx.scaleAvailable = scaleAvailable();
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  ctx.scaleSupportsIndependentBeep =
      link.features.has(ScaleFeatureIndependentBeep);
  ctx.scaleSupportsCommandFeedback =
      link.features.has(ScaleFeatureCommandAudibleFeedback);
  return ctx;
}

bool playLocalAlertTone(const BuzzerToneCommand &cmd, bool honorMute) {
  if (honorMute && !soundAlertsEnabled()) {
    return false;
  }
  if (!BUZZER_SUPPORT_ENABLED || !localBuzzer.ready || !cmd.valid) {
    return false;
  }
  return localBuzzer.requestTone(cmd);
}

bool startPulseTrain(BuzzerPattern pattern, uint32_t durationMs) {
  if (!BUZZER_SUPPORT_ENABLED || !localBuzzer.ready ||
      !buzzerPatternIsPulseTrain(pattern)) {
    return false;
  }
  return localBuzzer.request(pattern, durationMs);
}

bool startExtendedPulseTrain(uint32_t durationMs) {
  BuzzerToneCommand cmd =
      deriveBuzzerTone(AlertEvent::EXTENDED_PULSE, session.slowExtractionExtended,
                       runtimeConfig.buzzerExtendedPulseRate,
                       runtimeConfig.buzzerSlowExtendedPulseRate);
  if (!cmd.valid) {
    return false;
  }
  cmd.durationMs = durationMs;
  return playLocalAlertTone(cmd, false);
}

void stopPulseTrains() {
  localBuzzer.stopPulseTrains();
}

bool queueScaleIndependentAlert(AlertEvent event, uint32_t cycleId) {
  if (!scaleAvailable() ||
      !getScaleLinkSnapshot().features.has(ScaleFeatureIndependentBeep)) {
    return false;
  }
  switch (event) {
    case AlertEvent::FIRST_DROP:
      requestScaleBrewBeep(cycleId);
      return true;
    case AlertEvent::PADDLE_REMINDER:
      requestScalePaddleReturnReminderBeep();
      return true;
    case AlertEvent::COMPLETION_EXTRA:
      requestScaleCompletionBeep();
      return true;
    default:
      return false;
  }
}

bool dispatchAlert(AlertKind kind, AlertEvent event, uint32_t cycleId,
                   bool commandAttempted, bool writeSucceeded,
                   bool commandFeedbackExpected) {
  AlertChannelContext ctx = currentAlertChannelContext();
  ctx.commandAttempted = commandAttempted;
  ctx.writeSucceeded = writeSucceeded;
  ctx.commandFeedbackExpected = commandFeedbackExpected;
  const AlertSink sink = selectAlertSink(kind, event, ctx);
  if (sink == AlertSink::None) {
    return false;
  }
  if (sink == AlertSink::Scale) {
    return queueScaleIndependentAlert(event, cycleId);
  }
  const BuzzerToneCommand tone =
      deriveBuzzerTone(event, session.slowExtractionExtended,
                       runtimeConfig.buzzerExtendedPulseRate,
                       runtimeConfig.buzzerSlowExtendedPulseRate);
  return playLocalAlertTone(tone, kind != AlertKind::Recovery);
}

// Independent / multi-tone alerts: first drop, paddle, completion, triples.
bool emitAlert(AlertEvent event, uint32_t cycleId) {
  return dispatchAlert(AlertKind::Independent, event, cycleId, false, false,
                       false);
}

bool commandAlertUsesBuzzer() {
  AlertChannelContext ctx = currentAlertChannelContext();
  return selectAlertSink(AlertKind::CommandImmediate, AlertEvent::START_TIMER,
                         ctx) == AlertSink::Buzzer;
}

// Tare/start/stop replacement sounds: fire at the local circuit/paddle/retare
// moment when the buzzer is the routed output. Never wait for BLE.
void emitImmediateCommandAlertIfBuzzer(AlertEvent event) {
  (void)dispatchAlert(AlertKind::CommandImmediate, event, 0, false, false,
                      false);
}

// Shot start/stop follow the relay, not the scale timer or Output channel.
// Both preempt so the circuit edge is not stuck behind a warning, echo, or pulse.
bool emitCircuitCycleAlert(AlertEvent event, bool preempt) {
  if (!soundAlertsEnabled()) {
    return false;
  }
  if (BUZZER_SUPPORT_ENABLED && localBuzzer.ready) {
    if (preempt) {
      localBuzzer.stopAll();
    }
    const BuzzerToneCommand tone =
        deriveBuzzerTone(event, session.slowExtractionExtended,
                         runtimeConfig.buzzerExtendedPulseRate,
                         runtimeConfig.buzzerSlowExtendedPulseRate);
    return playLocalAlertTone(tone, true);
  }
  if (event == AlertEvent::COMPLETION_EXTRA) {
    return emitAlert(event);
  }
  return false;
}

void playRecoveryCue(BuzzerCue cue) {
  AlertChannelContext ctx = currentAlertChannelContext();
  if (selectAlertSink(AlertKind::Recovery, AlertEvent::TARE, ctx) !=
      AlertSink::Buzzer) {
    return;
  }
  (void)playLocalAlertTone(deriveRecoveryTone(cue), false);
}

// BLE-result fallback only. Buzzer only already played at the local event.
// SCALE_PRIORITY still falls back here if the write missed, even if the
// scale dropped while the ATT write was in flight.
void emitCommandAlert(AlertEvent event, bool commandAttempted,
                      bool writeSucceeded, bool commandFeedbackExpected) {
  (void)dispatchAlert(AlertKind::CommandFallback, event, 0, commandAttempted,
                      writeSucceeded, commandFeedbackExpected);
}

bool shotCompletionGetsLongBeep(EndReason reason) {
  switch (reason) {
    case EndReason::ACTIVATOR:
    case EndReason::SCALE_THRESHOLD:
    case EndReason::WEIGHT_ANOMALY:
    case EndReason::TOUCH_WEIGHT_FALLBACK:
    case EndReason::GLOBAL_LIMIT:
    case EndReason::CONFIGURED_WALL_LIMIT:
    case EndReason::WEB_STOP:
    case EndReason::PHYSICAL_OVERRIDE:
    case EndReason::WEB_HEARTBEAT_TIMEOUT:
    case EndReason::FAST_EXTRACTION_MAX_WEIGHT:
    case EndReason::FAST_EXTRACTION_MIN_TIME:
    case EndReason::SLOW_EXTRACTION_MAX_TIME:
    case EndReason::SLOW_EXTRACTION_MIN_WEIGHT:
    case EndReason::AUTO_TO_MANUAL_GUARD:
    case EndReason::CUP_REMOVED:
    case EndReason::RINSE_COMPLETE:
      return true;
    default:
      return false;
  }
}



void requestCompletionAlert() {
  if (!soundAlertsEnabled()) {
    return;
  }
  if (emitCircuitCycleAlert(AlertEvent::COMPLETION_EXTRA, true)) {
    scaleCompletionBeepScheduled = false;
    return;
  }
  if (BUZZER_SUPPORT_ENABLED) {
    scaleCompletionBeepScheduled = true;
    return;
  }
  scaleCompletionBeepScheduled = false;
}

void cancelScaleCompletionBeep() {
  scaleCompletionBeepScheduled = false;
  cancelScaleCompletionBeepMailbox();
}

void cancelOperationalAlerts() {
  scaleCompletionBeepScheduled = false;
  cancelOperationalScaleBeeps();
  if (localBuzzer.busy()) {
    localBuzzer.stopAll();
  }
}

void serviceExtendedPulseAlert() {
  const BuzzerToneCommand cmd = deriveBuzzerTone(
      AlertEvent::EXTENDED_PULSE, session.slowExtractionExtended,
      runtimeConfig.buzzerExtendedPulseRate,
      runtimeConfig.buzzerSlowExtendedPulseRate);
  const bool want =
      soundAlertsEnabled() && BUZZER_SUPPORT_ENABLED && localBuzzer.ready &&
      session.active &&
      (session.extractionExtended || session.slowExtractionExtended) &&
      cmd.valid &&
      currentAlertOutputChannel() != AlertOutputChannel::SCALE_ONLY;
  if (!want) {
    localBuzzer.stopExtendedPulse();
    return;
  }
  if (localBuzzer.playingExtendedPulse(cmd)) {
    return;
  }
  if (localBuzzer.busy()) {
    localBuzzer.stopExtendedPulse();
    if (localBuzzer.busy()) {
      return;
    }
  }
  emitAlert(AlertEvent::EXTENDED_PULSE, session.id);
}

void serviceScaleCompletionBeep() {
  if (!soundAlertsEnabled()) {
    cancelScaleCompletionBeep();
    return;
  }
  if (!scaleCompletionBeepScheduled) {
    return;
  }
  requestCompletionAlert();
}














void servicePreferredScaleMacPersistence() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  char mac[PREFERRED_SCALE_MAC_CAPACITY];
  char name[PREFERRED_SCALE_NAME_CAPACITY];
  ScaleHistoryEntry history[SCALE_HISTORY_CAPACITY];
  if (!scaleWorkerCopyPreferredIfDirty(mac, name, history)) {
    return;
  }
  const bool identityUnchanged =
      strncmp(persistedSettings.preferredScaleMac, mac,
              PREFERRED_SCALE_MAC_CAPACITY) == 0 &&
      strncmp(persistedSettings.preferredScaleName, name,
              PREFERRED_SCALE_NAME_CAPACITY) == 0 &&
      scaleHistoryIdentityEqual(persistedSettings.scaleHistory, history);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  const ScaleMacNvsAction action = decideScaleMacNvsAction(
      identityUnchanged, link.connecting,
      link.state == ScaleLinkState::CONNECTED);
  if (action == ScaleMacNvsAction::CLEAR_DIRTY) {
    scaleWorkerClearPreferredDirty(mac, name, history);
    return;
  }
  if (action == ScaleMacNvsAction::DEFER) {
    return;
  }
  bool persistBusy = false;
  settingsPersistMux.lock();
  persistBusy = settingsPersistInFlight || settingsPersistResultReady;
  settingsPersistMux.unlock();
  if (persistBusy) {
    return;
  }
  memcpy(persistedSettings.preferredScaleMac, mac,
         sizeof(persistedSettings.preferredScaleMac));
  memcpy(persistedSettings.preferredScaleName, name,
         sizeof(persistedSettings.preferredScaleName));
  memcpy(persistedSettings.scaleHistory, history,
         sizeof(persistedSettings.scaleHistory));
  if (!runtimePersistPending ||
      (runtimePersistReasonBits & RUNTIME_PERSIST_REASON_SCALE_MAC) == 0) {
    queueRuntimePersist(RUNTIME_PERSIST_REASON_SCALE_MAC);
  }
  networkManager.syncPreferredScale(mac, name);
  if (mac[0] == '\0') {
    serialTraceCategory(LogLevel::INFO, DebugCategory::SCALE,
                        "Preferred scale MAC cleared; persist queued");
  } else {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                         "Preferred scale MAC queued: %s — %s",
                         name[0] != '\0' ? name : "(unknown)", mac);
  }
#endif
}













void processScaleLinkTransitions() {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  serviceCupWeight(link);
  const bool connectedEdge =
      link.connectionGeneration != handledScaleConnectionGeneration;
  const bool disconnectedEdge =
      link.disconnectSequence != handledScaleDisconnectSequence;

  auto handleConnected = [&]() {
    requestActivePresetMachineTemperature();
    if (runtimeConfig.buzzerScaleConnectedBeep) {
      emitAlert(AlertEvent::SCALE_CONNECTED);
    }
  };
  auto handleDisconnected = [&]() {
    markShotCurveBreak();
    if (runtimeConfig.buzzerScaleLostBeep) {
      emitAlert(AlertEvent::SCALE_LOST);
    }
    // Cup presence is control-owned. A link loss invalidates the observation,
    // but only this task is allowed to mutate the detector.
    resetCupPresence();
  };

  // If both edges occurred between control slices, the final state determines
  // their order. Side effects are intentionally coalesced to one per edge kind;
  // the monotonic counters make the disconnect reset impossible to lose.
  if (connectedEdge && disconnectedEdge) {
    if (link.state == ScaleLinkState::CONNECTED) {
      handleDisconnected();
      handleConnected();
    } else {
      handleConnected();
      handleDisconnected();
    }
  } else if (connectedEdge) {
    handleConnected();
  } else if (disconnectedEdge) {
    handleDisconnected();
  }
  handledScaleConnectionGeneration = link.connectionGeneration;
  handledScaleDisconnectSequence = link.disconnectSequence;
}

void processScaleWorkerEvents() {
  captureLateTareCompletion();
  processScaleLinkTransitions();
  ScaleEvent event;
  size_t processed = 0;
  while (processed < SCALE_EVENT_QUEUE_LENGTH + 1) {
    bool receivedCritical = false;
    bool receivedWeight = false;
    // Every iteration blocks on this mutex; record how long the wait took
    // so a drained-events phase maximum can blame lock contention.
    const uint32_t criticalLockStartedUs = micros();
    scaleCriticalEventMux.lock();
    noteMutexWaitUs(scaleCriticalEventWaitLastUsStat(),
                    scaleCriticalEventWaitMaxUsStat(),
                    micros() - criticalLockStartedUs);
    if (scaleReferenceChangedEventPending) {
      event = scaleReferenceChangedEvent;
      scaleReferenceChangedEventPending = false;
      receivedCritical = true;
    } else if (scaleCriticalEventPending) {
      event = scaleCriticalEvent;
      scaleCriticalEventPending = false;
      receivedCritical = true;
    } else if (scaleTimerStartEventPending) {
      event = scaleTimerStartEvent;
      scaleTimerStartEventPending = false;
      receivedCritical = true;
    }
    if (!receivedCritical && scaleEventQueue != nullptr)
      receivedCritical = xQueueReceive(scaleEventQueue, &event, 0) == pdTRUE;
    if (!receivedCritical) {
      if (scaleWeightEventCount != 0) {
        event = scaleWeightEvents[scaleWeightEventHead];
        scaleWeightEventHead = (scaleWeightEventHead + 1U) %
                              SCALE_WEIGHT_EVENT_CAPACITY;
        --scaleWeightEventCount;
        scaleWeightEventPending = scaleWeightEventCount != 0;
        receivedWeight = true;
      }
    }
    scaleCriticalEventMux.unlock();
    if (!receivedCritical && !receivedWeight) {
      break;
    }
    ++processed;
    // A retained retare completion can outlive its queue/fallback event.
    if (receivedWeight) captureLateTareCompletion();
    const ScaleLinkSnapshot eventLink = getScaleLinkSnapshot();
    if (event.connectionGeneration != eventLink.connectionGeneration ||
        event.disconnectSequence != eventLink.disconnectSequence) {
      if (receivedWeight) markShotCurveBreak();
      session.touchStop = {};
      char detail[DEBUG_EVENT_TEXT_CAPACITY];
      snprintf(detail, sizeof(detail),
               "stale scale event rejected: %s reason=link_epoch "
               "gen(e/n)=%lu/%lu disc(e/n)=%lu/%lu",
               scaleEventTypeName(event.type),
               static_cast<unsigned long>(event.connectionGeneration),
               static_cast<unsigned long>(eventLink.connectionGeneration),
               static_cast<unsigned long>(event.disconnectSequence),
               static_cast<unsigned long>(eventLink.disconnectSequence));
      addDebugEventDetail(DebugCategory::SCALE,
                    DebugCode::SCALE_STALE_EVENT_REJECTED,
                    static_cast<int32_t>(event.connectionGeneration),
                    static_cast<int32_t>(eventLink.connectionGeneration), detail);
      continue;
    }
    switch (event.type) {
      case ScaleEventType::REFERENCE_CHANGED:
        scaleProfileNoteEvent(
            ScaleProfileEvent::REFERENCE_CHANGED, millis(),
            eventLink.connectionGeneration, 0, NAN,
            event.writeSucceeded ? 1 : 0, 0);
        markShotCurveBreak();
        session.touchStop = {};
        if (idleTare.requestId != 0) {
          idleTare.cupStillPresent = false;
          releaseIdleTareRequest(IdleTareReason::SAMPLE_GAP);
          resetCupPresence();
        }
        resetCupSampleEvidence();
        cupPresence.emptyAnchorG = NAN;
        cupPresence.weight.pendingId = 0;
        if (event.writeSucceeded) notifyCupPresenceTare();
        else markCupTareReferenceUncertain();
        break;
      case ScaleEventType::WEIGHT:
        if (session.active && event.captureSequence != 0 &&
            event.connectionGeneration == session.tareConnectionGeneration &&
            static_cast<int32_t>(event.captureSequence - session.tareCaptureBoundary) <= 0)
          break; // Exclude buffered evidence before publishing machine sense.
        if (!std::isfinite(event.weightG) ||
            fabsf(event.weightG) > MAX_PARSED_WEIGHT_G) {
          session.touchStop = {};
          resetCupSampleEvidence();
          serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                              "Invalid scale weight event ignored");
          rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_INVALID,
                            event.weightG);
          break;
        }
        {
          if (session.active &&
              static_cast<int32_t>(event.receivedAtMs -
                                   session.startedAtMs) < 0) {
            session.touchStop = {};
            char detail[DEBUG_EVENT_TEXT_CAPACITY];
            snprintf(detail, sizeof(detail),
                     "stale scale event rejected: weight reason=before cycle "
                     "sample_ms=%lu cycle_ms=%lu cycle=%lu",
                     static_cast<unsigned long>(event.receivedAtMs),
                     static_cast<unsigned long>(session.startedAtMs),
                     static_cast<unsigned long>(session.id));
            addDebugEventDetail(DebugCategory::SCALE,
                          DebugCode::SCALE_STALE_EVENT_REJECTED,
                          static_cast<int32_t>(event.connectionGeneration),
                          static_cast<int32_t>(eventLink.connectionGeneration),
                          detail);
            break;
          }
        }
        if (event.sampleDiscontinuity) {
          markShotCurveBreak();
          session.touchStop = {};
          resetCupSampleEvidence();
          idleTare.absentObserved = false;
          if (idleTare.requestId != 0 &&
              idleScaleTareStatus().phase == IdleTarePhase::QUEUED) {
            releaseIdleTareRequest(IdleTareReason::SAMPLE_GAP);
          }
        }
        observedWeight = event.weightG;
        observedWeightReceivedAtMs = event.receivedAtMs;
        observedWeightSequence = event.packetSequence;
        observedWeightConnectionGeneration = event.connectionGeneration;
        weightStreamState = fabsf(event.weightG) > MAX_AUTOMATION_WEIGHT_G
                                ? WeightStreamState::OVERLOAD
                                : WeightStreamState::FRESH;
        if (recordWeightSampleWithProvenance(
                event.weightG, event.receivedAtMs, event.packetSequence,
                event.connectionGeneration, event.captureSequence)) {
          currentWeight = event.weightG;
          currentWeightReceivedAtMs = event.receivedAtMs;
          currentWeightSequence = event.packetSequence;
          currentWeightConnectionGeneration = event.connectionGeneration;
          if (session.active && session.scaleName[0] == '\0')
            copyCString(session.scaleName, sizeof(session.scaleName),
                        eventLink.shotScaleName);
        }
        break;

      case ScaleEventType::TIMER_START_RESULT:
        finishCupWeight(event.cupWeightRequestId, event.tareSucceeded,
                        event.tareAttempted || event.discardedStaleConnection,
                        event.preTareWeightG);
        if (event.cycleId == session.id) {
          session.remoteTimerStarted = event.writeSucceeded;
          session.remoteTimerStartSettled = true;
          if (event.tareSucceeded) applyShotTareResult(event);
        }
        if (!event.discardedStaleConnection) {
          emitCommandAlert(event.usedCombinedTareStart ? AlertEvent::TARE_START
                                                       : AlertEvent::START_TIMER,
                           event.commandAttempted, event.writeSucceeded,
                           event.commandFeedbackExpected);
        }
        serialTraceCategoryf(LogLevel::DEBUG, DebugCategory::SCALE,
                             "Remote timer start write: %s",
                             event.writeSucceeded ? "successful" : "failed/skipped");
        addDebugEvent(DebugCategory::SCALE,
                      event.writeSucceeded ? DebugCode::SCALE_TIMER_START_OK
                                           : DebugCode::SCALE_TIMER_START_FAILED,
                      static_cast<int32_t>(event.cycleId));
        break;

      case ScaleEventType::TARE_RESULT:
        if (event.idleTareRequestId != 0) {
          // Durable worker status owns idle completion, never CycleSession.
          serviceIdleTare();
          break;
        }
        finishCupWeight(event.cupWeightRequestId, event.writeSucceeded,
                        event.commandAttempted || event.discardedStaleConnection,
                        event.preTareWeightG);
        if (event.writeSucceeded) applyShotTareResult(event);
        if (!event.discardedStaleConnection) {
          emitCommandAlert(AlertEvent::TARE, event.commandAttempted,
                           event.writeSucceeded, event.commandFeedbackExpected);
        }
        break;

      case ScaleEventType::TIMER_STOP_RESULT:
        if (event.cycleId == session.id) {
          session.stopTimerCommandQueued = false;
          session.timerStopResult =
              !event.commandAttempted
                  ? TimerStopResult::NOT_ATTEMPTED
                  : (event.writeSucceeded ? TimerStopResult::WRITE_SUCCEEDED
                                          : TimerStopResult::WRITE_FAILED);
        }
        if (!event.discardedStaleConnection) {
          emitCommandAlert(AlertEvent::STOP_TIMER, event.commandAttempted,
                           event.writeSucceeded, event.commandFeedbackExpected);
        }
        // Structured SCALE_TIMER_STOP_* events cover Serial when enabled.
        addDebugEvent(DebugCategory::SCALE,
                      event.writeSucceeded ? DebugCode::SCALE_TIMER_STOP_OK
                                           : DebugCode::SCALE_TIMER_STOP_FAILED,
                      static_cast<int32_t>(event.cycleId));
        break;
    }
  }
}

// ---------------------------------------------------------------------------

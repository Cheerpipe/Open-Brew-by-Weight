// Scale timer session
// ---------------------------------------------------------------------------

void requestRemoteTimerStop();

// A cycle's scale-timer stop waits for the scale clock to reach the shot time,
// then the configured extra delay.
struct PendingScaleTimerStop {
  bool pending = false;
  uint32_t targetMs = 0;
  uint32_t extraDelayMs = 0;
  uint32_t catchupDeadlineAtMs = 0;
  uint32_t extraDueAtMs = 0;
};
PendingScaleTimerStop pendingScaleTimerStop;

void completePendingScaleTimerStop() {
  pendingScaleTimerStop = PendingScaleTimerStop{};
  requestRemoteTimerStop();
}

void flushPendingScaleTimerStopNow() {
  if (pendingScaleTimerStop.pending) completePendingScaleTimerStop();
}

void armScaleTimerStopExtraDelay(uint32_t extraDelayMs) {
  if (extraDelayMs == 0U) {
    completePendingScaleTimerStop();
    return;
  }
  pendingScaleTimerStop.pending = true;
  pendingScaleTimerStop.extraDueAtMs = millis() + extraDelayMs;
}

void servicePendingScaleTimerStop() {
  if (!pendingScaleTimerStop.pending) return;
  const uint32_t nowMs = millis();
  if (pendingScaleTimerStop.extraDueAtMs != 0U) {
    if (static_cast<int32_t>(nowMs - pendingScaleTimerStop.extraDueAtMs) >= 0 ||
        !scaleAvailable()) {
      completePendingScaleTimerStop();
    }
    return;
  }
  if (!scaleAvailable()) {
    completePendingScaleTimerStop();
    return;
  }
  const bool timedOut =
      static_cast<int32_t>(nowMs -
                           pendingScaleTimerStop.catchupDeadlineAtMs) >= 0;
  if (!session.remoteTimerStartSettled && !timedOut) return;
  const uint32_t extraDelayMs = pendingScaleTimerStop.extraDelayMs;
  if (session.remoteTimerStartSettled && !session.remoteTimerStarted) {
    armScaleTimerStopExtraDelay(extraDelayMs);
    return;
  }
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (timedOut || !link.timerValid ||
      link.timerMs >= pendingScaleTimerStop.targetMs) {
    armScaleTimerStopExtraDelay(extraDelayMs);
  }
}

void scheduleScaleTimerStopAfterCycle(uint32_t internalElapsedMs) {
  if (!session.timerStartCommandQueued || session.stopTimerRequested) return;
  pendingScaleTimerStop = PendingScaleTimerStop{};
  pendingScaleTimerStop.pending = true;
  // The scale displays whole seconds.
  pendingScaleTimerStop.targetMs = (internalElapsedMs / 1000U) * 1000U;
  pendingScaleTimerStop.extraDelayMs = session.config.scaleTimerStopExtraDelayMs;
  pendingScaleTimerStop.catchupDeadlineAtMs =
      millis() + MAX_SCALE_TIMER_STOP_CATCHUP_MS;
  servicePendingScaleTimerStop();
}

uint32_t nextCupWeightRequestId = 0;
uint32_t allocateCupWeightRequestId() {
  if (++nextCupWeightRequestId == 0) ++nextCupWeightRequestId;
  return nextCupWeightRequestId;
}
void trackCupWeightTare(uint32_t requestId);

bool requestRemoteTimerStart() {
  ScaleCommand command;
  command.type = ScaleCommandType::START_TIMER_AND_TARE;
  command.cycleId = session.id;
  command.autoTare = session.config.autoTare;
  if (command.autoTare) command.cupWeightRequestId = allocateCupWeightRequestId();
  command.canTareStartTimer = session.config.canTareStartTimer;
  command.commandFeedbackExpected =
      getScaleLinkSnapshot().features.has(ScaleFeatureCommandAudibleFeedback);
  session.timerStartCommandQueued = enqueueScaleCommand(command);
  if (session.timerStartCommandQueued && command.autoTare) {
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_REQUEST, millis(),
                          scaleConnectionGenerationNow(), 0, NAN,
                          command.cupWeightRequestId,
                          static_cast<uint32_t>(
                              ScaleProfileTareOrigin::SHOT_START));
    trackCupWeightTare(command.cupWeightRequestId);
  }
  return session.timerStartCommandQueued;
}

bool requestRemoteRetare() {
  if (!session.config.autoTare) {
    return false;
  }
  ScaleCommand command;
  command.type = ScaleCommandType::TARE_ONLY;
  command.cycleId = session.id;
  command.autoTare = true;
  command.cupWeightRequestId = allocateCupWeightRequestId();
  command.commandFeedbackExpected =
      getScaleLinkSnapshot().features.has(ScaleFeatureCommandAudibleFeedback);
  if (!enqueueScaleCommand(command)) return false;
  scaleProfileNoteEvent(ScaleProfileEvent::TARE_REQUEST, millis(),
                        scaleConnectionGenerationNow(), 0, NAN,
                        command.cupWeightRequestId,
                        static_cast<uint32_t>(ScaleProfileTareOrigin::RETARE));
  session.retareRequestId = command.cupWeightRequestId;
  trackCupWeightTare(command.cupWeightRequestId);
  return true;
}

bool tryQueueRemoteTimerStop() {
  if (!session.stopTimerRequested || session.stopTimerCommandQueued ||
      session.timerStopResult == TimerStopResult::WRITE_SUCCEEDED ||
      session.stopTimerAttempts >= SCALE_STOP_MAX_ATTEMPTS ||
      static_cast<int32_t>(millis() - session.stopTimerRetryDeadlineMs) >= 0 ||
      !scaleAvailable()) {
    return false;
  }
  ScaleCommand command;
  command.type = ScaleCommandType::STOP_TIMER;
  command.cycleId = session.id;
  command.commandFeedbackExpected =
      getScaleLinkSnapshot().features.has(ScaleFeatureCommandAudibleFeedback);
  ++session.stopTimerAttempts;
  session.stopTimerLastAttemptMs = millis();
  if (!enqueueScaleCommand(command, session.remoteTimerStartSettled)) {
    session.timerStopResult = TimerStopResult::NOT_ATTEMPTED;
    return false;
  } else {
    session.timerStopResult = TimerStopResult::PENDING;
    session.stopTimerCommandQueued = true;
    serialTraceCategoryf(LogLevel::DEBUG, DebugCategory::SCALE,
                         "Remote timer stop queued for cycle %lu",
                         static_cast<unsigned long>(session.id));
    return true;
  }
}

void requestRemoteTimerStop() {
  if (!session.timerStartCommandQueued || session.stopTimerRequested) {
    return;
  }

  session.stopTimerRequested = true;
  session.stopTimerRetryDeadlineMs = millis() + SCALE_STOP_RETRY_WINDOW_MS;
  if (!tryQueueRemoteTimerStop()) {
    session.timerStopResult = TimerStopResult::NOT_ATTEMPTED;
    serialTraceCategoryf(LogLevel::DEBUG, DebugCategory::SCALE,
                         "Remote timer stop deferred for cycle %lu",
                         static_cast<unsigned long>(session.id));
  }
}

void serviceRemoteTimerStopRetry() {
  if (!session.stopTimerRequested || session.stopTimerCommandQueued ||
      session.timerStopResult == TimerStopResult::WRITE_SUCCEEDED ||
      session.stopTimerAttempts >= SCALE_STOP_MAX_ATTEMPTS ||
      static_cast<int32_t>(millis() - session.stopTimerRetryDeadlineMs) >= 0) {
    return;
  }
  if (session.stopTimerAttempts > 0 &&
      elapsedMs(session.stopTimerLastAttemptMs) <
          SCALE_STOP_RETRY_INTERVAL_MS) {
    return;
  }
  (void)tryQueueRemoteTimerStop();
}
// ---------------------------------------------------------------------------
// Brew policy + scale sense + cup (domain headers). Orchestrator glue follows.
// These layers stay decoupled: no paddle/momentary internals, no direct calls
// between brew ↔ cup ↔ scale — this file applies their effects.
// ---------------------------------------------------------------------------

void notifyWebhookFirstDrop(const FirstFlowObservation &observation);

void profileControlState(uint32_t captureSequence = 0, uint32_t evidenceConnection = 0);
// Observe all exits of sample/timer handlers, including early rejection. The
// callback only reads control-owned scalars; it never services control logic.
struct ProfileControlObservation {
  uint32_t captureSequence;
  uint32_t connection;
  explicit ProfileControlObservation(uint32_t sequence = 0, uint32_t generation = 0)
      : captureSequence(sequence), connection(generation) {
    profileControlState();
  }
  ~ProfileControlObservation() { profileControlState(captureSequence, connection); }
};

#include "OpenBrewByWeightCupPresence.h"
#include "OpenBrewByWeightBrew.h"
#include "OpenBrewByWeightRinse.h"
#include "OpenBrewByWeightScaleSense.h"
void serviceCupWeight(const ScaleLinkSnapshot &link = getScaleLinkSnapshot()) {
  auto &mass = cupPresence.weight;
  const uint32_t droppedEvents = scaleWorkerDroppedEventCount();
  if (droppedEvents != mass.droppedEvents) {
    invalidateCupWeight();
    mass.sampleSequence = 0;
    mass.droppedEvents = droppedEvents;
  }
  if (!scaleLinkAvailable(link) ||
      mass.connectionGeneration != link.connectionGeneration ||
      mass.sampleSequence == 0 ||
      elapsedMs(mass.sampleAtMs) > MAX_AUTOMATION_WEIGHT_AGE_MS ||
      !cupPresenceIsKnown()) invalidateCupWeight();
  if (mass.pendingId != 0 && elapsedMs(mass.pendingAtMs) >
      SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs) {
    invalidateCupWeight();
    cupPresence.emptyAnchorG = NAN;
    mass.pendingId = 0;
  }
}
bool observeCupWeight(float weight, uint32_t atMs, uint32_t sequence,
                      uint32_t generation) {
  serviceCupWeight();
  auto &mass = cupPresence.weight;
  const uint32_t currentGeneration = scaleConnectionGenerationNow();
  if (mass.settingsGeneration != cupSettingsGeneration) {
    if (cupPresence.placementId == 0) invalidateCupWeight();
    mass.unloadSamples = 0;
    mass.unloadQualified = false;
    resetCupPlaceStabilityStreak();
    mass.settingsGeneration = cupSettingsGeneration;
  }
  if (generation == 0) generation = currentGeneration;
  if (!std::isfinite(weight) || fabsf(weight) > MAX_PARSED_WEIGHT_G || generation != currentGeneration ||
      elapsedMs(atMs) > MAX_AUTOMATION_WEIGHT_AGE_MS ||
      (mass.sampleSequence != 0 && mass.connectionGeneration == generation &&
       (static_cast<int32_t>(sequence - mass.sampleSequence) <= 0 ||
        static_cast<int32_t>(atMs - mass.sampleAtMs) < 0))) {
    resetCupSampleEvidence();
    approveScaleTareSample(ScaleTareSample{});
    return false;
  }
  if (mass.sampleSequence != 0 && atMs - mass.sampleAtMs >
      runtimeConfig.retareStabilityMaxGapMs) invalidateCupWeight();
  mass.sampleAtMs = atMs;
  mass.sampleSequence = sequence;
  mass.connectionGeneration = generation;
  approveScaleTareSample(ScaleTareSample{weight, atMs, sequence, generation});
  return true;
}
void trackCupWeightTare(uint32_t requestId) {
  serviceCupWeight();
  auto &mass = cupPresence.weight;
  if (mass.pendingId != 0) {
    invalidateCupWeight();
    cupPresence.emptyAnchorG = NAN;
  }
  mass.pendingId = requestId;
  mass.pendingAtMs = millis();
  mass.pendingPlacementId = cupPresencePlacementId();
  mass.pendingEmptyAnchorG = mass.sampleSequence != 0 &&
      elapsedMs(mass.sampleAtMs) <= MAX_AUTOMATION_WEIGHT_AGE_MS &&
      mass.connectionGeneration == scaleConnectionGenerationNow()
      ? cupPresence.emptyAnchorG : NAN;
  mass.emptyValid = false;
  mass.emptySamples = 0;
  mass.pendingValid = mass.valid && cupPresenceState() == CupPresenceState::PRESENT &&
      cupPresenceIsKnown() && mass.sampleSequence != 0 &&
      scaleAvailable() && mass.connectionGeneration ==
          scaleConnectionGenerationNow() &&
      elapsedMs(mass.sampleAtMs) <= MAX_AUTOMATION_WEIGHT_AGE_MS;
}
void finishCupWeight(uint32_t requestId, bool succeeded, bool uncertain,
                     float preTareWeightG = NAN) {
  serviceCupWeight();
  auto &mass = cupPresence.weight;
  if (requestId == 0 || requestId != mass.pendingId) return;
  if (succeeded) {
    cupPresence.emptyAnchorG = mass.pendingPlacementId == cupPresencePlacementId()
        ? mass.pendingEmptyAnchorG - preTareWeightG : NAN;
    mass.valid = mass.pendingValid && std::isfinite(preTareWeightG) &&
        cupPresenceState() == CupPresenceState::PRESENT &&
        mass.pendingPlacementId == cupPresencePlacementId();
    if (std::isfinite(cupPresence.emptyAnchorG)) {
      scaleProfileNoteEvent(
          ScaleProfileEvent::CUP_ANCHOR_CHANGED, millis(), 0, 0, NAN,
          mass.pendingPlacementId,
          static_cast<uint32_t>(weightToCentigrams(cupPresence.emptyAnchorG)));
    }
  } else if (uncertain) {
    invalidateCupWeight();
    cupPresence.emptyAnchorG = NAN;
  }
  mass.pendingId = 0;
  mass.pendingValid = false;
}
#include "OpenBrewByWeightIdleTare.inc"

uint32_t webhookUnixSecAt(uint32_t occurredAtMs) {
  if (!g_wallClock.synced()) return 0U;
  const uint32_t nowMs = millis();
  const uint32_t nowUtcSec = g_wallClock.nowUtcSec(nowMs);
  const uint32_t ageSec = static_cast<uint32_t>(nowMs - occurredAtMs) / 1000U;
  return nowUtcSec >= ageSec ? nowUtcSec - ageSec : 0U;
}

WebhookEvent baseWebhookEvent(WebhookEventType type, uint32_t cycleId,
                              uint32_t occurredAtMs = millis()) {
  WebhookEvent event;
  event.type = type;
  event.cycleId = cycleId;
  event.uptimeMs = occurredAtMs;
  event.unixSec = webhookUnixSecAt(occurredAtMs);
  event.bootId = currentBootId();
  return event;
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
void queueWebhookBrewStart() {
  if (!webhookBrewStartPending) return;
  webhookBrewStartPending = false;
  const WebhookConfig config = networkManager.webhookConfig();
  if (!config.enabled || !config.brewState) return;
  const uint32_t startedAtMs = session.circuitClosedAtMs != 0U
                                   ? session.circuitClosedAtMs
                                   : session.startedAtMs;
  WebhookEvent event = baseWebhookEvent(WebhookEventType::BREWING, session.id,
                                        startedAtMs);
  event.targetWeightG = goalWeightDgToGrams(session.config.goalWeightDg);
  event.presetId = session.activePresetId;
  copyCString(event.presetName, sizeof(event.presetName), session.activePresetName);
  (void)networkManager.enqueueWebhook(event);
}

void serviceWebhookBrewStart() {
  if (!webhookBrewStartPending || !session.active) return;
  const uint32_t ageMs = elapsedMs(session.startedAtMs);
  if ((ageMs >= WEBHOOK_START_MIN_DELAY_MS && currentWeightIsFresh() &&
       currentWeightSequence != session.weightSequenceAtStart) ||
      ageMs >= WEBHOOK_START_MAX_DELAY_MS) {
    queueWebhookBrewStart();
  }
}

void notifyWebhookFirstDrop(const FirstFlowObservation &observation) {
  queueWebhookBrewStart();
  const WebhookConfig config = networkManager.webhookConfig();
  if (!config.enabled || !config.firstDrop) return;
  WebhookEvent event = baseWebhookEvent(WebhookEventType::FIRST_DROP, session.id,
                                        observation.atMs);
  const uint32_t anchor = session.circuitClosedAtMs != 0
                              ? session.circuitClosedAtMs
                              : session.startedAtMs;
  event.firstDropValid = static_cast<int32_t>(observation.atMs - anchor) >= 0;
  event.firstDropMs = event.firstDropValid ? observation.atMs - anchor : 0;
  event.weightValid = std::isfinite(observation.weightG);
  event.weightG = event.weightValid ? observation.weightG : 0.0f;
  event.targetWeightG = goalWeightDgToGrams(session.config.goalWeightDg);
  event.presetId = session.activePresetId;
  copyCString(event.presetName, sizeof(event.presetName), session.activePresetName);
  (void)networkManager.enqueueWebhook(event);
}
#else
void notifyWebhookFirstDrop(const FirstFlowObservation &observation) {
  (void)observation;
  ++hostFirstDropWebhookCount;
}
void serviceWebhookBrewStart() {}
#endif

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#define OPEN_BREW_BY_WEIGHT_SCALE_WORKER_IN_ORCHESTRATOR
#include "OpenBrewByWeightScaleWorker.cpp"
#endif

// Called by the control publisher. Worker locks are acquired before the
// publication lock; diagnostic readers only see the committed scalar copy.
CupTareDiagnostics captureCupTareDiagnostics() {
  serviceCupWeight();
  CupTareDiagnostics out;
  out.weightG = cupPresence.weight.weightG;
  out.weightValid = cupPresence.weight.valid;
  out.accessoryState = idleAccessoryState();
  out.accessoryG = idleTare.knownAccessoryMassG;
  const IdleTareStatus status = idleScaleTareStatus();
  out.cupState = static_cast<uint8_t>(cupPresenceState());
  out.holdTransitions = cupPresence.holdTransitions;
  out.inNegativeHole = cupPresence.inNegativeHole;
  out.tared = cupPresenceIsTared();
  out.referenceKnown = cupPresenceIsKnown();
  out.emptyReferenceBlocked = !session.active &&
      cupPresenceState() == CupPresenceState::ABSENT && cupPresence.inNegativeHole &&
      !std::isfinite(cupPresence.emptyAnchorG) && !cupPresence.holdTransitions &&
      cupPresence.weight.pendingId == 0 &&
      idleTare.lastWeightG >= cupPresence.holeWeightG + runtimeConfig.minimumCupWeightG;
  out.absentObserved = idleTare.absentObserved;
  out.removedConfirmations = cupPresence.removedConfirmations;
  out.stabilitySamples = cupPresence.placeStabilitySamples;
  out.phase = static_cast<uint8_t>(status.phase);
  out.eligibilityReason = static_cast<uint8_t>(idleTare.eligibilityReason);
  out.lastReason = static_cast<uint8_t>(idleTare.lastReason);
  out.holeWeightG = cupPresence.holeWeightG;
  out.candidateWeightG = cupPresence.placeCandidateWeightG;
  out.occupiedReferenceG = cupPresenceReferenceG();
  out.requestMinimumG = idleTare.requestMinimumG;
  out.requestMaximumG = idleTare.requestMaximumG;
  out.lastWeightG = idleTare.lastWeightG;
  out.anchorDeltaG = !session.active && cupPresenceState() == CupPresenceState::ABSENT &&
      !cupPresence.weight.emptyValid && std::isfinite(cupPresence.emptyAnchorG)
      ? idleTare.lastWeightG - cupPresence.emptyAnchorG : NAN;
  out.anchorWaitMs = std::isfinite(out.anchorDeltaG) && cupPresence.weight.offAnchorSamples != 0
      ? millis() - cupPresence.weight.offAnchorStartedAtMs : 0;
  out.placementId = cupPresencePlacementId();
  out.requestPlacementId = idleTare.placementId;
  out.requestId = idleTare.requestId;
  out.connectionGeneration = idleTare.connectionGeneration;
  out.configRevision = idleTare.configRevision;
  out.requestedAtMs = idleTare.requestedAtMs;
  out.startedAtMs = idleTare.requestId != 0 ? status.startedAtMs : idleTare.lastStartedAtMs;
  out.writtenAtMs = idleTare.requestId != 0 ? status.writtenAtMs : idleTare.lastWrittenAtMs;
  out.effectAtMs = idleTare.effectAtMs;
  out.captureBoundary = idleTare.requestId != 0 ? status.captureBoundary
                                               : idleTare.lastCaptureBoundary;
  out.lastSampleAtMs = idleTare.lastSampleAtMs;
  out.lastPacketSequence = idleTare.lastPacketSequence;
  out.lastCaptureSequence = idleTare.lastCaptureSequence;
  out.lastTerminalRequestId = idleTare.lastTerminalRequestId;
  out.lastTerminalAtMs = idleTare.lastTerminalAtMs;
  out.rejectedSamples = idleTare.rejectedSamples;
  scaleCriticalEventMux.lock();
  out.droppedSamples = scaleWeightEventDrops;
  scaleCriticalEventMux.unlock();
  return out;
}

void copyDebugExportExtras(DebugExportExtras &out,
                           const ControlStatusSnapshot &control) {
  out = DebugExportExtras{};
  if (session.active) {
    out.sessionActive = true;
    out.sessionAutomaticEnabled = session.automaticEnabled;
    out.sessionBbwProtectionEnabled = session.bbwProtectionEnabled;
    out.sessionBbwProtectionEnded = session.bbwProtectionEnded;
    out.sessionStartedWithScale = session.startedWithScale;
    out.sessionScaleWasLost = session.scaleWasLost;
    out.sessionCupRemovedPending = session.cupRemovedPending;
    out.sessionExtractionExtended = session.extractionExtended;
    out.sessionSlowExtractionExtended = session.slowExtractionExtended;
    out.sessionTargetReachedEarly = session.targetReachedEarly;
    out.sessionAutoToManualGuardArmed = session.autoToManualGuardArmed;
    out.sessionAutoToManualGuardEnforced = session.autoToManualGuardEnforced;
    out.sessionAccidentalTouchHolding = session.accidentalTouchHolding;
    out.sessionAccidentalTouchPhase =
        static_cast<uint8_t>(session.accidentalTouchPhase);
    out.sessionAccidentalTouchClass =
        static_cast<uint8_t>(session.accidentalTouchClass);
    out.sessionAccidentalTouchPendingCount = session.accidentalTouchPendingCount;
    out.sessionActivePresetId = session.activePresetId;
    out.sessionWeightControlState =
        static_cast<uint8_t>(session.weightControlState);
    out.sessionId = session.id;
    out.sessionStartedAtMs = session.startedAtMs;
    out.sessionFirstDropMs = session.firstDropMs;
    out.sessionAutoToManualGuardDeadlineAtMs =
        session.autoToManualGuardDeadlineAtMs;
    out.sessionTargetReachedAtMs = session.targetReachedAtMs;
  }
  {
    const CupTareDiagnostics &cup = control.cupTare;
    out.cupTare = cup;
    out.cupState = cup.cupState;
    out.cupHoldTransitions = cup.holdTransitions;
    out.cupInNegativeHole = cup.inNegativeHole;
    out.cupRemovedConfirmations = cup.removedConfirmations;
    out.cupPlaceStabilitySamples = cup.stabilitySamples;
    out.cupHoleWeightG = cup.holeWeightG;
    out.cupPlaceCandidateWeightG = cup.candidateWeightG;
  }
  {
    const ScaleLinkSnapshot link = getScaleLinkSnapshot();
    out.scaleLinkState = static_cast<uint8_t>(link.state);
    out.scaleDisconnectSequence = link.disconnectSequence;
    out.scaleConnectionGeneration = link.connectionGeneration;
    out.scalePacketSequence = link.packetSequence;
    out.scaleWorkerProgressAtMs = link.workerProgressAtMs;
    out.scaleTimerValid = link.timerValid;
    out.scaleTimerMs = link.timerMs;
    out.scaleTimerAgeMs = link.timerAgeMs;
    copyCString(out.scaleProtocolName, sizeof(out.scaleProtocolName),
                link.protocolName);
  }
  out.relay = getRelaySafetySnapshot();
  out.rawActivatorOn = control.rawActivatorOn;
  out.physicalActivatorOn = control.physicalActivatorOn;
  out.machineRunState = static_cast<uint8_t>(control.machineRunState);
  out.machineStartAckPending = control.machineStartAckPending;
  out.machineStopAckPending = control.machineStopAckPending;
  out.machineOrphanRun = control.machineOrphanRun;
  out.noScaleShotGuardHold = control.noScaleShotGuardHold;
  out.noScaleShotGuardScaleWasAvailable =
      control.noScaleShotGuardScaleWasAvailable;
  out.cupStartGuardHold = control.cupStartGuardHold;
  out.healthHeapAlertLatched = healthHeapAlertLatched;
  out.healthStackAlertLatched = healthStackAlertLatched;
  out.healthLoopGapAlertLatched = healthLoopGapAlertLatched;
}

GuardInputs loopGuardInputs;

void observeMachineSenseFromSession(const ScaleLinkSnapshot &link) {
  MachineSense sense;
  if (session.active && session.hasWeightAnchor &&
      std::isfinite(session.lastAcceptedWeightG)) {
    sense.weightG = session.lastAcceptedWeightG;
    sense.weightSequence = session.lastAcceptedPacketSequence != 0
                               ? session.lastAcceptedPacketSequence
                               : currentWeightSequence;
  } else {
    sense.weightG = currentWeight;
    sense.weightSequence = currentWeightSequence;
  }
  sense.weightFresh = observedWeightIsFresh(millis(), link.connectionGeneration);
  sense.accidentalHold = session.accidentalTouchHolding;
  sense.brewCycleActive = session.active;
  sense.firstDropSeen = session.firstDropObserved || session.firstDropMs != 0;
  sense.scaleConnectedEdge = scaleWorkerTakeConnectedEdge();
  sense.cupRemovedEdge = pendingCupRemovedSettle;
  pendingCupRemovedSettle = false;
  machineObserveSense(sense);
}

void fillLoopGuardsFromIntention(const MachineIntention &intention,
                                 const ScaleLinkSnapshot &link) {
  loopGuardInputs.holdActive = intention.holdActive;
  loopGuardInputs.physicalOn = intention.physicalOn;
  loopGuardInputs.stablyOff = intention.stablyOff;
  loopGuardInputs.scaleAvailable = scaleLinkAvailable(link);
  loopGuardInputs.scaleUsable =
      loopGuardInputs.scaleAvailable && currentWeightIsFresh(millis(), link.connectionGeneration);
  loopGuardInputs.cup = cupPresenceIsKnown() ? cupPresenceState()
                                            : CupPresenceState::ABSENT;
  loopGuardInputs.currentWeightG = currentWeight;
  // Product reuses the machine rinse-gesture window as the blocked-start hold
  // timeout. Brew sees only this composed field, not rinseGestureMs.
  loopGuardInputs.blockedHoldTimeoutMs = runtimeConfig.rinseGestureMs;
}

void refreshLoopGuardsFromLastIntention(
    const ScaleLinkSnapshot &link = getScaleLinkSnapshot()) {
  fillLoopGuardsFromIntention(machineLastIntention(), link);
}

void pushActivatorDrivePermission(const ScaleLinkSnapshot &link) {
  serviceIdleTare();
  refreshLoopGuardsFromLastIntention(link);
  // A pending trip denies drive in both modes, so the mirror suppresses the
  // current hold instead of re-closing K1 before stateMachineTask consumes it.
  // Compatibility bypasses brew guards, while retaining the boot OFF interlock.
  const auto relay = getRelaySafetySnapshot();
  const bool tripPending = relay.tripped || relay.operationalTripped;
  if (firmwareCompatibilityMode()) {
    machineSetActivatorDriveAllowed(stopperState == StopperState::READY &&
                                    !tripPending);
    return;
  }
  machineSetActivatorDriveAllowed(
      !tripPending &&
      (machineWakePassthroughActive || backflushActive(stopperState) ||
       session.active || !guardsWouldBlockActivatorDrive(loopGuardInputs)));
}

// Grace ended without a zero. If the shot-start tare left the tared load on
// the scale, measure the shot from it (software zero) so the cup can neither
// cut nor mark first drop, and keep it removable as an untared cup.
void expirePostTareBaseline() {
  const float zero = session.preTareWeightG;
  const float minCupG = runtimeConfig.minimumCupWeightG;
  const bool software = !session.retarePerformed && zero >= minCupG &&
                        observedWeight >= zero - minCupG / 2.0f;
  if (!expirePostTareBaselineIfNeeded(software ? zero : NAN)) return;
  holdCupPresenceTransitions(false);
  if (!software) return;
  session.softwareZeroG = zero;
  markTareZeroReady();
  restoreCupTareReference(false, zero);
}

void orchestratePostTareBaselineArm() {
  armPostTareBaselineWindow();
  notifyCupPresenceTare();
  holdCupPresenceTransitions(true);
}

void confirmLateRetareBaseline();

// A light cup's pre-zero packet can sit inside the baseline band; after
// taring a cup, the baseline must have dropped by half a minimum cup.
bool postTareBaselinePlausible(float weightG) {
  return fabsf(weightG) <= POST_TARE_BASELINE_MAX_ABS_G &&
      !(session.preTareWeightG >= runtimeConfig.minimumCupWeightG &&
        weightG > session.preTareWeightG - runtimeConfig.minimumCupWeightG / 2.0f);
}

void applyShotTareResult(const ScaleEvent &event) {
  const bool lateRetare = session.retarePerformed &&
      event.cupWeightRequestId == session.retareRequestId;
  const bool ended = !session.active && lateRetare && pendingFinalize.pending &&
      pendingFinalize.cycleId == session.id &&
      static_cast<int32_t>(event.receivedAtMs - pendingFinalize.endedAtMs) <= 0;
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (event.cycleId != session.id || !session.startedWithScale ||
      (!ended && (!session.active || stopperState != StopperState::BREW)) ||
      event.discardedStaleConnection ||
      link.state != ScaleLinkState::CONNECTED ||
      event.connectionGeneration != link.connectionGeneration ||
      (event.cupWeightRequestId != 0 && session.appliedTareRequestId != 0 &&
       static_cast<int32_t>(event.cupWeightRequestId - session.appliedTareRequestId) <= 0)) return;
  session.appliedTareRequestId = event.cupWeightRequestId;
  session.tareCaptureBoundary = event.captureSequence;
  session.tareConnectionGeneration = event.connectionGeneration;
  if (lateRetare || std::isfinite(event.preTareWeightG))
    session.preTareWeightG = event.preTareWeightG;
  session.retareEffectPending = lateRetare;
  if (!ended) {
    orchestratePostTareBaselineArm();
    markTareZeroReady();
    resetDirectStopConfirmation();
  }
  session.postTareBaselineDeadlineMs =
      event.receivedAtMs + session.config.postTareBaselineGraceMs;
  confirmLateRetareBaseline();
}

void confirmLateRetareBaseline() {
  const ScaleTareSample &zero = session.retareZeroSample;
  const uint32_t atMs = zero.atMs;
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (!session.retareEffectPending ||
      session.appliedTareRequestId != session.retareRequestId ||
      zero.connectionGeneration != session.tareConnectionGeneration ||
      zero.connectionGeneration != link.connectionGeneration ||
      link.state != ScaleLinkState::CONNECTED ||
      zero.packetSequence == 0 || session.retareZeroCaptureSequence == 0 ||
      static_cast<int32_t>(atMs - session.startedAtMs) < 0 ||
      static_cast<int32_t>(session.retareZeroCaptureSequence - session.tareCaptureBoundary) <= 0 ||
      static_cast<int32_t>(atMs - session.postTareBaselineDeadlineMs) >= 0 ||
      static_cast<int32_t>(millis() - atMs) < 0 ||
      elapsedMs(atMs) > MAX_AUTOMATION_WEIGHT_AGE_MS ||
      !std::isfinite(zero.weightG) || !postTareBaselinePlausible(zero.weightG) ||
      (!session.active && (!pendingFinalize.pending ||
       pendingFinalize.cycleId != session.id ||
       static_cast<int32_t>(atMs - pendingFinalize.endedAtMs) > 0))) return;
  session.retareEffectPending = false;
  scaleProfileNoteEvent(ScaleProfileEvent::TARE_BASELINE_CONFIRMED, millis(),
                        zero.connectionGeneration, session.retareZeroCaptureSequence,
                        zero.weightG, session.retareRequestId, 0);
  session.tareCaptureBoundary = session.retareZeroCaptureSequence;
  session.firstDropMs = 0;
  shotCurveSampler.firstDrop = missingShotCurveEvent();
  markTareZeroReady();
  if (session.active) {
    session.awaitingPostTareBaseline = false;
    holdCupPresenceTransitions(false);
  }
  if (pendingFinalize.pending && pendingFinalize.cycleId == session.id) {
    pendingFinalize.firstDropDs = SHOT_LOG_METRIC_MISSING;
    pendingFinalize.curve.firstDrop = missingShotCurveEvent();
    if (homeCycleResult.cycleId == session.id) {
      homeCycleResult.firstDropElapsedMs = 0;
      homeCycleResult.averageFlowValid = false;
    }
    PersistedLastShot last;
    {
      const TaskLockGuard lock(shotStoreMutex);
      last = persistedLastShot;
    }
    if (last.valid && last.cycleId == session.id) {
      last.firstDropElapsedMs = 0;
      persistLastShotSnapshot(last);
    }
  }
}

// Observes the control decision about one weight sample. Accepted rows carry
// the packet sequence; rejected rows carry the DebugCode and reference.
void profileScaleWeightDecision(bool accepted, DebugCode reason, float weight,
                                uint32_t receivedAtMs, uint32_t packetSequence,
                                uint32_t connectionGeneration,
                                uint32_t captureSequence, float referenceG) {
  (void)receivedAtMs;
  scaleProfileNoteEvent(
      accepted ? ScaleProfileEvent::WEIGHT_ACCEPTED
               : ScaleProfileEvent::WEIGHT_REJECTED,
      millis(), connectionGeneration, captureSequence, weight,
      accepted ? packetSequence : static_cast<uint32_t>(reason),
      accepted ? 0 : static_cast<uint32_t>(weightToCentigrams(referenceG)));
}

void notePostStopTailSample(PendingShotFinalize &pending, float weightG,
                            uint32_t atMs) {
  pending.tailMaxG = pending.tailCount == 0
                         ? weightG
                         : fmaxf(pending.tailMaxG, weightG);
  if (pending.tailCount > 0 &&
      static_cast<uint32_t>(
          atMs - pending.tailAtMs[pending.tailCount - 1U]) <
          POST_STOP_TAIL_SAMPLE_SPACING_MS) {
    return;
  }
  if (pending.tailCount < POST_STOP_TAIL_SAMPLE_COUNT) {
    pending.tailG[pending.tailCount] = weightG;
    pending.tailAtMs[pending.tailCount] = atMs;
    ++pending.tailCount;
  } else {
    memmove(&pending.tailG[0], &pending.tailG[1],
            (POST_STOP_TAIL_SAMPLE_COUNT - 1U) * sizeof(float));
    memmove(&pending.tailAtMs[0], &pending.tailAtMs[1],
            (POST_STOP_TAIL_SAMPLE_COUNT - 1U) * sizeof(uint32_t));
    pending.tailG[POST_STOP_TAIL_SAMPLE_COUNT - 1U] = weightG;
    pending.tailAtMs[POST_STOP_TAIL_SAMPLE_COUNT - 1U] = atMs;
  }
}

// Median of the plausible post-stop tail; early post-stop samples are still
// rising, so a sparse or short-span tail falls back to the running max
// rather than letting one dip or one pre-settle sample define the shot.
float postStopFinalWeightG(const PendingShotFinalize &pending,
                           float fallbackG) {
  if (pending.tailCount == 0) {
    return fallbackG;
  }
  if (pending.tailCount < POST_STOP_TAIL_SAMPLE_COUNT ||
      static_cast<uint32_t>(
          pending.tailAtMs[pending.tailCount - 1U] - pending.tailAtMs[0]) <
          POST_STOP_TAIL_MIN_COVERAGE_MS) {
    return pending.tailMaxG;
  }
  float sorted[POST_STOP_TAIL_SAMPLE_COUNT];
  memcpy(sorted, pending.tailG, sizeof(sorted));
  for (size_t i = 1; i < POST_STOP_TAIL_SAMPLE_COUNT; ++i) {
    const float value = sorted[i];
    size_t j = i;
    while (j > 0 && sorted[j - 1] > value) {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = value;
  }
  const float median = sorted[POST_STOP_TAIL_SAMPLE_COUNT / 2U];
  // A majority of dipped samples would own the median; the plausibility
  // pre-filter only rejects individual outliers, so cap the damage by
  // falling back to the running max.
  return median < pending.tailMaxG - MAX_RECOVERY_WEIGHT_DROP_G
             ? pending.tailMaxG
             : median;
}

// Coffee filling a cup-less pan can look stable over the placement window;
// a late cup must stand half a minimum cup above the lowest accepted reading
// of the second before that window started, both in shot coordinates.
bool cupPlacementIsStep(float weight, uint32_t placementStartedAtMs) {
  const float startS = static_cast<int32_t>(placementStartedAtMs - shot.startMs) / 1000.0f;
  float baseG = INFINITY;
  for (size_t i = shot.datapoints; i-- > 0;) {
    if (shot.timeS[i] >= startS) continue;
    baseG = fminf(baseG, shot.weight[i]);
    if (shot.timeS[i] < startS - 1.0f) break;
  }
  return !std::isfinite(baseG) ||
         baseG < weight - session.softwareZeroG - runtimeConfig.minimumCupWeightG / 2.0f;
}

bool recordWeightSampleWithProvenance(float weight, uint32_t receivedAtMs,
                                      uint32_t packetSequence,
                                      uint32_t connectionGeneration,
                                      uint32_t captureSequence = 0) {
  const ProfileControlObservation profile(captureSequence, connectionGeneration);
  const bool touchProvenanceValid = packetSequence != 0 && connectionGeneration != 0;
  const TouchStopEvidence previousTouchStop = session.touchStop;
  session.touchStop = {};  // Only a qualifying touch sample may retain evidence.
  if (!std::isfinite(weight) || fabsf(weight) > MAX_PARSED_WEIGHT_G) {
    resetCupSampleEvidence();
    approveScaleTareSample(ScaleTareSample{});
    serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                        "Invalid or out-of-range weight ignored");
    rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_INVALID, weight);
    profileScaleWeightDecision(false, DebugCode::SCALE_SAMPLE_REJECTED_INVALID,
                               weight, receivedAtMs, packetSequence,
                               connectionGeneration, captureSequence, 0);
    return false;
  }

  if (connectionGeneration == 0) {
    connectionGeneration = session.ownedConnectionGeneration;
  }
  if (packetSequence == 0) {
    packetSequence = session.lastAcceptedPacketSequence + 1U;
    if (packetSequence == 0) {
      packetSequence = 1;
    }
  }
  if (session.retarePerformed &&
      (session.retareEffectPending || session.appliedTareRequestId != session.retareRequestId) &&
      postTareBaselinePlausible(weight) &&
      connectionGeneration == scaleConnectionGenerationNow() &&
      (!session.hasWeightAnchor ||
       (static_cast<int32_t>(receivedAtMs - session.lastAcceptedWeightAtMs) > 0 &&
        static_cast<int32_t>(packetSequence - session.lastAcceptedPacketSequence) > 0))) {
    session.retareZeroSample = {weight, receivedAtMs, packetSequence, connectionGeneration};
    session.retareZeroCaptureSequence = captureSequence;
    confirmLateRetareBaseline();
  }

  if ((runtimeConfig.autoTareOutsideBrew || idleTare.requestId != 0) &&
      !session.active &&
      !observeIdleTareSample(weight, receivedAtMs, packetSequence,
                             connectionGeneration, captureSequence)) return false;
  const bool placementInRange = weight >= MIN_AUTOMATION_WEIGHT_G &&
                                weight <= MAX_AUTOMATION_WEIGHT_G;
  const bool cupSampleValid = observeCupWeight(weight, receivedAtMs, packetSequence,
                                               connectionGeneration);
  // Publish the approved pre-write load before allowing the worker to claim it.
  if (cupSampleValid && !session.active && idleTare.requestId != 0)
    approveIdleScaleTareSample(idleTare.requestId, packetSequence);
  const bool observedUnload = cupSampleValid && cupWeightNearKnownEmpty(weight);
  const uint32_t placementStartedAtMs = cupPresence.placeStabilityStartedAtMs;
  const CupPresenceEvent cupEvent = cupSampleValid ?
      feedCupPresence(weight, receivedAtMs, packetSequence,
                      session.active ||
                          (!runtimeConfig.autoTareOutsideBrew && idleTare.requestId == 0) ||
                          placementInRange, !session.active,
                      !session.active &&
                          (idleKnownAccessoryRemoval(weight) ||
                           idleAccessoryUnloadUnsettled(weight, receivedAtMs)),
                      // An unseen cup zeroed before connection reads ~0 g pre-tare.
                      session.active && session.config.cupProtectionEnabled &&
                          session.config.stopIfCupRemoved && session.scaleBaselineReady &&
                          !session.awaitingPostTareBaseline &&
                          fabsf(session.preTareWeightG) < runtimeConfig.minimumCupWeightG)
      : CupPresenceEvent::NONE;
  if (cupEvent == CupPresenceEvent::PLACED) {
    scaleProfileNoteEvent(ScaleProfileEvent::CUP_PLACED, millis(),
                          connectionGeneration, packetSequence, weight,
                          cupPresencePlacementId(), 0);
  } else if (cupEvent == CupPresenceEvent::REMOVED) {
    scaleProfileNoteEvent(ScaleProfileEvent::CUP_REMOVED, millis(),
                          connectionGeneration, packetSequence, weight,
                          cupPresencePlacementId(), 0);
  }
  if (!session.active && pendingFinalize.pending &&
      (observedUnload || cupEvent != CupPresenceEvent::NONE)) {
    machineCancelSettledWeightCutOff();
    cancelPendingFinalize("Cup continuity lost during drip analysis", 1);
  }
  if ((runtimeConfig.autoTareOutsideBrew || idleTare.requestId != 0) &&
      !session.active) {
    handleIdleCupEvent(cupEvent);
    if (cupSampleValid) observeIdleAccessory(weight, receivedAtMs);
  }
  if (cupEvent == CupPresenceEvent::REMOVED) {
    bullseyeTracker.clear();
    // Momentary-only IDLE_UNCONFIRMED run-state settle — independent of cut.
    pendingCupRemovedSettle = true;
    if (session.active && stopperState == StopperState::BREW &&
        !session.config.timerOnly && session.config.cupProtectionEnabled &&
        session.config.stopIfCupRemoved && session.startedWithScale) {
      session.cupRemovedPending = true;
      addDebugEvent(DebugCategory::SCALE, DebugCode::CUP_REMOVED_CONFIRMED,
                    weightToCentigrams(weight),
                    static_cast<int32_t>(elapsedMs(session.startedAtMs)));
    }
  }
  if (cupEvent == CupPresenceEvent::PLACED && session.active &&
      session.startedWithScale && retareWindowOpen() &&
      !session.awaitingPostTareBaseline && weight >= MIN_AUTOMATION_WEIGHT_G &&
      weight <= MAX_AUTOMATION_WEIGHT_G && cupPlacementIsStep(weight, placementStartedAtMs)) {
    performAutomaticRetare();
  }

  // Shot (and its drip tail) in shot coordinates after a software zero.
  if (session.active || pendingFinalize.pending) weight -= session.softwareZeroG;

  // First-flow markers belong to the connection that owns this cycle; a
  // foreign generation must not latch a first-drop timestamp.
  if (session.active && session.startedWithScale &&
      (session.ownedConnectionGeneration == 0 ||
       connectionGeneration == session.ownedConnectionGeneration)) {
    const FirstFlowObservation firstDrop =
        considerScaleFlowMarkers(weight, receivedAtMs, packetSequence);
    if (firstDrop.atMs != 0) {
      onFirstDropsDetected(firstDrop);
    }
  }

  if (!shouldTrackWeight()) {
    const bool valid = weight >= MIN_AUTOMATION_WEIGHT_G && weight <= MAX_AUTOMATION_WEIGHT_G;
    if (!session.active && pendingFinalize.pending && pendingFinalize.startedWithScale &&
        static_cast<int32_t>(receivedAtMs - pendingFinalize.endedAtMs) > 0) {
      const bool continuous = valid &&
          connectionGeneration == pendingFinalize.scaleConnectionGeneration &&
          static_cast<int32_t>(packetSequence - pendingFinalize.curveLastSequence) > 0 &&
          plausibleSettledBrewWeight(weight, pendingFinalize.lastKnownWeightG,
                                     pendingFinalize.lastKnownWeightValid);
      pendingFinalize.curveBreakPending = !continuous ||
          !appendShotCurveObservation(pendingFinalize.curve, weight,
              receivedAtMs - pendingFinalize.cycleStartedAtMs, pendingFinalize.curveBreakPending);
      if (continuous) {
        pendingFinalize.curveLastSequence = packetSequence;
        notePostStopTailSample(pendingFinalize, weight, receivedAtMs);
      }
    }
    return valid;
  }

  if ((session.weightControlState == WeightControlState::ACTIVE ||
       session.weightControlState == WeightControlState::VALIDATING) &&
      session.ownedConnectionGeneration != 0 &&
      connectionGeneration != session.ownedConnectionGeneration) {
    suspendWeightControl();
  }

  if (weight < MIN_AUTOMATION_WEIGHT_G ||
      weight > MAX_AUTOMATION_WEIGHT_G) {
    if (fabsf(weight) > MAX_AUTOMATION_WEIGHT_G) {
      considerDirectStopSample(weight, receivedAtMs, packetSequence,
                               connectionGeneration);
    }
    weightStreamState = WeightStreamState::OVERLOAD;
    session.calibrationEligible = false;
    if (session.weightControlState == WeightControlState::ACTIVE) {
      setWeightControlState(WeightControlState::VALIDATING);
    }
    session.recoveryConfirmations = 0;
    const float violatedLimit = weight < MIN_AUTOMATION_WEIGHT_G
                                    ? MIN_AUTOMATION_WEIGHT_G
                                    : MAX_AUTOMATION_WEIGHT_G;
    rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_RANGE, weight,
                      violatedLimit);
    profileScaleWeightDecision(false, DebugCode::SCALE_SAMPLE_REJECTED_RANGE,
                               weight, receivedAtMs, packetSequence,
                               connectionGeneration, captureSequence,
                               violatedLimit);
    return false;
  }

  if (static_cast<int32_t>(receivedAtMs - shot.startMs) < 0) {
    rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_PRE_CYCLE, weight);
    profileScaleWeightDecision(false, DebugCode::SCALE_SAMPLE_REJECTED_PRE_CYCLE,
                               weight, receivedAtMs, packetSequence,
                               connectionGeneration, captureSequence, 0);
    return false;
  }

  if (session.awaitingPostTareBaseline && session.retareEffectPending &&
      static_cast<int32_t>(receivedAtMs - session.postTareBaselineDeadlineMs) >= 0)
    expirePostTareBaseline();
  if (session.awaitingPostTareBaseline) {
    if (session.retareEffectPending) {
      shotCurveSampler.markBreak();
      return false;
    }
    if (postTareBaselinePlausible(weight)) {
      session.awaitingPostTareBaseline = false;
      holdCupPresenceTransitions(false);
      weightStreamState = WeightStreamState::FRESH;
      scaleProfileNoteEvent(ScaleProfileEvent::TARE_BASELINE_CONFIRMED,
                            millis(), connectionGeneration, captureSequence,
                            weight, session.appliedTareRequestId, 0);
      return acceptWeightIntoTrajectory(weight, receivedAtMs, packetSequence);
    }
    rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_SLEW, weight,
                      POST_TARE_BASELINE_MAX_ABS_G);
    profileScaleWeightDecision(false, DebugCode::SCALE_SAMPLE_REJECTED_SLEW,
                               weight, receivedAtMs, packetSequence,
                               connectionGeneration, captureSequence,
                               POST_TARE_BASELINE_MAX_ABS_G);
    return false;
  }

  bool plausible = true;
  if (session.hasWeightAnchor) {
    if (static_cast<int32_t>(receivedAtMs -
                             session.lastAcceptedWeightAtMs) < 0) {
      plausible = false;
    } else {
      uint32_t deltaMs = receivedAtMs - session.lastAcceptedWeightAtMs;
      if (deltaMs == 0) {
        deltaMs = 1;
      }
      const float allowedDelta = AUTOMATION_WEIGHT_SLEW_ALLOWANCE_G +
          MAX_AUTOMATION_WEIGHT_SLEW_G_PER_S * deltaMs / 1000.0f;
      plausible = fabsf(weight - session.lastAcceptedWeightG) <= allowedDelta;
    }
  }

  if (session.weightControlState == WeightControlState::ACTIVE && !plausible) {
    weightStreamState = WeightStreamState::ANOMALOUS;
    session.calibrationEligible = false;
    setWeightControlState(WeightControlState::VALIDATING);
    session.recoveryConfirmations = 0;
    session.recoveryLastAtMs = 0;
    session.recoveryLastPacketSequence = 0;
    rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_SLEW, weight,
                      session.lastAcceptedWeightG);
    profileScaleWeightDecision(false, DebugCode::SCALE_SAMPLE_REJECTED_SLEW,
                               weight, receivedAtMs, packetSequence,
                               connectionGeneration, captureSequence,
                               session.lastAcceptedWeightG);
    return false;
  }

  if (session.weightControlState == WeightControlState::VALIDATING ||
      session.weightControlState == WeightControlState::SUSPENDED) {
    bool recoveryPlausible = plausible;
    // Only a stream gap can hide a drop; a live anomaly (touch, overload) may
    // recover below the rejected level once three coherent samples agree.
    const bool dropFloor =
        session.weightControlState == WeightControlState::SUSPENDED;
    if (session.recoveryConfirmations > 0) {
      const bool consecutive =
          packetSequence == session.recoveryLastPacketSequence + 1U;
      const int32_t signedDelta =
          static_cast<int32_t>(receivedAtMs - session.recoveryLastAtMs);
      if (!consecutive || signedDelta <= 0 ||
          static_cast<uint32_t>(signedDelta) > MAX_AUTOMATION_WEIGHT_AGE_MS) {
        recoveryPlausible = false;
      } else {
        const float allowedIncrease = AUTOMATION_WEIGHT_SLEW_ALLOWANCE_G +
            MAX_AUTOMATION_WEIGHT_SLEW_G_PER_S *
                static_cast<uint32_t>(signedDelta) / 1000.0f;
        const float change = weight - session.recoveryLastWeightG;
        recoveryPlausible = (!dropFloor || change >= -MAX_RECOVERY_WEIGHT_DROP_G) &&
                            change <= allowedIncrease;
      }
    } else if (dropFloor && session.hasWeightAnchor) {
      recoveryPlausible = recoveryPlausible &&
          weight >= session.lastAcceptedWeightG - MAX_RECOVERY_WEIGHT_DROP_G;
    }

    if (!recoveryPlausible) {
      const float referenceG = session.recoveryConfirmations > 0
                                   ? session.recoveryLastWeightG
                                   : session.lastAcceptedWeightG;
      session.recoveryConfirmations = 0;
      session.recoveryLastAtMs = 0;
      session.recoveryLastPacketSequence = 0;
      rejectScaleSample(DebugCode::SCALE_SAMPLE_REJECTED_RECOVERY, weight,
                        referenceG);
      profileScaleWeightDecision(false, DebugCode::SCALE_SAMPLE_REJECTED_RECOVERY,
                                 weight, receivedAtMs, packetSequence,
                                 connectionGeneration, captureSequence,
                                 referenceG);
      return false;
    }

    ++session.recoveryConfirmations;
    session.recoveryLastAtMs = receivedAtMs;
    session.recoveryLastPacketSequence = packetSequence;
    session.recoveryLastWeightG = weight;
    if (session.recoveryConfirmations <
        WEIGHT_RECOVERY_CONFIRMATION_SAMPLES) {
      shotCurveSampler.markBreak();
      return false;
    }

    session.ownedConnectionGeneration = connectionGeneration;
    session.scaleDisconnectSequenceAtStart =
        getScaleLinkSnapshot().disconnectSequence;
    session.recoveryConfirmations = 0;
    // A live anomaly keeps the pre-anomaly anchor and trend so the touch
    // classifier below judges the recovered level as a step, not a plateau.
    if (dropFloor || !session.hasWeightAnchor ||
        !session.config.avoidAccidentalTouchEnabled) {
      resetWeightTrend();
      session.hasWeightAnchor = true;
      session.lastAcceptedWeightG = weight;
      session.lastAcceptedWeightAtMs = receivedAtMs;
      session.lastAcceptedPacketSequence = packetSequence;
    }
    setWeightControlState(WeightControlState::ACTIVE);
  }

  if (accidentalTouchSessionActive()) {
    const AccidentalTouchClass classified =
        evaluateAccidentalTouchSample(weight, receivedAtMs);
    if (classified == AccidentalTouchClass::TOUCH) {
      shotCurveSampler.accept(weight, receivedAtMs, true);
      resetDirectStopConfirmation();
      if (touchProvenanceValid) considerTouchStopSample(weight, receivedAtMs, packetSequence,
                              connectionGeneration, previousTouchStop);
      weightStreamState = WeightStreamState::FRESH;
      return true;
    }
  }

  considerDirectStopSample(weight, receivedAtMs, packetSequence,
                           connectionGeneration);
  weightStreamState = WeightStreamState::FRESH;
  profileScaleWeightDecision(true, static_cast<DebugCode>(0), weight,
                             receivedAtMs, packetSequence, connectionGeneration,
                             captureSequence, 0);
  return acceptWeightIntoTrajectory(weight, receivedAtMs, packetSequence);
}

// Host-test-only entry point: the firmware path feeds weight through
// processScaleWorkerEvents() -> recordWeightSampleWithProvenance(). Kept out
// of the ESP32 build so a future caller cannot bypass the worker's sequence
// accounting from a BLE callback.
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
bool recordWeightSample(float weight, uint32_t receivedAtMs) {
  uint32_t packetSequence = 0;
  portENTER_CRITICAL(&scaleLinkMux);
  ++scalePacketSequence;
  if (scalePacketSequence == 0) {
    scalePacketSequence = 1;
  }
  packetSequence = scalePacketSequence;
  portEXIT_CRITICAL(&scaleLinkMux);
  return recordWeightSampleWithProvenance(
      weight, receivedAtMs, packetSequence,
      session.ownedConnectionGeneration);
}
#endif


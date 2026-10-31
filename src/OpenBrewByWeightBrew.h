#pragma once

#include "OpenBrewByWeightAlert.h"
#include "OpenBrewByWeightBrewTypes.h"

// =============================================================================
// LAYER: Brew (policy + extraction guards)
// =============================================================================
// WHAT: Brew-by-weight cuts, extraction guards (fast/slow, A→M, min/max time),
//       thresholds, and in-shot brew decisions. Included from openBrewByWeight.cpp
//       after Machine and session BSS. No heap.
//
// BOUNDARY (hard rules — do not cross):
// - Talks to the machine ONLY through the generic façade (UserIntent,
//   machineIsRunning / machineRunningElapsed, start/stop requests). Never
//   include or call paddle/momentary/reed internals.
// - Must not know paddle vs switch vs reed details. If a guard or brew path
//   branches on paddle-mode settings, reed GPIO, momentary pulse state, or
//   compiled machine identity, that is a layer violation — move it into the
//   machine specialization.
// - Does not own cup presence or scale link; consumes GuardInputs / session
//   snapshots the stopper already built. Must not read rinseGestureMs.
// - Start-guard WouldBlock predicates are read-only. They must not call
//   machineSet* or drive K1; the stopper composes them into the façade bit.

bool shouldTrackWeight() {
  return session.active && !session.config.timerOnly &&
         session.weightControlState != WeightControlState::INACTIVE &&
         session.weightControlState != WeightControlState::FAULT_STOPPED &&
         stopperState == StopperState::BREW;
}

// Shot time (ms from the machine running start) of an observation stamped
// atMs; false when the machine is not running or the stamp predates it.
bool shotTimeAtMs(uint32_t atMs, int32_t &shotMs) {
  uint32_t runningMs = 0;
  const uint32_t ageMs = millis() - atMs;
  if (!machineRunningElapsed(runningMs) || ageMs > runningMs) return false;
  shotMs = static_cast<int32_t>(runningMs - ageMs);
  return true;
}

int32_t gramsToDg(float grams) {
  return static_cast<int32_t>(lroundf(grams * 10.0f));
}

// The smooth cut line f(t) of this shot (corridor kernel).
int32_t cutLineAtDg(int32_t shotMs) {
  return corridor::cutLineDg(session.cut.params, shotMs);
}

int32_t cutLineNowDg() {
  int32_t shotMs = 0;
  return cutLineAtDg(shotTimeAtMs(millis(), shotMs) ? shotMs : 0);
}

// Backstop direct-law threshold now: f(t) - r, without the prediction lead.
float effectiveStopThreshold() {
  return static_cast<float>(cutLineNowDg() - session.cut.offsetDg) / 10.0f;
}

// A weight cut reports the segment of f that fired (13 raised, 3 target,
// 16 lowered).
// Latches the exact shot time and line, which the record and learning use.
EndReason weightCutReasonNow() {
  corridor::CorridorState &cut = session.cut;
  if (!shotTimeAtMs(millis(), cut.stopMs)) cut.stopMs = 0;
  cut.stopLineDg = cutLineAtDg(cut.stopMs);
  return corridor::cutReason(cut.params, cut.stopLineDg);
}

// Snapshot of the shot's cut parameters; the control task owns this state.
// The stopper passes the connected scale protocol's sensor lag.
void armSmoothCut(uint16_t sensorLagMs) {
  corridor::CorridorState &cut = session.cut;
  cut = corridor::CorridorState{};
  cut.params.goalDg = session.config.goalWeightDg;
  cut.params.fastMarginDg = session.config.fastMarginDg;
  cut.params.slowMarginDg = session.config.slowMarginDg;
  cut.params.fastOn = session.config.fastSideEnabled;
  cut.params.slowOn = session.config.slowSideEnabled;
  cut.params.delayMs = static_cast<int32_t>(session.config.bbwProtectionMs);
  cut.params.fastBeforeMs = static_cast<int32_t>(session.config.fastBeforeMs);
  cut.params.slowAfterMs = static_cast<int32_t>(session.config.slowAfterMs);
  cut.params.wallMs = static_cast<int32_t>(session.config.operationalWallMs);
  // The exact curve evaluator is bounded for ramps up to MAX_RAMP_MS.
  cut.params.fastRampMs = session.config.fastRampMs > MAX_RAMP_MS
                              ? static_cast<int32_t>(MAX_RAMP_MS)
                              : session.config.fastRampMs;
  cut.params.slowRampMs = session.config.slowRampMs > MAX_RAMP_MS
                              ? static_cast<int32_t>(MAX_RAMP_MS)
                              : session.config.slowRampMs;
  cut.params.fastCurve =
      corridor::rampCurveIndex(session.config.fastCurve, session.config.fastCurveHardness);
  cut.params.slowCurve =
      corridor::rampCurveIndex(session.config.slowCurve, session.config.slowCurveHardness);
  cut.params.fastCurveHardnessPct =
      rampHardnessPctEffective(session.config.fastCurveHardnessPct,
                               session.config.fastCurveHardness);
  cut.params.slowCurveHardnessPct =
      rampHardnessPctEffective(session.config.slowCurveHardnessPct,
                               session.config.slowCurveHardness);
  cut.tauMs = corridor::tauMs(sensorLagMs, machineStopCommandLatencyMs());
  cut.offsetDg = gramsToDg(session.config.weightOffsetG);
}

// After each accepted sample: the predicted stop-command time t* where the
// projected weight plus in-flight lead and drip offset meets f. No valid rate
// (or no crossing before the wall) leaves only the direct backstop.
void updateCutPrediction(float weight, uint32_t receivedAtMs) {
  shot.expectedEndS = NO_CUT_PREDICTION_S;
  int32_t shotMs = 0;
  corridor::CorridorState &cut = session.cut;
  cut.crossingValid = false;
  const int32_t weightDg = gramsToDg(weight);
  if (!shotTimeAtMs(receivedAtMs, shotMs) ||
      cut.rate.push(shotMs, weightDg) !=
          corridor::RateEstimator::Result::ACCEPTED ||
      !cut.rate.valid) {
    return;
  }
  // Channeling: f := min(f, target) for the rest of the shot.
  if (cut.rate.channeling && cut.params.fastOn) {
    cut.params.fastOn = false;
    addDebugEvent(DebugCategory::SCALE, DebugCode::CHANNELING_DETECTED,
                  cut.rate.rawX100, shotMs);
  }
  cut.crossingValid = corridor::crossingMs(cut.params, shotMs, weightDg,
                                          cut.rate.rateX100, cut.tauMs,
                                          cut.offsetDg, cut.crossingMs);
  if (cut.crossingValid && cut.crossingMs < cut.params.wallMs) {
    shot.expectedEndS = cut.crossingMs / 1000.0f;
  }
  // Read-only shot-end prediction for the hero tint (never actuates).
  {
    const uint32_t uptime = millis();
    predictor::PredictorInputs in;
    in.cut = &cut;
    in.nowMs = shotMs;
    in.weightDg = weightDg;
    in.firstDropMs = session.firstDropMs > session.circuitClosedAtMs &&
                             session.circuitClosedAtMs != 0
                         ? static_cast<int32_t>(session.firstDropMs -
                                                session.circuitClosedAtMs)
                         : 0;
    in.qPX100 = session.predictorQpX100;
    in.autoMode = session.automaticEnabled && session.startedWithScale &&
                  !session.config.timerOnly;
    in.scaleOk = true;  // runs only on accepted weights
    in.retarePending = session.retareEffectPending;
    in.sampleDtMs = session.predictor.lastSampleMs != 0 &&
                            uptime > session.predictor.lastSampleMs
                        ? static_cast<int32_t>(uptime -
                                               session.predictor.lastSampleMs)
                        : 100;
    if (in.sampleDtMs > 2000) in.sampleDtMs = 2000;
    session.predictor.lastSampleMs = uptime;
    predictor::updateShotPredictor(session.predictor, in);
  }
}

void enterBrewOrManualFromStart();

void resetFirstFlowDetector() {
  resetFirstFlowState(session.firstFlow);
}

void markTareZeroReady() {
  session.scaleBaselineG = 0.0f;
  session.scaleBaselineReady = true;
  resetFirstFlowDetector();
}

void recordFirstDropTimestamp(uint32_t receivedAtMs) {
  if (session.firstDropMs == 0) {
    session.firstDropMs = receivedAtMs;
  }
}

void requestFirstDropBeep() {
  if (!session.firstDropsBeepSent && session.config.firstDropBeep) {
    emitAlert(AlertEvent::FIRST_DROP, session.id);
    session.firstDropsBeepSent = true;
  }
}
bool retareWindowOpen();

void notifyRetareFlowDetected(uint32_t receivedAtMs) {
  if (!retareWindowOpen()) {
    return;
  }
  if (session.retareFlowFirstDetectedAtMs == 0) {
    session.retareFlowFirstDetectedAtMs = receivedAtMs;
    addDebugEvent(DebugCategory::SCALE, DebugCode::FIRST_DROP_DURING_RETARE,
                  static_cast<int32_t>(session.id),
                  static_cast<int32_t>(elapsedMs(session.startedAtMs)));
  }
  session.flowDuringRetare = true;
}

void resetDirectStopConfirmation() {
  if (session.thresholdConfirmations != 0) shotTrace().noteConfirmationReset();
  session.thresholdConfirmations = 0;
  session.lastThresholdAtMs = 0;
  session.lastThresholdPacketSequence = 0;
  session.lastThresholdConnectionGeneration = 0;
  session.directStopPending = false;
}

bool bbwWeightStopInhibited();

bool accidentalTouchSessionActive() {
  return shouldTrackWeight() && session.config.avoidAccidentalTouchEnabled &&
         session.startedWithScale &&
         session.weightControlState == WeightControlState::ACTIVE;
}

// Touch fallback policy: the segment of f at the sample. f moves along a
// ramp, so evidence compares the segment only and f is re-read at fire time.
EndReason touchStopPolicy(float &threshold, uint32_t atMs) {
  int32_t shotMs = 0;
  const int32_t line = cutLineAtDg(shotTimeAtMs(atMs, shotMs) ? shotMs : 0);
  threshold = static_cast<float>(line - session.cut.offsetDg) / 10.0f;
  return corridor::cutReason(session.cut.params, line);
}

void considerTouchStopSample(float weight, uint32_t atMs, uint32_t sequence,
                            uint32_t generation,
                            const TouchStopEvidence &previous) {
  if (!accidentalTouchSessionActive() || !session.config.touchStopFallbackEnabled ||
      bbwWeightStopInhibited() || session.awaitingPostTareBaseline ||
      !machineAllowsAutomationStop() || sequence == 0 || generation == 0 ||
      generation != session.ownedConnectionGeneration ||
      static_cast<int32_t>(millis() - atMs) < 0 ||
      elapsedMs(atMs) > MAX_AUTOMATION_WEIGHT_AGE_MS) return;
  float threshold;
  const EndReason policy = touchStopPolicy(threshold, atMs);
  if (weight < threshold) return;
  const int32_t gap = static_cast<int32_t>(atMs - previous.lastAtMs);
  // Duplicate/out-of-order observations cannot start a new interval either.
  if (previous.samples && generation == previous.connectionGeneration &&
      (gap <= 0 || static_cast<int32_t>(sequence - previous.packetSequence) <= 0))
    return;
  const bool continuous = previous.samples && previous.policy == policy &&
      generation == previous.connectionGeneration &&
      sequence == previous.packetSequence + 1U && gap > 0 &&
      static_cast<uint32_t>(gap) <= DIRECT_STOP_CONFIRMATION_WINDOW_MS;
  session.touchStop = {continuous ? previous.firstAtMs : atMs, atMs, sequence,
      generation, threshold, weight, policy,
      static_cast<uint8_t>(continuous ? (previous.samples < 3 ? previous.samples + 1 : 3) : 1)};
}

void noteAccidentalTouchClass(AccidentalTouchClass classified, float weight,
                              float residualG, uint32_t atMs) {
  const bool wasHolding = session.accidentalTouchHolding;
  session.accidentalTouchClass = classified;
  if (classified == AccidentalTouchClass::TOUCH) {
    session.accidentalTouchHolding = true;
    if (session.accidentalTouchPendingCount <
        ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES) {
      session.accidentalTouchPendingG[session.accidentalTouchPendingCount++] =
          residualG;
    } else {
      memmove(&session.accidentalTouchPendingG[0],
              &session.accidentalTouchPendingG[1],
              (ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES - 1U) * sizeof(float));
      session.accidentalTouchPendingG[ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES - 1U] =
          residualG;
    }
    if (!wasHolding) {
      session.accidentalTouchHeldSinceMs = atMs;
      addDebugEvent(DebugCategory::SCALE, DebugCode::ACCIDENTAL_TOUCH_HOLD,
                    weightToCentigrams(weight));
      scaleProfileNoteEvent(ScaleProfileEvent::TOUCH_HOLD, millis(), 0, 0,
                            weight, static_cast<uint32_t>(classified), 0);
    }
    return;
  }

  session.accidentalTouchHolding = false;
  session.accidentalTouchPendingCount = 0;
  if (classified == AccidentalTouchClass::SUSTAINED) {
    // An accepted level shift starts a new trend epoch and a curve segment.
    session.accidentalTouchEpochS =
        static_cast<int32_t>(atMs - shot.startMs) / 1000.0f;
    shotCurveSampler.markBreak();
    session.calibrationEligible = false;
    addDebugEvent(DebugCategory::SCALE, DebugCode::ACCIDENTAL_TOUCH_SUSTAINED,
                  weightToCentigrams(weight));
    scaleProfileNoteEvent(ScaleProfileEvent::TOUCH_SUSTAINED, millis(), 0, 0,
                          weight, static_cast<uint32_t>(classified), 0);
  } else if (wasHolding) {
    addDebugEvent(DebugCategory::SCALE, DebugCode::ACCIDENTAL_TOUCH_RELEASE,
                  weightToCentigrams(weight));
    scaleProfileNoteEvent(ScaleProfileEvent::TOUCH_RELEASE, millis(), 0, 0,
                          weight, static_cast<uint32_t>(classified), 0);
  }
}

AccidentalTouchClass evaluateAccidentalTouchSample(float weight,
                                                   uint32_t receivedAtMs) {
  const float nowS =
      static_cast<int32_t>(receivedAtMs - shot.startMs) / 1000.0f;
  const float lastS =
      static_cast<int32_t>(session.lastAcceptedWeightAtMs - shot.startMs) /
      1000.0f;
  const AccidentalTouchResidual trend = accidentalTouchResidual(
      shot.timeS, shot.weight, session.hasWeightAnchor ? shot.datapoints : 0,
      session.accidentalTouchEpochS, session.lastAcceptedWeightG, lastS, weight,
      nowS);
  const AccidentalTouchPhase phase = trend.frozen
                                         ? AccidentalTouchPhase::TREND
                                         : AccidentalTouchPhase::STARTUP;
  if (phase == AccidentalTouchPhase::TREND &&
      session.accidentalTouchPhase == AccidentalTouchPhase::STARTUP) {
    addDebugEvent(DebugCategory::SCALE, DebugCode::ACCIDENTAL_TOUCH_HANDOFF,
                  static_cast<int32_t>(shot.datapoints));
  }
  session.accidentalTouchPhase = phase;
  AccidentalTouchClass classified = classifyAccidentalTouch(
      trend.residualG, session.accidentalTouchHolding,
      receivedAtMs - session.accidentalTouchHeldSinceMs,
      session.accidentalTouchPendingG, session.accidentalTouchPendingCount);
  // With the touch fallback on, a plateau or a hold above the cut line stays
  // touch evidence (labelled fallback) instead of releasing into a direct cut.
  float threshold = 0.0f;
  if (classified != AccidentalTouchClass::TOUCH &&
      session.config.touchStopFallbackEnabled &&
      (classified == AccidentalTouchClass::SUSTAINED || session.accidentalTouchHolding)) {
    touchStopPolicy(threshold, receivedAtMs);
    if (weight >= threshold) classified = AccidentalTouchClass::TOUCH;
  }
  // A contact's leading edge may slip under the gate one sample early; keep
  // it out of the trend so it can never become the reference for clean flow.
  if (classified == AccidentalTouchClass::TOUCH &&
      !session.accidentalTouchHolding && shot.datapoints > 0 &&
      shot.timeS[shot.datapoints - 1U] == lastS) {
    const float edgeG = accidentalTouchResidual(
        shot.timeS, shot.weight, shot.datapoints, session.accidentalTouchEpochS,
        session.lastAcceptedWeightG, lastS, session.lastAcceptedWeightG,
        lastS).residualG;
    if (edgeG * trend.residualG > 0.0f &&
        fabsf(edgeG) >= ACCIDENTAL_TOUCH_HOLD_RESIDUAL_G) {
      --shot.datapoints;
    }
  }
  noteAccidentalTouchClass(classified, weight, trend.residualG, receivedAtMs);
  return classified;
}

bool bbwAutomaticScaleSession() {
  return session.active && session.bbwProtectionEnabled;
}

bool retareWindowOpen() {
  if (!bbwAutomaticScaleSession() || !session.config.autoRetare) {
    return false;
  }
  if (session.retareEnded || session.retarePerformed) {
    return false;
  }
  // A pre-infusing machine may show no coffee for seconds: until first drop,
  // a late cup may still be tared while weight stops remain inhibited.
  const uint32_t elapsed = elapsedMs(session.startedAtMs);
  return elapsed < session.config.retareWindowMs ||
         (!session.firstDropObserved && elapsed < session.config.bbwProtectionMs);
}

bool bbwProtectionActive() {
  if (!bbwAutomaticScaleSession()) {
    return false;
  }
  return !session.bbwProtectionEnded;
}

void markRetareEnded(uint32_t endedAtMs) {
  (void)endedAtMs;
  if (session.retareEnded) {
    return;
  }
  session.retareEnded = true;
  session.retareDisabled = true;
}

bool bbwWeightStopInhibited() {
  if (!bbwAutomaticScaleSession()) {
    return false;
  }
  return bbwProtectionActive();
}

void onFirstDropsDetected(const FirstFlowObservation &observation) {
  const bool first = !session.firstDropObserved;
  session.firstDropObserved = true;
  recordFirstDropTimestamp(observation.atMs);
  int32_t onsetMs = 0;
  if (shotTimeAtMs(observation.atMs, onsetMs)) session.cut.rate.onsetMs = onsetMs;
  // Flow onset ends the flat pre-flow trend: a touch epoch starts with it.
  session.accidentalTouchEpochS = fmaxf(session.accidentalTouchEpochS,
      static_cast<int32_t>(observation.atMs - shot.startMs) / 1000.0f);
  const uint32_t firstDropAnchorMs = session.circuitClosedAtMs != 0U
                                         ? session.circuitClosedAtMs
                                         : session.startedAtMs;
  scaleProfileNoteEvent(
      ScaleProfileEvent::FIRST_DROP, millis(), 0, 0, observation.weightG,
      static_cast<uint32_t>(observation.atMs - firstDropAnchorMs), 0);
  if (std::isfinite(observation.weightG)) {
    shotCurveSampler.latchFirstDrop(observation.atMs, observation.weightG);
  }
  requestFirstDropBeep();
  notifyRetareFlowDetected(observation.atMs);
  if (first) {
    notifyWebhookFirstDrop(observation);
  }
}

void performAutomaticRetare() {
  if (session.retarePerformed || session.retareDisabled || !retareWindowOpen()) {
    return;
  }
  if (!requestRemoteRetare()) {
    return;
  }
  session.retarePerformed = true;
  emitImmediateCommandAlertIfBuzzer(AlertEvent::TARE);
  markRetareEnded(millis());
}
void initializeBbwProtection() {
  session.bbwProtectionEnabled = false;
  session.retareEnded = false;
  session.bbwProtectionEnded = false;
  session.flowDuringRetare = false;
  session.retareFlowFirstDetectedAtMs = 0;
  session.retarePerformed = false;
  session.retareAtMs = 0;
  session.retareDisabled = false;
  session.firstDropsBeepSent = false;
  session.firstDropObserved = false;
  session.retareEffectPending = false;
  resetFirstFlowDetector();

  if (!session.startedWithScale || session.config.timerOnly ||
      !session.automaticEnabled) {
    session.retareEnded = true;
    session.bbwProtectionEnded = true;
    return;
  }
  session.bbwProtectionEnabled = true;
  if (!session.config.autoRetare) {
    session.retareEnded = true;
  }
}

void serviceBbwProtectionPhases() {
  if (!session.active || !session.bbwProtectionEnabled) {
    return;
  }
  const uint32_t nowMs = millis();
  if (!session.retareEnded && session.config.autoRetare &&
      !session.retarePerformed && !retareWindowOpen()) {
    markRetareEnded(nowMs);
  }
  if (!session.bbwProtectionEnded &&
      elapsedMs(session.startedAtMs) >= session.config.bbwProtectionMs) {
    session.bbwProtectionEnded = true;
    resetDirectStopConfirmation();
  }
}
// Latches when the cut line leaves the target: raised once the shot reaches
// the target while f is above it, lowered once f has dropped below it.
void noteCutLineSegment(int32_t weightDg, int32_t shotMs, uint32_t atMs) {
  const corridor::Params &p = session.cut.params;
  const int32_t line = cutLineAtDg(shotMs);
  const bool reached = weightDg >= p.goalDg - session.cut.offsetDg;
  if (!session.extractionExtended && reached && line > p.goalDg) {
    session.extractionExtended = true;
    session.targetReachedEarly = true;
    session.targetReachedAtMs = atMs;
    shotCurveSampler.latchExtended(atMs, weightDg / 10.0f);
    addDebugEvent(DebugCategory::SCALE, DebugCode::FAST_EXTRACTION_ENTERED,
                  weightDg * 10, shotMs);
    if (runtimeConfig.buzzerExtendedPulseRate !=
        static_cast<uint8_t>(ExtendedPulseRate::OFF)) {
      emitAlert(AlertEvent::EXTENDED_PULSE, session.id);
    }
  } else if (!session.extractionExtended && !session.slowExtractionExtended &&
             !reached && line < p.goalDg) {
    session.slowExtractionExtended = true;
    shotCurveSampler.latchExtended(atMs, weightDg / 10.0f);
    addDebugEvent(DebugCategory::SCALE, DebugCode::SLOW_EXTRACTION_ENTERED,
                  weightDg * 10, shotMs);
    if (runtimeConfig.buzzerSlowExtendedPulseRate !=
        static_cast<uint8_t>(ExtendedPulseRate::OFF)) {
      emitAlert(AlertEvent::EXTENDED_PULSE, session.id);
    }
  }
}

// Backstop direct law: two consecutive fresh samples at or above f(t) - r.
void considerDirectStopSample(float weight, uint32_t receivedAtMs,
                              uint32_t packetSequence,
                              uint32_t connectionGeneration) {
  if (!shouldTrackWeight() || packetSequence == 0 || !std::isfinite(weight)) {
    return;
  }
  if (bbwWeightStopInhibited()) {
    // Trace the first reading the protection window keeps above the line.
    int32_t shotMs = 0;
    if (shotTrace().capturing() && !(session.traceLatches & TRACE_LATCH_PROTECTION) &&
        shotTimeAtMs(receivedAtMs, shotMs) &&
        gramsToDg(weight) >= cutLineAtDg(shotMs) - session.cut.offsetDg) {
      session.traceLatches |= TRACE_LATCH_PROTECTION;
      shotTrace().note(ShotTraceKind::STOP_BLOCKED, millis(), 1,
                       SHOT_TRACE_BLOCKED_PROTECTION);
    }
    return;
  }

  const bool overload = fabsf(weight) > MAX_AUTOMATION_WEIGHT_G;
  const bool active =
      session.weightControlState == WeightControlState::ACTIVE;
  const bool validating =
      session.weightControlState == WeightControlState::VALIDATING;
  if (!overload && !active) {
    return;
  }
  if (overload && !active && !validating) {
    return;
  }
  int32_t shotMs = 0;
  const bool timed = shotTimeAtMs(receivedAtMs, shotMs);
  const int32_t weightDg = gramsToDg(weight);
  if (timed && !overload) noteCutLineSegment(weightDg, shotMs, receivedAtMs);
  const int32_t lineDg = timed ? cutLineAtDg(shotMs) : 0;
  const bool overThreshold = timed && weightDg >= lineDg - session.cut.offsetDg;
  if (timed && !overload && !(session.traceLatches & TRACE_LATCH_SLOW_WINDOW) &&
      shotMs >= session.cut.params.slowAfterMs) {
    // The slow-window end decides between the target and a slow extension.
    session.traceLatches |= TRACE_LATCH_SLOW_WINDOW;
    const uint16_t flags =
        (weightDg >= session.cut.params.goalDg - session.cut.offsetDg ? SHOT_TRACE_SLOW_REACHED : 0) |
        (session.slowExtractionExtended ? SHOT_TRACE_SLOW_EXTENDED : 0) |
        (session.extractionExtended ? SHOT_TRACE_SLOW_FAST_EXTENDED : 0) |
        (overThreshold ? SHOT_TRACE_SLOW_OVER_LINE : 0);
    shotTrace().note(ShotTraceKind::SLOW_WINDOW_END, millis(), shotTraceSaturate16(weightDg),
                     static_cast<uint16_t>(lineDg), true,
                     shotTraceSaturate16(session.cut.offsetDg), flags);
  }
  if (!overThreshold && !overload) {
    resetDirectStopConfirmation();
    return;
  }

  const bool consecutive = session.thresholdConfirmations > 0 &&
      connectionGeneration == session.lastThresholdConnectionGeneration &&
      packetSequence == session.lastThresholdPacketSequence + 1U &&
      static_cast<int32_t>(receivedAtMs - session.lastThresholdAtMs) > 0 &&
      static_cast<uint32_t>(receivedAtMs - session.lastThresholdAtMs) <=
          DIRECT_STOP_CONFIRMATION_WINDOW_MS;
  if (!consecutive && session.thresholdConfirmations != 0)
    shotTrace().noteConfirmationReset();
  session.thresholdConfirmations = consecutive
      ? static_cast<uint8_t>(session.thresholdConfirmations + 1U)
      : 1U;
  if (!overload && !(session.traceLatches & TRACE_LATCH_OVER_LINE)) {
    // The first reading over the line, whether or not it ever confirms.
    session.traceLatches |= TRACE_LATCH_OVER_LINE;
    shotTrace().note(ShotTraceKind::OVER_LINE, millis(), shotTraceSaturate16(weightDg),
                     static_cast<uint16_t>(lineDg), true,
                     shotTraceSaturate16(session.cut.offsetDg),
                     session.thresholdConfirmations);
  }
  session.lastThresholdAtMs = receivedAtMs;
  session.lastThresholdPacketSequence = packetSequence;
  session.lastThresholdConnectionGeneration = connectionGeneration;

  if (session.thresholdConfirmations < DIRECT_STOP_CONFIRMATION_SAMPLES) {
    return;
  }

  if (overload) {
    session.directStopPending = true;
    session.directStopReason = EndReason::WEIGHT_ANOMALY;
    weightStreamState = WeightStreamState::OVERLOAD;
    session.calibrationEligible = false;
    setWeightControlState(WeightControlState::FAULT_STOPPED);
    addDebugEvent(DebugCategory::SCALE,
                  DebugCode::SCALE_OVERLOAD_CONFIRMED,
                  static_cast<int32_t>(weight));
    return;
  }

  session.directStopPending = true;
  session.directStopReason = weightCutReasonNow();
  // Arg2 is the firing law: 1 = confirmed weight samples (direct),
  // 2 = predicted end time.
  addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_THRESHOLD_CONFIRMED,
                weightDg * 10, 1);
}
void armNoScaleShotGuard() {
  if (noScaleShotGuardArmed) {
    return;
  }
  noScaleShotGuardArmed = true;
  addDebugEvent(DebugCategory::STATE, DebugCode::NO_SCALE_SHOT_GUARD_ARMED);
}

void consumeNoScaleShotGuard() {
  if (noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode)) {
    noScaleShotGuardArmed = true;
    return;
  }
  noScaleShotGuardArmed = false;
  noScaleShotGuardActivityAtMs = millis();
  addDebugEvent(DebugCategory::STATE, DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED);
}

bool noScaleShotGuardWouldBlock(const GuardInputs &inputs) {
  if (!noScaleBbwEnabled(runtimeConfig.noScaleBbwMode) || inputs.scaleUsable ||
      !noScaleShotGuardArmed) {
    return false;
  }
  return !effectiveRuntimeConfig().timerOnly;
}

void maybeEmitManualNoScaleBeep(const GuardInputs &inputs) {
  if (!runtimeConfig.buzzerManualNoScaleBeep || inputs.scaleUsable) {
    return;
  }
  const RuntimeConfig effective = effectiveRuntimeConfig();
  if (effective.timerOnly) {
    return;
  }
  emitAlert(AlertEvent::MANUAL_NO_SCALE);
}

void blockNoScaleShotGuard() {
  noScaleShotGuardHold = false;
  if (!noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode)) {
    consumeNoScaleShotGuard();
  }
  addDebugEvent(DebugCategory::STATE, DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED);
}

constexpr uint8_t NO_SCALE_REQUIRE_BYPASS_CYCLES = 3;
constexpr uint32_t NO_SCALE_REQUIRE_BYPASS_WINDOW_MS = 2000;

void resetNoScaleRequireBypassGesture() {
  noScaleRequireBypassReady = false;
  noScaleRequireBypassHoldSeen = false;
  noScaleRequireBypassCycles = 0;
  noScaleRequireBypassStartedAtMs = 0;
}

void temporarilyAllowNoScaleRequireMode() {
  noScaleShotGuardArmed = false;
  noScaleShotGuardActivityAtMs = millis();
  noScaleShotGuardHold = false;
  // Consume the release that completed the gesture. A later, fresh
  // activation may start a manual shot.
  noScaleShotGuardNeedsFreshActivator = true;
  noScaleRequireBypassCompletedThisLoop = true;
  // Confirmation must be heard even if a recent disconnect or one of the
  // blocked attempts is still sounding.
  localBuzzer.stopIfCue(BuzzerCue::SCALE_LOST);
  localBuzzer.stopIfCue(BuzzerCue::NO_SCALE);
  emitAlert(AlertEvent::SCALE_CONNECTED);
  addDebugEvent(DebugCategory::STATE, DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED);
  resetNoScaleRequireBypassGesture();
}

void serviceNoScaleRequireBypassGesture(const GuardInputs &inputs) {
  const bool eligible =
      noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode) &&
      !inputs.scaleUsable && !session.active && noScaleShotGuardArmed &&
      !effectiveRuntimeConfig().timerOnly;
  if (!eligible) {
    resetNoScaleRequireBypassGesture();
    return;
  }

  // The gesture must begin from a settled OFF state.
  if (!noScaleRequireBypassReady) {
    if (inputs.stablyOff) {
      noScaleRequireBypassReady = true;
    }
    return;
  }

  // Count only the debounced physical activator. holdActive intentionally
  // includes raw/logical continuity for circuit safety and can otherwise make
  // one physical click look like more than one gesture cycle.
  if (inputs.physicalOn) {
    if (!noScaleRequireBypassHoldSeen) {
      if (noScaleRequireBypassCycles == 0 ||
          elapsedMs(noScaleRequireBypassStartedAtMs) >
              NO_SCALE_REQUIRE_BYPASS_WINDOW_MS) {
        noScaleRequireBypassCycles = 0;
        noScaleRequireBypassStartedAtMs = millis();
      }
      noScaleRequireBypassHoldSeen = true;
    }
    return;
  }

  if (!noScaleRequireBypassHoldSeen || !inputs.stablyOff) {
    return;
  }
  noScaleRequireBypassHoldSeen = false;
  if (elapsedMs(noScaleRequireBypassStartedAtMs) >
      NO_SCALE_REQUIRE_BYPASS_WINDOW_MS) {
    noScaleRequireBypassCycles = 0;
    noScaleRequireBypassStartedAtMs = 0;
    return;
  }
  ++noScaleRequireBypassCycles;
  if (noScaleRequireBypassCycles >= NO_SCALE_REQUIRE_BYPASS_CYCLES) {
    temporarilyAllowNoScaleRequireMode();
  }
}

void serviceNoScaleShotGuard(const GuardInputs &inputs) {
  if (inputs.scaleAvailable && !noScaleShotGuardScaleWasAvailable) {
    armNoScaleShotGuard();
  }
  noScaleShotGuardScaleWasAvailable = inputs.scaleAvailable;
  if (noScaleShotGuardHold) {
    if (!inputs.holdActive) {
      noScaleShotGuardHold = false;
    } else if (noScaleShotGuardWouldBlock(inputs) &&
               elapsedMs(noScaleShotGuardHoldAtMs) >
                   inputs.blockedHoldTimeoutMs) {
      if (!noScaleBbwRequiresScale(runtimeConfig.noScaleBbwMode)) {
        blockNoScaleShotGuard();
      }
    }
  }
  if (!noScaleShotGuardArmed && !session.active &&
      noScaleShotGuardActivityAtMs != 0 &&
      elapsedMs(noScaleShotGuardActivityAtMs) >=
          runtimeConfig.lastShotCooldownMs) {
    armNoScaleShotGuard();
  }
  serviceNoScaleRequireBypassGesture(inputs);
}

bool cupStartGuardWouldBlock(const GuardInputs &inputs) {
  if (!inputs.scaleUsable || inputs.cup == CupPresenceState::PRESENT) {
    return false;
  }
  const RuntimeConfig effective = effectiveRuntimeConfig();
  return effective.cupProtectionEnabled && effective.requireCupToStart &&
         !effective.timerOnly;
}

// Read-only composition for the stopper. Does not arm, consume, or call machine.
bool guardsWouldBlockActivatorDrive(const GuardInputs &inputs) {
  return noScaleShotGuardHold || noScaleShotGuardWouldBlock(inputs) ||
         cupStartGuardWouldBlock(inputs);
}

void serviceCupStartGuard(const GuardInputs &inputs) {
  if (!cupStartGuardHold) {
    return;
  }
  const RuntimeConfig effective = effectiveRuntimeConfig();
  if (!effective.cupProtectionEnabled || !effective.requireCupToStart ||
      effective.timerOnly) {
    cupStartGuardHold = false;
    return;
  }
  if (inputs.scaleUsable && inputs.cup == CupPresenceState::PRESENT) {
    cupStartGuardHold = false;
    return;
  }
  if (inputs.holdActive &&
      elapsedMs(cupStartGuardHoldAtMs) > inputs.blockedHoldTimeoutMs) {
    cupStartGuardHold = false;
    addDebugEvent(DebugCategory::STATE, DebugCode::CUP_START_GUARD_BLOCKED,
                  weightToCentigrams(inputs.currentWeightG));
    emitAlert(AlertEvent::CUP_START_BLOCKED);
  }
}
// "If the scale drops now" for an idle preset: its fixed time, recent shots
// or the window midpoint. Cached until the preset, its epoch, window, target
// or the shot log changes, so status publication never rescans the log.
corridor::LossDeadline idleScaleLossDeadline(const ShotPreset &preset) {
  static struct {
    uint8_t id = 0;
    uint32_t epoch = 0, generation = 0, fixedMs = 0, t1 = 0, t2 = 0, delay = 0, wall = 0;
    uint16_t goal = 0;
    corridor::LossDeadline deadline;
  } cache;
  const uint32_t generation = shotStoreDirtyGeneration.load(std::memory_order_acquire);
  if (cache.id == preset.id && cache.epoch == preset.learningEpoch &&
      cache.generation == generation && cache.fixedMs == preset.scaleLossFixedMs &&
      cache.t1 == preset.fastBeforeMs && cache.t2 == preset.slowAfterMs &&
      cache.delay == preset.bbwProtectionMs && cache.wall == preset.operationalWallMs &&
      cache.goal == preset.goalWeightDg)
    return cache.deadline;
  corridor::Params p;
  p.goalDg = preset.goalWeightDg;
  p.delayMs = static_cast<int32_t>(preset.bbwProtectionMs);
  p.fastBeforeMs = static_cast<int32_t>(preset.fastBeforeMs);
  p.slowAfterMs = static_cast<int32_t>(preset.slowAfterMs);
  p.wallMs = static_cast<int32_t>(preset.operationalWallMs);
  corridor::LossInputs in;
  in.fixedMs = preset.scaleLossFixedMs;
  in.beforeFirstDrops = true;
  {
    TaskLockGuard lock(shotStoreMutex);
    in.history = shotLog.arrivalStats(preset);
  }
  cache = {preset.id, preset.learningEpoch, generation, preset.scaleLossFixedMs,
           preset.fastBeforeMs, preset.slowAfterMs, preset.bbwProtectionMs,
           preset.operationalWallMs, preset.goalWeightDg, corridor::lossDeadline(p, in)};
  return cache.deadline;
}

// Scale-loss backup v2 (plan §4.9): armed on every automatic BBW shot while
// the device switch is on; the deadline is chosen when the stream drops.
void armAutoToManualGuardForAutomaticBrew() {
  if (!session.config.scaleLossEnabled ||
      session.config.timerOnly || !session.startedWithScale ||
      session.weightControlState == WeightControlState::INACTIVE) {
    return;
  }
  session.autoToManualGuardArmed = true;
  addDebugEvent(DebugCategory::SCALE, DebugCode::AUTO_TO_MANUAL_GUARD_ARMED,
                static_cast<int32_t>(session.config.scaleLossFixedMs));
  if (session.weightControlState == WeightControlState::SUSPENDED)
    enforceScaleLossBackup();
}

// First applicable: the preset's fixed time, cut now, a projection from the
// last valid flow (<= 3 s ahead), recent shots, or the window midpoint;
// clamped to [max(last sample, delay), wall] and fired at that command time.
void enforceScaleLossBackup() {
  if (!session.autoToManualGuardArmed || session.autoToManualGuardEnforced) return;
  const corridor::CorridorState &cut = session.cut;
  corridor::LossInputs in;
  int32_t nowMs = 0;
  shotTimeAtMs(millis(), nowMs);
  in.nowMs = nowMs;
  in.lastSampleMs = nowMs;
  if (session.hasWeightAnchor) shotTimeAtMs(session.lastAcceptedWeightAtMs, in.lastSampleMs);
  in.fixedMs = session.config.scaleLossFixedMs;
  in.beforeFirstDrops = !session.firstDropObserved;
  in.crossingValid = cut.crossingValid;
  in.crossingMs = cut.crossingMs;
  in.history = cut.history;
  const corridor::LossDeadline deadline = corridor::lossDeadline(cut.params, in);
  session.scaleLossSource = static_cast<uint8_t>(deadline.source);
  session.scaleLossBlind = !cut.crossingValid;
  session.autoToManualGuardDeadlineAtMs =
      millis() + static_cast<uint32_t>(deadline.deadlineMs - nowMs);
  session.autoToManualGuardEnforced = true;
  latchAtmCurveFromSession(millis());
  addDebugEvent(DebugCategory::SCALE, DebugCode::AUTO_TO_MANUAL_GUARD_ENFORCED,
                nowMs, deadline.deadlineMs);
}

// Time left before the scale-loss backup stops the shot (0 once due).
uint32_t scaleLossBackupRemainingMs() {
  const int32_t left =
      static_cast<int32_t>(session.autoToManualGuardDeadlineAtMs - millis());
  return left > 0 ? static_cast<uint32_t>(left) : 0U;
}

bool autoToManualGuardDeadlineDue() {
  if (!session.active || !session.autoToManualGuardEnforced) {
    return false;
  }
  return static_cast<int32_t>(millis() -
                              session.autoToManualGuardDeadlineAtMs) >= 0;
}
bool automaticScaleStopDue() {
  if (session.config.timerOnly || stopperState != StopperState::BREW ||
      bbwWeightStopInhibited()) {
    return false;
  }

  const bool holding = session.accidentalTouchHolding;
  if (holding && accidentalTouchSessionActive() &&
      session.config.touchStopFallbackEnabled && session.touchStop.samples >= 3 &&
      !session.awaitingPostTareBaseline && !scaleAutomationUnavailableForSession()) {
    const TouchStopEvidence &evidence = session.touchStop;
    float threshold;
    const EndReason policy = touchStopPolicy(threshold, millis());
    if (policy == evidence.policy &&
        evidence.connectionGeneration == session.ownedConnectionGeneration &&
        static_cast<int32_t>(millis() - evidence.lastAtMs) >= 0 &&
        elapsedMs(evidence.lastAtMs) <= MAX_AUTOMATION_WEIGHT_AGE_MS &&
        static_cast<uint32_t>(evidence.lastAtMs - evidence.firstAtMs) >=
            TOUCH_STOP_FALLBACK_MS) {
      session.calibrationEligible = false;
      session.directStopPending = true;
      session.directStopReason = EndReason::TOUCH_WEIGHT_FALLBACK;
      return true;
    }
  }
  const bool directStopFresh = session.directStopPending &&
      session.thresholdConfirmations >= DIRECT_STOP_CONFIRMATION_SAMPLES &&
      static_cast<int32_t>(millis() - session.lastThresholdAtMs) >= 0 &&
      elapsedMs(session.lastThresholdAtMs) <= MAX_AUTOMATION_WEIGHT_AGE_MS;
  const bool directStopHonored =
      directStopFresh &&
      (session.directStopReason == EndReason::WEIGHT_ANOMALY ||
       session.weightControlState == WeightControlState::ACTIVE);
  // An overload ends the shot even while a touch hold is latched.
  if (directStopHonored &&
      (!holding || session.directStopReason == EndReason::WEIGHT_ANOMALY)) {
    return true;
  }

  if (holding) {
    return false;
  }

  if (!session.receivedFreshWeightInCycle ||
      session.weightControlState != WeightControlState::ACTIVE ||
      currentWeightSequence == session.weightSequenceAtStart ||
      scaleAutomationUnavailableForSession()) {
    return false;
  }
  // Timed law: the predicted stop-command time t* has arrived.
  if (cycleElapsedSeconds() < shot.expectedEndS) {
    return false;
  }
  session.directStopPending = true;
  session.directStopReason = weightCutReasonNow();
  addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_THRESHOLD_CONFIRMED,
                gramsToDg(session.lastAcceptedWeightG) * 10, 2);
  return true;
}

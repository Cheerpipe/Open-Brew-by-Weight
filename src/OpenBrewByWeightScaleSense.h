#pragma once

#include "OpenBrewByWeightScaleTypes.h"
#include "OpenBrewByWeightShotLogTypes.h"

// =============================================================================
// LAYER: Scale (sense / trajectory / first-drop)
// =============================================================================
// WHAT: Shot weight trajectory and first-drop detector. Predicts against a
//       brew-injected target. Cup presence is OpenBrewByWeightCupPresence.h.
//       No GPIO. No heap; trajectory is BSS.
//
// BOUNDARY: Weight stream only. Does not call brew or cup — the stopper
// applies those effects. Must not include or know paddle/momentary/reed or
// machine actuators. Never branch on MachineType or PaddleMode.

void resetShotTrajectory(uint32_t startedAtMs) {
  shot.startMs = startedAtMs;
  shot.expectedEndS = NO_CUT_PREDICTION_S;
  shot.datapoints = 0;
  shot.automaticBrew = false;
  shotCurveSampler.reset(startedAtMs);
}

void rejectScaleSample(DebugCode code, float weightG, float referenceG = 0.0f) {
  markShotCurveBreak();
  addDebugEvent(DebugCategory::SCALE, code, weightToCentigrams(weightG),
                weightToCentigrams(referenceG));
}

void armPostTareBaselineWindow() {
  markShotCurveBreak();
  session.awaitingPostTareBaseline = true;
  session.softwareZeroG = 0.0f;
  session.lateTareDroppedZeroG = 0.0f;
  session.postTareBaselineDeadlineMs =
      millis() + session.config.postTareBaselineGraceMs;
  session.hasWeightAnchor = false;
  session.recoveryConfirmations = 0;
  session.recoveryLastAtMs = 0;
  session.recoveryLastPacketSequence = 0;
  resetWeightTrend();
  if (session.weightControlState == WeightControlState::VALIDATING) {
    setWeightControlState(WeightControlState::ACTIVE);
  }
}

bool expirePostTareBaselineIfNeeded(float softwareZeroG = NAN) {
  if (!session.awaitingPostTareBaseline) {
    return false;
  }
  if (static_cast<int32_t>(millis() - session.postTareBaselineDeadlineMs) <
      0) {
    return false;
  }
  session.awaitingPostTareBaseline = false;
  session.retareEffectPending = false;
  scaleProfileNoteEvent(ScaleProfileEvent::TARE_BASELINE_TIMEOUT, millis(),
                        session.ownedConnectionGeneration, 0, softwareZeroG,
                        session.appliedTareRequestId,
                        static_cast<uint32_t>(IdleTareReason::EFFECT_UNCONFIRMED));
  addDebugEvent(DebugCategory::SCALE,
                DebugCode::SCALE_POST_TARE_BASELINE_TIMEOUT,
                static_cast<int32_t>(session.id),
                static_cast<int32_t>(session.config.postTareBaselineGraceMs));
  return true;
}

bool acceptWeightIntoTrajectory(float weight, uint32_t receivedAtMs,
                                uint32_t packetSequence) {
  if (shot.datapoints > 0 && receivedAtMs == session.lastAcceptedWeightAtMs) {
    // Re-transmitted timestamp: counts as freshness for staleness watchers,
    // but a zero-duration sample must not widen the trajectory fit.
    session.receivedFreshWeightInCycle = true;
    session.lastAcceptedWeightG = weight;
    session.lastAcceptedPacketSequence = packetSequence;
    serialTraceCategoryf(LogLevel::DEBUG, DebugCategory::SCALE,
                         "%.2fg, t=dup, kept freshness only", weight);
    return true;
  }
  size_t index;
  if (shot.datapoints < MAX_SHOT_DATAPOINTS) {
    index = shot.datapoints++;
  } else {
    memmove(&shot.timeS[0], &shot.timeS[1],
            (MAX_SHOT_DATAPOINTS - 1U) * sizeof(float));
    memmove(&shot.weight[0], &shot.weight[1],
            (MAX_SHOT_DATAPOINTS - 1U) * sizeof(float));
    index = MAX_SHOT_DATAPOINTS - 1U;
  }
  shot.timeS[index] =
      static_cast<uint32_t>(receivedAtMs - shot.startMs) / 1000.0f;
  shot.weight[index] = weight;
  shotCurveSampler.accept(weight, receivedAtMs);
  session.receivedFreshWeightInCycle = true;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightAtMs = receivedAtMs;
  session.lastAcceptedWeightG = weight;
  session.lastAcceptedPacketSequence = packetSequence;
  updateCutPrediction(weight, receivedAtMs);

  serialTraceCategoryf(LogLevel::DEBUG, DebugCategory::SCALE,
                       "%.2fg, t=%.2fs, expected end=%.2fs",
                       weight, shot.timeS[index], shot.expectedEndS);
  return true;
}

FirstFlowObservation considerScaleFlowMarkers(float weight,
                                               uint32_t receivedAtMs,
                                               uint32_t packetSequence) {
  if (!session.active || !session.startedWithScale || session.firstDropMs != 0) {
    return {};
  }
  if (!session.scaleBaselineReady) {
    if (fabsf(weight) > FIRST_DROP_BASELINE_SETTLE_G) {
      return {};
    }
    session.scaleBaselineG = weight;
    session.scaleBaselineReady = true;
    return {};
  }
  if (session.firstFlow.phase == FirstFlowPhase::SEEKING &&
      session.firstFlow.confirmations == 0 &&
      fabsf(weight) <= FIRST_DROP_BASELINE_LOCK_G &&
      fabsf(weight) <= fabsf(session.scaleBaselineG)) {
    session.scaleBaselineG = weight;
  }

  const bool seekingStreak = session.firstFlow.phase == FirstFlowPhase::SEEKING &&
      session.firstFlow.confirmations > 0;
  const FirstFlowClass classified =
      stepFirstFlow(session.firstFlow, weight, receivedAtMs, packetSequence,
                    session.scaleBaselineG, runtimeConfig.minimumCupWeightG);
  if (seekingStreak && classified == FirstFlowClass::NONE &&
      static_cast<uint32_t>(receivedAtMs - session.firstFlow.lastFaAtMs) >=
          1000U) {
    // A confirmation streak died sub-threshold without firing. Args carry
    // the diagnosis: effective threshold (cg) and σ̂ (cg) separate an
    // inflated noise floor (vibration) from a healthy 0.3 g floor, without
    // which a mistuned threshold is indistinguishable from a dead sensor.
    // Rate-limited so a noisy counter cannot evict the real first drop.
    session.firstFlow.lastFaAtMs = receivedAtMs;
    scaleProfileNoteEvent(
        ScaleProfileEvent::FIRST_DROP_SEEKING_FA, millis(),
        session.ownedConnectionGeneration, packetSequence, weight,
        static_cast<uint32_t>(firstFlowEffectiveThresholdG(session.firstFlow) *
                              100.0f),
        static_cast<uint32_t>(sqrtf(session.firstFlow.noiseVarHatG) * 100.0f));
  }
  if (classified == FirstFlowClass::FIRE) {
    return {session.firstFlow.candidateMs != 0 ? session.firstFlow.candidateMs
                                               : receivedAtMs,
            session.firstFlow.candidateMs != 0
                ? session.firstFlow.candidateWeightG
                : weight};
  }
  return {};
}

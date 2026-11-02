#pragma once

#include "OpenBrewByWeightScaleTypes.h"

// =============================================================================
// LAYER: Cup (presence FSM)
// =============================================================================
// WHAT: PRESENT / ABSENT from weight hysteresis on the scale stream. Tare does
//       not change state. No heap; state is BSS.
//
// BOUNDARY: Scale-weight sensing only. Must not talk to machine, paddle,
// momentary, reed, or brew actuators. Emits CupPresenceEvent; the stopper
// decides whether REMOVED cuts the shot. No machine type / paddle details.

namespace {

// Informational placement delta, owned and reset with the detected cup.
struct CupStableWeight {
  float absoluteG = 0.0f;
  uint32_t atMs = 0;
  bool valid = false;
};

struct CupWeightRuntime {
  CupStableWeight absent;
  CupStableWeight present;
  float weightG = 0.0f;
  float emptyMinimumG = 0.0f;
  float emptyMaximumG = 0.0f;
  uint32_t emptyStartedAtMs = 0;
  uint32_t emptyLastAtMs = 0;
  uint8_t emptySamples = 0;
  bool emptyValid = false;
  // Stable plateau rejected as off the empty reference, for diagnostics only.
  float offAnchorMinimumG = 0.0f;
  float offAnchorMaximumG = 0.0f;
  uint32_t offAnchorStartedAtMs = 0;
  uint32_t offAnchorLastAtMs = 0;
  uint8_t offAnchorSamples = 0;
  bool offAnchorReported = false;
  bool unloadQualified = false;
  uint8_t unloadSamples = 0;
  uint32_t unloadAtMs = 0;
  uint32_t unloadSequence = 0;
  uint32_t sampleAtMs = 0;
  uint32_t sampleSequence = 0;
  uint32_t connectionGeneration = 0;
  uint32_t settingsGeneration = 0;
  uint32_t pendingId = 0;
  uint32_t pendingPlacementId = 0;
  uint32_t pendingAtMs = 0;
  float pendingEmptyAnchorG = NAN;
  uint32_t droppedEvents = 0;
  bool valid = false;
  bool pendingValid = false;
};

struct CupPresenceRuntime {
  CupWeightRuntime weight;
  CupPresenceState state = CupPresenceState::ABSENT;
  bool holdTransitions = false;
  bool inNegativeHole = false;
  // True after a tare while the cup is already PRESENT. 0 g then means "cup
  // tared on the pan", so removal requires the negative hole. Untared PRESENT
  // treats a stable empty pan (~0 g) as REMOVED.
  bool taredWhilePresent = false;
  bool referenceUncertain = false;
  // A trusted placement or uncertain tare closes initial-zero recovery.
  bool initialZeroRecoveryAllowed = true;
  uint8_t removedConfirmations = 0;
  uint8_t placeStabilitySamples = 0;
  uint32_t lastRemovedAtMs = 0;
  uint32_t lastRemovedPacketSequence = 0;
  uint32_t removedStartedAtMs = 0;
  uint32_t placeStabilityStartedAtMs = 0;
  uint32_t placeLastSampleAtMs = 0;
  uint32_t placementId = 0;
  float holeWeightG = 0.0f;
  // Last reading that was not a removal; a lift must step down from it.
  float lastPresentWeightG = NAN;
  float placeCandidateWeightG = 0.0f;
  float placeMinimumG = 0.0f;
  float placeMaximumG = 0.0f;
  float occupiedReferenceG = 0.0f;
  float occupiedMinimumG = 0.0f;
  float occupiedMaximumG = 0.0f;
  float occupiedPlacementThresholdG = 0.0f;
  // Empty-pan coordinates outlive sample freshness and placement snapshots.
  float emptyAnchorG = NAN;
};

CupPresenceRuntime cupPresence;

void resetCupPlaceStabilityStreak() {
  cupPresence.placeStabilitySamples = 0;
  cupPresence.placeStabilityStartedAtMs = 0;
  cupPresence.placeLastSampleAtMs = 0;
  cupPresence.placeCandidateWeightG = 0.0f;
  cupPresence.placeMinimumG = 0.0f;
  cupPresence.placeMaximumG = 0.0f;
}

}  // namespace

CupPresenceState cupPresenceState() { return cupPresence.state; }
bool cupPresenceIsTared() {
  return cupPresence.taredWhilePresent && !cupPresence.referenceUncertain;
}
bool cupPresenceIsKnown() { return !cupPresence.referenceUncertain; }
float cupPresenceReferenceG() { return cupPresence.occupiedReferenceG; }
float cupPresenceMinimumG() { return cupPresence.occupiedMinimumG; }
float cupPresenceMaximumG() { return cupPresence.occupiedMaximumG; }
float cupPresencePlacementThresholdG() { return cupPresence.occupiedPlacementThresholdG; }
uint32_t cupPresencePlacementId() { return cupPresence.placementId; }

void invalidateCupWeight() {
  cupPresence.weight.valid = false;
  cupPresence.weight.pendingValid = false;
  cupPresence.weight.absent.valid = false;
  cupPresence.weight.present.valid = false;
  cupPresence.weight.emptyValid = false;
  cupPresence.weight.emptySamples = 0;
  cupPresence.weight.offAnchorSamples = 0;
  cupPresence.weight.unloadSamples = 0;
  cupPresence.weight.unloadQualified = false;
  if (cupPresence.placementId == 0) cupPresence.inNegativeHole = false;
}

bool cupWeightNearKnownEmpty(float weight) {
  const float band = fminf(runtimeConfig.retareStabilityToleranceG,
      fminf(FIRST_DROP_BASELINE_SETTLE_G, runtimeConfig.minimumCupWeightG / 2.0f));
  return cupPresence.placementId != 0 && !cupPresence.initialZeroRecoveryAllowed &&
      std::isfinite(cupPresence.emptyAnchorG) &&
      !cupPresence.referenceUncertain && !cupPresence.holdTransitions &&
      cupPresence.weight.pendingId == 0 && weight <= cupPresence.emptyAnchorG + band;
}

// Ages the current rejected plateau and profiles it once when it is stable.
void noteOffAnchorEmptyWeight(float weight, uint32_t atMs, float referenceG) {
  auto &mass = cupPresence.weight;
  if (mass.offAnchorSamples == 0 ||
      static_cast<uint32_t>(atMs - mass.offAnchorLastAtMs) > runtimeConfig.retareStabilityMaxGapMs ||
      fmaxf(weight, mass.offAnchorMaximumG) - fminf(weight, mass.offAnchorMinimumG) >
          runtimeConfig.retareStabilityToleranceG) {
    mass.offAnchorSamples = 0;
    mass.offAnchorReported = false;
    mass.offAnchorStartedAtMs = atMs;
    mass.offAnchorMinimumG = mass.offAnchorMaximumG = weight;
  }
  mass.offAnchorMinimumG = fminf(mass.offAnchorMinimumG, weight);
  mass.offAnchorMaximumG = fmaxf(mass.offAnchorMaximumG, weight);
  mass.offAnchorLastAtMs = atMs;
  if (mass.offAnchorSamples < UINT8_MAX) ++mass.offAnchorSamples;
  if (!mass.offAnchorReported && mass.offAnchorSamples >= runtimeConfig.retareStabilitySamples &&
      static_cast<uint32_t>(atMs - mass.offAnchorStartedAtMs) >=
          runtimeConfig.retareStabilityMinDurationMs) {
    mass.offAnchorReported = true;
    scaleProfileNoteEvent(ScaleProfileEvent::CUP_ANCHOR_REJECTED, millis(),
                          mass.connectionGeneration, mass.sampleSequence, weight,
                          cupPresence.placementId,
                          static_cast<uint32_t>(weightToCentigrams(weight - referenceG)));
  }
}

void observeEmptyCupWeight(float weight, uint32_t atMs, bool initialRecovery,
                           bool idle) {
  auto &mass = cupPresence.weight;
  const bool anchored = std::isfinite(cupPresence.emptyAnchorG);
  const float referenceG = anchored ? cupPresence.emptyAnchorG : 0.0f;
  const float movementG = initialRecovery || cupPresence.placementId == 0
      ? fminf(5.0f, runtimeConfig.minimumCupWeightG / 2.0f) : 0.0f;
  const bool initialZero = initialRecovery && fabsf(referenceG) <= movementG;
  const float toleranceG = initialZero ? movementG
      : fmaxf(anchored ? runtimeConfig.retareStabilityToleranceG
                       : FIRST_DROP_BASELINE_SETTLE_G, movementG);
  // After a lift the tare-translated anchor is only provisional: any plateau
  // too low to be pan plus a cup (or below the anchor) may become the empty
  // reference.
  const bool provisional = anchored && cupPresence.placementId != 0 &&
      (cupPresence.inNegativeHole || weight < referenceG - toleranceG);
  // An idle stable zero far from a large anchor is an external re-zero (the
  // scale's button): it becomes the empty reference instead of a deadlock.
  const bool externalZero = idle && anchored &&
      fabsf(referenceG) >= runtimeConfig.minimumCupWeightG &&
      fabsf(weight) <= fminf(5.0f, runtimeConfig.minimumCupWeightG / 2.0f);
  if (mass.pendingId != 0 || cupPresence.holdTransitions ||
      (cupPresence.inNegativeHole && !anchored &&
       weight >= cupPresence.holeWeightG + runtimeConfig.minimumCupWeightG) ||
      (!externalZero && provisional &&
       weight >= referenceG + runtimeConfig.minimumCupWeightG) ||
      ((!cupPresence.inNegativeHole || anchored) && !provisional && !externalZero &&
       fabsf(weight - (initialZero ? 0.0f : referenceG)) > toleranceG)) {
    // Intermediate upward loads can be a placement ramp. A downward
    // disturbance must settle back at the anchor before rearming placement.
    if (!anchored || weight < referenceG || mass.pendingId != 0 || cupPresence.holdTransitions)
      mass.emptyValid = false;
    mass.emptySamples = 0;
    if (mass.pendingId == 0 && !cupPresence.holdTransitions)
      noteOffAnchorEmptyWeight(weight, atMs, referenceG);
    else mass.offAnchorSamples = 0;
    return;
  }
  mass.offAnchorSamples = 0;
  if (mass.emptyValid) {
    // Requalify a small downward shift before the first cup placement.
    if (!anchored || weight >= referenceG - runtimeConfig.retareStabilityToleranceG)
      return;
    mass.emptyValid = false;
  }
  if (mass.emptySamples == 0 ||
      static_cast<uint32_t>(atMs - mass.emptyLastAtMs) > runtimeConfig.retareStabilityMaxGapMs ||
      fmaxf(weight, mass.emptyMaximumG) - fminf(weight, mass.emptyMinimumG) >
          runtimeConfig.retareStabilityToleranceG) {
    // Restart qualification without discarding the last stable plateau.
    if (mass.emptySamples != 0)
      scaleProfileNoteEvent(ScaleProfileEvent::CUP_QUALIFICATION_RESET, millis(),
                            mass.connectionGeneration, mass.sampleSequence, weight, 0,
                            static_cast<uint32_t>(atMs - mass.emptyLastAtMs > runtimeConfig.retareStabilityMaxGapMs
                                ? IdleTareReason::SAMPLE_GAP : IdleTareReason::UNSTABLE));
    mass.emptySamples = 0;
    mass.emptyStartedAtMs = atMs;
    mass.emptyMinimumG = mass.emptyMaximumG = weight;
  }
  mass.emptyMinimumG = fminf(mass.emptyMinimumG, weight);
  mass.emptyMaximumG = fmaxf(mass.emptyMaximumG, weight);
  mass.emptyLastAtMs = atMs;
  if (mass.emptySamples < UINT8_MAX) ++mass.emptySamples;
  if (mass.emptySamples >= runtimeConfig.retareStabilitySamples &&
      static_cast<uint32_t>(atMs - mass.emptyStartedAtMs) >=
          runtimeConfig.retareStabilityMinDurationMs) {
    if (externalZero) {
      scaleProfileNoteEvent(ScaleProfileEvent::CUP_ANCHOR_CHANGED, millis(),
                            mass.connectionGeneration, mass.sampleSequence, referenceG,
                            cupPresence.placementId,
                            static_cast<uint32_t>(weightToCentigrams(weight)));
      cupPresence.inNegativeHole = false;
      cupPresence.holeWeightG = 0.0f;
    }
    if (!anchored || provisional || externalZero || (movementG > 0.0f &&
                      weight < referenceG - runtimeConfig.retareStabilityToleranceG))
      cupPresence.emptyAnchorG = weight;
    mass.absent = CupStableWeight{cupPresence.emptyAnchorG, atMs, true};
    mass.emptyValid = true;
  }
}

// A changed tare reference also invalidates the reading a lift steps from.
void restoreCupTareReference(bool previouslyTared, float previousReferenceG) {
  cupPresence.lastPresentWeightG = NAN;
  if (cupPresence.state == CupPresenceState::PRESENT) {
    cupPresence.taredWhilePresent = previouslyTared;
    cupPresence.occupiedReferenceG = previousReferenceG;
    cupPresence.referenceUncertain = false;
  }
}

void markCupTareReferenceUncertain() {
  const ProfileControlObservation profile;
  invalidateCupWeight();
  cupPresence.emptyAnchorG = NAN;
  cupPresence.lastPresentWeightG = NAN;
  cupPresence.initialZeroRecoveryAllowed = false;
  if (cupPresence.state == CupPresenceState::PRESENT) {
    cupPresence.referenceUncertain = true;
  }
}

void resetCupSampleEvidence() {
  invalidateCupWeight();
  cupPresence.weight.sampleSequence = 0;
  resetCupPlaceStabilityStreak();
  cupPresence.removedConfirmations = 0;
  cupPresence.lastRemovedAtMs = 0;
  cupPresence.lastRemovedPacketSequence = 0;
}

void resetCupPresence() {
  const ProfileControlObservation profile;
  scaleProfileNoteEvent(ScaleProfileEvent::CUP_RESET, millis(),
                        cupPresence.weight.connectionGeneration, 0, NAN,
                        cupPresence.placementId, 0);
  cupPresence = CupPresenceRuntime{};
}

void resyncCupPresenceIfPanEmpty(float weight) {
  if (cupPresence.state != CupPresenceState::PRESENT ||
      cupPresence.taredWhilePresent || cupPresence.referenceUncertain) {
    return;
  }
  if (!std::isfinite(weight) || weight >= runtimeConfig.minimumCupWeightG) {
    return;
  }
  cupPresence.state = CupPresenceState::ABSENT;
  invalidateCupWeight();
  cupPresence.taredWhilePresent = false;
  cupPresence.inNegativeHole = false;
  cupPresence.removedConfirmations = 0;
  cupPresence.lastRemovedAtMs = 0;
  cupPresence.lastRemovedPacketSequence = 0;
  cupPresence.holeWeightG = 0.0f;
  cupPresence.occupiedReferenceG = 0.0f;
  resetCupPlaceStabilityStreak();
}

void holdCupPresenceTransitions(bool hold) {
  const ProfileControlObservation profile;
  cupPresence.holdTransitions = hold;
  cupPresence.weight.unloadSamples = 0;
  cupPresence.weight.unloadQualified = false;
  resetCupPlaceStabilityStreak();
  cupPresence.removedConfirmations = 0;
  cupPresence.lastRemovedAtMs = 0;
  cupPresence.lastRemovedPacketSequence = 0;
}

void notifyCupPresenceTare() {
  cupPresence.lastPresentWeightG = NAN;
  cupPresence.weight.unloadSamples = 0;
  cupPresence.weight.unloadQualified = false;
  cupPresence.removedConfirmations = 0;
  if (cupPresence.state == CupPresenceState::PRESENT) {
    cupPresence.initialZeroRecoveryAllowed = false;
    cupPresence.taredWhilePresent = true;
    cupPresence.occupiedReferenceG = 0.0f;
    cupPresence.referenceUncertain = false;
  } else {
    invalidateCupWeight();
  }
  cupPresence.inNegativeHole = false;
  cupPresence.holeWeightG = 0.0f;
  resetCupPlaceStabilityStreak();
}

// removalWhileAbsent: the caller vouches for a 0 g tare reference, so a
// confirmed reading at or below the removed threshold is a lift even when no
// placement was observed (the cup was already tared when the scale connected).
CupPresenceEvent feedCupPresence(float weight, uint32_t receivedAtMs,
                                 uint32_t packetSequence, bool allowPlacement = true,
                                 bool allowFastReplacement = false,
                                 bool protectedPartialUnload = false,
                                 bool removalWhileAbsent = false) {
  if (!std::isfinite(weight)) {
    return CupPresenceEvent::NONE;
  }

  const float minCupG = runtimeConfig.minimumCupWeightG;
  const float removedG = runtimeConfig.cupRemovedWeightG;
  const bool initialPlacement = allowFastReplacement
      ? cupPresence.initialZeroRecoveryAllowed : cupPresence.placementId == 0;
  const float zeroBandG = allowFastReplacement
      ? fminf(5.0f, minCupG / 2.0f) : FIRST_DROP_BASELINE_SETTLE_G;
  auto &mass = cupPresence.weight;
  const bool nearEmpty = allowFastReplacement && cupWeightNearKnownEmpty(weight);
  if (nearEmpty) {
    const bool consecutive = mass.unloadSamples != 0 &&
        receivedAtMs - mass.unloadAtMs <= runtimeConfig.retareStabilityMaxGapMs &&
        receivedAtMs - mass.unloadAtMs <= DIRECT_STOP_CONFIRMATION_WINDOW_MS &&
        packetSequence != 0 && packetSequence ==
            (mass.unloadSequence == UINT32_MAX ? 1U : mass.unloadSequence + 1U);
    if (!consecutive) mass.unloadSamples = 0;
    if (mass.unloadSamples < DIRECT_STOP_CONFIRMATION_SAMPLES) ++mass.unloadSamples;
    mass.unloadAtMs = receivedAtMs;
    mass.unloadSequence = packetSequence;
    if (mass.unloadSamples >= DIRECT_STOP_CONFIRMATION_SAMPLES)
      mass.unloadQualified = true;
  } else {
    mass.unloadSamples = 0;
  }
  // A lighter put-back may remain below zero until tare. Its stationary
  // occupied plateau is not a second lift; preserve the additional drop.
  const float removalReferenceG = fminf(0.0f, cupPresence.occupiedReferenceG);
  const bool removalCandidate = !protectedPartialUnload && (
      nearEmpty || weight <= removalReferenceG + removedG ||
      (!cupPresence.taredWhilePresent && !cupPresence.referenceUncertain &&
       weight < minCupG));

  if (!removalCandidate) {
    cupPresence.removedConfirmations = 0;
    cupPresence.lastRemovedAtMs = 0;
    cupPresence.lastRemovedPacketSequence = 0;
  }
  if (cupPresence.state == CupPresenceState::PRESENT
          ? !removalCandidate && !protectedPartialUnload : weight > removedG)
    cupPresence.lastPresentWeightG = weight;

  if (cupPresence.holdTransitions) {
    return CupPresenceEvent::NONE;
  }

  if (cupPresence.state == CupPresenceState::PRESENT ||
      (removalWhileAbsent && weight <= removedG)) {
    if (!removalCandidate) {
      resetCupPlaceStabilityStreak();
      return CupPresenceEvent::NONE;
    }
    // A slow creep across the removed threshold is not a lift.
    if (cupPresence.lastPresentWeightG - weight < fminf(fabsf(removedG), minCupG / 2.0f)) {
      cupPresence.removedConfirmations = 0;
      return CupPresenceEvent::NONE;
    }
    const bool consecutive = cupPresence.removedConfirmations > 0 &&
        static_cast<int32_t>(receivedAtMs - cupPresence.lastRemovedAtMs) >= 0 &&
        static_cast<uint32_t>(receivedAtMs - cupPresence.lastRemovedAtMs) <=
            DIRECT_STOP_CONFIRMATION_WINDOW_MS &&
        (packetSequence == 0 || cupPresence.lastRemovedPacketSequence == 0 ||
         packetSequence == (cupPresence.lastRemovedPacketSequence == UINT32_MAX
                                ? 1U : cupPresence.lastRemovedPacketSequence + 1U));
    cupPresence.removedConfirmations = consecutive
        ? static_cast<uint8_t>(cupPresence.removedConfirmations + 1U)
        : 1U;
    cupPresence.lastRemovedAtMs = receivedAtMs;
    cupPresence.lastRemovedPacketSequence = packetSequence;
    if (cupPresence.removedConfirmations == 1) cupPresence.removedStartedAtMs = receivedAtMs;
    // In a shot, a dip shallower than half the known cup mass is a tilt until
    // it persists; a full lift still confirms on the usual two samples.
    const bool partial = !allowFastReplacement && mass.valid && mass.weightG >= minCupG &&
        weight > cupPresence.occupiedReferenceG - mass.weightG / 2.0f;
    if (cupPresence.removedConfirmations <
            (partial ? CUP_PARTIAL_REMOVAL_SAMPLES : DIRECT_STOP_CONFIRMATION_SAMPLES) ||
        (partial && receivedAtMs - cupPresence.removedStartedAtMs < CUP_PARTIAL_REMOVAL_MS)) {
      return CupPresenceEvent::NONE;
    }

    cupPresence.state = CupPresenceState::ABSENT;
    const uint8_t unloadSamples = mass.unloadSamples;
    invalidateCupWeight();
    mass.unloadSamples = unloadSamples;
    mass.unloadQualified = unloadSamples >= DIRECT_STOP_CONFIRMATION_SAMPLES;
    cupPresence.taredWhilePresent = false;
    cupPresence.referenceUncertain = false;
    cupPresence.occupiedReferenceG = 0.0f;
    cupPresence.inNegativeHole = true;
    cupPresence.holeWeightG = weight;
    cupPresence.removedConfirmations = 0;
    resetCupPlaceStabilityStreak();
    return CupPresenceEvent::REMOVED;
  }

  if (allowFastReplacement && initialPlacement &&
      !cupPresence.referenceUncertain && !cupPresence.holdTransitions &&
      std::isfinite(cupPresence.emptyAnchorG) &&
      fabsf(cupPresence.emptyAnchorG) <= zeroBandG &&
      (mass.emptyValid || cupPresence.inNegativeHole) &&
      weight < cupPresence.emptyAnchorG - FIRST_DROP_BASELINE_SETTLE_G) {
    cupPresence.inNegativeHole = true;
    if (weight <= cupPresence.emptyAnchorG - minCupG) {
      cupPresence.emptyAnchorG = NAN;
      mass.emptyValid = false;
    }
  }
  if (cupPresence.inNegativeHole && weight < cupPresence.holeWeightG) {
    cupPresence.holeWeightG = weight;
  }
  const bool knownZeroReturn = allowFastReplacement && mass.absent.valid &&
      fabsf(mass.absent.absoluteG) <= zeroBandG && weight > zeroBandG &&
      weight < mass.absent.absoluteG + minCupG;
  if (initialPlacement && cupPresence.inNegativeHole &&
      (fabsf(weight) <= zeroBandG || knownZeroReturn) &&
      ((std::isfinite(cupPresence.emptyAnchorG) &&
        (allowFastReplacement ? fabsf(cupPresence.emptyAnchorG) > zeroBandG
            : cupPresence.emptyAnchorG <= zeroBandG - minCupG)) ||
       (allowFastReplacement && mass.pendingId == 0 && !cupPresence.referenceUncertain &&
        !cupPresence.holdTransitions && !std::isfinite(cupPresence.emptyAnchorG)))) {
    // Returning to the original zero is indistinguishable from pan movement.
    // A positive sub-minimum return reuses known zero, never a new empty offset.
    cupPresence.emptyAnchorG = allowFastReplacement
        ? (knownZeroReturn ? mass.absent.absoluteG : weight) : 0.0f;
    mass.emptyValid = false;
    mass.emptySamples = 0;
    // Idle placement must wait for this zero to qualify again.
    cupPresence.inNegativeHole = allowFastReplacement;
    if (allowFastReplacement) cupPresence.holeWeightG = 0.0f;
  }
  // A continuous return near the initial zero can reuse its qualified empty
  // reference when a cup arrives before the empty pan settles again.
  if (allowFastReplacement && initialPlacement && mass.absent.valid &&
      fabsf(mass.absent.absoluteG) <= zeroBandG &&
      std::isfinite(cupPresence.emptyAnchorG) &&
      fabsf(cupPresence.emptyAnchorG) <= zeroBandG &&
      weight >= fmaxf(cupPresence.emptyAnchorG, mass.absent.absoluteG) + minCupG) {
    cupPresence.emptyAnchorG = mass.absent.absoluteG;
    mass.emptyValid = true;
  }
  // A brief confirmed unload can reuse the anchor; never use a lift minimum.
  const bool qualifiedReference = mass.emptyValid ||
      (allowFastReplacement && mass.unloadQualified && std::isfinite(cupPresence.emptyAnchorG));
  const bool awaitingAbsence = cupPresence.inNegativeHole &&
                               !qualifiedReference;
  const float placementThresholdG = qualifiedReference
      ? cupPresence.emptyAnchorG + minCupG : minCupG;
  const bool placeCandidate = !awaitingAbsence && weight >= placementThresholdG;
  if (!placeCandidate) {
    if (cupPresence.weight.sampleSequence == packetSequence &&
        cupPresence.weight.sampleAtMs == receivedAtMs && packetSequence != 0)
      observeEmptyCupWeight(weight, receivedAtMs, allowFastReplacement && initialPlacement,
                            allowFastReplacement);
    resetCupPlaceStabilityStreak();
    return CupPresenceEvent::NONE;
  }
  if (!allowPlacement) {
    resetCupPlaceStabilityStreak();
    return CupPresenceEvent::NONE;
  }

  if (cupPresence.placeStabilitySamples == 0) {
    cupPresence.weight.emptySamples = 0;
    cupPresence.placeCandidateWeightG = weight;
    cupPresence.placeMinimumG = weight;
    cupPresence.placeMaximumG = weight;
    cupPresence.placeStabilitySamples = 1;
    cupPresence.placeStabilityStartedAtMs = receivedAtMs;
    cupPresence.placeLastSampleAtMs = receivedAtMs;
    return CupPresenceEvent::NONE;
  }

  if (static_cast<uint32_t>(receivedAtMs - cupPresence.placeLastSampleAtMs) >
          runtimeConfig.retareStabilityMaxGapMs ||
      fmaxf(weight, cupPresence.placeMaximumG) -
              fminf(weight, cupPresence.placeMinimumG) >
          runtimeConfig.retareStabilityToleranceG) {
    scaleProfileNoteEvent(ScaleProfileEvent::CUP_QUALIFICATION_RESET, millis(),
                          mass.connectionGeneration, packetSequence, weight, 1,
                          static_cast<uint32_t>(receivedAtMs - cupPresence.placeLastSampleAtMs >
                              runtimeConfig.retareStabilityMaxGapMs
                              ? IdleTareReason::SAMPLE_GAP : IdleTareReason::UNSTABLE));
    cupPresence.placeCandidateWeightG = weight;
    cupPresence.placeMinimumG = weight;
    cupPresence.placeMaximumG = weight;
    cupPresence.placeStabilitySamples = 1;
    cupPresence.placeStabilityStartedAtMs = receivedAtMs;
    cupPresence.placeLastSampleAtMs = receivedAtMs;
    return CupPresenceEvent::NONE;
  }

  cupPresence.placeCandidateWeightG = weight;
  cupPresence.placeMinimumG = fminf(weight, cupPresence.placeMinimumG);
  cupPresence.placeMaximumG = fmaxf(weight, cupPresence.placeMaximumG);
  cupPresence.placeLastSampleAtMs = receivedAtMs;
  if (cupPresence.placeStabilitySamples < UINT8_MAX) {
    ++cupPresence.placeStabilitySamples;
  }
  const uint32_t stableDurationMs = static_cast<uint32_t>(
      receivedAtMs - cupPresence.placeStabilityStartedAtMs);
  const bool samplesMet = cupPresence.placeStabilitySamples >=
                          runtimeConfig.retareStabilitySamples;
  const bool durationMet =
      runtimeConfig.retareStabilityMinDurationMs == 0U ||
      stableDurationMs >= runtimeConfig.retareStabilityMinDurationMs;
  if (!samplesMet || !durationMet) {
    return CupPresenceEvent::NONE;
  }

  cupPresence.state = CupPresenceState::PRESENT;
  const float placementWeightG = cupPresence.placeCandidateWeightG -
                                cupPresence.emptyAnchorG;
  const bool placementSampleValid = cupPresence.weight.sampleSequence == packetSequence &&
      packetSequence != 0 && cupPresence.weight.sampleAtMs == receivedAtMs;
  const bool placementWeightValid = qualifiedReference &&
      placementSampleValid && std::isfinite(placementWeightG) && placementWeightG >= minCupG;
  // Record the new occupied reading without inventing a stable-empty sample.
  cupPresence.weight.present = CupStableWeight{
      cupPresence.placeCandidateWeightG, receivedAtMs, placementSampleValid};
  cupPresence.weight.emptyValid = false;
  cupPresence.weight.unloadQualified = false;
  cupPresence.weight.unloadSamples = 0;
  cupPresence.weight.pendingValid = false;
  cupPresence.weight.weightG = placementWeightValid ? placementWeightG : 0.0f;
  cupPresence.weight.valid = placementWeightValid;
  if (placementWeightValid) cupPresence.initialZeroRecoveryAllowed = false;
  ++cupPresence.placementId;
  if (cupPresence.placementId == 0) ++cupPresence.placementId;
  cupPresence.referenceUncertain = false;
  cupPresence.occupiedReferenceG = cupPresence.placeCandidateWeightG;
  cupPresence.occupiedMinimumG = cupPresence.placeMinimumG;
  cupPresence.occupiedMaximumG = cupPresence.placeMaximumG;
  cupPresence.occupiedPlacementThresholdG = placementThresholdG;
  // A relative placement may remain below the absolute minimum until tare.
  cupPresence.taredWhilePresent = cupPresence.placeCandidateWeightG < minCupG;
  cupPresence.inNegativeHole = false;
  cupPresence.holeWeightG = 0.0f;
  resetCupPlaceStabilityStreak();
  return CupPresenceEvent::PLACED;
}

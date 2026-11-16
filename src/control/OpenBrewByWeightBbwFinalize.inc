// Control-owned post-shot learning; no actuation or direct flash writes.
// Leaves in snapshot.dripLearnNote what the shot did to the drip (its record
// keeps it); every skip names its reason in the Web UI log and the shot trace.
bool learnPendingBbw(PendingShotFinalize &snapshot, float finalWeightG,
                     bool postDripWeightValid) {
  // r learns from weight cuts only (target, raised or lowered segment),
  // never from channeling, scale loss, the wall or a manual stop, nor from a
  // cut without a valid rate (no q x tau lead, so the error is not drip).
  // A rinse never reaches the shot log.
  snapshot.dripLearnNote = DripLearnNote::NONE;
  if (pendingFinalizeIsRinse(snapshot)) return false;
  const auto skip = [&snapshot](DripLearnNote note, int32_t neededCg = INT32_MIN) {
    snapshot.dripLearnNote = note;
    addDebugEvent(DebugCategory::CONFIG, DebugCode::DRIP_LEARN_SKIPPED,
                  static_cast<int32_t>(note), neededCg);
    return false;
  };
  if (!snapshot.offsetAnalysis)
    return skip(!snapshot.automaticBrew ? DripLearnNote::MANUAL_BREW
                : snapshot.timerOnly    ? DripLearnNote::TIMER_ONLY
                                        : DripLearnNote::WEIGHT_DISTURBED);
  if (snapshot.channeling) return skip(DripLearnNote::CHANNELING);
  if (snapshot.rateAtStopX100 == 0) return skip(DripLearnNote::NO_FLOW_RATE);
  if (!corridor::weightCutReason(static_cast<uint8_t>(snapshot.endReason)))
    return skip(DripLearnNote::NOT_WEIGHT_CUT);
  if (!scaleAvailable() || !std::isfinite(finalWeightG) ||
      !std::isfinite(snapshot.weightOffsetG) ||
      currentWeightSequence == snapshot.endedWeightSequence ||
      !tickAfter(currentWeightReceivedAtMs, snapshot.endedAtMs))
    return skip(DripLearnNote::NO_FINAL_WEIGHT);
  if (finalWeightG < 1.0f) return skip(DripLearnNote::FINAL_TOO_LOW);

  // Observation = offsetUsed + (final - f(t_cut)): the cut line at the stop.
  const float cutLineG = pendingCutLineDg(snapshot) / 10.0f;
  const float observation = snapshot.weightOffsetG + (finalWeightG - cutLineG);
  const int32_t neededCg = lroundf(observation * 100.0f);
  serialTracef(LogLevel::INFO,
               "Final weight: %.2fg; cut line: %.2fg; cycle offset: %.2fg",
               finalWeightG, cutLineG, snapshot.weightOffsetG);

  // A tail that settled is the yield even when the cup left before the hold
  // ended: the live reading may be refused or stale by then.
  float updatedOffset = 0.0f;
  const bool acceptedFresh = postDripWeightValid && snapshot.scaleBaselineReady &&
      shotLogBbwEligible(snapshot.startedWithScale, snapshot.timerOnly,
                         snapshot.automaticBrew) &&
      (std::isfinite(snapshot.tailSettledG) ||
       (currentWeightIsFresh(millis()) &&
        currentWeightConnectionGeneration == snapshot.scaleConnectionGeneration)) &&
      snapshot.scaleConnectionGeneration == scaleConnectionGenerationNow();
  if (!bbwEwma::learn(snapshot.weightOffsetG, observation, snapshot.bbwAlpha,
                      acceptedFresh, updatedOffset))
    return skip(acceptedFresh ? DripLearnNote::OUT_OF_RANGE : DripLearnNote::NOT_FRESH,
                neededCg);

  ShotPreset *preset = mutableShotPreset(
      presetBank, snapshot.activePresetId != 0 ? snapshot.activePresetId
                                               : presetBank.activeId);
  if (preset == nullptr) return skip(DripLearnNote::PRESET_GONE, neededCg);
  if (snapshot.bbwAlgorithm !=
          static_cast<uint8_t>(BbwAlgorithm::LINEAR_EWMA) ||
      snapshot.bbwProfileVersion != bbwAlgorithmVersion(snapshot.bbwAlgorithm))
    return skip(DripLearnNote::OTHER_ALGORITHM, neededCg);
  auto &learning = bbwLearningBank.forPreset(preset->id, presetBank);
  if (snapshot.bbwLearningGeneration != learning.generation)
    return skip(DripLearnNote::RESTARTED, neededCg);
  // Physics gate (design review 2026-10-29): flow that kept running or a
  // scale event is never drip. It also clears a pending outlier: a step is
  // confirmed by statistics on physically possible observations only.
  if (!bbwEwma::physicsPlausibleDrip(observation, snapshot.rateAtStopX100)) {
    if (std::exchange(learning.pendingOutlier, false)) stageBbwLearning();
    return skip(DripLearnNote::IMPOSSIBLE, neededCg);
  }
  bool stepConfirmed = false;
  if (bbwEwma::hampelOutlier(learning.evidence, observation)) {
    if (learning.pendingOutlier &&
        fabsf(observation - learning.pendingCg / 100.0f) <=
            bbwEwma::DRIP_STEP_AGREE_G) {
      // Two shots agree far from the history: a real change, not noise. Jump
      // to their mean and restart the evidence for the new regime.
      updatedOffset = bbwEwma::clampOffset(
          (observation + learning.pendingCg / 100.0f) * 0.5f);
      bbwLearningBank.invalidate(preset->id, presetBank);
      stepConfirmed = true;
      serialTracef(LogLevel::INFO,
                   "Drip step: two shots agree on %.2fg; evidence restarted",
                   updatedOffset);
    } else {
      learning.pendingOutlier = true;
      learning.pendingCg = static_cast<int16_t>(neededCg);
      stageBbwLearning();
      return skip(DripLearnNote::OUTLIER, neededCg);
    }
  } else {
    learning.pendingOutlier = false;
  }
  // Quick start: the third shot stopped at the target that agrees sets the
  // drip to the median. It has no outlier gate yet, so a raised or lowered
  // cut trains the correction but never counts there; only good shots reach
  // here, so the chart markers line up.
  int16_t jumpCg = -1;
  const QuickStartEntry *quick = quickStartEntry(bbwQuickStart, presetBank, preset->id, false);
  if (snapshot.endReason != EndReason::SCALE_THRESHOLD && quick != nullptr &&
      quick->state == static_cast<uint8_t>(QuickStartState::ARMED)) {
    snapshot.dripLearnNote = DripLearnNote::TARGET_ONLY;
  } else if (!stepConfirmed &&
             observeQuickStart(bbwQuickStart, presetBank, preset->id,
                               {static_cast<int16_t>(neededCg), shotStatisticsEpochNow()},
                               jumpCg)) {
    if (jumpCg >= 0) updatedOffset = jumpCg / 100.0f;
    if (quick->count == QUICK_START_SHOTS)
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::CONFIG,
                           jumpCg >= 0 ? "Quick start: three shots agree; drip correction %.2f g"
                                       : "Quick start: shots disagree; learning goes on from %.2f g",
                           updatedOffset);
    stageBbwQuickStart();
  }
  preset->bbwEwmaOffsetG = updatedOffset;
  const uint8_t nextAlpha =
      stepConfirmed ? preset->bbwEwmaAlpha
                    : learning.evidence.observe(observation,
                                                snapshot.weightOffsetG,
                                                snapshot.bbwAlpha);
  if (nextAlpha != preset->bbwEwmaAlpha) preset->bbwAlphaLearned = 1;
  preset->bbwEwmaAlpha = nextAlpha;
  learning.generation = bbwLearningBank.newGeneration();
  stageBbwLearning();
  RuntimeConfig candidate = runtimeConfig;
  applyShotPresetToConfig(activeShotPreset(presetBank), candidate);
  ++candidate.revision;
  if (candidate.revision == 0) {
    candidate.revision = 1;
  }
  commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_OFFSET);
  serialTracef(LogLevel::INFO, "New offset pending durable commit: %.2f",
               updatedOffset);
  return true;
}

// Control side: hand the quick start record to the settings_persist worker.
void stageBbwQuickStart() {
  bbwQuickStartMux.lock();
  bbwQuickStartStaged = bbwQuickStart;
  bbwQuickStartPersistPending = true;
  bbwQuickStartMux.unlock();
}

// Control side: hand a copy of the learning memory to the settings_persist worker.
void stageBbwLearning() {
  bbwLearningMux.lock();
  snapshotBbwLearning(bbwLearningStaged, bbwLearningBank, presetBank);
  bbwLearningPersistPending = true;
  bbwLearningMux.unlock();
}

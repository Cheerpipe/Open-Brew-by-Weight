// Control-owned post-shot learning; no actuation or direct flash writes.
bool learnPendingBbw(const PendingShotFinalize &snapshot, float finalWeightG,
                     bool postDripWeightValid) {
  // r learns from weight cuts only (target, raised or lowered segment),
  // never from channeling, scale loss, the wall or a manual stop, nor from a
  // cut without a valid rate (no q x tau lead, so the error is not drip).
  if (pendingFinalizeIsRinse(snapshot) || !snapshot.offsetAnalysis ||
      snapshot.channeling || snapshot.rateAtStopX100 == 0 ||
      !corridor::weightCutReason(static_cast<uint8_t>(snapshot.endReason))) {
    return false;
  }

  if (!scaleAvailable() || !std::isfinite(finalWeightG) ||
      !std::isfinite(snapshot.weightOffsetG) ||
      currentWeightSequence == snapshot.endedWeightSequence ||
      static_cast<int32_t>(currentWeightReceivedAtMs - snapshot.endedAtMs) <=
          0) {
    serialTrace(LogLevel::INFO,
                "Final weight unavailable; offset unchanged");
    return false;
  }

  if (finalWeightG < 1.0f) {
    serialTrace(LogLevel::INFO,
                "Final weight below history threshold; offset unchanged");
    return false;
  }

  // Observation = offsetUsed + (final - f(t_cut)): the cut line at the stop.
  const float cutLineG = pendingCutLineDg(snapshot) / 10.0f;
  serialTracef(LogLevel::INFO,
               "Final weight: %.2fg; cut line: %.2fg; cycle offset: %.2fg",
               finalWeightG, cutLineG, snapshot.weightOffsetG);

  float updatedOffset = 0.0f;
  const bool acceptedFresh = postDripWeightValid && snapshot.scaleBaselineReady &&
      shotLogBbwEligible(snapshot.startedWithScale, snapshot.timerOnly,
                         snapshot.automaticBrew) &&
      currentWeightIsFresh(millis()) &&
      currentWeightConnectionGeneration == snapshot.scaleConnectionGeneration &&
      snapshot.scaleConnectionGeneration == scaleConnectionGenerationNow();
  if (!bbwEwma::learn(snapshot.weightOffsetG, finalWeightG, cutLineG,
                      snapshot.bbwAlpha, acceptedFresh, updatedOffset)) {
    serialTrace(acceptedFresh ? LogLevel::WARNING : LogLevel::INFO,
                acceptedFresh
                    ? "Calculated offset outside safe range; offset unchanged"
                    : "Final weight not a fresh post-drip reading; offset unchanged");
    return false;
  }

  ShotPreset *preset = mutableShotPreset(
      presetBank, snapshot.activePresetId != 0 ? snapshot.activePresetId
                                               : presetBank.activeId);
  if (preset == nullptr) {
    serialTrace(LogLevel::WARNING,
                "Offset learn skipped; shot preset no longer exists");
    return false;
  }
  if (snapshot.bbwAlgorithm !=
          static_cast<uint8_t>(BbwAlgorithm::LINEAR_EWMA) ||
      snapshot.bbwProfileVersion != bbwAlgorithmVersion(snapshot.bbwAlgorithm)) return false;
  auto &learning = bbwLearningBank.forPreset(preset->id, presetBank);
  if (snapshot.bbwLearningGeneration != learning.generation)
    return false;
  const float observation = snapshot.weightOffsetG + (finalWeightG - cutLineG);
  // Quick start: the third shot that agrees sets the drip to the median;
  // only good shots reach here (pendingShotFinalizeTask), so the chart
  // markers line up.
  int16_t jumpCg = -1;
  if (observeQuickStart(bbwQuickStart, presetBank, preset->id,
                        static_cast<int16_t>(lroundf(observation * 100.0f)),
                        shotStatisticsEpochNow(), jumpCg)) {
    if (jumpCg >= 0) updatedOffset = jumpCg / 100.0f;
    stageBbwQuickStart();
  }
  preset->bbwEwmaOffsetG = updatedOffset;
  const uint8_t nextAlpha = learning.evidence.observe(
      observation, snapshot.weightOffsetG, snapshot.bbwAlpha);
  if (nextAlpha != preset->bbwEwmaAlpha) preset->bbwAlphaLearned = 1;
  preset->bbwEwmaAlpha = nextAlpha;
  learning.generation = bbwLearningBank.newGeneration();
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

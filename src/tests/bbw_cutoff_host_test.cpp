#define OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST
#include "../OpenBrewByWeightBbwLearning.h"
#include "../OpenBrewByWeightBbwQuickStart.h"
#include "../OpenBrewByWeightPersistence.h"
#include "../OpenBrewByWeightShotLog.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace openbrewbyweight;

int main() {
  uint8_t parsedAlgorithm = 2;
  assert(!parseBbwAlgorithm("legacy", parsedAlgorithm));
  assert(!parseBbwAlgorithm("linear_ewma", parsedAlgorithm));
  assert(!parseBbwAlgorithm("regression", parsedAlgorithm));
  assert(!parseBbwAlgorithm(nullptr, parsedAlgorithm));
  assert(parseBbwAlgorithm("corridor", parsedAlgorithm) && parsedAlgorithm == 1);
  assert(bbwAlgorithmVersion(0) == BBW_PROFILE_VERSION &&
         bbwAlgorithmVersion(1) == BBW_PROFILE_VERSION);
  float next = 0;
  assert(bbwEwma::learn(1.5f, 36.2f, 36, 30, true, next));
  assert(std::fabs(next - 1.56f) < 1e-5f);
  assert(!bbwEwma::learn(1.5f, 36.2f, 36, 30, false, next));
  assert(!bbwEwma::learn(1.5f, 42, 36, 10, true, next));
  assert(!bbwEwma::learn(NAN, 36, 36, 30, true, next));
  assert(!bbwEwma::learn(1.5f, 36, 36, 0, true, next));
  for (uint8_t gain : {1, 30, 37, 100}) {
    assert(bbwEwma::learn(1.5f, 36.2f, 36, gain, true, next));
    assert(fabsf(next - (1.5f + gain * .002f)) < 1e-5f);
    uint8_t parsed = 0;
    assert(parseBbwAlphaBaseline(gain / 100.0, parsed) && parsed == gain);
    bbwEwma::Evidence custom;
    uint8_t active = gain;
    for (int i = 0; i < 100; ++i) active = custom.observe(1.5f, 1.5f, active);
    assert(active == gain);  // Equal evidence never replaces a custom incumbent.
    custom = {};
    for (int i = 0; i < 25; ++i) {
      active = custom.observe(3.0f, 1.5f, active);
      if (i < 24) assert(active == gain);
    }
    assert(active == 100);
  }
  for (double invalid : {-1.0, 0.0, 0.001, 0.375, 1.01, double(INFINITY), double(NAN)}) {
    uint8_t parsed = 50;
    assert(!parseBbwAlphaBaseline(invalid, parsed) && parsed == 50);
  }
  assert(bbwEwma::learn(0, 35, 36, 30, true, next) && next == 0);
  assert(bbwEwma::learn(5, 36, 36, 30, true, next) && next == 5);
  bbwEwma::Evidence constant, noisy, changed;
  uint8_t alpha = 30;
  for (int i = 0; i < 100; ++i) assert(constant.observe(1.5f, 1.5f, 30) == 30);
  for (int i = 0; i < 100; ++i) {
    const uint8_t selected = noisy.observe(i % 2 ? 2 : 1, 1.5f, alpha);
    if (i < 24) assert(selected == 30);
    alpha = selected;
  }
  assert(alpha == 10);
  const auto beforeInvalid = noisy;
  assert(noisy.observe(NAN, 1.5f, alpha) == alpha);
  assert(memcmp(&beforeInvalid, &noisy, sizeof(noisy)) == 0);
  alpha = 30;
  for (int i = 0; i < 25; ++i) {
    alpha = changed.observe(3, 1.5f, alpha);
    if (i < 24) assert(alpha == 30);
  }
  assert(alpha == 100);
  assert(changed.anchors[0] < changed.anchors[3]);
  bbwEwma::Evidence noLookAhead;
  assert(noLookAhead.observe(2.5f, 1.5f, 30) == 30);
  for (float anchor : noLookAhead.anchors) assert(anchor == 1.5f);
  // Anchors represent the complete discarded prefix, never a reseeded window.
  bbwEwma::Evidence replay;
  std::vector<float> observations;
  uint8_t replayAlpha = 37;
  for (int n = 0; n < 80; ++n) {
    observations.push_back(1.5f + 0.7f * std::sin(n * .23f));
    replayAlpha = replay.observe(observations.back(), 1.5f, replayAlpha);
    for (int i = 0; i < 5; ++i) {
      const float gain = (i < 4 ? BBW_ALPHA_CANDIDATES[i] : 37) / 100.0f;
      float referenceAnchor = 1.5f;
      for (int j = 0; j <= n - 20; ++j)
        referenceAnchor = bbwEwma::clampOffset(referenceAnchor + gain * (observations[j] - referenceAnchor));
      assert(fabsf(replay.anchors[i] - referenceAnchor) < 1e-6f);
    }
  }

  PersistedSettings settings;
  assert(initializeDefaultSettings(settings));
  for (const auto &preset : settings.presets.presets) {
    assert(preset.bbwEwmaAlpha == 30);
  }
  assert(validPersistedSettings(settings));

  ShotPresetBank bank = settings.presets;
  BbwLearningBank learning;
  auto &state = learning.forPreset(bank.presets[0].id, bank);
  const uint32_t generation = state.generation;
  state.evidence = noisy;
  learning.invalidate(bank.presets[0].id, bank);
  assert(state.generation != generation && state.evidence.count == 0);
  uint8_t duplicate = 0;
  bank.presets[0].bbwAlphaBaseline = 37;
  assert(duplicateShotPreset(bank, bank.presets[0].id, duplicate));
  assert(mutableShotPreset(bank, duplicate)->bbwEwmaAlpha == 30);
  assert(mutableShotPreset(bank, duplicate)->bbwAlphaBaseline == 37);
  uint8_t created = 0;
  assert(createUntitledShotPreset(bank, created));
  assert(findShotPreset(bank, created)->bbwAlphaBaseline == 30);
  assert(restoreFactoryShotPresetValues(bank, FACTORY_PRESET_ID_DOUBLE_ADAPTIVE));
  assert(findShotPreset(bank, FACTORY_PRESET_ID_DOUBLE_ADAPTIVE)->bbwAlphaBaseline == 30);
  assert(learning.forPreset(duplicate, bank).evidence.count == 0);
  state.evidence = noisy;
  unsigned char beforeOther[sizeof(state.evidence)];
  memcpy(beforeOther, &state.evidence, sizeof(beforeOther));
  auto &otherState = learning.forPreset(bank.presets[1].id, bank);
  otherState.evidence.observe(3.0f, 3.10f, 10);
  assert(memcmp(&state.evidence, beforeOther, sizeof(beforeOther)) == 0);
  assert(otherState.evidence.count == 1);

  ShotLogStore store;
  resetShotLogStore(store);
  store.header.count = 1;
  store.header.writeIndex = 1;
  auto &record = store.records[0];
  record.offsetUsedCg = 0;
  record.sideFlags = shotLogPackRating(3, 5);
  record.extractionExtended = 3;
  record.shotType = static_cast<uint8_t>(ShotLogType::AUTO);
  record.cutType = static_cast<uint8_t>(ShotLogCut::LIMIT);
  for (unsigned id = 0; id <= 255; ++id) {
    shotLogSetPresetId(record, static_cast<uint8_t>(id));
    assert(shotLogPresetId(record) == id);
    assert(shotLogType(record) == ShotLogType::AUTO);
    assert(shotLogCut(record) == ShotLogCut::LIMIT);
  }
  for (uint8_t gain = 1; gain <= 100; ++gain) {
    shotLogSetBbw(record, {.algorithm = 1, .version = BBW_PROFILE_VERSION, .alpha = gain,
                           .learningApplied = true});
    for (unsigned id = 0; id <= 255; ++id) {
      shotLogSetPresetId(record, static_cast<uint8_t>(id));
      assert(shotLogBbwAlpha(record) == gain && shotLogPresetId(record) == id);
      assert(shotLogCut(record) == ShotLogCut::LIMIT);
    }
    assert(strcmp(shotLogBbwVersion(record), "1") == 0);
    record.sideFlags = shotLogPackRating(record.sideFlags, 4);
    assert(shotLogRating(record.sideFlags) == 4);
    assert(shotLogFastGuardEnabled(record.sideFlags));
    assert(shotLogSlowExtended(record.extractionExtended));
    assert(shotLogBbwAlpha(record) == gain);
    assert(strcmp(shotLogBbwLearningApplied(record), "true") == 0);
    finalizeShotLogStore(store);
    ShotLogStore decoded = store;
    assert(validShotLogStore(decoded));
    assert(decoded.records[0].offsetUsedCg == 0);
    assert(shotLogPresetId(decoded.records[0]) == 255);
    assert(decoded.records[0].extractionExtended == record.extractionExtended);
    decoded.header.checksum ^= 1;
    assert(!validShotLogStore(decoded));
    decoded.header.checksum ^= 1;
    decoded.header.schemaVersion = 5;
    assert(!validShotLogStore(decoded));
  }
  // Per-protocol sensor-lag priors feed the cut's in-flight lead.
  {
    assert(kScaleProtocolAcaiaLegacy.features.sensorLagMs ==
           kScaleProtocolAcaia.features.sensorLagMs);
    // Every shipping protocol carries a prior: a zero would drop the
    // in-flight lead from that scale's cut.
    for (const ScaleProtocol *protocol : {&kScaleProtocolAcaiaLegacy,
                                          &kScaleProtocolAcaia,
                                          &kScaleProtocolGenericFf11,
                                          &kScaleProtocolFelicita,
                                          &kScaleProtocolEclair,
                                          &kScaleProtocolDecent,
                                          &kScaleProtocolDifluid,
                                          &kScaleProtocolMyscale,
                                          &kScaleProtocolWeighMyBru,
                                          &kScaleProtocolVaria,
                                          &kScaleProtocolEureka}) {
      assert(protocol->features.sensorLagMs >= 200 &&
             protocol->features.sensorLagMs <= 1000);
    }
  }

  // Drip quick start: three shots within 0.40 g jump the drip to their median.
  {
    ShotPresetBank bank;
    seedDefaultShotPresetBank(bank);
    const uint8_t id = bank.presets[0].id;
    QuickStartRecord record;
    int16_t jump = -1;
    assert(!observeQuickStart(record, bank, id, {50, 1}, jump));  // never armed
    armQuickStart(record, bank, id, {1.2f, 40});
    assert(observeQuickStart(record, bank, id, {48, 40}, jump) && jump == -1);
    assert(observeQuickStart(record, bank, id, {52, 41}, jump) && jump == -1);
    assert(observeQuickStart(record, bank, id, {50, 42}, jump) && jump == 50);
    const QuickStartEntry *entry = quickStartEntry(record, bank, id, false);
    assert(entry != nullptr && entry->state == static_cast<uint8_t>(QuickStartState::APPLIED) &&
           entry->setCg == 120 && entry->setAtId == 40 && entry->quickCg == 50 &&
           entry->quickAtId == 42);
    assert(!observeQuickStart(record, bank, id, {50, 43}, jump));  // decided once
    // Shots that disagree leave normal learning in charge.
    armQuickStart(record, bank, id, {1.2f, 50});
    jump = -1;
    for (const int16_t cg : {63, -5, 10}) assert(observeQuickStart(record, bank, id, {cg, 50}, jump));
    assert(jump == -1);
    assert(quickStartEntry(record, bank, id, false)->state ==
           static_cast<uint8_t>(QuickStartState::DISAGREED));
    // Raw observations decide; the drip itself never goes below zero.
    armQuickStart(record, bank, id, {0.5f, 60});
    for (const int16_t cg : {-10, -20}) assert(observeQuickStart(record, bank, id, {cg, 60}, jump));
    assert(observeQuickStart(record, bank, id, {-15, 62}, jump) && jump == 0);
    // A damaged count can never index past the three observations.
    quickStartEntry(record, bank, id, false)->state = static_cast<uint8_t>(QuickStartState::ARMED);
    assert(!observeQuickStart(record, bank, id, {50, 63}, jump));
    // A fresh install quick starts every brew-by-weight preset, not Manual.
    const QuickStartRecord fresh = armedQuickStart(bank);
    for (uint8_t i = 0; i < bank.count; ++i) {
      QuickStartRecord copy = fresh;
      const QuickStartEntry *e = quickStartEntry(copy, bank, bank.presets[i].id, false);
      assert((e != nullptr && e->state == static_cast<uint8_t>(QuickStartState::ARMED) &&
              e->setAtId == 0) == bank.presets[i].brewByWeight);
    }
    // The record round-trips through NVS; a damaged one is rejected.
    QuickStartRecord loaded;
    assert(saveQuickStart(record) && loadQuickStart(loaded));
    assert(memcmp(loaded.entries, record.entries, sizeof(record.entries)) == 0);
    loaded.checksum ^= 1;
    assert(!validQuickStart(loaded));
    // Boot: fresh settings or no record (fresh install, factory reset) arm;
    // a normal boot keeps the stored record.
    QuickStartRecord booted;
    assert(bootQuickStart(booted, true, bank, &record));
    assert(memcmp(&booted, &fresh, sizeof(booted)) == 0);
    assert(bootQuickStart(booted, false, bank, nullptr));
    assert(memcmp(&booted, &fresh, sizeof(booted)) == 0);
    assert(!bootQuickStart(booted, false, bank, &record));
    assert(memcmp(&booted, &record, sizeof(booted)) == 0);
    // Status JSON: no markers, an empty history and a buffer too small.
    ControlStatusFields fields;
    char json[256];
    assert(formatDripChartJson(json, sizeof(json), fields) > 0);
    assert(strstr(json, "\"bbwRecentAfterSet\":-1,\"bbwRecentAfterQuick\":-1,\"bbwRecentG\":[]") != nullptr);
    assert(formatDripChartJson(json, 40, fields) == 0);
  }

  std::cout << "BBW numeric, adaptation, state, and history checks passed\n";
}

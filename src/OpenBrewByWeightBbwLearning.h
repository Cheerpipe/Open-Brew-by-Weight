#pragma once

#include <stddef.h>

#include <algorithm>
#include <utility>

#include "OpenBrewByWeightBbwEwma.h"
#include "OpenBrewByWeightPresets.h"
#include "OpenBrewByWeightWebAccess.h"

namespace openbrewbyweight {

// Control-owned RAM, keyed by preset identity (not its movable array index).
// Everything but the session-only generation survives a restart through
// BbwLearningRecord.
struct BbwLearningBank {
  struct State {
    uint8_t id = 0;
    uint32_t generation = 0;
    // Last gate-rejected observation: two rejects that agree adopt a step.
    bool pendingOutlier = false;
    int16_t pendingCg = 0;
    bbwEwma::Evidence evidence;
  } states[MAX_SHOT_PRESETS];
  uint32_t nextGeneration = 0;

  // Zero is never a generation, so the counter skips it on wrap.
  uint32_t newGeneration() {
    nextGeneration = nextGeneration == UINT32_MAX ? 1 : nextGeneration + 1;
    return nextGeneration;
  }

  State &forPreset(uint8_t id, const ShotPresetBank &bank) {
    State *const end = std::end(states);
    State *found = std::find_if(std::begin(states), end,
                                [id](const State &state) { return state.id == id; });
    if (found != end) return *found;
    found = std::find_if(std::begin(states), end, [&bank](const State &state) {
      return state.id == 0 || findShotPresetIndex(bank, state.id) < 0;
    });
    // All eight extant presets are already present, so valid IDs never miss.
    if (found == end) return states[0];
    *found = State{};
    found->id = id;
    found->generation = newGeneration();
    return *found;
  }

  // Stale pending shots stop learning and the candidate evidence restarts.
  void invalidate(uint8_t id, const ShotPresetBank &bank) {
    State &state = forPreset(id, bank);
    state.generation = newGeneration();
    state.pendingOutlier = false;
    state.evidence = bbwEwma::Evidence{};
  }
};
static_assert(sizeof(BbwLearningBank) <= 3000, "BBW per-preset RAM budget");

// The learning memory in its own NVS key, outside the settings blob. Floats
// are stored as they are, so a restart restores the exact state. An entry
// counts only while its preset exists with the drip it had when saved: a
// drip that moved without this record (an older image, a power cut between
// the two writes) makes it stale.
constexpr uint32_t BBW_LEARNING_MAGIC = 0x4C574242U;  // "BBWL"
constexpr uint16_t BBW_LEARNING_VERSION = 1;
constexpr const char *BBW_LEARNING_KEY = "bbwLearn";

struct BbwLearningEntry {
  uint8_t presetId = 0;
  uint8_t pendingOutlier = 0;
  int16_t pendingCg = 0;
  float dripG = 0.0f;
  bbwEwma::Evidence evidence;
};

struct BbwLearningRecord {
  uint32_t magic = BBW_LEARNING_MAGIC;
  uint16_t version = BBW_LEARNING_VERSION;
  uint16_t size = 0;
  BbwLearningEntry entries[MAX_SHOT_PRESETS];
  uint32_t checksum = 0;
};
// Any layout change, Evidence included, needs a new version. The checksum
// covers the bytes as written, Evidence's tail padding included.
static_assert(sizeof(BbwLearningRecord) == 940, "drip learning record layout changed");

inline uint32_t bbwLearningChecksum(const BbwLearningRecord &record) {
  return webAccessFnv(&record, offsetof(BbwLearningRecord, checksum));
}

inline bool validBbwLearning(const BbwLearningRecord &record) {
  return record.magic == BBW_LEARNING_MAGIC && record.version == BBW_LEARNING_VERSION &&
         record.size == sizeof(BbwLearningRecord) &&
         record.checksum == bbwLearningChecksum(record) &&
         std::all_of(std::begin(record.entries), std::end(record.entries),
                     [](const BbwLearningEntry &entry) {
                       return entry.evidence.count <= bbwEwma::Evidence::WINDOW &&
                              entry.evidence.position < bbwEwma::Evidence::WINDOW;
                     });
}

// Copies the states of existing presets, each with its preset's drip.
inline void snapshotBbwLearning(BbwLearningRecord &out, const BbwLearningBank &bank,
                                const ShotPresetBank &presets) {
  for (uint8_t i = 0; i < MAX_SHOT_PRESETS; ++i) {
    const BbwLearningBank::State &state = bank.states[i];
    const int index = state.id == 0 ? -1 : findShotPresetIndex(presets, state.id);
    out.entries[i] = index < 0 ? BbwLearningEntry{}
                               : BbwLearningEntry{state.id, state.pendingOutlier,
                                                  state.pendingCg,
                                                  presets.presets[index].bbwEwmaOffsetG,
                                                  state.evidence};
  }
}

// Boot: restores the entries that still count (see BbwLearningRecord) under
// fresh generations; returns how many.
inline uint8_t restoreBbwLearning(BbwLearningBank &bank, const BbwLearningRecord &record,
                                  const ShotPresetBank &presets) {
  uint8_t restored = 0;
  for (const BbwLearningEntry &entry : record.entries) {
    const int index = entry.presetId == 0 ? -1 : findShotPresetIndex(presets, entry.presetId);
    if (index < 0 || presets.presets[index].bbwEwmaOffsetG != entry.dripG) continue;
    BbwLearningBank::State &state = bank.forPreset(entry.presetId, presets);
    state.pendingOutlier = entry.pendingOutlier != 0;
    state.pendingCg = entry.pendingCg;
    state.evidence = entry.evidence;
    ++restored;
  }
  return restored;
}

}  // namespace openbrewbyweight

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include "OpenBrewByWeightPersistence.h"

namespace openbrewbyweight {

// Callers pass stack copies: flash I/O must not read or write PSRAM.
inline SettingsBlobLoad loadBbwLearning(BbwLearningRecord &record) {
  return loadSettingsBlob(NvsSubsystem::SETTINGS, BBW_LEARNING_KEY, record, validBbwLearning);
}

inline bool saveBbwLearning(BbwLearningRecord &record) {
  record.magic = BBW_LEARNING_MAGIC;
  record.version = BBW_LEARNING_VERSION;
  record.size = sizeof(BbwLearningRecord);
  record.checksum = bbwLearningChecksum(record);
  return saveSettingsBlob(NvsSubsystem::SETTINGS, BBW_LEARNING_KEY, record);
}

}  // namespace openbrewbyweight
#endif

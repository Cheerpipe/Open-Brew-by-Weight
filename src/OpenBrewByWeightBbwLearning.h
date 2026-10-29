#pragma once

#include "OpenBrewByWeightBbwEwma.h"
#include "OpenBrewByWeightPresets.h"

namespace openbrewbyweight {

// Control-owned RAM, keyed by preset identity (not its movable array index).
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

  uint32_t newGeneration() {
    if (++nextGeneration == 0) ++nextGeneration;
    return nextGeneration;
  }

  State &forPreset(uint8_t id, const ShotPresetBank &bank) {
    for (State &state : states) if (state.id == id) return state;
    for (State &state : states) {
      if (state.id == 0 || findShotPresetIndex(bank, state.id) < 0) {
        state = State{};
        state.id = id;
        state.generation = newGeneration();
        return state;
      }
    }
    // All eight extant presets are already present, so valid IDs return above.
    return states[0];
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

}  // namespace openbrewbyweight

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

}  // namespace openbrewbyweight

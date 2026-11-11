#pragma once

// Power test helper: firmware never asks whether the tracker holds an
// assumption, so tests read the hold the way the effective status shows it.

#include "../machine/OpenBrewByWeightLaMarzoccoPowerState.h"

namespace openbrewbyweight {

// A hold reads as an assumption even over a fresh reading; nothing else does.
inline bool retained(const LaMarzoccoPowerStateTracker &power) {
  LaMarzoccoStatus fresh;
  fresh.sampleAtMs = 1;
  fresh.connectionFreshness = true;
  fresh.quality = LaMarzoccoObservationQuality::CURRENT;
  return power.effectiveStatus(fresh, true, 1).quality == LaMarzoccoObservationQuality::STALE;
}

}  // namespace openbrewbyweight

#pragma once

// Power test helper: firmware never asks whether the tracker holds an
// assumption, so tests read the hold the way the effective status shows it.

#include "../machine/OpenBrewByWeightMicraPowerState.h"

namespace openbrewbyweight {

// A hold reads as an assumption even over a fresh reading; nothing else does.
inline bool retained(const LineaMicraPowerStateTracker &power) {
  LineaMicraStatus fresh;
  fresh.sampleAtMs = 1;
  fresh.connectionFreshness = true;
  fresh.quality = LineaMicraObservationQuality::CURRENT;
  return power.effectiveStatus(fresh, true, 1).quality == LineaMicraObservationQuality::STALE;
}

}  // namespace openbrewbyweight

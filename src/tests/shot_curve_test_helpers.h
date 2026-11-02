#pragma once

// Curve test helper: firmware only walks the delta stream forward, so tests
// read points back the same way (and build exact curves with
// appendShotCurvePoint).

#include "../OpenBrewByWeightShotCurveTypes.h"

namespace openbrewbyweight {

struct CurvePoint {
  uint16_t atMs = 0;
  int16_t cg = SHOT_LOG_WEIGHT_MISSING;
};

inline ShotCurveRecord emptyShotCurveRecord() {
  ShotCurveRecord curve{};
  resetShotCurveRecord(curve);
  return curve;
}

// Point `index`, or a missing-weight point past the curve.
inline CurvePoint curvePoint(const ShotCurveRecord &curve, size_t index) {
  ShotCurveReader reader;
  if (index >= curve.count ||
      !shotCurveSeek(curve, static_cast<uint16_t>(index + 1U), reader))
    return {};
  return {reader.atMs, reader.weightCg};
}

}  // namespace openbrewbyweight

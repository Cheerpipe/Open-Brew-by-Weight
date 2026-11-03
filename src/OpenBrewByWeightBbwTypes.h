#pragma once

#include <stdint.h>
#include <string.h>
#include <cmath>

namespace openbrewbyweight {

// The single cutoff algorithm id (1). Its API name is "corridor" (the smooth
// cut with drip-offset learning).
enum class BbwAlgorithm : uint8_t { LINEAR_EWMA = 1 };
constexpr uint8_t DEFAULT_BBW_EWMA_ALPHA = 30;  // Hundredths, dimensionless.
// Profile 3: the drip-offset EWMA learning against the smooth-cut line.
constexpr uint8_t BBW_PROFILE_VERSION = 3;
constexpr uint8_t BBW_ALPHA_CANDIDATES[] = {10, 30, 50, 100};

inline bool validBbwAlpha(uint8_t alpha) {
  return alpha >= 1 && alpha <= 100;
}

inline uint8_t bbwAlgorithmVersion(uint8_t algorithm) {
  (void)algorithm;
  return BBW_PROFILE_VERSION;
}

inline bool parseBbwAlphaBaseline(double value, uint8_t &out) {
  if (!(value >= 0.01 && value <= 1.0)) return false;
  const unsigned hundredths = static_cast<unsigned>(std::lround(value * 100.0));
  if (fabs(value * 100.0 - hundredths) > 1e-9) return false;
  out = static_cast<uint8_t>(hundredths);
  return true;
}

inline bool parseBbwAlgorithm(const char *name, uint8_t &algorithm) {
  if (name == nullptr) return false;
  if (strcmp(name, "corridor") == 0) {
    algorithm = static_cast<uint8_t>(BbwAlgorithm::LINEAR_EWMA);
    return true;
  }
  return false;
}

}  // namespace openbrewbyweight

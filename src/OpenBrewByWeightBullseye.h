#pragma once

#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OpenBrewByWeightRtttl.h"
#include "OpenBrewByWeightScaleTypes.h"

namespace openbrewbyweight {

constexpr size_t BULLSEYE_RTTTL_MAX_CHARS = 500;
constexpr size_t BULLSEYE_RTTTL_CAPACITY = BULLSEYE_RTTTL_MAX_CHARS + 1;
static_assert(BULLSEYE_RTTTL_MAX_CHARS == RTTTL_MAX_INPUT_CHARS,
              "Bullseye and parser RTTTL limits must stay aligned");
// The shortest useful note token is one character plus a separator.
constexpr uint8_t BULLSEYE_RTTTL_MAX_NOTES = 250;
constexpr uint32_t BULLSEYE_STABILITY_MS = 1000;

struct BullseyeMelodyConfig {
  bool enabled = false;
  char rtttl[BULLSEYE_RTTTL_CAPACITY] = {};
};

static_assert(sizeof(BullseyeMelodyConfig) == 502,
              "Bullseye config must remain a fixed-size persisted record");

inline bool bullseyeRtttlHasSafeText(const char *text) {
  if (text == nullptr) {
    return false;
  }
  const size_t length = strnlen(text, BULLSEYE_RTTTL_CAPACITY);
  if (length == 0 || length > BULLSEYE_RTTTL_MAX_CHARS) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    const unsigned char ch = static_cast<unsigned char>(text[i]);
    // RTTTL is a single-line ASCII format. Excluding JSON escape characters
    // also lets status serialization copy the validated value verbatim.
    if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\') {
      return false;
    }
  }
  return true;
}

inline bool validBullseyeRtttl(const char *text) {
  if (!bullseyeRtttlHasSafeText(text)) {
    return false;
  }
  uint8_t count = 0;
  return parseRtttlBounded(text, nullptr, BULLSEYE_RTTTL_MAX_NOTES, count) &&
         count > 0;
}

inline bool validBullseyeMelodyConfig(const BullseyeMelodyConfig &config) {
  const size_t length = strnlen(config.rtttl, BULLSEYE_RTTTL_CAPACITY);
  if (length >= BULLSEYE_RTTTL_CAPACITY) {
    return false;
  }
  if (length == 0) {
    return !config.enabled;
  }
  return validBullseyeRtttl(config.rtttl);
}

inline bool bullseyeWeightExactlyTarget(float weightG, uint16_t targetWeightDg) {
  return std::isfinite(weightG) &&
         fabsf(weightG - goalWeightDgToGrams(targetWeightDg)) < 0.005f;
}

// The just-ended shot whose settled weight the tracker watches.
struct BullseyeShot {
  uint16_t targetWeightDg = 0;
  uint32_t endedAtMs = 0;
  uint32_t dripDelayMs = 0;
  uint32_t endedWeightSequence = 0;
  uint32_t cupPlacementId = 0;
};

struct BullseyeTracker {
  bool pending = false;
  BullseyeShot shot;
  uint32_t processedWeightSequence = 0;
  uint32_t targetSinceAtMs = 0;
  uint32_t lastTargetSampleAtMs = 0;
  uint16_t targetSampleCount = 0;

  void clear() { *this = BullseyeTracker{}; }

  void arm(const BullseyeShot &ended) {
    clear();
    pending = true;
    shot = ended;
    processedWeightSequence = ended.endedWeightSequence;
  }

  uint32_t opensAtMs() const { return shot.endedAtMs + shot.dripDelayMs; }

  // Returns true once, after at least two post-shot samples have continuously
  // reported the exact target over a full second. A stale/gapped stream or a
  // different weight resets the candidate.
  bool accept(WeightSample sample, uint32_t maxSampleGapMs) {
    const uint32_t sequence = sample.packetSequence;
    const uint32_t receivedAtMs = sample.atMs;
    if (!pending || sequence == 0 || sequence == processedWeightSequence ||
        sequence == shot.endedWeightSequence ||
        static_cast<int32_t>(receivedAtMs - opensAtMs()) < 0) {
      return false;
    }
    processedWeightSequence = sequence;
    if (!bullseyeWeightExactlyTarget(sample.weightG, shot.targetWeightDg)) {
      targetSinceAtMs = 0;
      lastTargetSampleAtMs = 0;
      targetSampleCount = 0;
      return false;
    }
    const bool gap = targetSampleCount != 0 &&
                     (static_cast<int32_t>(receivedAtMs - lastTargetSampleAtMs) <= 0 ||
                      static_cast<uint32_t>(receivedAtMs - lastTargetSampleAtMs) >
                          maxSampleGapMs);
    if (targetSampleCount == 0 || gap) {
      targetSinceAtMs = receivedAtMs;
      targetSampleCount = 1;
    } else if (targetSampleCount != UINT16_MAX) {
      ++targetSampleCount;
    }
    lastTargetSampleAtMs = receivedAtMs;
    if (targetSampleCount >= 2 &&
        static_cast<uint32_t>(receivedAtMs - targetSinceAtMs) >=
            BULLSEYE_STABILITY_MS) {
      pending = false;
      return true;
    }
    return false;
  }
};

}  // namespace openbrewbyweight

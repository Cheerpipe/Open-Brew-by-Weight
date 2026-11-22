#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OpenBrewByWeightRtttl.h"

namespace openbrewbyweight {

constexpr size_t BULLSEYE_RTTTL_MAX_CHARS = 500;
constexpr size_t BULLSEYE_RTTTL_CAPACITY = BULLSEYE_RTTTL_MAX_CHARS + 1;
static_assert(BULLSEYE_RTTTL_MAX_CHARS == RTTTL_MAX_INPUT_CHARS,
              "Bullseye and parser RTTTL limits must stay aligned");
// The shortest useful note token is one character plus a separator.
constexpr uint8_t BULLSEYE_RTTTL_MAX_NOTES = 250;

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

}  // namespace openbrewbyweight

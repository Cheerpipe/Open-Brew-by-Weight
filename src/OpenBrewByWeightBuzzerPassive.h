#pragma once

#include <stdint.h>

#include "OpenBrewByWeightBuzzerPatterns.h"
#include "OpenBrewByWeightHardware.h"
#include "OpenBrewByWeightRtttl.h"

namespace openbrewbyweight {

inline bool buzzerPassiveBegin(uint8_t pin) {
  if (!ledcAttach(pin, BUZZER_TONE_HZ, 10)) {
    return false;
  }
  // Silence must hold the inactive level so the transducer is not left
  // across the noisy 3.3 V rail.
  if (!ledcOutputInvert(pin, BUZZER_ACTIVE_LEVEL == LOW)) {
    return false;
  }
  ledcWriteTone(pin, 0);
  return true;
}

inline void buzzerPassiveSetTone(uint8_t pin, uint32_t freqHz) {
  ledcWriteTone(pin, freqHz);
}

}  // namespace openbrewbyweight

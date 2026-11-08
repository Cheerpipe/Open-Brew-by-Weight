#pragma once

#include <stdint.h>

#include "OpenBrewByWeightBuzzerPatterns.h"
#include "OpenBrewByWeightRtttl.h"

namespace openbrewbyweight {

inline bool buzzerPassiveBegin(uint8_t pin) {
  if (!ledcAttach(pin, BUZZER_TONE_HZ, 10)) {
    return false;
  }
  // The recommended module (S8550 PNP) sounds on a low input: idle the pin
  // high so the transducer is not left across the noisy 3.3 V rail.
  if (!ledcOutputInvert(pin, true)) {
    return false;
  }
  ledcWriteTone(pin, 0);
  return true;
}

inline void buzzerPassiveSetTone(uint8_t pin, uint32_t freqHz) {
  ledcWriteTone(pin, freqHz);
}

}  // namespace openbrewbyweight

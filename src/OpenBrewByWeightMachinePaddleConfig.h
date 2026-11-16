#pragma once

#include <stdint.h>
#include <string.h>

namespace openbrewbyweight {

// =============================================================================
// SPECIALIZATION: Paddle — config helpers (API encoding)
// =============================================================================
// WHAT: Latch-switch brew feel (Natural / Original / Auto) and paddle-return
//       reminder bounds. Snapshot by the paddle specialization at cycle start.
//
// BOUNDARY: Paddle-only helpers. Momentary does not use this; keep it out of
// OpenBrewByWeightMachineTypes.h so brew/stopper never grow paddle-specific parsing
// paths or branch on PaddleMode.

#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_INTERVAL_MS
#define OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_INTERVAL_MS 30000
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_MAX_MS
#define OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_MAX_MS (15UL * 60UL * 1000UL)
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_REMINDER
#define OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_REMINDER 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_MODE
#define OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_MODE 0
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_MOMENTARY_START_ON_PRESS
#define OPEN_BREW_BY_WEIGHT_DEFAULT_MOMENTARY_START_ON_PRESS 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_ASSUME_IDLE_ON_SCALE_CONNECT
#define OPEN_BREW_BY_WEIGHT_DEFAULT_ASSUME_IDLE_ON_SCALE_CONNECT 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S
#define OPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S 0
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_ENABLED
#define OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_ENABLED 0
#endif

constexpr uint32_t DEFAULT_PADDLE_RETURN_REMINDER_INTERVAL_MS =
    OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_INTERVAL_MS;
constexpr uint32_t MIN_PADDLE_RETURN_REMINDER_INTERVAL_MS = 5000;
constexpr uint32_t MAX_PADDLE_RETURN_REMINDER_INTERVAL_MS = 120000;
constexpr uint32_t DEFAULT_PADDLE_RETURN_REMINDER_MAX_DURATION_MS =
    OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_MAX_MS;
constexpr uint32_t MIN_PADDLE_RETURN_REMINDER_MAX_DURATION_MS = 60000UL;
constexpr uint32_t MAX_PADDLE_RETURN_REMINDER_MAX_DURATION_MS =
    60UL * 60UL * 1000UL;

// Latch-switch brew feel. Snapshot by the machine at cycle start; brew/stopper
// never branch on this enum. Natural = OFF ends the shot. Original = legacy
// start gesture (BBW+scale: OFF after rinse keeps the circuit closed until
// weight stop; ON again promotes to Natural). Auto = BBW finish in either
// position (no hold-off of automation); OFF then ON promotes like Original.
enum class PaddleMode : uint8_t {
  NATURAL = 0,
  ORIGINAL = 1,
  AUTO = 2
};

inline bool validPaddleMode(uint8_t mode) {
  return mode == static_cast<uint8_t>(PaddleMode::NATURAL) ||
         mode == static_cast<uint8_t>(PaddleMode::ORIGINAL) ||
         mode == static_cast<uint8_t>(PaddleMode::AUTO);
}

inline const char *paddleModeId(uint8_t mode) {
  switch (static_cast<PaddleMode>(mode)) {
    case PaddleMode::ORIGINAL:
      return "original";
    case PaddleMode::AUTO:
      return "auto";
    case PaddleMode::NATURAL:
    default:
      return "natural";
  }
}

inline bool parsePaddleMode(const char *text, uint8_t &mode) {
  if (text == nullptr) {
    return false;
  }
  if (strcmp(text, "natural") == 0) {
    mode = static_cast<uint8_t>(PaddleMode::NATURAL);
    return true;
  }
  if (strcmp(text, "original") == 0) {
    mode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
    return true;
  }
  if (strcmp(text, "auto") == 0) {
    mode = static_cast<uint8_t>(PaddleMode::AUTO);
    return true;
  }
  return false;
}

}  // namespace openbrewbyweight

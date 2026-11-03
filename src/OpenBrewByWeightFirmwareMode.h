#pragma once

// Boot-scoped firmware mode (master switch). The persisted record lives in
// its own NVS namespace — deliberately outside the PersistedSettings blob —
// so toggling the mode never rewrites the settings blob. An absent, invalid, or
// unreadable record means compatibility mode: a fresh or factory-reset
// controller stays transparent until the owner enables the firmware.

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightPreferences.h"

namespace openbrewbyweight {

constexpr const char *FIRMWARE_MODE_NAMESPACE = "fwmode";
constexpr const char *FIRMWARE_MODE_KEY = "mode";

enum class FirmwareMode : uint8_t {
  FULL = 1,
  COMPATIBILITY = 2,
};

inline bool validFirmwareMode(uint8_t raw) {
  return raw == static_cast<uint8_t>(FirmwareMode::FULL) ||
         raw == static_cast<uint8_t>(FirmwareMode::COMPATIBILITY);
}

// Onboarding "brew by weight" choice, written before the Wi-Fi commit it is
// bound to (staCrc of the staged STA config) so boot can finish or drop it
// with that Wi-Fi: the pair is saved together or not at all.
constexpr const char *SETUP_INTENT_KEY = "setup";
constexpr uint8_t SETUP_INTENT_VERSION = 1;

struct SetupIntent {
  uint8_t version = SETUP_INTENT_VERSION;
  uint8_t presetId = 0;
  bool bound = false;
  uint8_t reserved = 0;
  uint32_t staCrc = 0;
};
static_assert(sizeof(SetupIntent) == 8, "setup intent layout changed");

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline bool readFirmwareModeRecord(const char *key, void *value, size_t size) {
  if (!lockFlashIo()) {
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::FIRMWARE_MODE);
  bool read = false;
  if (preferences.begin(FIRMWARE_MODE_NAMESPACE, true)) {
    read = preferences.getBytesLength(key) == size &&
           preferences.getBytes(key, value, size) == size;
    preferences.end();
  }
  unlockFlashIo();
  return read;
}

// Writes and read-back verifies one record; a null value removes it.
inline bool writeFirmwareModeRecord(const char *key, const void *value,
                                    size_t size) {
  if (!lockFlashIo()) {
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::FIRMWARE_MODE);
  if (!preferences.begin(FIRMWARE_MODE_NAMESPACE, false)) {
    unlockFlashIo();
    return false;
  }
  bool saved = false;
  if (value == nullptr) {
    saved = (preferences.getBytesLength(key) == 0 || preferences.remove(key)) &&
            preferences.getBytesLength(key) == 0;
  } else {
    uint8_t verified[sizeof(SetupIntent)] = {};
    saved = size <= sizeof(verified) &&
            preferences.putBytes(key, value, size) == size &&
            preferences.getBytesLength(key) == size &&
            preferences.getBytes(key, verified, size) == size &&
            memcmp(verified, value, size) == 0;
  }
  preferences.end();
  unlockFlashIo();
  return saved;
}

inline FirmwareMode loadFirmwareMode() {
  uint8_t raw = 0;
  return readFirmwareModeRecord(FIRMWARE_MODE_KEY, &raw, sizeof(raw)) &&
                 validFirmwareMode(raw)
             ? static_cast<FirmwareMode>(raw)
             : FirmwareMode::COMPATIBILITY;
}

inline bool saveFirmwareMode(FirmwareMode mode) {
  const uint8_t raw = static_cast<uint8_t>(mode);
  return validFirmwareMode(raw) &&
         writeFirmwareModeRecord(FIRMWARE_MODE_KEY, &raw, sizeof(raw));
}

inline bool loadSetupIntent(SetupIntent &intent) {
  return readFirmwareModeRecord(SETUP_INTENT_KEY, &intent, sizeof(intent)) &&
         intent.version == SETUP_INTENT_VERSION && intent.presetId != 0;
}

inline bool saveSetupIntent(const SetupIntent &intent) {
  return writeFirmwareModeRecord(SETUP_INTENT_KEY, &intent, sizeof(intent));
}

inline bool clearSetupIntent() {
  return writeFirmwareModeRecord(SETUP_INTENT_KEY, nullptr, 0);
}

inline bool clearFirmwareMode() {
  return writeFirmwareModeRecord(FIRMWARE_MODE_KEY, nullptr, 0) &&
         clearSetupIntent();
}
#endif

}  // namespace openbrewbyweight

// Live boot-scoped copy. setup() writes it once from loadFirmwareMode();
// it is immutable for the rest of the boot so every subsystem gate reads one
// stable value with no transition states.
extern uint8_t firmwareModeRaw;

inline bool firmwareCompatibilityMode() {
  return firmwareModeRaw ==
         static_cast<uint8_t>(openbrewbyweight::FirmwareMode::COMPATIBILITY);
}

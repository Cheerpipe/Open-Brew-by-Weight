#pragma once

// NVS I/O for the companion pairing blob (OpenBrewByWeightCompanionLink.h),
// Linea Micra builds. One key in the settings namespace; NVS keeps the
// previous value until a write commits. Callers pass a stack copy because
// flash I/O must not read PSRAM.

#include "OpenBrewByWeightPersistence.h"
#include "network/OpenBrewByWeightCompanionLink.h"

namespace openbrewbyweight {

constexpr const char *COMPANION_KEY = "companion";

enum class CompanionLoad : uint8_t { LOADED, MISSING, INVALID };

// A missing blob is a controller that never paired; INVALID is worth a log line.
inline CompanionLoad loadCompanionStored(CompanionStored &stored) {
  CompanionLoad result = CompanionLoad::INVALID;
  if (lockSettingsNvs()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::COMPANION);
    result = CompanionLoad::MISSING;
    if (preferences.begin(SETTINGS_NAMESPACE, true)) {
      if (preferences.isKey(COMPANION_KEY)) {
        const bool loaded =
            preferences.getBytesLength(COMPANION_KEY) == sizeof(stored) &&
            preferences.getBytes(COMPANION_KEY, &stored, sizeof(stored)) == sizeof(stored) &&
            validCompanionStored(stored);
        result = loaded ? CompanionLoad::LOADED : CompanionLoad::INVALID;
      }
      preferences.end();
    }
    unlockSettingsNvs();
  }
  if (result != CompanionLoad::LOADED) stored = CompanionStored{};
  return result;
}

inline bool saveCompanionStored(CompanionStored &stored) {
  finalizeCompanionStored(stored);
  if (!lockSettingsNvs()) return false;
  OpenBrewByWeightPreferences preferences(NvsSubsystem::COMPANION);
  bool saved = false;
  if (preferences.begin(SETTINGS_NAMESPACE, false)) {
    saved = preferences.putBytes(COMPANION_KEY, &stored, sizeof(stored)) == sizeof(stored);
    preferences.end();
  }
  unlockSettingsNvs();
  return saved;
}

inline bool eraseCompanionStored() {
  return eraseSettingsNvsKey(NvsSubsystem::COMPANION, COMPANION_KEY);
}

}  // namespace openbrewbyweight

#pragma once

// NVS I/O for the Web UI access blob (OpenBrewByWeightWebAccess.h), one key in
// the settings namespace (see loadSettingsBlob).

#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightWebAccess.h"

namespace openbrewbyweight {

constexpr const char *WEB_ACCESS_KEY = "webAccess";

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)

using WebAccessLoad = SettingsBlobLoad;

// A missing or invalid blob loads as the default: login off.
inline WebAccessLoad loadWebAccessStored(WebAccessStored &stored) {
  return loadSettingsBlob(NvsSubsystem::WEB_ACCESS, WEB_ACCESS_KEY, stored,
                          validWebAccessStored);
}

inline bool saveWebAccessStored(WebAccessStored &stored) {
  finalizeWebAccessStored(stored);
  return saveSettingsBlob(NvsSubsystem::WEB_ACCESS, WEB_ACCESS_KEY, stored);
}

// Factory reset forgets everything, including the Home notice choice.
inline bool eraseWebAccessStored() {
  return eraseSettingsNvsKey(NvsSubsystem::WEB_ACCESS, WEB_ACCESS_KEY);
}
#endif

}  // namespace openbrewbyweight

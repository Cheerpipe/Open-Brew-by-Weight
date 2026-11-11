#pragma once

#include <stdint.h>

#if __has_include("OpenBrewByWeightBuildProfileGenerated.h")
#include "OpenBrewByWeightBuildProfileGenerated.h"
#elif defined(ESP_PLATFORM)
#error "Firmware builds need the profile header resolved from --hardware and --machine"
#endif

#ifndef OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID
#define OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID "unknown"
#endif
#ifndef OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID
#define OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID "unknown"
#endif
#ifndef OPEN_BREW_BY_WEIGHT_MACHINE_BRAND
#define OPEN_BREW_BY_WEIGHT_MACHINE_BRAND "Generic"
#endif
#ifndef OPEN_BREW_BY_WEIGHT_MACHINE_MODEL
#define OPEN_BREW_BY_WEIGHT_MACHINE_MODEL "Compile-time selection"
#endif
#ifndef OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_NONE
#define OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_NONE 0
#endif
#ifndef OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LA_MARZOCCO_CLOUD
#define OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LA_MARZOCCO_CLOUD 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION
#define OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_NONE
#endif
#ifndef OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_DURATION_MS
#define OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_DURATION_MS 4000
#endif

namespace openbrewbyweight {

constexpr uint32_t DEFAULT_RINSE_DURATION_MS =
    OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_DURATION_MS;

}  // namespace openbrewbyweight

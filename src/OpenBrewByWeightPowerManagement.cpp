#include "OpenBrewByWeightPowerManagement.h"
#include <esp_pm.h>
#include <esp32-hal-cpu.h>

namespace openbrewbyweight {

bool applyPowerProfile(PowerProfile profile, bool boost) {
  static PowerClock clock;
  powerRequestedProfile.store(profile, std::memory_order_release);
  if (powerBleError.load(std::memory_order_acquire) != 0) profile = PowerProfile::OFF;
  const bool safe = clock.apply(profile, boost, [](int minimum, int maximum) {
    esp_pm_config_t config = {};
    config.min_freq_mhz = minimum;
    config.max_freq_mhz = maximum;
    config.light_sleep_enable = false;
    const esp_err_t error = esp_pm_configure(&config);
    if (error != ESP_OK) return int(error);
    return minimum == maximum && getCpuFrequencyMhz() != uint32_t(maximum)
               ? int(ESP_ERR_INVALID_STATE) : int(ESP_OK);
  });
  powerClockError.store(clock.error(), std::memory_order_relaxed);
  powerAppliedProfile.store(clock.applied(), std::memory_order_release);
  return safe;
}

}  // namespace openbrewbyweight

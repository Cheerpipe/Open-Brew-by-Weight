#pragma once

#include "OpenBrewByWeightPowerPolicy.h"
#include <atomic>

namespace openbrewbyweight {

// Fixed-size cross-owner mailbox; no owner reads another service's live state.
// Control writes applied profile. HTTP writes web expiry; control expires it.
inline std::atomic<uint32_t> powerWebUntilMs{0};
inline std::atomic<bool> powerScaleBusy{false};
inline std::atomic<bool> powerNetworkBusy{true};
inline std::atomic<PowerProfile> powerAppliedProfile{PowerProfile::OFF};
inline std::atomic<PowerProfile> powerRequestedProfile{PowerProfile::OFF};
inline std::atomic<int> powerClockError{0};
inline std::atomic<int> powerBleError{0};
inline std::atomic<bool> powerBleSleeping{false};
inline std::atomic<uint32_t> powerCooldownRemainingMs{0};

inline void notePowerWebActivity(uint32_t now, uint32_t seconds) {
  if (seconds == 0 || seconds > 30) return;
  const uint32_t until = now + seconds * 1000;
  powerWebUntilMs.store(until == 0 ? 1 : until, std::memory_order_release);
}
inline bool powerWebActive(uint32_t now) {
  uint32_t until = powerWebUntilMs.load(std::memory_order_acquire);
  if (until == 0) return false;
  if (int32_t(until - now) > 0) return true;
  powerWebUntilMs.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
  return false;
}
inline bool powerIdleSavings() {
  return powerAppliedProfile.load(std::memory_order_acquire) == PowerProfile::IDLE;
}

// Called only by control, outside critical sections. No actuation here.
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
inline bool applyPowerProfile(PowerProfile profile, bool = false) {
  powerRequestedProfile.store(profile, std::memory_order_release);
  powerAppliedProfile.store(powerBleError.load() == 0 ? profile : PowerProfile::OFF,
                            std::memory_order_release);
  return true;
}
#else
// boost: Performance boost lets the scale-window profiles reach 160 MHz.
bool applyPowerProfile(PowerProfile profile, bool boost = false);
#endif

}  // namespace openbrewbyweight

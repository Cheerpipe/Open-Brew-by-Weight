#pragma once

#include "OpenBrewByWeightLineaMicraSettings.h"
#include "OpenBrewByWeightTimingTypes.h"

#include <cstdint>

namespace openbrewbyweight {

// ScaleDisconnectReason::REMOTE_DISCONNECTED is the only disconnect treated
// as an explicit, scale-initiated power-off. The numeric value mirrors the
// BLE library enum (see scaleDisconnectReasonName); supervision timeouts,
// packet timeouts, scan failures, and firmware-requested disconnects never
// match, so radio silence cannot turn the machine off.
constexpr uint8_t LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT = 9;

// Machine-side policy for turning the Micra off when the scale powers off.
// The stopper only forwards scale link edges and relay state each loop; this
// tracker owns the eligibility rules and the grace window, and the Micra
// cloud service owns the standby command.
class MicraScaleShutdownTracker {
 public:
  struct Snapshot {
    uint32_t disconnectSequence = 0;
    uint8_t disconnectReason = 0;
    bool linkUp = false;
    bool relayClosed = false;
  };

  // Returns true exactly once per accepted trigger: an explicit scale
  // power-off observed while the relay was open stayed offline for the
  // configured grace window with the option and account still active.
  bool service(uint32_t now, const Snapshot &scale, uint8_t micraOptions,
               bool accountConfigured) {
    const bool enabled =
        accountConfigured &&
        (micraOptions & LINEA_MICRA_SHUTDOWN_WITH_SCALE) != 0;
    if (scale.disconnectSequence != lastDisconnectSequence_) {
      lastDisconnectSequence_ = scale.disconnectSequence;
      // A closed relay (shot or rinse in progress, wake passthrough) means
      // the event is ignored completely, never deferred.
      pending_ = enabled && !scale.linkUp && !scale.relayClosed &&
                 scale.disconnectReason == LINEA_MICRA_SCALE_EXPLICIT_DISCONNECT;
      fireAtMs_ = now + graceMs(micraOptions);
    } else if (pending_) {
      if (scale.linkUp || !enabled) pending_ = false;
    }
    if (!pending_) return false;
    if (tickBefore(now, fireAtMs_)) return false;
    pending_ = false;
    // A relay that closed during the grace window cancels the shutdown;
    // the machine is never powered off while brewing.
    return !scale.relayClosed;
  }

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  bool pending() const { return pending_; }
#endif

 private:
  static uint32_t graceMs(uint8_t micraOptions) {
    return 1000U * lineaMicraShutdownGraceSeconds(micraOptions);
  }

  uint32_t lastDisconnectSequence_ = 0;
  uint32_t fireAtMs_ = 0;
  bool pending_ = false;
};

}  // namespace openbrewbyweight

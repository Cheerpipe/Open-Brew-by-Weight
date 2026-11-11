#pragma once

#include "OpenBrewByWeightLaMarzoccoSettings.h"
#include "OpenBrewByWeightLaMarzoccoScaleShutdown.h"

#include <cstdint>

namespace openbrewbyweight {

// Machine-side policy for turning the Micra on when the scale powers on: the
// inverse of LaMarzoccoScaleShutdownTracker. A link-up edge counts as the scale
// powering on only when the preceding disconnect was the scale's own
// power-off (REMOTE_DISCONNECTED), so radio-loss reconnects and the first
// connection after boot never wake the machine. The Micra cloud service owns
// the BrewingMode command.
class LaMarzoccoScalePowerOnTracker {
 public:
  struct Snapshot {
    bool linkUp = false;
    uint8_t lastDisconnectReason = 0;
    bool relayClosed = false;
  };

  // Returns true exactly once per accepted trigger: the scale came back from
  // its own explicit power-off while the option and account stayed active and
  // the relay stayed open. A trigger observed with the option off or a closed
  // relay is consumed, never deferred.
  bool service(const Snapshot &observed, uint8_t laMarzoccoOptions,
               bool accountConfigured) {
    const bool enabled =
        accountConfigured &&
        (laMarzoccoOptions & LA_MARZOCCO_POWER_ON_WITH_SCALE) != 0;
    const bool edge = !lastLinkUp_ && observed.linkUp;
    lastLinkUp_ = observed.linkUp;
    if (!edge || !enabled || observed.relayClosed) return false;
    return observed.lastDisconnectReason == LA_MARZOCCO_SCALE_EXPLICIT_DISCONNECT;
  }

 private:
  bool lastLinkUp_ = false;
};

}  // namespace openbrewbyweight

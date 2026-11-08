#pragma once

#include "OpenBrewByWeightLineaMicraSettings.h"
#include "OpenBrewByWeightLineaMicraTypes.h"

namespace openbrewbyweight {

// Scale-discovery duty requested from the machine power state. NONE leaves
// the saved BLE scan mode in charge.
enum class MicraScanOverride : uint8_t { NONE, AGGRESSIVE, RELAXED };

// The assumed power state the firmware acts on: a live optimistic overlay
// still leaves the last confirmed value in powerState, so the overlay flags
// replace it; together they are the same resolved state Home shows. UNKNOWN —
// whatever effectiveOn says — covers no account, observation disabled,
// unsupported, and failed observations.
inline LineaMicraPowerState micraAssumedPowerState(const LineaMicraStatus &machine) {
  return machine.optimisticOn    ? LineaMicraPowerState::ON
         : machine.optimisticOff ? LineaMicraPowerState::OFF
                                 : machine.powerState;
}

// One compact discovery override computed from the persisted scale options
// and the assumed power state. Only a known ON or OFF state can override;
// UNKNOWN keeps NONE.
inline MicraScanOverride micraScanOverride(const LineaMicraStatus &machine,
                                           uint8_t micraScaleOptions) {
  const LineaMicraPowerState resolved = micraAssumedPowerState(machine);
  if (resolved == LineaMicraPowerState::ON &&
      (micraScaleOptions & LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) != 0) {
    return MicraScanOverride::AGGRESSIVE;
  }
  if (resolved == LineaMicraPowerState::OFF &&
      (micraScaleOptions & LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF) != 0) {
    return MicraScanOverride::RELAXED;
  }
  return MicraScanOverride::NONE;
}

// Machine-side policy for powering the scale off when the machine powers off.
// Only current-quality dashboard classifications change the confirmed state;
// stale, optimistic, unknown, or failed observations never arm or fire the
// trigger. The scale worker owns the actual power-off command and its
// protocol support check.
class MicraMachinePowerTracker {
 public:
  // Returns true exactly once per confirmed ON -> OFF transition observed
  // while the option and account stayed active. A transition observed with
  // the option off is consumed, never deferred.
  bool service(const LineaMicraStatus &machine, uint8_t micraScaleOptions,
               bool accountConfigured) {
    if (machine.quality != LineaMicraObservationQuality::CURRENT) return false;
    if (machine.powerState == LineaMicraPowerState::ON) {
      confirmedOn_ = true;
    } else if (machine.powerState == LineaMicraPowerState::OFF &&
               confirmedOn_) {
      confirmedOn_ = false;
      return accountConfigured &&
             (micraScaleOptions & LINEA_MICRA_SCALE_OFF_WITH_MACHINE) != 0;
    }
    return false;
  }

 private:
  bool confirmedOn_ = false;
};

}  // namespace openbrewbyweight

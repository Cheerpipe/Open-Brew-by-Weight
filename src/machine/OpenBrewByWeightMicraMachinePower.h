#pragma once

#include "OpenBrewByWeightLineaMicraSettings.h"
#include "OpenBrewByWeightLineaMicraTypes.h"

namespace openbrewbyweight {

// Scale-discovery duty requested from the machine power state. NONE leaves
// the saved BLE scan mode in charge.
enum class MicraScanOverride : uint8_t { NONE, AGGRESSIVE, RELAXED };

// One compact discovery override computed from the persisted scale options
// and the assumed power state. Assumptions count like confirmations; UNKNOWN
// keeps NONE.
inline MicraScanOverride micraScanOverride(const LineaMicraStatus &machine,
                                           uint8_t micraScaleOptions) {
  switch (machine.assumedPower) {
    case LineaMicraAssumedPower::ON:
    case LineaMicraAssumedPower::ON_STALE:
      return (micraScaleOptions & LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON) != 0
                 ? MicraScanOverride::AGGRESSIVE : MicraScanOverride::NONE;
    case LineaMicraAssumedPower::OFF:
    case LineaMicraAssumedPower::OFF_STALE:
      return (micraScaleOptions & LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF) != 0
                 ? MicraScanOverride::RELAXED : MicraScanOverride::NONE;
    case LineaMicraAssumedPower::UNKNOWN: break;
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

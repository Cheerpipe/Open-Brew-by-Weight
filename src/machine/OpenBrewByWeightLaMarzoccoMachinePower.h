#pragma once

#include "OpenBrewByWeightLaMarzoccoSettings.h"
#include "OpenBrewByWeightLaMarzoccoTypes.h"

namespace openbrewbyweight {

// Scale-discovery duty requested from the machine power state. NONE leaves
// the saved BLE scan mode in charge.
enum class LaMarzoccoScanOverride : uint8_t { NONE, AGGRESSIVE, RELAXED };

// One compact discovery override computed from the persisted scale options
// and the assumed power state. Assumptions count like confirmations; UNKNOWN
// keeps NONE.
inline LaMarzoccoScanOverride laMarzoccoScanOverride(const LaMarzoccoStatus &machine,
                                           uint8_t laMarzoccoScaleOptions) {
  switch (machine.assumedPower) {
    case LaMarzoccoAssumedPower::ON:
    case LaMarzoccoAssumedPower::ON_STALE:
      return (laMarzoccoScaleOptions & LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON) != 0
                 ? LaMarzoccoScanOverride::AGGRESSIVE : LaMarzoccoScanOverride::NONE;
    case LaMarzoccoAssumedPower::OFF:
    case LaMarzoccoAssumedPower::OFF_STALE:
      return (laMarzoccoScaleOptions & LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF) != 0
                 ? LaMarzoccoScanOverride::RELAXED : LaMarzoccoScanOverride::NONE;
    case LaMarzoccoAssumedPower::UNKNOWN: break;
  }
  return LaMarzoccoScanOverride::NONE;
}

// Machine-side policy for powering the scale off when the machine powers off.
// Only current-quality dashboard classifications change the confirmed state;
// stale, optimistic, unknown, or failed observations never arm or fire the
// trigger. The scale worker owns the actual power-off command and its
// protocol support check.
class LaMarzoccoMachinePowerTracker {
 public:
  // Returns true exactly once per confirmed ON -> OFF transition observed
  // while the option and account stayed active. A transition observed with
  // the option off is consumed, never deferred.
  bool service(const LaMarzoccoStatus &machine, uint8_t laMarzoccoScaleOptions,
               bool accountConfigured) {
    if (machine.quality != LaMarzoccoObservationQuality::CURRENT) return false;
    if (machine.powerState == LaMarzoccoPowerState::ON) {
      confirmedOn_ = true;
    } else if (machine.powerState == LaMarzoccoPowerState::OFF &&
               confirmedOn_) {
      confirmedOn_ = false;
      return accountConfigured &&
             (laMarzoccoScaleOptions & LA_MARZOCCO_SCALE_OFF_WITH_MACHINE) != 0;
    }
    return false;
  }

 private:
  bool confirmedOn_ = false;
};

}  // namespace openbrewbyweight

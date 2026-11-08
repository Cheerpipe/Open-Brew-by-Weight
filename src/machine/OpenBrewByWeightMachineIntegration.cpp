#include "OpenBrewByWeightMachineIntegration.h"

namespace openbrewbyweight {
void inhibitMachineIntegrationCloud() {}

bool initializeMachineIntegration() { return true; }
void publishMachineIntegrationConfig(const PersistedSettings &, uint32_t) {}
void publishMachineIntegrationNetworkState(bool, bool, bool, bool) {}
void serviceMachineIntegrationScaleLink(uint32_t, bool, uint32_t, uint8_t,
                                        bool) {}
void serviceMachineIntegrationMachinePower(bool, bool, bool) {}
MachinePower machineIntegrationPowerState() { return MachinePower::UNKNOWN; }
void requestMachineIntegrationPresetTemperature(PresetTemperature, uint32_t) {}
uint16_t machineIntegrationShotTargetDeciC() { return 0; }
MachinePhysicalStartDisposition machineIntegrationPhysicalStart(MachineBackflushPermit *permit) {
  if (permit) *permit = {};
  return MachinePhysicalStartDisposition::NORMAL;
}
MachineBackflushSnapshot machineIntegrationBackflush(bool) { return {}; }
void finishMachineIntegrationBackflush(uint32_t) {}

}  // namespace openbrewbyweight

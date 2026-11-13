#include "OpenBrewByWeightMachineIntegration.h"

namespace openbrewbyweight {
void inhibitMachineIntegrationCloud() {}

bool initializeMachineIntegration() { return true; }
void publishMachineIntegrationConfig([[maybe_unused]] const PersistedSettings &settings,
                                     [[maybe_unused]] uint32_t configGeneration) {}
void publishMachineIntegrationManualPower([[maybe_unused]] bool enabled) {}
void publishMachineIntegrationNetworkState([[maybe_unused]] bool staConnected,
                                           [[maybe_unused]] bool apActive,
                                           [[maybe_unused]] bool shotActive,
                                           [[maybe_unused]] bool scaleConnecting) {}
void serviceMachineIntegrationScaleLink([[maybe_unused]] uint32_t now,
                                        [[maybe_unused]] bool scaleLinkUp,
                                        [[maybe_unused]] uint32_t scaleDisconnectSequence,
                                        [[maybe_unused]] uint8_t scaleDisconnectReason,
                                        [[maybe_unused]] bool machineBusy) {}
void serviceMachineIntegrationMachinePower([[maybe_unused]] bool scaleLinkUp,
                                           [[maybe_unused]] bool scaleSupportsPowerOff,
                                           [[maybe_unused]] bool machineBusy) {}
MachinePower machineIntegrationPowerState() { return MachinePower::UNKNOWN; }
void requestMachineIntegrationPresetTemperature([[maybe_unused]] PresetTemperature preset,
                                                [[maybe_unused]] uint32_t configGeneration) {}
uint16_t machineIntegrationShotTargetDeciC() { return 0; }
MachinePhysicalStartDisposition machineIntegrationPhysicalStart(MachineBackflushPermit *permit) {
  if (permit) *permit = {};
  return MachinePhysicalStartDisposition::NORMAL;
}
MachineBackflushSnapshot machineIntegrationBackflush([[maybe_unused]] bool consume) { return {}; }
void finishMachineIntegrationBackflush([[maybe_unused]] uint32_t attempt) {}

}  // namespace openbrewbyweight

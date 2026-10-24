#pragma once

#include <cstdint>
#include "OpenBrewByWeightMachineBackflush.h"

namespace openbrewbyweight {
// Callback-safe wakeup; admission itself is published by discovery.
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
inline uint32_t hostMachineCloudInhibitCount = 0;
inline void inhibitMachineIntegrationCloud() { ++hostMachineCloudInhibitCount; }
#else
void inhibitMachineIntegrationCloud();
#endif

struct PersistedSettings;

enum class MachinePhysicalStartDisposition : uint8_t {
  NORMAL,
  WAKE_PASSTHROUGH,
  BACKFLUSH_CANDIDATE,
  BLOCKED_CLEANING
};

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
inline bool initializeMachineIntegration() { return true; }
inline void publishMachineIntegrationConfig(const PersistedSettings &,
                                            uint32_t) {}
inline void publishMachineIntegrationNetworkState(bool, bool, bool,
                                                  bool = false) {}
inline void serviceMachineIntegrationScaleLink(uint32_t, bool, uint32_t,
                                               uint8_t, bool) {}
inline void serviceMachineIntegrationMachinePower(bool, bool, bool) {}
inline uint32_t hostMachineTemperatureRequestCount = 0;
inline uint8_t hostMachineTemperaturePresetId = 0;
inline uint32_t hostMachineTemperatureGeneration = 0;
inline uint16_t hostMachineTemperatureTargetDeciC = 0;
inline void requestMachineIntegrationPresetTemperature(
    uint8_t presetId, uint32_t configGeneration, uint16_t targetDeciC) {
  ++hostMachineTemperatureRequestCount;
  hostMachineTemperaturePresetId = presetId;
  hostMachineTemperatureGeneration = configGeneration;
  hostMachineTemperatureTargetDeciC = targetDeciC;
}
inline uint16_t hostMachineShotTargetDeciC = 0;
inline uint16_t machineIntegrationShotTargetDeciC() {
  return hostMachineShotTargetDeciC;
}
inline constexpr uint8_t machineIntegrationTaskCount() { return 0; }
inline MachinePhysicalStartDisposition
    hostMachinePhysicalStartDisposition =
        MachinePhysicalStartDisposition::NORMAL;
inline uint32_t hostMachinePhysicalStartCount = 0;
inline MachineBackflushObservations hostBackflushObservations;
inline MachineBackflushSnapshot machineIntegrationBackflush(bool consume = false) {
  return consume ? hostBackflushObservations.take() : hostBackflushObservations.status();
}
inline void finishMachineIntegrationBackflush(uint32_t attempt) {
  hostBackflushObservations.finish(attempt);
}
inline MachinePhysicalStartDisposition machineIntegrationPhysicalStart(
    MachineBackflushPermit *permit = nullptr) {
  ++hostMachinePhysicalStartCount;
  const auto admitted = hostBackflushObservations.start();
  if (permit) *permit = admitted;
  if (admitted.attempt) return MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE;
  if (hostBackflushObservations.status().busy())
    return MachinePhysicalStartDisposition::BLOCKED_CLEANING;
  return hostMachinePhysicalStartDisposition;
}
#else
bool initializeMachineIntegration();
void publishMachineIntegrationConfig(const PersistedSettings &settings,
                                     uint32_t configGeneration);
void publishMachineIntegrationNetworkState(bool staConnected, bool apActive,
                                           bool shotActive,
                                           bool scaleConnecting = false);
// Forwards one scale link observation per control loop so the machine
// integration can apply its own scale power-off policy. The stopper stays a
// coordinator and owns no machine logic. machineBusy: relay closed or a
// backflush in progress (the loop's own backflush observation).
void serviceMachineIntegrationScaleLink(uint32_t now, bool scaleLinkUp,
                                        uint32_t scaleDisconnectSequence,
                                        uint8_t scaleDisconnectReason,
                                        bool machineBusy);
// Forwards one machine power observation per control loop plus the scale
// link's ability to accept a power-off command; the integration decides
// whether the connected scale is switched off and publishes its discovery
// duty override, if any.
void serviceMachineIntegrationMachinePower(bool scaleLinkUp,
                                           bool scaleSupportsPowerOff,
                                           bool machineBusy);
void requestMachineIntegrationPresetTemperature(
    uint8_t presetId, uint32_t configGeneration, uint16_t targetDeciC);
// The integration's current coffee target in deci-Celsius for the shot
// record's snapshot, taken on the control task at the shot's end; 0 when no
// integration reports a valid target.
uint16_t machineIntegrationShotTargetDeciC();
uint8_t machineIntegrationTaskCount();
MachinePhysicalStartDisposition machineIntegrationPhysicalStart(
    MachineBackflushPermit *permit = nullptr);
MachineBackflushSnapshot machineIntegrationBackflush(bool consume = false);
void finishMachineIntegrationBackflush(uint32_t attempt);
#endif

}  // namespace openbrewbyweight

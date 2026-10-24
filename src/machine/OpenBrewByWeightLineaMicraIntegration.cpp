#include "OpenBrewByWeightLineaMicraIntegration.h"

#include "OpenBrewByWeightMachineIntegration.h"
#include "OpenBrewByWeightMicraMachinePower.h"
#include "OpenBrewByWeightMicraScalePowerOn.h"
#include "OpenBrewByWeightMicraScaleShutdown.h"
#include "OpenBrewByWeightMicraService.h"
#include "OpenBrewByWeightPersistedSettings.h"
#include "OpenBrewByWeightScaleWorker.h"

#include <atomic>
#include <cstdint>

void serialTraceCategoryf(openbrewbyweight::LogLevel level,
                          openbrewbyweight::DebugCategory category,
                          const char *fmt, ...);

namespace openbrewbyweight {
namespace {

OpenBrewByWeightMicraService service;
MicraScaleShutdownTracker scaleShutdown;
MicraScalePowerOnTracker scalePowerOn;
MicraMachinePowerTracker machinePower;
std::atomic<uint8_t> micraOptions{LINEA_MICRA_DEFAULT_OPTIONS};
std::atomic<uint8_t> micraScaleOptions{0};
std::atomic<bool> micraAccountConfigured{false};

// A standby command is pointless once the machine is already effectively off.
// Skipping it also keeps the remote disconnect that follows our own scale
// power-off command from re-arming the shutdown cycle. A stale poll cache
// counts as unknown, not off: the redundant command is re-validated anyway.
bool machineEffectivelyOff() {
  const LineaMicraStatus status = service.statusPoll();
  return !service.statusPollStale() && !status.effectiveOn &&
         (status.quality == LineaMicraObservationQuality::CURRENT ||
          status.quality == LineaMicraObservationQuality::OPTIMISTIC);
}

}  // namespace

bool initializeMachineIntegration() { return service.begin(); }

void publishMachineIntegrationConfig(const PersistedSettings &settings,
                                     uint32_t configGeneration) {
  micraOptions.store(settings.lineaMicra.options,
                     std::memory_order_relaxed);
  micraScaleOptions.store(settings.lineaMicra.scaleOptions,
                          std::memory_order_relaxed);
  micraAccountConfigured.store(settings.lineaMicra.accountConfigured,
                               std::memory_order_relaxed);
  service.publishConfig(settings.lineaMicra, configGeneration);
}

void publishMachineIntegrationNetworkState(bool staConnected, bool apActive,
                                           bool shotActive,
                                           bool scaleConnecting) {
  service.publishNetworkState(staConnected, apActive, shotActive,
                              scaleConnecting);
}

void inhibitMachineIntegrationCloud() { service.inhibitCloud(); }

void requestMachineIntegrationPresetTemperature(
    uint8_t presetId, uint32_t configGeneration, uint16_t targetDeciC) {
  LineaMicraRequest request;
  request.configGeneration = configGeneration;
  request.targetDeciC = targetDeciC;
  request.presetId = presetId;
  request.type = LineaMicraRequestType::APPLY_TEMPERATURE;
  service.post(request);
}

// Control task only (the finalize path): the cached poll never blocks on the
// service mutex, and the loop refreshes that cache each iteration.
uint16_t machineIntegrationShotTargetDeciC() {
  const LineaMicraStatus status = service.statusPoll();
  return status.targetValid ? status.targetDeciC : 0;
}

void serviceMachineIntegrationScaleLink(uint32_t now, bool scaleLinkUp,
                                        uint32_t scaleDisconnectSequence,
                                        uint8_t scaleDisconnectReason,
                                        bool machineBusy) {
  const uint8_t options = micraOptions.load(std::memory_order_relaxed);
  const bool accountConfigured =
      micraAccountConfigured.load(std::memory_order_relaxed);
  const MicraScaleShutdownTracker::Snapshot shutdownSnapshot{
      scaleDisconnectSequence, scaleDisconnectReason, scaleLinkUp,
      machineBusy};
  if (scaleShutdown.service(now, shutdownSnapshot, options,
                            accountConfigured) &&
      !machineEffectivelyOff()) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "Micra scale shutdown: scale powered off, requesting StandBy");
    LineaMicraRequest request;
    request.type = LineaMicraRequestType::SET_STANDBY;
    service.post(request);
  }
  const MicraScalePowerOnTracker::Snapshot powerOnSnapshot{
      scaleLinkUp, scaleDisconnectReason, machineBusy};
  if (scalePowerOn.service(powerOnSnapshot, options, accountConfigured)) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "Micra scale power-on: scale powered on, requesting BrewingMode");
    LineaMicraRequest request;
    request.type = LineaMicraRequestType::SET_POWER_ON;
    service.post(request);
  }
}

void serviceMachineIntegrationMachinePower(bool scaleLinkUp,
                                           bool scaleSupportsPowerOff,
                                           bool machineBusy) {
  const uint8_t scaleOptions =
      micraScaleOptions.load(std::memory_order_relaxed);
  const LineaMicraStatus status = service.statusPoll();
  // Refresh the discovery override every loop before any scale power-off
  // early return: eligibility changes (option, identity, observation, or
  // state) clear it here without a separate timer or event.
  const MicraScanOverride scanOverride = micraScanOverride(status,
                                                           scaleOptions);
  applyLiveBleScanOverride(
      scanOverride == MicraScanOverride::AGGRESSIVE
          ? BleScanIntensity::AGGRESSIVE
      : scanOverride == MicraScanOverride::RELAXED
          ? BleScanIntensity::RELAXED
          : BleScanIntensity::BALANCED);
  if (!machinePower.service(status, scaleOptions,
                            micraAccountConfigured.load(
                                std::memory_order_relaxed))) {
    return;
  }
  if (!scaleLinkUp || machineBusy) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                         "Scale power-off skipped: scale %s",
                         scaleLinkUp ? "busy with an active cycle"
                                     : "not connected");
    return;
  }
  if (!scaleSupportsPowerOff) {
    serialTraceCategoryf(
        LogLevel::WARNING, DebugCategory::SCALE,
        "Scale power-off skipped: connected scale does not support power-off");
    return;
  }
  requestScalePowerOff();
}

uint8_t machineIntegrationTaskCount() { return 1; }

MachinePhysicalStartDisposition machineIntegrationPhysicalStart(MachineBackflushPermit *permit) {
  return service.physicalStart(permit);
}

MachineBackflushSnapshot machineIntegrationBackflush(bool consume) {
  return service.backflush(consume);
}

void finishMachineIntegrationBackflush(uint32_t attempt) { service.finishBackflush(attempt); }

bool queueMachineIntegrationConnect(uint32_t requestId, const char *username,
                                    const char *password) {
  return service.queueConnect(requestId, username, password);
}

bool queueMachineIntegrationRelist(uint32_t requestId) {
  return service.queueRelist(requestId);
}

bool queueMachineIntegrationRequest(const LineaMicraRequest &request) {
  return service.queue(request);
}

bool selectMachineIntegrationDevice(
    const char *serial, LineaMicraPersistedSettings &settings) {
  return service.selectDiscoveredMachine(serial, settings);
}

void clearMachineIntegrationDiscovery() { service.clearDiscovery(); }

LineaMicraStatus machineIntegrationStatus() { return service.status(); }

LineaMicraCloudCall machineIntegrationCloudCall() { return service.cloudCall(); }
MicraWebSocketStatus machineIntegrationWebSocketStatus(bool includeTraffic) { return service.websocketStatus(includeTraffic); }

HeapLifecycleAggregate machineIntegrationHeapTelemetry() {
  return service.heapTelemetry();
}

LineaMicraDiscoverySnapshot machineIntegrationDiscovery() {
  return service.discovery();
}

}  // namespace openbrewbyweight

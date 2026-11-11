#include "OpenBrewByWeightLaMarzoccoIntegration.h"

#include "OpenBrewByWeightMachineIntegration.h"
#include "OpenBrewByWeightLaMarzoccoCompanion.h"
#include "OpenBrewByWeightLaMarzoccoMachinePower.h"
#include "OpenBrewByWeightLaMarzoccoScalePowerOn.h"
#include "OpenBrewByWeightLaMarzoccoScaleShutdown.h"
#include "OpenBrewByWeightLaMarzoccoService.h"
#include "OpenBrewByWeightPersistedSettings.h"
#include "OpenBrewByWeightScaleWorker.h"

#include <atomic>
#include <cstdint>

void serialTraceCategoryf(openbrewbyweight::LogLevel level,
                          openbrewbyweight::DebugCategory category,
                          const char *fmt, ...);

namespace openbrewbyweight {
namespace {

OpenBrewByWeightLaMarzoccoService service;
LaMarzoccoScaleShutdownTracker scaleShutdown;
LaMarzoccoScalePowerOnTracker scalePowerOn;
LaMarzoccoMachinePowerTracker machinePower;
std::atomic<uint8_t> laMarzoccoOptions{LA_MARZOCCO_DEFAULT_OPTIONS};
std::atomic<uint8_t> laMarzoccoScaleOptions{0};
std::atomic<bool> laMarzoccoAccountConfigured{false};

// A standby command is pointless once the machine is already effectively off.
// Skipping it also keeps the remote disconnect that follows our own scale
// power-off command from re-arming the shutdown cycle. A stale poll cache or
// a retained OFF counts as unknown, not off: the redundant command is
// re-validated anyway. Only our own standby in flight (optimistic OFF) skips.
bool machineEffectivelyOff() {
  const LaMarzoccoStatus status = service.statusPoll();
  const bool trusted = !service.statusPollStale();
  switch (status.assumedPower) {
    case LaMarzoccoAssumedPower::OFF: return trusted;
    case LaMarzoccoAssumedPower::OFF_STALE:
      return trusted && status.quality == LaMarzoccoObservationQuality::OPTIMISTIC;
    case LaMarzoccoAssumedPower::ON:
    case LaMarzoccoAssumedPower::ON_STALE:
    case LaMarzoccoAssumedPower::UNKNOWN: break;
  }
  return false;
}

}  // namespace

bool initializeMachineIntegration() { return service.begin(); }

void publishMachineIntegrationConfig(const PersistedSettings &settings,
                                     uint32_t configGeneration) {
  laMarzoccoOptions.store(settings.laMarzocco.options,
                     std::memory_order_relaxed);
  laMarzoccoScaleOptions.store(settings.laMarzocco.scaleOptions,
                          std::memory_order_relaxed);
  laMarzoccoAccountConfigured.store(settings.laMarzocco.accountConfigured,
                               std::memory_order_relaxed);
  service.publishConfig(settings.laMarzocco, configGeneration);
}

void publishMachineIntegrationNetworkState(bool staConnected, bool apActive,
                                           bool shotActive,
                                           bool scaleConnecting) {
  service.publishNetworkState(staConnected, apActive, shotActive,
                              scaleConnecting);
}

void inhibitMachineIntegrationCloud() { service.inhibitCloud(); }

void requestMachineIntegrationPresetTemperature(PresetTemperature preset,
                                                uint32_t configGeneration) {
  LaMarzoccoRequest request;
  request.configGeneration = configGeneration;
  request.targetDeciC = preset.targetDeciC;
  request.presetId = preset.presetId;
  request.type = LaMarzoccoRequestType::APPLY_TEMPERATURE;
  service.post(request);
}

// Control task only (the finalize path): the cached poll never blocks on the
// service mutex, and the loop refreshes that cache each iteration.
uint16_t machineIntegrationShotTargetDeciC() {
  const LaMarzoccoStatus status = service.statusPoll();
  return status.targetValid ? status.targetDeciC : 0;
}

void serviceMachineIntegrationScaleLink(uint32_t now, bool scaleLinkUp,
                                        uint32_t scaleDisconnectSequence,
                                        uint8_t scaleDisconnectReason,
                                        bool machineBusy) {
  const uint8_t options = laMarzoccoOptions.load(std::memory_order_relaxed);
  const bool accountConfigured =
      laMarzoccoAccountConfigured.load(std::memory_order_relaxed);
  const LaMarzoccoScaleShutdownTracker::Snapshot shutdownSnapshot{
      scaleDisconnectSequence, scaleDisconnectReason, scaleLinkUp,
      machineBusy};
  if (scaleShutdown.service(now, shutdownSnapshot, options,
                            accountConfigured) &&
      !machineEffectivelyOff()) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "La Marzocco scale shutdown: scale powered off, requesting StandBy");
    LaMarzoccoRequest request;
    request.type = LaMarzoccoRequestType::SET_STANDBY;
    service.post(request);
  }
  const LaMarzoccoScalePowerOnTracker::Snapshot powerOnSnapshot{
      scaleLinkUp, scaleDisconnectReason, machineBusy};
  if (scalePowerOn.service(powerOnSnapshot, options, accountConfigured)) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "La Marzocco scale power-on: scale powered on, requesting BrewingMode");
    LaMarzoccoRequest request;
    request.type = LaMarzoccoRequestType::SET_POWER_ON;
    service.post(request);
  }
}

// Control task only, like the other cached status poll readers.
MachinePower machineIntegrationPowerState() {
  switch (service.statusPoll().assumedPower) {
    case LaMarzoccoAssumedPower::ON:
    case LaMarzoccoAssumedPower::ON_STALE: return MachinePower::ON;
    case LaMarzoccoAssumedPower::OFF:
    case LaMarzoccoAssumedPower::OFF_STALE: return MachinePower::OFF;
    case LaMarzoccoAssumedPower::UNKNOWN: break;
  }
  return MachinePower::UNKNOWN;
}

void serviceMachineIntegrationMachinePower(bool scaleLinkUp,
                                           bool scaleSupportsPowerOff,
                                           bool machineBusy) {
  const uint8_t scaleOptions =
      laMarzoccoScaleOptions.load(std::memory_order_relaxed);
  const LaMarzoccoStatus status = service.statusPoll();
  // Refresh the discovery override every loop before any scale power-off
  // early return: eligibility changes (option, identity, observation, or
  // state) clear it here without a separate timer or event.
  const LaMarzoccoScanOverride scanOverride = laMarzoccoScanOverride(status,
                                                           scaleOptions);
  applyLiveBleScanOverride(
      scanOverride == LaMarzoccoScanOverride::AGGRESSIVE
          ? BleScanIntensity::AGGRESSIVE
      : scanOverride == LaMarzoccoScanOverride::RELAXED
          ? BleScanIntensity::RELAXED
          : BleScanIntensity::BALANCED);
  if (!machinePower.service(status, scaleOptions,
                            laMarzoccoAccountConfigured.load(
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

bool queueMachineIntegrationRequest(const LaMarzoccoRequest &request) {
  return service.queue(request);
}

bool selectMachineIntegrationDevice(
    const char *serial, LaMarzoccoPersistedSettings &settings) {
  return service.selectDiscoveredMachine(serial, settings);
}

void clearMachineIntegrationDiscovery() { service.clearDiscovery(); }

LaMarzoccoStatus machineIntegrationStatus() { return service.currentStatus(); }

LaMarzoccoCloudCall machineIntegrationCloudCall() { return service.cloudCall(); }
LaMarzoccoWebSocketStatus machineIntegrationWebSocketStatus(bool includeTraffic) { return service.websocketStatus(includeTraffic); }

HeapLifecycleAggregate machineIntegrationHeapTelemetry() {
  return service.heapTelemetry();
}

LaMarzoccoDiscoverySnapshot machineIntegrationDiscovery() {
  return service.discovery();
}

void machineIntegrationCompanionLink(bool ready, bool lmSwitch) {
  service.companionLink(ready, lmSwitch);
}

void machineIntegrationCompanionPing(bool upstreamAlive) { service.companionPing(upstreamAlive); }

void machineIntegrationCompanionSyncHold(bool on) { service.companionSyncHold(on); }

void machineIntegrationCompanionUpstream(bool subscribed, uint32_t epoch,
                                         uint32_t tokenRenewInS, bool refused, bool policy) {
  service.companionUpstream(subscribed, epoch, tokenRenewInS, refused, policy);
}

void machineIntegrationCompanionCredentials(uint32_t generation, bool ok) {
  service.companionCredentials(generation, ok);
}

LaMarzoccoCompanionIngress machineIntegrationCompanionIngress() { return service.companionIngress(); }

bool machineIntegrationCompanionState(LaMarzoccoCompanionStateKind kind, uint32_t epoch,
                                      uint32_t afterCommand, const cJSON *dashboard,
                                      LaMarzoccoCompanionIngress ingress) {
  return service.companionState(kind, epoch, afterCommand, dashboard, ingress);
}

void machineIntegrationCompanionCommandResult(uint32_t id, bool accepted,
                                              uint16_t httpStatus) {
  service.companionCommandResult(id, accepted, httpStatus);
}

LaMarzoccoCompanionOutbox machineIntegrationCompanionOutbox() { return service.companionOutbox(); }

LaMarzoccoCompanionRouteStatus machineIntegrationCompanionRouteStatus() {
  return service.companionRouteStatus();
}

bool machineIntegrationCompanionCopyCredentials(uint32_t generation, char *email,
                                                size_t emailCapacity, char *password,
                                                size_t passwordCapacity, char *serial,
                                                size_t serialCapacity) {
  return service.companionCopyCredentials(generation, email, emailCapacity, password,
                                          passwordCapacity, serial, serialCapacity);
}

void setMachineIntegrationCompanionNotify(void (*notify)()) { service.setCompanionNotify(notify); }

}  // namespace openbrewbyweight

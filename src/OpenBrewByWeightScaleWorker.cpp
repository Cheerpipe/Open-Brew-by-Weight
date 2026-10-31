#if !defined(OPEN_BREW_BY_WEIGHT_SCALE_WORKER_IN_ORCHESTRATOR)

#include "OpenBrewByWeightScaleWorker.h"

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include "tests/open_brew_by_weight_host_stubs.h"
#else
#include "OpenBrewByWeightBleRuntime.h"
#include "OpenBrewByWeightWatchdog.h"
#include "OpenBrewByWeightHardwareTimer.h"
#endif

#include "OpenBrewByWeightAlert.h"
#include "OpenBrewByWeightRfCoex.h"
#include "OpenBrewByWeightSafety.h"
#include "OpenBrewByWeightScheduling.h"

#include <atomic>
#include <cmath>
#include <stdio.h>
#include <string.h>

#endif  // !OPEN_BREW_BY_WEIGHT_SCALE_WORKER_IN_ORCHESTRATOR

#include "OpenBrewByWeightPowerManagement.h"
#include "OpenBrewByWeightScaleProfiler.h"
#include "OpenBrewByWeightOutboundAdmission.h"
#include "OpenBrewByWeightTaskProfiler.h"
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include <esp_bt.h>
#endif

#if !defined(OPEN_BREW_BY_WEIGHT_SCALE_WORKER_IN_ORCHESTRATOR)
// Orchestrator symbols live in the global namespace (openBrewByWeight.cpp).
uint32_t elapsedMs(uint32_t sinceMs);
extern openbrewbyweight::TaskProfiler taskProfiler;
void addDebugEvent(openbrewbyweight::DebugCategory category, openbrewbyweight::DebugCode code,
                   int32_t argument1 = 0, int32_t argument2 = 0);
void addDebugEventDetail(openbrewbyweight::DebugCategory category,
                         openbrewbyweight::DebugCode code, int32_t argument1,
                         int32_t argument2, const char *detail);
void serialTraceCategory(openbrewbyweight::LogLevel level,
                         openbrewbyweight::DebugCategory category,
                         const char *message);
void serialTraceCategoryf(openbrewbyweight::LogLevel level,
                          openbrewbyweight::DebugCategory category,
                          const char *fmt, ...);
void logEmit(openbrewbyweight::LogLevel level, openbrewbyweight::DebugCategory category,
             openbrewbyweight::DebugCode code, int32_t argument1 = 0,
             int32_t argument2 = 0);
void feedOrTripCurrentTaskWatchdog();
bool feedCurrentTaskWatchdog();
void reportTaskWatchdogFault();
openbrewbyweight::RelaySafetySnapshot getRelaySafetySnapshot();
#endif

namespace openbrewbyweight {

constexpr bool DEBUG = false;

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
void reportNimbleRuntimeHealth(bool force) {
  static bool reported = false;
  static OpenBrewByWeightBleRuntimeState previousState =
      OpenBrewByWeightBleRuntimeState::Stopped;
  static int32_t previousError = 0;
  static int32_t previousResetReason = 0;
  static uint32_t previousSyncGeneration = 0;
  static uint32_t previousStaleCallbacks = 0;
  static uint32_t previousCleanupCount = 0;
  static uint32_t previousTotalDrops = 0;
  static uint8_t previousClientState = 0xff;
  OpenBrewByWeightBleHealth health = openBrewByWeightBleRuntimeHealth(false);
  const ScaleBleBackendHealth client = scale.backendHealth();
  const uint32_t totalDrops = client.criticalEventDrops +
                              client.controlEventDrops + client.rxDrops;
  if (!force && reported && health.state == previousState &&
      health.lastError == previousError &&
      health.lastResetReason == previousResetReason &&
      health.syncGeneration == previousSyncGeneration &&
      client.staleCallbacks == previousStaleCallbacks &&
      client.cleanupCount == previousCleanupCount &&
      totalDrops == previousTotalDrops && client.state == previousClientState) {
    return;
  }
  health = openBrewByWeightBleRuntimeHealth();  // heap walk only when reporting
  reported = true;
  previousState = health.state;
  previousError = health.lastError;
  previousResetReason = health.lastResetReason;
  previousSyncGeneration = health.syncGeneration;
  previousStaleCallbacks = client.staleCallbacks;
  previousCleanupCount = client.cleanupCount;
  previousTotalDrops = totalDrops;
  previousClientState = client.state;
  serialTraceCategoryf(
      LogLevel::DEBUG, DebugCategory::BLE,
      "NimBLE runtime state=%u raw=%ld linkRaw=%ld reset=%ld sync=%lu resets=%lu "
      "hostStackMin=%lu internal=%lu/%lu/%lu psram=%lu/%lu/%lu",
      static_cast<unsigned>(health.state), static_cast<long>(health.lastError),
      static_cast<long>(scale.lastBackendStatus()),
      static_cast<long>(health.lastResetReason),
      static_cast<unsigned long>(health.syncGeneration),
      static_cast<unsigned long>(health.resetCount),
      static_cast<unsigned long>(health.hostTaskStackHighWaterBytes),
      static_cast<unsigned long>(health.internalFreeBytes),
      static_cast<unsigned long>(health.internalMinimumFreeBytes),
      static_cast<unsigned long>(health.internalLargestBlockBytes),
      static_cast<unsigned long>(health.psramFreeBytes),
      static_cast<unsigned long>(health.psramMinimumFreeBytes),
      static_cast<unsigned long>(health.psramLargestBlockBytes));
  serialTraceCategoryf(
      LogLevel::DEBUG, DebugCategory::BLE,
      "NimBLE client state=%u age=%lu gen/op=%lu/%lu adv=%lu compatible=%lu "
      "discarded=%lu malformed=%lu cache=%u hits=%lu scans=%lu/%lu/%lu "
      "connect=%lu/%lu gattFail=%lu/%lu writeFail=%lu",
      static_cast<unsigned>(client.state),
      static_cast<unsigned long>(client.stateAgeMs),
      static_cast<unsigned long>(client.generation),
      static_cast<unsigned long>(client.operationId),
      static_cast<unsigned long>(client.advertisementsSeen),
      static_cast<unsigned long>(client.compatibleAdvertisements),
      static_cast<unsigned long>(client.discardedAdvertisements),
      static_cast<unsigned long>(client.malformedAdvertisements),
      static_cast<unsigned>(client.negativeCacheEntries),
      static_cast<unsigned long>(client.negativeCacheHits),
      static_cast<unsigned long>(client.scanStarts),
      static_cast<unsigned long>(client.scanCancels),
      static_cast<unsigned long>(client.scanRestarts),
      static_cast<unsigned long>(client.connectAttempts),
      static_cast<unsigned long>(client.connectionFailures),
      static_cast<unsigned long>(client.discoveryFailures),
      static_cast<unsigned long>(client.subscriptionFailures),
      static_cast<unsigned long>(client.writeFailures));
  serialTraceCategoryf(
      LogLevel::DEBUG, DebugCategory::BLE,
      "NimBLE resilience stale=%lu cleanup=%lu duplicate=%lu backoff=%lu/%u "
      "queueHwm=%u/%u/%u drops=%lu/%lu/%lu mbuf=%lu latency=%lu/%lu/%lu/%lu",
      static_cast<unsigned long>(client.staleCallbacks),
      static_cast<unsigned long>(client.cleanupCount),
      static_cast<unsigned long>(client.duplicateCleanups),
      static_cast<unsigned long>(client.backoffCount),
      static_cast<unsigned>(client.backoffFailures),
      static_cast<unsigned>(client.criticalEventHighWater),
      static_cast<unsigned>(client.controlEventHighWater),
      static_cast<unsigned>(client.rxHighWater),
      static_cast<unsigned long>(client.criticalEventDrops),
      static_cast<unsigned long>(client.controlEventDrops),
      static_cast<unsigned long>(client.rxDrops),
      static_cast<unsigned long>(client.mbufFailures),
      static_cast<unsigned long>(client.lastAdvertisementToConnectMs),
      static_cast<unsigned long>(client.lastAdvertisementToReadyMs),
      static_cast<unsigned long>(client.lastAdvertisementToFirstWeightMs),
      static_cast<unsigned long>(client.lastReadyToFirstWeightMs));
}
#endif

EspressoScaleBLE scale(DEBUG);
static ScaleWorkerBridgeCallbacks scaleWorkerBridge;
static std::atomic<TaskHandle_t> scaleWorkerTaskHandle{nullptr};
static std::atomic<bool> scaleSoftApActive{false};
static TaskMutex scaleWorkerTaskHandleMutex;
QueueHandle_t scaleCommandQueue = nullptr;
QueueHandle_t scaleEventQueue = nullptr;
portMUX_TYPE scaleLinkMux = portMUX_INITIALIZER_UNLOCKED;
TaskMutex idleScaleTareMux;
IdleTareStatus workerIdleTare;
uint32_t activeScaleCommandCycle = 0; // Protected by idleScaleTareMux.
ScaleTareSample approvedTareSample; // Protected by idleScaleTareMux.
TaskMutex scalePreferredMacMux;
portMUX_TYPE scaleBeepMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE scaleDebugMux = portMUX_INITIALIZER_UNLOCKED;
TaskMutex scaleCriticalEventMux;
static ScaleEvent shotTareResult;

ScaleEvent copyScaleShotTareResult() {
  const TaskLockGuard lock(scaleCriticalEventMux);
  return shotTareResult;
}

ScaleLinkState scaleLinkState = ScaleLinkState::DISCONNECTED;
bool scaleConnecting = false;
bool pendingScaleConnectIdleSync = false;
uint32_t scaleDisconnectSequence = 0;
uint32_t scaleConnectionGeneration = 0;
uint32_t scalePacketSequence = 0;
uint32_t scalePacketGaps = 0;
static uint32_t scaleMaxPacketGapMs = 0;
static std::atomic<bool> scaleGapMaxResetRequested{false};
uint32_t lastScalePacketGapLogMs = 0;
uint32_t lastScaleWeightAtMs = 0;
uint32_t scaleWeightUpdateIntervalMs = 0;
uint32_t scaleRejectedPackets = 0;
uint32_t scaleReconnects = 0;
uint8_t scaleLastDisconnectReason = 0;
ScaleBleDiagnostics scaleBleDiagnostics = {};
bool scaleTimerValid = false;
uint32_t scaleTimerMs = 0;
uint32_t scaleTimerAgeMs = 0;
bool scaleFlowRateValid = false;
float scaleFlowRateGps = 0.0f;
uint32_t scaleFlowRateAtMs = 0;
static char scaleProtocolName[20] = "none";
char scaleConnectedMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
static char scaleShotName[SCALE_FRIENDLY_NAME_MAX_LEN + 1] = {};
ScaleModel scaleLinkModel = ScaleModel::Unknown;
ScaleFeatureSet scaleLinkFeatures = {};
bool scaleLinkRssiValid = false;
int8_t scaleLinkRssi = 0;
uint32_t lastScaleLinkRssiSampleMs = 0;
char scalePreferredMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
char scalePreferredName[PREFERRED_SCALE_NAME_CAPACITY] = {};
ScaleHistoryEntry scaleHistory[SCALE_HISTORY_CAPACITY] = {};
uint32_t scaleHistorySeq = 0;
bool scalePreferredMacDirty = false;
uint32_t scaleDiscoveryPausedUntilMs = 0;
uint32_t scalePreferredDirectedResetGeneration = 0;
uint32_t scalePreferredAppliedResetGeneration = 0;
uint8_t scalePreferredResetReasonBits = 0;
uint32_t scaleWorkerProgressAtMs = 0;
static std::atomic<uint32_t> scaleEventsDropped{0};
static std::atomic<uint32_t> scaleWorkerStackMinBytes{UINT32_MAX};
static std::atomic<uint32_t> scaleWorkerMaxGapMs{0};
static std::atomic<uint32_t> scaleWorkerDeadlineMisses{0};
static std::atomic<uint32_t> scaleWorkerMaxExecutionUs{0};
ScaleEvent scaleCriticalEvent;
bool scaleCriticalEventPending = false;
ScaleEvent scaleReferenceChangedEvent;
bool scaleReferenceChangedEventPending = false;
ScaleEvent scaleTimerStartEvent;
bool scaleTimerStartEventPending = false;
ScaleEvent scaleWeightEvents[SCALE_WEIGHT_EVENT_CAPACITY];
uint8_t scaleWeightEventHead = 0;
uint8_t scaleWeightEventCount = 0;
uint32_t scaleWeightEventDrops = 0;
uint32_t profileLastWeightFifoDrops = 0;
std::atomic<bool> scaleWeightEventPending{false};
bool scaleBeepPending = false;
uint32_t scaleBeepCycleId = 0;
uint32_t scaleBeepConnectionGeneration = 0;
bool scaleDebugPending = false;
BookooDebugAction scaleDebugAction = BookooDebugAction::START;
uint8_t scaleDebugBeepLevel = 0;
uint32_t scaleDebugConnectionGeneration = 0;
bool scalePaddleReturnReminderBeepPending = false;
uint32_t scalePaddleReturnReminderBeepConnectionGeneration = 0;
bool scaleCompletionBeepPending = false;
uint32_t scaleCompletionBeepConnectionGeneration = 0;
bool scalePowerOffPending = false;
uint32_t scalePowerOffConnectionGeneration = 0;
uint32_t scalePowerOffBlockedGeneration = 0;
uint32_t scalePowerOffSentAtMs = 0;
bool scalePowerOffSent = false;
bool scalePowerOffBlocksGeneration(uint32_t generation);
void clearScalePowerOffLifecycle(uint32_t preserveGeneration = 0);
uint16_t scaleScanAppliedInterval = 0;
uint16_t scaleScanAppliedWindow = 0;
uint32_t scaleHuntRfUntilMs = 0;
bool scaleLoggedGattConnecting = false;
uint8_t scaleLoggedGattConnectAttempts = 0;
bool scaleDiscoveryDirected = false;
std::atomic<uint8_t> liveBleScanIntensityRaw{
    static_cast<uint8_t>(BLE_SCAN_FACTORY_INTENSITY)};
// Admin BLE master switch: 0 keeps the scale client idle (no scans, no
// links), indistinguishable from having no scale for the rest of the FW.
std::atomic<uint8_t> liveBleEnabledRaw{1};
// Machine-integration override of the saved discovery duty; BALANCED means
// no override. Boot zero-init selects the saved intensity.
std::atomic<uint8_t> liveBleScanOverrideRaw{
    static_cast<uint8_t>(BleScanIntensity::BALANCED)};
bool bookooConnectVolumePending = false;
static std::atomic<bool> bleStackReady{false};
static std::atomic<bool> scaleWorkerStartupFinished{false};

namespace {

constexpr uint32_t kPolicyControlReadyBit = 1U << 0;
constexpr uint32_t kPolicySoundAlertsBit = 1U << 1;
constexpr uint32_t kPolicyBookooMuteBit = 1U << 2;
constexpr uint32_t kPolicyAlertChannelShift = 3;
constexpr uint32_t kPolicyAlertChannelMask = 0x3U;
constexpr uint32_t kPolicyBookooLevelShift = 8;
constexpr uint32_t kPolicyBookooLevelMask = 0xffU;
constexpr uint32_t kPolicyMacCacheModeShift = 16;
constexpr uint32_t kPolicyMacCacheModeMask = 0x3U;

// Safe pre-publication default: do not emit sound, use ONLY discovery.
static std::atomic<uint32_t> scaleWorkerPolicyBits{
    static_cast<uint32_t>(ScaleMacCacheMode::ONLY)
    << kPolicyMacCacheModeShift};

struct ScaleWorkerPolicySnapshot {
  bool controlReady = false;
  bool soundAlertsEnabled = false;
  bool bookooMuteOnBuzzerOnly = true;
  AlertOutputChannel alertOutputChannel = AlertOutputChannel::SCALE_ONLY;
  uint8_t bookooConnectBeepLevel = 0;
  ScaleMacCacheMode macCacheMode = ScaleMacCacheMode::ONLY;
};

ScaleWorkerPolicySnapshot currentScaleWorkerPolicy() {
  const uint32_t bits = scaleWorkerPolicyBits.load(std::memory_order_acquire);
  ScaleWorkerPolicySnapshot policy;
  policy.controlReady = (bits & kPolicyControlReadyBit) != 0;
  policy.soundAlertsEnabled = (bits & kPolicySoundAlertsBit) != 0;
  policy.bookooMuteOnBuzzerOnly = (bits & kPolicyBookooMuteBit) != 0;
  policy.alertOutputChannel = static_cast<AlertOutputChannel>(
      (bits >> kPolicyAlertChannelShift) & kPolicyAlertChannelMask);
  policy.bookooConnectBeepLevel = static_cast<uint8_t>(
      (bits >> kPolicyBookooLevelShift) & kPolicyBookooLevelMask);
  policy.macCacheMode = static_cast<ScaleMacCacheMode>(
      (bits >> kPolicyMacCacheModeShift) & kPolicyMacCacheModeMask);
  return policy;
}

}  // namespace

void publishScaleWorkerPolicy(const RuntimeConfig &config, bool controlReady) {
  const AlertOutputChannel channel =
      effectiveAlertOutputChannel(config.alertOutputChannel);
  const ScaleMacCacheMode cacheMode =
      validScaleMacCacheMode(config.scaleMacCacheMode)
          ? static_cast<ScaleMacCacheMode>(config.scaleMacCacheMode)
          : ScaleMacCacheMode::ONLY;
  uint32_t bits = controlReady ? kPolicyControlReadyBit : 0U;
  if (!config.soundAlertsMuted) {
    bits |= kPolicySoundAlertsBit;
  }
  if (config.bookooMuteOnBuzzerOnly) {
    bits |= kPolicyBookooMuteBit;
  }
  bits |= static_cast<uint32_t>(channel) << kPolicyAlertChannelShift;
  bits |= static_cast<uint32_t>(config.bookooConnectBeepLevel)
          << kPolicyBookooLevelShift;
  bits |= static_cast<uint32_t>(cacheMode) << kPolicyMacCacheModeShift;
  scaleWorkerPolicyBits.store(bits, std::memory_order_release);
  wakeScaleWorker();
}

bool scaleWorkerReady() {
  return scaleWorkerTaskHandle.load(std::memory_order_acquire) != nullptr &&
         bleStackReady.load(std::memory_order_acquire);
}

void wakeScaleWorker() {
  TaskLockGuard lock(scaleWorkerTaskHandleMutex);
  TaskHandle_t worker =
      scaleWorkerTaskHandle.load(std::memory_order_acquire);
  if (worker != nullptr) {
    (void)xTaskNotifyGive(worker);
  }
}

// NimBLE host task, once per queued weight frame. It must not block, so it
// reads the handle without scaleWorkerTaskHandleMutex: the BLE runtime stops
// before the handle clears, so no late frame reaches a gone task.
void wakeScaleWorkerForFrame() {
  TaskHandle_t worker = scaleWorkerTaskHandle.load(std::memory_order_acquire);
  if (worker != nullptr) (void)xTaskNotifyGive(worker);
}

// Worker task, after every connection and accepted update. A peer request the
// clamp overrode is a scale that wants a longer interval than the controller
// allows: rare, and worth keeping in the persistent audit log.
void noteScaleLinkParams(uint16_t intervalUnits, uint16_t peerIntervalUnits) {
  if (peerIntervalUnits == 0) return;
  addDebugEvent(DebugCategory::BLE, DebugCode::BLE_LINK_PARAMS_OVERRIDDEN,
                static_cast<int32_t>(peerIntervalUnits) * 125 / 100,
                static_cast<int32_t>(intervalUnits) * 125 / 100);
}

void syncScaleSoftApRadio(bool apActive) {
  const bool previous =
      scaleSoftApActive.exchange(apActive, std::memory_order_acq_rel);
  if (previous != apActive) {
    wakeScaleWorker();
  }
}

uint32_t scaleWorkerDroppedEventCount() {
  return scaleEventsDropped.load(std::memory_order_relaxed);
}

uint32_t scaleWorkerStackMinBytesValue() {
  return scaleWorkerStackMinBytes.load(std::memory_order_relaxed);
}

uint32_t scaleWorkerMaxGapMsValue() {
  return scaleWorkerMaxGapMs.load(std::memory_order_relaxed);
}

uint32_t scaleWorkerDeadlineMissCount() {
  return scaleWorkerDeadlineMisses.load(std::memory_order_relaxed);
}

uint32_t scaleWorkerMaxExecutionUsValue() {
  return scaleWorkerMaxExecutionUs.load(std::memory_order_relaxed);
}

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
void setScaleWorkerBleReadyForHost(bool ready) {
  bleStackReady.store(ready, std::memory_order_release);
}

void setScaleWorkerTaskPresentForHost(bool present) {
  scaleWorkerTaskHandle.store(
      present ? reinterpret_cast<TaskHandle_t>(1) : nullptr,
      std::memory_order_release);
}

void setScaleWorkerStackMinBytesForHost(uint32_t bytes) {
  scaleWorkerStackMinBytes.store(bytes, std::memory_order_relaxed);
}

void resetScaleWorkerMetricsForHost() {
  shotTareResult = ScaleEvent{};
  scaleWorkerTaskHandle.store(nullptr, std::memory_order_release);
  bleStackReady.store(false, std::memory_order_relaxed);
  scaleWorkerStartupFinished.store(false, std::memory_order_relaxed);
  scaleEventsDropped.store(0, std::memory_order_relaxed);
  scaleWorkerStackMinBytes.store(UINT32_MAX, std::memory_order_relaxed);
  scaleWorkerMaxGapMs.store(0, std::memory_order_relaxed);
  scaleWorkerDeadlineMisses.store(0, std::memory_order_relaxed);
  scaleWorkerMaxExecutionUs.store(0, std::memory_order_relaxed);
}
#endif

static uint32_t scaleCommandDropCount = 0;

void scaleWorkerLoadPreferred(const char *mac, const char *name,
                              const ScaleHistoryEntry *history) {
  scalePreferredMacMux.lock();
  if (mac != nullptr && validPreferredScaleMac(mac)) {
    copyCString(scalePreferredMac, sizeof(scalePreferredMac), mac);
    canonicalizePreferredScaleMac(scalePreferredMac, sizeof(scalePreferredMac));
  }
  if (name != nullptr && validPreferredScaleName(name)) {
    copyCString(scalePreferredName, sizeof(scalePreferredName), name);
  }
  if (history != nullptr) {
    memcpy(scaleHistory, history, sizeof(scaleHistory));
  }
  scaleHistorySeq = 0;
  for (auto & i : scaleHistory) {
    clearScaleHistorySessionMarker(i);
    if (i.mac[0] != '\0') {
      canonicalizePreferredScaleMac(i.mac,
                                    sizeof(i.mac));
    }
    if (i.lastSeenSeq > scaleHistorySeq) {
      scaleHistorySeq = i.lastSeenSeq;
    }
  }
  seedScaleHistoryFromPreferred(scaleHistory, scaleHistorySeq,
                                scalePreferredMac, scalePreferredName);
  scalePreferredMacMux.unlock();
}

bool scaleWorkerCopyPreferredIfDirty(char *mac, char *name,
                                     ScaleHistoryEntry *history) {
  bool dirty = false;
  scalePreferredMacMux.lock();
  dirty = scalePreferredMacDirty;
  if (dirty) {
    if (mac != nullptr) {
      memcpy(mac, scalePreferredMac, sizeof(scalePreferredMac));
    }
    if (name != nullptr) {
      memcpy(name, scalePreferredName, sizeof(scalePreferredName));
    }
    if (history != nullptr) {
      memcpy(history, scaleHistory, sizeof(scaleHistory));
      for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
        clearScaleHistorySessionMarker(history[i]);
      }
    }
  }
  scalePreferredMacMux.unlock();
  return dirty;
}

bool scaleWorkerClearPreferredDirty(const char *mac, const char *name,
                                   const ScaleHistoryEntry *history) {
  const TaskLockGuard lock(scalePreferredMacMux);
  const bool matches = strncmp(mac, scalePreferredMac, PREFERRED_SCALE_MAC_CAPACITY) == 0 &&
      strncmp(name, scalePreferredName, PREFERRED_SCALE_NAME_CAPACITY) == 0 &&
      scaleHistoryIdentityEqual(history, scaleHistory);
  if (matches) scalePreferredMacDirty = false;
  return matches;
}

bool scaleWorkerTakeConnectedEdge() {
  bool edge = false;
  portENTER_CRITICAL(&scaleLinkMux);
  edge = pendingScaleConnectIdleSync;
  pendingScaleConnectIdleSync = false;
  portEXIT_CRITICAL(&scaleLinkMux);
  return edge;
}

void cancelScaleCompletionBeepMailbox() {
  portENTER_CRITICAL(&scaleBeepMux);
  scaleCompletionBeepPending = false;
  scaleCompletionBeepConnectionGeneration = 0;
  portEXIT_CRITICAL(&scaleBeepMux);
}

void cancelOperationalScaleBeeps() {
  cancelScaleCompletionBeepMailbox();
  cancelScalePaddleReturnReminderBeep();
  portENTER_CRITICAL(&scaleBeepMux);
  scaleBeepPending = false;
  scaleBeepCycleId = 0;
  scaleBeepConnectionGeneration = 0;
  portEXIT_CRITICAL(&scaleBeepMux);
}

uint32_t scaleConnectionGenerationNow() {
  portENTER_CRITICAL(&scaleLinkMux);
  const uint32_t generation = scaleConnectionGeneration;
  portEXIT_CRITICAL(&scaleLinkMux);
  return generation;
}

ScaleLinkSnapshot getScaleLinkSnapshot() {
  ScaleLinkSnapshot snapshot = {};
  portENTER_CRITICAL(&scaleLinkMux);
  snapshot.state = scaleLinkState;
  snapshot.connecting = scaleConnecting;
  snapshot.disconnectSequence = scaleDisconnectSequence;
  snapshot.connectionGeneration = scaleConnectionGeneration;
  snapshot.packetSequence = scalePacketSequence;
  snapshot.packetGaps = scalePacketGaps;
  snapshot.maxPacketGapMs = scaleMaxPacketGapMs;
  snapshot.weightUpdateIntervalMs = scaleWeightUpdateIntervalMs;
  snapshot.rejectedPackets = scaleRejectedPackets;
  snapshot.reconnects = scaleReconnects;
  snapshot.lastDisconnectReason = scaleLastDisconnectReason;
  snapshot.bleDiagnostics = scaleBleDiagnostics;
  snapshot.workerProgressAtMs = scaleWorkerProgressAtMs;
  snapshot.timerValid = scaleTimerValid;
  snapshot.timerMs = scaleTimerMs;
  snapshot.timerAgeMs = scaleTimerAgeMs;
  snapshot.flowRateValid = scaleFlowRateValid;
  snapshot.flowRateGps = scaleFlowRateGps;
  snapshot.flowRateAtMs = scaleFlowRateAtMs;
  memcpy(snapshot.protocolName, scaleProtocolName, sizeof(snapshot.protocolName));
  memcpy(snapshot.connectedMac, scaleConnectedMac, sizeof(snapshot.connectedMac));
  memcpy(snapshot.shotScaleName, scaleShotName, sizeof(snapshot.shotScaleName));
  snapshot.model = scaleLinkModel;
  snapshot.features = scaleLinkFeatures;
  snapshot.rssiValid = scaleLinkRssiValid;
  snapshot.rssi = scaleLinkRssi;
  portEXIT_CRITICAL(&scaleLinkMux);
  return snapshot;
}

bool scaleLinkAvailable(const ScaleLinkSnapshot &snapshot) {
  return snapshot.state == ScaleLinkState::CONNECTED &&
         elapsedMs(snapshot.workerProgressAtMs) <= SCALE_WORKER_STALE_MS;
}

void setScaleLinkState(ScaleLinkState state) {
  const uint32_t progressAtMs = millis();
  ScaleLinkState previous;
  uint32_t disconnectedGeneration = 0;
  uint32_t connectedGeneration = 0;
  portENTER_CRITICAL(&scaleLinkMux);
  previous = scaleLinkState;
  if (scaleLinkState == ScaleLinkState::CONNECTED &&
      state == ScaleLinkState::DISCONNECTED) {
    disconnectedGeneration = scaleConnectionGeneration;
    ++scaleDisconnectSequence;
    lastScaleWeightAtMs = 0;
    scaleWeightUpdateIntervalMs = 0;
  }
  if (scaleLinkState != ScaleLinkState::CONNECTED &&
      state == ScaleLinkState::CONNECTED) {
    ++scaleConnectionGeneration;
    if (scaleConnectionGeneration == 0) {
      scaleConnectionGeneration = 1;
    }
    pendingScaleConnectIdleSync = true;
    connectedGeneration = scaleConnectionGeneration;
  }
  scaleLinkState = state;
  outboundScaleConnected.store(state == ScaleLinkState::CONNECTED,
                               std::memory_order_release);
  if (state == ScaleLinkState::CONNECTED) {
    scaleConnecting = false;
  }
  scaleWorkerProgressAtMs = progressAtMs;
  if (state != ScaleLinkState::CONNECTED) {
    scaleTimerValid = false;
    scaleTimerMs = 0;
    scaleTimerAgeMs = 0;
    scaleFlowRateValid = false;
    scaleFlowRateGps = 0.0f;
    scaleFlowRateAtMs = 0;
    scaleLinkFeatures = scaleFeatureSetNone();
    scaleLinkModel = ScaleModel::Unknown;
    scaleLinkRssiValid = false;
    scaleLinkRssi = 0;
    lastScaleLinkRssiSampleMs = 0;
  }
  portEXIT_CRITICAL(&scaleLinkMux);
  if (disconnectedGeneration != 0 &&
      scalePowerOffBlocksGeneration(disconnectedGeneration)) {
    clearScalePowerOffLifecycle();
  } else if (previous != ScaleLinkState::CONNECTED &&
             state == ScaleLinkState::CONNECTED) {
    clearScalePowerOffLifecycle(connectedGeneration);
  }
  if (previous != state) {
    scaleProfileNoteEvent(
        state == ScaleLinkState::CONNECTED
            ? ScaleProfileEvent::SCALE_CONNECTED
            : ScaleProfileEvent::SCALE_DISCONNECTED,
        progressAtMs,
        state == ScaleLinkState::CONNECTED ? connectedGeneration
                                           : disconnectedGeneration,
        0, NAN,
        state == ScaleLinkState::CONNECTED ? 0 : scaleLastDisconnectReason, 0);
    addDebugEvent(DebugCategory::SCALE,
                  state == ScaleLinkState::CONNECTED
                      ? DebugCode::SCALE_CONNECTED
                      : DebugCode::SCALE_DISCONNECTED,
                  state == ScaleLinkState::CONNECTED
                      ? 0
                      : static_cast<int32_t>(scaleLastDisconnectReason));
  }
}

void requestScaleGapMaxReset() {
  scaleGapMaxResetRequested.store(true, std::memory_order_release);
  wakeScaleWorker();
}

void markScaleWorkerProgress() {
  const uint32_t progressAtMs = millis();
  portENTER_CRITICAL(&scaleLinkMux);
  if (scaleGapMaxResetRequested.exchange(false, std::memory_order_acquire)) {
    scaleMaxPacketGapMs = 0;
  }
  scaleWorkerProgressAtMs = progressAtMs;
  portEXIT_CRITICAL(&scaleLinkMux);
}

bool scalePowerOffBlocksGeneration(uint32_t generation) {
  portENTER_CRITICAL(&scaleBeepMux);
  const bool blocked = generation != 0 &&
      scalePowerOffBlockedGeneration == generation;
  portEXIT_CRITICAL(&scaleBeepMux);
  return blocked;
}

void clearScalePowerOffLifecycle(uint32_t preserveGeneration) {
  portENTER_CRITICAL(&scaleBeepMux);
  if (preserveGeneration == 0 ||
      scalePowerOffBlockedGeneration != preserveGeneration) {
    scalePowerOffPending = false;
    scalePowerOffConnectionGeneration = 0;
    scalePowerOffBlockedGeneration = 0;
    scalePowerOffSentAtMs = 0;
    scalePowerOffSent = false;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
}

uint32_t scaleWorkerTickDelayMs() {
  if (scale.isConnecting()) {
    return 1;
  }
  if (scale.isLinkUp()) return SCALE_WEIGHT_POLL_INTERVAL_MS;
  if (scaleCommandQueue != nullptr &&
      uxQueueMessagesWaiting(scaleCommandQueue) > 0) {
    return 1;
  }
  return SCALE_WORKER_NO_SCALE_DELAY_MS;
}

IdleTareStatus idleScaleTareStatus() {
  const TaskLockGuard lock(idleScaleTareMux);
  return workerIdleTare;
}

void approveScaleTareSample(const ScaleTareSample &sample) {
  const TaskLockGuard lock(idleScaleTareMux);
  approvedTareSample = sample;
}

bool cancelIdleScaleTare(uint32_t requestId, IdleTareStatus *released) {
  idleScaleTareMux.lock();
  const bool writing = workerIdleTare.requestId == requestId &&
                       workerIdleTare.phase == IdleTarePhase::WRITING;
  if (!writing && workerIdleTare.requestId == requestId) {
    if (released != nullptr) *released = workerIdleTare;
    workerIdleTare = IdleTareStatus{};
  }
  idleScaleTareMux.unlock();
  return !writing;
}

void approveIdleScaleTareSample(uint32_t requestId, uint32_t packetSequence) {
  idleScaleTareMux.lock();
  if (workerIdleTare.requestId == requestId &&
      workerIdleTare.phase == IdleTarePhase::QUEUED) {
    workerIdleTare.approvedPacketSequence = packetSequence;
  }
  idleScaleTareMux.unlock();
}

bool claimIdleScaleTare(uint32_t requestId, uint32_t expectedPacketSequence,
                        uint32_t captureBoundary) {
  idleScaleTareMux.lock();
  const bool claimed = workerIdleTare.requestId == requestId &&
                       workerIdleTare.phase == IdleTarePhase::QUEUED &&
                       (expectedPacketSequence == 0 ||
                        workerIdleTare.approvedPacketSequence == expectedPacketSequence);
  if (claimed) {
    workerIdleTare.phase = IdleTarePhase::WRITING;
    workerIdleTare.startedAtMs = millis();
    workerIdleTare.captureBoundary = captureBoundary;
  }
  const uint32_t startedAtMs = workerIdleTare.startedAtMs;
  const uint32_t generation = workerIdleTare.connectionGeneration;
  idleScaleTareMux.unlock();
  if (claimed) {
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_PHASE, startedAtMs,
                          generation, captureBoundary,
                          NAN, requestId, static_cast<uint32_t>(IdleTarePhase::WRITING));
  }
  return claimed;
}

void finishIdleScaleTare(uint32_t requestId, bool succeeded,
                         IdleTareReason failureReason = IdleTareReason::WRITE_FAILED,
                         float preTareWeightG = NAN) {
  idleScaleTareMux.lock();
  const bool matched = workerIdleTare.requestId == requestId;
  if (matched) {
    workerIdleTare.phase = succeeded ? IdleTarePhase::SUCCEEDED
                                    : IdleTarePhase::FAILED;
    workerIdleTare.writtenAtMs = millis();
    workerIdleTare.preTareWeightG = preTareWeightG;
    workerIdleTare.reason = succeeded ? IdleTareReason::NONE : failureReason;
  }
  const uint32_t writtenAtMs = workerIdleTare.writtenAtMs;
  const uint32_t generation = workerIdleTare.connectionGeneration;
  const uint32_t boundary = workerIdleTare.captureBoundary;
  idleScaleTareMux.unlock();
  if (matched) {
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_PHASE, writtenAtMs,
                          generation, boundary,
                          preTareWeightG, requestId,
                          static_cast<uint32_t>(succeeded ? IdleTarePhase::SUCCEEDED : IdleTarePhase::FAILED) |
                              (static_cast<uint32_t>(succeeded ? IdleTareReason::NONE : failureReason) << 8));
  }
}

bool enqueueScaleCommand(const ScaleCommand &command, bool toFront) {
  if (scaleCommandQueue == nullptr) {
    return false;
  }

  ScaleCommand stamped = command;
  if (stamped.connectionGeneration == 0) {
    stamped.connectionGeneration = scaleConnectionGenerationNow();
  }
  if (scalePowerOffBlocksGeneration(stamped.connectionGeneration)) {
    return false;
  }
  if (stamped.idleTareRequestId != 0) {
    idleScaleTareMux.lock();
    const bool available = workerIdleTare.phase == IdleTarePhase::NONE;
    if (available) {
      workerIdleTare = IdleTareStatus{};
      workerIdleTare.requestId = stamped.idleTareRequestId;
      workerIdleTare.connectionGeneration = stamped.connectionGeneration;
      workerIdleTare.approvedPacketSequence = stamped.qualifiedPacketSequence;
      workerIdleTare.phase = IdleTarePhase::QUEUED;
    }
    idleScaleTareMux.unlock();
    if (!available) return false;
    scaleProfileNoteEvent(ScaleProfileEvent::TARE_PHASE, millis(),
                          stamped.connectionGeneration, 0, NAN, stamped.idleTareRequestId,
                          static_cast<uint32_t>(IdleTarePhase::QUEUED));
  }
  BaseType_t queued = pdFALSE;
  if (toFront) {
    queued = xQueueSendToFront(scaleCommandQueue, &stamped, 0);
  } else {
    queued = xQueueSend(scaleCommandQueue, &stamped, 0);
  }
  if (queued == pdTRUE) {
    wakeScaleWorker();
    return true;
  }
  ++scaleCommandDropCount;
  if (stamped.idleTareRequestId != 0) {
    cancelIdleScaleTare(stamped.idleTareRequestId);
  }
  serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                      "Scale command queue full");
  return false;
}

bool publishScaleEvent(const ScaleEvent &event, bool critical) {
  ScaleEvent stamped = event;
  if (event.type == ScaleEventType::WEIGHT) {
    uint32_t streamGapMs = 0;
    portENTER_CRITICAL(&scaleLinkMux);
    if (stamped.connectionGeneration == 0) {
      stamped.connectionGeneration = scaleConnectionGeneration;
    }
    stamped.disconnectSequence = scaleDisconnectSequence;
    if (stamped.packetSequence == 0) {
      ++scalePacketSequence;
      if (scalePacketSequence == 0) {
        scalePacketSequence = 1;
      }
      stamped.packetSequence = scalePacketSequence;
    }
    if (currentScaleWorkerPolicy().controlReady &&
        scaleLinkState == ScaleLinkState::CONNECTED &&
        lastScaleWeightAtMs != 0 &&
        static_cast<int32_t>(stamped.receivedAtMs - lastScaleWeightAtMs) > 0) {
      const uint32_t dt = stamped.receivedAtMs - lastScaleWeightAtMs;
      if (dt > SCALE_STREAM_GAP_MS) {
        ++scalePacketGaps;
        if (dt > scaleMaxPacketGapMs) scaleMaxPacketGapMs = dt;
        streamGapMs = dt;
        scaleWeightUpdateIntervalMs = 0;
      } else if (scaleWeightUpdateIntervalMs == 0) {
        scaleWeightUpdateIntervalMs = dt;
      } else {
        // Low-cost EWMA over roughly eight updates. Keeping this integer-only
        // makes the diagnostic passive even on the scale worker's hot path.
        scaleWeightUpdateIntervalMs =
            (scaleWeightUpdateIntervalMs * 7U + dt + 4U) / 8U;
      }
    }
    lastScaleWeightAtMs = stamped.receivedAtMs;
    portEXIT_CRITICAL(&scaleLinkMux);

    scaleCriticalEventMux.lock();
    if (scaleWeightEventCount == SCALE_WEIGHT_EVENT_CAPACITY) {
      // Lost samples may contain a sign reversal. Never join evidence across
      // overflow; retain the newest reading with an explicit discontinuity.
      scaleWeightEventDrops += scaleWeightEventCount;
      scaleWeightEventHead = 0;
      scaleWeightEventCount = 0;
      stamped.sampleDiscontinuity = true;
    }
    const size_t tail = (scaleWeightEventHead + scaleWeightEventCount) %
                        SCALE_WEIGHT_EVENT_CAPACITY;
    scaleWeightEvents[tail] = stamped;
    ++scaleWeightEventCount;
    scaleWeightEventPending = true;
    scaleCriticalEventMux.unlock();
    if (scaleWeightEventDrops != profileLastWeightFifoDrops) {
      const uint32_t droppedWindow =
          scaleWeightEventDrops - profileLastWeightFifoDrops;
      profileLastWeightFifoDrops = scaleWeightEventDrops;
      scaleProfileNoteEvent(ScaleProfileEvent::CONTROL_FIFO_LOSS, millis(),
                            stamped.connectionGeneration,
                            stamped.packetSequence, NAN, droppedWindow, 0);
    }
    if (streamGapMs != 0) {
      const uint32_t nowMs = millis();
      scaleProfileNoteEvent(ScaleProfileEvent::PACKET_GAP, nowMs,
                            stamped.connectionGeneration,
                            stamped.packetSequence, NAN, streamGapMs, 0);
      if (lastScalePacketGapLogMs == 0 ||
          static_cast<uint32_t>(nowMs - lastScalePacketGapLogMs) >=
              SCALE_PACKET_GAP_LOG_MIN_MS) {
        lastScalePacketGapLogMs = nowMs;
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_PACKET_GAP,
                      static_cast<int32_t>(stamped.packetSequence),
                      static_cast<int32_t>(streamGapMs));
      }
    }
    if (scaleWorkerBridge.notifyWeightSample != nullptr) {
      scaleWorkerBridge.notifyWeightSample();
    }
    return true;
  }

  portENTER_CRITICAL(&scaleLinkMux);
  if (stamped.connectionGeneration == 0) {
    stamped.connectionGeneration = scaleConnectionGeneration;
  }
  stamped.disconnectSequence = scaleDisconnectSequence;
  portEXIT_CRITICAL(&scaleLinkMux);

  TaskLockGuard eventLock(scaleCriticalEventMux);
  if (critical) {
    // Weight events use their own bounded FIFO. Command results normally
    // use this FIFO; dedicated fallbacks preserve reference changes and
    // distinct START/STOP results when the FIFO is full.
    if (scaleEventQueue != nullptr &&
        xQueueSend(scaleEventQueue, &stamped, 0) == pdTRUE) {
      return true;
    }
    ScaleEvent *fallback = &scaleCriticalEvent;
    bool *fallbackPending = &scaleCriticalEventPending;
    if (stamped.type == ScaleEventType::REFERENCE_CHANGED) {
      fallback = &scaleReferenceChangedEvent;
      fallbackPending = &scaleReferenceChangedEventPending;
    } else if (stamped.type == ScaleEventType::TIMER_START_RESULT) {
      fallback = &scaleTimerStartEvent;
      fallbackPending = &scaleTimerStartEventPending;
    }
    if (*fallbackPending) {
      ++scaleEventsDropped;
    }
    *fallback = stamped;
    *fallbackPending = true;
    return true;
  }
  if (scaleEventQueue == nullptr) {
    ++scaleEventsDropped;
    eventLock.unlock();
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_EVENT_DROPPED,
                  static_cast<int32_t>(stamped.type));
    scaleProfileNoteEvent(ScaleProfileEvent::EVENT_DROPPED, millis(),
                          stamped.connectionGeneration, 0, NAN,
                          scaleEventsDropped.load(std::memory_order_relaxed), 0);
    return false;
  }
  if (xQueueSend(scaleEventQueue, &stamped, 0) != pdTRUE) {
    ++scaleEventsDropped;
    eventLock.unlock();
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_EVENT_DROPPED,
                  static_cast<int32_t>(stamped.type));
    scaleProfileNoteEvent(ScaleProfileEvent::EVENT_DROPPED, millis(),
                          stamped.connectionGeneration, 0, NAN,
                          scaleEventsDropped.load(std::memory_order_relaxed), 0);
    return false;
  }
  return true;
}

void updateWorkerLinkState() {
  const bool linkUp = scale.isLinkUp();
  char shotName[sizeof(scaleShotName)] = {};
  if (linkUp) {
    const TaskLockGuard lock(scalePreferredMacMux);
    const char *name = scaleDefaultFriendlyName(scale.localName());
    for (const auto &entry : scaleHistory) {
      if (preferredScaleMacEqual(entry.mac, scale.address()) && entry.friendlyName[0]) {
        name = entry.friendlyName;
        break;
      }
    }
    copyCString(shotName, sizeof(shotName), name);
    // Truncate at a UTF-8 boundary, never in the middle of a BLE name character.
    size_t length = strlen(shotName);
    while (length && (static_cast<unsigned char>(name[length]) & 0xc0U) == 0x80U)
      shotName[--length] = '\0';
  }
  const bool timerValid = scale.hasTimer();
  const uint32_t timerMs = timerValid ? scale.getTimerMs() : 0;
  const uint32_t timerAgeMs = timerValid ? scale.lastTimerAgeMs() : 0;
  const bool connecting = scale.isConnecting() && !scale.isLinkUp();
  const ScaleBleDiagnostics diagnostics = scale.diagnostics();
  portENTER_CRITICAL(&scaleLinkMux);
  scaleConnecting = connecting;
  scaleRejectedPackets = scale.rejectedPacketCount();
  scaleReconnects = scale.reconnectCount();
  scaleLastDisconnectReason =
      static_cast<uint8_t>(scale.lastDisconnectReason());
  scaleBleDiagnostics = diagnostics;
  copyCString(scaleProtocolName, sizeof(scaleProtocolName),
              scale.connectedProtocolName());
  copyCString(scaleConnectedMac, sizeof(scaleConnectedMac),
              linkUp ? scale.address() : "");
  memcpy(scaleShotName, shotName, sizeof(scaleShotName));
  scaleLinkFeatures = linkUp ? scale.features()
                                       : scaleFeatureSetNone();
  scaleLinkModel = linkUp ? scale.model() : ScaleModel::Unknown;
  scaleTimerValid = timerValid;
  scaleTimerMs = timerMs;
  scaleTimerAgeMs = timerAgeMs;
  portEXIT_CRITICAL(&scaleLinkMux);
  setScaleLinkState(linkUp ? ScaleLinkState::CONNECTED
                                        : ScaleLinkState::DISCONNECTED);
}

bool publishPendingScaleWeightEvent() {
  if (!scale.isLinkUp()) {
    return false;
  }
  const bool weightAvailable = scale.newWeightAvailable();
  if (!scale.isLinkUp()) {
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    return false;
  }
  if (!weightAvailable) {
    return false;
  }
  ScaleEvent event;
  event.type = ScaleEventType::WEIGHT;
  const ScaleWeightSample sample = scale.getWeightSample();
  // Raw observation at the decode boundary: before acceptance, tare grace,
  // range/slew/recovery rejection, or control FIFO publication.
  scaleProfileNoteWeight(sample.weightG, sample.receivedAtMs,
                         scaleConnectionGenerationNow(),
                         sample.captureSequence, false, millis());
  event.receivedAtMs = sample.receivedAtMs;
  event.captureSequence = sample.captureSequence;
  event.weightG = sample.weightG;
  // Display-only: the flow skips control and travels with the link snapshot.
  portENTER_CRITICAL(&scaleLinkMux);
  scaleFlowRateValid = sample.flowRateValid;
  scaleFlowRateGps = sample.flowRateGps;
  scaleFlowRateAtMs = sample.receivedAtMs;
  portEXIT_CRITICAL(&scaleLinkMux);
  publishScaleEvent(event, false);
  return true;
}

// Drains every frame queued since the last wake: the host task wakes the
// worker per frame and the 10 ms tick only covers a missed notification, so
// no weight waits for the next tick.
bool pollPendingScaleWeightEvent() {
  bool published = false;
  for (unsigned n = 0; n < SCALE_WEIGHT_DRAIN_MAX && publishPendingScaleWeightEvent(); ++n)
    published = true;
  return published;
}

void yieldBetweenScaleAttOps() {
  markScaleWorkerProgress();
  feedOrTripCurrentTaskWatchdog();
}

float capturePreTareWeight(const ScaleCommand &command) {
  // Include buffered notifications before comparing with control's approval.
  publishPendingScaleWeightEvent();
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  const TaskLockGuard lock(idleScaleTareMux);
  return approvedTareSample.packetSequence != 0 &&
      approvedTareSample.packetSequence == link.packetSequence &&
      approvedTareSample.connectionGeneration == command.connectionGeneration &&
      command.connectionGeneration == link.connectionGeneration &&
      static_cast<uint32_t>(millis() - approvedTareSample.atMs) <= MAX_AUTOMATION_WEIGHT_AGE_MS
      ? approvedTareSample.weightG : NAN;
}

void setScaleCommandCycle(uint32_t cycleId) {
  const TaskLockGuard lock(idleScaleTareMux);
  activeScaleCommandCycle = cycleId;
}

struct ScaleWriteAdmission {
  const ScaleCommand &command;
  ScaleEvent &event;
  bool tare = false;
};

bool approveScaleGeneration(void *context, uint32_t &captureBoundary) {
  captureBoundary = scale.notificationSequence();
  return !scalePowerOffBlocksGeneration(*static_cast<uint32_t *>(context));
}

bool approveScaleWrite(void *context, uint32_t &captureBoundary) {
  auto &write = *static_cast<ScaleWriteAdmission *>(context);
  if (write.tare) {
    write.event.preTareWeightG = capturePreTareWeight(write.command);
    captureBoundary = scale.notificationSequence();
    write.event.captureSequence = captureBoundary;
    if (write.command.idleTareRequestId != 0) {
      if (static_cast<int32_t>(millis() - write.command.expiresAtMs) >= 0) {
        finishIdleScaleTare(write.command.idleTareRequestId, false, IdleTareReason::EXPIRED);
        return false;
      }
      if (!std::isfinite(write.event.preTareWeightG) ||
          scale.getWeightSample().captureSequence != captureBoundary ||
          !claimIdleScaleTare(write.command.idleTareRequestId,
                             getScaleLinkSnapshot().packetSequence, captureBoundary)) return false;
    }
  }
  TaskLockGuard lock(idleScaleTareMux);
  if (write.command.cycleId != 0 && write.command.cycleId != activeScaleCommandCycle) {
    write.event.discardedStaleConnection = true;
    return false;
  }
  lock.unlock();
  if (scalePowerOffBlocksGeneration(write.command.connectionGeneration)) {
    write.event.discardedStaleConnection = true;
    return false;
  }
  write.event.commandAttempted = true;
  return true;
}

void executeScaleStartCommand(const ScaleCommand &command) {
  ScaleEvent event;
  event.type = ScaleEventType::TIMER_START_RESULT;
  event.cupWeightRequestId = command.cupWeightRequestId;
  event.cycleId = command.cycleId;
  event.connectionGeneration = command.connectionGeneration;
  event.commandFeedbackExpected = command.commandFeedbackExpected;
  ScaleWriteAdmission write{command, event};
  const ScaleCommandAdmission admission{approveScaleWrite, &write};

  if (scale.isConnected()) {
    bool allowSeparateStart = true;
    if (command.canTareStartTimer && command.autoTare &&
        scale.features().has(ScaleFeatureCombinedTareStart)) {
      event.usedCombinedTareStart = true;
      write.tare = true;
      const ScaleCommandResult result = scale.tareStartTimer(&admission);
      if (result == ScaleCommandResult::Deferred) event.commandAttempted = false;
      event.tareAttempted = event.commandAttempted;
      event.tareSucceeded = scaleCommandOk(result);
      event.writeSucceeded = scaleCommandOk(result);
      // A failed ATT response does not prove the scale ignored the command.
      // Only an unsupported operation permits a separate start/tare fallback.
      allowSeparateStart = result == ScaleCommandResult::Unsupported;
      yieldBetweenScaleAttOps();
    }
    if (!event.writeSucceeded && allowSeparateStart) {
      event.usedCombinedTareStart = false;
      write.tare = false;
      bool resetSucceeded = true;
      if (scale.features().has(ScaleFeatureResetTimer)) {
        resetSucceeded = scaleCommandOk(scale.resetTimer(&admission));
        yieldBetweenScaleAttOps();
      }
      if (resetSucceeded && scale.features().has(ScaleFeatureStartTimer)) {
        event.writeSucceeded = scaleCommandOk(scale.startTimer(&admission));
        yieldBetweenScaleAttOps();
      }
      if (event.writeSucceeded && command.autoTare &&
          scale.features().has(ScaleFeatureTare)) {
        write.tare = true;
        const ScaleCommandResult result = scale.tare(&admission);
        event.tareAttempted = result != ScaleCommandResult::Unsupported &&
                              result != ScaleCommandResult::Deferred;
        event.tareSucceeded = scaleCommandOk(result);
        yieldBetweenScaleAttOps();
      }
    }
  }

  updateWorkerLinkState();
  if (event.tareAttempted) {
    scaleProfileNoteEvent(
        event.tareSucceeded ? ScaleProfileEvent::TARE_WRITE_OK
                            : ScaleProfileEvent::TARE_WRITE_FAILED,
        millis(), event.connectionGeneration, event.captureSequence, NAN,
        event.cupWeightRequestId, 0);
  }
  publishScaleEvent(event, true);
}

void executeScaleStopCommand(const ScaleCommand &command) {
  ScaleEvent event;
  event.type = ScaleEventType::TIMER_STOP_RESULT;
  event.cycleId = command.cycleId;
  event.connectionGeneration = command.connectionGeneration;
  event.commandFeedbackExpected = command.commandFeedbackExpected;

  if (scale.isConnected()) {
    // A failed start write may only mean its ATT response was lost. Attempting
    // STOP on the existing connection is harmless and covers that case.
    uint32_t generation = command.connectionGeneration;
    const ScaleCommandAdmission admission{approveScaleGeneration, &generation};
    const ScaleCommandResult result = scale.stopTimer(&admission);
    event.commandAttempted = result != ScaleCommandResult::Deferred;
    event.writeSucceeded = scaleCommandOk(result);
    yieldBetweenScaleAttOps();
  }

  updateWorkerLinkState();
  publishScaleEvent(event, true);
}

void executeScaleTareCommand(const ScaleCommand &command) {
  ScaleEvent event;
  event.type = ScaleEventType::TARE_RESULT;
  event.cupWeightRequestId = command.cupWeightRequestId;
  event.cycleId = command.cycleId;
  event.idleTareRequestId = command.idleTareRequestId;
  event.connectionGeneration = command.connectionGeneration;
  event.commandFeedbackExpected = command.commandFeedbackExpected;
  ScaleWriteAdmission write{command, event, true};
  const ScaleCommandAdmission admission{approveScaleWrite, &write};

  if (scale.isConnected()) {
    const ScaleCommandResult result = scale.tare(&admission);
    if (result == ScaleCommandResult::Deferred) {
      event.commandAttempted = false;
      if (command.idleTareRequestId != 0) {
        {
          const TaskLockGuard lock(idleScaleTareMux);
          if (workerIdleTare.requestId == command.idleTareRequestId &&
              workerIdleTare.phase == IdleTarePhase::WRITING)
            workerIdleTare.phase = IdleTarePhase::QUEUED;
        }
        const IdleTareStatus status = idleScaleTareStatus();
        if (status.requestId == command.idleTareRequestId &&
            status.phase == IdleTarePhase::QUEUED) {
          scaleProfileNoteEvent(ScaleProfileEvent::TARE_PHASE, millis(),
                                status.connectionGeneration, status.captureBoundary, NAN,
                                status.requestId, static_cast<uint32_t>(IdleTarePhase::QUEUED));
          if (xQueueSend(scaleCommandQueue, &command, 0) != pdTRUE)
            finishIdleScaleTare(command.idleTareRequestId, false, IdleTareReason::QUEUE_FULL);
        }
        return;
      }
    }
    event.writeSucceeded = scaleCommandOk(result);
    event.receivedAtMs = millis();
    if (event.commandAttempted) {
      scaleProfileNoteEvent(
          event.writeSucceeded ? ScaleProfileEvent::TARE_WRITE_OK
                               : ScaleProfileEvent::TARE_WRITE_FAILED,
          event.receivedAtMs, event.connectionGeneration,
          event.captureSequence, NAN,
          command.idleTareRequestId != 0 ? command.idleTareRequestId
                                         : command.cupWeightRequestId,
          0);
    }
    if (event.writeSucceeded && command.cycleId != 0 && command.idleTareRequestId == 0) {
      const TaskLockGuard lock(scaleCriticalEventMux);
      shotTareResult = event;
    }
    yieldBetweenScaleAttOps();
  }

  updateWorkerLinkState();
  if (command.idleTareRequestId != 0) {
    finishIdleScaleTare(command.idleTareRequestId, event.writeSucceeded,
                       IdleTareReason::WRITE_FAILED, event.preTareWeightG);
  }
  publishScaleEvent(event, true);
}

void executeScaleBeepCommand(DebugCode successCode, DebugCode failureCode,
                             DebugCode unsupportedCode) {
  if (!scale.isConnected()) {
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    addDebugEvent(DebugCategory::SCALE, failureCode);
    return;
  }
  if (!scale.features().has(ScaleFeatureIndependentBeep)) {
    addDebugEvent(DebugCategory::SCALE, unsupportedCode);
    return;
  }
  uint32_t generation = scaleConnectionGenerationNow();
  const ScaleCommandAdmission admission{approveScaleGeneration, &generation};
  const bool succeeded = scaleCommandOk(scale.beepWithoutStateChange(&admission));
  yieldBetweenScaleAttOps();
  addDebugEvent(DebugCategory::SCALE, succeeded ? successCode : failureCode);
  updateWorkerLinkState();
}

bool scaleHasVolumeControl() {
  return scale.isConnected() && scale.features().has(ScaleFeatureVolume);
}

bool enqueueScaleDebugCommand(BookooDebugAction action, uint8_t beepLevel) {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (link.state != ScaleLinkState::CONNECTED ||
      !link.features.has(ScaleFeatureVolume) ||
      scalePowerOffBlocksGeneration(link.connectionGeneration)) {
    return false;
  }
  if (action == BookooDebugAction::VOLUME && beepLevel > BOOKOO_BEEP_LEVEL_MAX) {
    return false;
  }
  portENTER_CRITICAL(&scaleDebugMux);
  const bool busy = scaleDebugPending;
  if (!busy) {
    scaleDebugPending = true;
    scaleDebugAction = action;
    scaleDebugBeepLevel = beepLevel;
    scaleDebugConnectionGeneration = link.connectionGeneration;
  }
  portEXIT_CRITICAL(&scaleDebugMux);
  return !busy;
}

bool takeScaleDebugCommand(BookooDebugAction &action, uint8_t &beepLevel) {
  bool pending = false;
  uint32_t connectionGeneration = 0;
  portENTER_CRITICAL(&scaleDebugMux);
  if (scaleDebugPending) {
    pending = true;
    action = scaleDebugAction;
    beepLevel = scaleDebugBeepLevel;
    connectionGeneration = scaleDebugConnectionGeneration;
    scaleDebugPending = false;
    scaleDebugAction = BookooDebugAction::START;
    scaleDebugBeepLevel = 0;
    scaleDebugConnectionGeneration = 0;
  }
  portEXIT_CRITICAL(&scaleDebugMux);
  if (!pending) {
    return false;
  }
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (link.state != ScaleLinkState::CONNECTED ||
      connectionGeneration == 0 ||
      connectionGeneration != link.connectionGeneration ||
      scalePowerOffBlocksGeneration(connectionGeneration)) {
    static const char *const actionNames[] = {
        "start", "stop", "tare", "combined", "beep", "volume"};
    const size_t actionIndex = static_cast<size_t>(action);
    char detail[DEBUG_EVENT_TEXT_CAPACITY];
    snprintf(detail, sizeof(detail),
             "stale scale event rejected: debug=%s reason=%s gen=%lu/%lu",
             actionIndex < sizeof(actionNames) / sizeof(actionNames[0])
                 ? actionNames[actionIndex] : "unknown",
             link.state != ScaleLinkState::CONNECTED ? "disconnected" :
             connectionGeneration == 0 ? "missing generation" :
             connectionGeneration != link.connectionGeneration
                 ? "generation mismatch" : "power-off pending",
             static_cast<unsigned long>(connectionGeneration),
             static_cast<unsigned long>(link.connectionGeneration));
    addDebugEventDetail(DebugCategory::SCALE, DebugCode::SCALE_STALE_EVENT_REJECTED,
                  static_cast<int32_t>(connectionGeneration),
                  static_cast<int32_t>(link.connectionGeneration), detail);
    return false;
  }
  return true;
}

void executeScaleDebugCommand(BookooDebugAction action, uint8_t beepLevel) {
  if (!scale.isConnected()) {
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_FAILED);
    return;
  }
  if (!scaleHasVolumeControl()) {
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
    return;
  }
  ScaleCommandResult result = ScaleCommandResult::Unsupported;
  uint32_t generation = scaleConnectionGenerationNow();
  const ScaleCommandAdmission admission{approveScaleGeneration, &generation};
  switch (action) {
    case BookooDebugAction::START:
      result = scale.startTimer(&admission);
      break;
    case BookooDebugAction::STOP:
      result = scale.stopTimer(&admission);
      break;
    case BookooDebugAction::TARE:
    case BookooDebugAction::COMBINED:
      if (action == BookooDebugAction::COMBINED &&
          !scale.features().has(ScaleFeatureCombinedTareStart)) {
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
        return;
      }
      scaleProfileNoteEvent(
          ScaleProfileEvent::TARE_REQUEST, millis(), generation, 0, NAN, 0,
          static_cast<uint32_t>(ScaleProfileTareOrigin::MANUAL));
      result = action == BookooDebugAction::TARE
                   ? scale.tare(&admission)
                   : scale.tareStartTimer(&admission);
      scaleProfileNoteEvent(
          scaleCommandOk(result) ? ScaleProfileEvent::TARE_WRITE_OK
                                 : ScaleProfileEvent::TARE_WRITE_FAILED,
          millis(), generation, 0, NAN, 0, 0);
      break;
    case BookooDebugAction::BEEP:
      if (!scale.features().has(ScaleFeatureIndependentBeep)) {
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
        return;
      }
      result = scale.beepWithoutStateChange(&admission);
      break;
    case BookooDebugAction::VOLUME:
      if (!scale.features().has(ScaleFeatureVolume)) {
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
        return;
      }
      result = scale.setBeepLevel(beepLevel, &admission);
      break;
  }
  if (result == ScaleCommandResult::Unsupported ||
      result == ScaleCommandResult::InvalidArgument) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE,
                         "Scale command not sent: %s%u is unsupported by this model",
                         action == BookooDebugAction::VOLUME ? "volume level " : "action ",
                         action == BookooDebugAction::VOLUME
                                 ? static_cast<unsigned>(beepLevel)
                                 : static_cast<unsigned>(action));
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
    return;
  }
  const bool succeeded = scaleCommandOk(result);
  yieldBetweenScaleAttOps();
  if (result != ScaleCommandResult::Deferred &&
      (action == BookooDebugAction::TARE || action == BookooDebugAction::COMBINED)) {
    // Debug commands bypass the normal pre-tare capture contract.
    ScaleEvent event;
    event.type = ScaleEventType::REFERENCE_CHANGED;
    event.connectionGeneration = scaleConnectionGenerationNow();
    event.writeSucceeded = succeeded;
    publishScaleEvent(event, true);
  }
  addDebugEvent(DebugCategory::SCALE,
                succeeded ? DebugCode::SCALE_DEBUG_OK
                          : DebugCode::SCALE_DEBUG_FAILED);
  updateWorkerLinkState();
}

void applyBookooConnectBeepPolicy() {
  if (scalePowerOffBlocksGeneration(
          scaleConnectionGenerationNow())) {
    return;
  }
  if (!scaleHasVolumeControl() ||
      !scale.features().has(ScaleFeatureIndependentBeep)) {
    return;
  }
  const ScaleWorkerPolicySnapshot policy = currentScaleWorkerPolicy();
  const int8_t volume = bookooDesiredVolume(
      policy.soundAlertsEnabled, policy.alertOutputChannel,
      policy.bookooMuteOnBuzzerOnly, policy.bookooConnectBeepLevel);
  if (volume >= 0) {
    uint32_t generation = scaleConnectionGenerationNow();
    const ScaleCommandAdmission admission{approveScaleGeneration, &generation};
    const ScaleCommandResult result =
        scale.setBeepLevel(static_cast<uint8_t>(volume), &admission);
    if (result == ScaleCommandResult::InvalidArgument ||
        result == ScaleCommandResult::Unsupported) {
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE,
                           "Connection volume %d not sent: connected scale supports levels %u-%u",
                           static_cast<int>(volume),
                           static_cast<unsigned>(scale.features().volumeMin),
                           static_cast<unsigned>(scale.features().volumeMax));
    } else {
      yieldBetweenScaleAttOps();
    }
  }
}

void cancelBookooConnectBeepPolicy() {
  bookooConnectVolumePending = false;
}

void armBookooConnectBeepPolicy() {
  if (!scaleHasVolumeControl() ||
      !scale.features().has(ScaleFeatureIndependentBeep)) {
    cancelBookooConnectBeepPolicy();
    return;
  }
  scalePreferredMacMux.lock();
  bookooConnectVolumePending = consumeScaleHistorySessionConnection(
      scaleHistory, scaleHistorySeq, scale.address(), scale.localName());
  scalePreferredMacMux.unlock();
}

void serviceBookooConnectBeepPolicy(bool sawWeightThisTick) {
  if (!bookooConnectVolumePending) {
    return;
  }
  if (!scale.isLinkUp()) {
    cancelBookooConnectBeepPolicy();
    return;
  }
  if (!sawWeightThisTick) {
    return;
  }
  if (scalePowerOffBlocksGeneration(
          scaleConnectionGenerationNow())) {
    cancelBookooConnectBeepPolicy();
    return;
  }
  bookooConnectVolumePending = false;
  applyBookooConnectBeepPolicy();
}

void requestScaleBrewBeep(uint32_t cycleId) {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (scalePowerOffBlocksGeneration(link.connectionGeneration)) return;
  portENTER_CRITICAL(&scaleBeepMux);
  scaleBeepPending = true;
  scaleBeepCycleId = cycleId;
  scaleBeepConnectionGeneration = link.connectionGeneration;
  portEXIT_CRITICAL(&scaleBeepMux);
  wakeScaleWorker();
}

bool takeScaleBrewBeep(uint32_t &cycleId) {
  bool pending = false;
  uint32_t connectionGeneration = 0;
  portENTER_CRITICAL(&scaleBeepMux);
  if (scaleBeepPending) {
    pending = true;
    cycleId = scaleBeepCycleId;
    connectionGeneration = scaleBeepConnectionGeneration;
    scaleBeepPending = false;
    scaleBeepCycleId = 0;
    scaleBeepConnectionGeneration = 0;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  return pending && link.state == ScaleLinkState::CONNECTED &&
         connectionGeneration != 0 &&
         connectionGeneration == link.connectionGeneration &&
         !scalePowerOffBlocksGeneration(connectionGeneration);
}

void cancelScaleBrewBeep(uint32_t cycleId) {
  portENTER_CRITICAL(&scaleBeepMux);
  if (scaleBeepPending && scaleBeepCycleId == cycleId) {
    scaleBeepPending = false;
    scaleBeepCycleId = 0;
    scaleBeepConnectionGeneration = 0;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
}

void requestScalePaddleReturnReminderBeep() {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (scalePowerOffBlocksGeneration(link.connectionGeneration)) return;
  portENTER_CRITICAL(&scaleBeepMux);
  scalePaddleReturnReminderBeepPending = true;
  scalePaddleReturnReminderBeepConnectionGeneration = link.connectionGeneration;
  portEXIT_CRITICAL(&scaleBeepMux);
  wakeScaleWorker();
}

bool takeScalePaddleReturnReminderBeep() {
  bool pending = false;
  uint32_t connectionGeneration = 0;
  portENTER_CRITICAL(&scaleBeepMux);
  if (scalePaddleReturnReminderBeepPending) {
    pending = true;
    scalePaddleReturnReminderBeepPending = false;
    connectionGeneration = scalePaddleReturnReminderBeepConnectionGeneration;
    scalePaddleReturnReminderBeepConnectionGeneration = 0;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  return pending && link.state == ScaleLinkState::CONNECTED &&
         connectionGeneration != 0 &&
         connectionGeneration == link.connectionGeneration &&
         !scalePowerOffBlocksGeneration(connectionGeneration);
}

void cancelScalePaddleReturnReminderBeep() {
  portENTER_CRITICAL(&scaleBeepMux);
  scalePaddleReturnReminderBeepPending = false;
  scalePaddleReturnReminderBeepConnectionGeneration = 0;
  portEXIT_CRITICAL(&scaleBeepMux);
}

void requestScaleCompletionBeep() {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (scalePowerOffBlocksGeneration(link.connectionGeneration)) return;
  portENTER_CRITICAL(&scaleBeepMux);
  scaleCompletionBeepPending = true;
  scaleCompletionBeepConnectionGeneration = link.connectionGeneration;
  portEXIT_CRITICAL(&scaleBeepMux);
  wakeScaleWorker();
}

bool takeScaleCompletionBeep() {
  bool pending = false;
  uint32_t connectionGeneration = 0;
  portENTER_CRITICAL(&scaleBeepMux);
  if (scaleCompletionBeepPending) {
    pending = true;
    scaleCompletionBeepPending = false;
    connectionGeneration = scaleCompletionBeepConnectionGeneration;
    scaleCompletionBeepConnectionGeneration = 0;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  return pending && link.state == ScaleLinkState::CONNECTED &&
         connectionGeneration != 0 &&
         connectionGeneration == link.connectionGeneration &&
         !scalePowerOffBlocksGeneration(connectionGeneration);
}

// Machine-link request: power the connected scale off. Generation-guarded
// like the beep mailboxes so a disconnect between request and execution
// drops the command instead of writing to a stale connection.
void requestScalePowerOff() {
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (link.state != ScaleLinkState::CONNECTED ||
      link.connectionGeneration == 0) {
    return;
  }
  if (!link.features.has(ScaleFeaturePowerOff)) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE,
                         "Power off not sent: %s does not support it",
                         link.model == ScaleModel::BookooMini
                             ? "Bookoo Themis Mini" : "connected scale");
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
    return;
  }
  bool accepted = false;
  portENTER_CRITICAL(&scaleBeepMux);
  if (scalePowerOffBlockedGeneration != link.connectionGeneration) {
    scalePowerOffPending = true;
    scalePowerOffConnectionGeneration = link.connectionGeneration;
    scalePowerOffBlockedGeneration = link.connectionGeneration;
    scalePowerOffSentAtMs = 0;
    scalePowerOffSent = false;
    accepted = true;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
  if (!accepted) return;
  cancelOperationalScaleBeeps();
  portENTER_CRITICAL(&scaleDebugMux);
  scaleDebugPending = false;
  scaleDebugConnectionGeneration = 0;
  portEXIT_CRITICAL(&scaleDebugMux);
  wakeScaleWorker();
}

bool takeScalePowerOff() {
  bool pending = false;
  uint32_t connectionGeneration = 0;
  portENTER_CRITICAL(&scaleBeepMux);
  if (scalePowerOffPending) {
    pending = true;
    scalePowerOffPending = false;
    connectionGeneration = scalePowerOffConnectionGeneration;
    scalePowerOffConnectionGeneration = 0;
  }
  portEXIT_CRITICAL(&scaleBeepMux);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  return pending && link.state == ScaleLinkState::CONNECTED &&
         connectionGeneration != 0 &&
         connectionGeneration == link.connectionGeneration;
}

void executeScalePowerOffCommand() {
  if (!scale.isConnected()) {
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_FAILED);
    return;
  }
  if (!scale.supportsPowerOff()) {
    clearScalePowerOffLifecycle();
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE,
                         "Power off not sent: connected scale does not support it");
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
    return;
  }
  portENTER_CRITICAL(&scaleBeepMux);
  scalePowerOffSentAtMs = millis();
  scalePowerOffSent = true;
  portEXIT_CRITICAL(&scaleBeepMux);
  const ScaleCommandResult result = scale.powerOff();
  if (result == ScaleCommandResult::Unsupported) {
    clearScalePowerOffLifecycle();
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE,
                         "Power off not sent: connected scale does not support it");
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_DEBUG_UNSUPPORTED);
    return;
  }
  const bool succeeded = scaleCommandOk(result);
  yieldBetweenScaleAttOps();
  addDebugEvent(DebugCategory::SCALE,
                succeeded ? DebugCode::SCALE_DEBUG_OK
                          : DebugCode::SCALE_DEBUG_FAILED);
  updateWorkerLinkState();
}

void serviceScalePowerOffTimeout(uint32_t nowMs) {
  uint32_t generation = 0;
  uint32_t sentAtMs = 0;
  bool sent = false;
  portENTER_CRITICAL(&scaleBeepMux);
  generation = scalePowerOffBlockedGeneration;
  sentAtMs = scalePowerOffSentAtMs;
  sent = scalePowerOffSent;
  portEXIT_CRITICAL(&scaleBeepMux);
  if (generation == 0 || !sent ||
      !scalePowerOffBlocksGeneration(
          scaleConnectionGenerationNow()) ||
      static_cast<uint32_t>(nowMs - sentAtMs) <
          SCALE_POWER_OFF_DISCONNECT_TIMEOUT_MS) {
    return;
  }
  scale.disconnect();
  updateWorkerLinkState();
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
}

void executeScaleCommand(const ScaleCommand &command) {
  markScaleWorkerProgress();
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (command.connectionGeneration == 0 ||
      command.connectionGeneration != link.connectionGeneration ||
      link.state != ScaleLinkState::CONNECTED ||
      scalePowerOffBlocksGeneration(command.connectionGeneration)) {
    ScaleEvent event;
    event.cycleId = command.cycleId;
    event.cupWeightRequestId = command.cupWeightRequestId;
    event.idleTareRequestId = command.idleTareRequestId;
    event.connectionGeneration = command.connectionGeneration;
    event.commandFeedbackExpected = command.commandFeedbackExpected;
    event.discardedStaleConnection = true;
    if (command.idleTareRequestId != 0) {
      finishIdleScaleTare(command.idleTareRequestId, false);
    }
    switch (command.type) {
      case ScaleCommandType::START_TIMER_AND_TARE:
        event.type = ScaleEventType::TIMER_START_RESULT;
        break;
      case ScaleCommandType::TARE_ONLY:
        event.type = ScaleEventType::TARE_RESULT;
        break;
      case ScaleCommandType::STOP_TIMER:
        event.type = ScaleEventType::TIMER_STOP_RESULT;
        break;
    }
    publishScaleEvent(event, true);
    char detail[DEBUG_EVENT_TEXT_CAPACITY];
    snprintf(detail, sizeof(detail),
             "stale scale event rejected: command=%s reason=%s "
             "gen=%lu/%lu cycle=%lu",
             scaleCommandTypeName(command.type),
             command.connectionGeneration == 0 ? "missing generation" :
             command.connectionGeneration != link.connectionGeneration
                 ? "generation mismatch" :
             link.state != ScaleLinkState::CONNECTED ? "disconnected" :
                                                        "power-off pending",
             static_cast<unsigned long>(command.connectionGeneration),
             static_cast<unsigned long>(link.connectionGeneration),
             static_cast<unsigned long>(command.cycleId));
    addDebugEventDetail(DebugCategory::SCALE, DebugCode::SCALE_STALE_EVENT_REJECTED,
                  static_cast<int32_t>(command.connectionGeneration),
                  static_cast<int32_t>(link.connectionGeneration), detail);
    return;
  }
  switch (command.type) {
    case ScaleCommandType::START_TIMER_AND_TARE:
      executeScaleStartCommand(command);
      break;
    case ScaleCommandType::TARE_ONLY:
      executeScaleTareCommand(command);
      break;
    case ScaleCommandType::STOP_TIMER:
      executeScaleStopCommand(command);
      break;
  }
}

void copyPreferredScaleMac(char *out, size_t capacity) {
  if (out == nullptr || capacity == 0) {
    return;
  }
  scalePreferredMacMux.lock();
  copyCString(out, capacity, scalePreferredMac);
  scalePreferredMacMux.unlock();
}

void copyPreferredScaleName(char *out, size_t capacity) {
  if (out == nullptr || capacity == 0) {
    return;
  }
  scalePreferredMacMux.lock();
  copyCString(out, capacity, scalePreferredName);
  scalePreferredMacMux.unlock();
}

void copyScaleHistory(ScaleHistoryEntry *out) {
  if (out == nullptr) {
    return;
  }
  scalePreferredMacMux.lock();
  memcpy(out, scaleHistory, sizeof(scaleHistory));
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    clearScaleHistorySessionMarker(out[i]);
  }
  scalePreferredMacMux.unlock();
}

bool hasPreferredScaleMac() {
  char mac[PREFERRED_SCALE_MAC_CAPACITY];
  copyPreferredScaleMac(mac, sizeof(mac));
  return mac[0] != '\0' && validPreferredScaleMac(mac);
}

uint32_t scaleMacCachePauseRemainingMs(uint32_t nowMs) {
  scalePreferredMacMux.lock();
  const uint32_t until = scaleDiscoveryPausedUntilMs;
  scalePreferredMacMux.unlock();
  if (until == 0) {
    return 0;
  }
  const int32_t remaining = static_cast<int32_t>(until - nowMs);
  return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
}

bool scaleDiscoveryPaused(uint32_t nowMs) {
  return scaleMacCachePauseRemainingMs(nowMs) > 0;
}

constexpr uint8_t SCALE_PREFERENCE_RESET_IDENTITY = 1U << 0;
constexpr uint8_t SCALE_PREFERENCE_RESET_MODE = 1U << 1;

void requestScalePreferenceModeReset() {
  scalePreferredMacMux.lock();
  ++scalePreferredDirectedResetGeneration;
  scalePreferredResetReasonBits |= SCALE_PREFERENCE_RESET_MODE;
  scalePreferredMacMux.unlock();
}

uint8_t takeScalePreferenceResetReasons() {
  uint8_t reasons = 0;
  scalePreferredMacMux.lock();
  if (scalePreferredAppliedResetGeneration !=
      scalePreferredDirectedResetGeneration) {
    scalePreferredAppliedResetGeneration =
        scalePreferredDirectedResetGeneration;
    reasons = scalePreferredResetReasonBits;
    scalePreferredResetReasonBits = 0;
  }
  scalePreferredMacMux.unlock();
  return reasons;
}

ScaleMacCacheMode currentScaleMacCacheMode() {
  return currentScaleWorkerPolicy().macCacheMode;
}

bool scaleHuntRfClearActive(uint32_t nowMs = millis()) {
  return scaleHuntRfUntilMs != 0 &&
         static_cast<int32_t>(nowMs - scaleHuntRfUntilMs) < 0;
}

void armScaleHuntRfClear() {
  scaleHuntRfUntilMs = millis() + SCALE_HUNT_RF_CLEAR_MS;
}

void noteScaleHistory(const char *mac, const char *name, bool persist) {
  if (mac == nullptr || !validPreferredScaleMac(mac) || mac[0] == '\0') {
    return;
  }
  scalePreferredMacMux.lock();
  (void)upsertScaleHistory(scaleHistory, scaleHistorySeq, mac, name);
  if (persist) {
    scalePreferredMacDirty = true;
  }
  scalePreferredMacMux.unlock();
}

void notePreferredScale(const char *mac, const char *name) {
  const ScaleMacCacheMode cacheMode = currentScaleMacCacheMode();
  // FIRST never auto-writes preferred. PREFER/ONLY bootstrap from the first
  // successful compatible connection when no preferred MAC exists. Once set,
  // PREFER fallback connections must never replace it.
  if (mac == nullptr || !validPreferredScaleMac(mac) || mac[0] == '\0') {
    return;
  }
  char canonicalMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  copyCString(canonicalMac, sizeof(canonicalMac), mac);
  canonicalizePreferredScaleMac(canonicalMac, sizeof(canonicalMac));
  char safeName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  if (name != nullptr && validPreferredScaleName(name)) {
    copyCString(safeName, sizeof(safeName), name);
  }
  bool changed = false;
  bool adopted = false;
  scalePreferredMacMux.lock();
  (void)upsertScaleHistory(scaleHistory, scaleHistorySeq, canonicalMac,
                           safeName);
  // Successful connections are durable history even in FIRST mode.
  scalePreferredMacDirty = true;
  const uint32_t pauseUntil = scaleDiscoveryPausedUntilMs;
  const bool paused = pauseUntil != 0 &&
      static_cast<int32_t>(pauseUntil - millis()) > 0;
  if (cacheMode == ScaleMacCacheMode::FIRST || paused) {
    scalePreferredMacMux.unlock();
    return;
  }
  if (scalePreferredMac[0] == '\0') {
    memcpy(scalePreferredMac, canonicalMac, sizeof(scalePreferredMac));
    copyCString(scalePreferredName, sizeof(scalePreferredName), safeName);
    changed = true;
    adopted = true;
  } else if (!preferredScaleMacEqual(scalePreferredMac, canonicalMac)) {
    scalePreferredMacMux.unlock();
    return;
  } else if (strncmp(scalePreferredName, safeName,
                     PREFERRED_SCALE_NAME_CAPACITY) != 0) {
    copyCString(scalePreferredName, sizeof(scalePreferredName), safeName);
    changed = true;
  }
  scalePreferredMacMux.unlock();
  if (changed) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                         adopted ? "First detected scale adopted: %s — %s"
                                 : "Preferred scale name updated: %s — %s",
                         safeName[0] != '\0' ? safeName : "(unknown)", canonicalMac);
  }
}

// Settings choosing no preferred scale: clear it without the Forget 30 s pause.
static void clearPreferredScaleSelectionOnly() {
  scalePreferredMacMux.lock();
  scalePreferredMac[0] = '\0';
  scalePreferredName[0] = '\0';
  scalePreferredMacDirty = true;
  scaleDiscoveryPausedUntilMs = 0;
  ++scalePreferredDirectedResetGeneration;
  scalePreferredResetReasonBits |= SCALE_PREFERENCE_RESET_IDENTITY;
  scalePreferredMacMux.unlock();
  serialTraceCategory(LogLevel::INFO, DebugCategory::SCALE,
                      "Preferred scale cleared (history kept)");
}

void selectPreferredScale(const char *mac, const char *name) {
  if (mac == nullptr || mac[0] == '\0') {
    if (!hasPreferredScaleMac()) {
      return;
    }
    clearPreferredScaleSelectionOnly();
    return;
  }
  if (!validPreferredScaleMac(mac)) {
    return;
  }
  char canonicalMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  copyCString(canonicalMac, sizeof(canonicalMac), mac);
  canonicalizePreferredScaleMac(canonicalMac, sizeof(canonicalMac));
  char currentMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  copyPreferredScaleMac(currentMac, sizeof(currentMac));
  if (preferredScaleMacEqual(currentMac, canonicalMac)) {
    return;
  }
  char resolvedName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  if (name != nullptr && validPreferredScaleName(name) && name[0] != '\0') {
    copyCString(resolvedName, sizeof(resolvedName), name);
  } else {
    scalePreferredMacMux.lock();
    findScaleHistoryName(scaleHistory, canonicalMac, resolvedName,
                         sizeof(resolvedName));
    scalePreferredMacMux.unlock();
  }
  noteScaleHistory(canonicalMac, resolvedName, true);
  scalePreferredMacMux.lock();
  memcpy(scalePreferredMac, canonicalMac, sizeof(scalePreferredMac));
  copyCString(scalePreferredName, sizeof(scalePreferredName), resolvedName);
  scalePreferredMacDirty = true;
  scaleDiscoveryPausedUntilMs = 0;
  ++scalePreferredDirectedResetGeneration;
  scalePreferredResetReasonBits |= SCALE_PREFERENCE_RESET_IDENTITY;
  scalePreferredMacMux.unlock();
  serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                       "Preferred scale selected: %s — %s",
                       resolvedName[0] != '\0' ? resolvedName : "(unknown)",
                       canonicalMac);
}

// Assign or clear (empty name) the user-facing override of a known scale.
bool setScaleFriendlyName(const char *mac, const char *name) {
  if (mac == nullptr || mac[0] == '\0' || !validPreferredScaleMac(mac) ||
      name == nullptr || !validScaleFriendlyName(name)) {
    return false;
  }
  char canonicalMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  copyCString(canonicalMac, sizeof(canonicalMac), mac);
  canonicalizePreferredScaleMac(canonicalMac, sizeof(canonicalMac));
  bool applied = false;
  scalePreferredMacMux.lock();
  for (auto &entry : scaleHistory) {
    if (!preferredScaleMacEqual(entry.mac, canonicalMac)) {
      continue;
    }
    memset(entry.friendlyName, 0, sizeof(entry.friendlyName));
    copyCString(entry.friendlyName, sizeof(entry.friendlyName), name);
    applied = true;
    break;
  }
  if (applied) {
    scalePreferredMacDirty = true;
  }
  scalePreferredMacMux.unlock();
  if (applied) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                         "Scale friendly name set: %s — %s",
                         name[0] != '\0' ? name : "(cleared)", canonicalMac);
  }
  return applied;
}

void clearPreferredScaleCache() {
  scalePreferredMacMux.lock();
  // Forgetting drops the learned record too: the scale leaves the saved
  // list and its rename, and returns unnamed if it is ever re-learned.
  for (ScaleHistoryEntry &entry : scaleHistory) {
    if (preferredScaleMacEqual(entry.mac, scalePreferredMac)) {
      entry = ScaleHistoryEntry{};
      break;
    }
  }
  scalePreferredMac[0] = '\0';
  scalePreferredName[0] = '\0';
  scalePreferredMacDirty = true;
  scaleDiscoveryPausedUntilMs = millis() + SCALE_PAIRING_DISCOVERY_PAUSE_MS;
  ++scalePreferredDirectedResetGeneration;
  scalePreferredResetReasonBits |= SCALE_PREFERENCE_RESET_IDENTITY;
  scalePreferredMacMux.unlock();
  serialTraceCategory(LogLevel::INFO, DebugCategory::SCALE,
                      "Paired scale forgotten and removed from the scale "
                      "list; looking paused for 30 s");
}

void logScaleConnectionFailed(bool directed) {
  serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE, "%s: %s",
                       directed ? "Preferred scale connection failed"
                                : "Scale connection failed",
                       scale.lastDisconnectReasonName());
}

void logScaleScanStarted(bool directed) {
  scaleDiscoveryDirected = directed;
  // Report the duty this scan actually applies, so a machine-power override
  // stays visible even when the saved intensity differs.
  addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_SCAN_STARTED,
                directed ? SCALE_SCAN_TARGET_PREFERRED : SCALE_SCAN_TARGET_ANY,
                static_cast<int32_t>(discoveryScanIntensity()));
}

void logScaleConnectFailed(int32_t step) {
  addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_CONNECT_FAILED,
                static_cast<int32_t>(scale.lastDisconnectReason()), step);
}

bool serviceScaleGattConnecting() {
  feedOrTripCurrentTaskWatchdog();
  const uint8_t stepBefore = scale.connectStepId();
  if (!scaleLoggedGattConnecting) {
    scaleLoggedGattConnecting = true;
    scaleLoggedGattConnectAttempts = 0;
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_GATT_CONNECTING,
                  scaleDiscoveryDirected ? SCALE_SCAN_TARGET_PREFERRED
                                         : SCALE_SCAN_TARGET_ANY);
  }
  const bool connected = scale.pollScan();
  if (connected) {
    scaleLoggedGattConnecting = false;
    scaleLoggedGattConnectAttempts = 0;
    return true;
  }
  const uint8_t attempts = scale.connectAttemptCount();
  if (attempts > scaleLoggedGattConnectAttempts) {
    scaleLoggedGattConnectAttempts = attempts;
    addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_CONNECT_ATTEMPT_FAILED,
                  static_cast<int32_t>(attempts),
                  static_cast<int32_t>(scale.connectStepId()));
  }
  if (!scale.isConnecting()) {
    scaleLoggedGattConnecting = false;
    logScaleConnectFailed(stepBefore);
  }
  return false;
}

// ONLY + preferred MAC → name scan with connect-filter (not GAP directed).
// PREFER uses the same filter until SCALE_PREFER_FALLBACK_MS, then any scale.
bool shouldUseDirectedScaleScan(ScaleMacCacheMode cacheMode, bool hasMac,
                                bool preferStillWaiting) {
  if (!hasMac) {
    return false;
  }
  if (cacheMode == ScaleMacCacheMode::ONLY) {
    return true;
  }
  if (cacheMode == ScaleMacCacheMode::PREFER) {
    return preferStillWaiting;
  }
  return false;
}

bool applyScaleDiscoveryPause() {
  if (!scaleDiscoveryPaused()) {
    return false;
  }
  if (scale.isConnected() || scale.isScanning()) {
    scale.disconnect();
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    cancelBookooConnectBeepPolicy();
  }
  return true;
}

// Admin BLE master switch: while off, tear down any scale link or scan and
// hold the client idle. Firmware-wide this matches "no scale present".
bool serviceBleMasterSwitch() {
  if (liveBleEnabledRaw.load(std::memory_order_relaxed) != 0) {
    return false;
  }
  if (scaleLoggedGattConnecting || scale.isScanning() || scale.isConnecting() ||
      scale.isConnected()) {
    scale.disconnect();
    scaleLoggedGattConnecting = false;
    cancelBookooConnectBeepPolicy();
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
  }
  return true;
}

bool applySoftApDiscoveryYield() {
  if (!scaleSoftApActive.load(std::memory_order_relaxed)) {
    return false;
  }
  if (scale.isConnecting() || scale.isLinkUp()) {
    return false;
  }
  if (scale.isScanning()) {
    scale.disconnect();
    updateWorkerLinkState();
  }
  return true;
}

// Preference changes originate on the control task, but the BLE client is
// worker-owned. Consume the request here so a live scan/connect cannot keep
// using the old filter and no other task touches the BLE object.
bool applyScalePreferenceReset() {
  const uint8_t reasons = takeScalePreferenceResetReasons();
  if (reasons == 0) {
    return false;
  }

  if (scale.isLinkUp()) {
    char preferredMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
    copyPreferredScaleMac(preferredMac, sizeof(preferredMac));
    const bool hasPreferred =
        preferredMac[0] != '\0' && validPreferredScaleMac(preferredMac);
    const ScaleMacCacheMode mode = currentScaleMacCacheMode();
    const char *connectedMac = scale.address();
    const bool connectedMatches =
        hasPreferred && connectedMac != nullptr &&
        preferredScaleMacEqual(connectedMac, preferredMac);
    const bool identityCleared =
        (reasons & SCALE_PREFERENCE_RESET_IDENTITY) != 0 && !hasPreferred;
    const bool violatesSelection =
        mode != ScaleMacCacheMode::FIRST && hasPreferred && !connectedMatches;
    if (!scaleDiscoveryPaused() && !identityCleared && !violatesSelection) {
      return false;
    }
  } else if (!scale.isScanning() && !scale.isConnecting() &&
             !scaleLoggedGattConnecting) {
    return false;
  }

  scale.disconnect();
  scaleLoggedGattConnecting = false;
  scaleLoggedGattConnectAttempts = 0;
  cancelBookooConnectBeepPolicy();
  updateWorkerLinkState();
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  return true;
}

void resetScaleWorkerRadioStateForHost() {
  scaleConnecting = false;
  scaleScanAppliedInterval = 0;
  scaleScanAppliedWindow = 0;
  scaleHuntRfUntilMs = 0;
  scaleSoftApActive.store(false, std::memory_order_relaxed);
  scaleLoggedGattConnecting = false;
  scaleLoggedGattConnectAttempts = 0;
  scaleDiscoveryDirected = false;
  liveBleScanIntensityRaw.store(
      static_cast<uint8_t>(BLE_SCAN_FACTORY_INTENSITY),
      std::memory_order_relaxed);
  liveBleEnabledRaw.store(1, std::memory_order_relaxed);
  liveBleScanOverrideRaw.store(
      static_cast<uint8_t>(BleScanIntensity::BALANCED),
      std::memory_order_relaxed);
  bookooConnectVolumePending = false;
  scaleDebugConnectionGeneration = 0;
  scaleBeepConnectionGeneration = 0;
  scalePaddleReturnReminderBeepConnectionGeneration = 0;
  scaleCompletionBeepConnectionGeneration = 0;
  clearScalePowerOffLifecycle();
  for (auto &entry : scaleHistory) {
    clearScaleHistorySessionMarker(entry);
  }
  scaleLinkRssiValid = false;
  scaleLinkRssi = 0;
  lastScaleLinkRssiSampleMs = 0;
  portENTER_CRITICAL(&scaleLinkMux);
  copyCString(scaleProtocolName, sizeof(scaleProtocolName), "none");
  portEXIT_CRITICAL(&scaleLinkMux);
  scalePreferredMacMux.lock();
  scalePreferredDirectedResetGeneration = 0;
  scalePreferredAppliedResetGeneration = 0;
  scalePreferredResetReasonBits = 0;
  scalePreferredMacMux.unlock();
}

void serviceScaleLinkRssi(uint32_t nowMs) {
  if (!scale.isConnected()) {
    lastScaleLinkRssiSampleMs = 0;
    portENTER_CRITICAL(&scaleLinkMux);
    scaleLinkRssiValid = false;
    scaleLinkRssi = 0;
    portEXIT_CRITICAL(&scaleLinkMux);
    return;
  }
  if (lastScaleLinkRssiSampleMs != 0 &&
      static_cast<uint32_t>(nowMs - lastScaleLinkRssiSampleMs) <
          SCALE_LINK_RSSI_SAMPLE_MS) {
    return;
  }
  lastScaleLinkRssiSampleMs = nowMs;
  const int raw = scale.linkRssi();
  const bool valid = raw != SCALE_LINK_RSSI_UNAVAILABLE && raw >= -128 &&
                     raw <= 126;
  portENTER_CRITICAL(&scaleLinkMux);
  scaleLinkRssiValid = valid;
  scaleLinkRssi = valid ? static_cast<int8_t>(raw) : 0;
  portEXIT_CRITICAL(&scaleLinkMux);
}

void applyLiveBleScanIntensity(BleScanIntensity intensity) {
  liveBleScanIntensityRaw.store(static_cast<uint8_t>(clampBleScanIntensity(
                                    static_cast<uint8_t>(intensity))),
                                std::memory_order_relaxed);
}

void applyLiveBleEnabled(bool enabled) {
  liveBleEnabledRaw.store(enabled ? 1 : 0, std::memory_order_relaxed);
  // Wake the worker so an enabling edge resumes discovery on the next tick
  // and a disabling edge tears the link down without waiting for the cadence.
  wakeScaleWorker();
}

bool liveBleEnabled() {
  return liveBleEnabledRaw.load(std::memory_order_relaxed) != 0;
}

void applyLiveBleScanOverride(BleScanIntensity intensity) {
  liveBleScanOverrideRaw.store(
      static_cast<uint8_t>(clampBleScanIntensity(
          static_cast<uint8_t>(intensity))),
      std::memory_order_relaxed);
}

BleScanIntensity liveBleScanIntensity() {
  return clampBleScanIntensity(
      liveBleScanIntensityRaw.load(std::memory_order_relaxed));
}

BleScanIntensity discoveryScanIntensity() {
  const uint8_t discoveryOverride =
      liveBleScanOverrideRaw.load(std::memory_order_relaxed);
  if (discoveryOverride != static_cast<uint8_t>(BleScanIntensity::BALANCED)) {
    return clampBleScanIntensity(discoveryOverride);
  }
  return liveBleScanIntensity();
}

bool startScaleDiscoveryScan(const char *mac, bool forceRestart) {
  // Master switch guards the scan funnel itself, like the soft-AP yield
  // below: no caller may restart radio work while BLE is disabled.
  if (!liveBleEnabledRaw.load(std::memory_order_relaxed)) {
    return false;
  }
  if (scaleSoftApActive.load(std::memory_order_relaxed) &&
      !scale.isConnecting() && !scale.isLinkUp()) {
    return false;
  }
  uint16_t interval = BLE_SCAN_BALANCED_INTERVAL;
  uint16_t window = BLE_SCAN_BALANCED_WINDOW;
  bleScanHciParams(discoveryScanIntensity(), interval, window);
  const bool scanningBefore = scale.isScanning();
  const uint16_t prevInterval = scaleScanAppliedInterval;
  const uint16_t prevWindow = scaleScanAppliedWindow;
  const bool addressScan =
      mac != nullptr && mac[0] != '\0' &&
      currentScaleMacCacheMode() == ScaleMacCacheMode::ONLY;
  if (!scale.startScan(mac, forceRestart, interval, window, addressScan)) {
    return false;
  }
  scaleScanAppliedInterval = interval;
  scaleScanAppliedWindow = window;
  if (!scanningBefore || forceRestart || prevInterval != interval ||
      prevWindow != window) {
    armScaleHuntRfClear();
  }
  return true;
}

void fillCurrentScaleScanFilter(char *macOut, size_t cap, bool &useDirected) {
  char preferredMac[PREFERRED_SCALE_MAC_CAPACITY];
  copyPreferredScaleMac(preferredMac, sizeof(preferredMac));
  const bool hasMac =
      preferredMac[0] != '\0' && validPreferredScaleMac(preferredMac);
  useDirected = scale.isDirectedScan() && hasMac;
  if (useDirected) {
    copyCString(macOut, cap, preferredMac);
  } else if (macOut != nullptr && cap > 0) {
    macOut[0] = '\0';
  }
}

void serviceScaleScanIntensity() {
  if (!scale.isScanning() || scale.isConnecting()) {
    return;
  }
  uint16_t interval = BLE_SCAN_BALANCED_INTERVAL;
  uint16_t window = BLE_SCAN_BALANCED_WINDOW;
  bleScanHciParams(discoveryScanIntensity(), interval, window);
  if (scaleScanAppliedInterval == interval &&
      scaleScanAppliedWindow == window) {
    return;
  }
  char mac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  bool useDirected = false;
  fillCurrentScaleScanFilter(mac, sizeof(mac), useDirected);
  (void)startScaleDiscoveryScan(useDirected ? mac : nullptr, true);
}

// Called before polling Settling -> GAP connect. Sleep disable alone need not
// wake the controller; its wake path restores the driver's 80-MHz APB lock.
static bool scalePowerInitialized = false;
static bool scalePowerWaking = false;
static uint32_t scalePowerWakeStartedMs = 0;
bool syncScalePower() {
  bool busy = scale.isConnecting() || scale.isLinkUp();
  powerScaleBusy.store(busy, std::memory_order_release);
  bool sleep = powerIdleSavings() && !busy &&
               powerBleError.load(std::memory_order_relaxed) == 0;
  if (!scalePowerInitialized ||
      sleep != powerBleSleeping.load(std::memory_order_relaxed)) {
    const int error = sleep ? esp_bt_sleep_enable() : esp_bt_sleep_disable();
    if (error != ESP_OK) {
      powerBleError.store(error, std::memory_order_release);
      // Optional sleep failure may recover to awake operation. A failed
      // restore must not masquerade as a healthy, watchdog-fed worker.
      if (!sleep || esp_bt_sleep_disable() != ESP_OK) {
        reportTaskWatchdogFault();
        return false;
      }
      sleep = false;
    }
    scalePowerInitialized = true;
    powerBleSleeping.store(sleep, std::memory_order_release);
    if (!sleep) esp_bt_controller_wakeup_request();
  }
  if (!sleep && esp_bt_controller_is_sleeping()) {
    if (!scalePowerWaking) {
      scalePowerWaking = true;
      scalePowerWakeStartedMs = millis();
    } else if (uint32_t(millis() - scalePowerWakeStartedMs) >= 100) {
      powerBleError.store(ESP_ERR_TIMEOUT, std::memory_order_release);
      reportTaskWatchdogFault();
    }
    return false;
  }
  scalePowerWaking = false;
  return true;
}

void syncScaleRadioCoex() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  if (scaleWorkerBridge.syncNetworkRf != nullptr) {
    scaleWorkerBridge.syncNetworkRf(scale.isConnecting() || scale.isLinkUp(),
                                    scale.isConnecting(),
                                    scaleHuntRfClearActive(), scale.communicationSilenced());
  }
#endif
}

bool configureScaleWorkerBridge(const ScaleWorkerBridgeCallbacks &callbacks) {
  if (scaleWorkerTaskHandle.load(std::memory_order_acquire) != nullptr ||
      callbacks.syncNetworkRf == nullptr) {
    return false;
  }
  scaleWorkerBridge = callbacks;
  scale.setAcquisitionObserver(callbacks.notifyAcquisition);
  return true;
}

void serviceScaleWorkerDiscovery(uint32_t &lastScanCycleMs,
                                 uint32_t &lastConnectLogMs,
                                 bool &connectAttemptSeriesActive,
                                 uint32_t &scanSessionAtMs,
                                 uint32_t &scanLastAdvertAtMs) {
  const bool preferenceRestarted = applyScalePreferenceReset();
  if (preferenceRestarted) {
    connectAttemptSeriesActive = false;
    scanSessionAtMs = millis();
    scanLastAdvertAtMs = 0;
  }
  if (scale.communicationSilenced()) {
    updateWorkerLinkState();
    return;
  }
  // Library drop can happen on a beep/command path that never refreshed the
  // link snapshot. Clear CONNECTED before idle scan work so the UI cannot sit
  // on "BLE connected" for the whole (indefinite) discovery session.
  if (!scale.isConnected() && !scale.isConnecting()) {
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
  }
  if (applyScaleDiscoveryPause()) {
    return;
  }
  if (applySoftApDiscoveryYield()) {
    return;
  }

  const ScaleMacCacheMode cacheMode = currentScaleMacCacheMode();

  if (scale.isConnecting() || scaleLoggedGattConnecting) {
    const bool connected = serviceScaleGattConnecting();
    if (connected) {
      connectAttemptSeriesActive = false;
      const char *address = scale.address();
      const char *name = scale.localName();
      notePreferredScale(address, name);
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                           "Scale connected: %s @ %s (%s)",
                           name != nullptr && name[0] != '\0' ? name : "(unknown)",
                           address != nullptr && address[0] != '\0' ? address
                                                                     : "(no address)",
                           scale.connectedProtocolName());
      updateWorkerLinkState();
      setScaleLinkState(ScaleLinkState::CONNECTED);
      armBookooConnectBeepPolicy();
    } else {
      updateWorkerLinkState();
    }
    return;
  }

  if (scale.isScanning()) {
    feedOrTripCurrentTaskWatchdog();
    const bool connected = scale.pollScan();
    char seenMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
    char seenName[PREFERRED_SCALE_NAME_CAPACITY] = {};
    bool sawCompatibleAd = false;
    if (scale.takeSeenAdvertisement(seenMac, sizeof(seenMac), seenName,
                                    sizeof(seenName))) {
      noteScaleHistory(seenMac, seenName, false);
      sawCompatibleAd = true;
      scanLastAdvertAtMs = millis();
    }
    if (connected) {
      connectAttemptSeriesActive = false;
      const char *address = scale.address();
      const char *name = scale.localName();
      notePreferredScale(address, name);
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                           "Scale connected: %s @ %s (%s)",
                           name != nullptr && name[0] != '\0' ? name : "(unknown)",
                           address != nullptr && address[0] != '\0' ? address
                                                                     : "(no address)",
                           scale.connectedProtocolName());
      updateWorkerLinkState();
      setScaleLinkState(ScaleLinkState::CONNECTED);
      armBookooConnectBeepPolicy();
      return;
    }
    if (scale.isConnecting()) {
      updateWorkerLinkState();
      return;
    }
    if (scale.isScanning()) {
      serviceScaleScanIntensity();
      if (elapsedMs(lastScanCycleMs) < SCALE_DISCOVERY_TICK_MS) {
        return;
      }
      lastScanCycleMs = millis();
      const bool directed = scale.isDirectedScan();
      const bool logAttempt =
          !connectAttemptSeriesActive ||
          elapsedMs(lastConnectLogMs) >= SCALE_CONNECT_LOG_MS;
      if (logAttempt && !sawCompatibleAd) {
        lastConnectLogMs = lastScanCycleMs;
        connectAttemptSeriesActive = true;
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_SCAN_WAITING,
                      SCALE_SCAN_WAIT_NO_ADVERT);
        if (directed) {
          serialTraceCategory(LogLevel::DEBUG, DebugCategory::SCALE,
                              "Preferred scale attempt: no advertisement");
        } else {
          serialTraceCategory(LogLevel::DEBUG, DebugCategory::SCALE,
                              "Scale name scan: no advertisement");
        }
      } else if (logAttempt && directed && sawCompatibleAd) {
        lastConnectLogMs = lastScanCycleMs;
        connectAttemptSeriesActive = true;
        addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_SCAN_WAITING,
                      SCALE_SCAN_WAIT_OTHER_SCALE);
        serialTraceCategory(LogLevel::DEBUG, DebugCategory::SCALE,
                            "Preferred scale attempt: other scale seen, waiting");
      }
      // PREFER: after grace, drop the preferred-only filter and accept any.
      if (cacheMode == ScaleMacCacheMode::PREFER && directed &&
          elapsedMs(scanSessionAtMs) >= SCALE_PREFER_FALLBACK_MS) {
        if (startScaleDiscoveryScan(nullptr, true)) {
          serialTraceCategory(LogLevel::INFO, DebugCategory::SCALE,
                              "Preferred scale not found; falling back to any scale");
          scanLastAdvertAtMs = 0;
          logScaleScanStarted(false);
        }
        return;
      }
      // HCI/GAP force-restart only when the idle scan has gone quiet.
      const bool noAdsThisSession =
          scanLastAdvertAtMs == 0 ||
          elapsedMs(scanLastAdvertAtMs) >= SCALE_SCAN_HCI_RESTART_MS;
      if (elapsedMs(scanSessionAtMs) >= SCALE_SCAN_HCI_RESTART_MS &&
          noAdsThisSession) {
        char preferredMac[PREFERRED_SCALE_MAC_CAPACITY];
        copyPreferredScaleMac(preferredMac, sizeof(preferredMac));
        const bool hasMac =
            preferredMac[0] != '\0' && validPreferredScaleMac(preferredMac);
        const bool preferWaiting =
            cacheMode != ScaleMacCacheMode::PREFER ||
            elapsedMs(scanSessionAtMs) < SCALE_PREFER_FALLBACK_MS;
        const bool useDirected =
            shouldUseDirectedScaleScan(cacheMode, hasMac, preferWaiting);
        if (startScaleDiscoveryScan(useDirected ? preferredMac : nullptr, true)) {
          scanSessionAtMs = lastScanCycleMs;
          scanLastAdvertAtMs = 0;
          logScaleScanStarted(useDirected);
        }
      }
      return;
    }

    lastScanCycleMs = millis();
    const bool finishedDirectedAttempt =
        (cacheMode == ScaleMacCacheMode::ONLY ||
         cacheMode == ScaleMacCacheMode::PREFER) &&
        hasPreferredScaleMac();

    if (finishedDirectedAttempt) {
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SCALE,
                           "Preferred scale attempt: %s",
                           scale.lastDisconnectReasonName());
    }

    const bool logAttempt =
        !connectAttemptSeriesActive ||
        elapsedMs(lastConnectLogMs) >= SCALE_CONNECT_LOG_MS;
    if (finishedDirectedAttempt) {
      lastConnectLogMs = lastScanCycleMs;
      connectAttemptSeriesActive = true;
      logScaleConnectFailed(scale.connectStepId());
    } else if (logAttempt) {
      lastConnectLogMs = lastScanCycleMs;
      connectAttemptSeriesActive = true;
      logScaleConnectFailed(scale.connectStepId());
      logScaleConnectionFailed(false);
    }
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    // Fall through and startScan on this same tick. Do not wait after a
    // connect/GATT/scan-start failure.
  }

  lastScanCycleMs = millis();
  char preferredMac[PREFERRED_SCALE_MAC_CAPACITY];
  copyPreferredScaleMac(preferredMac, sizeof(preferredMac));
  const bool hasMac =
      preferredMac[0] != '\0' && validPreferredScaleMac(preferredMac);
  // PREFER always starts directed; fallback switches mid-session.
  const bool useDirected =
      shouldUseDirectedScaleScan(cacheMode, hasMac, true);
  if (startScaleDiscoveryScan(useDirected ? preferredMac : nullptr, false)) {
    scanSessionAtMs = lastScanCycleMs;
    scanLastAdvertAtMs = 0;
    lastConnectLogMs = lastScanCycleMs;
    connectAttemptSeriesActive = true;
    if (useDirected) {
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                           "Scanning for preferred scale %s...", preferredMac);
    } else {
      serialTraceCategory(LogLevel::INFO, DebugCategory::SCALE,
                          "Scanning for any compatible scale (name scan)");
    }
    logScaleScanStarted(useDirected);
    return;
  }

  lastConnectLogMs = lastScanCycleMs;
  connectAttemptSeriesActive = true;
  logScaleConnectFailed(0);
  if (useDirected) {
    serialTraceCategory(LogLevel::WARNING, DebugCategory::SCALE,
                        "Preferred scale scan failed to start");
  } else {
    logScaleConnectionFailed(false);
  }
  updateWorkerLinkState();
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
}

void serviceScaleWorkerLink() {
  if (applyScalePreferenceReset()) {
    return;
  }
  if (!scale.isLinkUp()) {
    cancelBookooConnectBeepPolicy();
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    return;
  }
  // Ready can arrive between ticks: open the new generation before its
  // first weight is stamped, or that sample would carry the old epoch.
  if (!outboundScaleConnected.load(std::memory_order_acquire))
    updateWorkerLinkState();

  // A user can enable PREFER/ONLY while an eligible scale is already linked.
  // Adopt it on the worker task just as if this were a fresh connection.
  if (currentScaleMacCacheMode() != ScaleMacCacheMode::FIRST &&
      !hasPreferredScaleMac() && !scaleDiscoveryPaused()) {
    notePreferredScale(scale.address(), scale.localName());
  }

  bool snapshotDirty = false;
  if (scale.heartbeatRequired() &&
      !scalePowerOffBlocksGeneration(
          scaleConnectionGenerationNow())) {
    if (!scaleCommandOk(scale.heartbeat())) {
      cancelBookooConnectBeepPolicy();
      updateWorkerLinkState();
      if (!scale.isLinkUp()) return;
    }
    snapshotDirty = true;
  }

  const bool sawWeight = pollPendingScaleWeightEvent();
  if (sawWeight) {
    snapshotDirty = true;
  }
  serviceBookooConnectBeepPolicy(sawWeight);
  if (!scale.isLinkUp()) {
    cancelBookooConnectBeepPolicy();
    updateWorkerLinkState();
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    return;
  }
  if (snapshotDirty) {
    updateWorkerLinkState();
  }
}

// Component-section profiling of the worker tick, on the same wall/own-CPU
// timeline and profiler session as the control-loop phases. The own-CPU
// delta reads this task's kernel runtime counter (same lock-free contract
// as the control loop's loopTaskCpuUs).
PhaseProfiler<SCALE_WORKER_PHASE_COUNT> scaleWorkerPhaseProfiler(
    scaleWorkerPhaseNameFor);

void copyScaleWorkerPhaseProfilerSnapshot(
    PhaseProfilerSnapshot<SCALE_WORKER_PHASE_COUNT> &out) {
  scaleWorkerPhaseProfiler.copySnapshot(out);
}

static uint32_t scaleSectionStartedAtUs = 0;
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
static uint32_t scaleSectionCpuStartedUs = 0;

static uint32_t scaleWorkerTaskCpuUs() {
  TaskStatus_t status;
  vTaskGetInfo(nullptr, &status, pdFALSE, eRunning);
  return static_cast<uint32_t>(status.ulRunTimeCounter);
}
#endif

static void finishScaleSection(ScaleWorkerPhase section) {
  const uint32_t endedAtUs = micros();
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Own-CPU reads take the SMP kernel lock: only during a profiler session
  // (as finishLoopPhase); otherwise the wall time stands in.
  if (scaleWorkerPhaseProfiler.active()) {
    const uint32_t cpuEndedUs = scaleWorkerTaskCpuUs();
    scaleWorkerPhaseProfiler.record(
        section, endedAtUs - scaleSectionStartedAtUs, endedAtUs,
        cpuEndedUs - scaleSectionCpuStartedUs);
    scaleSectionCpuStartedUs = cpuEndedUs;
  } else {
    scaleWorkerPhaseProfiler.record(section, endedAtUs - scaleSectionStartedAtUs,
                                    endedAtUs);
  }
#else
  scaleWorkerPhaseProfiler.record(section, endedAtUs - scaleSectionStartedAtUs,
                                  endedAtUs);
#endif
  scaleSectionStartedAtUs = endedAtUs;
}

void scaleWorkerTask(void *) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  scaleWorkerTaskHandle.store(xTaskGetCurrentTaskHandle(),
                              std::memory_order_release);
#endif
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  uint32_t telemetryAtMs = 0;

  const bool watchdogSubscribed = subscribeCurrentTaskToWatchdog();
  if (!watchdogSubscribed) {
    reportTaskWatchdogFault();
  }

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // The native host runtime is shared by the scale central role.
  const bool runtimeReady =
      openBrewByWeightBleRuntimeStart(BLE_STACK_READY_WAIT_MS);
  bleStackReady.store(runtimeReady, std::memory_order_release);
  if (!runtimeReady) {
    addDebugEvent(DebugCategory::BLE, DebugCode::INITIALIZATION_FAILED,
                  BOOT_SUBSYSTEM_BLE);
    logEmit(LogLevel::ERROR, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_BLE, 0);
    if (watchdogSubscribed) (void)feedCurrentTaskWatchdog();
    (void)openBrewByWeightBleRuntimeStop(BLE_STACK_STOP_WAIT_MS);
    if (watchdogSubscribed && esp_task_wdt_delete(nullptr) != ESP_OK)
      reportTaskWatchdogFault();
    {
      TaskLockGuard lock(scaleWorkerTaskHandleMutex);
      scaleWorkerTaskHandle.store(nullptr, std::memory_order_release);
    }
    // Queues stay allocated for the process lifetime: setup() may already
    // have timed out, so the control loop and HTTP tasks can hold the handles.
    scaleWorkerStartupFinished.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
    return;
  }
  scaleWorkerStartupFinished.store(true, std::memory_order_release);
  if (!ensureRfCoexBt()) {
    addDebugEvent(DebugCategory::BLE, DebugCode::INITIALIZATION_FAILED,
                  BOOT_SUBSYSTEM_BLE, rfCoexLastError());
  }
  logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
          BOOT_SUBSYSTEM_BLE, 1);
  reportNimbleRuntimeHealth(true);
#endif

  // Sleep requested since the previous service: the deadline bounds lateness
  // beyond it, not the cadence itself.
  uint32_t requestedSleepMs = 0;
  for (;;) {
    // NimBLE owns HCI waits in its host task. Yield this worker for the
    // selected connected/disconnected service cadence.
    const uint32_t tickDelayMs = scaleWorkerTickDelayMs();
    requestedSleepMs += tickDelayMs;
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(tickDelayMs));

    const uint32_t nowMs = millis();
    const uint32_t executionStartedUs = micros();
    // Section maxima have no dedicated reset control; start each profiler
    // session with fresh maxima so a new session's peaks are actionable.
    static bool sectionsEnabledPreviously = false;
    const bool sectionsEnabled = taskProfiler.loopPhaseProfilingEnabled();
    if (sectionsEnabled && !sectionsEnabledPreviously) {
      scaleWorkerPhaseProfiler.requestReset();
    }
    sectionsEnabledPreviously = sectionsEnabled;
    (void)scaleWorkerPhaseProfiler.consumeReset();
    scaleWorkerPhaseProfiler.beginIteration(sectionsEnabled, executionStartedUs,
                                            millis());
    scaleSectionStartedAtUs = executionStartedUs;
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    if (scaleWorkerPhaseProfiler.active()) scaleSectionCpuStartedUs = scaleWorkerTaskCpuUs();
#endif
    static uint32_t previousServiceAtMs = 0;
    if (previousServiceAtMs != 0) {
      const uint32_t gapMs = static_cast<uint32_t>(nowMs - previousServiceAtMs);
      if (gapMs > scaleWorkerMaxGapMs.load(std::memory_order_relaxed))
        scaleWorkerMaxGapMs.store(gapMs, std::memory_order_relaxed);
      if (gapMs > requestedSleepMs + SCALE_SERVICE_DEADLINE_MS) {
        scaleWorkerDeadlineMisses.fetch_add(1, std::memory_order_relaxed);
      }
    }
    previousServiceAtMs = nowMs;
    requestedSleepMs = 0;
    static uint32_t lastBackgroundMs = 0;
    const bool backgroundDue =
        lastBackgroundMs == 0 ||
        static_cast<uint32_t>(nowMs - lastBackgroundMs) >=
            SCALE_WORKER_BACKGROUND_MS;
    // Must run every tick: pollScan() defers GAP connect by one Settle step.
    if (!syncScalePower()) {
      finishScaleSection(ScaleWorkerPhase::ADMIN);
      feedOrTripCurrentTaskWatchdog();
      requestedSleepMs = 10;
      vTaskDelay(pdMS_TO_TICKS(requestedSleepMs));
      continue;
    }
    if (backgroundDue) {
      lastBackgroundMs = nowMs;
      markScaleWorkerProgress();
    }
    // Admin BLE master switch: while off, keep the scale client idle; every
    // consumer then observes the same state as with no scale present.
    if (serviceBleMasterSwitch()) {
      syncScaleRadioCoex();
      finishScaleSection(ScaleWorkerPhase::ADMIN);
      feedOrTripCurrentTaskWatchdog();
      requestedSleepMs = SCALE_WORKER_BACKGROUND_MS;
      vTaskDelay(pdMS_TO_TICKS(requestedSleepMs));
      continue;
    }
    syncScaleRadioCoex();

    serviceScalePowerOffTimeout(nowMs);
    finishScaleSection(ScaleWorkerPhase::ADMIN);

    // Live GAP check once per tick. Packet timeouts and HCI events cover the
    // rest of the hot path via isLinkUp().
    const bool linked = scale.isConnected();
    static uint32_t lastLinkSnapshotMs = 0;
    const bool linkSnapshotDue =
        lastLinkSnapshotMs == 0 ||
        static_cast<uint32_t>(nowMs - lastLinkSnapshotMs) >=
            SCALE_LINK_SNAPSHOT_INTERVAL_MS;

    // Packet timeout / remote-drop detection must not wait behind beeps or
    // queued commands. Bookoo has no heartbeat; silence is the only watchdog.
    if (linked) {
      connectAttemptSeriesActive = false;
      serviceScaleWorkerLink();
      serviceScaleLinkRssi(nowMs);
      if (linkSnapshotDue) {
        lastLinkSnapshotMs = nowMs;
        updateWorkerLinkState();
      }
    } else if (outboundScaleConnected.load(std::memory_order_acquire)) {
      cancelBookooConnectBeepPolicy();
      updateWorkerLinkState();
      setScaleLinkState(ScaleLinkState::DISCONNECTED);
      lastLinkSnapshotMs = nowMs;
    }
    finishScaleSection(ScaleWorkerPhase::LINK);

    // GAP/GATT setup must not wait behind tare/beep/debug: connecting is not
    // linked, so those would otherwise skip advanceConnection() for a tick.
    const bool connecting =
        scale.isConnecting() || scaleLoggedGattConnecting;
    if (connecting) {
      serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                                  connectAttemptSeriesActive, scanSessionAtMs,
                                  scanLastAdvertAtMs);
      finishScaleSection(ScaleWorkerPhase::DISCOVERY);
    } else {
      if (takeScalePowerOff()) {
        executeScalePowerOffCommand();
        finishScaleSection(ScaleWorkerPhase::COMMANDS);
      } else {
        ScaleCommand command;
        if (xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE) {
          executeScaleCommand(command);
          finishScaleSection(ScaleWorkerPhase::COMMANDS);
        } else {
          uint32_t beepCycleId = 0;
          if (takeScaleBrewBeep(beepCycleId)) {
            (void)beepCycleId;
            executeScaleBeepCommand(DebugCode::SCALE_BEEP_OK,
                                    DebugCode::SCALE_BEEP_FAILED,
                                    DebugCode::SCALE_BEEP_UNSUPPORTED);
            finishScaleSection(ScaleWorkerPhase::COMMANDS);
          } else if (takeScalePaddleReturnReminderBeep()) {
            executeScaleBeepCommand(DebugCode::SCALE_PADDLE_REMINDER_BEEP_OK,
                                    DebugCode::SCALE_PADDLE_REMINDER_BEEP_FAILED,
                                    DebugCode::SCALE_PADDLE_REMINDER_BEEP_UNSUPPORTED);
            finishScaleSection(ScaleWorkerPhase::COMMANDS);
          } else if (takeScaleCompletionBeep()) {
            executeScaleBeepCommand(DebugCode::SCALE_BEEP_OK,
                                    DebugCode::SCALE_BEEP_FAILED,
                                    DebugCode::SCALE_BEEP_UNSUPPORTED);
            finishScaleSection(ScaleWorkerPhase::COMMANDS);
          } else {
            BookooDebugAction debugAction = BookooDebugAction::START;
            uint8_t debugLevel = 0;
            if (takeScaleDebugCommand(debugAction, debugLevel)) {
              executeScaleDebugCommand(debugAction, debugLevel);
              finishScaleSection(ScaleWorkerPhase::COMMANDS);
            } else if (!linked && !applyScaleDiscoveryPause()) {
              serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                                          connectAttemptSeriesActive,
                                          scanSessionAtMs, scanLastAdvertAtMs);
              finishScaleSection(ScaleWorkerPhase::DISCOVERY);
            }
          }
        }
      }
    }

    syncScaleRadioCoex();

    static uint32_t watchdogFedAtMs = 0;
    if (watchdogFedAtMs == 0 ||
        elapsedMs(watchdogFedAtMs) >= TASK_WATCHDOG_FEED_INTERVAL_MS) {
      watchdogFedAtMs = nowMs;
      if (!feedCurrentTaskWatchdog()) {
        reportTaskWatchdogFault();
      }
    }
    if (elapsedMs(telemetryAtMs) >= HEALTH_TELEMETRY_INTERVAL_MS) {
      telemetryAtMs = millis();
      scaleWorkerStackMinBytes.store(
          static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
          std::memory_order_relaxed);
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
      reportNimbleRuntimeHealth(false);
#endif
    }
    finishScaleSection(ScaleWorkerPhase::TELEMETRY);
    const uint32_t executionUs = micros() - executionStartedUs;
    if (executionUs > scaleWorkerMaxExecutionUs.load(std::memory_order_relaxed))
      scaleWorkerMaxExecutionUs.store(executionUs, std::memory_order_relaxed);
  }
}

// Task-context observer for frames the library drops before decoding into
// weights. Pure observation: the frame stays ineligible for control. The
// note keeps the frame's own reception time.
void handleScaleDroppedFrameForProfile(const ScaleDroppedFrame &frame) {
  const bool stale = frame.reason == ScaleFrameDropReason::Stale;
  scaleProfileNoteDroppedFrame(
      stale, frame.weightDecoded, frame.weightG, frame.receivedAtMs,
      frame.generation, frame.captureSequence, frame.length, millis());
}

bool initializeScaleWorker() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  if (scaleWorkerBridge.syncNetworkRf == nullptr) return false;
#endif
  scale.setDroppedFrameObserver(&handleScaleDroppedFrameForProfile);
  scale.setFrameQueuedObserver(&wakeScaleWorkerForFrame);
  scale.setLinkParamsObserver(&noteScaleLinkParams);
  scaleWorkerStartupFinished.store(false, std::memory_order_relaxed);
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  bleStackReady.store(false, std::memory_order_relaxed);
#endif
  scaleCommandQueue =
      xQueueCreate(SCALE_COMMAND_QUEUE_LENGTH, sizeof(ScaleCommand));
  scaleEventQueue = xQueueCreate(SCALE_EVENT_QUEUE_LENGTH,
                                 sizeof(ScaleEvent));
  if (scaleCommandQueue == nullptr || scaleEventQueue == nullptr) {
    if (scaleCommandQueue != nullptr) {
      vQueueDelete(scaleCommandQueue);
      scaleCommandQueue = nullptr;
    }
    if (scaleEventQueue != nullptr) {
      vQueueDelete(scaleEventQueue);
      scaleEventQueue = nullptr;
    }
    return false;
  }

  TaskHandle_t createdWorker = nullptr;
  if (xTaskCreatePinnedToCore(scaleWorkerTask, "scale_worker",
                              SCALE_WORKER_TASK_STACK_SIZE, nullptr,
                              tskIDLE_PRIORITY + SCALE_WORKER_TASK_PRIORITY_OFFSET,
                              &createdWorker, SCALE_WORKER_TASK_CORE) != pdPASS) {
    vQueueDelete(scaleCommandQueue);
    vQueueDelete(scaleEventQueue);
    scaleCommandQueue = nullptr;
    scaleEventQueue = nullptr;
    scaleWorkerTaskHandle.store(nullptr, std::memory_order_release);
    scaleWorkerStartupFinished.store(true, std::memory_order_release);
    return false;
  }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Host task stubs do not execute scaleWorkerTask(), so publish its handle.
  scaleWorkerTaskHandle.store(createdWorker, std::memory_order_release);
#endif
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // The native NimBLE host starts from this worker. Do not start OTA/Wi-Fi
  // until the controller has finished HCI reset: setup() continues on the
  // same core and network_manager shares core 0 with bleTask.
  const uint32_t bleWaitStartMs = millis();
  while (!scaleWorkerStartupFinished.load(std::memory_order_acquire) &&
         elapsedMs(bleWaitStartMs) < BLE_STACK_READY_WAIT_MS) {
    feedOrTripCurrentTaskWatchdog();
    delay(20);
  }
#else
  // Host task stubs do not execute scaleWorkerTask(). Tests inject BLE startup
  // readiness explicitly before initializeScaleWorker().
  scaleWorkerStartupFinished.store(true, std::memory_order_release);
#endif
  return scaleWorkerReady();
}

}  // namespace openbrewbyweight

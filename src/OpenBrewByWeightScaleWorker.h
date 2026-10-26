#pragma once

#include "OpenBrewByWeightScaleLink.h"
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "OpenBrewByWeightTaskProfiler.h"

#include <type_traits>
#include <atomic>

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include <EspressoScaleBLE.h>
#endif

namespace openbrewbyweight {

// Worker timing. Keep identical to the values previously in openBrewByWeight.cpp.
constexpr uint32_t SCALE_CONNECT_LOG_MS = 10000;
constexpr uint32_t SCALE_PACKET_GAP_LOG_MIN_MS = 1000;
constexpr uint32_t SCALE_STREAM_GAP_MS = 250;
constexpr uint32_t SCALE_DISCOVERY_TICK_MS = 3000;
constexpr uint32_t SCALE_SCAN_HCI_RESTART_MS = 60000;
constexpr uint32_t SCALE_HUNT_RF_CLEAR_MS = 3000;

inline void bleScanHciParams(BleScanIntensity intensity, uint16_t &interval,
                             uint16_t &window) {
  switch (intensity) {
    case BleScanIntensity::AGGRESSIVE:
      interval = BLE_SCAN_AGGRESSIVE_INTERVAL;
      window = BLE_SCAN_AGGRESSIVE_WINDOW;
      return;
    case BleScanIntensity::RELAXED:
      interval = BLE_SCAN_RELAXED_INTERVAL;
      window = BLE_SCAN_RELAXED_WINDOW;
      return;
    case BleScanIntensity::BALANCED:
    default:
      interval = BLE_SCAN_BALANCED_INTERVAL;
      window = BLE_SCAN_BALANCED_WINDOW;
      return;
  }
}

void applyLiveBleScanIntensity(BleScanIntensity intensity);
// Admin BLE master switch: while disabled the scale worker holds the client
// idle (no scans, no links); the rest of the firmware sees "no scale".
void applyLiveBleEnabled(bool enabled);
bool liveBleEnabled();
// Machine-integration discovery override: AGGRESSIVE or RELAXED select that
// discovery duty while applicable; BALANCED clears it so the saved intensity
// decides. Atomic store only: safe from the Arduino control loop.
void applyLiveBleScanOverride(BleScanIntensity intensity);
BleScanIntensity liveBleScanIntensity();
BleScanIntensity discoveryScanIntensity();

constexpr uint32_t SCALE_WORKER_STALE_MS = 2000;
constexpr uint32_t SCALE_WORKER_NO_SCALE_DELAY_MS = 10;
// Linked fallback tick: the NimBLE host task wakes the worker per queued
// weight frame, so this only covers a missed notification.
constexpr uint32_t SCALE_WEIGHT_POLL_INTERVAL_MS = 10;
// Frames drained per wake; the client RX ring holds this many.
constexpr unsigned SCALE_WEIGHT_DRAIN_MAX = 16;
constexpr uint32_t SCALE_POWER_OFF_DISCONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t SCALE_WORKER_BACKGROUND_MS = 25;
constexpr uint32_t SCALE_LINK_SNAPSHOT_INTERVAL_MS = 50;
constexpr uint32_t SCALE_LINK_RSSI_SAMPLE_MS = 1000;
constexpr uint32_t SCALE_ATT_TIMEOUT_MS = 1000;
constexpr size_t SCALE_COMMAND_QUEUE_LENGTH = 12;
constexpr size_t SCALE_EVENT_QUEUE_LENGTH = 32;
constexpr uint32_t SCALE_WORKER_TASK_STACK_SIZE = 6656;
constexpr uint32_t BLE_STACK_READY_WAIT_MS = 4000;
constexpr uint32_t BLE_STACK_STOP_WAIT_MS = 1000;
constexpr int SCALE_WORKER_TASK_CORE = 1;
constexpr uint32_t HEALTH_TELEMETRY_INTERVAL_MS = 5000;

// The scale service publishes radio state through this narrow transport
// boundary instead of reaching into the NetworkService singleton. All fields
// are immutable after initializeScaleWorker() creates the task.
struct ScaleWorkerBridgeCallbacks {
  void (*notifyAcquisition)(bool held, uint32_t atMs) = nullptr;
  void (*syncNetworkRf)(bool scaleLinkOrConnecting, bool scaleConnecting,
                        bool huntWindowActive, bool communicationSilenced) = nullptr;
  // Optional, nonblocking: paces the Web UI stream behind each weight sample.
  void (*notifyWeightSample)() = nullptr;
};

static_assert(std::is_trivially_copyable<ScaleWorkerBridgeCallbacks>::value,
              "service boundaries must use trivially-copyable messages");

bool configureScaleWorkerBridge(const ScaleWorkerBridgeCallbacks &callbacks);

void scaleWorkerLoadPreferred(const char *mac, const char *name,
                              const ScaleHistoryEntry *history);
bool scaleWorkerCopyPreferredIfDirty(char *mac, char *name,
                                     ScaleHistoryEntry *history);
bool scaleWorkerClearPreferredDirty(const char *mac, const char *name,
                                   const ScaleHistoryEntry *history);
void setScaleCommandCycle(uint32_t cycleId);
bool scaleWorkerTakeConnectedEdge();

// Control publishes only the worker-owned subset of RuntimeConfig. The worker
// never reads the orchestrator's mutable runtimeConfig object directly.
void publishScaleWorkerPolicy(const RuntimeConfig &config, bool controlReady);

ScaleLinkSnapshot getScaleLinkSnapshot();
// The connection epoch alone, for callers that need nothing else.
uint32_t scaleConnectionGenerationNow();
// Nonblocking request; the scale owner clears only the maximum on its next tick.
void requestScaleGapMaxReset();
// Connected and worker-fresh: the authoritative "a scale is usable" gate.
bool scaleLinkAvailable(const ScaleLinkSnapshot &snapshot);
void setScaleLinkState(ScaleLinkState state);
void markScaleWorkerProgress();
void wakeScaleWorker();
void syncScaleSoftApRadio(bool apActive);
uint32_t scaleWorkerTickDelayMs();
void serviceScaleLinkRssi(uint32_t nowMs = millis());
bool enqueueScaleCommand(const ScaleCommand &command, bool toFront = false);
IdleTareStatus idleScaleTareStatus();
void approveScaleTareSample(const ScaleTareSample &sample);
bool claimIdleScaleTare(uint32_t requestId, uint32_t expectedPacketSequence = 0,
                        uint32_t captureBoundary = 0);
void approveIdleScaleTareSample(uint32_t requestId, uint32_t packetSequence);
// False only while an ATT write owns the request; never waits for BLE.
bool cancelIdleScaleTare(uint32_t requestId, IdleTareStatus *released = nullptr);
bool publishScaleEvent(const ScaleEvent &event, bool critical);
bool initializeScaleWorker();
void resetScaleWorkerRadioStateForHost();
bool scaleWorkerReady();
uint32_t scaleWorkerDroppedEventCount();
uint32_t scaleWorkerStackMinBytesValue();
uint32_t scaleWorkerMaxGapMsValue();
uint32_t scaleWorkerDeadlineMissCount();
uint32_t scaleWorkerMaxExecutionUsValue();
void copyScaleWorkerPhaseProfilerSnapshot(
    PhaseProfilerSnapshot<SCALE_WORKER_PHASE_COUNT> &out);

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
void setScaleWorkerBleReadyForHost(bool ready);
void setScaleWorkerTaskPresentForHost(bool present);
void setScaleWorkerStackMinBytesForHost(uint32_t bytes);
void resetScaleWorkerMetricsForHost();
#endif

void copyPreferredScaleMac(char *out, size_t capacity);
void copyPreferredScaleName(char *out, size_t capacity);
void copyScaleHistory(ScaleHistoryEntry *out);
// Successful shot tare survives link changes and a full event queue.
ScaleEvent copyScaleShotTareResult();
bool hasPreferredScaleMac();
uint32_t scaleMacCachePauseRemainingMs(uint32_t nowMs);
bool scaleDiscoveryPaused(uint32_t nowMs = millis());
void noteScaleHistory(const char *mac, const char *name, bool persist);
void selectPreferredScale(const char *mac, const char *name);
bool setScaleFriendlyName(const char *mac, const char *name);
void clearPreferredScaleCache();
void requestScalePreferenceModeReset();

bool enqueueScaleDebugCommand(BookooDebugAction action, uint8_t beepLevel);
bool scaleHasVolumeControl();

void requestScaleBrewBeep(uint32_t cycleId);
void cancelScaleBrewBeep(uint32_t cycleId);
void requestScalePaddleReturnReminderBeep();
void cancelScalePaddleReturnReminderBeep();
void requestScaleCompletionBeep();
void cancelScaleCompletionBeepMailbox();
void cancelOperationalScaleBeeps();
// Machine-link request: power the connected scale off. Dropped unless the
// link stays on the same connection generation and the protocol supports it.
void requestScalePowerOff();

// Orchestrator-owned scale BLE client.
extern EspressoScaleBLE scale;
extern QueueHandle_t scaleCommandQueue;
extern QueueHandle_t scaleEventQueue;
extern portMUX_TYPE scaleLinkMux;
extern TaskMutex scalePreferredMacMux;
extern TaskMutex scaleCriticalEventMux;
// Lock-wait accounting for the control loop's drain of critical scale
// events: the drain updates these, the task-profiler snapshot reads them.
inline std::atomic<uint32_t> &scaleCriticalEventWaitLastUsStat() {
  static std::atomic<uint32_t> value{0};
  return value;
}
inline std::atomic<uint32_t> &scaleCriticalEventWaitMaxUsStat() {
  static std::atomic<uint32_t> value{0};
  return value;
}
extern uint32_t scalePacketSequence;
extern char scalePreferredMac[PREFERRED_SCALE_MAC_CAPACITY];
extern char scalePreferredName[PREFERRED_SCALE_NAME_CAPACITY];
extern ScaleHistoryEntry scaleHistory[SCALE_HISTORY_CAPACITY];
extern uint32_t scaleHistorySeq;
extern bool scalePreferredMacDirty;
extern uint32_t scaleDiscoveryPausedUntilMs;
extern ScaleEvent scaleCriticalEvent;
extern bool scaleCriticalEventPending;
extern ScaleEvent scaleReferenceChangedEvent;
extern bool scaleReferenceChangedEventPending;
extern ScaleEvent scaleTimerStartEvent;
extern bool scaleTimerStartEventPending;
constexpr size_t SCALE_WEIGHT_EVENT_CAPACITY = 16;
extern ScaleEvent scaleWeightEvents[SCALE_WEIGHT_EVENT_CAPACITY];
extern uint8_t scaleWeightEventHead;
extern uint8_t scaleWeightEventCount;
extern uint32_t scaleWeightEventDrops;
extern std::atomic<bool> scaleWeightEventPending;
extern bool scaleBeepPending;
extern uint32_t scaleBeepCycleId;
extern bool scaleCompletionBeepPending;

}  // namespace openbrewbyweight

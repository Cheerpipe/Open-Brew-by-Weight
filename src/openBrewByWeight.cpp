/*
  Open Brew by Weight controller firmware

  An activator on ACTIVATOR_GPIO (paddle, switch, or another compatible
  mechanism) reports user intention. The stopper is the sole controller of the
  machine circuit (the intercepted brew-switch contact that makes the machine
  run) through a normally-open relay.

  Activator ON  (control closed) -> GPIO LOW
  Activator OFF (control open)   -> GPIO HIGH
  Relay de-energized             -> machine circuit open (safe state)

  LAYER: Stopper (orchestrator)
  -----------------------------
  This translation unit wires brew, cup, scale, and the generic machine façade.
  It must keep responsibilities decoupled:

  - The activator reads GPIO and translates it to UserIntent / MachineIntention.
    The stopper talks to Machine only through that abstract façade
    (machinePollIntention, machineRequestStart/Stop, machineRunState,
    MachineSense, machineSetActivatorDriveAllowed). It must NOT include
    paddle/momentary specialization headers, branch on MachineType, or know
    reed/pulse/PaddleMode internals.
  - Paddle logic stays in OpenBrewByWeightMachinePaddle*; momentary in
    OpenBrewByWeightMachineMomentary*. Run state is owned only by each type's
    *State* file. Brew / cup / scale / guards likewise never talk to switch
    or paddle details — if a guard uses paddle- or momentary-specific code,
    that is a layer bug.
  - Brew, cup, and scale do not call each other; this file polls machine once
    per loop, builds GuardInputs, pushes MachineSense and activator-drive
    permission, and applies scale/cup effects. Guards never call machine.

  Author: Felipe Urzúa <cheerpipe@gmail.com>
  https://github.com/Cheerpipe/Open-Brew-by-Weight
  Released under the GNU Affero General Public License v3.0.
*/

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#ifdef LOG_LOCAL_LEVEL
#undef LOG_LOCAL_LEVEL
#endif
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include <esp_log.h>
#endif

#include "OpenBrewByWeightPowerManagement.h"

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include "tests/open_brew_by_weight_host_stubs.h"
#else
#include <EspressoScaleBLE.h>
#include "OpenBrewByWeightBleRuntime.h"
#include <EEPROM.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>
#include <cmath>
#include "OpenBrewByWeightNetwork.h"
#include "OpenBrewByWeightOta.h"
#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightDurableStores.h"
#include "OpenBrewByWeightRecovery.h"
#endif

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightShotPredictor.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightOutboundAdmission.h"
#include "OpenBrewByWeightSettingsWriteAdmission.h"
#include "OpenBrewByWeightFirmwareMode.h"
#include "machine/OpenBrewByWeightMachineIntegration.h"
#include "machine/OpenBrewByWeightMicraTiming.h"
#include "OpenBrewByWeightIntegrationState.h"
#include "OpenBrewByWeightDebugExport.h"
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include "OpenBrewByWeightBleScanPersistence.h"
#endif
#include "OpenBrewByWeightBuzzer.h"
#include "OpenBrewByWeightAlert.h"
#include "OpenBrewByWeightSettings.h"
#include "OpenBrewByWeightAlertChannel.h"
#include "OpenBrewByWeightAlertTone.h"
#include "OpenBrewByWeightPresets.h"
#include "OpenBrewByWeightBbwLearning.h"
#include "OpenBrewByWeightBbwQuickStart.h"
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
namespace openbrewbyweight {
Print &serialCliOutput();
}
#ifdef Serial
#undef Serial
#endif
#define Serial serialCliOutput()
#endif
#include "OpenBrewByWeightSerialCli.h"
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#undef Serial
#if ARDUINO_USB_CDC_ON_BOOT
#if ARDUINO_USB_MODE
#define Serial HWCDCSerial
#else
#define Serial USBSerial
#endif
#else
#define Serial Serial0
#endif
#endif
#if defined(OPEN_BREW_BY_WEIGHT_USB_CONSOLE_OWN_HWCDC)
HWCDC openBrewByWeightUsbConsole;
#endif
#include <OpenBrewByWeightVersion.h>
#include "OpenBrewByWeightHardwareTimer.h"
#include "OpenBrewByWeightResetGuard.h"
#include "OpenBrewByWeightRecoveryGesture.h"
#include "OpenBrewByWeightResetHistoryStore.h"
#include "OpenBrewByWeightSafety.h"
#include "OpenBrewByWeightActivationStores.h"
#include "OpenBrewByWeightShotLog.h"
#include "OpenBrewByWeightCorridor.h"
#include "OpenBrewByWeightShotCurve.h"
#include "OpenBrewByWeightLastShot.h"
#include "OpenBrewByWeightTime.h"
#include "OpenBrewByWeightScheduledRestart.h"
#include "OpenBrewByWeightWatchdog.h"
#include "OpenBrewByWeightHwmon.h"
#include "OpenBrewByWeightTaskProfiler.h"
#include "OpenBrewByWeightScaleProfiler.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "OpenBrewByWeightScheduling.h"
#include "OpenBrewByWeightPsram.h"
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
#include "OpenBrewByWeightJsonArena.h"
#endif
#include "OpenBrewByWeightHardware.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <new>
#include <stdlib.h>

using namespace openbrewbyweight;

#include "OpenBrewByWeightScaleWorker.h"
#include "OpenBrewByWeightRfCoex.h"

// ---------------------------------------------------------------------------
// Control timing
// ---------------------------------------------------------------------------

constexpr uint32_t CONTROL_STATUS_REFRESH_WAIT_MS = 50;
constexpr uint32_t LOOP_NO_SCALE_DELAY_MS = 5;
// Persist blob is PSRAM BSS, not a 3 KiB stack local. Keep headroom for
// Preferences / TWDT on the flash-writing task (stack stays internal).
constexpr uint32_t SETTINGS_PERSIST_TASK_STACK_SIZE = 4096;
constexpr uint32_t SCALE_STOP_RETRY_INTERVAL_MS = 250;
constexpr uint32_t SCALE_STOP_RETRY_WINDOW_MS = 5000;
constexpr uint8_t SCALE_STOP_MAX_ATTEMPTS = 3;
constexpr uint32_t MAINTENANCE_LEASE_SETTLE_MS = 100;
constexpr uint32_t RUNTIME_PERSIST_RETRY_MS = 500;
constexpr uint32_t SHOT_STORE_PERSIST_RETRY_MS = 500;
constexpr uint32_t PLANNED_RESTART_RECORD_WAIT_MS = 30000;
constexpr uint32_t PERSIST_IO_RETRY_MAX_MS = 30000;
constexpr uint32_t RUNTIME_PERSIST_DEBOUNCE_MS = 300;
constexpr uint32_t SETTINGS_PERSIST_IDLE_WAIT_MS = 1000;
constexpr UBaseType_t PERSISTENCE_WORK_QUEUE_DEPTH = 4;
// Pin control/BLE/LED work with Arduino loopTask on APP_CPU (core 1).
// network_manager is pinned to PRO_CPU (core 0) in OpenBrewByWeightNetwork.cpp.
constexpr BaseType_t CONTROL_TASK_CORE = 1;
constexpr BaseType_t PERSISTENCE_TASK_CORE = 0;
static_assert(SCALE_WORKER_TASK_CORE == CONTROL_TASK_CORE,
              "Scale worker must stay pinned with the control/BLE task");

static_assert(SCALE_WORKER_STALE_MS > ACTIVATOR_DEBOUNCE_MS &&
                  SCALE_WORKER_STALE_MS < HARD_MAX_CIRCUIT_CLOSED_MS,
              "Scale worker stale timeout must be useful and safety-bounded");
static_assert(FLASH_IO_CONTROL_LOCK_TIMEOUT_MS * 20U <
                  TASK_WATCHDOG_TIMEOUT_MS,
              "Control flash lock must stay well under the task watchdog");
static_assert(BLE_CONNECT_TIMEOUT_MS + SCALE_ATT_TIMEOUT_MS <
                  TASK_WATCHDOG_TIMEOUT_MS,
              "GAP connect plus one ATT wait must fit under the task watchdog");
static_assert(BLE_DISCOVER_TIMEOUT_MS < TASK_WATCHDOG_TIMEOUT_MS,
              "GATT discovery must fit under the task watchdog");
static_assert(SCALE_STREAM_GAP_MS < MAX_AUTOMATION_WEIGHT_AGE_MS,
              "Stream gaps must stay below the 1 s STALE window");
static_assert(CONTROL_STATUS_REFRESH_WAIT_MS >= 20U &&
                  CONTROL_STATUS_REFRESH_WAIT_MS <= 50U,
              "GET snapshot wait must cover one control tick without hanging httpd");

// ---------------------------------------------------------------------------
// Persistent storage and the weight-trend window
// ---------------------------------------------------------------------------

constexpr size_t EEPROM_SIZE = 2;
static_assert(MAX_SHOT_DATAPOINTS * 0.1f >
                  ACCIDENTAL_TOUCH_REFERENCE_LAG_S + ACCIDENTAL_TOUCH_FLOW_WINDOW_S,
              "Shot trajectory must span the touch reference window at 10 Hz");

bool emitAlert(AlertEvent event, uint32_t cycleId = 0);
void emitImmediateCommandAlertIfBuzzer(AlertEvent event);

enum class TimerStopResult : uint8_t {
  NOT_REQUIRED,
  NOT_ATTEMPTED,
  PENDING,
  WRITE_SUCCEEDED,
  WRITE_FAILED
};

struct ShotTrajectory {
  uint32_t startMs = 0;
  float expectedEndS = NO_CUT_PREDICTION_S;
  float weight[MAX_SHOT_DATAPOINTS] = {};
  float timeS[MAX_SHOT_DATAPOINTS] = {};
  size_t datapoints = 0;
  bool automaticBrew = false;
};

struct CycleSession {
  bool active = false;
  bool automaticEnabled = false;
  bool bbwProtectionEnabled = false;
  bool startedWithScale = false;
  bool scaleWasLost = false;
  bool timerStartCommandQueued = false;
  bool remoteTimerStarted = false;
  bool remoteTimerStartSettled = false;
  bool stopTimerRequested = false;
  bool stopTimerCommandQueued = false;
  bool receivedFreshWeightInCycle = false;
  bool calibrationEligible = false;
  bool hasWeightAnchor = false;
  bool directStopPending = false;
  bool awaitingPostTareBaseline = false;
  TimerStopResult timerStopResult = TimerStopResult::NOT_REQUIRED;
  WeightControlState weightControlState = WeightControlState::INACTIVE;
  EndReason directStopReason = EndReason::NONE;
  uint8_t stopTimerAttempts = 0;
  uint8_t thresholdConfirmations = 0;
  uint8_t recoveryConfirmations = 0;
  uint32_t id = 0;
  uint32_t scaleDisconnectSequenceAtStart = 0;
  uint32_t weightSequenceAtStart = 0;
  uint32_t connectionGenerationAtStart = 0;
  uint32_t ownedConnectionGeneration = 0;
  uint32_t startedAtMs = 0;
  uint32_t circuitClosedAtMs = 0;
  // Mirror of rinseClockStartedAtMs after the stopper accepts. Tests only.
  uint32_t rinseStartedAtMs = 0;
  uint32_t postTareBaselineDeadlineMs = 0;
  uint32_t stopTimerRetryDeadlineMs = 0;
  uint32_t stopTimerLastAttemptMs = 0;
  uint32_t lastAcceptedWeightAtMs = 0;
  uint32_t lastAcceptedPacketSequence = 0;
  uint32_t lastThresholdAtMs = 0;
  uint32_t lastThresholdPacketSequence = 0;
  uint32_t lastThresholdConnectionGeneration = 0;
  uint32_t recoveryLastAtMs = 0;
  uint32_t recoveryLastPacketSequence = 0;
  uint32_t firstDropMs = 0;
  float scaleBaselineG = 0.0f;
  bool scaleBaselineReady = false;
  float lastAcceptedWeightG = 0.0f;
  float recoveryLastWeightG = 0.0f;
  bool retareEnded = false;
  bool bbwProtectionEnded = false;
  bool flowDuringRetare = false;
  uint32_t retareFlowFirstDetectedAtMs = 0;
  bool retarePerformed = false;
  uint32_t retareAtMs = 0;
  uint32_t retareRequestId = 0;
  uint32_t appliedTareRequestId = 0;
  uint32_t tareCaptureBoundary = 0;
  float preTareWeightG = NAN;  // Control-approved load the last tare zeroed.
  float softwareZeroG = 0.0f;  // Subtracted from shot weights if a tare failed.
  uint32_t tareConnectionGeneration = 0;
  bool retareDisabled = false;
  bool firstDropsBeepSent = false;
  bool firstDropObserved = false;
  bool retareEffectPending = false;
  ScaleTareSample retareZeroSample = {};
  uint32_t retareZeroCaptureSequence = 0;
  FirstFlowState firstFlow = {};
  ControlSource source = ControlSource::NONE;
  CycleConfigSnapshot config = {};
  corridor::CorridorState cut = {};  // Smooth-cut parameters and flow estimator.
  // Read-only shot-end prediction (hero tint and future consumers).
  predictor::ShotPredictor predictor = {};
  int32_t predictorQpX100 = 0;  // Preset plateau flow; 0 = no history.
  EndReason endReason = EndReason::NONE;
  bool extractionExtended = false;
  bool slowExtractionExtended = false;
  bool targetReachedEarly = false;
  uint32_t targetReachedAtMs = 0;
  bool autoToManualGuardArmed = false;
  bool autoToManualGuardEnforced = false;
  uint32_t autoToManualGuardDeadlineAtMs = 0;
  uint8_t scaleLossSource = 0; bool scaleLossBlind = false;  // deadline source; no valid flow
  uint8_t activePresetId = 0;
  char activePresetName[SHOT_PRESET_NAME_CAPACITY] = {};
  uint16_t activePresetDoseDg = 0;  // Snapshot at cycle start; 0 = unknown.
  char scaleName[SCALE_FRIENDLY_NAME_MAX_LEN + 1] = {};
  bool cupRemovedPending = false;
  AccidentalTouchPhase accidentalTouchPhase = AccidentalTouchPhase::STARTUP;
  AccidentalTouchClass accidentalTouchClass = AccidentalTouchClass::OK;
  bool accidentalTouchHolding = false;
  TouchStopEvidence touchStop = {};
  uint8_t accidentalTouchPendingCount = 0;
  float accidentalTouchPendingG[ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES] = {};  // residuals
  uint32_t accidentalTouchHeldSinceMs = 0;
  float accidentalTouchEpochS = 0.0f;  // trend start: last accepted level shift
};

struct PendingShotFinalize {
  uint8_t bbwAlgorithm = static_cast<uint8_t>(BbwAlgorithm::LINEAR_EWMA);
  uint8_t bbwAlpha = 100;
  uint8_t bbwProfileVersion = BBW_PROFILE_VERSION;
  uint32_t bbwLearningGeneration = 0;
  uint32_t scaleConnectionGeneration = 0;
  bool scaleBaselineReady = false;
  bool bbwLearningApplied = false;
  bool pending = false;
  bool curveBreakPending = false;
  uint32_t curveLastSequence = 0;
  bool offsetAnalysis = false;
  bool logEligible = false;
  uint32_t endedAtMs = 0;
  uint32_t endedAtUtcSec = 0;
  int16_t endedAtOffsetMinutes = 0;
  bool endedAtHasWallTime = false;
  uint32_t dripDelayMs = DEFAULT_DRIP_DELAY_MS;
  uint32_t endedWeightSequence = 0;
  uint32_t cycleStartedAtMs = 0;
  uint32_t bootId = 0;
  uint16_t durationDs = 0;
  uint16_t firstDropDs = SHOT_LOG_METRIC_MISSING;
  uint16_t tareAtDs = SHOT_LOG_METRIC_MISSING;
  uint16_t goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  float weightOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  float scaleBaselineG = 0.0f;
  float softwareZeroG = 0.0f;
  bool startedWithScale = false;
  bool timerOnly = false;
  bool automaticBrew = false;
  StopperState finalState = StopperState::READY;
  EndReason endReason = EndReason::NONE;
  bool extractionGuardEnabled = false;
  bool extractionExtended = false;
  bool slowExtractionGuardEnabled = false;
  // Cut parameters in force for the shot (the record's snapshot).
  uint16_t fastMarginDg = DEFAULT_FAST_MARGIN_DG;
  uint16_t slowMarginDg = DEFAULT_SLOW_MARGIN_DG;
  uint32_t fastBeforeMs = DEFAULT_FAST_BEFORE_MS;
  uint32_t slowAfterMs = DEFAULT_SLOW_AFTER_MS;
  uint32_t wallMs = DEFAULT_OPERATIONAL_WALL_MS;
  uint32_t fastRampMs = 0, slowRampMs = 0;
  uint8_t fastCurve = 0, slowCurve = 0;  // corridor::RAMP_CURVES index
  int32_t rateAtStopX100 = 0;  // q-hat at the stop command; 0 = invalid.
  bool channeling = false;
  uint8_t scaleLossSource = 0;
  bool scaleLossBlind = false;
  int32_t cutStopMs = 0, cutLineDg = 0;  // exact weight cut; 0 = none latched
  uint32_t protectionMs = DEFAULT_BBW_PROTECTION_MS;
  bool lastKnownWeightValid = false;
  float lastKnownWeightG = 0.0f;
  // Plausible post-stop tail collected during the drip hold; the final
  // weight is the median of the last POST_STOP_TAIL_SAMPLE_COUNT samples.
  float tailG[POST_STOP_TAIL_SAMPLE_COUNT] = {};
  uint32_t tailAtMs[POST_STOP_TAIL_SAMPLE_COUNT] = {};
  uint8_t tailCount = 0;
  float tailMaxG = 0.0f;
  uint8_t activePresetId = 0;
  char activePresetName[SHOT_PRESET_NAME_CAPACITY] = {};
  uint16_t targetTempDeciC = 0;  // Machine target at the shot's end; 0 = none.
  uint16_t activePresetDoseDg = 0;
  char scaleName[SCALE_FRIENDLY_NAME_MAX_LEN + 1] = {};
  uint32_t cycleId = 0;
  bool retarePerformed = false;
  bool autoToManualGuardEnabled = false;
  bool autoToManualGuardEnforced = false;
  bool autoToManualGuardArmed = false;
  uint32_t autoToManualGuardRemainingMs = 0;
  bool noScaleShotGuardEnabled = false;
  bool noScaleShotGuardArmed = false;
  uint8_t noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  char scaleProtocol[20] = "none";
  ShotCurveRecord curve = {};
};

struct CycleWallTime {
  uint32_t endedAtMs = 0;
  uint32_t utcSec = 0;
  int16_t offsetMinutes = 0;
  bool available = false;
};

struct MaintenanceLease {
  bool active = false;
  bool forwarded = false;
  bool applyRuntimeOnSuccess = false;
  uint32_t id = 0;
  uint32_t startedAtMs = 0;
  WebCommand command = {};
};

// ---------------------------------------------------------------------------
// Application state and input state
// ---------------------------------------------------------------------------

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
BleScanPersistedSettings bleScanPersistedSettings;
#endif

StopperState stopperState = StopperState::REQUIRES_OFF;
ShotTrajectory shot;
CycleSession session;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS PendingShotFinalize pendingFinalize;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS PersistedLastShot homeCycleResult;
uint32_t homeCycleResolvedAtMs = 0;
RuntimeConfig runtimeConfig;

CycleWallTime captureCycleWallTime() {
  CycleWallTime result;
  result.endedAtMs = millis();
  const TimeStatusSnapshot clock = g_wallClock.snapshot(result.endedAtMs);
  if (clock.utcSec != 0) {
    result.utcSec = clock.utcSec;
    (void)timeZoneOffsetAtOrUtc(runtimeConfig.timezoneId, clock.utcSec,
                                  result.offsetMinutes);
    result.available = true;
  }
  return result;
}
uint32_t cupSettingsGeneration = 1;
uint32_t idleTareSettingsGeneration = 1;
PowerPolicy powerPolicy;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS BullseyeMelodyConfig bullseyeMelodyConfig;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS BullseyeMelodyConfig stagedBullseyeMelodyConfig;
uint32_t stagedBullseyeRequestId = 0;
TaskMutex bullseyeConfigMux;
BullseyeTracker bullseyeTracker;
LocalBuzzer localBuzzer;
ShotPresetBank presetBank;
// All-zero defaults; touched at shot start/finalize and once per publish.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS BbwLearningBank bbwLearningBank;
// Control-owned drip quick start; the settings_persist worker writes the
// staged copy, guarded by bbwQuickStartMux.
QuickStartRecord bbwQuickStart;
QuickStartRecord bbwQuickStartStaged;
bool bbwQuickStartPersistPending = false;
bool bbwQuickStartPersistFailLatched = false;  // persist worker only
TaskMutex bbwQuickStartMux;
void stageBbwQuickStart();
uint32_t shotStatisticsEpochNow();
LastCycleSummary lastCycle;
// Debug events are not a flash DMA source and are not added from an ISR.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS DebugRingBuffer debugLog;
// Curated audit latch: classified rare events only, guarded by the same mutex
// as the common ring so logEmitDetail captures both in one critical section.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS AuditLogLatch auditLog;
// Audit flush bookkeeping: the worker drains records the latch marked
// pending; the timestamp opens the 2 s batching window on the first pending
// record so short bursts coalesce into one header commit.
uint32_t auditFlushPendingSinceMs = 0;
bool auditFlushBusy = false;
// The retained ring is task-only. A priority-inheriting mutex avoids disabling
// interrupts while HTTP/CLI readers copy records from PSRAM.
TaskMutex debugLogMutex;
LogLevel serialLogLevel = LogLevel::NONE;
LogLevel ringRetainLogLevel = LogLevel::NONE;
// Working copies: NVS/partition I/O copies through internal flash scratch
// first. Safe in PSRAM BSS because putBytes/erase/write never DMA these.
// ActivationStores is the single owner of the three activation ring stores;
// the aliases below keep every existing call site and test working against
// that owner. Static-init order is safe: PSRAM BSS plus references bound to
// constant addresses.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS ActivationStores activationStores;
ShotLog &shotLog = activationStores.shotLog;
ShotCurveLog &shotCurves = activationStores.shotCurves;
HistoryLog &historyLog = activationStores.historyLog;
// Serializes complete RAM-store operations across control and NetworkService;
// every durable write remains owned by the existing store/flash path.
TaskMutex shotStoreMutex;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotCurveSampler shotCurveSampler;

void markShotCurveBreak() {
  if (session.active) shotCurveSampler.markBreak();
  else if (pendingFinalize.pending) pendingFinalize.curveBreakPending = true;
}
// Same PSRAM-safe working-copy contract as ActivationStores above: NVS I/O
// goes through the internal flash scratch and mutations hold shotStoreMutex.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS LastShotStore lastShotStore;
const PersistedLastShot &persistedLastShot = lastShotStore.get();
const PersistedLastShot &persistedLastGoodShot = lastShotStore.getGood();
bool lastShotNvsDirty = false;
bool controllerStartedPending = false;
uint32_t shotStorePersistRetryAtMs = 0;
uint32_t shotStorePersistIoRetryMs = 0;
std::atomic<uint32_t> shotStoreDirtyGeneration{0};
uint32_t shotStoreObservedDirtyGeneration = 0;

bool noScaleShotGuardArmed = true;
uint32_t noScaleShotGuardActivityAtMs = 0;
bool noScaleShotGuardScaleWasAvailable = false;
bool noScaleShotGuardHold = false;
uint32_t noScaleShotGuardHoldAtMs = 0;
bool noScaleGuardHistoryPending = false;
bool noScaleShotGuardNeedsFreshActivator = false;
bool noScaleRequireBypassReady = false;
bool noScaleRequireBypassHoldSeen = false;
uint8_t noScaleRequireBypassCycles = 0;
uint32_t noScaleRequireBypassStartedAtMs = 0;
bool noScaleRequireBypassCompletedThisLoop = false;
bool cupStartGuardHold = false;
uint32_t cupStartGuardHoldAtMs = 0;
bool machineWakePassthroughActive = false;
uint32_t machineWakeStartedAtMs = 0;
bool machineWakeGestureConsumedThisLoop = false;
MachineBackflushSnapshot backflushObservation;
MachineBackflushPermit backflushPermit;
uint32_t backflushRelayGeneration = 0, backflushSequence = 0;
const char *backflushStopReason = "none";
// Boot-scoped master switch: FULL by default, COMPATIBILITY only when the
// persisted fwmode record says so. setup() assigns it once; nothing else
// writes it (toggling persists the record and restarts instead).
uint8_t firmwareModeRaw = static_cast<uint8_t>(openbrewbyweight::FirmwareMode::FULL);

float currentWeight = 0.0f;
uint32_t currentWeightReceivedAtMs = 0;
uint32_t currentWeightSequence = 0;
uint32_t currentWeightConnectionGeneration = 0;
float observedWeight = 0.0f;
uint32_t observedWeightReceivedAtMs = 0;
uint32_t observedWeightSequence = 0;
uint32_t observedWeightConnectionGeneration = 0;
WeightStreamState weightStreamState = WeightStreamState::NO_SAMPLE;
uint32_t nextCycleId = 1;
QueueHandle_t webCommandQueue = nullptr;
// Snapshot of ring overwrites (producers serialize on debugLogMutex).
uint32_t debugLogDroppedSnapshot = 0;
bool pendingCupRemovedSettle = false;
uint32_t scaleRecoveredStaleCount = 0;
uint32_t scaleRecoveredStaleMs = 0;
bool recoverableStaleOpen = false;
uint32_t recoverableStaleStartedAtMs = 0;
uint32_t recoverableStaleDisconnectSequence = 0;
uint32_t recoverableStaleConnectionGeneration = 0;
WeightStreamState telemetryWeightStreamState = WeightStreamState::NO_SAMPLE;
bool scaleCompletionBeepScheduled = false;
// Control-owned cursors over the worker-published monotonic link counters.
// They let control observe every kind of edge without touching worker state.
uint32_t handledScaleConnectionGeneration = 0;
uint32_t handledScaleDisconnectSequence = 0;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool webhookBrewStartPending = false;
#endif

bool virtualHoldOn = false;
// One lock/version commits internal scalars and the external presentation
// payload together. Control/safety never depend on an external pointer.
ControlStatusFields publishedControlStatus;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS ControlStatusCurve publishedControlCurve;
TaskMutex controlStatusMutex;
uint32_t controlStatusVersion = 0;
ControlGateSnapshot publishedControlGate;
TaskMutex controlGateMutex;
bool controlStatusPublishRequested = false;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS RuntimeConfig publishedRuntimeConfig;
SessionQuickSettings publishedSessionQuickSettings;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotPresetBank publishedPresetBank;
TaskMutex recipeMutex;
BootCapabilities bootCapabilities;
BootState bootState = BootState::BOOTING;
bool bootDegraded = false;
MaintenanceLease maintenanceLease;
// Flag-gated command mailboxes (each *Pending/*Held flag stays internal) are
// written whole before any read, so they live in zeroed PSRAM BSS.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS WebCommand maintenanceCancellationCommand;
bool maintenanceCancellationPending = false;
// Planned ESP.restart (OTA flash, Admin, CLI) waits here while a shot is
// running. The paddle is never blocked; the restart proceeds once idle.
bool plannedRestartHeld = false;
uint32_t plannedRestartHeldAtMs = 0;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS WebCommand pendingPlannedRestart;
// Daily scheduled restart; its internal request id marks the lease it owns.
ScheduledRestartPlanner scheduledRestartPlanner;
uint32_t scheduledRestartRequestId = 0;
uint32_t scheduledRestartStepAtMs = 0;
uint32_t scheduledRestartFiredAtMs = 0;
bool resetHistoryClearHeld = false;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS WebCommand pendingResetHistoryClear;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS WebCommand controlResultCommand;
bool controlResultPending = false;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool hostOtaUpdateActive = false;
bool hostOtaImageConfirmed = true;
#endif
// Lock-free read of the OTA publication for the scheduled restart gates.
void readScheduledRestartOta(ScheduledRestartInput &in) {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  in.updateActive = hostOtaUpdateActive;
  in.imageConfirmed = hostOtaImageConfirmed;
#else
  const OtaPublishedState ota = OpenBrewByWeightOta::instance().publishedState();
  in.updateActive = ota.busy || ota.imagePending;
  in.imageConfirmed = ota.confirmed;
#endif
}
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool hostForwardAcceptedNetworkCommandSucceeds = true;
uint32_t hostForwardAcceptedNetworkCommandCalls = 0;
WebCommand hostLastForwardedNetworkCommand;
uint32_t hostNtpSyncRequestCount = 0;
bool hostRuntimePersistSucceeds = true;
// Host: no NVS layer; the quick start flush reports this and keeps the copy.
bool hostQuickStartSaveSucceeds = true;
QuickStartRecord hostLastSavedQuickStart;
uint32_t hostRuntimePersistAttempts = 0;
RuntimeConfig hostLastFlushedRuntime;
ShotPresetBank hostLastFlushedPresets;
bool hostLastFlushIncludedLive = false;
bool hostLastMaintenanceSucceeded = false;
bool hostSettingsPersistQueueCreateSucceeds = true;
bool hostSettingsPersistTaskCreateSucceeds = true;
uint32_t hostSettingsPersistRollbackDeletes = 0;
uint32_t hostPresetWebhookCount = 0;
uint32_t hostQuickSettingsWebhookCount = 0;
uint32_t hostFirstDropWebhookCount = 0;
WebhookEvent hostWebhookEndEvent;
uint32_t hostControllerStartedWebhookCount = 0;
bool hostControllerStartedWebhookSucceeds = true;
bool (*hostControllerStartedWebhookBuildGuard)() = nullptr;
WebhookEvent hostControllerStartedWebhookEvent;
uint32_t hostIpChangedWebhookCount = 0;
char hostIpChangedWebhookAddress[16] = {};
uint32_t hostActivationHistoryWebhookCount = 0;
WebhookEvent hostActivationHistoryEvent;
#endif
bool runtimePersistPending = false;
bool runtimePersistFailed = false;
struct PendingPresetPersistence {
  uint32_t requestId = 0;
  uint32_t revision = 0;
  bool notifyPresets = false;
  bool applyMachineTemperature = false;
};
PendingPresetPersistence pendingPresetPersistence;
bool quickSettingsPersistPending = false;
// Control-task owned; RAM only by design (see SessionQuickSettings).
SessionQuickSettings sessionQuickSettings;
// Revision of the newest change that must reach flash; session-only
// revisions advance runtimeConfig.revision without moving it.
uint32_t runtimePersistRevision = 0;
uint32_t runtimePersistRetryAtMs = 0;
uint32_t runtimePersistIoRetryMs = 0;
int32_t runtimePersistReasonBits = 0;
uint32_t nextInternalRequestId = 0x80000000UL;
enum class PersistenceWork : uint8_t {
  SETTINGS = 1,
  SHOT_STORES = 2,
  SCALE_PROFILE = 3,
  AUDIT_FLUSH = 4,
  AUDIT_CLEAR = 5,
};
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
OPEN_BREW_BY_WEIGHT_PSRAM_BSS SettingsPersistRequest settingsPersistRequest;
TaskMutex settingsPersistMux;
bool settingsPersistInFlight = false;
bool settingsPersistResultReady = false;
bool settingsPersistResultOk = false;
bool settingsPersistResultLockContended = false;
uint32_t settingsPersistResultRuntimeRevision = 0;
uint32_t settingsPersistResultStorageRevision = 0;
#endif
QueueHandle_t settingsPersistQueue = nullptr;
TaskHandle_t settingsPersistTaskHandle = nullptr;
bool settingsPersistenceReady = false;
ActivationStoresPersistImage *shotStorePersistImage = nullptr;
LastShotStore *lastShotPersistImage = nullptr;
TaskMutex shotStorePersistMux;
bool shotStorePersistInFlight = false;
bool shotStorePersistResultReady = false;
bool shotStorePersistResultOk = false;
bool shotStorePersistResultIoFail = false;
bool shotStorePersistImageLastShotDirty = false;
uint32_t shotStorePersistImageGeneration = 0;
uint32_t shotStorePersistResultGeneration = 0;
// Generation of the last image the worker acknowledged as persisted with no
// newer mutation behind it. While shotStoreDirtyGeneration equals it, the
// 100 Hz persistence service is in its clean common case and skips the
// shotStoreMutex dirty-check round trip; a failed or superseded persist
// leaves it behind, so retries keep flowing through the locked path.
uint32_t shotStorePersistCleanGeneration = 0;
// Staged BLE scan settings: the control loop publishes, the settings_persist
// worker owns the durable NVS write. pending/intensity/result flags are
// guarded by bleScanPersistMux. Every request id accepted since the last
// flush is covered by that combined save and reports PERSISTED in FIFO order;
// a full ring drops the oldest id. Factory reset clears all staged state.
constexpr uint8_t PENDING_BLE_SCAN_REQUEST_CAPACITY = 4;
TaskMutex bleScanPersistMux;
bool bleScanPersistPending = false;
uint8_t bleScanPersistIntensity = 0;
bool bleScanEnabledPersistPending = false;
uint8_t bleScanPersistEnabled = 1;
bool bleScanPersistResultReady = false;
bool bleScanPersistResultOk = false;
uint32_t pendingBleScanRequestIds[PENDING_BLE_SCAN_REQUEST_CAPACITY] = {};
uint8_t pendingBleScanRequestIdCount = 0;
bool bleScanPersistFailLatched = false;
uint32_t lastLoopAtMs = 0;
uint32_t lastControlHousekeepingAtMs = UINT32_MAX;
uint32_t loopMaxGapMs = 0;
uint32_t loopDeadlineMisses = 0;
uint32_t loopMaxExecutionUs = 0;
uint32_t loopIntervalGapMs = 0;
uint32_t healthIntervalMaxGapMs = 0;
// Attribution of healthIntervalMaxGapMs: loop CPU, loop-core idle, flash I/O.
uint32_t healthIntervalGapCpuUs = 0;
uint32_t healthIntervalGapIdleUs = 0;
bool healthIntervalGapFlash = false;
uint32_t loopStackMinBytes = UINT32_MAX;
constexpr uint32_t HEALTH_SAMPLE_STALE_MS = 3U * HEALTH_TELEMETRY_INTERVAL_MS;
enum class HealthProfilerRequest : uint8_t { NONE, START, STOP };
struct HealthWorkerSample {
  uint32_t version = 0;
  uint32_t sampledAtMs = 0;
  bool heapValid = false;
  bool restartRequested = false;
  HeapCapSnapshot heap = {};
  uint8_t bleRuntimeState = 0;
  int32_t bleRuntimeLastError = 0;
  int32_t bleRuntimeLastResetReason = 0;
  uint32_t bleRuntimeSyncGeneration = 0;
  uint32_t bleRuntimeResetCount = 0;
  uint32_t bleRuntimeHostStackMinBytes = UINT32_MAX;
  HwmonSnapshot hwmon = {};
};
QueueHandle_t healthSampleQueue = nullptr;
TaskHandle_t healthTaskHandle = nullptr;
std::atomic<HealthProfilerRequest> healthProfilerRequest{
    HealthProfilerRequest::NONE};
// Single-slot request consumed by the health worker's 100 ms tick; the
// profiler state machine makes repeated requests idempotent.
std::atomic<ScaleProfilerRequest> scaleProfileRequest{
    ScaleProfilerRequest::NONE};
uint32_t healthSnapshotVersion = 0;
uint32_t healthSnapshotAtMs = 0;
bool healthSnapshotValid = false;
uint32_t freeHeapBytes = 0;
uint32_t minimumFreeHeapBytes = 0;
uint32_t largestFreeHeapBlockBytes = 0;
uint32_t internalHeapAllocatedBlocks = 0;
uint32_t internalHeapFreeBlocks = 0;
uint16_t internalHeapFragmentationPermille = 0;
uint32_t psramSizeBytes = 0;
uint32_t psramFreeBytes = 0;
uint32_t psramMinimumFreeBytes = 0;
uint32_t psramLargestFreeBlockBytes = 0;
uint8_t bleRuntimeState = 0;
int32_t bleRuntimeLastError = 0;
int32_t bleRuntimeLastResetReason = 0;
uint32_t bleRuntimeSyncGeneration = 0;
uint32_t bleRuntimeResetCount = 0;
uint32_t bleRuntimeHostStackMinBytes = UINT32_MAX;
bool healthHeapAlertLatched = false;
bool healthHeapRestartLatched = false;
bool healthStackAlertLatched = false;
bool healthLoopGapAlertLatched = false;
Hwmon hwmon;
HwmonSnapshot hwmonSnapshot = {};
OPEN_BREW_BY_WEIGHT_PSRAM_BSS TaskProfilerSnapshot taskProfilerReport;
TaskProfiler taskProfiler(taskProfilerReport);
LoopPhaseProfiler loopPhaseProfiler;
// Shared wall/own-CPU timeline for loop-phase recording, kept at file scope
// (not as loop() locals) so publishControlStatus can close the status gather
// sub-phase and open the store-copy/commit sub-phases on the same clock the
// loop body uses. loopPhaseStartedAtUs == 0 means the loop has not run yet,
// so boot-time publishes skip sub-phase recording.
static uint32_t loopPhaseStartedAtUs = 0;
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
static uint32_t loopPhaseCpuStartedUs = 0;

// Own-CPU time per phase reads THIS task's kernel-accounted runtime counter
// (vTaskGetInfo with eRunning — the lock-free branch; the SMP
// ulTaskGetRunTimeCounter is not on this TU's include path — esp_timer
// µs, U32). The global runtime counter would not do: it is the same
// clock as micros() and would carry preemption. The kernel folds a
// running stint's CPU in at switch-out, so a delta is attributed to the
// phase where the fold landed; record() clamps the stored pair so
// wall − cpu stays a conservative lower bound of preemption.
static uint32_t loopTaskCpuUs() {
  TaskStatus_t status;
  vTaskGetInfo(nullptr, &status, pdFALSE, eRunning);
  return static_cast<uint32_t>(status.ulRunTimeCounter);
}

// The loop core's idle-task runtime, read the same way (a non-eInvalid state
// skips the state lookup). Read from the loop, the idle task is switched out.
static uint32_t loopCoreIdleUs() {
  static const TaskHandle_t idle =
      xTaskGetIdleTaskHandleForCore(xPortGetCoreID());
  TaskStatus_t status;
  vTaskGetInfo(idle, &status, pdFALSE, eReady);
  return static_cast<uint32_t>(status.ulRunTimeCounter);
}
#endif

static void finishLoopPhase(LoopPhase phase) {
  const uint32_t endedAtUs = micros();
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Each own-CPU read takes the SMP kernel lock: ~20 per tick only while a
  // profiler session runs. Otherwise the wall time stands in (cpu = wall
  // keeps wall - cpu a valid, zero lower bound of preemption).
  if (loopPhaseProfiler.active()) {
    const uint32_t cpuEndedUs = loopTaskCpuUs();
    loopPhaseProfiler.record(phase, endedAtUs - loopPhaseStartedAtUs, endedAtUs,
                             cpuEndedUs - loopPhaseCpuStartedUs);
    loopPhaseCpuStartedUs = cpuEndedUs;
  } else {
    loopPhaseProfiler.record(phase, endedAtUs - loopPhaseStartedAtUs, endedAtUs);
  }
#else
  loopPhaseProfiler.record(phase, endedAtUs - loopPhaseStartedAtUs, endedAtUs);
#endif
  loopPhaseStartedAtUs = endedAtUs;
}

// Lock-wait accounting for the status publish's shot-store and control-status
// acquisitions on the control task: the acquire path updates these, the
// task-profiler snapshot reads them.
std::atomic<uint32_t> shotStoreWaitLastUs{0};
std::atomic<uint32_t> shotStoreWaitMaxUs{0};
std::atomic<uint32_t> controlStatusWaitLastUs{0};
std::atomic<uint32_t> controlStatusWaitMaxUs{0};
bool platformClockReady = false;
bool persistenceReady = false;
bool firmwareInitializationComplete = false;
bool serialLogSinkInstalled = false;
bool serialLogSinkEnabled = false;
std::atomic<bool> controlCriticalForLogging{false};

#include "diagnostics/OpenBrewByWeightSerialOutput.inc"

OPEN_BREW_BY_WEIGHT_PSRAM_BSS DebugEvent serialLogDumpSnapshot[DEBUG_EVENT_CAPACITY];
size_t serialLogDumpCount = 0;
size_t serialLogDumpIndex = 0;

LogLevel currentSerialLogLevel() {
  LogLevel level;
  __atomic_load(&serialLogLevel, &level, __ATOMIC_ACQUIRE);
  return level;
}

LogLevel currentRingRetainLogLevel() {
  LogLevel level;
  __atomic_load(&ringRetainLogLevel, &level, __ATOMIC_ACQUIRE);
  return level;
}

void publishLogLevels(LogLevel serialLevel, LogLevel ringLevel) {
  __atomic_store(&ringRetainLogLevel, &ringLevel, __ATOMIC_RELEASE);
  __atomic_store(&serialLogLevel, &serialLevel, __ATOMIC_RELEASE);
}

void completeMaintenanceLease(const WebCommand &result);
void rejectWebCommand(const WebCommand &command);
void queueRuntimePersist(int32_t reasonBits);
void commitLiveRuntimeConfig(const RuntimeConfig &composed, int32_t reasonBits,
                             const RuntimeConfig *savedEdit = nullptr);
bool settingsPersistenceAvailable();
void serviceHealthThresholdAlerts(uint32_t intervalMaxGapMs);

WebhookEvent baseWebhookEvent(WebhookEventType type, uint32_t cycleId,
                              uint32_t occurredAtMs);
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
OPEN_BREW_BY_WEIGHT_PSRAM_BSS PersistedSettings persistedSettings;
OpenBrewByWeightNetwork networkManager;

bool enqueueControllerStartedWebhook() {
  if (!controllerStartedPending) return true;
  WebhookEvent started =
      baseWebhookEvent(WebhookEventType::CONTROLLER_STARTED, 0, millis());
  started.presetRevision = runtimeConfig.revision;
  if (!networkManager.enqueueWebhook(started)) return false;
  controllerStartedPending = false;
  return true;
}

void syncScaleWorkerNetworkRf(bool scaleLinkOrConnecting,
                              bool scaleConnectingNow,
                              bool huntWindowActive, bool communicationSilenced) {
  outboundScaleSetup.store(scaleConnectingNow, std::memory_order_release);
  outboundBleQuiet.store(communicationSilenced, std::memory_order_release);
  networkManager.syncScaleLinkRf(scaleLinkOrConnecting);
  networkManager.syncScaleConnectingRf(scaleConnectingNow);
  networkManager.syncScaleHuntRf(huntWindowActive);
}

void notifyScaleAcquisition(bool held, uint32_t atMs) {
  publishOutboundAcquisition(held, atMs);
  networkManager.syncScaleConnectingRf(held);
  inhibitMachineIntegrationCloud();
}
#endif

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool enqueueControllerStartedWebhook() {
  if (!controllerStartedPending) return true;
  if (!hostControllerStartedWebhookSucceeds) return false;
  if (hostControllerStartedWebhookBuildGuard != nullptr &&
      !hostControllerStartedWebhookBuildGuard()) {
    return false;
  }
  hostControllerStartedWebhookEvent =
      baseWebhookEvent(WebhookEventType::CONTROLLER_STARTED, 0, millis());
  ++hostControllerStartedWebhookCount;
  controllerStartedPending = false;
  return true;
}

#endif

// Mirrors each activation-history entry to subscribed webhook receivers.
void enqueueActivationHistoryWebhook(uint32_t id, const HistoryRecord &record) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const WebhookConfig config = networkManager.webhookConfig();
  if (!config.enabled || !config.presetChanges) return;
#endif
  WebhookEvent event =
      baseWebhookEvent(WebhookEventType::ACTIVATION_HISTORY, 0, millis());
  event.activationId = id;
  event.activationEndedAtUnixSec = record.endedAtUnixSec;
  event.activationEndedAtLocalSec = record.endedAtLocalSec;
  event.durationMs = static_cast<uint32_t>(record.durationDs) * 100U;
  event.activationType = record.type;
  event.activationHasWallTime = (record.flags & HISTORY_FLAG_WALL_TIME) != 0;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  hostActivationHistoryEvent = event;
  ++hostActivationHistoryWebhookCount;
#else
  (void)networkManager.enqueueWebhook(event);
#endif
}

void reportStationIpChange(const char *ip) {
  if (ip == nullptr || ip[0] == '\0') return;
  if (controlCriticalForLogging.load(std::memory_order_acquire)) return;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  ++hostIpChangedWebhookCount;
  copyCString(hostIpChangedWebhookAddress, sizeof(hostIpChangedWebhookAddress), ip);
#else
  WebhookEvent changed = baseWebhookEvent(WebhookEventType::IP_CHANGED, 0, millis());
  copyCString(changed.ipAddress, sizeof(changed.ipAddress), ip);
  (void)networkManager.enqueueWebhook(changed);
#endif
}

// The esp_timer callback independently opens the machine circuit at the hard limit even if the
// normal control loop is delayed or unavailable.
esp_timer_handle_t relaySafetyTimer = nullptr;
esp_timer_handle_t operationalLimitTimer = nullptr;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
DRAM_ATTR IndependentSafetyTimer independentSafetyTimer;
#else
IndependentSafetyTimer independentSafetyTimer;
#endif
portMUX_TYPE relayMux = portMUX_INITIALIZER_UNLOCKED;
bool circuitClosed = false;
bool relaySafetyTripped = false;
bool operationalLimitTripped = false;
uint32_t circuitClosedAtMs = 0;
uint32_t relayHardLimitMs = HARD_MAX_CIRCUIT_CLOSED_MS;
uint32_t relayElectricalClosedAtMs = 0, relayElectricalOpenedAtMs = 0;
RelayPurpose relayPurpose = RelayPurpose::NORMAL;
uint32_t operationalLimitAtArmMs = HARD_MAX_CIRCUIT_CLOSED_MS;
RelaySafetyState relaySafetyState = RelaySafetyState::BOOT_SAFE;
RelaySafetyFault relaySafetyFault = RelaySafetyFault::NONE;
uint32_t relaySafetyGeneration = 0;
bool relaySafetyTimersReady = false;
bool taskWatchdogReady = false;
volatile bool feedbackTransitionPending = false;
volatile bool feedbackExpectedClosed = false;
volatile uint32_t feedbackTransitionStartedAtMs = 0;
volatile bool feedbackTransitionStampPending = false;
bool safetyHeartbeatLevel = false;
uint32_t safetyHeartbeatToggledAtMs = 0;
uint32_t bootStartedAtMs = 0;
SafetyResetSnapshot safetyResetStatus;
UsbSerialEnableSource usbSerialEnableSource = UsbSerialEnableSource::OFF;

bool usbConsoleJumperPresent();
RelaySafetySnapshot getRelaySafetySnapshot();
bool relayCircuitClosed();

bool scaleConnectedLedInitialized = false;
bool lastScaleConnectedLedOn = false;
enum class ScaleConnectedLedPattern : uint8_t {
  OFF,
  SOLID,
  FAST_BLINK,
  SLOW_BLINK
};
ScaleConnectedLedPattern lastScaleConnectedLedPattern =
    ScaleConnectedLedPattern::OFF;
uint32_t lastScaleConnectedLedToggleAtMs = 0;

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

uint32_t elapsedMs(uint32_t sinceMs) {
  return static_cast<uint32_t>(millis() - sinceMs);
}

#include "diagnostics/OpenBrewByWeightSerialLog.inc"

void copyTaskProfiler(TaskProfilerSnapshot &output) {
  taskProfiler.copySnapshot(output);
  loopPhaseProfiler.copySnapshot(output.loopPhases);
  copyScaleWorkerPhaseProfilerSnapshot(output.scaleWorkerPhases);
  output.micraBackflushWaitLastUs =
      micraBackflushWaitLastUsStat().load(std::memory_order_relaxed);
  output.micraBackflushWaitMaxUs =
      micraBackflushWaitMaxUsStat().load(std::memory_order_relaxed);
  output.micraBackflushHoldLastUs =
      micraBackflushHoldLastUsStat().load(std::memory_order_relaxed);
  output.micraBackflushHoldMaxUs =
      micraBackflushHoldMaxUsStat().load(std::memory_order_relaxed);
  output.scaleEventsWaitLastUs =
      scaleCriticalEventWaitLastUsStat().load(std::memory_order_relaxed);
  output.scaleEventsWaitMaxUs =
      scaleCriticalEventWaitMaxUsStat().load(std::memory_order_relaxed);
  output.shotStoreWaitLastUs =
      shotStoreWaitLastUs.load(std::memory_order_relaxed);
  output.shotStoreWaitMaxUs = shotStoreWaitMaxUs.load(std::memory_order_relaxed);
  output.controlStatusWaitLastUs =
      controlStatusWaitLastUs.load(std::memory_order_relaxed);
  output.controlStatusWaitMaxUs =
      controlStatusWaitMaxUs.load(std::memory_order_relaxed);
}

void copyScaleProfilerStatus(ScaleProfilerStatus &output) {
  output = scaleProfiler().status(millis());
}

void requestLoopMaxReset() { loopPhaseProfiler.requestReset(); }

void copyControlStatus(ControlStatusSnapshot &output) {
  TaskLockGuard lock(controlStatusMutex);
  static_cast<ControlStatusFields &>(output) = publishedControlStatus;
  copyShotCurve(output.shotCurve, publishedControlCurve.shotCurve);
  output.snapshotStale =
      __atomic_load_n(&controlStatusPublishRequested, __ATOMIC_ACQUIRE);
}

void copyControlGate(ControlGateSnapshot &output) {
  TaskLockGuard lock(controlGateMutex);
  output = publishedControlGate;
}

void reportTaskWatchdogFault() {
  safetyEventFlags.set(SAFETY_EVENT_CRITICAL_TASK_WATCHDOG);
}

bool criticalTaskWatchdogFaulted() {
  return safetyEventFlags.isSet(SAFETY_EVENT_CRITICAL_TASK_WATCHDOG);
}

void feedOrTripCurrentTaskWatchdog() {
  if (!feedCurrentTaskWatchdog()) {
    reportTaskWatchdogFault();
  }
}

void requestSafeRestart() {
  safetyEventFlags.set(SAFETY_EVENT_SAFE_RESTART);
}

bool safeRestartPending() {
  return safetyEventFlags.isSet(SAFETY_EVENT_SAFE_RESTART);
}

bool consumeSafeRestartRequest() {
  return safetyEventFlags.consume(SAFETY_EVENT_SAFE_RESTART);
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
// The Arduino core would otherwise cancel the bootloader rollback inside
// initArduino(), before this firmware has proven it can do anything. Deferring
// hands that decision to OpenBrewByWeightNetwork, which confirms the image only
// after it has served its Web UI.
extern "C" bool verifyRollbackLater() {
  return true;
}
#endif

uint32_t currentBootId() { return activationStores.bootId(); }

void persistLastShotSnapshot(const PersistedLastShot &snapshot) {
  TaskLockGuard lock(shotStoreMutex);
  lastShotStore.advance(snapshot, runtimeConfig.bbwProtectionMs);
  lastShotNvsDirty = true;
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_relaxed);
}

RuntimeConfig effectiveRuntimeConfig() {
  return overlaySessionQuickSettings(sessionQuickSettings,
                                     composeEffectiveConfig(runtimeConfig, presetBank));
}

void requestActivePresetMachineTemperature() {
  const ShotPreset &preset = activeShotPreset(presetBank);
  requestMachineIntegrationPresetTemperature(
      preset.id, runtimeConfig.revision, preset.lineaMicraBrewTargetDeciC);
}

bool scaleAvailable() {
  return scaleLinkAvailable(getScaleLinkSnapshot());
}

uint32_t controlLoopTickDelayMs(const ScaleLinkSnapshot &link) {
  return scaleLinkAvailable(link) ? 1 : LOOP_NO_SCALE_DELAY_MS;
}

bool weightStreamIsLive(WeightStreamState state) {
  return state == WeightStreamState::FRESH ||
         state == WeightStreamState::OVERLOAD ||
         state == WeightStreamState::ANOMALOUS;
}

void noteRecoverableStaleTransition(WeightStreamState previous,
                                    WeightStreamState next,
                                    const ScaleLinkSnapshot &link,
                                    uint32_t now) {
  if (recoverableStaleOpen) {
    const bool sameLink =
        link.state == ScaleLinkState::CONNECTED &&
        link.disconnectSequence == recoverableStaleDisconnectSequence &&
        link.connectionGeneration == recoverableStaleConnectionGeneration;
    if (!sameLink) {
      recoverableStaleOpen = false;
    } else if (weightStreamIsLive(next)) {
      ++scaleRecoveredStaleCount;
      scaleRecoveredStaleMs +=
          static_cast<uint32_t>(now - recoverableStaleStartedAtMs);
      recoverableStaleOpen = false;
    }
  }
  if (!recoverableStaleOpen && next == WeightStreamState::STALE &&
      weightStreamIsLive(previous) &&
      link.state == ScaleLinkState::CONNECTED &&
      observedWeightConnectionGeneration == link.connectionGeneration) {
    recoverableStaleOpen = true;
    recoverableStaleStartedAtMs = now;
    recoverableStaleDisconnectSequence = link.disconnectSequence;
    recoverableStaleConnectionGeneration = link.connectionGeneration;
  }
}

bool currentWeightIsFresh(uint32_t now = millis(),
                          uint32_t linkGeneration = scaleConnectionGenerationNow()) {
  return currentWeightSequence > 0 && std::isfinite(currentWeight) &&
         currentWeight >= MIN_AUTOMATION_WEIGHT_G &&
         currentWeight <= MAX_AUTOMATION_WEIGHT_G &&
         (currentWeightConnectionGeneration == 0 ||
          currentWeightConnectionGeneration == linkGeneration) &&
         static_cast<int32_t>(now - currentWeightReceivedAtMs) >= 0 &&
         static_cast<uint32_t>(now - currentWeightReceivedAtMs) <=
             MAX_AUTOMATION_WEIGHT_AGE_MS;
}

bool observedWeightIsFresh(uint32_t now = millis(),
                           uint32_t linkGeneration = scaleConnectionGenerationNow()) {
  return observedWeightSequence > 0 && std::isfinite(observedWeight) &&
         fabsf(observedWeight) <= MAX_PARSED_WEIGHT_G &&
         observedWeightConnectionGeneration != 0 &&
         observedWeightConnectionGeneration == linkGeneration &&
         static_cast<int32_t>(now - observedWeightReceivedAtMs) >= 0 &&
         static_cast<uint32_t>(now - observedWeightReceivedAtMs) <=
             MAX_AUTOMATION_WEIGHT_AGE_MS;
}

WeightStreamState observedWeightStreamStateAt(uint32_t now,
                                              const ScaleLinkSnapshot &link) {
  if (observedWeightSequence == 0) return WeightStreamState::NO_SAMPLE;
  if (!observedWeightIsFresh(now, link.connectionGeneration)) return WeightStreamState::STALE;
  return weightStreamState;
}

void serviceWeightStreamTelemetry(const ScaleLinkSnapshot &link) {
  const uint32_t now = millis();
  const WeightStreamState next = observedWeightStreamStateAt(now, link);
  noteRecoverableStaleTransition(telemetryWeightStreamState, next, link, now);
  telemetryWeightStreamState = next;
}

void latchAtmCurveFromSession(uint32_t atMs) {
  if (session.hasWeightAnchor && std::isfinite(session.lastAcceptedWeightG)) {
    shotCurveSampler.latchAtm(atMs, session.lastAcceptedWeightG);
  }
}

void enforceScaleLossBackup();

void setWeightControlState(WeightControlState state) {
  if (session.weightControlState == state) {
    session.automaticEnabled = state == WeightControlState::ACTIVE;
    return;
  }
  const WeightControlState previous = session.weightControlState;
  session.weightControlState = state;
  session.touchStop = {};
  session.automaticEnabled = state == WeightControlState::ACTIVE;
  if (state == WeightControlState::SUSPENDED) {
    session.scaleWasLost = true;
    addDebugEvent(DebugCategory::SCALE,
                  DebugCode::SCALE_CONTROL_SUSPENDED,
                  static_cast<int32_t>(previous));
    scaleProfileNoteEvent(ScaleProfileEvent::CONTROL_SUSPENDED, millis(),
                          scaleConnectionGenerationNow(), 0, NAN,
                          static_cast<uint32_t>(state), 0);
    enforceScaleLossBackup();
  } else if (state == WeightControlState::ACTIVE &&
             (previous == WeightControlState::SUSPENDED ||
              previous == WeightControlState::VALIDATING)) {
    addDebugEvent(DebugCategory::SCALE,
                  DebugCode::SCALE_CONTROL_RECOVERED);
    scaleProfileNoteEvent(ScaleProfileEvent::CONTROL_RECOVERED, millis(),
                          scaleConnectionGenerationNow(), 0, NAN,
                          static_cast<uint32_t>(state), 0);
    if (session.autoToManualGuardEnforced) {
      session.autoToManualGuardEnforced = false;
      shotCurveSampler.latchAtmCleared(millis());
      addDebugEvent(DebugCategory::SCALE,
                    DebugCode::AUTO_TO_MANUAL_GUARD_CLEARED);
    }
  }
}

void resetAccidentalTouchState() {
  session.touchStop = {};
  session.accidentalTouchPhase = AccidentalTouchPhase::STARTUP;
  session.accidentalTouchClass = AccidentalTouchClass::OK;
  session.accidentalTouchHolding = false;
  session.accidentalTouchPendingCount = 0;
  session.accidentalTouchEpochS = 0.0f;
}

void resetWeightTrend() {
  if (session.active) shotCurveSampler.markBreak();
  // q-hat is invalid (no lead) until its window refills.
  shot.expectedEndS = NO_CUT_PREDICTION_S;
  session.cut.rate.reset();
  shot.datapoints = 0;
  resetAccidentalTouchState();
}

void suspendWeightControl() {
  if (session.weightControlState == WeightControlState::ACTIVE ||
      session.weightControlState == WeightControlState::VALIDATING) {
    setWeightControlState(WeightControlState::SUSPENDED);
    session.recoveryConfirmations = 0;
    session.calibrationEligible = false;
    resetWeightTrend();
  }
}

bool scaleAutomationUnavailableForSession() {
  const ScaleLinkSnapshot snapshot = getScaleLinkSnapshot();
  return !scaleLinkAvailable(snapshot) ||
         !observedWeightIsFresh(millis(), snapshot.connectionGeneration) ||
         snapshot.disconnectSequence !=
             session.scaleDisconnectSequenceAtStart;
}

bool machineRunningElapsed(uint32_t &elapsedOut);
uint32_t machineElapsedMs();
bool machineIsRunning();
bool machineRequestStart(uint32_t operationalLimitMs,
                         bool remoteActuation = false);
bool machineRequestStop();
bool machineRequestWebStop();
bool machineRequestForcedPulse();
void requestRemoteTimerStop();

uint32_t cycleShotElapsedMs() { return session.active ? machineElapsedMs() : 0U; }

float cycleElapsedSeconds() {
  return cycleShotElapsedMs() / 1000.0f;
}

void captureCycleCircuitStart() {
  const RelaySafetySnapshot startedRelay = getRelaySafetySnapshot();
  session.circuitClosedAtMs =
      startedRelay.closed ? startedRelay.closedAtMs : session.startedAtMs;
}

uint32_t endedCycleDurationMs() {
  uint32_t durationMs = machineElapsedMs();
  if (durationMs != 0U) return durationMs;
  const uint32_t startMs = session.circuitClosedAtMs != 0U
                               ? session.circuitClosedAtMs
                               : session.startedAtMs;
  return startMs != 0U ? elapsedMs(startMs) : 0U;
}

void transitionTo(StopperState nextState) {
  if (stopperState == nextState) return;

  const StopperState previousState = stopperState;
  stopperState = nextState;
  addDebugEvent(DebugCategory::STATE, DebugCode::STATE_TRANSITION,
                static_cast<int32_t>(previousState),
                static_cast<int32_t>(nextState));
}

void initializeScaleConnectedLed() {
  if (!SCALE_STATUS_LED_PRESENT) {
    scaleConnectedLedInitialized = false;
    return;
  }
  pinMode(SCALE_CONNECTED_LED_GPIO, OUTPUT);
  digitalWrite(SCALE_CONNECTED_LED_GPIO,
               SCALE_CONNECTED_LED_INACTIVE_LEVEL);
  lastScaleConnectedLedOn = false;
  lastScaleConnectedLedPattern = ScaleConnectedLedPattern::OFF;
  lastScaleConnectedLedToggleAtMs = 0;
  scaleConnectedLedInitialized = true;
}

ScaleConnectedLedPattern desiredScaleConnectedLedPattern(
    const ScaleLinkSnapshot &link) {
  if (!runtimeConfig.scaleConnectedLed) return ScaleConnectedLedPattern::OFF;
  if (link.state == ScaleLinkState::CONNECTED) {
    if (observedWeightStreamStateAt(millis(), link) == WeightStreamState::STALE) {
      return ScaleConnectedLedPattern::SLOW_BLINK;
    }
    return ScaleConnectedLedPattern::SOLID;
  }
  if (link.connecting) return ScaleConnectedLedPattern::FAST_BLINK;
  return ScaleConnectedLedPattern::OFF;
}

void serviceScaleConnectedLed(const ScaleLinkSnapshot &link = getScaleLinkSnapshot()) {
  if (!SCALE_STATUS_LED_PRESENT) return;
  const ScaleConnectedLedPattern pattern = desiredScaleConnectedLedPattern(link);
  const uint32_t now = millis();
  bool on = false;
  uint32_t periodMs = 0;
  switch (pattern) {
    case ScaleConnectedLedPattern::SOLID:
      on = true;
      break;
    case ScaleConnectedLedPattern::FAST_BLINK:
      periodMs = SCALE_LED_FAST_BLINK_MS;
      break;
    case ScaleConnectedLedPattern::SLOW_BLINK:
      periodMs = SCALE_LED_SLOW_BLINK_MS;
      break;
    case ScaleConnectedLedPattern::OFF:
    default:
      on = false;
      break;
  }

  if (periodMs != 0) {
    if (!scaleConnectedLedInitialized ||
        pattern != lastScaleConnectedLedPattern) {
      on = true;
      lastScaleConnectedLedToggleAtMs = now;
    } else if (elapsedMs(lastScaleConnectedLedToggleAtMs) >= periodMs) {
      on = !lastScaleConnectedLedOn;
      lastScaleConnectedLedToggleAtMs = now;
    } else {
      on = lastScaleConnectedLedOn;
    }
  }

  if (scaleConnectedLedInitialized && on == lastScaleConnectedLedOn &&
      pattern == lastScaleConnectedLedPattern) {
    return;
  }
  digitalWrite(SCALE_CONNECTED_LED_GPIO,
               on ? SCALE_CONNECTED_LED_ACTIVE_LEVEL
                  : SCALE_CONNECTED_LED_INACTIVE_LEVEL);
  lastScaleConnectedLedOn = on;
  lastScaleConnectedLedPattern = pattern;
  scaleConnectedLedInitialized = true;
}

AlertOutputChannel currentAlertOutputChannel() {
  return effectiveAlertOutputChannel(runtimeConfig.alertOutputChannel);
}

bool soundAlertsEnabled() {
  // Compatibility mode silences every brew/scale alert. Recovery cues bypass
  // this gate downstream (selectAlertSink serves Recovery before the mute
  // check), so the emergency gesture stays audible in both modes.
  return !runtimeConfig.soundAlertsMuted && !firmwareCompatibilityMode();
}

// These policy decisions execute only on the control task. The worker receives
// a concrete volume command and never reads runtimeConfig.
void requestBookooSilenceIfConfigured() {
  if (bookooDesiredVolume(soundAlertsEnabled(), currentAlertOutputChannel(),
                          runtimeConfig.bookooMuteOnBuzzerOnly,
                          runtimeConfig.bookooConnectBeepLevel) == 0) {
    (void)enqueueScaleDebugCommand(BookooDebugAction::VOLUME, 0);
  }
}

void requestBookooAlertVolumeRestore() {
  if (bookooDesiredVolume(soundAlertsEnabled(), currentAlertOutputChannel(),
                          runtimeConfig.bookooMuteOnBuzzerOnly,
                          runtimeConfig.bookooConnectBeepLevel) > 0) {
    (void)enqueueScaleDebugCommand(BookooDebugAction::VOLUME,
                                   runtimeConfig.bookooConnectBeepLevel);
  }
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
void serviceBootRecoverySafety();
#endif

#include "OpenBrewByWeightMachine.h"

StopperState nextStateForUserHold(const MachineIntention &intent) {
  return intent.holdActive ? StopperState::REQUIRES_OFF : StopperState::READY;
}

void appendHistoryRecord(HistoryType type, uint32_t durationMs,
                         bool noWeight, const CycleWallTime *end = nullptr);

#include "platform/OpenBrewByWeightPowerRuntime.inc"
#include "control/OpenBrewByWeightWakeGesture.inc"
#include "control/OpenBrewByWeightCycleRuntime.inc"
#include "control/OpenBrewByWeightCycleFinalize.inc"
#include "scale/OpenBrewByWeightScaleEvents.inc"
#include "control/OpenBrewByWeightControlStateMachine.inc"
#include "control/OpenBrewByWeightSettingsCallbacks.inc"
#include "persistence/OpenBrewByWeightCommandPersistence.inc"
#include "control/OpenBrewByWeightCommands.inc"
#include "diagnostics/OpenBrewByWeightDiagnostics.inc"
#include "platform/OpenBrewByWeightEntrypoints.inc"

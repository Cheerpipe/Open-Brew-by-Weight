#define OPEN_BREW_BY_WEIGHT_HOST_TEST
#define ARDUINO_ESP32S3_DEV
#define OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL 1
#ifndef OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER
#define OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER 1
#endif

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <cmath>

#include "../openBrewByWeight.cpp"
#include "../OpenBrewByWeightNetworkSetup.h"
#include "shot_curve_test_helpers.h"

namespace {

int failures = 0;
int testsRun = 0;
bool hostAutoScaleWorkerProgress = true;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __func__ << ":" << __LINE__ << ": check failed: "       \
                << #condition << "\n";                                         \
      ++failures;                                                              \
      return;                                                                  \
    }                                                                          \
  } while (false)

#define CHECK_VALUE(condition, fallback)                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __func__ << ":" << __LINE__ << ": check failed: "       \
                << #condition << "\n";                                         \
      ++failures;                                                              \
      return fallback;                                                         \
    }                                                                          \
  } while (false)

void deleteHostResources() {
  releaseSettingsPersistenceWorkerForHost();
  delete healthSampleQueue;
  healthSampleQueue = nullptr;
  healthTaskHandle = nullptr;
  delete scaleCommandQueue;
  delete scaleEventQueue;
  delete webCommandQueue;
  delete relaySafetyTimer;
  delete operationalLimitTimer;
  scaleCommandQueue = nullptr;
  scaleEventQueue = nullptr;
  webCommandQueue = nullptr;
  relaySafetyTimer = nullptr;
  operationalLimitTimer = nullptr;
  independentSafetyTimer.resetForHost();
}

// Test helpers for the smooth cut: latch the raised/lowered cut-line event and
// read the ceiling backstop threshold of the current cycle.
void latchCutLineForTest(bool raised, float weightG, uint32_t atMs) {
  (raised ? session.extractionExtended : session.slowExtractionExtended) = true;
  shotCurveSampler.latchExtended(atMs, weightG);
}
float ceilingStopThresholdForTest() {
  return (corridor::ceilingDg(session.cut.params) - session.cut.offsetDg) / 10.0f;
}

void resetHarness(bool initialPaddleOn, bool scaleConnected) {
  deleteHostResources();

  firmwareModeRaw = static_cast<uint8_t>(openbrewbyweight::FirmwareMode::FULL);
  Serial.reset();
  resetSerialCliState();

  hostMillis = 0;
  outboundShotActive.store(false);
  outboundScaleConnected.store(false);
  hostMachineCloudInhibitCount = 0;
  hostMachineShotTargetDeciC = 0;
  powerPolicy = PowerPolicy{};
  powerWebUntilMs.store(0);
  powerNetworkBusy.store(false);
  powerScaleBusy.store(false);
  powerClockError.store(0);
  powerBleError.store(0);
  powerBleSleeping.store(false);
  scalePowerInitialized = scalePowerWaking = false;
  scalePowerWakeStartedMs = 0;
  hostBtEnableError = hostBtDisableError = ESP_OK;
  hostBtSleeping = false;
  hostBtWakeRequests = hostBtDisableCalls = 0;
  powerAppliedProfile.store(PowerProfile::OFF);
  bootStartedAtMs = 0;
  usbSerialEnableSource = UsbSerialEnableSource::OFF;
  hostPinLevel.fill(HIGH);
  hostPinMode.fill(0);
  hostTrackedRelayPin = RELAY_GPIO;
  hostTrackedRelayOpenLevel = RELAY_OPEN_LEVEL;
  hostTrackedRelayClosedLevel = RELAY_CLOSED_LEVEL;
  hostRelayOpenWrites = 0;
  hostRelayClosedWrites = 0;
  hostLedcAttachCalls = 0;
  hostLedcLastFreq = 0;
  hostEspTimerCreateSucceeds = true;
  hostEspTimerStartSucceeds = true;
  hostEspTimerStopSucceeds = true;
  hostEspTimerCreateCalls = 0;
  hostEspTimerCreateFailAtCall = 0;
  hostGptimerCreateSucceeds = true;
  hostGptimerArmSucceeds = true;
  hostCircuitArmBeforeCommitHook = nullptr;
  hostTaskWatchdogOperationsSucceed = true;
  hostTaskWatchdogConfigured = false;
  hostTaskWatchdogSubscriptions = 0;
  hostTaskWatchdogFeeds = 0;
  hostTaskYieldCalls = 0;
  safetyEventFlags.clear(SAFETY_EVENT_ALL);
  hostCpuFrequencySetSucceeds = true;
  EEPROM.beginSucceeds = true;
  BLE.beginSucceeds = true;
  resetSafetyResetGuardForHost();

  stopperState = StopperState::REQUIRES_OFF;
  shot = ShotTrajectory{};
  session = CycleSession{};
  resetCupPresence();
  idleTare = IdleTareRuntime{};
  workerIdleTare = IdleTareStatus{};
  setScaleCommandCycle(0);
  pendingFinalize = PendingShotFinalize{};
  homeCycleResult = PersistedLastShot{};
  homeCycleResolvedAtMs = 0;
  bullseyeTracker.clear();
  bullseyeMelodyConfig = BullseyeMelodyConfig{};
  stagedBullseyeMelodyConfig = BullseyeMelodyConfig{};
  stagedBullseyeRequestId = 0;
  pendingScaleTimerStop = PendingScaleTimerStop{};
  runtimeConfig = RuntimeConfig{};
  cupSettingsGeneration = 1;
  idleTareSettingsGeneration = 1;
  bbwLearningBank = BbwLearningBank{};
  bbwQuickStart = QuickStartRecord{};
  bbwQuickStartPersistPending = false;
  bbwQuickStartPersistFailLatched = false;
  hostQuickStartSaveSucceeds = true;
  // Legacy scenarios opt out; dedicated idle-tare cases exercise the ON default.
  runtimeConfig.autoTareOutsideBrew = false;
  runtimeConfig.rinseEnabled = true;
  // Host scenarios cover scale-path alerts unless a test sets the channel.
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  runtimeConfig.autoRetare = false;
  runtimeConfig.bbwProtectionMs = 3000;
  runtimeConfig.fastSideEnabled = false;
  runtimeConfig.slowSideEnabled = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  runtimeConfig.avoidAccidentalTouchEnabled = false;
  // Host scenarios assert immediate scale timer stop when the display is
  // already at/past circuit whole seconds; catch-up is covered by ST02–ST06.
  runtimeConfig.scaleTimerStopExtraDelayMs = 0;
  lastCycle = LastCycleSummary{};
  lastShotNvsDirty = false;
  activationStores.clearPersistFailLatches();
  controllerStartedPending = false;
  shotStorePersistRetryAtMs = 0;
  shotStorePersistIoRetryMs = 0;
  shotStoreDirtyGeneration.store(0, std::memory_order_relaxed);
  // The status publish's gather cache keys on the generation; force a fresh
  // fetch so a prior test's cache cannot serve stale data at generation 0.
  statusStoreCacheValid = false;
  // Same for the persistence fast path's clean latch; the sentinel means
  // "nothing acknowledged", so tests that create store dirtiness directly
  // (append without a wrapper bump) keep flowing through the locked path.
  shotStorePersistCleanGeneration = UINT32_MAX;
  shotStoreObservedDirtyGeneration = 0;
  shotStorePersistInFlight = false;
  shotStorePersistResultReady = false;
  shotStorePersistResultOk = false;
  shotStorePersistResultIoFail = false;
  shotStorePersistImageLastShotDirty = false;
  shotStorePersistImageGeneration = 0;
  shotStorePersistResultGeneration = 0;
  LastShotStore::setHostSaveSucceeds(true);
  lastShotStore.clear();
  g_hostFlashIoMutexAvailable = true;
  shotCurveSampler.reset(0);
  ShotLog::resetHostStorage();
  ShotLog::setHostSaveSucceeds(true);
  ShotCurveLog::resetHostStorage();
  HistoryLog::resetHostStorage();
  HistoryLog::setHostSaveSucceeds(true);
  shotLog.load();
  shotCurves.load();
  historyLog.load();
  noScaleShotGuardArmed = true;
  noScaleShotGuardActivityAtMs = 0;
  noScaleShotGuardScaleWasAvailable = false;
  noScaleShotGuardHold = false;
  noScaleShotGuardHoldAtMs = 0;
  noScaleGuardHistoryPending = false;
  noScaleShotGuardNeedsFreshActivator = false;
  resetNoScaleRequireBypassGesture();
  noScaleRequireBypassCompletedThisLoop = false;
  cupStartGuardHold = false;
  cupStartGuardHoldAtMs = 0;
  machineWakePassthroughActive = false;
  machineWakeStartedAtMs = 0;
  machineWakeGestureConsumedThisLoop = false;
  hostMachinePhysicalStartDisposition =
      MachinePhysicalStartDisposition::NORMAL;
  hostMachinePhysicalStartCount = 0;
  hostBackflushObservations = {};
  backflushObservation = {};
  backflushPermit = {};
  backflushStopReason = "none";
  relayHardLimitMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  relayPurpose = RelayPurpose::NORMAL;
  hostMachineTemperatureRequestCount = 0;
  hostMachineTemperaturePresetId = 0;
  hostMachineTemperatureGeneration = 0;
  hostMachineTemperatureTargetDeciC = 0;
  debugLog.clear();
  serialLogLevel = LogLevel::NONE;
  ringRetainLogLevel = LogLevel::INFO;
  publishedControlStatus = ControlStatusFields{};
  resetShotCurveRecord(publishedControlCurve.shotCurve);
  taskProfiler.resetForHost();
  healthProfilerRequest.store(HealthProfilerRequest::NONE,
                              std::memory_order_relaxed);
  healthSnapshotVersion = 0;
  healthSnapshotAtMs = 0;
  healthSnapshotValid = false;
  publishedControlGate = ControlGateSnapshot{};
  controlStatusPublishRequested = false;
  maintenanceLease = MaintenanceLease{};
  plannedRestartHeld = false;
  pendingPlannedRestart = WebCommand{};
  scheduledRestartPlanner = ScheduledRestartPlanner{};
  scheduledRestartRequestId = 0;
  scheduledRestartStepAtMs = 0;
  scheduledRestartFiredAtMs = 0;
  hostOtaUpdateActive = false;
  hostOtaImageConfirmed = true;
  resetHistoryClearHeld = false;
  pendingResetHistoryClear = WebCommand{};
  maintenanceCancellationCommand = WebCommand{};
  maintenanceCancellationPending = false;
  controlResultCommand = WebCommand{};
  controlResultPending = false;
  hostForwardAcceptedNetworkCommandSucceeds = true;
  hostForwardAcceptedNetworkCommandCalls = 0;
  hostLastForwardedNetworkCommand = WebCommand{};
  hostNtpSyncRequestCount = 0;
  hostRuntimePersistSucceeds = true;
  hostRuntimePersistAttempts = 0;
  hostLastFlushedRuntime = RuntimeConfig{};
  static const ShotPresetBank emptyPresetBank;
  hostLastFlushedPresets = emptyPresetBank;
  hostLastFlushIncludedLive = false;
  hostLastMaintenanceSucceeded = false;
  hostSettingsPersistQueueCreateSucceeds = true;
  hostSettingsPersistTaskCreateSucceeds = true;
  hostSettingsPersistRollbackDeletes = 0;
  hostPresetWebhookCount = 0;
  hostQuickSettingsWebhookCount = 0;
  hostFirstDropWebhookCount = 0;
  hostWebhookEndEvent = WebhookEvent{};
  hostControllerStartedWebhookCount = 0;
  hostControllerStartedWebhookSucceeds = true;
  hostControllerStartedWebhookBuildGuard = nullptr;
  hostControllerStartedWebhookEvent = WebhookEvent{};
  hostIpChangedWebhookCount = 0;
  hostIpChangedWebhookAddress[0] = '\0';
  g_wallClock.reset();
  runtimePersistPending = false;
  runtimePersistFailed = false;
  pendingPresetPersistence = {};
  quickSettingsPersistPending = false;
  sessionQuickSettings = SessionQuickSettings{};
  runtimePersistRevision = 0;
  runtimePersistRetryAtMs = 0;
  runtimePersistIoRetryMs = 0;
  runtimePersistReasonBits = 0;
  nextInternalRequestId = 0x80000000UL;
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = 0;
  currentWeightSequence = 0;
  currentWeightConnectionGeneration = 0;
  observedWeight = 0.0f;
  observedWeightReceivedAtMs = 0;
  observedWeightSequence = 0;
  observedWeightConnectionGeneration = 0;
  weightStreamState = WeightStreamState::NO_SAMPLE;
  nextCycleId = 1;
  resetScaleWorkerMetricsForHost();
  scale = EspressoScaleBLE(DEBUG);
  scale.connected = scaleConnected;
  resetScaleWorkerRadioStateForHost();
  scalePreferredMac[0] = '\0';
  scalePreferredName[0] = '\0';
  scalePreferredMacDirty = false;
  scaleDiscoveryPausedUntilMs = 0;
  applyLiveBleEnabled(true);
  scalePreferredDirectedResetGeneration = 0;
  scaleLinkState = ScaleLinkState::DISCONNECTED;
  scaleConnecting = false;
  scaleDisconnectSequence = 0;
  scaleConnectionGeneration = 0;
  scalePacketSequence = 0;
  scalePacketGaps = 0;
  scaleMaxPacketGapMs = 0;
  scaleGapMaxResetRequested.store(false, std::memory_order_relaxed);
  lastScalePacketGapLogMs = 0;
  lastScaleWeightAtMs = 0;
  scaleWeightUpdateIntervalMs = 0;
  scaleRejectedPackets = 0;
  scaleReconnects = 0;
  scaleRecoveredStaleCount = 0;
  scaleRecoveredStaleMs = 0;
  recoverableStaleOpen = false;
  recoverableStaleStartedAtMs = 0;
  recoverableStaleDisconnectSequence = 0;
  recoverableStaleConnectionGeneration = 0;
  telemetryWeightStreamState = WeightStreamState::NO_SAMPLE;
  scaleLastDisconnectReason = 0;
  scaleLinkRssiValid = false;
  scaleLinkRssi = 0;
  lastScaleLinkRssiSampleMs = 0;
  scaleTimerValid = false;
  scaleTimerMs = 0;
  scaleTimerAgeMs = 0;
  scaleLinkFeatures = scaleFeatureSetNone();
  scaleWorkerProgressAtMs = hostMillis;
  freeHeapBytes = 0;
  minimumFreeHeapBytes = 0;
  largestFreeHeapBlockBytes = 0;
  loopStackMinBytes = UINT32_MAX;
  loopMaxGapMs = 0;
  loopIntervalGapMs = 0;
  healthIntervalMaxGapMs = 0;
  healthIntervalGapCpuUs = healthIntervalGapIdleUs = 0;
  healthIntervalGapFlash = false;
  healthHeapAlertLatched = false;
  healthHeapRestartLatched = false;
  healthStackAlertLatched = false;
  healthLoopGapAlertLatched = false;
  scaleCriticalEvent = ScaleEvent{};
  scaleCriticalEventPending = false;
  scaleReferenceChangedEvent = ScaleEvent{};
  scaleReferenceChangedEventPending = false;
  scaleTimerStartEvent = ScaleEvent{};
  scaleTimerStartEventPending = false;
  scaleWeightEventHead = 0;
  scaleWeightEventCount = 0;
  scaleWeightEventDrops = 0;
  scaleWeightEventPending = false;
  scaleBeepPending = false;
  scaleBeepCycleId = 0;
  scaleDebugPending = false;
  scaleDebugAction = BookooDebugAction::START;
  scaleDebugBeepLevel = 0;
  scalePaddleReturnReminderBeepPending = false;
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0
  paddleReturnReminderActive = false;
  paddleReturnReminderLastAtMs = 0;
  paddleReturnReminderStartedAtMs = 0;
  paddleReturnReminderExhausted = false;
#endif
  scaleCompletionBeepPending = false;
  scaleCompletionBeepScheduled = false;
  hostAutoScaleWorkerProgress = true;
  updateWorkerLinkState();
  {
    const ScaleLinkSnapshot link = getScaleLinkSnapshot();
    handledScaleConnectionGeneration = link.connectionGeneration;
    handledScaleDisconnectSequence = link.disconnectSequence;
  }

  rawActivatorOn = false;
  activatorOn = false;
  activatorTurnedOn = false;
  activatorTurnedOff = false;
  rawActivatorChangedAtMs = 0;
  virtualHoldOn = false;
  machineEndCycle();
  rinseClear();
  circuitClosed = false;
  relaySafetyTripped = false;
  operationalLimitTripped = false;
  circuitClosedAtMs = 0;
  operationalLimitAtArmMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  relaySafetyState = RelaySafetyState::OPEN;
  relaySafetyFault = RelaySafetyFault::NONE;
  relaySafetyGeneration = 0;
  relaySafetyTimersReady = false;
  taskWatchdogReady = configureTaskWatchdog() &&
                      subscribeCurrentTaskToWatchdog();
  feedbackTransitionPending = false;
  feedbackExpectedClosed = false;
  feedbackTransitionStartedAtMs = 0;
  feedbackTransitionStampPending = false;
  safetyHeartbeatLevel = false;
  safetyHeartbeatToggledAtMs = 0;
  platformClockReady = true;
  persistenceReady = true;
  setScaleWorkerBleReadyForHost(true);
  bootCapabilities = BootCapabilities{};
  bootCapabilities.evaluated = true;
  bootCapabilities.platformClock = true;
  bootCapabilities.relaySafetyTimers = true;
  bootCapabilities.taskWatchdog = true;
  bootCapabilities.persistentStorage = true;
  bootCapabilities.settingsLoaded = true;
  bootCapabilities.settingsPersistenceWorker = true;
  bootCapabilities.scaleWorker = true;
  bootCapabilities.webCommandQueue = true;
  bootCapabilities.network = true;
  bootCapabilities.psram = true;
  bootState = BootState::READY;
  bootDegraded = false;
  safetyResetStatus = SafetyResetSnapshot{};
  firmwareInitializationComplete = true;
  publishScaleWorkerPolicy(runtimeConfig, firmwareInitializationComplete);
  scaleConnectedLedInitialized = false;
  lastScaleConnectedLedOn = false;
  lastScaleConnectedLedPattern = ScaleConnectedLedPattern::OFF;
  lastScaleConnectedLedToggleAtMs = 0;
  lastControlHousekeepingAtMs = UINT32_MAX;

  scaleCommandQueue =
      xQueueCreate(SCALE_COMMAND_QUEUE_LENGTH, sizeof(ScaleCommand));
  scaleEventQueue =
      xQueueCreate(SCALE_EVENT_QUEUE_LENGTH, sizeof(ScaleEvent));
  webCommandQueue =
      xQueueCreate(WEB_COMMAND_QUEUE_LENGTH, sizeof(WebCommand));
  CHECK(scaleCommandQueue != nullptr);
  CHECK(scaleEventQueue != nullptr);
  CHECK(webCommandQueue != nullptr);
  CHECK(initializeSettingsPersistenceWorker());
  CHECK(initializeRelaySafetyTimer());
  relaySafetyTimersReady = true;

  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
  hostPinLevel[ACTIVATOR_GPIO] = initialPaddleOn ? ACTIVATOR_ACTIVE_LEVEL
                                              : !ACTIVATOR_ACTIVE_LEVEL;
  initializeActivatorInput();
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE != 0
  machineOnActivatorReady();
#endif
  localBuzzer.begin(BUZZER_GPIO);
  hostRelayOpenWrites = 0;
  hostRelayClosedWrites = 0;
  seedDefaultShotPresetBank(presetBank);
  ensureShotPresetBank(presetBank, runtimeConfig.retareWindowMs,
                       runtimeConfig.autoRetare);
  {
    ShotPreset &preset = mutableActiveShotPreset(presetBank);
    copyUserRecipeFromConfig(runtimeConfig, preset);
  }
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.requireCupToStart = false;
  publishRecipeState();
}

void preparePendingBbwForTest() {
  pendingFinalize.pending = true;
  const ShotPreset &preset = activeShotPreset(presetBank);
  pendingFinalize.bbwAlpha = preset.bbwEwmaAlpha;
  pendingFinalize.bbwLearningGeneration = bbwLearningBank.forPreset(
      preset.id, presetBank).generation;
  pendingFinalize.rateAtStopX100 = 2000;  // a valid rate at the cut
}

void verifySafetyInvariants() {
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  const bool compatibilityMirror = firmwareCompatibilityMode();
  const bool stateMayCloseRelay =
      stopperState == StopperState::BREW ||
      stopperState == StopperState::RINSE ||
      stopperState == StopperState::MANUAL_NO_SCALE ||
      backflushActive(stopperState) ||
      machineWakePassthroughActive || compatibilityMirror;

  if (relay.closed &&
      (!stateMayCloseRelay ||
       (!session.active && !machineWakePassthroughActive &&
        !compatibilityMirror && !backflushActive(stopperState)))) {
    std::cerr << "Safety invariant failed: machine circuit closed in "
              << stopperStateName(stopperState) << "\n";
    ++failures;
  }
  if (!machineWakePassthroughActive && !compatibilityMirror &&
      (stopperState == StopperState::READY ||
       stopperState == StopperState::REQUIRES_OFF) &&
      relay.closed) {
    std::cerr << "Safety invariant failed: safe state has machine circuit closed\n";
    ++failures;
  }
  if (OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0 &&
      machineLastIntention().stablyOff &&
      stopperState != StopperState::RINSE &&
      session.source != ControlSource::WEB && relay.closed &&
      !machineHidesPhysicalStop()) {
    std::cerr << "Safety invariant failed: stable idle has machine circuit closed\n";
    ++failures;
  }
}

void stampHostObservedWeightFresh() {
  observedWeight = currentWeight;
  observedWeightReceivedAtMs = hostMillis;
  if (observedWeightSequence == 0) {
    observedWeightSequence = currentWeightSequence != 0 ? currentWeightSequence
                                                        : 1;
  }
  const uint32_t gen = getScaleLinkSnapshot().connectionGeneration;
  observedWeightConnectionGeneration = gen != 0 ? gen : 1;
}

void runLoopAfter(uint32_t deltaMs) {
  hostMillis += deltaMs;
  if (hostAutoScaleWorkerProgress && scale.connected) {
    markScaleWorkerProgress();
    if (session.active && session.automaticEnabled) {
      currentWeightReceivedAtMs = hostMillis;
      ++currentWeightSequence;
      session.receivedFreshWeightInCycle = true;
      stampHostObservedWeightFresh();
    }
  }
  hostServiceEspTimer(relaySafetyTimer);
  hostServiceEspTimer(operationalLimitTimer);
  hostServiceEspTimer(localBuzzer.phaseTimer);
  independentSafetyTimer.serviceForHost();
  loop();
  verifySafetyInvariants();
}

void setRawPaddle(bool on) {
  hostPinLevel[ACTIVATOR_GPIO] = on ? ACTIVATOR_ACTIVE_LEVEL
                                : !ACTIVATOR_ACTIVE_LEVEL;
  if (hostAutoScaleWorkerProgress && scale.connected) {
    markScaleWorkerProgress();
  }
  loop();
  verifySafetyInvariants();
}

void setScaleConnected(bool connected) {
  scale.connected = connected;
  updateWorkerLinkState();
  processScaleLinkTransitions();
}

void publishTestScaleWorkerPolicy() {
  publishScaleWorkerPolicy(runtimeConfig, firmwareInitializationComplete);
}

uint32_t hostLastWeightEventAtMs = 0;
void publishWeight(float weight, uint32_t receivedAtMs = UINT32_MAX,
                   uint32_t generation = 0, uint32_t sequence = 0) {
  // Deliver scheduled samples at their capture time; invalid-future tests use
  // publishScaleEvent/publishPendingScaleWeightEvent directly. Default-time
  // publishes never repeat a millisecond: real scales stamp each packet, and
  // duplicate-timestamp behavior is tested with explicit equal times.
  if (receivedAtMs != UINT32_MAX && static_cast<int32_t>(receivedAtMs - hostMillis) > 0)
    hostMillis = receivedAtMs;
  else if (receivedAtMs == UINT32_MAX && hostLastWeightEventAtMs == hostMillis)
    ++hostMillis;
  hostLastWeightEventAtMs =
      receivedAtMs == UINT32_MAX ? hostMillis : receivedAtMs;
  ScaleEvent event;
  event.type = ScaleEventType::WEIGHT;
  event.receivedAtMs = receivedAtMs == UINT32_MAX ? hostMillis : receivedAtMs;
  event.connectionGeneration = generation;
  event.packetSequence = sequence;
  event.weightG = weight;
  if (session.active) event.captureSequence = ++scale.weightCaptureSequence;
  CHECK(publishScaleEvent(event, false));
  processScaleWorkerEvents();
}

void establishPostTareBaseline(float weight = 0.0f) {
  publishWeight(weight, hostMillis + 1);
}

void simulateFirstDrops(float baselineG = 0.0f, uint32_t baseSequence = 20) {
  publishWeight(baselineG + 0.35f, hostMillis + 50, 1, baseSequence);
  publishWeight(baselineG + 0.40f, hostMillis + 150, 1, baseSequence + 1);
  publishWeight(baselineG + 0.45f, hostMillis + 250, 1, baseSequence + 2);
}

void publishStableCupWeight(float weight, uint32_t baseSequence = 10) {
  const uint8_t sampleCount = runtimeConfig.retareStabilitySamples;
  const uint32_t minDurationMs = runtimeConfig.retareStabilityMinDurationMs;
  const uint32_t stepMs =
      sampleCount > 1U && minDurationMs > 0U
          ? minDurationMs / static_cast<uint32_t>(sampleCount - 1U)
          : 100U;
  const uint32_t intervalMs = stepMs > 0U ? stepMs : 100U;
  const uint32_t startedAtMs = hostMillis;
  for (uint8_t index = 0; index < sampleCount; ++index) {
    hostMillis = startedAtMs + index * intervalMs;
    publishWeight(weight + (index == 1 ? 0.1f : 0.0f),
                  hostMillis, 1, baseSequence + index);
  }
}

void seedCupPresence(float weight) {
  currentWeight = weight;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  publishStableCupWeight(weight, cupPresence.weight.sampleSequence + 1U);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
}

void reachReadyFromBoot() {
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
}

uint32_t startCycle() {
  CHECK_VALUE(stopperState == StopperState::READY, hostMillis);
  if (scale.connected) {
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
  }
  const uint32_t rawOnAtMs = hostMillis;
  setRawPaddle(true);
  CHECK_VALUE(stopperState == StopperState::READY, rawOnAtMs);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK_VALUE(stopperState == StopperState::BREW ||
                  stopperState == StopperState::MANUAL_NO_SCALE,
              rawOnAtMs);
  CHECK_VALUE(getRelaySafetySnapshot().closed, rawOnAtMs);
  if (session.automaticEnabled) {
    ScaleEvent fresh;
    fresh.type = ScaleEventType::WEIGHT;
    fresh.receivedAtMs = hostMillis;
    fresh.weightG = currentWeight;
    CHECK_VALUE(publishScaleEvent(fresh, false), rawOnAtMs);
    processScaleWorkerEvents();
  }
  return rawOnAtMs;
}

void releaseAtPhysicalDuration(uint32_t rawOnAtMs, uint32_t durationMs) {
  const uint32_t rawOffAtMs = rawOnAtMs + durationMs;
  CHECK(hostMillis <= rawOffAtMs);
  runLoopAfter(rawOffAtMs - hostMillis);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
}

void reachSessionElapsed(uint32_t elapsed) {
  const uint32_t current = elapsedMs(session.startedAtMs);
  CHECK(current <= elapsed);
  runLoopAfter(elapsed - current);
}

void waitForRetareEnded(uint32_t maxWaitMs = 15000) {
  uint32_t waited = 0;
  while (!session.retareEnded && waited < maxWaitMs) {
    runLoopAfter(25);
    waited += 25;
  }
  CHECK(session.retareEnded);
}

void waitForBbwProtectionEnded(uint32_t maxWaitMs = 15000) {
  uint32_t waited = 0;
  while (!session.bbwProtectionEnded && waited < maxWaitMs) {
    runLoopAfter(25);
    waited += 25;
  }
  CHECK(session.bbwProtectionEnded);
}

void endBbwProtectionForTests() {
  if (session.bbwProtectionEnded) {
    return;
  }
  const uint32_t target = session.config.bbwProtectionMs;
  const uint32_t current = elapsedMs(session.startedAtMs);
  if (current < target) {
    runLoopAfter(target - current);
  } else {
    loop();
  }
  waitForBbwProtectionEnded();
}

void reachBrewState() {
  if (scale.connected && session.automaticEnabled && !runtimeConfig.timerOnly &&
      session.awaitingPostTareBaseline) {
    establishPostTareBaseline();
  }
  uint32_t waited = 0;
  while (stopperState != StopperState::BREW && waited < 5000) {
    runLoopAfter(25);
    waited += 25;
  }
  CHECK(stopperState == StopperState::BREW);
  if (!runtimeConfig.timerOnly) {
    CHECK(shot.automaticBrew);
  }
}

bool executeNextScaleCommand();

void advanceToBrew() {
  (void)executeNextScaleCommand();
  if (scale.connected && session.automaticEnabled && !runtimeConfig.timerOnly &&
      session.awaitingPostTareBaseline) {
    establishPostTareBaseline();
  }
  reachBrewState();
  // Past the rinse demotion window so paddle OFF ends the shot (not rinse).
  const uint32_t elapsed = elapsedMs(session.startedAtMs);
  if (elapsed <= runtimeConfig.rinseGestureMs) {
    runLoopAfter(runtimeConfig.rinseGestureMs - elapsed + 1);
  }
}

void reachManualNoScaleState() {
  uint32_t waited = 0;
  while (stopperState != StopperState::MANUAL_NO_SCALE && waited < 5000) {
    runLoopAfter(25);
    waited += 25;
  }
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
}

size_t commandCount(ScaleCommandType type) {
  size_t count = 0;
  for (const std::vector<uint8_t> &bytes : scaleCommandQueue->items) {
    ScaleCommand command;
    std::memcpy(&command, bytes.data(), sizeof(command));
    if (command.type == type) {
      ++count;
    }
  }
  return count;
}

ScaleCommand queuedCommandAt(size_t index) {
  ScaleCommand command;
  std::memcpy(&command, scaleCommandQueue->items.at(index).data(),
              sizeof(command));
  return command;
}

void publishScaleTimer(uint32_t timerMs) {
  scale.timerValid = true;
  scale.timerMs = timerMs;
  scaleTimerValid = true;
  scaleTimerMs = timerMs;
}

bool executeNextScaleCommand() {
  ScaleCommand command;
  if (xQueueReceive(scaleCommandQueue, &command, 0) != pdTRUE) {
    return false;
  }
  markScaleWorkerProgress();
  executeScaleCommand(command);
  processScaleWorkerEvents();
  return true;
}

bool executePendingScaleBrewBeep() {
  uint32_t cycleId = 0;
  if (!takeScaleBrewBeep(cycleId)) {
    return false;
  }
  CHECK_VALUE(cycleId == session.id, false);
  markScaleWorkerProgress();
  executeScaleBeepCommand(DebugCode::SCALE_BEEP_OK,
                          DebugCode::SCALE_BEEP_FAILED,
                          DebugCode::SCALE_BEEP_UNSUPPORTED);
  return true;
}

bool executePendingScalePaddleReturnReminderBeep() {
  if (!takeScalePaddleReturnReminderBeep()) {
    return false;
  }
  markScaleWorkerProgress();
  executeScaleBeepCommand(DebugCode::SCALE_PADDLE_REMINDER_BEEP_OK,
                          DebugCode::SCALE_PADDLE_REMINDER_BEEP_FAILED,
                          DebugCode::SCALE_PADDLE_REMINDER_BEEP_UNSUPPORTED);
  return true;
}

WebCommand webControlCommand(WebCommandType type) {
  WebCommand command;
  command.type = type;
  command.requestId = 1;
  return command;
}

void finishHostMaintenance() {
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS + 1);
  CHECK(!maintenanceLease.active);
}

void t01_boot_with_paddle_off() {
  resetHarness(false, false);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  reachReadyFromBoot();
}

void t02_boot_with_activator_on() {
  resetHarness(true, true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 4);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  startCycle();
}

void startBackflushFixture(bool qualified = true, bool scalePresent = false) {
  resetHarness(false, scalePresent);
  reachReadyFromBoot();
  historyLog.clear();
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  runtimeConfig.requireCupToStart = runtimeConfig.cupProtectionEnabled = true;
  if (qualified) hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, 0);
  hostBackflushObservations.observe(MachineBackflushPhase::AWAITING, 0);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BACKFLUSH_CANDIDATE);
  CHECK(getRelaySafetySnapshot().closed && !session.active);
  CHECK(!outboundShotActive.load() && hostMachineCloudInhibitCount == 0);
  CHECK(outboundScaleConnected.load() == scalePresent);
  CHECK(scaleCommandQueue->items.empty());
  publishControlGate();
  publishControlStatus();
  CHECK(publishedControlGate.activeCycle && !publishedControlStatus.activeCycle);
  CHECK(publishedControlStatus.cycleId == 0);  // Never expose a synthetic shot.
}

void bf01_confirmed_bounds_and_final_history() {
  for (bool scalePresent : {false, true}) {
    startBackflushFixture(true, scalePresent);
    const uint32_t origin = getRelaySafetySnapshot().closedAtMs;
    runLoopAfter(10000);
    hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, backflushPermit.attempt);
    loop();
    CHECK(stopperState == StopperState::BACKFLUSH_RUNNING);
    CHECK(getRelaySafetySnapshot().hardLimitMs == 180000);
    CHECK(getRelaySafetySnapshot().closedAtMs == origin);
    runLoopAfter(120000 - elapsedMs(origin));
    CHECK(getRelaySafetySnapshot().closed);
    CHECK(!session.active && scaleCommandQueue->items.empty());
    hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, backflushPermit.attempt);
    loop();
    CHECK(!getRelaySafetySnapshot().closed);
    CHECK(stopperState == StopperState::REQUIRES_OFF && historyLog.count() == 1);
    HistoryPage page;
    historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
    CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::BACKFLUSH));
    CHECK(page.records[0].durationDs >= 1200 && page.records[0].durationDs <= 1201);
    loop();
    CHECK(historyLog.count() == 1 && !getRelaySafetySnapshot().closed);
  }
}

void bf02_unconfirmed_loss_and_ordering() {
  for (int scenario = 0; scenario < 9; ++scenario) {
    startBackflushFixture(scenario != 3);
    const auto attempt = backflushPermit.attempt;
    runLoopAfter(10000);
    if (scenario == 0) hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, attempt);
    if (scenario == 1) hostBackflushObservations.invalidate();
    if (scenario == 2) runLoopAfter(60000 - machineElapsedMs());
    if (scenario == 3 || scenario == 4)
      hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, scenario == 4 ? attempt - 1 : attempt);
    if (scenario == 5 || scenario == 8) {
      hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, attempt);
      hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, attempt);
      if (scenario == 8) hostBackflushObservations.observe(MachineBackflushPhase::AWAITING, attempt);
    }
    if (scenario == 6) {
      hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, attempt);
      hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, attempt);
    }
    if (scenario == 7) {
      hostBackflushObservations.invalidate();
      hostBackflushObservations.observe(MachineBackflushPhase::AWAITING, attempt);
    }
    loop();
    CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);
    HistoryPage page;
    historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
    CHECK(page.records[0].type == static_cast<uint8_t>(scenario == 5 || scenario == 8 ? HistoryType::BACKFLUSH : HistoryType::OTHER));
    hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, attempt);
    loop();
    CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);
  }
}

void bf03_promotion_races_and_absolute_cap() {
  for (int failure = 0; failure < 7; ++failure) {
    startBackflushFixture();
    if (failure == 0) hostGptimerArmSucceeds = false;
    if (failure == 1) hostCircuitArmBeforeCommitHook = [] { independentSafetyTimerCallback(nullptr); };
    if (failure == 2) hostCircuitArmBeforeCommitHook = [] { machineRequestStop(); };
    if (failure == 3) hostCircuitArmBeforeCommitHook = [] { hostMillis += 60000; };
    if (failure == 4) hostEspTimerStopSucceeds = false;
    if (failure == 5) hostEspTimerStartSucceeds = false;
    if (failure == 6) hostCircuitArmBeforeCommitHook = [] {
      hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, backflushPermit.attempt);
    };
    hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, backflushPermit.attempt);
    loop();
    CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);
    CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  }
  startBackflushFixture();
  const uint32_t origin = getRelaySafetySnapshot().closedAtMs;
  hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, backflushPermit.attempt);
  loop();
  runLoopAfter(180000 - elapsedMs(origin));
  CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);
  hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, 0);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 2);
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  runtimeConfig.requireCupToStart = false;
  CHECK(machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS, false));
  CHECK(getRelaySafetySnapshot().hardLimitMs == 60000);
  CHECK(!machineConfirmBackflush(getRelaySafetySnapshot().generation));
}

void bf04_deadline_boundaries_and_physical_release() {
  for (uint32_t delay : {59999U, 60000U, 60001U}) {
    startBackflushFixture();
    hostMillis += delay;
    const auto generation = getRelaySafetySnapshot().generation;
    CHECK(machineConfirmBackflush(generation) == (delay < 60000));
    if (delay < 60000) {
      CHECK(!machineConfirmBackflush(generation));  // One promotion only.
      hostMillis += 180000 - delay;
    }
    CHECK(!reassertCommandedRelayClosedPin());  // Even without timer dispatch.
    CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  }
  startBackflushFixture();
  const uint32_t start = UINT32_MAX - 40000;
  circuitClosedAtMs = hostMillis = start;
  hostMillis += 50000;
  CHECK(machineConfirmBackflush(getRelaySafetySnapshot().generation));
  hostMillis += 130000;
  independentSafetyTimerCallback(nullptr);
  CHECK(!getRelaySafetySnapshot().closed && relaySafetyTripped);

  startBackflushFixture();
  const auto attempt = backflushPermit.attempt;
  setRawPaddle(false);
  hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, attempt);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::OTHER));
}

void bf05_no_adoption_reuse_or_false_history() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  const auto original = getRelaySafetySnapshot();
  hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, 0);
  hostBackflushObservations.observe(MachineBackflushPhase::AWAITING, 0);
  hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, 0);
  loop();
  CHECK(session.active && !backflushActive(stopperState));
  CHECK(getRelaySafetySnapshot().hardLimitMs == 60000);
  CHECK(getRelaySafetySnapshot().generation == original.generation);

  startBackflushFixture();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 2);
  CHECK(historyLog.count() == 1 && !getRelaySafetySnapshot().closed);
  loop();
  setRawPaddle(true);  // The same awaiting episode is already consumed.
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 2);
  CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);

  resetHarness(false, false);
  reachReadyFromBoot();
  historyLog.clear();
  hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, 0);
  hostBackflushObservations.observe(MachineBackflushPhase::AWAITING, 0);
  hostGptimerArmSucceeds = false;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 0);
}

void bf06_normal_flow_matches_absent_backflush() {
  const auto capture = [] {
    const auto relay = getRelaySafetySnapshot();
    HistoryPage page;
    historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
    return std::make_tuple(stopperState, session.active, session.source, session.endReason,
        relay.closed, relay.hardLimitMs, relay.operationalLimitMs, relay.purpose,
        relay.generation, hostRelayOpenWrites, hostRelayClosedWrites,
        outboundShotActive.load(), hostMachineCloudInhibitCount,
        commandCount(ScaleCommandType::START_TIMER_AND_TARE),
        commandCount(ScaleCommandType::STOP_TIMER), commandCount(ScaleCommandType::TARE_ONLY),
        scale.tareCalls, scale.tareStartTimerCalls, scale.stopTimerCalls,
        historyLog.count(), historyLog.count() ? page.records[0].type : uint8_t{0},
        noScaleShotGuardHold, machineWakePassthroughActive);
  };
  // Manual, scale shot, quick rinse, missing-scale guard, wake and compatibility.
  for (int scenario = 0; scenario < 6; ++scenario) {
    std::vector<decltype(capture())> baseline;
    for (int provider = 0; provider < 4; ++provider) {
      resetHarness(false, scenario == 1 || scenario == 2);
      reachReadyFromBoot();
      historyLog.clear();
      runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(scenario == 3
          ? NoScaleBbwMode::REQUIRE_SCALE : NoScaleBbwMode::OFF);
      if (scenario == 4) hostMachinePhysicalStartDisposition = MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
      if (scenario == 5) firmwareModeRaw = static_cast<uint8_t>(FirmwareMode::COMPATIBILITY);
      if (provider) hostBackflushObservations.observe(MachineBackflushPhase::INACTIVE, 0);
      if (provider == 2) hostBackflushObservations.invalidate();  // Lost idle evidence is not cleaning.
      if (provider == 3) {
        hostBackflushObservations.observe(MachineBackflushPhase::AWAITING, 0);
        hostBackflushObservations.reset();
      }
      loop();
      for (int phase = 0; phase < 5; ++phase) {
        if (phase == 0) setRawPaddle(true);
        if (phase == 4 || (scenario == 2 && phase == 2)) setRawPaddle(false);
        runLoopAfter(phase == 1 ? (scenario == 2 ? 100 : 3000) :
            phase == 2 ? 57000 : phase == 3 ? 30000 : ACTIVATOR_DEBOUNCE_MS);
        CHECK(!backflushActive(stopperState) &&
              getRelaySafetySnapshot().hardLimitMs == (scenario == 5 ? 0U : 60000U));
        // Ordinary flow matches the absent provider even across duration cuts;
        // GPIO write counts also catch otherwise invisible re-close pulses.
        if (provider == 0) baseline.push_back(capture());
        else CHECK(capture() == baseline[phase]);
      }
    }
  }
}

void bf07_configuration_retirement_opens_and_rearms_normally() {
  for (bool confirmed : {false, true}) {
    startBackflushFixture();
    if (confirmed) {
      hostBackflushObservations.observe(MachineBackflushPhase::ACTIVE, backflushPermit.attempt);
      loop();
    }
    hostBackflushObservations.reset();
    loop();
    CHECK(!getRelaySafetySnapshot().closed && historyLog.count() == 1);
    CHECK(!hostBackflushObservations.status().busy());
    loop();
    CHECK(stopperState == StopperState::REQUIRES_OFF && !getRelaySafetySnapshot().closed);
    setRawPaddle(false);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 2);
    runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
    runtimeConfig.requireCupToStart = false;
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    CHECK(session.active && !backflushActive(stopperState));
    CHECK(getRelaySafetySnapshot().closed && getRelaySafetySnapshot().hardLimitMs == 60000);
    CHECK(getRelaySafetySnapshot().purpose == RelayPurpose::NORMAL);
  }
}

void t02b_off_wake_bypasses_brew_guards_and_records_power_on() {
  for (const NoScaleBbwMode mode : {NoScaleBbwMode::WARN_ONCE,
                                    NoScaleBbwMode::REQUIRE_SCALE}) {
    resetHarness(false, false);
    reachReadyFromBoot();
    historyLog.clear();
    runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(mode);
    runtimeConfig.cupProtectionEnabled = true;
    runtimeConfig.requireCupToStart = true;
    hostMachinePhysicalStartDisposition =
        MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
    g_wallClock.setSyncing("pool.ntp.org", hostMillis);
    g_wallClock.queueSyncFromCallback(1'800'000'000U);
    CHECK(g_wallClock.applyPendingSync(hostMillis));
    const uint32_t buzzerRequests = localBuzzer.acceptedRequests;

    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    CHECK(hostMachinePhysicalStartCount == 1);
    CHECK(machineWakePassthroughActive);
    CHECK(getRelaySafetySnapshot().closed);
    CHECK(stopperState == StopperState::READY);
    CHECK(!session.active);
    CHECK(noScaleShotGuardArmed);
    CHECK(!outboundShotActive.load());
    CHECK(hostMachineCloudInhibitCount == 0);
    CHECK(!noScaleShotGuardHold);
    CHECK(!cupStartGuardHold);
    CHECK(scaleCommandQueue->items.empty());
    CHECK(localBuzzer.acceptedRequests == buzzerRequests);
    CHECK(historyLog.count() == 0);

    runLoopAfter(runtimeConfig.rinseGestureMs + 1);
    CHECK(machineWakePassthroughActive);
    CHECK(stopperState == StopperState::READY);
    CHECK(!session.active);
    setRawPaddle(false);
    CHECK(!outboundShotActive.load());
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    CHECK(!machineWakePassthroughActive);
    CHECK(!getRelaySafetySnapshot().closed);
    CHECK(stopperState == StopperState::READY);
    CHECK(!session.active);
    CHECK(historyLog.count() == 1);
    CHECK(!outboundShotActive.load());
    CHECK(hostMachineCloudInhibitCount == 0);
    HistoryPage page;
    historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
    CHECK(page.records[0].type ==
          static_cast<uint8_t>(HistoryType::POWER_ON));
    CHECK(page.records[0].durationDs >= runtimeConfig.rinseGestureMs / 100U);
    CHECK((page.records[0].flags & HISTORY_FLAG_WALL_TIME) != 0);
    CHECK(page.records[0].endedAtUnixSec >= 1'800'000'000U);
    CHECK(scaleCommandQueue->items.empty());
  }
}

void t02c_wake_hard_limit_requires_release_before_rearming() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostMachinePhysicalStartDisposition =
      MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(machineWakePassthroughActive);
  CHECK(getRelaySafetySnapshot().closed);

  hostMillis = circuitClosedAtMs + HARD_MAX_CIRCUIT_CLOSED_MS;
  hostServiceEspTimer(relaySafetyTimer);
  loop();
  CHECK(!machineWakePassthroughActive);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type ==
        static_cast<uint8_t>(HistoryType::POWER_ON));
  const size_t closedWrites = hostRelayClosedWrites;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 2);
  CHECK(hostRelayClosedWrites == closedWrites);

  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
}

void t02d_wake_timer_arm_failure_stays_open() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostMachinePhysicalStartDisposition =
      MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
  hostGptimerArmSucceeds = false;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!machineWakePassthroughActive);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == 0);
  CHECK(historyLog.count() == 0);
}

void t02e_wake_restores_suppressed_held_paddle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostMachinePhysicalStartDisposition =
      MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
  machineActivatorDriveSuppressedThisHold = true;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(machineWakePassthroughActive);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(1);
  CHECK(getRelaySafetySnapshot().closed);
}

void t03_sustained_on_enters_brew_once() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareStartTimerCalls == 1);
  CHECK(scale.resetTimerCalls == 0);
  CHECK(scale.startTimerCalls == 0);
  CHECK(scale.tareCalls == 0);
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "tareStartTimer");
  CHECK(session.remoteTimerStarted);
  establishPostTareBaseline();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(shot.automaticBrew);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  CHECK(!scaleBeepPending);
  simulateFirstDrops();
  CHECK(session.firstDropMs != 0);
  CHECK(scaleBeepPending);
  CHECK(executePendingScaleBrewBeep());
  CHECK(scale.beepCalls == 1);
  CHECK(scale.commandLog.size() == 2);
  CHECK(scale.commandLog[1] == "beepWithoutStateChange");
  runLoopAfter(100);
  CHECK(!scaleBeepPending);
  CHECK(scale.beepCalls == 1);
  CHECK(scale.startTimerCalls == 0);
}

void t04_exact_rinse_boundary_and_duration() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);

  const uint32_t remaining =
      runtimeConfig.rinseDurationMs - elapsedMs(session.rinseStartedAtMs);
  runLoopAfter(remaining - 1);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t04b_rinse_disabled_short_on_off_is_not_rinse() {
  resetHarness(false, true);
  runtimeConfig.rinseEnabled = false;
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState != StopperState::RINSE);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t04c_rinse_demote_does_not_learn_offset() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const float originalOffset = runtimeConfig.weightOffsetG;
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(shot.automaticBrew);
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(!shot.automaticBrew);
  CHECK(!session.calibrationEligible);
  runLoopAfter(runtimeConfig.rinseDurationMs);
  CHECK(!session.active);
  CHECK(!pendingFinalize.pending);
  currentWeight = goalWeightDgToGrams(DEFAULT_GOAL_WEIGHT_DG) + 3.0f;
  currentWeightReceivedAtMs = hostMillis + 1;
  ++currentWeightSequence;
  runLoopAfter(runtimeConfig.dripDelayMs);
  CHECK(runtimeConfig.weightOffsetG == originalOffset);
}

void t04d_rinse_demote_anchors_duration_to_activation() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  // The rinse clock anchors to the circuit close, not to the classification:
  // the gesture window is already part of the configured duration.
  CHECK(session.rinseStartedAtMs >= rawOnAt);
  CHECK(session.rinseStartedAtMs == getRelaySafetySnapshot().closedAtMs);
  const uint32_t remaining =
      runtimeConfig.rinseDurationMs - elapsedMs(session.rinseStartedAtMs);
  runLoopAfter(remaining - 1);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t29_rinse_demote_begin_fail_does_not_enter_rinse() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  const uint32_t rawOffAt = rawOnAt + runtimeConfig.rinseGestureMs;
  CHECK(hostMillis <= rawOffAt);
  runLoopAfter(rawOffAt - hostMillis);
  CHECK(stopperState == StopperState::BREW);
  CHECK(setMachineCircuitClosed(false));
  hostGptimerArmSucceeds = false;
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::RELAY_SAFETY_FAILURE);
  CHECK(stopperState != StopperState::RINSE);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!rinseActuationActive);
}

void t05_release_between_rinse_and_brew_is_short_shot() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs + 1);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t06_paddle_off_during_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t07_scale_prediction_requires_release_after_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t08_on_during_rinse_is_ignored() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, 500);
  CHECK(stopperState == StopperState::RINSE);
  const uint32_t rinseStarted = session.rinseStartedAtMs;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(session.rinseStartedAtMs == rinseStarted);
}

void t09_rinse_ending_on_requires_off() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, 500);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  runLoopAfter(runtimeConfig.rinseDurationMs - elapsedMs(session.rinseStartedAtMs));
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t10_paddle_bounce_does_not_start_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS - 1);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == 0);
}

void t11_ble_loss_suspends_brew_without_late_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  setScaleConnected(false);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.timerStopResult == TimerStopResult::NOT_ATTEMPTED);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void t12_global_limit_opens_manual_and_brew_cycles() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
  CHECK(!getRelaySafetySnapshot().closed);

  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t13_reset_path_starts_with_relay_open() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);

  delete relaySafetyTimer;
  relaySafetyTimer = nullptr;
  delete operationalLimitTimer;
  operationalLimitTimer = nullptr;
  independentSafetyTimer.resetForHost();
  delete scaleCommandQueue;
  delete scaleEventQueue;
  delete webCommandQueue;
  scaleCommandQueue = nullptr;
  scaleEventQueue = nullptr;
  webCommandQueue = nullptr;
  circuitClosed = false;
  relaySafetyTripped = false;
  stopperState = StopperState::REQUIRES_OFF;
  hostRelayOpenWrites = 0;
  setup();
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  CHECK(hostRelayOpenWrites >= 1);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(getRelaySafetySnapshot().state == RelaySafetyState::OPEN);
  CHECK(getRelaySafetySnapshot().fault == RelaySafetyFault::NONE);
}

void t14_automatic_stop_stays_open_while_activator_on() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  runLoopAfter(HARD_MAX_CIRCUIT_CLOSED_MS * 2);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t15_repeated_rinse_and_brew_reset_session_state() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t firstCycle = startCycle();
  const uint32_t firstId = session.id;
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(firstCycle, 500);
  runLoopAfter(runtimeConfig.rinseDurationMs - elapsedMs(session.rinseStartedAtMs));
  CHECK(stopperState == StopperState::READY);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
  CHECK(executeNextScaleCommand());

  startCycle();
  CHECK(session.id != firstId);
  CHECK(!session.stopTimerRequested);
  CHECK(session.timerStopResult == TimerStopResult::NOT_REQUIRED);
  CHECK(session.endReason == EndReason::NONE);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(scale.tareStartTimerCalls == 2);
}

void t16_only_micra_states_are_compiled() {
  CHECK(StopperState::REQUIRES_OFF != StopperState::READY);
  CHECK(ACTIVATOR_ACTIVE_LEVEL == LOW);
  CHECK(RELAY_OPEN_LEVEL != RELAY_CLOSED_LEVEL);
}

void t17_simultaneous_global_limit_and_paddle_off_is_idempotent() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.operationalWallMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  startCycle();
  advanceToBrew();
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS - ACTIVATOR_DEBOUNCE_MS);
  setRawPaddle(false);
  hostRelayOpenWrites = 0;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
  CHECK(hostRelayOpenWrites == 1);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t18_rinse_and_short_shot_each_request_one_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, 500);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
  runLoopAfter(runtimeConfig.rinseDurationMs);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);

  resetHarness(false, true);
  reachReadyFromBoot();
  rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs + 100);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t19_manual_cycle_without_scale_has_no_timer_commands() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void t20_rinse_without_scale() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, 500);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void t21_global_limit_without_scale_rearms_after_release() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
}

void t22_scale_connection_does_not_promote_manual_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  setScaleConnected(true);
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(!session.automaticEnabled);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
}

void t23_prediction_triggers_after_bbw_protection_ends() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  shot.expectedEndS = 1.0f;
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void t24_paddle_off_during_brew_is_immediate() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t25_ble_loss_during_rinse_window_preserves_classification() {
  resetHarness(false, true);
  reachReadyFromBoot();
  uint32_t rawOnAt = startCycle();
  setScaleConnected(false);
  loop();
  releaseAtPhysicalDuration(rawOnAt, 800);
  CHECK(stopperState == StopperState::RINSE);

  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  setScaleConnected(false);
  loop();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
}

void t26_reconnected_suspended_cycle_sends_one_stop_on_release() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  setScaleConnected(false);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  setScaleConnected(true);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void t27_configuration_is_rejected_while_cycle_is_active() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.config.goalWeightDg == DEFAULT_GOAL_WEIGHT_DG);
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG + 60;
  CHECK(enqueueWebCommand(update));
  loop();
  CHECK(runtimeConfig.goalWeightDg == DEFAULT_GOAL_WEIGHT_DG);
  CHECK(session.config.goalWeightDg == DEFAULT_GOAL_WEIGHT_DG);

  releaseAtPhysicalDuration(rawActivatorChangedAtMs,
                            runtimeConfig.rinseGestureMs + 100);
  CHECK(stopperState == StopperState::READY);
  CHECK(enqueueWebCommand(update));
  loop();
  CHECK(runtimeConfig.goalWeightDg == DEFAULT_GOAL_WEIGHT_DG + 60);
  startCycle();
  CHECK(session.config.goalWeightDg == DEFAULT_GOAL_WEIGHT_DG + 60);
}

void t28_paddle_motion_cannot_cancel_or_extend_rinse() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, 400);
  const uint32_t rinseDeadline = session.rinseStartedAtMs + runtimeConfig.rinseDurationMs;

  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(session.rinseStartedAtMs + runtimeConfig.rinseDurationMs == rinseDeadline);

  runLoopAfter(rinseDeadline - hostMillis - 1);
  CHECK(stopperState == StopperState::RINSE);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
}

void r01_transient_disconnect_suspends_weight_control() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);

  // Reconnect before the control loop gets a chance to observe DISCONNECTED.
  setScaleConnected(false);
  setScaleConnected(true);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.scaleWasLost);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);

  shot.expectedEndS = 0.0f;
  endBbwProtectionForTests();
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
}

void r02_stalled_scale_worker_suspends_until_validated() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);

  hostAutoScaleWorkerProgress = false;
  runLoopAfter(SCALE_WORKER_STALE_MS + 1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(getRelaySafetySnapshot().closed);

  hostAutoScaleWorkerProgress = true;
  markScaleWorkerProgress();
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(!session.automaticEnabled);
}

void r03_non_finite_weights_cannot_corrupt_state_or_offset() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = 0;
  currentWeightSequence = 0;
  resetShotTrajectory(session.startedAtMs);
  session.receivedFreshWeightInCycle = false;
  const float originalOffset = runtimeConfig.weightOffsetG;

  ScaleEvent event;
  event.type = ScaleEventType::WEIGHT;
  event.receivedAtMs = hostMillis;
  event.weightG = std::numeric_limits<float>::quiet_NaN();
  publishScaleEvent(event, true);
  processScaleWorkerEvents();
  CHECK(currentWeightSequence == 0);
  CHECK(currentWeight == 0.0f);
  CHECK(shot.datapoints == 0);

  recordWeightSample(std::numeric_limits<float>::infinity(), hostMillis);
  CHECK(shot.datapoints == 0);

  preparePendingBbwForTest();
  pendingFinalize.offsetAnalysis = true;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.endedWeightSequence = 0;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = originalOffset;
  currentWeight = std::numeric_limits<float>::quiet_NaN();
  currentWeightSequence = 1;
  currentWeightReceivedAtMs = hostMillis + 1;
  runLoopAfter(pendingFinalize.dripDelayMs);
  CHECK(!pendingFinalize.pending);
  CHECK(std::isfinite(runtimeConfig.weightOffsetG));
  CHECK(runtimeConfig.weightOffsetG == originalOffset);
}

void r04_scale_commands_execute_once_and_report_results() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareStartTimerCalls == 1);
  CHECK(scale.resetTimerCalls == 0);
  CHECK(scale.startTimerCalls == 0);
  CHECK(scale.tareCalls == 0);
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(scale.stopTimerCalls == 1);
  CHECK(session.timerStopResult == TimerStopResult::WRITE_SUCCEEDED);

  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  scale.tareStartTimerSucceeds = false;
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareStartTimerCalls == 1);
  CHECK(scale.resetTimerCalls == 0);
  CHECK(scale.startTimerCalls == 0);
  CHECK(scale.tareCalls == 0);
  loop();
  CHECK(!session.remoteTimerStarted);

  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  advanceToBrew();
  scale.stopTimerSucceeds = false;
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(executeNextScaleCommand());
  CHECK(scale.stopTimerCalls == 1);
  CHECK(session.timerStopResult == TimerStopResult::WRITE_FAILED);
}

void r05_regression_uses_last_ten_valid_samples() {
  // The timed law predicts t* from the time-window flow estimate; with no
  // valid rate only the direct backstop can cut.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(shot.expectedEndS == NO_CUT_PREDICTION_S);
  float weight = 0.0f;
  for (unsigned i = 0; i < 40; ++i) {
    hostMillis += 100;
    publishWeight(weight += 0.2f);
  }
  const corridor::CorridorState &cut = session.cut;
  CHECK(cut.rate.valid && std::abs(cut.rate.rateX100 - 2000) <= 20);
  int32_t shotMs = 0;
  CHECK(shotTimeAtMs(session.lastAcceptedWeightAtMs, shotMs));
  int32_t crossing = 0;
  CHECK(corridor::crossingMs(cut.params, shotMs, gramsToDg(weight),
                             cut.rate.rateX100, cut.tauMs, cut.offsetDg,
                             crossing));
  CHECK(fabsf(shot.expectedEndS - crossing / 1000.0f) < 0.001f);
  // A falling stream has no rate: no prediction.
  for (unsigned i = 0; i < 40; ++i) {
    hostMillis += 100;
    publishWeight(weight -= 0.05f);
  }
  CHECK(shot.expectedEndS == NO_CUT_PREDICTION_S);
  // A trend reset (late retare, scale recovery) drops the estimator.
  resetWeightTrend();
  CHECK(!session.cut.rate.valid && session.cut.rate.count == 0);

  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  resetShotTrajectory(session.startedAtMs);
  session.receivedFreshWeightInCycle = false;
  for (size_t i = 1; i <= MAX_SHOT_DATAPOINTS + 8; ++i) {
    recordWeightSample(static_cast<float>(i) * 0.5f,
                       shot.startMs + static_cast<uint32_t>(i * 1000));
  }
  CHECK(shot.datapoints == MAX_SHOT_DATAPOINTS);
  CHECK(shot.weight[MAX_SHOT_DATAPOINTS - 1U] ==
        static_cast<float>(MAX_SHOT_DATAPOINTS + 8U) * 0.5f);
  CHECK(shot.timeS[0] == static_cast<float>(9));
}

void r06_hard_timer_opens_circuit_without_control_loop() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(getRelaySafetySnapshot().closed);

  hostMillis = circuitClosedAtMs + HARD_MAX_CIRCUIT_CLOSED_MS;
  hostServiceEspTimer(relaySafetyTimer);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().tripped);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);

  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
}

void r07_timing_remains_correct_across_millis_wrap() {
  resetHarness(false, false);
  hostMillis = UINT32_MAX - 100;
  rawActivatorChangedAtMs = hostMillis;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  runtimeConfig.operationalWallMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  startCycle();
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
}

void r08_full_command_queue_forces_safe_manual_cycle() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ScaleCommand filler;
  filler.type = ScaleCommandType::STOP_TIMER;
  for (size_t i = 0; i < SCALE_COMMAND_QUEUE_LENGTH; ++i) {
    CHECK(enqueueScaleCommand(filler));
  }
  CHECK(scaleCommandQueue->items.size() == SCALE_COMMAND_QUEUE_LENGTH);

  startCycle();
  CHECK(!session.timerStartCommandQueued);
  CHECK(!session.automaticEnabled);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(getRelaySafetySnapshot().closed);
}

void r09_stop_is_not_retried_after_disconnect_before_execution() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);

  // Link-state publication can lag the library's own connection flag.
  scale.connected = false;
  CHECK(executeNextScaleCommand());
  CHECK(scale.stopTimerCalls == 0);
  CHECK(session.timerStopResult == TimerStopResult::NOT_ATTEMPTED);

  setScaleConnected(true);
  loop();
  CHECK(scale.stopTimerCalls == 0);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void r09b_old_commands_are_discarded_after_reconnect_but_new_stop_runs() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const ScaleCommandType oldTypes[] = {
      ScaleCommandType::START_TIMER_AND_TARE,
      ScaleCommandType::TARE_ONLY,
      ScaleCommandType::STOP_TIMER};
  for (ScaleCommandType type : oldTypes) {
    ScaleCommand command;
    command.type = type;
    command.cycleId = 99;
    CHECK(enqueueScaleCommand(command));
  }
  const uint32_t oldGeneration = getScaleLinkSnapshot().connectionGeneration;
  setScaleConnected(false);
  setScaleConnected(true);
  CHECK(getScaleLinkSnapshot().connectionGeneration != oldGeneration);
  debugLog.clear();
  CHECK(executeNextScaleCommand());
  CHECK(executeNextScaleCommand());
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareStartTimerCalls == 0);
  CHECK(scale.tareCalls == 0);
  CHECK(scale.stopTimerCalls == 0);
  DebugEvent rejected[8] = {};
  const size_t rejectedCount = copyDebugEvents(0, rejected, 8);
  bool sawTare = false;
  for (size_t index = 0; index < rejectedCount; ++index) {
    if (rejected[index].code == DebugCode::SCALE_STALE_EVENT_REJECTED) {
      sawTare |= strstr(rejected[index].text,
                        "command=tare_only reason=generation mismatch") != nullptr;
    }
  }
  CHECK(sawTare);

  ScaleCommand currentStop;
  currentStop.type = ScaleCommandType::STOP_TIMER;
  currentStop.cycleId = 100;
  CHECK(enqueueScaleCommand(currentStop));
  CHECK(executeNextScaleCommand());
  CHECK(scale.stopTimerCalls == 1);
}

void r10_relay_cannot_close_when_hard_timer_cannot_arm() {
  resetHarness(false, false);
  reachReadyFromBoot();
  delete relaySafetyTimer;
  relaySafetyTimer = nullptr;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::RELAY_SAFETY_FAILURE);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == 0);

  resetHarness(false, false);
  reachReadyFromBoot();
  hostEspTimerStartSucceeds = false;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::RELAY_SAFETY_FAILURE);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  CHECK(hostRelayClosedWrites == 0);
}

void r11_final_shot_analysis_updates_only_valid_offset() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const float originalOffset = runtimeConfig.weightOffsetG;
  const uint32_t generation = getScaleLinkSnapshot().connectionGeneration;
  for (int part = 0; part < 3; ++part) {
    preparePendingBbwForTest();
    pendingFinalize.offsetAnalysis = true;
    pendingFinalize.startedWithScale = pendingFinalize.automaticBrew = true;
    pendingFinalize.scaleBaselineReady = true;
    pendingFinalize.scaleConnectionGeneration = generation;
    pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
    pendingFinalize.dripDelayMs = 0;
    pendingFinalize.endedAtMs = hostMillis;
    pendingFinalize.endedWeightSequence = currentWeightSequence;
    pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
    currentWeightConnectionGeneration = generation;
    hostMillis += 20;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    if (part == 0) {
      pendingFinalize.weightOffsetG = originalOffset;
      currentWeight = goalWeightDgToGrams(DEFAULT_GOAL_WEIGHT_DG) + 1.0f;
    } else if (part == 1) {
      pendingFinalize.weightOffsetG = runtimeConfig.weightOffsetG;
      currentWeight = goalWeightDgToGrams(DEFAULT_GOAL_WEIGHT_DG) + MAX_OFFSET_G + 1.0f;
    } else {
      pendingFinalize.weightOffsetG = 3.0f;
      currentWeight = 32.0f;
    }
    pendingShotFinalizeTask();
    if (part == 0) {
      // EWMA (alpha 30): default + 0.3 x 1 g miss.
      CHECK(fabsf(runtimeConfig.weightOffsetG - (originalOffset + 0.3f)) < 0.001f);
    } else if (part == 1) {
      // Observation outside the safe range leaves the offset unchanged.
      CHECK(fabsf(runtimeConfig.weightOffsetG - (originalOffset + 0.3f)) < 0.001f);
    } else {
      // EWMA pulls the offset down: 3.0 g + 0.3 x (32 - 36) g = 1.8 g.
      CHECK(fabsf(runtimeConfig.weightOffsetG - 1.8f) < 0.001f);
    }
  }
}

void bbw01_snapshots_and_isolated_finalization() {
  for (EndReason reason : {EndReason::SCALE_THRESHOLD, EndReason::CONFIGURED_WALL_LIMIT,
                           EndReason::GLOBAL_LIMIT, EndReason::RELAY_SAFETY_FAILURE})
  for (bool resetBeforeFinalize : {false, true}) {
    const bool learn = !resetBeforeFinalize && reason == EndReason::SCALE_THRESHOLD;
    resetHarness(false, true);
    reachReadyFromBoot();
    ShotPreset &preset = mutableActiveShotPreset(presetBank);
    preset.bbwEwmaAlpha = 50;
    preset.bbwAlphaLearned = 1;
    startCycle();
    advanceToBrew();
    CHECK(session.config.bbwAlgorithm == 1 && session.config.bbwAlpha == 50);
    session.startedWithScale = true;
    session.scaleBaselineReady = true;
    session.lastAcceptedWeightG = 34.5f;
    session.hasWeightAnchor = true;
    shot.automaticBrew = true;
    schedulePendingShotFinalize(reason, 30000);
    session.active = false;
    pendingFinalize.dripDelayMs = 0;
    pendingFinalize.rateAtStopX100 = 2000;  // cut with a valid rate
    const uint8_t origin = preset.id;
    const float used = pendingFinalize.weightOffsetG;
    CHECK(pendingFinalize.bbwAlpha == 50);
    // A saved choice made after the shot does not redirect its captured policy.
    preset.bbwEwmaAlpha = 10;
    if (resetBeforeFinalize) bbwLearningBank.invalidate(origin, presetBank);
    setActiveShotPreset(presetBank, FACTORY_PRESET_ID_SINGLE_ADAPTIVE);
    hostMillis += 20;
    currentWeight = 36.2f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    currentWeightConnectionGeneration = pendingFinalize.scaleConnectionGeneration;
    pendingShotFinalizeTask();
    const ShotPreset *after = mutableShotPreset(presetBank, origin);
    CHECK(fabsf(after->bbwEwmaOffsetG -
                (learn ? used + 0.10f : used)) < 1e-5f);
    CHECK(activeShotPreset(presetBank).bbwEwmaOffsetG == 0.5f);
    ShotLogRecord record[1] = {};
    CHECK(shotLog.copyNewestFirst(record, 1) == 1);
    CHECK(record[0].offsetUsedCg == weightToCentigrams(used));
    CHECK(shotLogBbwAlpha(record[0]) == 50);
    CHECK(strcmp(shotLogBbwVersion(record[0]), "1") == 0);
    CHECK(strcmp(shotLogBbwLearningApplied(record[0]),
                 learn ? "true" : "false") == 0);
    CHECK(strcmp(shotLogBbwAlgorithm(record[0]), "corridor") == 0);
    CHECK(shotLogPresetId(record[0]) == origin);
  }
}

void bbw02_freshness_reset_and_safety() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.bbwEwmaAlpha = 50;
  preset.bbwAlphaLearned = 1;
  preset.bbwEwmaOffsetG = 2.0f;
  preparePendingBbwForTest();
  pendingFinalize.offsetAnalysis = true;
  pendingFinalize.startedWithScale = pendingFinalize.automaticBrew = true;
  pendingFinalize.scaleBaselineReady = true;
  pendingFinalize.scaleConnectionGeneration = getScaleLinkSnapshot().connectionGeneration;
  pendingFinalize.weightOffsetG = 2.0f;
  pendingFinalize.goalWeightDg = preset.goalWeightDg;
  pendingFinalize.endedAtMs = hostMillis;
  currentWeight = 36.2f;
  currentWeightConnectionGeneration = pendingFinalize.scaleConnectionGeneration;
  ++currentWeightSequence;
  currentWeightReceivedAtMs = ++hostMillis;
  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 1;
  CHECK(!learnPendingBbw(pendingFinalize, currentWeight, true));
  CHECK(preset.bbwEwmaOffsetG == 2.0f);
  currentWeightReceivedAtMs = hostMillis;
  CHECK(!learnPendingBbw(pendingFinalize, currentWeight, false));
  pendingFinalize.endReason = EndReason::ACTIVATOR;
  CHECK(!learnPendingBbw(pendingFinalize, currentWeight, true));
  CHECK(bbwLearningBank.forPreset(preset.id, presetBank).evidence.count == 0);
  const ShotPreset other = *findShotPreset(presetBank, FACTORY_PRESET_ID_SINGLE_ADAPTIVE);
  for (EndReason reason : {EndReason::CONFIGURED_WALL_LIMIT, EndReason::GLOBAL_LIMIT,
                           EndReason::RELAY_SAFETY_FAILURE, EndReason::CUP_REMOVED}) {
    pendingFinalize.endReason = reason;
    CHECK(!learnPendingBbw(pendingFinalize, currentWeight, true));
    CHECK(preset.bbwEwmaOffsetG == 2.0f && preset.bbwEwmaAlpha == 50);
    CHECK(bbwLearningBank.forPreset(preset.id, presetBank).evidence.count == 0);
  }
  // Snapshots still carrying the removed regression id must never learn;
  // migrated presets learn through bbw01/r58 below.
  PendingShotFinalize legacy = pendingFinalize;
  legacy.bbwAlgorithm = 0;
  legacy.bbwProfileVersion = 1;
  legacy.endReason = EndReason::SCALE_THRESHOLD;
  legacy.bbwLearningGeneration = bbwLearningBank.forPreset(preset.id, presetBank).generation;
  const float offsetBeforeReject = preset.bbwEwmaOffsetG;
  CHECK(!learnPendingBbw(legacy, currentWeight, true));
  CHECK(preset.bbwEwmaOffsetG == offsetBeforeReject);
  auto &evidence = bbwLearningBank.forPreset(preset.id, presetBank);
  evidence.evidence.observe(2.1f, 2.0f, preset.bbwEwmaAlpha);
  const uint32_t beforeBaseGeneration = evidence.generation;
  WebCommand saveBaseline;
  saveBaseline.type = WebCommandType::PRESET_OP;
  saveBaseline.presetAction = static_cast<uint8_t>(PresetAction::SAVE);
  saveBaseline.presetId = preset.id;
  saveBaseline.config = runtimeConfig;
  saveBaseline.config.weightOffsetBaselineG = 0.80f;
  saveBaseline.bbwAlphaBaseline = 37;
  processWebCommand(saveBaseline);
  CHECK(preset.weightOffsetBaselineG == 0.80f);
  CHECK(preset.bbwAlphaBaseline == 37 && preset.bbwEwmaAlpha == 50);
  CHECK(evidence.evidence.count == 1 && evidence.generation == beforeBaseGeneration);
  saveBaseline.bbwAlphaBaseline = 0;
  processWebCommand(saveBaseline);
  CHECK(preset.bbwAlphaBaseline == 37);
  saveBaseline.bbwAlphaBaseline = 101;
  processWebCommand(saveBaseline);
  CHECK(preset.bbwAlphaBaseline == 37 && preset.bbwEwmaAlpha == 50);
  CHECK(preset.bbwEwmaOffsetG == 2.0f);
  WebCommand reset;
  reset.type = WebCommandType::RESET_WEIGHT_OFFSET;
  reset.bbwResetRevisionSpecified = true;
  reset.config.revision = runtimeConfig.revision + 1;
  processWebCommand(reset);
  CHECK(preset.bbwEwmaOffsetG == 2.0f);
  reset.config.revision = runtimeConfig.revision;
  processWebCommand(reset);
  CHECK(preset.bbwEwmaOffsetG == preset.weightOffsetBaselineG);
  CHECK(preset.bbwEwmaAlpha == 50 && preset.bbwAlphaLearned == 1);
  reset.bbwFullReset = true;
  reset.config.revision = runtimeConfig.revision;
  processWebCommand(reset);
  CHECK(preset.bbwEwmaAlpha == 37 && preset.bbwAlphaLearned == 0);
  CHECK(preset.bbwEwmaOffsetG == 0.80f);
  CHECK(memcmp(&other, findShotPreset(presetBank, other.id), sizeof(other)) == 0);
  CHECK(bbwLearningBank.forPreset(preset.id, presetBank).evidence.count == 0);
  {
    resetHarness(false, true);
    reachReadyFromBoot();
    startCycle();
    advanceToBrew();
    endBbwProtectionForTests();
    shot.expectedEndS = 1.0f;
    runLoopAfter(1000);
    loop();
    CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
    CHECK(!getRelaySafetySnapshot().closed);
  }
}

void r58_extended_shot_does_not_learn_weight_offset() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const float originalOffset = runtimeConfig.weightOffsetG;
  preparePendingBbwForTest();
  pendingFinalize.offsetAnalysis = true;
  pendingFinalize.extractionExtended = true;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.endedWeightSequence = 0;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = originalOffset;
  currentWeight = goalWeightDgToGrams(DEFAULT_GOAL_WEIGHT_DG) + 3.0f;
  currentWeightSequence = 1;
  currentWeightReceivedAtMs = hostMillis + 1;
  runLoopAfter(pendingFinalize.dripDelayMs);
  finishHostMaintenance();
  CHECK(runtimeConfig.weightOffsetG == originalOffset);
}

void r65_non_weight_cut_does_not_learn_weight_offset() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const float originalOffset = runtimeConfig.weightOffsetG;
  preparePendingBbwForTest();
  // No weight cut fired (EndReason NONE): the drip offset must not learn.
  pendingFinalize.offsetAnalysis = true;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.endedWeightSequence = 0;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = originalOffset;
  currentWeight = goalWeightDgToGrams(DEFAULT_GOAL_WEIGHT_DG) - 6.0f;
  currentWeightSequence = 1;
  currentWeightReceivedAtMs = hostMillis + 1;
  runLoopAfter(pendingFinalize.dripDelayMs);
  finishHostMaintenance();
  CHECK(runtimeConfig.weightOffsetG == originalOffset);
}

void r12_scale_worker_service_publishes_weight_and_detects_failure() {
  resetHarness(false, true);
  scale.weight = 12.34f;
  scale.newWeightAvailableValue = true;
  serviceScaleWorkerLink();
  processScaleWorkerEvents();
  CHECK(scale.newWeightAvailableCalls == 1);
  CHECK(currentWeightSequence == 1);
  CHECK(fabsf(currentWeight - 12.34f) < 0.001f);
  CHECK(scaleAvailable());

  scale.weight = 23.45f;
  scale.newWeightAvailableValue = true;
  serviceScaleWorkerLink();
  CHECK(scale.newWeightAvailableCalls == 1);
  hostMillis += SCALE_WEIGHT_POLL_INTERVAL_MS - 1;
  serviceScaleWorkerLink();
  CHECK(scale.newWeightAvailableCalls == 1);
  ++hostMillis;
  serviceScaleWorkerLink();
  processScaleWorkerEvents();
  CHECK(scale.newWeightAvailableCalls == 2);
  CHECK(fabsf(currentWeight - 23.45f) < 0.001f);

  scale.heartbeatRequiredValue = true;
  scale.heartbeatSucceeds = false;
  serviceScaleWorkerLink();
  CHECK(scale.heartbeatCalls == 1);
  CHECK(!scaleAvailable());
  CHECK(getScaleLinkSnapshot().disconnectSequence == 1);
}

void r12e_rejected_heartbeat_preserves_connected_epoch() {
  resetHarness(false, true);
  scale.weight = 12.34f;
  scale.newWeightAvailableValue = true;
  serviceScaleWorkerLink();
  processScaleWorkerEvents();
  const ScaleLinkSnapshot before = getScaleLinkSnapshot();
  CHECK(before.state == ScaleLinkState::CONNECTED);
  CHECK(currentWeightIsFresh());
  const uint32_t weightAtMs = lastScaleWeightAtMs;
  scale.heartbeatRequiredValue = true;
  scale.heartbeatSucceeds = false;
  scale.heartbeatFailureKeepsLink = true;
  serviceScaleWorkerLink();
  const ScaleLinkSnapshot after = getScaleLinkSnapshot();
  CHECK(scale.heartbeatCalls == 1);
  CHECK(after.state == ScaleLinkState::CONNECTED);
  CHECK(after.connectionGeneration == before.connectionGeneration);
  CHECK(after.disconnectSequence == before.disconnectSequence);
  CHECK(lastScaleWeightAtMs == weightAtMs);
  CHECK(currentWeightIsFresh());
  CHECK(scaleAvailable());
  for (unsigned attempt = 0; attempt < 4; ++attempt) {
    hostMillis += SCALE_WEIGHT_POLL_INTERVAL_MS;
    scale.weight += 1.0f;
    scale.newWeightAvailableValue = true;
    serviceScaleWorkerLink();
    processScaleWorkerEvents();
    CHECK(fabsf(currentWeight - scale.weight) < 0.001f);
    CHECK(lastScaleWeightAtMs == hostMillis);
    CHECK(getScaleLinkSnapshot().disconnectSequence == before.disconnectSequence);
  }
  CHECK(scale.heartbeatCalls == 5);
  scale.connected = false;
  serviceScaleWorkerLink();
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::DISCONNECTED);
  CHECK(getScaleLinkSnapshot().disconnectSequence == before.disconnectSequence + 1);
  CHECK(lastScaleWeightAtMs == 0);
  CHECK(!scaleAvailable());
}

void r12d_command_does_not_drain_buffered_weight() {
  resetHarness(false, true);
  ScaleCommand command;
  command.type = ScaleCommandType::STOP_TIMER;
  CHECK(enqueueScaleCommand(command));
  scale.weight = 9.87f;
  scale.newWeightAvailableValue = true;
  CHECK(executeNextScaleCommand());
  CHECK(scale.stopTimerCalls == 1);
  CHECK(scale.newWeightAvailableCalls == 0);
  CHECK(pollPendingScaleWeightEvent(hostMillis));
  CHECK(scale.newWeightAvailableCalls == 1);
}

void r12b_discovery_clears_stale_connected_link_snapshot() {
  resetHarness(false, true);
  setScaleLinkState(ScaleLinkState::CONNECTED);
  markScaleWorkerProgress();
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::CONNECTED);
  CHECK(scaleAvailable());

  // Library already dropped (e.g. beep path called isConnected()), but the
  // snapshot was left CONNECTED. Discovery must clear it immediately even
  // while an indefinite idle scan is in progress.
  scale.connected = false;
  scale.scanning = true;
  uint32_t lastScanCycleMs = hostMillis;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = hostMillis;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::DISCONNECTED);
  CHECK(!scaleAvailable());
}

// The published scale level keeps its hysteresis across status publishes.
void r12d_scale_signal_level_keeps_hysteresis_across_publishes() {
  resetHarness(false, true);
  ControlStatusSnapshot status;
  const auto level = [&](int8_t rssi, uint32_t at) {
    scale.linkRssiValue = rssi;
    serviceScaleLinkRssi(at);
    publishControlStatus();
    copyControlStatus(status);
    return status.scaleSignalLevel;
  };
  CHECK(level(-59, 1000) == 3);
  CHECK(level(-61, 2000) == 3);
  CHECK(level(-59, 3000) == 3);
  CHECK(level(-64, 4000) == 2);
  CHECK(level(-59, 5000) == 2);
  CHECK(level(-57, 6000) == 3);
}

void r12c_connected_link_rssi_samples_and_clears() {
  resetHarness(false, true);
  scale.linkRssiValue = -64;
  serviceScaleLinkRssi(1000);
  ScaleLinkSnapshot snap = getScaleLinkSnapshot();
  CHECK(snap.rssiValid);
  CHECK(snap.rssi == -64);
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.scaleRssiValid);
  CHECK(status.scaleRssi == -64);

  scale.linkRssiValue = -40;
  serviceScaleLinkRssi(1500);
  snap = getScaleLinkSnapshot();
  CHECK(snap.rssiValid);
  CHECK(snap.rssi == -64);

  serviceScaleLinkRssi(2000);
  snap = getScaleLinkSnapshot();
  CHECK(snap.rssi == -40);

  scale.linkRssiValue = SCALE_LINK_RSSI_UNAVAILABLE;
  serviceScaleLinkRssi(3000);
  snap = getScaleLinkSnapshot();
  CHECK(!snap.rssiValid);

  scale.linkRssiValue = -70;
  serviceScaleLinkRssi(4000);
  setScaleConnected(false);
  snap = getScaleLinkSnapshot();
  CHECK(!snap.rssiValid);
  publishControlStatus();
  copyControlStatus(status);
  CHECK(!status.scaleRssiValid);
}

void r13_full_queue_prevents_stop_without_delaying_relay_open() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  advanceToBrew();

  ScaleCommand filler;
  filler.type = ScaleCommandType::START_TIMER_AND_TARE;
  for (size_t i = 0; i < SCALE_COMMAND_QUEUE_LENGTH; ++i) {
    CHECK(enqueueScaleCommand(filler));
  }
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(session.stopTimerRequested);
  CHECK(session.timerStopResult == TimerStopResult::NOT_ATTEMPTED);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void r14_invalid_runtime_configuration_is_transactionally_rejected() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const RuntimeConfig original = runtimeConfig;
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.requestId = 14;
  update.config = runtimeConfig;
  update.config.goalWeightDg = MIN_GOAL_WEIGHT_DG - 1;
  CHECK(validateRuntimeConfig(update.config) ==
        ConfigValidationError::GOAL_WEIGHT);
  CHECK(enqueueWebCommand(update));
  loop();
  CHECK(runtimeConfig.goalWeightDg == original.goalWeightDg);
  CHECK(runtimeConfig.revision == original.revision);
  CHECK(hostLastForwardedNetworkCommand.requestId == 14);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::FAILED);

  update.config = runtimeConfig;
  update.config.operationalWallMs = HARD_MAX_CIRCUIT_CLOSED_MS + 1;
  CHECK(validateRuntimeConfig(update.config) ==
        ConfigValidationError::OPERATIONAL_WALL);
  CHECK(enqueueWebCommand(update));
  loop();
  CHECK(runtimeConfig.operationalWallMs == original.operationalWallMs);
}

void r15_gptimer_opens_circuit_without_arduino_or_esp_timer_tasks() {
  resetHarness(false, false);
  CHECK(setMachineCircuitClosed(true, 5000));
  hostMillis += 5000;
  independentSafetyTimer.serviceForHost();

  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  CHECK(!relay.closed);
  CHECK(relay.operationalTripped);
  CHECK(relay.state == RelaySafetyState::TRIPPED);
  CHECK(relay.fault == RelaySafetyFault::OPERATIONAL_LIMIT);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
}

// SAF-03: the ISR leaves RTC/timer work to the task, which must finish it
// even when the trip ends a wake passthrough instead of a session.
void r15b_isr_trip_without_session_clears_close_marker() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostMachinePhysicalStartDisposition =
      MachinePhysicalStartDisposition::WAKE_PASSTHROUGH;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(machineWakePassthroughActive);
  hostMillis = circuitClosedAtMs + HARD_MAX_CIRCUIT_CLOSED_MS;
  independentSafetyTimer.serviceForHost();
  CHECK(safetyResetRecordForHost().relayMarker == SAFETY_RELAY_CLOSED_MARKER);
  loop();
  CHECK(!session.active);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(safetyResetRecordForHost().relayMarker == SAFETY_RELAY_OPEN_MARKER);
  CHECK(!relaySafetyTimer->active && !operationalLimitTimer->active);
}

// SAF-01: the hook replaces ESP-IDF's aborting one, so it must abort too.
void r15c_stack_overflow_hook_opens_circuit_then_aborts() {
  resetHarness(false, false);
  CHECK(setMachineCircuitClosed(true, 5000));
  hostSystemAbortDetails = nullptr;
  vApplicationStackOverflowHook(nullptr, nullptr);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  CHECK(safetyResetRecordForHost().relayMarker == SAFETY_RELAY_OPEN_MARKER);
  CHECK(hostSystemAbortDetails != nullptr);
}

void r16_timeout_during_arm_transaction_can_never_close_circuit() {
  resetHarness(false, false);
  hostCircuitArmBeforeCommitHook = []() {
    ++hostMillis;
    independentSafetyTimer.serviceForHost();
  };

  CHECK(!setMachineCircuitClosed(true, 1));
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  CHECK(!relay.closed);
  CHECK(relay.operationalTripped);
  CHECK(relay.state == RelaySafetyState::TRIPPED);
  CHECK(hostRelayClosedWrites == 0);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
}

void r17_gptimer_arm_failure_prevents_relay_energization() {
  resetHarness(false, false);
  hostGptimerArmSucceeds = false;

  CHECK(!setMachineCircuitClosed(true, 5000));
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  CHECK(!relay.closed);
  CHECK(relay.state == RelaySafetyState::LOCKOUT);
  CHECK(relay.fault == RelaySafetyFault::TIMER_ARM_FAILED);
  CHECK(hostRelayClosedWrites == 0);
}

void r18_watchdog_fault_opens_circuit_and_requests_safe_restart() {
  resetHarness(false, false);
  CHECK(setMachineCircuitClosed(true, 5000));

  reportTaskWatchdogFault();
  serviceRelaySafety();
  const RelaySafetySnapshot relay = getRelaySafetySnapshot();
  CHECK(!relay.closed);
  CHECK(relay.state == RelaySafetyState::LOCKOUT);
  CHECK(relay.fault == RelaySafetyFault::TASK_WATCHDOG_FAILURE);
  CHECK(safeRestartPending());
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
}

void r18b_lockout_mid_shot_finalizes_session() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.active);
  tripRelaySafety(RelaySafetyFault::INITIALIZATION_FAILED);
  loop();
  CHECK(!session.active);
  CHECK(session.endReason == EndReason::RELAY_SAFETY_FAILURE);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
}

void r19_reset_during_close_reopens_without_recovery_lockout() {
  resetHarness(false, false);
  recordRelayCommandedClosed(true);
  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
  safetyResetStatus = beginSafetyResetGuard();
  initializeRelaySafetyStateAfterBoot();

  CHECK(safetyResetStatus.resetDuringClose);
  CHECK(safetyResetStatus.unsafeReset);
  CHECK(relaySafetyState == RelaySafetyState::OPEN);
  CHECK(relaySafetyFault == RelaySafetyFault::NONE);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
  CHECK(safetyResetRecordForHost().relayMarker == SAFETY_RELAY_OPEN_MARKER);
}

void r19b_panic_boot_is_ready_for_webui_and_next_circuit_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostSafetyResetReasonCode = 4;
  hostSafetyResetReasonUnsafe = true;
  recordRelayCommandedClosed(true);
  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
  safetyResetStatus = beginSafetyResetGuard();
  initializeRelaySafetyStateAfterBoot();

  CHECK(safetyResetStatus.unsafeReset);
  CHECK(relaySafetyState == RelaySafetyState::OPEN);
  CHECK(stopperState == StopperState::READY);
  CHECK(controlAllowsConfigurationNow());

  CHECK(setMachineCircuitClosed(true, runtimeConfig.operationalWallMs));
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(setMachineCircuitClosed(false));
}

void r20_three_unsafe_resets_are_latched_as_a_boot_loop() {
  resetHarness(false, false);
  hostSafetyResetReasonCode = 6;
  hostSafetyResetReasonUnsafe = true;

  SafetyResetSnapshot reset;
  for (uint32_t count = 1; count <= SAFETY_BOOT_LOOP_THRESHOLD; ++count) {
    reset = beginSafetyResetGuard();
    CHECK(reset.unsafeResetCount == count);
  }
  CHECK(reset.bootLoopDetected);
}

// A factory reset that crashes every boot must stop replaying (the unit
// bricked on a stack overflow inside the wipe); a power cut must still resume.
void r20g_boot_recovery_stops_replaying_after_a_crash_loop() {
  resetHarness(false, false);
  (void)beginSafetyResetGuard();  // the controller has been running
  hostSafetyResetReasonCode = 3;  // software restart into the pending reset
  hostSafetyResetReasonUnsafe = false;
  hostSafetyResetReasonPowerOn = false;
  CHECK(bootRecoveryStep(beginSafetyResetGuard()) == BootRecoveryStep::RESUME);
  hostSafetyResetReasonCode = 4;  // the wipe, then the settings fallback, panic
  hostSafetyResetReasonUnsafe = true;
  for (uint32_t count = 1; count <= 2 * SAFETY_BOOT_LOOP_THRESHOLD; ++count) {
    const BootRecoveryStep expected =
        count < SAFETY_BOOT_LOOP_THRESHOLD       ? BootRecoveryStep::RESUME
        : count < 2 * SAFETY_BOOT_LOOP_THRESHOLD ? BootRecoveryStep::SETTINGS_ONLY
                                                 : BootRecoveryStep::GIVE_UP;
    CHECK(bootRecoveryStep(beginSafetyResetGuard()) == expected);
  }
  hostSafetyResetReasonCode = 1;  // a power cut mid-wipe resumes again
  hostSafetyResetReasonUnsafe = false;
  hostSafetyResetReasonPowerOn = true;
  CHECK(bootRecoveryStep(beginSafetyResetGuard()) == BootRecoveryStep::RESUME);
}

void r20h_boot_recovery_stack_reads_as_a_sentence() {
  DebugEvent event{};
  event.code = DebugCode::BOOT_RECOVERY_STACK;
  event.argument1 = 2304;
  event.argument2 = 2;
  char message[96] = {};
  formatDebugEventMessage(event, message, sizeof(message), FW_VERSION);
  CHECK(strcmp(message, "factory reset left 2304 B of loopTask stack unused") == 0);
}

void r20b_reset_history_keeps_reason_and_previous_uptime() {
  resetHarness(false, false);
  SafetyResetSnapshot reset = beginSafetyResetGuard();
  CHECK(reset.resetHistoryCount == 1);
  CHECK(reset.resetHistory[0].reasonCode == 1);
  CHECK(reset.resetHistory[0].uptimeMs == 0);

  recordResetUptime(123456);
  hostSafetyResetReasonCode = 4;
  hostSafetyResetReasonUnsafe = true;
  reset = beginSafetyResetGuard();
  CHECK(reset.resetHistoryCount == 2);
  CHECK(reset.resetHistory[0].reasonCode == 4);
  CHECK(reset.resetHistory[0].uptimeMs == 123456);
  CHECK(reset.resetHistory[1].reasonCode == 1);
}

void r20c_reset_uptime_checkpoint_is_no_more_frequent_than_one_minute() {
  resetHarness(false, false);
  (void)beginSafetyResetGuard();
  recordResetUptime(59999);
  CHECK(safetyResetRecordForHost().currentUptimeMs == 0);
  recordResetUptime(60000);
  CHECK(safetyResetRecordForHost().currentUptimeMs == 60000);
  recordResetUptime(119999);
  CHECK(safetyResetRecordForHost().currentUptimeMs == 60000);
  recordResetUptime(120000);
  CHECK(safetyResetRecordForHost().currentUptimeMs == 120000);
}

void r20d_clear_reset_history_keeps_current_reset_reason() {
  resetHarness(false, false);
  SafetyResetSnapshot reset = beginSafetyResetGuard();
  const uint32_t reason = reset.reasonCode;
  CHECK(reset.resetHistoryCount == 1);
  CHECK(clearPersistedResetHistory(reset.unsafeResetCount));
  CHECK(safetyResetRecordValid());
  CHECK(safetyResetRecordForHost().historyCount == 0);
  CHECK(reason == hostSafetyResetReasonCode);
}

void r20e_uptime_forced_invalid_and_wrapped_checkpoints() {
  resetHarness(false, false);
  for (const uint32_t historyCount : {0U, uint32_t(RESET_HISTORY_CAPACITY)}) {
    ResetHistoryEntry history[RESET_HISTORY_CAPACITY] = {};
    for (uint32_t i = 0; i < historyCount; ++i) history[i] = {i + 1U, i * 10U};
    initializeSafetyResetRecord(SAFETY_RELAY_OPEN_MARKER, 0, history, historyCount);
    detail::resetUptimeLastCheckpointMs = 0;
    const uint32_t checksum = detail::safetyResetRecord.historyChecksum;
    recordResetUptime(17, true);
    CHECK(detail::safetyResetRecord.currentUptimeMs == 17);
    CHECK(detail::safetyResetRecord.currentUptimeMsInverse == ~17U);
    recordResetUptime(60016);
    CHECK(detail::safetyResetRecord.currentUptimeMs == 17);
    recordResetUptime(60017);
    CHECK(detail::safetyResetRecord.currentUptimeMs == 60017);
    CHECK(detail::safetyResetRecord.historyChecksum == checksum);
    // Invalid due/forced writes must preserve both data and checkpoint clock.
    detail::safetyResetRecord.historyChecksum ^= 1U;
    recordResetUptime(120017);
    recordResetUptime(120018, true);
    CHECK(detail::safetyResetRecord.currentUptimeMs == 60017);
    CHECK(detail::resetUptimeLastCheckpointMs == 60017);
    detail::safetyResetRecord.historyChecksum = checksum;
    recordResetUptime(UINT32_MAX - 100U, true);
    recordResetUptime(59999);
    CHECK(detail::safetyResetRecord.currentUptimeMs == UINT32_MAX - 100U);
    recordResetUptime(60000);
    CHECK(detail::safetyResetRecord.currentUptimeMs == 60000);
    CHECK(safetyResetRecordValid());
  }
}

void r20f_reset_history_clear_waits_for_configuration_safe() {
  resetHarness(false, false);
  hostSafetyResetReasonUnsafe = true;
  safetyResetStatus = beginSafetyResetGuard();
  reachReadyFromBoot();
  startCycle();
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);

  WebCommand clear;
  clear.type = WebCommandType::CLEAR_RESET_HISTORY;
  clear.requestId = 41;
  clear.unsafeWebUiOverride = true;
  processWebCommand(clear);
  CHECK(resetHistoryClearHeld);
  CHECK(!pendingResetHistoryClear.unsafeWebUiOverride);
  CHECK(!maintenanceLease.active);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(safetyResetStatus.resetHistoryCount == 1);

  const uint32_t elapsed = elapsedMs(session.startedAtMs);
  if (elapsed <= runtimeConfig.rinseGestureMs) {
    runLoopAfter(runtimeConfig.rinseGestureMs - elapsed + 1);
  }
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!session.active);
  CHECK(!resetHistoryClearHeld);
  CHECK(maintenanceLease.active);
  CHECK(safetyResetStatus.resetHistoryCount == 1);
  finishHostMaintenance();
  CHECK(safetyResetStatus.resetHistoryCount == 0);
  CHECK(hostLastForwardedNetworkCommand.type ==
        WebCommandType::CLEAR_RESET_HISTORY);
  CHECK(hostLastForwardedNetworkCommand.resetHistoryUnsafeCount == 1);
}

void w01_default_runtime_configuration_is_valid() {
  const RuntimeConfig config;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  CHECK(config.operationalWallMs == DEFAULT_OPERATIONAL_WALL_MS);
  CHECK(config.rinseGestureMs == 1000);
  CHECK(!config.rinseEnabled);
  CHECK(config.fastBeforeMs == DEFAULT_FAST_BEFORE_MS);
  CHECK(config.canTareStartTimer);
  CHECK(config.postTareBaselineGraceMs == DEFAULT_POST_TARE_BASELINE_GRACE_MS);
  CHECK(config.scaleTimerStopExtraDelayMs ==
        DEFAULT_SCALE_TIMER_STOP_EXTRA_DELAY_MS);
  CHECK(config.firstDropBeep);
  CHECK(config.paddleReturnReminderBeep);
  CHECK(config.paddleReturnReminderIntervalMs == 30000);
  RuntimeConfig twoMinuteReminder = config;
  twoMinuteReminder.paddleReturnReminderIntervalMs = 120000;
  CHECK(validateRuntimeConfig(twoMinuteReminder) == ConfigValidationError::NONE);
  CHECK(!config.soundAlertsMuted);
  CHECK(config.buzzerScaleLostBeep);
  CHECK(config.buzzerAutoToManualGuardEndBeep);
  CHECK(config.buzzerManualNoScaleBeep);
  CHECK(config.buzzerScaleConnectedBeep);
  CHECK(config.scaleConnectedLed);
  CHECK(config.buzzerExtendedPulseRate ==
        static_cast<uint8_t>(DEFAULT_EXTENDED_PULSE_RATE));
  CHECK(config.buzzerSlowExtendedPulseRate ==
        static_cast<uint8_t>(DEFAULT_EXTENDED_PULSE_RATE));
  CHECK(DEFAULT_EXTENDED_PULSE_RATE == ExtendedPulseRate::FAST);
  CHECK(buzzerPatternForExtendedPulseRate(config.buzzerExtendedPulseRate) ==
        BuzzerPattern::PULSE_4HZ);
  CHECK(buzzerPatternForExtendedPulseRate(config.buzzerSlowExtendedPulseRate) ==
        BuzzerPattern::PULSE_4HZ);
  CHECK(config.alertOutputChannel ==
        static_cast<uint8_t>(DEFAULT_ALERT_OUTPUT_CHANNEL));
  CHECK(DEFAULT_ALERT_OUTPUT_CHANNEL ==
        (BUZZER_SUPPORT_ENABLED ? AlertOutputChannel::BUZZER_ONLY
                                : AlertOutputChannel::SCALE_ONLY));
  CHECK(config.bookooMuteOnBuzzerOnly);
  CHECK(config.bookooConnectBeepLevel == DEFAULT_BOOKOO_CONNECT_BEEP_LEVEL);
  CHECK(config.fastSideEnabled);
  CHECK(config.slowSideEnabled);
  CHECK(config.slowMarginDg == DEFAULT_SLOW_MARGIN_DG);
  CHECK(config.slowAfterMs == DEFAULT_SLOW_AFTER_MS);
  CHECK(config.scaleLossEnabled && config.scaleLossFixedMs == 0);
  CHECK(noScaleBbwModeValue(config.noScaleBbwMode) ==
        static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE));
  CHECK(idleAccessoryRetareEnabled(config.noScaleBbwMode));
  CHECK(!noScaleAllowRinseWhileArmed(config.noScaleBbwMode));
  CHECK(config.cupProtectionEnabled);
  CHECK(config.stopIfCupRemoved);
  CHECK(!config.requireCupToStart);
  CHECK(config.avoidAccidentalTouchEnabled);
  CHECK(fabsf(config.cupRemovedWeightG - DEFAULT_CUP_REMOVED_WEIGHT_G) < 0.001f);
  CHECK(config.lastShotCooldownMs == DEFAULT_LAST_SHOT_COOLDOWN_MS);
  CHECK(config.dripDelayMs == DEFAULT_DRIP_DELAY_MS);
  CHECK(serialLogLevelFromRuntime(config) == LogLevel::NONE);
  CHECK(config.ringRetainLogLevel == static_cast<uint8_t>(LogLevel::NONE));
  CHECK(config.paddleMode == static_cast<uint8_t>(PaddleMode::NATURAL));
  CHECK(runtimeStopPulseMs(config) == COMPILED_STOP_PULSE_MS);
  CHECK(runtimeMaxSinglePressMs(config) == COMPILED_MAX_SINGLE_PRESS_MS);
}

void w02_each_runtime_field_is_validated() {
  RuntimeConfig config;
  config.goalWeightDg = 90;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::GOAL_WEIGHT);
  config = RuntimeConfig{};
  config.weightOffsetG = std::numeric_limits<float>::quiet_NaN();
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::WEIGHT_OFFSET);
  config = RuntimeConfig{};
  config.rinseGestureMs = 99;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::RINSE_GESTURE);
  config = RuntimeConfig{};
  config.rinseDurationMs = 499;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::RINSE_DURATION);
  config = RuntimeConfig{};
  config.bbwProtectionMs = MIN_BBW_PROTECTION_MS - 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::BBW_PROTECTION_TIMEOUT);
  config = RuntimeConfig{};
  config.bbwProtectionMs = MAX_BBW_PROTECTION_MS + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::BBW_PROTECTION_TIMEOUT);
  // Smooth-cut edit rules (plan §4.3).
  config = RuntimeConfig{};
  config.fastMarginDg = 4;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::FAST_MARGIN);
  config.fastMarginDg = 91;  // Over 25 % of a 36 g target.
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::FAST_MARGIN);
  config.fastSideEnabled = false;  // An off side keeps an unchecked margin.
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config = RuntimeConfig{};
  config.goalWeightDg = 1980;
  config.fastMarginDg = 30;  // Target plus extra over 200 g.
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::FAST_MARGIN);
  config = RuntimeConfig{};
  config.goalWeightDg = 100;
  config.fastMarginDg = 20;
  config.slowMarginDg = 51;  // Must leave 5 g.
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::SLOW_MARGIN);
  config.slowMarginDg = 50;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config = RuntimeConfig{};
  config.fastBeforeMs = config.bbwProtectionMs + 1500;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::FAST_BEFORE);
  config = RuntimeConfig{};
  config.fastBeforeMs = 27250;  // 0.5 s steps.
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::FAST_BEFORE);
  config = RuntimeConfig{};
  config.slowAfterMs = config.fastBeforeMs + 500;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::FAST_BEFORE);
  config = RuntimeConfig{};
  config.slowAfterMs = config.operationalWallMs - 500;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::SLOW_AFTER);
  config = RuntimeConfig{};
  config.scaleLossFixedMs = config.bbwProtectionMs - 1;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::SCALE_LOSS_FIXED);
  config.scaleLossFixedMs = config.operationalWallMs;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
}

void w03_runtime_timing_relations_are_transactional() {
  RuntimeConfig config;
  config.operationalWallMs = 10000;
  config.bbwProtectionMs = 11000;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::TIMING_RELATION);
  config = RuntimeConfig{};
  config.autoRetare = false;
  config.operationalWallMs = 5000;
  config.bbwProtectionMs = 6000;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::TIMING_RELATION);
  config = RuntimeConfig{};
  config.canTareStartTimer = true;
  config.autoTare = false;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::COMBINED_TARE_REQUIRES_AUTOTARE);
  config = RuntimeConfig{};
  config.postTareBaselineGraceMs = MIN_POST_TARE_BASELINE_GRACE_MS - 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::POST_TARE_BASELINE_GRACE);
  config = RuntimeConfig{};
  config.postTareBaselineGraceMs = MAX_POST_TARE_BASELINE_GRACE_MS + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::POST_TARE_BASELINE_GRACE);
  config = RuntimeConfig{};
  config.postTareBaselineGraceMs = MIN_POST_TARE_BASELINE_GRACE_MS;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config.postTareBaselineGraceMs = MAX_POST_TARE_BASELINE_GRACE_MS;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config = RuntimeConfig{};
  config.scaleTimerStopExtraDelayMs = MAX_SCALE_TIMER_STOP_EXTRA_DELAY_MS + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::SCALE_TIMER_STOP_EXTRA_DELAY);
  config = RuntimeConfig{};
  config.bookooConnectBeepLevel = BOOKOO_BEEP_LEVEL_MAX + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::BOOKOO_CONNECT_BEEP_LEVEL);
  config = RuntimeConfig{};
  config.buzzerExtendedPulseRate = 9;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::EXTENDED_PULSE_RATE);
  config = RuntimeConfig{};
  config.buzzerSlowExtendedPulseRate = 9;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::SLOW_EXTENDED_PULSE_RATE);
  config = RuntimeConfig{};
  config.noScaleBbwMode = 3;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::NO_SCALE_BBW_MODE);
  config = RuntimeConfig{};
  config.noScaleBbwMode = packNoScaleBbwMode(
      static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE), true);
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config = RuntimeConfig{};
  config.lastShotCooldownMs = MIN_LAST_SHOT_COOLDOWN_MS - 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::LAST_SHOT_COOLDOWN);
  config = RuntimeConfig{};
  config.lastShotCooldownMs = MAX_LAST_SHOT_COOLDOWN_MS + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::LAST_SHOT_COOLDOWN);
  config = RuntimeConfig{};
  config.dripDelayMs = MIN_DRIP_DELAY_MS;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config.dripDelayMs = MAX_DRIP_DELAY_MS;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  config.dripDelayMs = MAX_DRIP_DELAY_MS + 1;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::DRIP_DELAY);
  config = RuntimeConfig{};
  config.ringRetainLogLevel = static_cast<uint8_t>(LogLevel::NONE) + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::RING_RETAIN_LOG_LEVEL);
  config = RuntimeConfig{};
  config.serialLogLevel = static_cast<uint8_t>(LogLevel::NONE) + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::SERIAL_LOG_LEVEL);
  uint8_t parsedLevel = 255;
  CHECK(parseLogLevel("info", parsedLevel));
  CHECK(parsedLevel == static_cast<uint8_t>(LogLevel::INFO));
  CHECK(parseLogLevel("none", parsedLevel));
  CHECK(parsedLevel == static_cast<uint8_t>(LogLevel::NONE));
  CHECK(!parseLogLevel("verbose", parsedLevel));
  config = RuntimeConfig{};
  config.paddleMode = 9;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::PADDLE_MODE);
  uint8_t parsedPaddle = 255;
  CHECK(parsePaddleMode("natural", parsedPaddle));
  CHECK(parsedPaddle == static_cast<uint8_t>(PaddleMode::NATURAL));
  CHECK(parsePaddleMode("original", parsedPaddle));
  CHECK(parsedPaddle == static_cast<uint8_t>(PaddleMode::ORIGINAL));
  CHECK(parsePaddleMode("auto", parsedPaddle));
  CHECK(parsedPaddle == static_cast<uint8_t>(PaddleMode::AUTO));
  CHECK(!parsePaddleMode("legacy", parsedPaddle));
  config = RuntimeConfig{};
  config.stopPulseTenMs = 4;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::STOP_PULSE);
  config = RuntimeConfig{};
  config.maxSinglePressHundredMs = 51;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::MAX_SINGLE_PRESS);
  config = RuntimeConfig{};
  config.stopPulseTenMs = 0;
  config.maxSinglePressHundredMs = 0;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  CHECK(runtimeStopPulseMs(config) == COMPILED_STOP_PULSE_MS);
  CHECK(runtimeMaxSinglePressMs(config) == COMPILED_MAX_SINGLE_PRESS_MS);
  config = RuntimeConfig{};
  config.reedConfirmTimeoutHundredMs = 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::REED_CONFIRM_TIMEOUT);
  config.reedConfirmTimeoutHundredMs = 51;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::REED_CONFIRM_TIMEOUT);
  config.reedConfirmTimeoutHundredMs = 0;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  CHECK(runtimeReedConfirmTimeoutMs(config) ==
        COMPILED_REED_CONFIRM_TIMEOUT_MS);
  config = RuntimeConfig{};
  config.shotReactTimeoutS = 2;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::SHOT_REACT_TIMEOUT);
  config.shotReactTimeoutS = 31;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::SHOT_REACT_TIMEOUT);
  config.shotReactTimeoutS = 0;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  CHECK(runtimeShotReactTimeoutMs(config) == COMPILED_SHOT_REACT_TIMEOUT_MS);
  config.shotReactTimeoutS = 12;
  CHECK(validateRuntimeConfig(config) == ConfigValidationError::NONE);
  CHECK(runtimeShotReactTimeoutS(config) == 12);
  config = RuntimeConfig{};
  config.cupRemovedWeightG = 0.0f;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::CUP_REMOVED_WEIGHT);
  config = RuntimeConfig{};
  config.cupRemovedWeightG = MAX_CUP_REMOVED_WEIGHT_G + 0.1f;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::CUP_REMOVED_WEIGHT);
  config = RuntimeConfig{};
  config.cupRemovedWeightG = MIN_CUP_REMOVED_WEIGHT_G - 0.1f;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::CUP_REMOVED_WEIGHT);
}

void w04_wifi_credentials_have_strict_bounds() {
  CHECK(validWifiSsid("Micra"));
  CHECK(!validWifiSsid(""));
  {
    const uint8_t mac[6] = {0x3C, 0xDC, 0x75, 0xFC, 0xBF, 0x0D};
    char ssid[WIFI_SSID_CAPACITY] = {};
    CHECK(formatSoftApSsid(ssid, sizeof(ssid), mac));
    CHECK(strcmp(ssid, "OpenBrewByWeightAP-75fcbf0d") == 0);
    CHECK(validWifiSsid(ssid));
    CHECK(!formatSoftApSsid(ssid, SOFT_AP_SSID_LENGTH, mac));
  }
  CHECK(validWifiPassword("12345678", false));
  CHECK(!validWifiPassword("1234", false));
  CHECK(validWifiPassword("", true));
  CHECK(validDevicePassword("ineedacoffee"));
  CHECK(shouldReuseSavedWifiCredentials("CafeLAN", "", false, true, "CafeLAN",
                                        false));
  CHECK(shouldReuseSavedWifiCredentials("CafeLAN", "", true, true, "CafeLAN",
                                        true));
  CHECK(!shouldReuseSavedWifiCredentials("CafeLAN", "CafePass1", false, true,
                                         "CafeLAN", false));
  CHECK(!shouldReuseSavedWifiCredentials("OtherNet", "", false, true, "CafeLAN",
                                         false));
  CHECK(!shouldReuseSavedWifiCredentials("CafeLAN", "", false, false, "CafeLAN",
                                         false));
  CHECK(!shouldReuseSavedWifiCredentials("CafeLAN", "", true, true, "CafeLAN",
                                         false));
  CHECK(!shouldReuseSavedWifiCredentials("", "", false, true, "CafeLAN", false));
}

// /setup onboarding transaction (audit F-04, F-05, F-10, F-14, F-17/F-18):
// scenarios drive the pure phase reducer the network task applies.
void w04c_setup_onboarding_phase_scenarios() {
  using P = SetupPhase;
  using E = SetupEvent;
  const auto run = [](P phase, std::initializer_list<E> events) {
    for (E event : events) phase = nextSetupPhase(phase, event);
    return phase;
  };

  // Fresh device, working network: the attempt keeps the AP, never confirms
  // before its own lease, then waits for the owner's "Save Wi-Fi" (status
  // polls do not confirm it) and hands over to the grace once confirmed.
  P phase = nextSetupPhase(P::NONE, E::APPLY_SAVED);
  CHECK(phase == P::QUEUED && setupKeepsSoftAp(phase));
  CHECK(!setupConfirmAllowed(phase, true));
  phase = nextSetupPhase(phase, E::ATTEMPT_STARTED);
  CHECK(phase == P::ATTEMPTING && setupKeepsSoftAp(phase));
  CHECK(!setupConfirmAllowed(phase, true) && !setupHoldsReconnect(phase));
  CHECK(strcmp(setupPhaseName(phase), "connecting") == 0);
  phase = nextSetupPhase(phase, E::JOINED);
  CHECK(phase == P::JOINED && setupConfirmAllowed(phase, true));
  CHECK(setupAwaitsOwnerSave(phase, true) && !setupAwaitsOwnerSave(phase, false));
  CHECK(!setupAwaitsOwnerSave(P::CONFIRMED, true) && !setupAwaitsOwnerSave(P::NONE, false));
  CHECK(softApTeardownStep(phase, 1, 0, 5000) == SoftApTeardown::KEEP);
  phase = nextSetupPhase(phase, E::CONFIRMED);
  CHECK(phase == P::CONFIRMED && !setupKeepsSoftAp(phase));
  CHECK(softApTeardownStep(phase, 1, 65000, 5000) == SoftApTeardown::KEEP);
  CHECK(softApTeardownStep(phase, 1, 65000, 65000) == SoftApTeardown::STOP);
  CHECK(nextSetupPhase(phase, E::AP_STOPPED) == P::NONE);

  // F-04: an AP_START keep while still associated to the old network. The
  // save must not be confirmed by a status poll before the new attempt
  // joins, and a lease on another SSID fails the attempt instead.
  phase = run(P::NONE, {E::KEEP_REQUESTED});
  CHECK(phase == P::KEEP && setupKeepsSoftAp(phase));
  phase = run(phase, {E::APPLY_SAVED});
  CHECK(!setupConfirmAllowed(phase, true));
  phase = run(phase, {E::ATTEMPT_STARTED, E::JOINED_OTHER});
  CHECK(phase == P::FAILED && !setupConfirmAllowed(phase, true));
  CHECK(setupHoldsReconnect(phase) && setupKeepsSoftAp(phase));

  // Wrong password: the attempt fails, holds the reconnect loop, and a new
  // save from the page releases the hold; the 180 s window reverts.
  phase = run(P::NONE, {E::APPLY_SAVED, E::ATTEMPT_STARTED, E::ATTEMPT_FAILED});
  CHECK(phase == P::FAILED && setupHoldsReconnect(phase));
  CHECK(strcmp(setupPhaseName(phase), "failed") == 0);
  CHECK(nextSetupPhase(phase, E::APPLY_SAVED) == P::QUEUED);
  CHECK(!setupHoldsReconnect(P::QUEUED));
  CHECK(nextSetupPhase(phase, E::REVERTED) == P::NONE);
  // A held attempt the driver completes late still proves the network.
  CHECK(nextSetupPhase(phase, E::JOINED) == P::JOINED);
  // Stale events never resurrect a finished session.
  CHECK(run(P::NONE, {E::ATTEMPT_STARTED, E::ATTEMPT_FAILED, E::JOINED,
                      E::CONFIRMED}) == P::NONE);

  // AP_STOP drops the keep, but a queued apply survives to restart.
  CHECK(nextSetupPhase(P::KEEP, E::KEEP_RELEASED) == P::NONE);
  CHECK(nextSetupPhase(P::ATTEMPTING, E::KEEP_RELEASED) == P::NONE);
  CHECK(nextSetupPhase(P::QUEUED, E::KEEP_RELEASED) == P::QUEUED);
  CHECK(nextSetupPhase(P::QUEUED, E::APPLY_RESTARTS) == P::NONE);
  // AP_START during an attempt does not reset it.
  CHECK(nextSetupPhase(P::ATTEMPTING, E::KEEP_REQUESTED) == P::ATTEMPTING);
  // Outside onboarding (Admin restart path) confirm needs the AP down.
  CHECK(setupConfirmAllowed(P::NONE, false) && !setupConfirmAllowed(P::NONE, true));

  // F-10: the recovery AP closes at once when nobody is on it, but phones
  // still associated get the grace window, which then ends on time.
  CHECK(softApTeardownStep(P::NONE, 0, 0, 1000) == SoftApTeardown::STOP);
  CHECK(softApTeardownStep(P::NONE, 2, 0, 1000) == SoftApTeardown::ARM_GRACE);
  CHECK(softApTeardownStep(P::NONE, 2, 61000, 1000) == SoftApTeardown::KEEP);
  CHECK(softApTeardownStep(P::NONE, 2, 61000, 61000) == SoftApTeardown::STOP);
  CHECK(softApTeardownStep(P::KEEP, 0, 0, 1000) == SoftApTeardown::KEEP);
  CHECK(softApTeardownStep(P::NONE, 1, 0xFFFFF000u, 0x00000100u) ==
        SoftApTeardown::STOP);

  // Finish ends a saved session during the grace, never with an unsaved
  // joined test, mid-attempt, after the deadline, or with the AP down.
  CHECK(!setupFinishAllowed(P::JOINED, true, 0, 1000));
  CHECK(!setupFinishAllowed(P::JOINED, true, 61000, 1000));
  CHECK(setupFinishAllowed(P::CONFIRMED, true, 61000, 1000));
  CHECK(setupFinishAllowed(P::NONE, true, 61000, 1000));
  CHECK(!setupFinishAllowed(P::ATTEMPTING, true, 0, 1000));
  CHECK(!setupFinishAllowed(P::CONFIRMED, true, 61000, 61000));
  CHECK(!setupFinishAllowed(P::JOINED, false, 0, 1000));

  // "Keep" / "Continue without Wi-Fi" drop a test once its attempt began:
  // joined, failed, or still attempting; never a saved or queued network.
  CHECK(setupDiscardAllowed(P::JOINED) && setupDiscardAllowed(P::FAILED) &&
        setupDiscardAllowed(P::ATTEMPTING));
  CHECK(!setupDiscardAllowed(P::QUEUED) && !setupDiscardAllowed(P::CONFIRMED) &&
        !setupDiscardAllowed(P::NONE) && !setupDiscardAllowed(P::KEEP));
}

void w04b_select_best_sta_ap_prefers_strongest_matching_bssid() {
  const uint8_t weakBssid[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  const uint8_t strongBssid[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  const uint8_t otherBssid[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  const StaApScanEntry entries[] = {
      {"CafeLAN", weakBssid, 1, -80, false},
      {"OtherNet", strongBssid, 6, -40, false},
      {"CafeLAN", strongBssid, 11, -55, false},
      {"CafeLAN", otherBssid, 6, -50, true},
  };
  size_t bestIndex = 99;
  CHECK(selectBestStaAp(entries, 4, "CafeLAN", false, bestIndex));
  CHECK(bestIndex == 2);
  CHECK(entries[bestIndex].rssi == -55);
  CHECK(entries[bestIndex].bssid[0] == 0xAA);

  bestIndex = 99;
  CHECK(!selectBestStaAp(entries, 4, "Missing", false, bestIndex));
  CHECK(!selectBestStaAp(nullptr, 0, "CafeLAN", false, bestIndex));
  CHECK(selectBestStaAp(entries, 4, "CafeLAN", true, bestIndex));
  CHECK(bestIndex == 3);
}

void attemptActiveConfigUpdate() {
  const RuntimeConfig before = runtimeConfig;
  char preferredBefore[PREFERRED_SCALE_MAC_CAPACITY] = {};
  copyPreferredScaleMac(preferredBefore, sizeof(preferredBefore));
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = before;
  update.config.goalWeightDg = before.goalWeightDg + 1;
  update.preferredScaleSpecified = true;
  strcpy(update.preferredScaleMac, "AA:BB:CC:DD:EE:99");
  processWebCommand(update);
  CHECK(runtimeConfig.goalWeightDg == before.goalWeightDg);
  CHECK(runtimeConfig.revision == before.revision);
  CHECK(preferredScaleMacEqual(scalePreferredMac, preferredBefore));
}

void w05_config_is_blocked_while_brewing_early() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  attemptActiveConfigUpdate();
}

void w06_config_is_blocked_while_brewing() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  attemptActiveConfigUpdate();
}

void w07_config_is_blocked_while_rinsing() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, 400);
  CHECK(stopperState == StopperState::RINSE);
  attemptActiveConfigUpdate();
}

void w08_config_is_blocked_during_manual_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  attemptActiveConfigUpdate();
}

void w09_valid_config_applies_only_from_ready() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.weightOffsetG = 2.25f;
  mutableActiveShotPreset(presetBank).bbwEwmaOffsetG = 2.25f;
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.goalWeightDg = 420;
  update.config.weightOffsetG = 4.5f;  // Not a Web-editable field.
  noteScaleHistory("AA:BB:CC:DD:EE:99", "BOOKOO_SC_U 369041", false);
  CHECK(setScaleFriendlyName("AA:BB:CC:DD:EE:99", "Coffee scale"));
  update.preferredScaleSpecified = true;
  strcpy(update.preferredScaleMac, "aa:bb:cc:dd:ee:99");
  update.config.scaleMacCacheMode = static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  const uint32_t oldRevision = runtimeConfig.revision;
  processWebCommand(update);
  CHECK(runtimeConfig.goalWeightDg == 420);
  CHECK(fabsf(runtimeConfig.weightOffsetG - 2.25f) < 0.001f);
  CHECK(runtimeConfig.revision == oldRevision + 1);
  CHECK(!maintenanceLease.active);
  CHECK(strcmp(scalePreferredMac, "AA:BB:CC:DD:EE:99") == 0);
  CHECK(strcmp(scalePreferredName, "BOOKOO_SC_U 369041") == 0);
  const uint32_t appliedRevision = runtimeConfig.revision;
  // Invalid identity, stale revision, and invalid settings reject both changes.
  for (int failure = 0; failure < 3; ++failure) {
    update.config = runtimeConfig;
    update.config.firstDropBeep = !runtimeConfig.firstDropBeep;
    strcpy(update.preferredScaleMac, "AA:BB:CC:DD:EE:98");
    if (failure == 0) strcpy(update.preferredScaleMac, "invalid");
    if (failure == 1) --update.config.revision;
    if (failure == 2) update.config.minimumCupWeightG = -1;
    processWebCommand(update);
    CHECK(runtimeConfig.revision == appliedRevision);
    CHECK(strcmp(scalePreferredMac, "AA:BB:CC:DD:EE:99") == 0);
  }
  update.config = runtimeConfig;
  update.preferredScaleSpecified = false;
  processWebCommand(update);
  CHECK(strcmp(scalePreferredMac, "AA:BB:CC:DD:EE:99") == 0);
  update.config = runtimeConfig;
  update.preferredScaleSpecified = true;
  update.preferredScaleMac[0] = '\0';
  processWebCommand(update);
  CHECK(scalePreferredMac[0] == '\0');
  CHECK(scaleDiscoveryPausedUntilMs == 0);
  char friendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  CHECK(findScaleHistoryFriendlyName(scaleHistory, "AA:BB:CC:DD:EE:99",
                                     friendly, sizeof(friendly)));
  CHECK(strcmp(friendly, "Coffee scale") == 0);
}

void w10_cycle_configuration_snapshot_is_immutable() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  const CycleConfigSnapshot frozen = session.config;
  runtimeConfig.goalWeightDg = frozen.goalWeightDg + 10;
  runtimeConfig.operationalWallMs = 10000;
  CHECK(session.config.goalWeightDg == frozen.goalWeightDg);
  CHECK(session.config.operationalWallMs == frozen.operationalWallMs);
  runtimeConfig.firstDropBeep = !frozen.firstDropBeep;
  CHECK(session.config.firstDropBeep == frozen.firstDropBeep);
}

void configureShortOperationalWallForManual() {
  runtimeConfig.operationalWallMs = 20000;
  runtimeConfig.bbwProtectionMs = 7000;
  runtimeConfig.fastBeforeMs = 9000;
  runtimeConfig.slowAfterMs = 19000;
  runtimeConfig.scaleLossFixedMs = 15000;
  CHECK(validateRuntimeConfig(runtimeConfig) == ConfigValidationError::NONE);
}

void armTimerOnlySession() {
  runtimeConfig.timerOnly = true;
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.brewByWeight = false;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
}

void assertManualCycleSkipsOperationalWallUntilHardCap(
    StopperState expectedState) {
  CHECK(stopperState == expectedState);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().operationalLimitMs ==
        HARD_MAX_CIRCUIT_CLOSED_MS);
  hostMillis = circuitClosedAtMs + runtimeConfig.operationalWallMs;
  hostServiceEspTimer(operationalLimitTimer);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!getRelaySafetySnapshot().operationalTripped);
  CHECK(stopperState == expectedState);
  hostMillis = circuitClosedAtMs + HARD_MAX_CIRCUIT_CLOSED_MS;
  hostServiceEspTimer(relaySafetyTimer);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().tripped);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
}

void w11_operational_timer_opens_without_control_loop() {
  resetHarness(false, false);
  reachReadyFromBoot();
  configureShortOperationalWallForManual();
  startCycle();
  assertManualCycleSkipsOperationalWallUntilHardCap(
      StopperState::MANUAL_NO_SCALE);
}

void w11b_timer_only_natural_skips_operational_wall() {
  resetHarness(false, true);
  armTimerOnlySession();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::NATURAL);
  reachReadyFromBoot();
  configureShortOperationalWallForManual();
  startCycle();
  assertManualCycleSkipsOperationalWallUntilHardCap(StopperState::BREW);
}

void w11c_timer_only_original_skips_operational_wall() {
  resetHarness(false, true);
  armTimerOnlySession();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  reachReadyFromBoot();
  configureShortOperationalWallForManual();
  startCycle();
  CHECK(!machineCycleHardMaxArmedForTest());
  assertManualCycleSkipsOperationalWallUntilHardCap(StopperState::BREW);
}

void w11d_timer_only_auto_skips_operational_wall() {
  resetHarness(false, true);
  armTimerOnlySession();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  reachReadyFromBoot();
  configureShortOperationalWallForManual();
  startCycle();
  assertManualCycleSkipsOperationalWallUntilHardCap(StopperState::BREW);
}

void w12_hard_limit_cannot_be_configured_above_sixty_seconds() {
  RuntimeConfig config;
  config.operationalWallMs = HARD_MAX_CIRCUIT_CLOSED_MS + 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::OPERATIONAL_WALL);
}

void w45_bbw_protection_retare_relation_is_validated() {
  RuntimeConfig config;
  config.retareWindowMs = 5000;
  config.bbwProtectionMs =
      config.retareWindowMs + MIN_BBW_PROTECTION_AFTER_RETARE_MS - 1;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::BBW_PROTECTION_RETARE_RELATION);
}

void w13_virtual_paddle_uses_normal_state_machine() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand on = webControlCommand(WebCommandType::REMOTE_ON);
  processWebCommand(on);
  CHECK(session.active);
  CHECK(session.source == ControlSource::WEB);
  CHECK(virtualHoldOn);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  WebCommand off = webControlCommand(WebCommandType::REMOTE_OFF);
  processWebCommand(off);
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w14_physical_motion_overrides_web_control() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand on = webControlCommand(WebCommandType::REMOTE_ON);
  processWebCommand(on);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::PHYSICAL_OVERRIDE);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w15_web_rinse_starts_scale_timer() {
  resetHarness(false, true);
  reachReadyFromBoot();
  WebCommand rinse = webControlCommand(WebCommandType::RINSE);
  processWebCommand(rinse);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(session.source == ControlSource::WEB);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  CHECK(executeNextScaleCommand());
  runLoopAfter(runtimeConfig.rinseDurationMs);
  CHECK(stopperState == StopperState::READY);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);

  processWebCommand(rinse);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::PHYSICAL_OVERRIDE);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(scale.tareStartTimerCalls == 1);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
}

void w16_web_stop_during_rinse_preserves_rearm() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, 400);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  WebCommand stop;
  stop.type = WebCommandType::STOP;
  processWebCommand(stop);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::WEB_STOP);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w17_web_session_stop_is_a_safe_stop() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand on = webControlCommand(WebCommandType::REMOTE_ON);
  processWebCommand(on);
  WebCommand timeout;
  timeout.type = WebCommandType::STOP_HEARTBEAT;
  processWebCommand(timeout);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::WEB_HEARTBEAT_TIMEOUT);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w18_web_stop_can_end_a_physical_brew_only_by_opening() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  WebCommand stop;
  stop.type = WebCommandType::STOP;
  processWebCommand(stop);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::WEB_STOP);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w19_web_start_is_rejected_outside_ready() {
  resetHarness(true, false);
  WebCommand on = webControlCommand(WebCommandType::REMOTE_ON);
  processWebCommand(on);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w20_restart_waits_while_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  const RelaySafetySnapshot before = getRelaySafetySnapshot();
  WebCommand restart;
  restart.type = WebCommandType::RESTART;
  restart.unsafeWebUiOverride = true;
  processWebCommand(restart);
  CHECK(session.active);
  CHECK(plannedRestartHeld);
  CHECK(!maintenanceLease.active);
  CHECK(getRelaySafetySnapshot().closed == before.closed);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);

  const uint32_t elapsed = elapsedMs(session.startedAtMs);
  if (elapsed <= runtimeConfig.rinseGestureMs) {
    runLoopAfter(runtimeConfig.rinseGestureMs - elapsed + 1);
  }
  CHECK(session.active);
  CHECK(plannedRestartHeld);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!session.active);
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS + 1);
  CHECK(!plannedRestartHeld);
  CHECK(maintenanceLease.active ||
        hostLastForwardedNetworkCommand.type == WebCommandType::RESTART);
}

void w20b_planned_esp_restart_waits_for_shot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  requestSafeRestart();
  loop();
  CHECK(session.active);
  CHECK(safeRestartPending());
  CHECK(getRelaySafetySnapshot().closed);

  reportTaskWatchdogFault();
  loop();
  CHECK(!safeRestartPending());
  CHECK(!getRelaySafetySnapshot().closed);
}

void w21_network_change_is_rejected_while_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  WebCommand network;
  network.setNetworkType(WebCommandType::SAVE_NETWORK);
  strcpy(network.network.ssid, "test");
  strcpy(network.network.password, "password");
  processWebCommand(network);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
}

void w21b_override_maintenance_mid_shot_records_the_shot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  WebCommand network;
  network.setNetworkType(WebCommandType::SAVE_NETWORK);
  strcpy(network.network.ssid, "test");
  strcpy(network.network.password, "password");
  network.unsafeWebUiOverride = true;
  processWebCommand(network);
  CHECK(maintenanceLease.active);
  CHECK(!session.active);
  CHECK(session.endReason == EndReason::WEB_STOP);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w22_timer_only_disables_predictive_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.timerOnly = true;
  startCycle();
  advanceToBrew();
  CHECK(!scaleBeepPending);
  shot.expectedEndS = 0.0f;
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
}

void w23_combined_tare_command_uses_cycle_snapshot() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTare = true;
  runtimeConfig.canTareStartTimer = true;
  startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareStartTimerCalls == 1);
  CHECK(scale.resetTimerCalls == 0);
  CHECK(scale.startTimerCalls == 0);
}

void w24_debug_ring_is_bounded_and_ordered() {
  DebugRingBuffer ring;
  for (size_t index = 0; index < DEBUG_EVENT_CAPACITY + 5; ++index) {
    ring.add(static_cast<uint32_t>(index), 0, LogLevel::INFO, DebugCategory::WEB,
             DebugCode::WEB_COMMAND_ACCEPTED, static_cast<int32_t>(index));
  }
  CHECK(ring.overwritten() == 5);
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  DebugLogReadMetadata initialRead;
  const size_t copied =
      ring.copyAfter(0, events, DEBUG_EVENT_CAPACITY, &initialRead);
  CHECK(copied == DEBUG_EVENT_CAPACITY);
  CHECK(initialRead.historyOverwritten == 5);
  CHECK(initialRead.missedEvents == 0);
  CHECK(!initialRead.hasMore);
  CHECK(!initialRead.cursorInvalid);
  CHECK(events[0].argument1 == 5);
  CHECK(events[copied - 1].argument1 ==
        static_cast<int32_t>(DEBUG_EVENT_CAPACITY + 4));
  CHECK(ring.countAfter(0) == DEBUG_EVENT_CAPACITY);
  DebugEvent first = {};
  CHECK(ring.copyFirstAfter(0, first));
  CHECK(first.argument1 == 5);
  DebugEvent second = {};
  CHECK(ring.copyFirstAfter(first.sequence, second));
  CHECK(second.argument1 == 6);

  DebugEvent page[4] = {};
  DebugLogReadMetadata laggedRead;
  CHECK(ring.copyAfter(1, page, 4, &laggedRead) == 4);
  CHECK(page[0].sequence == 6);
  CHECK(laggedRead.missedEvents == 4);
  CHECK(laggedRead.hasMore);
  CHECK(!laggedRead.cursorInvalid);

  DebugLogReadMetadata invalidRead;
  CHECK(ring.copyAfter(DEBUG_EVENT_CAPACITY + 6, page, 4, &invalidRead) == 0);
  CHECK(invalidRead.missedEvents == 0);
  CHECK(!invalidRead.hasMore);
  CHECK(invalidRead.cursorInvalid);

  DebugRingBuffer reused;
  reused.add(1, 0, LogLevel::INFO, DebugCategory::SYSTEM,
             DebugCode::LOG_TEXT, 0, 0, "old text");
  for (size_t index = 0; index < DEBUG_EVENT_CAPACITY; ++index) {
    reused.add(static_cast<uint32_t>(index + 2), 0, LogLevel::INFO,
               DebugCategory::SYSTEM, DebugCode::BOOT_SUBSYSTEM,
               static_cast<int32_t>(index));
  }
  DebugEvent reusedEvents[DEBUG_EVENT_CAPACITY] = {};
  const size_t reusedCount =
      reused.copyAfter(0, reusedEvents, DEBUG_EVENT_CAPACITY);
  CHECK(reusedCount == DEBUG_EVENT_CAPACITY);
  for (size_t index = 0; index < reusedCount; ++index) {
    CHECK(reusedEvents[index].text[0] == '\0');
  }

  const LogLevel previousRingLevel = ringRetainLogLevel;
  debugLog.clear();
  ringRetainLogLevel = LogLevel::INFO;
  for (size_t index = 0; index < DEBUG_EVENT_CAPACITY + 8; ++index) {
    logEmit(LogLevel::INFO, DebugCategory::SYSTEM, DebugCode::BOOT_SUBSYSTEM,
            static_cast<int32_t>(index));
  }
  DebugEvent retained[DEBUG_EVENT_CAPACITY] = {};
  const size_t retainedCount =
      copyDebugEvents(0, retained, DEBUG_EVENT_CAPACITY);
  CHECK(debugLog.overwritten() == 8);
  for (size_t index = 0; index < retainedCount; ++index) {
    CHECK(retained[index].code != DebugCode::SYSTEM_LOG_OVERRUN);
  }
  debugLog.clear();
  __atomic_store_n(&debugLogDroppedSnapshot, 0U, __ATOMIC_RELAXED);
  ringRetainLogLevel = previousRingLevel;
}

void w25_weight_samples_do_not_fill_debug_log() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  DebugEvent existing[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, existing, DEBUG_EVENT_CAPACITY);
  const uint32_t lastSequence = count == 0 ? 0 : existing[count - 1].sequence;
  recordWeightSample(12.0f, session.startedAtMs + 1000);
  DebugEvent newEvents[2] = {};
  CHECK(copyDebugEvents(lastSequence, newEvents, 2) == 0);
}

void w26_status_is_a_copied_snapshot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.activeCycle);
  CHECK(status.relayClosed);
  CHECK(status.machineRunning);
  CHECK(status.source == ControlSource::PHYSICAL);
  CHECK(status.config.revision == session.config.revision);
  ControlGateSnapshot gate;
  copyControlGate(gate);
  CHECK(gate.activeCycle);
  CHECK(gate.relayClosed);
  CHECK(gate.machineRunning);
  CHECK(gate.source == ControlSource::PHYSICAL);
}

void w46_status_reports_uptime_since_boot() {
  resetHarness(false, false);
  bootStartedAtMs = 1000;
  hostMillis = 6500;
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.uptimeMs == 5500);
}

void w47_status_reports_live_scale_weight_and_timer() {
  resetHarness(false, true);
  reachReadyFromBoot();
  publishWeight(18.5f);
  scale.timerValid = true;
  scale.timerMs = 12340;
  scale.timerAgeMs = 40;
  updateWorkerLinkState();
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.observedWeightValid);
  CHECK(fabsf(status.observedWeightG - 18.5f) < 0.01f);
  CHECK(status.currentTimerValid);
  CHECK(status.currentTimerMs == 12340);
  CHECK(status.currentTimerAgeMs == 40);

  CHECK(!status.currentFlowRateValid);

  // A scale with the flow capability reports its own flow with fresh weight.
  scale.connectedFeatures.flags |= ScaleFeatureFlowRate;
  updateWorkerLinkState();
  scale.weight = 18.6f;
  scale.flowRateValid = true;
  scale.flowRateGps = 1.57f;
  scale.newWeightAvailableValue = true;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.currentFlowRateValid);
  CHECK(status.currentFlowRateCgS == 157);
  // A newer weight alone cannot keep an old flow alive.
  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 1;
  publishWeight(18.7f);
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.currentWeightValid);
  CHECK(!status.currentFlowRateValid);
  CHECK(status.currentFlowRateCgS == 0);
  // A fresh frame without a flow reading clears the last one.
  scale.newWeightAvailableValue = true;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.currentFlowRateValid);
  scale.flowRateValid = false;
  scale.newWeightAvailableValue = true;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  publishControlStatus();
  copyControlStatus(status);
  CHECK(!status.currentFlowRateValid);
  scale.flowRateValid = true;
  // Without the capability the same flow sample is never published.
  scale.connectedFeatures.flags &= ~static_cast<uint32_t>(ScaleFeatureFlowRate);
  updateWorkerLinkState();
  scale.newWeightAvailableValue = true;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  publishControlStatus();
  copyControlStatus(status);
  CHECK(!status.currentFlowRateValid);
  scale.connectedFeatures.flags |= ScaleFeatureFlowRate;
  updateWorkerLinkState();

  setScaleConnected(false);
  publishControlStatus();
  copyControlStatus(status);
  CHECK(!status.currentTimerValid);
  CHECK(status.currentTimerMs == 0);
  CHECK(!status.currentFlowRateValid);
  CHECK(!getScaleLinkSnapshot().flowRateValid);
}

void r47_reset_reason_name_maps_known_codes() {
  CHECK(strcmp(safetyResetReasonName(1), "Power-on") == 0);
  CHECK(strcmp(safetyResetReasonName(3), "Software") == 0);
  CHECK(strcmp(safetyResetReasonName(6), "Task WDT") == 0);
  CHECK(strcmp(safetyResetReasonName(9), "Brownout") == 0);
  CHECK(strcmp(safetyResetReasonName(999), "Unknown") == 0);
}

void w27_stale_weight_is_not_presented_as_current() {
  resetHarness(false, true);
  currentWeight = 12.0f;
  currentWeightSequence = 1;
  currentWeightReceivedAtMs = 0;
  hostMillis = SCALE_WORKER_STALE_MS + 1;
  scaleWorkerProgressAtMs = hostMillis;
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(!status.currentWeightValid);
}

void w28_web_command_queue_is_bounded() {
  resetHarness(false, false);
  WebCommand command;
  command.type = WebCommandType::STOP;
  for (size_t index = 0; index < WEB_COMMAND_QUEUE_LENGTH; ++index) {
    CHECK(enqueueWebCommand(command));
  }
  CHECK(!enqueueWebCommand(command));
  CHECK(!getRelaySafetySnapshot().closed);
}

void w29_operational_wall_of_60001_is_rejected() {
  RuntimeConfig config;
  config.operationalWallMs = 60001;
  CHECK(validateRuntimeConfig(config) ==
        ConfigValidationError::OPERATIONAL_WALL);
}

void w30_last_cycle_weight_must_belong_to_that_cycle() {
  resetHarness(false, false);
  currentWeight = 99.0f;
  currentWeightSequence = 1;
  currentWeightReceivedAtMs = hostMillis;
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs + 100);
  CHECK(lastCycle.valid);
  CHECK(!lastCycle.weightValid);

  startCycle();
  ScaleEvent event;
  event.type = ScaleEventType::WEIGHT;
  event.receivedAtMs = hostMillis + 1;
  event.packetSequence = 2;
  event.weightG = 7.5f;
  publishScaleEvent(event, true);
  processScaleWorkerEvents();
  releaseAtPhysicalDuration(rawActivatorChangedAtMs,
                            runtimeConfig.rinseGestureMs + 100);
  CHECK(lastCycle.weightValid);
  CHECK(fabsf(lastCycle.lastWeightG - 7.5f) < 0.001f);
}

void w31_unsupported_scale_never_uses_tare_as_a_beep() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  scale.independentBeepSupported = false;
  updateWorkerLinkState();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  simulateFirstDrops();
  CHECK(!scaleBeepPending);
  CHECK(!executePendingScaleBrewBeep());
  CHECK(scale.beepCalls == 0);
  CHECK(scale.tareStartTimerCalls == 1);
  CHECK(scale.tareCalls == 0);
  CHECK(scale.connected);
  CHECK(session.automaticEnabled);
}

void w32_full_scale_queue_cannot_block_brew_start() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  ScaleCommand filler;
  filler.type = ScaleCommandType::STOP_TIMER;
  for (size_t index = 0; index < SCALE_COMMAND_QUEUE_LENGTH; ++index) {
    CHECK(xQueueSend(scaleCommandQueue, &filler, 0) == pdTRUE);
  }
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(shot.automaticBrew);
  CHECK(getRelaySafetySnapshot().closed);
  simulateFirstDrops();
  CHECK(scaleBeepPending);
  CHECK(scale.beepCalls == 0);
}

void w33_first_drop_beep_can_be_disabled() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.firstDropBeep = false;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  simulateFirstDrops();
  CHECK(!scaleBeepPending);
  CHECK(scale.beepCalls == 0);
}

void w34_calibration_reset_restores_baseline_and_cancels_analysis() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.weightOffsetBaselineG = 2.0f;
  runtimeConfig.weightOffsetG = 3.2f;
  {
    ShotPreset &preset = mutableActiveShotPreset(presetBank);
    preset.weightOffsetBaselineG = 2.0f;
    preset.bbwEwmaOffsetG = 3.2f;
  }
  const uint32_t previousRevision = runtimeConfig.revision;
  preparePendingBbwForTest();
  WebCommand reset;
  reset.type = WebCommandType::RESET_WEIGHT_OFFSET;
  processWebCommand(reset);
  CHECK(fabsf(runtimeConfig.weightOffsetG - 2.0f) < 0.001f);
  CHECK(fabsf(runtimeConfig.weightOffsetBaselineG - 2.0f) < 0.001f);
  CHECK(runtimeConfig.revision == previousRevision + 1);
  CHECK(pendingFinalize.pending);  // History still finalizes; stale learning cannot.
  CHECK(pendingFinalize.bbwLearningGeneration != bbwLearningBank.forPreset(
      presetBank.activeId, presetBank).generation);
  CHECK(!maintenanceLease.active);
}

void w35_status_reports_the_live_physical_paddle_gpio() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostPinLevel[ACTIVATOR_GPIO] = ACTIVATOR_ACTIVE_LEVEL;
  CHECK(!rawActivatorOn);
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.physicalActivatorOn);
}

void w36_paddle_return_reminder_beeps_at_configured_interval_only_while_open() {
  resetHarness(true, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runLoopAfter(0);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!scalePaddleReturnReminderBeepPending);
  const uint32_t interval = runtimeConfig.paddleReturnReminderIntervalMs;
  const uint32_t before = localBuzzer.acceptedRequests;
  runLoopAfter(interval - 1);
  CHECK(localBuzzer.acceptedRequests == before);
  runLoopAfter(1);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.busy());
  CHECK(scale.beepCalls == 0);
  CHECK(!scalePaddleReturnReminderBeepPending);
  CHECK(!executePendingScalePaddleReturnReminderBeep());

  setRawPaddle(false);
  // Drain any in-flight pattern without counting a new request.
  for (uint32_t step = 0; step < 20 && localBuzzer.busy(); ++step) {
    runLoopAfter(50);
  }
  const uint32_t afterOff = localBuzzer.acceptedRequests;
  runLoopAfter(interval);
  CHECK(localBuzzer.acceptedRequests == afterOff);
  CHECK(!scalePaddleReturnReminderBeepPending);
}

void w44_paddle_return_reminder_stops_after_fifteen_minutes() {
  resetHarness(true, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runLoopAfter(0);
  runLoopAfter(runtimeConfig.paddleReturnReminderIntervalMs);
  CHECK(localBuzzer.acceptedRequests >= 1);
  for (uint32_t step = 0; step < 20 && localBuzzer.busy(); ++step) {
    runLoopAfter(50);
  }
  runLoopAfter(runtimeConfig.paddleReturnReminderMaxDurationMs -
               runtimeConfig.paddleReturnReminderIntervalMs);
  const uint32_t afterLimit = localBuzzer.acceptedRequests;
  for (int i = 0; i < 3; ++i)
    runLoopAfter(runtimeConfig.paddleReturnReminderIntervalMs);
  CHECK(localBuzzer.acceptedRequests == afterLimit);
  CHECK(!scalePaddleReturnReminderBeepPending);

  // Only a paddle return clears the exhausted latch for the next hold.
  setRawPaddle(false);
  setRawPaddle(true);
  runLoopAfter(runtimeConfig.paddleReturnReminderIntervalMs);
  CHECK(localBuzzer.acceptedRequests > afterLimit);
}

void w50_local_buzzer_plays_triple_pattern_non_blocking() {
  resetHarness(false, false);
  CHECK(localBuzzer.ready);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.busy());
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  // Control loop must keep running while the pattern plays.
  runLoopAfter(10);
  CHECK(getRelaySafetySnapshot().closed == false);
  for (uint32_t step = 0; step < 40 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  CHECK(!localBuzzer.busy());
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!localBuzzer.request(BuzzerPattern::NONE));
}

void w50b_buzzer_phase_timer_holds_triple_rhythm_without_loop() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_BEEP_ON_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(localBuzzer.busy());
  hostMillis += BUZZER_BEEP_GAP_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_BEEP_ON_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_BEEP_GAP_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_BEEP_ON_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!localBuzzer.busy());
}

void w50f_buzzer_timer_failures_drop_audio_without_affecting_relay() {
  resetHarness(false, false);
  hostEspTimerStartSucceeds = false;
  CHECK(!localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.phaseTimerFailures == 1);
  CHECK(localBuzzer.lastPhaseTimerError != ESP_OK);
  CHECK(!localBuzzer.busy());
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!getRelaySafetySnapshot().closed);

  hostEspTimerStartSucceeds = true;
  CHECK(localBuzzer.request(BuzzerPattern::SINGLE));
  hostEspTimerStopSucceeds = false;
  localBuzzer.stopAll();
  CHECK(localBuzzer.phaseTimerFailures == 2);
  CHECK(!localBuzzer.busy());
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!getRelaySafetySnapshot().closed);
}

void w50e_buzzer_phase_timer_holds_double_as_truncated_triple() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::DOUBLE));
  CHECK(localBuzzer.beepCount == 2);
  CHECK(localBuzzer.onMs == BUZZER_BEEP_ON_MS);
  CHECK(localBuzzer.gapMs == BUZZER_BEEP_GAP_MS);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_BEEP_ON_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(localBuzzer.busy());
  hostMillis += BUZZER_BEEP_GAP_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_BEEP_ON_MS;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!localBuzzer.busy());
}

void w50c_buzzer_phase_timer_advances_despite_loop_stall() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += 150;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(localBuzzer.busy());
}

void w50d_recovery_buzzer_patterns_have_exact_timings() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::RECOVERY_LONG));
  CHECK(localBuzzer.beepCount == 1);
  CHECK(localBuzzer.onMs == 1500);
  hostMillis += 1499;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += 1;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(!localBuzzer.busy());

  CHECK(localBuzzer.request(BuzzerPattern::RECOVERY_NETWORK_OK));
  CHECK(localBuzzer.beepCount == 3);
  CHECK(localBuzzer.onMs == 50);
  CHECK(localBuzzer.gapMs == 50);
  for (uint8_t note = 0; note < 3; ++note) {
    hostMillis += 50;
    hostServiceEspTimer(localBuzzer.phaseTimer);
    if (note + 1U < 3U) {
      hostMillis += 50;
      hostServiceEspTimer(localBuzzer.phaseTimer);
    }
  }
  CHECK(!localBuzzer.busy());

  CHECK(localBuzzer.request(BuzzerPattern::RECOVERY_FACTORY_OK));
  CHECK(localBuzzer.beepCount == 5);
  CHECK(localBuzzer.onMs == 50);
  CHECK(localBuzzer.gapMs == 50);
}

void checkLocalCompletionTone();

void w51_local_buzzer_echo_inverted_on_scale_lost_during_bbw() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(session.automaticEnabled ||
        session.weightControlState == WeightControlState::ACTIVE ||
        session.weightControlState == WeightControlState::VALIDATING);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(false);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_LOST);
  loop();
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

void w51b_scale_lost_echo_inverted_when_idle() {
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(!session.active);
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(false);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_LOST);
}

void w51c_scale_lost_silent_when_flag_off() {
  resetHarness(false, true);
  runtimeConfig.buzzerScaleLostBeep = false;
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(false);
  CHECK(localBuzzer.acceptedRequests == before);
}

void w52_local_buzzer_triple_on_manual_bbw_without_scale() {
  resetHarness(false, false);
  CHECK(!runtimeConfig.timerOnly);
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  // No-scale TRIPLE (warning) then circuit start cue, which queues behind it.
  CHECK(localBuzzer.acceptedRequests == before + 2);
}

void w53_local_buzzer_silent_when_bbw_off_without_scale() {
  resetHarness(false, false);
  runtimeConfig.timerOnly = true;
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.brewByWeight = false;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  CHECK(runtimeConfig.timerOnly);
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  // No-scale TRIPLE is suppressed when BBW is off; circuit start still beeps.
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

bool debugEventExists(DebugCode code, int32_t argument1 = INT32_MIN,
                      int32_t argument2 = INT32_MIN);

void enableNoScaleShotGuardForTest() {
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  noScaleShotGuardArmed = true;
  noScaleShotGuardActivityAtMs = 0;
  noScaleShotGuardHold = false;
  noScaleShotGuardHoldAtMs = 0;
  noScaleShotGuardNeedsFreshActivator = false;
}

void attemptBlockedNoScaleStart() {
  CHECK(stopperState == StopperState::READY);
  CHECK(noScaleShotGuardArmed);
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(noScaleShotGuardArmed);
  CHECK(noScaleShotGuardHold);
  CHECK(historyLog.count() == 0);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
  CHECK(!noScaleShotGuardHold);
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED));
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(historyLog.count() == 1);
  CHECK(shotLog.count() == 0);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type ==
        static_cast<uint8_t>(HistoryType::NO_SCALE_GUARD_ABORTED));
  CHECK(page.records[0].durationDs > 0);
  CHECK((page.records[0].flags & HISTORY_FLAG_NO_WEIGHT) != 0);
}

void ns01_armed_blocks_first_bbw_no_scale_shot() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
}

void ns02_idle_allows_second_bbw_no_scale_shot() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
  startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
}

void ns03_bbw_off_does_not_block() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.timerOnly = true;
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.brewByWeight = false;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  reachReadyFromBoot();
  startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(noScaleShotGuardArmed);
}

void ns04_scale_available_rearms() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
  CHECK(!noScaleShotGuardArmed);
  setScaleConnected(true);
  markScaleWorkerProgress();
  loop();
  CHECK(noScaleShotGuardArmed);
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_ARMED));
}

void ns05_cooldown_rearms_from_idle() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.lastShotCooldownMs = 1000;
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
  CHECK(!noScaleShotGuardArmed);
  runLoopAfter(1000);
  CHECK(noScaleShotGuardArmed);
}

void ns06_finished_shot_extends_cooldown() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.lastShotCooldownMs = 3000;
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs + 50);
  CHECK(stopperState == StopperState::READY ||
        stopperState == StopperState::REQUIRES_OFF);
  CHECK(!noScaleShotGuardArmed);
  runLoopAfter(2000);
  CHECK(!noScaleShotGuardArmed);
  runLoopAfter(1500);
  CHECK(noScaleShotGuardArmed);
}

void ns07_web_rinse_consumes_guard() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  processWebCommand(webControlCommand(WebCommandType::RINSE));
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!noScaleShotGuardArmed);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED));
  CHECK(!debugEventExists(DebugCode::RINSE_CLASSIFIED));
  CHECK(historyLog.count() == 0);
}

void ns07b_web_rinse_allowed_while_armed_runs() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode = packNoScaleBbwMode(
      static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE), true);
  reachReadyFromBoot();
  processWebCommand(webControlCommand(WebCommandType::RINSE));
  CHECK(stopperState == StopperState::RINSE);
  CHECK(!noScaleShotGuardArmed);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED));
}

void ns08_blocked_beep_respects_alert_checkbox() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.buzzerManualNoScaleBeep = false;
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(localBuzzer.acceptedRequests == before);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  CHECK(stopperState == StopperState::READY);
  CHECK(!noScaleShotGuardArmed);
  CHECK(localBuzzer.acceptedRequests == before);
}

void ns09_armed_rinse_gesture_consumes_guard() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  CHECK(noScaleShotGuardArmed);
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  const uint32_t rawOnAt = hostMillis;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(noScaleShotGuardArmed);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
  CHECK(!debugEventExists(DebugCode::RINSE_CLASSIFIED));
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED));
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type ==
        static_cast<uint8_t>(HistoryType::NO_SCALE_GUARD_ABORTED));
}

void ns09b_allow_rinse_while_armed_runs() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode = packNoScaleBbwMode(
      static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE), true);
  reachReadyFromBoot();
  CHECK(noScaleShotGuardArmed);
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  const uint32_t rawOnAt = hostMillis;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(noScaleShotGuardArmed);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
  CHECK(debugEventExists(DebugCode::RINSE_CLASSIFIED));
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED));
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 2);
  CHECK(historyLog.count() == 0);
  runLoopAfter(runtimeConfig.rinseDurationMs + 1);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::RINSE));
}

void ns10_idle_rinse_gesture_does_not_rearm() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
  CHECK(!noScaleShotGuardArmed);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  const uint32_t rawOnAt = startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 2);
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(!noScaleShotGuardArmed);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 2);
}

void ns11_web_rinse_with_scale_keeps_armed() {
  resetHarness(false, true);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  publishWeight(0.0f);
  CHECK(noScaleShotGuardArmed);
  processWebCommand(webControlCommand(WebCommandType::RINSE));
  CHECK(stopperState == StopperState::RINSE);
  CHECK(noScaleShotGuardArmed);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_BLOCKED));
}

void ns12_failed_rinse_does_not_consume_guard() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode = packNoScaleBbwMode(
      static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE), true);
  reachReadyFromBoot();
  CHECK(noScaleShotGuardArmed);
  hostGptimerArmSucceeds = false;
  processWebCommand(webControlCommand(WebCommandType::RINSE));
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(noScaleShotGuardArmed);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
}

void ns13_require_scale_blocks_repeated_starts() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  reachReadyFromBoot();
  for (int attempt = 0; attempt < 2; ++attempt) {
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    runLoopAfter(runtimeConfig.rinseGestureMs + 1);
    CHECK(stopperState == StopperState::READY);
    CHECK(noScaleShotGuardArmed);
    CHECK(noScaleShotGuardHold);
    CHECK(!getRelaySafetySnapshot().closed);
    CHECK(historyLog.count() == static_cast<size_t>(attempt));
    setRawPaddle(false);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    CHECK(!noScaleShotGuardHold);
    CHECK(historyLog.count() == static_cast<size_t>(attempt + 1));
  }
  runLoopAfter(runtimeConfig.lastShotCooldownMs + 1);
  CHECK(noScaleShotGuardArmed);
}

void ns14_require_scale_blocks_rinse() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  reachReadyFromBoot();
  processWebCommand(webControlCommand(WebCommandType::RINSE));
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(noScaleShotGuardArmed);
  CHECK(!getRelaySafetySnapshot().closed);
}

void ns15_require_scale_is_inactive_when_bbw_off() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  runtimeConfig.timerOnly = true;
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.brewByWeight = false;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  reachReadyFromBoot();
  startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(getRelaySafetySnapshot().closed);
}

void ns16_require_scale_reconnect_while_held_needs_new_gesture() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  reachReadyFromBoot();
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(noScaleShotGuardHold);
  setScaleConnected(true);
  markScaleWorkerProgress();
  publishWeight(0.0f);
  loop();
  CHECK(noScaleShotGuardHold);
  CHECK(!getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  runLoopAfter(1);
  CHECK(!noScaleShotGuardHold);
  CHECK(stopperState == StopperState::READY);
  startCycle();
  CHECK(getRelaySafetySnapshot().closed);
}

void completeRequireScaleBypassGesture(uint32_t gapMs = 100) {
  for (int cycle = 0; cycle < 3; ++cycle) {
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    setRawPaddle(false);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    if (cycle < 2) {
      runLoopAfter(gapMs);
    }
  }
}

void ns17b_require_scale_double_cycle_stays_blocked() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  reachReadyFromBoot();

  for (int cycle = 0; cycle < 2; ++cycle) {
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
    setRawPaddle(false);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  }

  CHECK(noScaleShotGuardArmed);
  CHECK(noScaleRequireBypassCycles == 2);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.activeCue != BuzzerCue::SCALE_CONNECTED);
}

void ns17_require_scale_triple_cycle_temporarily_allows() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  runtimeConfig.lastShotCooldownMs = 1000;
  reachReadyFromBoot();

  completeRequireScaleBypassGesture();

  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
  CHECK(!noScaleShotGuardHold);
  CHECK(noScaleShotGuardNeedsFreshActivator);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_CONNECTED);
  CHECK(debugEventExists(DebugCode::NO_SCALE_SHOT_GUARD_CONSUMED));
  CHECK(historyLog.count() == 3);

  runLoopAfter(runtimeConfig.lastShotCooldownMs + 1);
  CHECK(noScaleShotGuardArmed);
}

void ns18_require_scale_triple_cycle_must_fit_window() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  runtimeConfig.noScaleBbwMode =
      static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  reachReadyFromBoot();

  completeRequireScaleBypassGesture(NO_SCALE_REQUIRE_BYPASS_WINDOW_MS);

  CHECK(noScaleShotGuardArmed);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.activeCue != BuzzerCue::SCALE_CONNECTED);
}

void w54_local_buzzer_triple_on_auto_to_manual_guard_end() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t before = localBuzzer.acceptedRequests;
  finalizeCycle(EndReason::AUTO_TO_MANUAL_GUARD, StopperState::REQUIRES_OFF);
  CHECK(localBuzzer.acceptedRequests == before + 2);
  CHECK(session.endReason == EndReason::AUTO_TO_MANUAL_GUARD);
  checkLocalCompletionTone();
}

void w55_local_buzzer_queues_second_triple_while_busy() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.active == BuzzerPattern::TRIPLE);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.pending == BuzzerPattern::TRIPLE);
  CHECK(localBuzzer.acceptedRequests == 2);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  CHECK(!localBuzzer.busy());
  CHECK(localBuzzer.active == BuzzerPattern::NONE);
  CHECK(localBuzzer.pending == BuzzerPattern::NONE);
}

void w56_atm_beep_queued_when_scale_lost_after_deadline() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = true;
  runtimeConfig.scaleLossFixedMs = 15000;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
  // Past the ATM deadline while scale is still up: guard is armed but not
  // enforced, so the cycle continues until weight control suspends.
  reachSessionElapsed(16000);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(false);
  loop();
  CHECK(session.endReason == EndReason::AUTO_TO_MANUAL_GUARD);
  // Scale-lost RTTTL is preempted by machine circuit completion; ATM cue
  // queues behind it.
  CHECK(localBuzzer.acceptedRequests == before + 3);
  checkLocalCompletionTone();
}

void w63_scale_priority_paddle_uses_scale_when_connected() {
  resetHarness(true, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  runLoopAfter(0);
  const uint32_t interval = runtimeConfig.paddleReturnReminderIntervalMs;
  const uint32_t before = localBuzzer.acceptedRequests;
  runLoopAfter(interval);
  CHECK(localBuzzer.acceptedRequests == before);
  CHECK(scalePaddleReturnReminderBeepPending);
  CHECK(executePendingScalePaddleReturnReminderBeep());
}

void w64_buzzer_only_first_drop_uses_local_buzzer() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runtimeConfig.firstDropBeep = true;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  const uint32_t before = localBuzzer.acceptedRequests;
  const uint32_t beforeScaleBeeps = scale.beepCalls;
  requestFirstDropBeep();
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(scale.beepCalls == beforeScaleBeeps);
  CHECK(!scaleBeepPending);
}

void w65_scale_only_mutes_scale_lost() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(false);
  loop();
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(localBuzzer.acceptedRequests == before);
}

void w57_paddle_return_reminder_falls_back_to_scale_when_piezo_not_ready() {
  resetHarness(true, true);
  localBuzzer.ready = false;
  runLoopAfter(0);
  CHECK(!getRelaySafetySnapshot().closed);
  const uint32_t interval = runtimeConfig.paddleReturnReminderIntervalMs;
  const uint32_t beforeBuzzer = localBuzzer.acceptedRequests;
  runLoopAfter(interval);
  CHECK(localBuzzer.acceptedRequests == beforeBuzzer);
  CHECK(scalePaddleReturnReminderBeepPending);
  CHECK(executePendingScalePaddleReturnReminderBeep());
}

void w58_paddle_return_reminder_does_not_advance_interval_when_muted() {
  resetHarness(true, false);
  runtimeConfig.paddleReturnReminderIntervalMs = 200;
  runtimeConfig.paddleReturnReminderMaxDurationMs = 60000;
  runLoopAfter(0);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.pending == BuzzerPattern::TRIPLE);
  runLoopAfter(200);
  CHECK(localBuzzer.acceptedRequests == 2);
  CHECK(!scalePaddleReturnReminderBeepPending);
  // Interval must remain due so a later scale fallback can sound immediately.
  setScaleConnected(true);
  runLoopAfter(0);
  CHECK(scalePaddleReturnReminderBeepPending);
}

void drainLocalBuzzer() {
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  CHECK(!localBuzzer.busy());
}

void checkLocalCompletionTone() {
  CHECK(localBuzzer.activeCue == BuzzerCue::SHOT_END);
  CHECK(localBuzzer.rtttlPlayback);
}

void w59_local_buzzer_plays_short_long_and_double_patterns() {
  resetHarness(false, false);
  CHECK(localBuzzer.ready);
  CHECK(localBuzzer.request(BuzzerPattern::SINGLE));
  CHECK(localBuzzer.active == BuzzerPattern::SINGLE);
  CHECK(localBuzzer.beepCount == 1);
  CHECK(localBuzzer.onMs == BUZZER_SINGLE_ON_MS);
  drainLocalBuzzer();
  CHECK(localBuzzer.request(BuzzerPattern::LONG));
  CHECK(localBuzzer.active == BuzzerPattern::LONG);
  CHECK(localBuzzer.beepCount == 1);
  CHECK(localBuzzer.onMs == BUZZER_LONG_ON_MS);
  drainLocalBuzzer();
  CHECK(localBuzzer.request(BuzzerPattern::DOUBLE));
  CHECK(localBuzzer.active == BuzzerPattern::DOUBLE);
  CHECK(localBuzzer.beepCount == 2);
  CHECK(localBuzzer.onMs == BUZZER_BEEP_ON_MS);
  drainLocalBuzzer();
}

void w82_pulse_train_loops_until_deadline_or_stopIf() {
  resetHarness(false, false);
  CHECK(localBuzzer.ready);
  CHECK(startPulseTrain(BuzzerPattern::PULSE_TRAIN, 0));
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_TRAIN);
  CHECK(localBuzzer.looping);
  CHECK(localBuzzer.onMs == BUZZER_PULSE_TRAIN_ON_MS);
  CHECK(localBuzzer.gapMs == BUZZER_PULSE_TRAIN_GAP_MS);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_PULSE_TRAIN_ON_MS;
  localBuzzer.service();
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_TRAIN);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_PULSE_TRAIN_GAP_MS;
  localBuzzer.service();
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_TRAIN);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  localBuzzer.stopIf(BuzzerPattern::PULSE_TRAIN);
  CHECK(!localBuzzer.busy());
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
}

void w83_web_buzzer_test_pulse_uses_same_train_for_3s() {
  resetHarness(false, false);
  reachReadyFromBoot();
  BuzzerPattern parsed = BuzzerPattern::NONE;
  CHECK(parseBuzzerPatternId("pulse2", parsed));
  CHECK(parsed == BuzzerPattern::PULSE_TRAIN);
  CHECK(parseBuzzerPatternId("pulse", parsed));
  CHECK(parsed == BuzzerPattern::PULSE_TRAIN);
  WebCommand command = webControlCommand(WebCommandType::BUZZER_TEST);
  command.buzzerPattern = BuzzerPattern::PULSE_TRAIN;
  const uint32_t before = localBuzzer.acceptedRequests;
  processWebCommand(command);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_TRAIN);
  CHECK(localBuzzer.looping);
  CHECK(localBuzzer.onMs == BUZZER_PULSE_TRAIN_ON_MS);
  CHECK(localBuzzer.gapMs == BUZZER_PULSE_TRAIN_GAP_MS);
  runLoopAfter(BUZZER_PULSE_TRAIN_DEBUG_MS - 20);
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_TRAIN);
  runLoopAfter(40);
  CHECK(!localBuzzer.busy());
}

void w84_pulse_train_yields_to_triple() {
  resetHarness(false, false);
  CHECK(startExtendedPulseTrain(0));
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL_FAST);
  CHECK(localBuzzer.activePulseRate ==
        static_cast<uint8_t>(ExtendedPulseRate::FAST));
  CHECK(localBuzzer.rtttlPlayback);
  CHECK(localBuzzer.request(BuzzerPattern::TRIPLE));
  CHECK(localBuzzer.active == BuzzerPattern::TRIPLE);
  CHECK(localBuzzer.activeCue == BuzzerCue::NONE);
  CHECK(!buzzerPatternIsPulseTrain(localBuzzer.pending));
  drainLocalBuzzer();
}

void w84b_echo_inverted_upgrades_weak_pending() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::SINGLE));
  CHECK(localBuzzer.active == BuzzerPattern::SINGLE);
  CHECK(localBuzzer.request(BuzzerPattern::DOUBLE));
  CHECK(localBuzzer.pending == BuzzerPattern::DOUBLE);
  CHECK(localBuzzer.request(BuzzerPattern::ECHO_INVERTED));
  CHECK(localBuzzer.pending == BuzzerPattern::ECHO_INVERTED);
  drainLocalBuzzer();
}

void w85_debug_pulse_rates_use_same_on_ms_and_3s() {
  resetHarness(false, false);
  reachReadyFromBoot();
  BuzzerPattern parsed = BuzzerPattern::NONE;
  CHECK(parseBuzzerPatternId("pulse2", parsed));
  CHECK(parsed == BuzzerPattern::PULSE_TRAIN);
  CHECK(parseBuzzerPatternId("pulse3", parsed));
  CHECK(parsed == BuzzerPattern::PULSE_3HZ);
  CHECK(parseBuzzerPatternId("pulse4", parsed));
  CHECK(parsed == BuzzerPattern::PULSE_4HZ);
  CHECK(parseBuzzerPatternId("pulse5", parsed));
  CHECK(parsed == BuzzerPattern::PULSE_5HZ);
  CHECK(buzzerPatternForExtendedPulseRate(
            static_cast<uint8_t>(ExtendedPulseRate::OFF)) ==
        BuzzerPattern::NONE);
  CHECK(buzzerPatternForExtendedPulseRate(
            static_cast<uint8_t>(ExtendedPulseRate::SLOW)) ==
        BuzzerPattern::PULSE_TRAIN);
  CHECK(buzzerPatternForExtendedPulseRate(
            static_cast<uint8_t>(ExtendedPulseRate::MEDIUM)) ==
        BuzzerPattern::PULSE_3HZ);
  CHECK(buzzerPatternForExtendedPulseRate(
            static_cast<uint8_t>(ExtendedPulseRate::FAST)) ==
        BuzzerPattern::PULSE_4HZ);
  CHECK(buzzerPatternForExtendedPulseRate(
            static_cast<uint8_t>(ExtendedPulseRate::RAPID)) ==
        BuzzerPattern::PULSE_5HZ);

  WebCommand command = webControlCommand(WebCommandType::BUZZER_TEST);
  command.buzzerPattern = BuzzerPattern::PULSE_3HZ;
  processWebCommand(command);
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_3HZ);
  CHECK(localBuzzer.looping);
  CHECK(localBuzzer.onMs == BUZZER_PULSE_TRAIN_ON_MS);
  CHECK(localBuzzer.gapMs == BUZZER_PULSE_TRAIN_3HZ_GAP_MS);
  runLoopAfter(BUZZER_PULSE_TRAIN_DEBUG_MS - 20);
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_3HZ);
  runLoopAfter(40);
  CHECK(!localBuzzer.busy());

  command.buzzerPattern = BuzzerPattern::PULSE_4HZ;
  processWebCommand(command);
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_4HZ);
  CHECK(localBuzzer.onMs == BUZZER_PULSE_TRAIN_ON_MS);
  CHECK(localBuzzer.gapMs == BUZZER_PULSE_TRAIN_4HZ_GAP_MS);
  runLoopAfter(BUZZER_PULSE_TRAIN_DEBUG_MS + 20);
  CHECK(!localBuzzer.busy());

  command.buzzerPattern = BuzzerPattern::PULSE_5HZ;
  processWebCommand(command);
  CHECK(localBuzzer.active == BuzzerPattern::PULSE_5HZ);
  CHECK(localBuzzer.onMs == BUZZER_PULSE_TRAIN_ON_MS);
  CHECK(localBuzzer.gapMs == BUZZER_PULSE_TRAIN_5HZ_GAP_MS);
  runLoopAfter(BUZZER_PULSE_TRAIN_DEBUG_MS + 20);
  CHECK(!localBuzzer.busy());
}

void w91_chime_sequence_uses_irregular_note_timings() {
  resetHarness(false, false);
  CHECK(localBuzzer.request(BuzzerPattern::CHIME));
  CHECK(localBuzzer.active == BuzzerPattern::CHIME);
  CHECK(localBuzzer.beepCount == 3);
  CHECK(localBuzzer.onMs == BUZZER_CHIME_NOTES[0].onMs);
  CHECK(localBuzzer.gapMs == BUZZER_CHIME_NOTES[0].gapMs);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_CHIME_NOTES[0].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_CHIME_NOTES[0].gapMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  CHECK(localBuzzer.onMs == BUZZER_CHIME_NOTES[1].onMs);
  CHECK(localBuzzer.gapMs == BUZZER_CHIME_NOTES[1].gapMs);
  hostMillis += BUZZER_CHIME_NOTES[1].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_CHIME_NOTES[1].gapMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  CHECK(localBuzzer.onMs == BUZZER_CHIME_NOTES[2].onMs);
  hostMillis += BUZZER_CHIME_NOTES[2].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!localBuzzer.busy());
}

void w92_parse_sequence_pattern_ids() {
  BuzzerPattern parsed = BuzzerPattern::NONE;
  CHECK(parseBuzzerPatternId("chime", parsed));
  CHECK(parsed == BuzzerPattern::CHIME);
  CHECK(parseBuzzerPatternId("swing", parsed));
  CHECK(parsed == BuzzerPattern::SWING);
  CHECK(parseBuzzerPatternId("echo", parsed));
  CHECK(parsed == BuzzerPattern::ECHO);
  CHECK(parseBuzzerPatternId("echoinv", parsed));
  CHECK(parsed == BuzzerPattern::ECHO_INVERTED);
  CHECK(parseBuzzerPatternId("morse", parsed));
  CHECK(parsed == BuzzerPattern::MORSE);
  CHECK(parseBuzzerPatternId("snap", parsed));
  CHECK(parsed == BuzzerPattern::SNAP);
}

void w93_scale_connected_echo_on_rising_edge() {
  resetHarness(false, false);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  CHECK(runtimeConfig.buzzerScaleConnectedBeep);
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(true);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_CONNECTED);
  CHECK(localBuzzer.rtttlPlayback);
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER == 1
  CHECK(localBuzzer.beepCount ==
        localBuzzer.cueNoteCount[static_cast<uint8_t>(
            BuzzerCue::SCALE_CONNECTED)]);
#endif
  const uint32_t afterConnect = localBuzzer.acceptedRequests;
  setScaleConnected(true);
  CHECK(localBuzzer.acceptedRequests == afterConnect);
  setScaleConnected(false);
  CHECK(localBuzzer.acceptedRequests == afterConnect + 1);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_LOST ||
        localBuzzer.pendingCue == BuzzerCue::SCALE_LOST);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    hostMillis += 40;
    hostServiceEspTimer(localBuzzer.phaseTimer);
    localBuzzer.service();
  }
  setScaleConnected(true);
  CHECK(localBuzzer.acceptedRequests == afterConnect + 2);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_CONNECTED);
}

void w93b_buzzer_phase_uses_current_time_and_wraps() {
  resetHarness(false, false);
  const BuzzerToneCommand connected =
      deriveBuzzerTone(AlertEvent::SCALE_CONNECTED, false, 0, 0);
  CHECK(localBuzzer.requestTone(connected));
  hostMillis = localBuzzer.phaseStartedAtMs + localBuzzer.onMs;
  localBuzzer.service();
  const uint8_t index = localBuzzer.beepIndex;
  const uint32_t phase = localBuzzer.phaseStartedAtMs;
  const bool on = localBuzzer.toneOn;
  localBuzzer.service();
  CHECK(localBuzzer.beepIndex == index);
  CHECK(localBuzzer.phaseStartedAtMs == phase);
  CHECK(localBuzzer.toneOn == on);
  localBuzzer.stopAll();

  hostMillis = UINT32_MAX - 10;
  CHECK(localBuzzer.requestTone(connected));
  const uint32_t duration = localBuzzer.onMs;
  hostMillis += duration;
  localBuzzer.service();
  CHECK(localBuzzer.phaseStartedAtMs == hostMillis);
  CHECK(localBuzzer.busy());
  localBuzzer.stopAll();
}

void w94_scale_connected_silent_when_flag_off_or_scale_only() {
  resetHarness(false, false);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runtimeConfig.buzzerScaleConnectedBeep = false;
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(true);
  CHECK(localBuzzer.acceptedRequests == before);

  resetHarness(false, false);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  runtimeConfig.buzzerScaleConnectedBeep = true;
  const uint32_t beforePriority = localBuzzer.acceptedRequests;
  setScaleConnected(true);
  CHECK(localBuzzer.acceptedRequests == beforePriority + 1);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_CONNECTED);

  resetHarness(false, false);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  const uint32_t beforeScaleOnly = localBuzzer.acceptedRequests;
  setScaleConnected(true);
  CHECK(localBuzzer.acceptedRequests == beforeScaleOnly);
}

void f01_link_side_effects_run_only_on_control() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runtimeConfig.buzzerScaleConnectedBeep = true;
  runtimeConfig.buzzerScaleLostBeep = true;
  cupPresence.state = CupPresenceState::PRESENT;

  const uint32_t beforeConnect = localBuzzer.acceptedRequests;
  scale.connected = true;
  updateWorkerLinkState();
  CHECK(localBuzzer.acceptedRequests == beforeConnect);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);

  processScaleLinkTransitions();
  CHECK(localBuzzer.acceptedRequests == beforeConnect + 1);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(hostMachineTemperatureRequestCount == 1);
  CHECK(hostMachineTemperaturePresetId == presetBank.activeId);
  CHECK(hostMachineTemperatureTargetDeciC ==
        activeShotPreset(presetBank).lineaMicraBrewTargetDeciC);

  const uint32_t beforeDisconnect = localBuzzer.acceptedRequests;
  scale.connected = false;
  updateWorkerLinkState();
  CHECK(localBuzzer.acceptedRequests == beforeDisconnect);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);

  processScaleLinkTransitions();
  CHECK(localBuzzer.acceptedRequests == beforeDisconnect + 1);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);

  setScaleConnected(true);
  const uint32_t beforeCoalesced = localBuzzer.acceptedRequests;
  for (uint32_t step = 0; step < 160 && localBuzzer.busy(); ++step) {
    hostMillis += 40;
    hostServiceEspTimer(localBuzzer.phaseTimer);
    localBuzzer.service();
  }
  CHECK(!localBuzzer.busy());
  scale.connected = false;
  updateWorkerLinkState();
  scale.connected = true;
  updateWorkerLinkState();
  processScaleLinkTransitions();
  CHECK(localBuzzer.acceptedRequests == beforeCoalesced + 2);
  CHECK(hostMachineTemperatureRequestCount == 3);
  processScaleLinkTransitions();
  CHECK(localBuzzer.acceptedRequests == beforeCoalesced + 2);
  CHECK(hostMachineTemperatureRequestCount == 3);

  scale.bleDiagnostics.commandFailureSequence = 1;
  scale.bleDiagnostics.commandStatus = 15;
  updateWorkerLinkState();
  processScaleLinkTransitions();
  CHECK(localBuzzer.acceptedRequests == beforeCoalesced + 2);
  publishControlStatus();
  ControlStatusSnapshot published;
  copyControlStatus(published);
  CHECK(published.scaleBleDiagnostics.commandStatus == 15);
}

void f01_worker_uses_only_published_policy() {
  resetHarness(false, false);
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  publishTestScaleWorkerPolicy();
  CHECK(currentScaleMacCacheMode() == ScaleMacCacheMode::FIRST);

  // Mutating control-owned state cannot be observed until control publishes.
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  CHECK(currentScaleMacCacheMode() == ScaleMacCacheMode::FIRST);
  publishTestScaleWorkerPolicy();
  CHECK(currentScaleMacCacheMode() == ScaleMacCacheMode::ONLY);
}

void w95_web_buzzer_test_plays_chime_sequence() {
  resetHarness(false, false);
  reachReadyFromBoot();
  BuzzerPattern parsed = BuzzerPattern::NONE;
  CHECK(parseBuzzerPatternId("chime", parsed));
  CHECK(parsed == BuzzerPattern::CHIME);
  WebCommand command = webControlCommand(WebCommandType::BUZZER_TEST);
  command.buzzerPattern = BuzzerPattern::CHIME;
  const uint32_t before = localBuzzer.acceptedRequests;
  processWebCommand(command);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.active == BuzzerPattern::CHIME);
  CHECK(localBuzzer.beepCount == 3);
  CHECK(localBuzzer.onMs == BUZZER_CHIME_NOTES[0].onMs);
}

void w96_echo_inverted_uses_long_bookend_tones() {
  resetHarness(false, false);
  CHECK(BUZZER_ECHO_INVERTED_NOTES[0].onMs == 220);
  CHECK(BUZZER_ECHO_INVERTED_NOTES[3].onMs == 220);
  CHECK(BUZZER_ECHO_INVERTED_NOTES[1].onMs == 50);
  CHECK(BUZZER_ECHO_INVERTED_NOTES[2].onMs == 50);
  CHECK(BUZZER_ECHO_INVERTED_NOTES[3].gapMs == 0);
  CHECK(localBuzzer.request(BuzzerPattern::ECHO_INVERTED));
  CHECK(localBuzzer.active == BuzzerPattern::ECHO_INVERTED);
  CHECK(localBuzzer.beepCount == 4);
  CHECK(localBuzzer.onMs == BUZZER_ECHO_INVERTED_NOTES[0].onMs);
  CHECK(localBuzzer.gapMs == BUZZER_ECHO_INVERTED_NOTES[0].gapMs);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[0].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[0].gapMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  CHECK(localBuzzer.onMs == BUZZER_ECHO_INVERTED_NOTES[1].onMs);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[1].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[1].gapMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  CHECK(localBuzzer.onMs == BUZZER_ECHO_INVERTED_NOTES[2].onMs);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[2].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[2].gapMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  CHECK(localBuzzer.onMs == BUZZER_ECHO_INVERTED_NOTES[3].onMs);
  hostMillis += BUZZER_ECHO_INVERTED_NOTES[3].onMs;
  hostServiceEspTimer(localBuzzer.phaseTimer);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(!localBuzzer.busy());

  reachReadyFromBoot();
  BuzzerPattern parsed = BuzzerPattern::NONE;
  CHECK(parseBuzzerPatternId("echoinv", parsed));
  CHECK(parsed == BuzzerPattern::ECHO_INVERTED);
  WebCommand command = webControlCommand(WebCommandType::BUZZER_TEST);
  command.buzzerPattern = BuzzerPattern::ECHO_INVERTED;
  const uint32_t before = localBuzzer.acceptedRequests;
  processWebCommand(command);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.active == BuzzerPattern::ECHO_INVERTED);
}

void w98_buzzer_sequences_start_and_end_with_sound() {
  const BuzzerPattern patterns[] = {
      BuzzerPattern::CHIME,          BuzzerPattern::SWING,
      BuzzerPattern::ECHO,           BuzzerPattern::ECHO_INVERTED,
      BuzzerPattern::MORSE,          BuzzerPattern::SNAP,
      BuzzerPattern::RECOVERY_NETWORK_OK, BuzzerPattern::RECOVERY_FACTORY_OK,
      BuzzerPattern::RECOVERY_ERROR};
  for (const BuzzerPattern pattern : patterns) {
    uint8_t count = 0;
    const BuzzerNote *notes = buzzerSequenceNotes(pattern, count);
    CHECK(buzzerNotesStartAndEndWithSound(notes, count));
  }
}

void w60_web_buzzer_test_plays_requested_pattern() {
  resetHarness(false, false);
  reachReadyFromBoot();
  BuzzerPattern parsed = BuzzerPattern::NONE;
  CHECK(parseBuzzerPatternId("triple", parsed));
  CHECK(parsed == BuzzerPattern::TRIPLE);
  WebCommand command = webControlCommand(WebCommandType::BUZZER_TEST);
  command.buzzerPattern = BuzzerPattern::TRIPLE;
  const uint32_t before = localBuzzer.acceptedRequests;
  processWebCommand(command);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(localBuzzer.active == BuzzerPattern::TRIPLE);
}

void w61_web_buzzer_test_rejected_while_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  WebCommand command = webControlCommand(WebCommandType::BUZZER_TEST);
  command.buzzerPattern = BuzzerPattern::SINGLE;
  const uint32_t before = localBuzzer.acceptedRequests;
  processWebCommand(command);
  CHECK(localBuzzer.acceptedRequests == before);
  CHECK(session.active);
}

bool executePendingScaleDebugCommand() {
  BookooDebugAction action = BookooDebugAction::START;
  uint8_t level = 0;
  if (!takeScaleDebugCommand(action, level)) {
    return false;
  }
  executeScaleDebugCommand(action, level);
  return true;
}

void w66_web_bookoo_debug_dispatches_actions() {
  static const struct {
    const char *id;
    BookooDebugAction action;
    uint8_t level;
    const char *expected;
  } cases[] = {
      {"start", BookooDebugAction::START, 0, "startTimer"},
      {"stop", BookooDebugAction::STOP, 0, "stopTimer"},
      {"tare", BookooDebugAction::TARE, 0, "tare"},
      {"combined", BookooDebugAction::COMBINED, 0, "tareStartTimer"},
      {"beep", BookooDebugAction::BEEP, 0, "beepWithoutStateChange"},
      {"volume", BookooDebugAction::VOLUME, 0, "setBeepLevel:0"},
      {"volume", BookooDebugAction::VOLUME, 3, "setBeepLevel:3"},
      {"volume", BookooDebugAction::VOLUME, 5, "setBeepLevel:5"},
  };
  for (const auto &entry : cases) {
    BookooDebugAction parsed = BookooDebugAction::START;
    CHECK(parseBookooDebugActionId(entry.id, parsed));
    CHECK(parsed == entry.action);
    resetHarness(false, true);
    reachReadyFromBoot();
    scale.commandLog.clear();
    WebCommand command = webControlCommand(WebCommandType::BOOKOO_DEBUG);
    command.bookooDebugAction = entry.action;
    command.bookooBeepLevel = entry.level;
    processWebCommand(command);
    CHECK(scaleDebugPending);
    CHECK(executePendingScaleDebugCommand());
    CHECK(scale.commandLog.size() == 1);
    CHECK(scale.commandLog[0] == entry.expected);
  }
}

void w67_web_bookoo_debug_rejected_while_active() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  WebCommand command = webControlCommand(WebCommandType::BOOKOO_DEBUG);
  command.bookooDebugAction = BookooDebugAction::TARE;
  processWebCommand(command);
  CHECK(!scaleDebugPending);
  CHECK(session.active);
}

void w68_web_bookoo_debug_rejected_when_disconnected() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand command = webControlCommand(WebCommandType::BOOKOO_DEBUG);
  command.bookooDebugAction = BookooDebugAction::START;
  processWebCommand(command);
  CHECK(!scaleDebugPending);
  CHECK(scale.commandLog.empty());
}

void w69_web_bookoo_debug_rejected_when_not_generic() {
  resetHarness(false, true);
  reachReadyFromBoot();
  std::strncpy(scale.connectedProtocol, "acaia",
               sizeof(scale.connectedProtocol) - 1);
  scale.independentBeepSupported = false;
  scale.tareStartTimerSupported = false;
  updateWorkerLinkState();
  WebCommand command = webControlCommand(WebCommandType::BOOKOO_DEBUG);
  command.bookooDebugAction = BookooDebugAction::TARE;
  processWebCommand(command);
  CHECK(!scaleDebugPending);
  CHECK(scale.commandLog.empty());
}

void w70_bookoo_connect_mutes_in_buzzer_only() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooMuteOnBuzzerOnly = true;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  publishTestScaleWorkerPolicy();
  scale.commandLog.clear();
  applyBookooConnectBeepPolicy();
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:0");
}

void w71_bookoo_connect_sets_volume_in_scale_priority() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooMuteOnBuzzerOnly = true;
  runtimeConfig.bookooConnectBeepLevel = DEFAULT_BOOKOO_CONNECT_BEEP_LEVEL;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  publishTestScaleWorkerPolicy();
  scale.commandLog.clear();
  applyBookooConnectBeepPolicy();
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:3");
}

void w72_bookoo_connect_mutes_disabled_volume_and_skips_non_bookoo() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooConnectBeepLevel = 0;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  publishTestScaleWorkerPolicy();
  scale.commandLog.clear();
  applyBookooConnectBeepPolicy();
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:0");

  scale.commandLog.clear();
  runtimeConfig.bookooConnectBeepLevel = 4;
  std::strncpy(scale.connectedProtocol, "acaia",
               sizeof(scale.connectedProtocol) - 1);
  scale.independentBeepSupported = false;
  updateWorkerLinkState();
  applyBookooConnectBeepPolicy();
  CHECK(scale.commandLog.empty());
}

void w73_apply_config_buzzer_only_sends_bookoo_silence() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooMuteOnBuzzerOnly = true;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  scale.commandLog.clear();
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  processWebCommand(update);
  CHECK(scaleDebugPending);
  CHECK(executePendingScaleDebugCommand());
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:0");
}

void w74_apply_config_enabling_mute_sends_silence_only_in_buzzer_only() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooMuteOnBuzzerOnly = false;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  scale.commandLog.clear();
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.bookooMuteOnBuzzerOnly = true;
  processWebCommand(update);
  CHECK(scaleDebugPending);
  CHECK(executePendingScaleDebugCommand());
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:0");

  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooMuteOnBuzzerOnly = false;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  scale.commandLog.clear();
  update = WebCommand{};
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.bookooMuteOnBuzzerOnly = true;
  processWebCommand(update);
  CHECK(!scaleDebugPending);
  CHECK(scale.commandLog.empty());
}

void w74b_sound_alert_master_mutes_and_cancels_all_routes() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  CHECK(emitAlert(AlertEvent::FIRST_DROP, 7));
  CHECK(scaleBeepPending);

  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.soundAlertsMuted = true;
  processWebCommand(update);
  CHECK(runtimeConfig.soundAlertsMuted);
  CHECK(!scaleBeepPending);
  CHECK(!scaleCompletionBeepScheduled);
  CHECK(!emitAlert(AlertEvent::FIRST_DROP, 8));
  CHECK(!scaleBeepPending);

  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  const uint32_t accepted = localBuzzer.acceptedRequests;
  emitImmediateCommandAlertIfBuzzer(AlertEvent::START_TIMER);
  CHECK(localBuzzer.acceptedRequests == accepted);
  requestCompletionAlert();
  CHECK(!scaleCompletionBeepScheduled);

  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  CHECK(emitAlert(AlertEvent::SCALE_CONNECTED));
  CHECK(localBuzzer.busy());
  update = WebCommand{};
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.soundAlertsMuted = true;
  processWebCommand(update);
  CHECK(!localBuzzer.busy());
}

void w74c_unrelated_config_save_does_not_repeat_live_effects() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t cupGeneration = cupSettingsGeneration;
  const uint32_t idleGeneration = idleTareSettingsGeneration;
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.serialLogLevel = static_cast<uint8_t>(LogLevel::DEBUG);
  processWebCommand(update);
  CHECK(!scaleDebugPending);
  CHECK(cupSettingsGeneration == cupGeneration);
  CHECK(idleTareSettingsGeneration == idleGeneration);

  update.config = runtimeConfig;
  update.config.bookooConnectBeepLevel = 2;
  processWebCommand(update);
  CHECK(scaleDebugPending);
  CHECK(executePendingScaleDebugCommand());
  CHECK(scale.commandLog.back() == "setBeepLevel:2");
  const size_t commandCountBefore = scale.commandLog.size();

  update.config = runtimeConfig;
  processWebCommand(update);
  CHECK(!scaleDebugPending);
  CHECK(scale.commandLog.size() == commandCountBefore);

  update.config = runtimeConfig;
  update.config.minimumCupWeightG += 1.0f;
  update.config.autoTareOutsideBrew = !update.config.autoTareOutsideBrew;
  processWebCommand(update);
  CHECK(cupSettingsGeneration == cupGeneration + 1);
  CHECK(idleTareSettingsGeneration == idleGeneration + 1);
  CHECK(!scaleDebugPending);
}

void w74d_settings_dispatches_each_matching_subscriber_once() {
  static unsigned logCalls = 0;
  static unsigned volumeCalls = 0;
  logCalls = volumeCalls = 0;
  const RuntimeSettingsSubscription subscribers[] = {
      {[](const RuntimeConfig &a, const RuntimeConfig &b) {
         return a.serialLogLevel != b.serialLogLevel;
       }, [](const RuntimeConfig &, const RuntimeConfig &) { ++logCalls; }},
      {[](const RuntimeConfig &a, const RuntimeConfig &b) {
         return a.bookooConnectBeepLevel != b.bookooConnectBeepLevel;
       }, [](const RuntimeConfig &, const RuntimeConfig &) { ++volumeCalls; }},
  };
  const RuntimeConfig before;
  RuntimeConfig after = before;
  dispatchRuntimeSettingsChanges(before, after, subscribers);
  CHECK(logCalls == 0 && volumeCalls == 0);
  after.serialLogLevel = static_cast<uint8_t>(LogLevel::DEBUG);
  after.bookooConnectBeepLevel = 2;
  dispatchRuntimeSettingsChanges(before, after, subscribers);
  CHECK(logCalls == 1 && volumeCalls == 1);
  dispatchRuntimeSettingsChanges(after, after, subscribers);
  CHECK(logCalls == 1 && volumeCalls == 1);
}

void w75_bookoo_discovery_connect_applies_beep_policy() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.bookooMuteOnBuzzerOnly = true;
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  publishTestScaleWorkerPolicy();
  scale.scanning = true;
  scale.connected = true;
  scale.commandLog.clear();
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.commandLog.empty());
  ScaleHistoryEntry copiedHistory[SCALE_HISTORY_CAPACITY] = {};
  copyScaleHistory(copiedHistory);
  bool copiedIdentityFound = false;
  for (const ScaleHistoryEntry &entry : copiedHistory) {
    if (preferredScaleMacEqual(entry.mac, scale.connectedAddress)) {
      copiedIdentityFound = true;
      CHECK(!scaleHistorySessionConsumed(entry));
    }
  }
  CHECK(copiedIdentityFound);

  hostMillis += 10000;
  serviceScaleWorkerLink();
  CHECK(scale.commandLog.empty());
  scale.newWeightAvailableValue = true;
  hostMillis += SCALE_WEIGHT_POLL_INTERVAL_MS;
  serviceScaleWorkerLink();
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:0");

  // Reconnecting the same identity never rearms automatic volume.
  setScaleConnected(false);
  setScaleConnected(true);
  armBookooConnectBeepPolicy();
  scale.newWeightAvailableValue = true;
  serviceScaleWorkerLink();
  CHECK(scale.commandLog.size() == 1);

  // Consuming occurs at connection time: a drop before weight still prevents
  // the recovered link from sending volume.
  resetHarness(false, true);
  reachReadyFromBoot();
  armBookooConnectBeepPolicy();
  CHECK(bookooConnectVolumePending);
  setScaleConnected(false);
  serviceScaleWorkerLink();
  CHECK(!bookooConnectVolumePending);
  setScaleConnected(true);
  armBookooConnectBeepPolicy();
  CHECK(!bookooConnectVolumePending);

  // A failed first write is also consumed and cannot be replayed.
  resetHarness(false, true);
  reachReadyFromBoot();
  armBookooConnectBeepPolicy();
  scale.beepSucceeds = false;
  scale.newWeightAvailableValue = true;
  serviceScaleWorkerLink();
  CHECK(scale.setBeepLevelCalls == 1);
  setScaleConnected(true);
  armBookooConnectBeepPolicy();
  CHECK(!bookooConnectVolumePending);

  // Explicit forget reopens the session for that identity.
  resetHarness(false, true);
  reachReadyFromBoot();
  scalePreferredMacMux.lock();
  copyCString(scalePreferredMac, sizeof(scalePreferredMac),
              scale.connectedAddress);
  scalePreferredMacMux.unlock();
  armBookooConnectBeepPolicy();
  CHECK(bookooConnectVolumePending);
  cancelBookooConnectBeepPolicy();
  clearPreferredScaleCache();
  armBookooConnectBeepPolicy();
  CHECK(bookooConnectVolumePending);
}

void w75b_old_debug_volume_and_beeps_do_not_cross_connections() {
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(enqueueScaleDebugCommand(BookooDebugAction::VOLUME, 0));
  requestScaleBrewBeep(77);
  setScaleConnected(false);
  setScaleConnected(true);
  BookooDebugAction action = BookooDebugAction::START;
  uint8_t level = 9;
  uint32_t cycleId = 0;
  CHECK(!takeScaleDebugCommand(action, level));
  CHECK(!takeScaleBrewBeep(cycleId));
  CHECK(scale.commandLog.empty());
  DebugEvent rejected[8] = {};
  const size_t rejectedCount = copyDebugEvents(0, rejected, 8);
  bool sawVolume = false;
  for (size_t index = 0; index < rejectedCount; ++index) {
    sawVolume |= rejected[index].code == DebugCode::SCALE_STALE_EVENT_REJECTED &&
                 strstr(rejected[index].text,
                        "debug=volume reason=generation mismatch") != nullptr;
  }
  CHECK(sawVolume);

  CHECK(enqueueScaleDebugCommand(BookooDebugAction::VOLUME, 0));
  CHECK(takeScaleDebugCommand(action, level));
  executeScaleDebugCommand(action, level);
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:0");
  requestScaleBrewBeep(78);
  CHECK(takeScaleBrewBeep(cycleId));
  CHECK(cycleId == 78);
}

void w76_buzzer_only_start_beeps_at_circuit_not_ble_result() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

void w77_scale_priority_disconnected_beeps_on_circuit_without_ble() {
  resetHarness(false, false);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(localBuzzer.acceptedRequests == before + 2);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  // Drain manual-no-scale TRIPLE so stop completion LONG can start cleanly.
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t afterStart = localBuzzer.acceptedRequests;
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == afterStart + 1);
  checkLocalCompletionTone();
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void w78_scale_priority_connected_start_beeps_at_circuit() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

void w79_buzzer_only_stop_beeps_before_timer_stop_result() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  for (uint32_t step = 0; step < 20 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t before = localBuzzer.acceptedRequests;
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  checkLocalCompletionTone();
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

void w79b_stop_beeps_at_circuit_while_scale_timer_stop_still_pending() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runtimeConfig.scaleTimerStopExtraDelayMs = 200;
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  drainLocalBuzzer();
  const uint32_t before = localBuzzer.acceptedRequests;
  publishScaleTimer(5000);
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  checkLocalCompletionTone();
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
}

void w79c_rinse_without_scale_beeps_at_circuit_open_and_close() {
  resetHarness(false, false);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runtimeConfig.buzzerManualNoScaleBeep = false;
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  processWebCommand(webControlCommand(WebCommandType::RINSE));
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  drainLocalBuzzer();
  const uint32_t afterStart = localBuzzer.acceptedRequests;
  finalizeCycle(EndReason::RINSE_COMPLETE, StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == afterStart + 1);
  checkLocalCompletionTone();
}

void w79d_scale_only_start_and_stop_still_use_local_circuit_beeps() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  reachReadyFromBoot();
  const uint32_t before = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(executeNextScaleCommand());
  drainLocalBuzzer();
  const uint32_t afterStart = localBuzzer.acceptedRequests;
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(localBuzzer.acceptedRequests == afterStart + 1);
  checkLocalCompletionTone();
}

void w80_buzzer_only_retare_beeps_before_tare_result() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t before = localBuzzer.acceptedRequests;
  publishStableCupWeight(150.0f, 10);
  CHECK(session.retarePerformed);
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

void w81_scale_priority_failed_start_falls_back_after_disconnect() {
  resetHarness(false, true);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  reachReadyFromBoot();
  startCycle();
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t before = localBuzzer.acceptedRequests;
  setScaleConnected(false);
  // Scale-lost RTTTL is current. The pending start belongs to the old link and
  // must settle without replaying its fallback cue after disconnect.
  CHECK(localBuzzer.acceptedRequests == before + 1);
  CHECK(executeNextScaleCommand());
  CHECK(localBuzzer.acceptedRequests == before + 1);
}

void configureEclairCapabilities() {
  strncpy(scale.connectedProtocol, "atomheart_eclair",
          sizeof(scale.connectedProtocol) - 1);
  scale.connectedProtocol[sizeof(scale.connectedProtocol) - 1] = '\0';
  scale.tareStartTimerSupported = false;
  scale.independentBeepSupported = false;
  scale.commandFeedbackSupported = false;
  updateWorkerLinkState();
}

void w94_eclair_scale_priority_uses_local_alerts() {
  resetHarness(false, true);
  configureEclairCapabilities();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  reachReadyFromBoot();
  const uint32_t startAlerts = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(localBuzzer.acceptedRequests == startAlerts + 1);
  CHECK(executeNextScaleCommand());
  CHECK(scale.commandLog.size() == 3);
  CHECK(scale.commandLog[0] == "resetTimer");
  CHECK(scale.commandLog[1] == "startTimer");
  CHECK(scale.commandLog[2] == "tare");
  CHECK(scale.beepCalls == 0);
  CHECK(scale.setBeepLevelCalls == 0);
  establishPostTareBaseline();
  advanceToBrew();
  drainLocalBuzzer();

  const uint32_t retareAlerts = localBuzzer.acceptedRequests;
  CHECK(requestRemoteRetare());
  emitImmediateCommandAlertIfBuzzer(AlertEvent::TARE);
  CHECK(localBuzzer.acceptedRequests == retareAlerts + 1);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);

  drainLocalBuzzer();
  const uint32_t firstDropAlerts = localBuzzer.acceptedRequests;
  simulateFirstDrops();
  CHECK(localBuzzer.acceptedRequests == firstDropAlerts + 1);
  CHECK(!scaleBeepPending);
  CHECK(scale.beepCalls == 0);
}

void w95_eclair_scale_only_omits_unsupported_alerts() {
  resetHarness(false, true);
  configureEclairCapabilities();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  reachReadyFromBoot();
  const uint32_t alerts = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(localBuzzer.acceptedRequests == alerts + 1);
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  advanceToBrew();
  drainLocalBuzzer();
  const uint32_t afterStart = localBuzzer.acceptedRequests;
  simulateFirstDrops();
  CHECK(localBuzzer.acceptedRequests == afterStart);
  CHECK(!scaleBeepPending);
  CHECK(scale.beepCalls == 0);
  CHECK(scale.setBeepLevelCalls == 0);
}

void w97_eclair_buzzer_only_never_queues_scale_beeps() {
  resetHarness(false, true);
  configureEclairCapabilities();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  reachReadyFromBoot();
  const uint32_t startAlerts = localBuzzer.acceptedRequests;
  startCycle();
  CHECK(localBuzzer.acceptedRequests == startAlerts + 1);
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  advanceToBrew();
  drainLocalBuzzer();
  const uint32_t firstDropAlerts = localBuzzer.acceptedRequests;
  simulateFirstDrops();
  CHECK(localBuzzer.acceptedRequests == firstDropAlerts + 1);
  CHECK(!scaleBeepPending);
  CHECK(scale.beepCalls == 0);
  CHECK(scale.setBeepLevelCalls == 0);
}

void setHostPreferredScaleMac(const char *mac) {
  strncpy(scalePreferredMac, mac, PREFERRED_SCALE_MAC_CAPACITY - 1);
  scalePreferredMac[PREFERRED_SCALE_MAC_CAPACITY - 1] = '\0';
}

void d01_idle_scan_stays_enabled_between_ticks() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  CHECK(scale.lastAddressScan);
  CHECK(scale.startScanCalls == 1);
  const size_t calls = scale.startScanCalls;
  hostMillis += SCALE_DISCOVERY_TICK_MS - 1;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  CHECK(scale.startScanCalls == calls);
}

void d13_idle_delays_relax_without_scale() {
  resetHarness(false, false);
  CHECK(!scale.isConnected());
  CHECK(!scale.isConnecting());
  CHECK(scaleWorkerTickDelayMs() == SCALE_WORKER_NO_SCALE_DELAY_MS);
  CHECK(controlLoopTickDelayMs(getScaleLinkSnapshot()) ==
        LOOP_NO_SCALE_DELAY_MS);

  scale.connecting = true;
  CHECK(scaleWorkerTickDelayMs() == 1);
  CHECK(controlLoopTickDelayMs(getScaleLinkSnapshot()) ==
        LOOP_NO_SCALE_DELAY_MS);
  scale.connecting = false;

  ScaleCommand command;
  command.type = ScaleCommandType::TARE_ONLY;
  CHECK(xQueueSend(scaleCommandQueue, &command, 0) == pdTRUE);
  CHECK(scaleWorkerTickDelayMs() == 1);
  CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
  CHECK(scaleWorkerTickDelayMs() == SCALE_WORKER_NO_SCALE_DELAY_MS);

  setScaleConnected(true);
  markScaleWorkerProgress();
  CHECK(scaleWorkerTickDelayMs() == SCALE_WEIGHT_POLL_INTERVAL_MS);
  CHECK(controlLoopTickDelayMs(getScaleLinkSnapshot()) == 1);
}

void d13b_scale_power_off_is_terminal_for_connection_generation() {
  resetHarness(false, false);
  scale.connectedFeatures.flags |= ScaleFeaturePowerOff;
  setScaleConnected(true);
  const uint32_t generation = getScaleLinkSnapshot().connectionGeneration;

  ScaleCommand queued;
  queued.type = ScaleCommandType::TARE_ONLY;
  CHECK(enqueueScaleCommand(queued));
  requestScaleBrewBeep(1);
  CHECK(enqueueScaleDebugCommand(BookooDebugAction::VOLUME, 0));

  requestScalePowerOff();
  CHECK(scalePowerOffBlocksGeneration(generation));
  CHECK(!scaleBeepPending);
  CHECK(!scaleDebugPending);
  CHECK(!enqueueScaleCommand(queued));
  CHECK(takeScalePowerOff());
  executeScalePowerOffCommand();
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "powerOff");
  uint32_t lastScanCycleMs=0, lastConnectLogMs=0, scanSessionAtMs=0;
  uint32_t scanLastAdvertAtMs=0;
  bool connectAttemptSeriesActive=false;
  serviceScaleWorkerDiscovery(lastScanCycleMs,lastConnectLogMs,
      connectAttemptSeriesActive,scanSessionAtMs,scanLastAdvertAtMs);
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::CONNECTED);
  CHECK(scalePowerOffBlocksGeneration(generation));
  CHECK(scale.startScanCalls == 0);
  requestScalePowerOff();
  CHECK(!takeScalePowerOff());

  CHECK(executeNextScaleCommand());
  CHECK(scale.commandLog.size() == 1);
  hostMillis += SCALE_POWER_OFF_DISCONNECT_TIMEOUT_MS;
  serviceScalePowerOffTimeout(hostMillis);
  CHECK(!scale.isConnected());
  CHECK(!scalePowerOffBlocksGeneration(generation));
  CHECK(!scaleDiscoveryPaused());
  CHECK(scale.communicationSilenceRemainingMs() ==
        SCALE_DISCONNECT_SILENCE_MS);
  serviceScaleWorkerDiscovery(lastScanCycleMs,lastConnectLogMs,
      connectAttemptSeriesActive,scanSessionAtMs,scanLastAdvertAtMs);
  CHECK(scale.startScanCalls==0);
  hostMillis+=SCALE_DISCONNECT_SILENCE_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs,lastConnectLogMs,
      connectAttemptSeriesActive,scanSessionAtMs,scanLastAdvertAtMs);
  CHECK(!scaleDiscoveryPaused()); CHECK(scale.startScanCalls==1);
}

bool scaleWarningContains(const char *fragment) {
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  for (size_t i = 0; i < count; ++i) {
    if (events[i].level == LogLevel::WARNING &&
        strstr(events[i].text, fragment) != nullptr) return true;
  }
  return false;
}

void d13d_known_mini_rejects_shutdown_before_terminal_barrier() {
  resetHarness(false, false);
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName),
              "BOOKOO_SC 715097");
  scale.connectedFeatures.flags |= ScaleFeaturePowerOff;
  setScaleConnected(true);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  CHECK(link.model == ScaleModel::BookooMini);
  CHECK(!link.features.has(ScaleFeaturePowerOff));
  requestScaleBrewBeep(1);
  CHECK(scaleBeepPending);
  CHECK(enqueueScaleDebugCommand(BookooDebugAction::VOLUME, 3));
  CHECK(scaleDebugPending);
  requestScalePowerOff();
  CHECK(!scalePowerOffBlocksGeneration(link.connectionGeneration));
  CHECK(!takeScalePowerOff());
  CHECK(scaleBeepPending);
  CHECK(scaleDebugPending);
  CHECK(scale.commandLog.empty());
  CHECK(scaleWarningContains("Power off not sent: Bookoo Themis Mini"));
  setScaleConnected(false);
  CHECK(getScaleLinkSnapshot().model == ScaleModel::Unknown);
}

void d13e_ultra_volume_rejection_preserves_saved_level() {
  resetHarness(false, false);
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName),
              "BOOKOO_SC_U 369041");
  setScaleConnected(true);
  CHECK(getScaleLinkSnapshot().model == ScaleModel::BookooUltra);
  CHECK(scale.features().volumeMax == 3);
  runtimeConfig.bookooConnectBeepLevel = 5;
  publishTestScaleWorkerPolicy();
  applyBookooConnectBeepPolicy();
  CHECK(runtimeConfig.bookooConnectBeepLevel == 5);
  CHECK(scale.commandLog.empty());
  CHECK(scaleWarningContains("Connection volume 5 not sent"));
  executeScaleDebugCommand(BookooDebugAction::VOLUME, 4);
  CHECK(scale.commandLog.empty());
  CHECK(scaleWarningContains("volume level 4 is unsupported"));
  executeScaleDebugCommand(BookooDebugAction::VOLUME, 3);
  CHECK(scale.commandLog.size() == 1);
  CHECK(scale.commandLog[0] == "setBeepLevel:3");
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName),
              "BOOKOO_SC U");
  updateWorkerLinkState();
  CHECK(getScaleLinkSnapshot().model == ScaleModel::Unknown);
  CHECK(scale.features().volumeMax == 5);
}

void d13c_disconnect_silence_clears_connecting_snapshot() {
  resetHarness(false, false);
  scale.connecting = true;
  scaleLoggedGattConnecting = true;
  updateWorkerLinkState();
  CHECK(desiredScaleConnectedLedPattern(getScaleLinkSnapshot()) ==
        ScaleConnectedLedPattern::FAST_BLINK);

  scale.connecting = false;
  scale.silenceArmed = true;
  scale.silenceStartedAtMs = hostMillis;
  uint32_t lastScanCycleMs = 0, lastConnectLogMs = 0, scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  bool connectAttemptSeriesActive = false;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
      connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(!getScaleLinkSnapshot().connecting);
  CHECK(desiredScaleConnectedLedPattern(getScaleLinkSnapshot()) ==
        ScaleConnectedLedPattern::OFF);
  hostMillis += 2999;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
      connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.communicationSilenceRemainingMs() == 1);
  CHECK(scale.startScanCalls == 0);
  ++hostMillis;
  CHECK(!scale.communicationSilenced());
}

void d15_control_housekeeping_has_wrap_safe_10ms_cadence() {
  resetHarness(false, false);
  CHECK(controlHousekeepingDue(0));
  CHECK(!controlHousekeepingDue(CONTROL_HOUSEKEEPING_INTERVAL_MS - 1));
  CHECK(controlHousekeepingDue(CONTROL_HOUSEKEEPING_INTERVAL_MS));
  lastControlHousekeepingAtMs = UINT32_MAX - 4;
  CHECK(!controlHousekeepingDue(3));
  CHECK(controlHousekeepingDue(5));
}

void d16_cloud_pause_requires_a_real_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  startCycle();
  CHECK(session.active && outboundShotActive.load());
  CHECK(!outboundScaleConnected.load());
  CHECK(enterRinse());
  CHECK(!outboundShotActive.load() && !outboundScaleConnected.load());

  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(noScaleShotGuardHold);
  CHECK(!session.active);
  CHECK(!outboundShotActive.load());
  CHECK(hostMachineCloudInhibitCount == 0);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!outboundShotActive.load());

  resetHarness(false, true);
  reachReadyFromBoot();
  beginCycle(ControlSource::WEB);
  CHECK(session.active);
  CHECK(outboundShotActive.load());
  CHECK(hostMachineCloudInhibitCount > 0);
  setRawPaddle(false);
  machineRequestStop();
  session.active = false;
  publishControlGate();
  CHECK(!outboundShotActive.load());
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(beginRinseCycle(ControlSource::WEB));
  CHECK(!outboundShotActive.load());
  CHECK(enterRinse());
  CHECK(!outboundShotActive.load());
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(outboundShotActive.load());
  CHECK(enterRinse());
  CHECK(session.active && getRelaySafetySnapshot().closed);
  CHECK(!outboundShotActive.load());
}

void d14_control_status_publishes_on_cycle_edge() {
  resetHarness(false, false);
  reachReadyFromBoot();
  publishControlStatus();
  CHECK(!publishedControlStatus.activeCycle);
  hostMillis += 10;
  startCycle();
  CHECK(publishedControlStatus.activeCycle);
  CHECK(publishedControlStatus.state == StopperState::BREW ||
        publishedControlStatus.state == StopperState::MANUAL_NO_SCALE);
  ControlGateSnapshot gate;
  copyControlGate(gate);
  CHECK(gate.activeCycle);
  CHECK(gate.relayClosed);

  const uint32_t publishedAtEdge = publishedControlStatus.uptimeMs;
  serviceControlStatusPublish();
  CHECK(publishedControlStatus.uptimeMs == publishedAtEdge);
  ++hostMillis;
  controlStatusPublishRequested = true;
  serviceControlStatusPublish();
  CHECK(publishedControlStatus.uptimeMs == publishedAtEdge + 1U);
  CHECK(!controlStatusPublishRequested);
}


void d02_first_mode_uses_name_scan() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  scalePreferredMac[0] = '\0';
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
  CHECK(!scale.lastAddressScan);
  CHECK(scale.lastStartScanMac[0] == '\0');
}

void d02b_only_without_preferred_bootstraps_and_adopts_on_connect() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  scalePreferredMac[0] = '\0';
  scalePreferredName[0] = '\0';
  scalePreferredMacDirty = false;
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
  CHECK(!scale.lastAddressScan);
  CHECK(scalePreferredMac[0] == '\0');

  strncpy(scale.seenMac, scale.connectedAddress, sizeof(scale.seenMac) - 1);
  strncpy(scale.seenName, scale.connectedLocalName, sizeof(scale.seenName) - 1);
  scale.seenPending = true;
  hostMillis += SCALE_DISCOVERY_TICK_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scalePreferredMac[0] == '\0');
  CHECK(!scalePreferredMacDirty);

  scale.pollScanConnects = true;
  scale.pollScanStepsToConnect = 1;
  hostMillis += SCALE_DISCOVERY_TICK_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.connected);
  CHECK(preferredScaleMacEqual(scalePreferredMac, scale.connectedAddress));
  CHECK(strcmp(scalePreferredName, scale.connectedLocalName) == 0);
  CHECK(scalePreferredMacDirty);
}

void d02c_prefer_adopts_an_already_connected_scale() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::PREFER);
  scalePreferredMac[0] = '\0';
  scalePreferredName[0] = '\0';
  scalePreferredMacDirty = false;
  scale.connected = true;
  serviceScaleWorkerLink();
  CHECK(preferredScaleMacEqual(scalePreferredMac, scale.connectedAddress));
  CHECK(strcmp(scalePreferredName, scale.connectedLocalName) == 0);
  CHECK(scalePreferredMacDirty);
}

void d02d_apply_only_without_preferred_is_accepted() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  scalePreferredMac[0] = '\0';
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  processWebCommand(update);
  CHECK(runtimeConfig.scaleMacCacheMode ==
        static_cast<uint8_t>(ScaleMacCacheMode::ONLY));
}

void d02e_mode_change_restarts_an_active_scan_with_the_new_filter() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
  const size_t calls = scale.startScanCalls;

  RuntimeConfig candidate = runtimeConfig;
  candidate.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.startScanCalls == calls + 1);
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  CHECK(scale.lastAddressScan);
  CHECK(strcmp(scale.lastStartScanMac, "AA:BB:CC:DD:EE:FF") == 0);
}

void r12f_first_weight_of_a_new_link_carries_its_generation() {
  resetHarness(false, true);
  // The old link dropped; the new one reached Ready between worker ticks.
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  const uint32_t oldGeneration = getScaleLinkSnapshot().connectionGeneration;
  scale.weight = 12.34f;
  scale.newWeightAvailableValue = true;
  serviceScaleWorkerLink();
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  CHECK(link.state == ScaleLinkState::CONNECTED &&
        link.connectionGeneration == oldGeneration + 1);
  CHECK(scaleWeightEventCount == 1);
  CHECK(scaleWeightEvents[scaleWeightEventHead].connectionGeneration ==
        link.connectionGeneration);
}

void d02f_only_mode_disconnects_a_nonpreferred_live_scale() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  scale.connected = true;
  CHECK(!preferredScaleMacEqual(scale.address(), scalePreferredMac));
  noteScaleHistory(scale.address(), scale.localName(), false);
  CHECK(setScaleFriendlyName(scale.address(), "Fallback scale"));
  updateWorkerLinkState();
  setScaleLinkState(ScaleLinkState::CONNECTED);
  markScaleWorkerProgress();
  publishControlStatus();
  CHECK(strcmp(publishedControlStatus.connectedScaleMac,
               scale.connectedAddress) == 0);
  char friendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  CHECK(findScaleHistoryFriendlyName(scaleHistory,
                                     publishedControlStatus.connectedScaleMac,
                                     friendly, sizeof(friendly)));
  CHECK(strcmp(friendly, "Fallback scale") == 0);

  RuntimeConfig candidate = runtimeConfig;
  candidate.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
  serviceScaleWorkerLink();
  CHECK(!scale.connected);
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::DISCONNECTED);
  publishControlStatus();
  CHECK(publishedControlStatus.connectedScaleMac[0] == '\0');
}

void d02g_clear_preferred_disconnects_on_the_ble_worker() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  setHostPreferredScaleMac(scale.connectedAddress);
  scale.connected = true;

  selectPreferredScale("", "");
  CHECK(scale.connected);
  serviceScaleWorkerLink();
  CHECK(!scale.connected);
  CHECK(scalePreferredMac[0] == '\0');
}

void d03_scan_start_failed_retries_immediately() {
  resetHarness(false, false);
  reachReadyFromBoot();
  scale.startScanSucceeds = false;
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(!scale.scanning);
  CHECK(scale.startScanCalls == 1);
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.startScanCalls == 2);
  CHECK(!scale.scanning);
}

void d04_full_cache_keeps_directed_scan() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  for (uint8_t tick = 0; tick < 3; ++tick) {
    hostMillis += SCALE_DISCOVERY_TICK_MS;
    serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                                connectAttemptSeriesActive,
                                scanSessionAtMs, scanLastAdvertAtMs);
  }
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  CHECK(scale.lastAddressScan);
  CHECK(strcmp(scale.lastStartScanMac, "AA:BB:CC:DD:EE:FF") == 0);
}

void d05_hci_watchdog_force_restarts_same_filter() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.startScanCalls == 1);
  CHECK(!scale.lastForceRestart);
  CHECK(scale.lastScanInterval == BLE_SCAN_BALANCED_INTERVAL);
  CHECK(scale.lastScanWindow == BLE_SCAN_BALANCED_WINDOW);
  const size_t callsBeforeRestart = scale.startScanCalls;
  size_t ticks = 0;
  while (scale.startScanCalls == callsBeforeRestart) {
    hostMillis += SCALE_DISCOVERY_TICK_MS;
    serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                                connectAttemptSeriesActive,
                                scanSessionAtMs, scanLastAdvertAtMs);
    ++ticks;
    CHECK(ticks < 40);
  }
  CHECK(ticks == SCALE_SCAN_HCI_RESTART_MS / SCALE_DISCOVERY_TICK_MS);
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  CHECK(scale.lastAddressScan);
  CHECK(scale.lastForceRestart);
  CHECK(scale.startScanCalls == 2);
}

void d05b_scan_intensity_change_restarts_gap() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.startScanCalls == 1);
  CHECK(scale.lastScanInterval == BLE_SCAN_BALANCED_INTERVAL);
  applyLiveBleScanIntensity(BleScanIntensity::RELAXED);
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.startScanCalls == 2);
  CHECK(scale.lastForceRestart);
  CHECK(scale.lastScanInterval == BLE_SCAN_RELAXED_INTERVAL);
  CHECK(scale.lastScanWindow == BLE_SCAN_RELAXED_WINDOW);
  const size_t calls = scale.startScanCalls;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.startScanCalls == calls);
}

void d05c_scan_intensity_hci_presets() {
  CHECK(BLE_SCAN_RELAXED_INTERVAL == 0x00B8);
  CHECK(BLE_SCAN_RELAXED_WINDOW == 0x002E);
  CHECK(BLE_SCAN_BALANCED_INTERVAL == 0x0064);
  CHECK(BLE_SCAN_BALANCED_WINDOW == 0x0032);
  CHECK(BLE_SCAN_AGGRESSIVE_INTERVAL == 0x0020);
  CHECK(BLE_SCAN_AGGRESSIVE_WINDOW == 0x0020);
  uint16_t interval = 0;
  uint16_t window = 0;
  bleScanHciParams(BleScanIntensity::RELAXED, interval, window);
  CHECK(interval == BLE_SCAN_RELAXED_INTERVAL);
  CHECK(window == BLE_SCAN_RELAXED_WINDOW);
  bleScanHciParams(BleScanIntensity::BALANCED, interval, window);
  CHECK(interval == BLE_SCAN_BALANCED_INTERVAL);
  CHECK(window == BLE_SCAN_BALANCED_WINDOW);
  bleScanHciParams(BleScanIntensity::AGGRESSIVE, interval, window);
  CHECK(interval == BLE_SCAN_AGGRESSIVE_INTERVAL);
  CHECK(window == BLE_SCAN_AGGRESSIVE_WINDOW);
  BleScanIntensity parsed = BleScanIntensity::RELAXED;
  CHECK(parseBleScanIntensityId("aggressive", parsed));
  CHECK(parsed == BleScanIntensity::AGGRESSIVE);
  CHECK(parseBleScanIntensityId("relaxed", parsed));
  CHECK(parsed == BleScanIntensity::RELAXED);
  CHECK(parseBleScanIntensityId("balanced", parsed));
  CHECK(parsed == BleScanIntensity::BALANCED);
  CHECK(!parseBleScanIntensityId("burst", parsed));
  CHECK(!parseBleScanIntensityId("light", parsed));
  CHECK(strcmp(bleScanIntensityName(BleScanIntensity::BALANCED), "balanced") == 0);
}

void d16_scale_connect_debug_reports_phases() {
  resetHarness(false, false);
  reachReadyFromBoot();
  debugLog.clear();
  ringRetainLogLevel = LogLevel::DEBUG;
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(debugEventExists(DebugCode::SCALE_SCAN_STARTED, SCALE_SCAN_TARGET_ANY,
                         static_cast<int32_t>(BleScanIntensity::BALANCED)));
  CHECK(!debugEventExists(DebugCode::SCALE_CONNECTING));

  scale.connecting = true;
  scale.scanning = false;
  scale.connectStep = 2;
  scale.connectAttempts = 1;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(debugEventExists(DebugCode::SCALE_GATT_CONNECTING, SCALE_SCAN_TARGET_ANY,
                         INT32_MIN));
  CHECK(debugEventExists(DebugCode::SCALE_CONNECT_ATTEMPT_FAILED, 1, 2));

  scale.connectAttempts = 3;
  scale.pollScanFailsConnect = true;
  scale.disconnectReason = ScaleDisconnectReason::CONNECT_FAILED;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(debugEventExists(DebugCode::SCALE_CONNECT_ATTEMPT_FAILED, 3, 2));
  CHECK(debugEventExists(DebugCode::SCALE_CONNECT_FAILED, 4, 2));

  char message[128] = {};
  DebugEvent failed = {};
  failed.code = DebugCode::SCALE_CONNECT_FAILED;
  failed.argument1 = 4;
  failed.argument2 = 2;
  CHECK(formatLifecycleDebugMessage(failed, message, sizeof(message)));
  CHECK(std::string(message).find("connect failed") != std::string::npos);
  CHECK(std::string(message).find("step=connect") != std::string::npos);
  CHECK(debugCodeDefaultLevel(DebugCode::SCALE_CONNECT_FAILED) ==
        LogLevel::WARNING);
  CHECK(debugCodeDefaultLevel(DebugCode::SCALE_SCAN_STARTED) == LogLevel::INFO);
  CHECK(debugCodeDefaultLevel(DebugCode::SCALE_GATT_CONNECTING) ==
        LogLevel::INFO);
}

void d06_forget_pauses_discovery_for_30s() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  const size_t callsBeforeForget = scale.startScanCalls;
  scale.connected = true;
  clearPreferredScaleCache();
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(!scale.connected);
  CHECK(!scale.scanning);
  CHECK(scale.startScanCalls == callsBeforeForget);
  hostMillis += SCALE_PAIRING_DISCOVERY_PAUSE_MS - 1;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(!scale.scanning);
  CHECK(scale.startScanCalls == callsBeforeForget);
  hostMillis += 2;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs, scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
  CHECK(scale.lastStartScanMac[0] == '\0');
}

void d07_prefer_falls_back_after_grace() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::PREFER);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(scale.directedScan);
  CHECK(!scale.lastAddressScan);
  const size_t callsBefore = scale.startScanCalls;
  hostMillis += SCALE_PREFER_FALLBACK_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
  CHECK(!scale.lastAddressScan);
  CHECK(scale.startScanCalls > callsBefore);
  CHECK(scale.lastStartScanMac[0] == '\0');
}

void d08_select_none_clears_without_pause() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
  publishTestScaleWorkerPolicy();
  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  selectPreferredScale("", "");
  CHECK(scalePreferredMac[0] == '\0');
  CHECK(runtimeConfig.scaleMacCacheMode ==
        static_cast<uint8_t>(ScaleMacCacheMode::ONLY));
  CHECK(!scaleDiscoveryPaused());
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
}

void d09_first_mode_connects_seen_advertisement() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  publishTestScaleWorkerPolicy();
  scalePreferredMac[0] = '\0';
  for (ScaleHistoryEntry &entry : scaleHistory) {
    entry = ScaleHistoryEntry{};
  }
  scaleHistorySeq = 0;
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  CHECK(!scale.directedScan);
  CHECK(scale.lastStartScanMac[0] == '\0');

  copyCString(scale.seenMac, sizeof(scale.seenMac), "AA:BB:CC:DD:EE:FF");
  strncpy(scale.seenName, "LUNAR", sizeof(scale.seenName) - 1);
  scale.seenPending = true;
  scale.pollScanConnects = true;
  scale.pollScanStepsToConnect = 1;
  hostMillis += SCALE_DISCOVERY_TICK_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.connected);
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::CONNECTED);
  CHECK(preferredScaleMacEqual(scaleHistory[0].mac, "AA:BB:CC:DD:EE:FF"));
  CHECK(scalePreferredMac[0] == '\0');
}

void d12_advertisement_history_does_not_dirty_persist() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  scalePreferredMac[0] = '\0';
  for (ScaleHistoryEntry &entry : scaleHistory) {
    entry = ScaleHistoryEntry{};
  }
  scaleHistorySeq = 0;
  scalePreferredMacDirty = false;
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  copyCString(scale.seenMac, sizeof(scale.seenMac), "AA:BB:CC:DD:EE:01");
  strncpy(scale.seenName, "LUNAR", sizeof(scale.seenName) - 1);
  scale.seenPending = true;
  scale.pollScanConnects = false;
  hostMillis += SCALE_DISCOVERY_TICK_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(preferredScaleMacEqual(scaleHistory[0].mac, "AA:BB:CC:DD:EE:01"));
  CHECK(!scalePreferredMacDirty);

  scale.pollScanConnects = true;
  scale.pollScanStepsToConnect = 1;
  copyCString(scale.seenMac, sizeof(scale.seenMac), "AA:BB:CC:DD:EE:01");
  strncpy(scale.seenName, "LUNAR", sizeof(scale.seenName) - 1);
  scale.seenPending = true;
  hostMillis += SCALE_DISCOVERY_TICK_MS;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.connected);
  CHECK(scalePreferredMacDirty);
}

void d10c_softap_yields_discovery() {
  resetHarness(false, false);
  reachReadyFromBoot();
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  hostMillis += SCALE_HUNT_RF_CLEAR_MS + 1;
  syncScaleSoftApRadio(true);
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(!scale.scanning);
  CHECK(!startScaleDiscoveryScan(nullptr, false));
}


void d11_select_preferred_is_noop_when_unchanged() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scaleMacCacheMode =
      static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
  uint32_t lastScanCycleMs = 0;
  uint32_t lastConnectLogMs = 0;
  bool connectAttemptSeriesActive = false;
  uint32_t scanSessionAtMs = 0;
  uint32_t scanLastAdvertAtMs = 0;
  serviceScaleWorkerDiscovery(lastScanCycleMs, lastConnectLogMs,
                              connectAttemptSeriesActive, scanSessionAtMs,
                              scanLastAdvertAtMs);
  CHECK(scale.scanning);
  const size_t calls = scale.startScanCalls;
  selectPreferredScale("", "");
  CHECK(scale.scanning);
  CHECK(scale.startScanCalls == calls);

  setHostPreferredScaleMac("AA:BB:CC:DD:EE:FF");
  strncpy(scalePreferredName, "LUNAR", sizeof(scalePreferredName) - 1);
  selectPreferredScale("AA:BB:CC:DD:EE:FF", "LUNAR");
  CHECK(scale.scanning);
  CHECK(scale.startScanCalls == calls);
}

void d13_scale_friendly_name_set_clear_and_preserve() {
  resetHarness(false, false);
  scalePreferredMac[0] = '\0';
  for (ScaleHistoryEntry &entry : scaleHistory) {
    entry = ScaleHistoryEntry{};
  }
  scaleHistorySeq = 0;
  noteScaleHistory("AA:BB:CC:DD:EE:01", "BOOKOO_SC 715097", false);
  CHECK(!setScaleFriendlyName("AA:BB:CC:DD:EE:01", "bad name!"));
  CHECK(!setScaleFriendlyName("AA:BB:CC:DD:EE:01", " leading"));
  CHECK(!setScaleFriendlyName("AA:BB:CC:DD:EE:99", "Mini"));
  CHECK(setScaleFriendlyName("AA:BB:CC:DD:EE:01", "Bookoo Themis mini"));
  // Re-advertisement must refresh the advertised name without clobbering the
  // stored override.
  noteScaleHistory("aa:bb:cc:dd:ee:01", "BOOKOO_SC 715098", false);
  ScaleHistoryEntry history[SCALE_HISTORY_CAPACITY] = {};
  copyScaleHistory(history);
  CHECK(strcmp(history[0].name, "BOOKOO_SC 715098") == 0);
  CHECK(strcmp(history[0].friendlyName, "Bookoo Themis mini") == 0);
  char friendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  CHECK(findScaleHistoryFriendlyName(history, "AA:BB:CC:DD:EE:01", friendly,
                                     sizeof(friendly)));
  CHECK(strcmp(friendly, "Bookoo Themis mini") == 0);
  CHECK(setScaleFriendlyName("AA:BB:CC:DD:EE:01", ""));
  copyScaleHistory(history);
  CHECK(history[0].friendlyName[0] == '\0');
  CHECK(findScaleHistoryFriendlyName(history, "AA:BB:CC:DD:EE:01", friendly,
                                     sizeof(friendly)));
  CHECK(strcmp(friendly, "BOOKOO Themis Mini") == 0);
}

void d13a_auto_friendly_name_for_themis_models() {
  CHECK(strcmp(scaleDefaultFriendlyName("BOOKOO_SC 715097"),
               "BOOKOO Themis Mini") == 0);
  CHECK(strcmp(scaleDefaultFriendlyName("BOOKOO_SC U 90210"),
               "BOOKOO Themis Ultra") == 0);
  CHECK(strcmp(scaleDefaultFriendlyName("BOOKOO_SC_U 369041"),
               "BOOKOO Themis Ultra") == 0);
  CHECK(strcmp(scaleDefaultFriendlyName("LUNAR"), "LUNAR") == 0);
  CHECK(scaleModelForAdvertisement("BOOKOO_SCTE 1") == ScaleModel::Unknown);
  CHECK(scaleModelForAdvertisement("BOOKOO_SC U ") == ScaleModel::Unknown);
  CHECK(scaleModelForAdvertisement(nullptr) == ScaleModel::Unknown);

  resetHarness(false, false);
  scalePreferredMac[0] = '\0';
  for (ScaleHistoryEntry &entry : scaleHistory) {
    entry = ScaleHistoryEntry{};
  }
  scaleHistorySeq = 0;
  noteScaleHistory("AA:BB:CC:DD:EE:01", "BOOKOO_SC 715097", false);
  noteScaleHistory("AA:BB:CC:DD:EE:02", "BOOKOO_SC_U 369041", false);
  ScaleHistoryEntry history[SCALE_HISTORY_CAPACITY] = {};
  copyScaleHistory(history);
  CHECK(history[0].friendlyName[0] == '\0');
  CHECK(history[1].friendlyName[0] == '\0');
  char friendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  CHECK(findScaleHistoryFriendlyName(history, "AA:BB:CC:DD:EE:01", friendly,
                                     sizeof(friendly)));
  CHECK(strcmp(friendly, "BOOKOO Themis Mini") == 0);
  CHECK(findScaleHistoryFriendlyName(history, "AA:BB:CC:DD:EE:02", friendly,
                                     sizeof(friendly)));
  CHECK(strcmp(friendly, "BOOKOO Themis Ultra") == 0);
  noteScaleHistory("AA:BB:CC:DD:EE:03", "LUNAR", false);
  copyScaleHistory(history);
  CHECK(findScaleHistoryFriendlyName(history, "AA:BB:CC:DD:EE:03", friendly,
                                     sizeof(friendly)));
  CHECK(strcmp(friendly, "LUNAR") == 0);
  // Repeat advertisements do not overwrite a user-assigned friendly name.
  CHECK(setScaleFriendlyName("AA:BB:CC:DD:EE:01", "Left cup scale"));
  noteScaleHistory("AA:BB:CC:DD:EE:01", "BOOKOO_SC 715098", false);
  copyScaleHistory(history);
  CHECK(strcmp(history[0].name, "BOOKOO_SC 715098") == 0);
  CHECK(strcmp(history[0].friendlyName, "Left cup scale") == 0);
}

void d13b_friendly_names_are_per_scale() {
  // Naming scale A must not change scale B: overrides are stored per MAC.
  resetHarness(false, false);
  scalePreferredMac[0] = '\0';
  for (ScaleHistoryEntry &entry : scaleHistory) {
    entry = ScaleHistoryEntry{};
  }
  scaleHistorySeq = 0;
  noteScaleHistory("AA:BB:CC:DD:EE:01", "BOOKOO_SC 715097", false);
  noteScaleHistory("AA:BB:CC:DD:EE:02", "BOOKOO_SC U 90210", false);
  CHECK(setScaleFriendlyName("AA:BB:CC:DD:EE:01", "Scale A"));
  CHECK(setScaleFriendlyName("AA:BB:CC:DD:EE:02", "Scale B"));
  ScaleHistoryEntry history[SCALE_HISTORY_CAPACITY] = {};
  copyScaleHistory(history);
  CHECK(strcmp(history[0].friendlyName, "Scale A") == 0);
  CHECK(strcmp(history[1].friendlyName, "Scale B") == 0);
  CHECK(strcmp(history[0].friendlyName, history[1].friendlyName) != 0);
  // Re-advertising or reconnecting B keeps A's name untouched.
  noteScaleHistory("AA:BB:CC:DD:EE:02", "BOOKOO_SC U 90210", false);
  copyScaleHistory(history);
  CHECK(strcmp(history[0].friendlyName, "Scale A") == 0);
  CHECK(strcmp(history[1].friendlyName, "Scale B") == 0);
}

void w62_local_buzzer_drive_matches_compile_flag() {
  resetHarness(false, false);
  CHECK(localBuzzer.ready);
  CHECK(hostPinMode[BUZZER_GPIO] == OUTPUT);
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(hostLedcAttachCalls == 1);
}

void w99_rtttl_parser_decodes_notes_and_rests() {
  RtttlNote notes[RTTTL_MAX_NOTES];
  uint8_t count = 0;
  CHECK(parseRtttl(RTTTL_TARE, notes, count));
  CHECK(count == 1);
  CHECK(notes[0].freqHz >= 780 && notes[0].freqHz <= 790);
  CHECK(notes[0].durationMs >= 60 && notes[0].durationMs <= 80);
  CHECK(parseRtttl(RTTTL_PADDLE_OFF, notes, count));
  CHECK(count == 3);
  CHECK(notes[0].freqHz >= 780 && notes[0].freqHz <= 790);
  CHECK(notes[1].freqHz == 0);
  CHECK(notes[2].freqHz >= 780 && notes[2].freqHz <= 790);
  CHECK(parseRtttl(RTTTL_START_TIMER, notes, count));
  CHECK(count == 1);
  CHECK(notes[0].freqHz >= 1040 && notes[0].freqHz <= 1055);
  for (uint8_t i = 1; i <= static_cast<uint8_t>(BuzzerCue::RECOVERY_ERROR);
       ++i) {
    const BuzzerCue cue = static_cast<BuzzerCue>(i);
    const char *rtttl = rtttlForCue(cue);
    if (rtttl == nullptr) {
      CHECK(buzzerCueIsLooping(cue) || cue == BuzzerCue::BULLSEYE);
      continue;
    }
    CHECK(parseRtttl(rtttl, notes, count));
    CHECK(count > 0);
  }
  CHECK(parseRtttl(RTTTL_EXTENDED_PULSE_SLOW, notes, count));
  CHECK(count == 3);
  CHECK(notes[0].freqHz >= 490 && notes[0].freqHz <= 498);
  CHECK(notes[0].durationMs == 20);
  CHECK(notes[1].freqHz == 0 && notes[1].durationMs == 320);
  CHECK(notes[2].freqHz == 0 && notes[2].durationMs == 160);
  CHECK(parseRtttl(RTTTL_EXTENDED_PULSE_MEDIUM, notes, count));
  CHECK(count == 2);
  CHECK(notes[0].durationMs == 20 && notes[1].durationMs == 320);
  CHECK(parseRtttl(RTTTL_EXTENDED_PULSE_FAST, notes, count));
  CHECK(count == 3);
  CHECK(notes[0].durationMs == 20 && notes[1].durationMs == 213);
  CHECK(notes[2].durationMs == 20);
  CHECK(parseRtttl(RTTTL_EXTENDED_PULSE_RAPID, notes, count));
  CHECK(count == 3);
  CHECK(notes[0].durationMs == 20 && notes[1].durationMs == 160);
  CHECK(notes[2].durationMs == 20);
  CHECK(rtttlForExtendedPulseRate(static_cast<uint8_t>(
            ExtendedPulseRate::OFF)) == nullptr);
  CHECK(rtttlForExtendedPulseRate(static_cast<uint8_t>(
            ExtendedPulseRate::FAST)) == RTTTL_EXTENDED_PULSE_FAST);
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER == 1
  resetHarness(false, false);
  CHECK(localBuzzer.cueNoteCount[static_cast<uint8_t>(BuzzerCue::TARE)] == 1);
  CHECK(localBuzzer.cueNoteCount[static_cast<uint8_t>(
            BuzzerCue::PADDLE_REMINDER)] == 3);
  CHECK(localBuzzer.pulseNoteCount[static_cast<uint8_t>(
            ExtendedPulseRate::FAST)] == 3);
#endif
}

void w99f_bullseye_rtttl_is_bounded_and_plays_without_allocation() {
  resetHarness(false, false);
  constexpr const char *tune =
      "bullseye:d=16,o=5,b=180:c,e,g,c6,8p,c6";
  BullseyeMelodyConfig config;
  config.enabled = true;
  copyCString(config.rtttl, sizeof(config.rtttl), tune);
  CHECK(validBullseyeMelodyConfig(config));
  CHECK(localBuzzer.configureBullseyeRtttl(config.rtttl));
  CHECK(localBuzzer.requestBullseye());
  CHECK(localBuzzer.activeCue == BuzzerCue::BULLSEYE);
  CHECK(localBuzzer.rtttlPlayback);
  CHECK(localBuzzer.rtttlCount == 6);

  config.rtttl[0] = '\0';
  CHECK(!validBullseyeMelodyConfig(config));
  config.enabled = false;
  CHECK(validBullseyeMelodyConfig(config));
  copyCString(config.rtttl, sizeof(config.rtttl), "not-rtttl");
  CHECK(!validBullseyeMelodyConfig(config));

  uint8_t count = 0;
  CHECK(!parseRtttlBounded("x:d=4,o=5,b=120:,c", nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  CHECK(!parseRtttlBounded("x:d=4,o=5,b=120:c,,d", nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  CHECK(!parseRtttlBounded("x:d=4,o=5,b=120:cd", nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  CHECK(!parseRtttlBounded("x:d=4,o=5,b=120:65c", nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  CHECK(!parseRtttlBounded("x:d=4,o=5,b=120:p#", nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  CHECK(!parseRtttlBounded("x:d=4,o=5,b=120:c9", nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  char unterminated[RTTTL_INPUT_CAPACITY];
  memset(unterminated, 'c', sizeof(unterminated));
  CHECK(!parseRtttlBounded(unterminated, nullptr,
                           BULLSEYE_RTTTL_MAX_NOTES, count));
  for (const char *badHeader :
       {"x:b=1000:c", "x:d=0:c", "x:o=3:c", "x:b=:c", "x:q=4:c", "x:d=4 5:c",
        "x:b=4294967396:c", "x:d4:c"}) {
    CHECK(!parseRtttlBounded(badHeader, nullptr, BULLSEYE_RTTTL_MAX_NOTES,
                             count));
  }
  RtttlNote notes[RTTTL_MAX_NOTES];
  CHECK(parseRtttl("x::c", notes, count) && count == 1);
  CHECK(notes[0].durationMs == 952);
  CHECK(parseRtttl("x: D=8 , o=5 ,B=120 :c", notes, count) && count == 1);
  CHECK(notes[0].durationMs == 250);

  // A repeated pitch keeps a short break; a pitch change stays legato.
  localBuzzer.stopAll();
  CHECK(localBuzzer.configureBullseyeRtttl("x:d=4,o=5,b=60:c,c,e"));
  CHECK(localBuzzer.requestBullseye(/*allowQueue=*/false));
  CHECK(localBuzzer.onMs == 900 && localBuzzer.gapMs == 100);
  hostMillis += 900;
  localBuzzer.service();
  CHECK(hostPinLevel[BUZZER_GPIO] == LOW);
  CHECK(localBuzzer.activeCue == BuzzerCue::BULLSEYE);
  hostMillis += 100;
  localBuzzer.service();
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
  CHECK(localBuzzer.beepIndex == 1);
  CHECK(localBuzzer.onMs == 1000 && localBuzzer.gapMs == 0);

  // A weak cue waits for the melody; a high-priority alert interrupts it and
  // keeps the waiting cue.
  const BuzzerToneCommand tare =
      deriveBuzzerTone(AlertEvent::TARE, false, 0, 0);
  CHECK(localBuzzer.requestTone(tare));
  CHECK(localBuzzer.activeCue == BuzzerCue::BULLSEYE);
  CHECK(localBuzzer.pendingCue == BuzzerCue::TARE);
  BuzzerToneCommand guard;
  guard.cue = BuzzerCue::GUARD_STOP;
  guard.valid = true;
  CHECK(localBuzzer.requestTone(guard));
  CHECK(localBuzzer.activeCue == BuzzerCue::GUARD_STOP);
  CHECK(localBuzzer.pendingCue == BuzzerCue::TARE);
}

void w99i_bullseye_test_never_queues_mutable_custom_notes() {
  resetHarness(false, false);
  constexpr const char *testTune = "test:d=16,o=5,b=180:c,e,g";
  CHECK(localBuzzer.configureBullseyeRtttl(testTune));
  const BuzzerToneCommand tare =
      deriveBuzzerTone(AlertEvent::TARE, false, 0, 0);
  CHECK(localBuzzer.requestTone(tare));
  CHECK(!localBuzzer.canStartNow());
  CHECK(!localBuzzer.requestBullseye(/*allowQueue=*/false));
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE);
  CHECK(localBuzzer.pendingCue == BuzzerCue::NONE);

  localBuzzer.stopAll();
  CHECK(localBuzzer.canStartNow());
  CHECK(localBuzzer.requestBullseye(/*allowQueue=*/false));
  CHECK(localBuzzer.activeCue == BuzzerCue::BULLSEYE);
  CHECK(localBuzzer.rtttlCount == 3);
}

void w99g_bullseye_requires_one_second_of_exact_fresh_samples() {
  BullseyeTracker tracker;
  tracker.arm(360, 1000, 3000, 10);
  CHECK(!tracker.accept(36.0f, 3999, 11, 1000));
  CHECK(!tracker.accept(36.0f, 4000, 12, 1000));
  CHECK(!tracker.accept(35.99f, 4500, 13, 1000));
  CHECK(!tracker.accept(36.0f, 5000, 14, 1000));
  CHECK(tracker.accept(36.0f, 6000, 15, 1000));
  CHECK(!tracker.pending);

  tracker.arm(360, 1000, 3000, 20);
  CHECK(!tracker.accept(36.0f, 60000, 21, 1000));
  CHECK(tracker.accept(36.0f, 61000, 22, 1000));

  tracker.arm(360, 1000, 3000, 30);
  CHECK(!tracker.accept(36.0f, 4000, 31, 1000));
  CHECK(!tracker.accept(36.0f, 5001, 32, 1000));
  CHECK(tracker.targetSampleCount == 1);
  CHECK(tracker.pending);
}

void w99h_bullseye_service_runs_only_in_buzzer_only_mode() {
  resetHarness(false, true);
  bullseyeMelodyConfig.enabled = true;
  copyCString(bullseyeMelodyConfig.rtttl,
              sizeof(bullseyeMelodyConfig.rtttl),
              "bullseye:d=16,o=5,b=180:c,e,g,c6");
  CHECK(localBuzzer.configureBullseyeRtttl(bullseyeMelodyConfig.rtttl));
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  hostMillis = 1000;
  markScaleWorkerProgress();
  cupPresence.state = CupPresenceState::PRESENT;
  cupPresence.placementId = 7;
  bullseyeTracker.arm(360, hostMillis, 3000, 10, cupPresencePlacementId());
  currentWeight = 36.0f;
  currentWeightSequence = 11;
  currentWeightReceivedAtMs = 4000;
  hostMillis = 4000;
  markScaleWorkerProgress();
  serviceBullseyeMelody();
  CHECK(bullseyeTracker.pending);
  currentWeightSequence = 12;
  currentWeightReceivedAtMs = 5000;
  hostMillis = 5000;
  markScaleWorkerProgress();
  serviceBullseyeMelody();
  CHECK(!bullseyeTracker.pending);
  CHECK(localBuzzer.activeCue == BuzzerCue::BULLSEYE);

  localBuzzer.stopAll();
  bullseyeTracker.arm(360, hostMillis, 0, currentWeightSequence,
                      cupPresencePlacementId());
  ++cupPresence.placementId;
  serviceBullseyeMelody();
  CHECK(!bullseyeTracker.pending);
  CHECK(localBuzzer.activeCue == BuzzerCue::NONE);

  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_PRIORITY);
  bullseyeTracker.arm(360, hostMillis, 3000, currentWeightSequence,
                      cupPresencePlacementId());
  ++currentWeightSequence;
  currentWeightReceivedAtMs = hostMillis + 100;
  hostMillis += 100;
  serviceBullseyeMelody();
  CHECK(!bullseyeTracker.pending);
  CHECK(localBuzzer.activeCue == BuzzerCue::NONE);
}

void w99b_passive_cue_drives_ledc_note_frequency() {
  resetHarness(false, false);
  const BuzzerToneCommand cmd =
      deriveBuzzerTone(AlertEvent::TARE, false, 0, 0);
  CHECK(cmd.valid);
  CHECK(cmd.cue == BuzzerCue::TARE);
  CHECK(localBuzzer.requestTone(cmd));
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE);
  CHECK(localBuzzer.rtttlPlayback);
  CHECK(hostLedcLastFreq >= 780 && hostLedcLastFreq <= 790);
  CHECK(hostPinLevel[BUZZER_GPIO] == HIGH);
}

void w99c_select_alert_sink_preserves_channel_rules() {
  AlertChannelContext ctx;
  ctx.soundAlertsEnabled = true;
  ctx.buzzerSupportEnabled = true;
  ctx.buzzerReady = true;
  ctx.channel = AlertOutputChannel::SCALE_ONLY;
  CHECK(selectAlertSink(AlertKind::Independent, AlertEvent::SCALE_LOST, ctx) ==
        AlertSink::None);
  ctx.channel = AlertOutputChannel::BUZZER_ONLY;
  CHECK(selectAlertSink(AlertKind::Independent, AlertEvent::SCALE_LOST, ctx) ==
        AlertSink::Buzzer);
  ctx.scaleAvailable = true;
  ctx.scaleSupportsIndependentBeep = true;
  ctx.channel = AlertOutputChannel::SCALE_PRIORITY;
  CHECK(selectAlertSink(AlertKind::Independent, AlertEvent::FIRST_DROP, ctx) ==
        AlertSink::Scale);
  ctx.channel = AlertOutputChannel::BUZZER_ONLY;
  CHECK(selectAlertSink(AlertKind::CommandImmediate, AlertEvent::START_TIMER,
                        ctx) == AlertSink::Buzzer);
  ctx.channel = AlertOutputChannel::SCALE_ONLY;
  CHECK(selectAlertSink(AlertKind::CommandImmediate, AlertEvent::START_TIMER,
                        ctx) == AlertSink::None);
  ctx.soundAlertsEnabled = false;
  ctx.channel = AlertOutputChannel::BUZZER_ONLY;
  CHECK(selectAlertSink(AlertKind::Independent, AlertEvent::FIRST_DROP, ctx) ==
        AlertSink::None);
  CHECK(selectAlertSink(AlertKind::Recovery, AlertEvent::TARE, ctx) ==
        AlertSink::Buzzer);
  ctx.soundAlertsEnabled = true;
  ctx.scaleAvailable = false;
  ctx.channel = AlertOutputChannel::SCALE_PRIORITY;
  CHECK(selectAlertSink(AlertKind::Independent, AlertEvent::FIRST_DROP, ctx) ==
        AlertSink::Buzzer);
  ctx.commandFeedbackExpected = true;
  ctx.commandAttempted = true;
  ctx.writeSucceeded = false;
  CHECK(selectAlertSink(AlertKind::CommandFallback, AlertEvent::START_TIMER,
                        ctx) == AlertSink::Buzzer);
  const BuzzerToneCommand tareTone =
      deriveBuzzerTone(AlertEvent::TARE, false, 0, 0);
  CHECK(tareTone.valid && tareTone.cue == BuzzerCue::TARE);
  const BuzzerToneCommand endTone =
      deriveBuzzerTone(AlertEvent::COMPLETION_EXTRA, false, 0, 0);
  CHECK(endTone.valid && endTone.cue == BuzzerCue::SHOT_END);
  const BuzzerToneCommand lostTone =
      deriveBuzzerTone(AlertEvent::SCALE_LOST, false, 0, 0);
  CHECK(lostTone.valid && lostTone.cue == BuzzerCue::SCALE_LOST);
  const BuzzerToneCommand pulseOff =
      deriveBuzzerTone(AlertEvent::EXTENDED_PULSE, false, 0, 0);
  CHECK(!pulseOff.valid);
  const BuzzerToneCommand pulseFast = deriveBuzzerTone(
      AlertEvent::EXTENDED_PULSE, false,
      static_cast<uint8_t>(ExtendedPulseRate::FAST), 0);
  CHECK(pulseFast.valid && pulseFast.cue == BuzzerCue::ABNORMAL_FAST &&
        pulseFast.pulseRate == static_cast<uint8_t>(ExtendedPulseRate::FAST) &&
        pulseFast.looping);
}

void w99d_pending_rtttl_starts_from_preparsed_catalog() {
  resetHarness(false, false);
  const BuzzerToneCommand tare =
      deriveBuzzerTone(AlertEvent::TARE, false, 0, 0);
  const BuzzerToneCommand end =
      deriveBuzzerTone(AlertEvent::COMPLETION_EXTRA, false, 0, 0);
  CHECK(localBuzzer.requestTone(tare));
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE);
  CHECK(localBuzzer.requestTone(end));
  CHECK(localBuzzer.pendingCue == BuzzerCue::SHOT_END);
  localBuzzer.stopIfCue(BuzzerCue::TARE);
  CHECK(localBuzzer.activeCue == BuzzerCue::SHOT_END);
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER == 1
  CHECK(localBuzzer.rtttlPlayback);
  CHECK(localBuzzer.beepCount ==
        localBuzzer.cueNoteCount[static_cast<uint8_t>(BuzzerCue::SHOT_END)]);
#endif
}

void w99e_recovery_cue_bypasses_mute_via_pipeline() {
  resetHarness(false, false);
  runtimeConfig.soundAlertsMuted = true;
  CHECK(!soundAlertsEnabled());
  playRecoveryCue(BuzzerCue::RECOVERY_START);
  CHECK(localBuzzer.activeCue == BuzzerCue::RECOVERY_START);
  CHECK(localBuzzer.rtttlPlayback);
}

void w39_history_mutation_blocked_while_brew_rf_active() {
  ControlStatusSnapshot status;
  CHECK(controlAllowsHistoryMutation(status));
  status.activeCycle = true;
  CHECK(!controlAllowsHistoryMutation(status));
  status.activeCycle = false;
  status.machineRunning = true;
  CHECK(!controlAllowsHistoryMutation(status));
}

void w37_factory_reset_is_rejected_while_control_is_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  DebugEvent before[DEBUG_EVENT_CAPACITY] = {};
  const size_t beforeCount =
      copyDebugEvents(0, before, DEBUG_EVENT_CAPACITY);
  const uint32_t afterSequence =
      beforeCount == 0 ? 0 : before[beforeCount - 1].sequence;

  WebCommand reset;
  reset.type = WebCommandType::FACTORY_RESET;
  processWebCommand(reset);

  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  DebugEvent events[4] = {};
  const size_t count = copyDebugEvents(afterSequence, events, 4);
  bool rejected = false;
  for (size_t index = 0; index < count; ++index) {
    rejected |= events[index].code == DebugCode::WEB_COMMAND_REJECTED &&
                events[index].argument1 ==
                    static_cast<int32_t>(WebCommandType::FACTORY_RESET);
  }
  CHECK(rejected);
}

void r21_automatic_control_requires_fresh_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();

  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(session.active);
  CHECK(!session.startedWithScale);
  CHECK(!session.automaticEnabled);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  CHECK(getRelaySafetySnapshot().closed);

  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 0.0f;
  currentWeightSequence = 1;
  currentWeightReceivedAtMs = hostMillis;
  markScaleWorkerProgress();
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(session.startedWithScale);
  CHECK(session.automaticEnabled);
  CHECK(stopperState == StopperState::BREW);
  CHECK(!session.receivedFreshWeightInCycle);
  CHECK(getRelaySafetySnapshot().closed);
}

void r22_confirmed_implausible_weight_does_not_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(session.automaticEnabled);
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  endBbwProtectionForTests();
  publishWeight(900.0f, hostMillis, 1, 10);
  CHECK(session.weightControlState == WeightControlState::VALIDATING);
  CHECK(getRelaySafetySnapshot().closed);
  publishWeight(900.0f, hostMillis, 1, 11);
  publishWeight(900.0f, hostMillis, 1, 12);
  CHECK(!session.directStopPending);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
}

// The scheduled restart takes the planned-restart lease once the FIRED audit
// record is drained; a paddle press during the lease hands it back to the
// planner (5 quiet minutes, 30-minute window) instead of the generic hold.
void w20c_scheduled_restart_uses_the_planned_restart_lease() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.scheduledRestartEnabled = 1;
  runtimeConfig.scheduledRestartMinuteOfDay = 720;
  copyCString(runtimeConfig.timezoneId, sizeof(runtimeConfig.timezoneId),
              "Etc/UTC");
  runLoopAfter(SCHEDULED_RESTART_MIN_UPTIME_MS);
  constexpr uint32_t noon = 1768046400U;  // 2026-01-10 12:00 UTC
  g_wallClock.setSyncing("pool.ntp.org", hostMillis);
  g_wallClock.queueSyncFromCallback(noon - 60);
  CHECK(g_wallClock.applyPendingSync(hostMillis));
  runLoopAfter(1000);
  CHECK(debugEventExists(DebugCode::SCHEDULED_RESTART_ARMED, 0, 720));
  runLoopAfter(60000);
  CHECK(debugEventExists(DebugCode::SCHEDULED_RESTART_FIRED, 0, 720));
  CHECK(!maintenanceLease.active);  // the durable record is still pending
  {
    TaskLockGuard lock(debugLogMutex);
    CHECK(auditLog.hasPending());
    auditLog.clear();
  }
  runLoopAfter(1);
  CHECK(maintenanceLease.active &&
        maintenanceLease.command.type == WebCommandType::RESTART &&
        maintenanceLease.command.requestId == scheduledRestartRequestId);
  CHECK(publishedControlStatus.scheduledRestartState ==
            static_cast<uint8_t>(ScheduledRestartState::SUBMITTED) ||
        scheduledRestartPlanner.state() == ScheduledRestartState::SUBMITTED);

  setRawPaddle(true);
  CHECK(!maintenanceLease.active);
  CHECK(!plannedRestartHeld);
  CHECK(scheduledRestartPlanner.state() == ScheduledRestartState::PENDING);
  setRawPaddle(false);
  runLoopAfter(1000);
  CHECK(debugEventExists(
      DebugCode::SCHEDULED_RESTART_DEFERRED,
      static_cast<int32_t>(ScheduledRestartCause::SHOT), 720));
  CHECK(!maintenanceLease.active && !plannedRestartHeld);
}

void r23_maintenance_is_canceled_fail_open_by_physical_paddle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand command;
  command.type = WebCommandType::RESTART;
  command.requestId = 77;
  processWebCommand(command);
  CHECK(maintenanceLease.active);
  CHECK(!maintenanceLease.forwarded);
  CHECK(!getRelaySafetySnapshot().closed);

  setRawPaddle(true);
  CHECK(!maintenanceLease.active);
  CHECK(!maintenanceCancellationPending);
  CHECK(plannedRestartHeld);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
}

void r23d_restart_waits_for_unpersisted_shot_records() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runLoopAfter(1000);
  lastShotNvsDirty = true;
  shotStorePersistRetryAtMs = millis() + 2000;
  WebCommand command;
  command.type = WebCommandType::RESTART;
  command.requestId = 79;
  processWebCommand(command);
  CHECK(plannedRestartHeld && !maintenanceLease.active);
  runLoopAfter(1000);
  CHECK(plannedRestartHeld && !maintenanceLease.active);
  for (int i = 0; i < 50 && !maintenanceLease.active; ++i) runLoopAfter(100);
  CHECK(maintenanceLease.active && !lastShotNvsDirty);
}

// A paused shot-store image must be acknowledged before maintenance (factory
// reset) runs, or the resumed write would land over the reset slot.
void r23b_maintenance_waits_for_shot_store_persist() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand command;
  command.type = WebCommandType::RESTART;
  command.requestId = 78;
  processWebCommand(command);
  CHECK(maintenanceLease.active);
  shotStorePersistInFlight = true;
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS + 1);
  runLoopAfter(1);
  CHECK(maintenanceLease.active);
  CHECK(!maintenanceLease.forwarded);
  shotStorePersistInFlight = false;
  runLoopAfter(1);
  CHECK(!maintenanceLease.active || maintenanceLease.forwarded);
}

// Micra settings take no maintenance lease and persist on the deferred path:
// configuration stays editable and the cloud is not paused, but a working
// machine still refuses.
void r23c_micra_settings_save_without_a_lease() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand command;
  command.type = WebCommandType::SAVE_LINEA_MICRA;
  command.requestId = 79;
  const uint32_t callsBefore = hostForwardAcceptedNetworkCommandCalls;
  processWebCommand(command);
  CHECK(!maintenanceLease.active);
  CHECK(hostForwardAcceptedNetworkCommandCalls == callsBefore + 1);
  CHECK(hostLastForwardedNetworkCommand.type == WebCommandType::SAVE_LINEA_MICRA);
  CHECK(hostLastForwardedNetworkCommand.maintenanceLeaseId == 0);
  // The network task commits it to the PSRAM record and asks for the
  // deferred settings persist, which the control task schedules.
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  CHECK(!runtimePersistPending);
  networkSettingsPersistRequested.store(true);
  serviceRuntimePersistence();
  CHECK(runtimePersistPending && !networkSettingsPersistRequested.load());

  // The only forward for a refused save is its FAILED result.
  setRawPaddle(true);
  command.requestId = 80;
  processWebCommand(command);
  CHECK(!maintenanceLease.active);
  CHECK(hostLastForwardedNetworkCommand.requestId == 80);
  CHECK(hostLastForwardedNetworkCommand.type != WebCommandType::SAVE_LINEA_MICRA);
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::FAILED);
}

void w86_config_applies_to_ram_immediately_and_coalesces() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t firstRevision = runtimeConfig.revision;
  const uint8_t originalMode = runtimeConfig.noScaleBbwMode;
  const uint8_t alternateMode = originalMode ==
                                        static_cast<uint8_t>(NoScaleBbwMode::OFF)
                                    ? static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE)
                                    : static_cast<uint8_t>(NoScaleBbwMode::OFF);
  const uint32_t originalCooldown = runtimeConfig.lastShotCooldownMs;
  const bool originalAutoTare = runtimeConfig.autoTare;
  WebCommand first;
  first.type = WebCommandType::APPLY_CONFIG;
  first.config = runtimeConfig;
  first.config.noScaleBbwMode = alternateMode;
  first.config.dripDelayMs = 4200;
  first.config.postTareBaselineGraceMs = 3500;
  processWebCommand(first);
  CHECK(!maintenanceLease.active);
  CHECK(runtimeConfig.noScaleBbwMode == alternateMode);
  CHECK(runtimeConfig.lastShotCooldownMs == originalCooldown);
  CHECK(runtimeConfig.autoTare == originalAutoTare);
  CHECK(runtimeConfig.dripDelayMs == 4200);
  CHECK(runtimeConfig.postTareBaselineGraceMs == 3500);
  CHECK(runtimeConfig.revision == firstRevision + 1);
  CHECK(runtimePersistPending);
  CHECK(quickSettingsPersistPending);
  CHECK(hostQuickSettingsWebhookCount == 0);

  WebCommand second;
  second.type = WebCommandType::APPLY_CONFIG;
  second.config = runtimeConfig;
  second.config.lastShotCooldownMs = runtimeConfig.lastShotCooldownMs;
  second.config.noScaleBbwMode = originalMode;
  processWebCommand(second);
  CHECK(runtimeConfig.noScaleBbwMode == originalMode);
  CHECK(runtimeConfig.revision == firstRevision + 2);
  CHECK(runtimePersistPending);
  CHECK(!maintenanceLease.active);

  setRawPaddle(true);
  CHECK(runtimeConfig.noScaleBbwMode == originalMode);
  CHECK(runtimeConfig.revision == firstRevision + 2);
}

void tz01_first_auto_zone_is_once_and_estimated_after_sync() {
  resetHarness(false, false);
  durableTimezoneSaved().store(false);
  reachReadyFromBoot();
  WebCommand first;
  first.type = WebCommandType::APPLY_CONFIG;
  first.config = runtimeConfig;
  strcpy(first.config.timezoneId, "America/Santiago");
  first.config.timezoneSource = 3;
  processWebCommand(first);
  CHECK(strcmp(runtimeConfig.timezoneId, "America/Santiago") == 0);
  CHECK(runtimeConfig.timezoneSource == 1);
  CHECK(runtimeConfig.firstTimezoneAutoClockQuality == 1);
  CHECK(runtimeConfig.firstTimezoneAutoBootId == currentBootId());
  const uint64_t firstUptime = runtimeConfig.firstTimezoneAutoUptimeUs;
  const uint32_t appliedRevision = runtimeConfig.revision;
  first.config = runtimeConfig;
  first.config.timezoneSource = 3;
  processWebCommand(first);
  CHECK(runtimeConfig.revision == appliedRevision + 1);
  CHECK(runtimeConfig.firstTimezoneAutoUptimeUs == firstUptime);
  durableTimezoneSaved().store(true);
  first.config = runtimeConfig;
  first.config.timezoneSource = 3;
  processWebCommand(first);
  CHECK(runtimeConfig.revision == appliedRevision + 1);

  runtimeConfig.revision = UINT32_MAX;
  hostMillis += 10000;
  g_wallClock.setSyncing("pool.ntp.org", hostMillis);
  g_wallClock.queueSyncFromCallback(1'800'000'000U);
  CHECK(g_wallClock.applyPendingSync(hostMillis));
  serviceRuntimePersistence();
  CHECK(runtimeConfig.firstTimezoneAutoClockQuality == 3);
  CHECK(runtimeConfig.revision == 1);
  CHECK(runtimeConfig.firstTimezoneAutoUtcSec ==
        1'800'000'000U - (hostMillis * 1000ULL - firstUptime) / 1000000ULL);
  CHECK(runtimePersistPending);
  WebCommand manual;
  manual.type = WebCommandType::APPLY_CONFIG;
  manual.config = runtimeConfig;
  strcpy(manual.config.timezoneId, "America/New_York");
  manual.config.timezoneSource = 2;
  manual.config.timezoneAutomatic = 0;
  processWebCommand(manual);
  CHECK(strcmp(runtimeConfig.timezoneId, "America/New_York") == 0);
  CHECK(runtimeConfig.timezoneSource == 2);
  CHECK(runtimeConfig.firstTimezoneAutoUtcSec != 0);
  const uint32_t manualRevision = runtimeConfig.revision;
  first.config = runtimeConfig;
  first.config.timezoneSource = 4;
  strcpy(first.config.timezoneId, "Asia/Tokyo");
  processWebCommand(first);
  CHECK(runtimeConfig.revision == manualRevision);
  CHECK(runtimeConfig.timezoneAutomatic == 0);
  manual.config = runtimeConfig;
  manual.config.timezoneAutomatic = 1;
  processWebCommand(manual);
  first.config.revision = runtimeConfig.revision;
  first.config.timezoneAutomatic = 1;
  processWebCommand(first);
  CHECK(strcmp(runtimeConfig.timezoneId, "Asia/Tokyo") == 0);
  CHECK(runtimeConfig.timezoneSource == 1);
  durableTimezoneSaved().store(false);
}

void tz02_unknown_zone_preserves_utc_with_zero_offset() {
  resetHarness(false, false);
  reachReadyFromBoot();
  strcpy(runtimeConfig.timezoneId, "Future/Retired_Zone");
  runtimeConfig.timezoneSource = 2;
  CHECK(validateRuntimeConfig(runtimeConfig) == ConfigValidationError::NONE);
  CHECK(strcmp(timeZoneResolution(runtimeConfig.timezoneId, 1'800'000'000U),
               "unknown_zone") == 0);
  hostMillis = 1000;
  g_wallClock.setSyncing("pool.ntp.org", hostMillis);
  g_wallClock.queueSyncFromCallback(1'800'000'000U);
  CHECK(g_wallClock.applyPendingSync(hostMillis));
  const CycleWallTime wall = captureCycleWallTime();
  CHECK(wall.available);
  CHECK(wall.utcSec == 1'800'000'000U);
  CHECK(wall.offsetMinutes == 0);
  WebCommand bad;
  bad.type = WebCommandType::APPLY_CONFIG;
  bad.config = runtimeConfig;
  strcpy(bad.config.timezoneId, "Invalid/Zone");
  processWebCommand(bad);
  CHECK(strcmp(runtimeConfig.timezoneId, "Future/Retired_Zone") == 0);
  strcpy(runtimeConfig.timezoneId, "America/Santiago");
  CHECK(strcmp(timeZoneResolution(runtimeConfig.timezoneId, 1'700'000'000U),
               "rules_out_of_range") == 0);
}

void w87_nvs_fail_keeps_ram_and_requeues() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t firstRevision = runtimeConfig.revision;
  const uint8_t originalMode = runtimeConfig.noScaleBbwMode;
  const uint8_t alternateMode = originalMode ==
                                        static_cast<uint8_t>(NoScaleBbwMode::OFF)
                                    ? static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE)
                                    : static_cast<uint8_t>(NoScaleBbwMode::OFF);
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.noScaleBbwMode = alternateMode;
  processWebCommand(update);
  CHECK(runtimeConfig.noScaleBbwMode == alternateMode);
  CHECK(runtimeConfig.revision == firstRevision + 1);
  CHECK(runtimePersistPending);

  hostRuntimePersistSucceeds = false;
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  CHECK(runtimeConfig.noScaleBbwMode == alternateMode);
  CHECK(runtimeConfig.revision == firstRevision + 1);
  CHECK(runtimePersistPending);
  CHECK(runtimePersistFailed);
  CHECK(hostRuntimePersistAttempts >= 1);
  CHECK(publishedControlStatus.configPersistFailed);
  CHECK(hostQuickSettingsWebhookCount == 0);

  hostRuntimePersistSucceeds = true;
  runLoopAfter(RUNTIME_PERSIST_RETRY_MS + 1);
  CHECK(!runtimePersistPending);
  CHECK(!runtimePersistFailed);
  CHECK(hostLastFlushIncludedLive);
  CHECK(hostLastFlushedRuntime.revision == runtimeConfig.revision);
  CHECK(hostLastFlushedRuntime.noScaleBbwMode == alternateMode);
  CHECK(hostQuickSettingsWebhookCount == 1);
}

void w87b_runtime_persist_failure_is_logged_once_per_episode() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostRuntimePersistSucceeds = false;
  runtimePersistPending = true;
  runtimePersistReasonBits = RUNTIME_PERSIST_REASON_OFFSET;
  runtimePersistRetryAtMs = millis();
  debugLog.clear();
  serviceRuntimePersistence();
  CHECK(runtimePersistFailed);
  CHECK(debugEventExists(DebugCode::RUNTIME_PERSIST_FAILED));

  debugLog.clear();
  hostMillis += runtimePersistIoRetryMs;
  serviceRuntimePersistence();
  CHECK(!debugEventExists(DebugCode::RUNTIME_PERSIST_FAILED));

  hostRuntimePersistSucceeds = true;
  hostMillis += runtimePersistIoRetryMs;
  serviceRuntimePersistence();
  CHECK(!runtimePersistFailed);
  hostRuntimePersistSucceeds = false;
  queueRuntimePersist(RUNTIME_PERSIST_REASON_USER);
  hostMillis += RUNTIME_PERSIST_DEBOUNCE_MS;
  debugLog.clear();
  serviceRuntimePersistence();
  CHECK(debugEventExists(DebugCode::RUNTIME_PERSIST_FAILED));
}

void w87c_runtime_persist_io_backoff_is_bounded_and_resets() {
  resetHarness(false, false);
  reachReadyFromBoot();
  hostRuntimePersistSucceeds = false;
  runtimePersistPending = true;
  runtimePersistRetryAtMs = millis();
  for (uint32_t expected : {500U, 1000U, 2000U, 4000U, 8000U, 16000U,
                            30000U, 30000U}) {
    serviceRuntimePersistence();
    CHECK(runtimePersistIoRetryMs == expected);
    CHECK(runtimePersistRetryAtMs - millis() == expected);
    hostMillis = runtimePersistRetryAtMs;
  }
  queueRuntimePersist(RUNTIME_PERSIST_REASON_OFFSET);
  CHECK(runtimePersistIoRetryMs == 0);
  hostRuntimePersistSucceeds = true;
  hostMillis = runtimePersistRetryAtMs;
  serviceRuntimePersistence();
  CHECK(!runtimePersistPending);
  CHECK(runtimePersistIoRetryMs == 0);
}

void w88_save_network_flush_includes_live_runtime() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const uint32_t firstRevision = runtimeConfig.revision;
  WebCommand apply;
  apply.type = WebCommandType::APPLY_CONFIG;
  apply.config = runtimeConfig;
  apply.config.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  processWebCommand(apply);
  CHECK(runtimePersistPending);
  CHECK(runtimeConfig.revision == firstRevision + 1);

  WebCommand network;
  network.setNetworkType(WebCommandType::SAVE_NETWORK);
  strcpy(network.network.ssid, "CafeLAN");
  strcpy(network.network.password, "CafePass1");
  processWebCommand(network);
  finishHostMaintenance();
  CHECK(hostLastFlushIncludedLive);
  CHECK(hostLastFlushedRuntime.revision == runtimeConfig.revision);
  CHECK(hostLastFlushedRuntime.noScaleBbwMode ==
        runtimeConfig.noScaleBbwMode);
  CHECK(!runtimePersistPending);
}

void w89_restart_flush_is_best_effort() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand apply;
  apply.type = WebCommandType::APPLY_CONFIG;
  apply.config = runtimeConfig;
  apply.config.lastShotCooldownMs = runtimeConfig.lastShotCooldownMs + 1000;
  processWebCommand(apply);
  const uint32_t liveRevision = runtimeConfig.revision;
  CHECK(runtimePersistPending);

  hostRuntimePersistSucceeds = false;
  WebCommand restart;
  restart.type = WebCommandType::RESTART;
  processWebCommand(restart);
  finishHostMaintenance();
  CHECK(hostLastMaintenanceSucceeded);
  CHECK(runtimeConfig.revision == liveRevision);
  CHECK(runtimeConfig.lastShotCooldownMs == apply.config.lastShotCooldownMs);
  CHECK(runtimePersistPending);
  CHECK(runtimePersistFailed);
  CHECK(hostLastFlushedRuntime.revision == liveRevision);

  hostRuntimePersistSucceeds = true;
  processWebCommand(restart);
  finishHostMaintenance();
  CHECK(!runtimePersistPending);
  CHECK(!runtimePersistFailed);
  CHECK(hostLastFlushedRuntime.revision == liveRevision);
}

void r24_web_control_is_available_without_session_owner() {
  resetHarness(false, false);
  reachReadyFromBoot();
  processWebCommand(webControlCommand(WebCommandType::REMOTE_ON));
  CHECK(session.active);
  CHECK(session.source == ControlSource::WEB);
  CHECK(getRelaySafetySnapshot().closed);

  WebCommand paddleOff = webControlCommand(WebCommandType::REMOTE_OFF);
  processWebCommand(paddleOff);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);

  WebCommand emergencyStop;
  emergencyStop.type = WebCommandType::STOP;
  processWebCommand(emergencyStop);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
}

void prepareIdleTare();
void idleCup(float weight);
void idleWeight(float weight, uint32_t dtMs = 150);

void race01_idle_tare_validates_after_command_spacing() {
  for (bool expire : {false, true}) {
    prepareIdleTare();
    idleCup(80.0f);
    const ScaleCommand command = queuedCommandAt(0);
    scale.weight = 80.0f;
    scale.weightCaptureSequence = 40;
    scale.beforeCommandAdmission = [&, expire] {
      if (expire) hostMillis = command.expiresAtMs;
      else {
        scale.weight = 0.0f;
        ++hostMillis;
        scale.weightCaptureSequence = 41;
        scale.newWeightAvailableValue = true;
      }
    };
    CHECK(executeNextScaleCommand());
    scale.beforeCommandAdmission = {};
    serviceIdleTare();
    CHECK(scale.tareCalls == 0);
    CHECK(!cupPresenceIsTared());
    CHECK(idleTare.lastReason != IdleTareReason::EFFECT_CONFIRMED);
    if (expire) CHECK(idleTare.lastReason == IdleTareReason::EXPIRED);
    else {
      CHECK(idleTare.requestId == 0);
      CHECK(executeNextScaleCommand()); // Cancelled queue entry cannot write.
      CHECK(scale.tareCalls == 0);
    }
  }
}

void race02_retare_result_preserves_post_reference_weights() {
  for (bool together : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    CHECK(requestRemoteRetare());
    ScaleCommand command;
    CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
    scale.weightCaptureSequence = 40;
    executeScaleCommand(command);
    const ScaleEvent result = shotTareResult;
    CHECK(result.writeSucceeded);
    if (!together) processScaleWorkerEvents();
    ScaleEvent sample;
    sample.type = ScaleEventType::WEIGHT;
    sample.receivedAtMs = ++hostMillis;
    sample.captureSequence = 40;
    sample.weightG = 80.0f;
    CHECK(publishScaleEvent(sample, false));
    sample.captureSequence = 41;
    sample.weightG = 0.0f;
    CHECK(publishScaleEvent(sample, false));
    processScaleWorkerEvents();
    CHECK(!session.awaitingPostTareBaseline);
    CHECK(shot.datapoints == 1);
    CHECK(shot.weight[0] == 0.0f);
    CHECK(publishScaleEvent(result, true)); // Duplicate result is idempotent.
    ScaleEvent older = result;
    --older.cupWeightRequestId;
    CHECK(publishScaleEvent(older, true));
    processScaleWorkerEvents();
    CHECK(!session.awaitingPostTareBaseline);
    CHECK(shot.datapoints == 1);
  }
}

void race03_ended_cycle_cancels_queued_and_paced_retare() {
  for (bool duringSpacing : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    CHECK(requestRemoteRetare());
    const auto before = scale.tareCalls;
    if (duringSpacing) scale.beforeCommandAdmission = [] {
      scale.beforeCommandAdmission = {};
      (void)finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
    };
    else CHECK(finalizeCycle(EndReason::ACTIVATOR, StopperState::READY));
    while (executeNextScaleCommand()) {}
    scale.beforeCommandAdmission = {};
    CHECK(!session.active);
    CHECK(scale.tareCalls == before);
    CHECK(scale.stopTimerCalls == 1);
  }
}

void race05_shutdown_during_spacing_blocks_all_operational_writes() {
  for (unsigned operation = 0; operation < 9; ++operation) {
    resetHarness(false, false);
    scale.connectedFeatures.flags |= ScaleFeaturePowerOff;
    setScaleConnected(true);
    scale.beforeCommandAdmission = [] { requestScalePowerOff(); };
    if (operation < 6) {
      const auto action = static_cast<BookooDebugAction>(operation);
      CHECK(enqueueScaleDebugCommand(action, 1));
      BookooDebugAction taken;
      uint8_t level;
      CHECK(takeScaleDebugCommand(taken, level));
      executeScaleDebugCommand(taken, level);
    } else if (operation == 6) {
      ScaleCommand stop;
      stop.type = ScaleCommandType::STOP_TIMER;
      stop.connectionGeneration = getScaleLinkSnapshot().connectionGeneration;
      executeScaleStopCommand(stop);
    } else if (operation == 7) {
      executeScaleBeepCommand(DebugCode::SCALE_DEBUG_OK, DebugCode::SCALE_DEBUG_FAILED,
                              DebugCode::SCALE_DEBUG_UNSUPPORTED);
    } else {
      applyBookooConnectBeepPolicy();
    }
    scale.beforeCommandAdmission = {};
    CHECK(scalePowerOffPending);
    CHECK(scale.commandLog.empty());
    CHECK(takeScalePowerOff());
    executeScalePowerOffCommand();
    CHECK(scale.commandLog.size() == 1 && scale.commandLog[0] == "powerOff");
  }
  resetHarness(false, false);
  scale.connectedFeatures.flags |= ScaleFeaturePowerOff;
  setScaleConnected(true);
  scale.duringTareWrite = [] { requestScalePowerOff(); };
  executeScaleDebugCommand(BookooDebugAction::TARE, 0);
  scale.duringTareWrite = {};
  CHECK(scalePowerOffPending && scale.tareCalls == 1); // Already admitted.
}

void race06_connection_cleanup_preserves_new_shutdown() {
  resetHarness(false, false);
  scale.connectedFeatures.flags |= ScaleFeaturePowerOff;
  scale.connected = true;
  portENTER_CRITICAL(&scaleBeepMux);
  std::thread worker([] { updateWorkerLinkState(); });
  while (getScaleLinkSnapshot().state != ScaleLinkState::CONNECTED)
    std::this_thread::yield();
  requestScalePowerOff();
  const bool accepted = scalePowerOffPending;
  portEXIT_CRITICAL(&scaleBeepMux);
  worker.join();
  CHECK(accepted);
  CHECK(scalePowerOffBlocksGeneration(getScaleLinkSnapshot().connectionGeneration));
  CHECK(takeScalePowerOff());
  CHECK(!takeScalePowerOff());
  executeScalePowerOffCommand();
  CHECK(scale.powerOffCalls == 1);
}

void race04_preferred_ack_preserves_newer_history() {
  resetHarness(false, true);
  notePreferredScale("AA:BB:CC:DD:EE:01", "Scale A");
  char mac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char name[PREFERRED_SCALE_NAME_CAPACITY] = {};
  ScaleHistoryEntry history[SCALE_HISTORY_CAPACITY] = {};
  CHECK(scaleWorkerCopyPreferredIfDirty(mac, name, history));
  notePreferredScale("AA:BB:CC:DD:EE:02", "Scale B");
  CHECK(!scaleWorkerClearPreferredDirty(mac, name, history));
  CHECK(scaleWorkerCopyPreferredIfDirty(mac, name, history));
  CHECK(scaleWorkerClearPreferredDirty(mac, name, history));
  CHECK(!scaleWorkerCopyPreferredIfDirty(mac, name, history));
}

void race07_result_and_weight_selection_is_atomic() {
  for (unsigned delivery = 0; delivery < 4; ++delivery) {
    resetHarness(false, true);
    hostMillis = 1000;
    markScaleWorkerProgress();
    session.active = true;
    session.id = 7;
    session.startedAtMs = 500;
    session.startedWithScale = true;
    session.retarePerformed = delivery != 0;
    session.retareRequestId = 901;
    session.appliedTareRequestId = 900;
    session.tareConnectionGeneration = getScaleLinkSnapshot().connectionGeneration;
    session.tareCaptureBoundary = 10;
    stopperState = StopperState::BREW;
    ScaleEvent weight;
    weight.type = ScaleEventType::WEIGHT;
    weight.receivedAtMs = hostMillis;
    weight.captureSequence = 11;
    weight.weightG = 37;
    CHECK(publishScaleEvent(weight, false));
    ScaleEvent result;
    result.type = delivery == 0 ? ScaleEventType::TIMER_START_RESULT : ScaleEventType::TARE_RESULT;
    result.cycleId = session.id;
    result.cupWeightRequestId = session.retareRequestId;
    result.captureSequence = 11;
    result.receivedAtMs = hostMillis;
    result.writeSucceeded = result.tareSucceeded = true;
    if (delivery == 2) { // Full result FIFO uses the critical fallback.
      ScaleEvent filler;
      filler.type = ScaleEventType::TIMER_STOP_RESULT;
      for (size_t i = 0; i < SCALE_EVENT_QUEUE_LENGTH; ++i)
        CHECK(publishScaleEvent(filler, true));
    }
    if (delivery == 3) {
      // Publish only the retained completion after control's entry snapshot
      // has already been copied. Its queue event was lost/overwritten.
      static thread_local ScaleEvent retained;
      retained = result;
      retained.connectionGeneration = session.tareConnectionGeneration;
      TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
        if (mutex == &scaleCriticalEventMux && !acquired) {
          shotTareResult = retained;
          TaskMutex::hostObserver = nullptr;
        }
      };
    } else {
      CHECK(publishScaleEvent(result, true));
    }
    processScaleWorkerEvents();
    TaskMutex::hostObserver = nullptr;
    processScaleWorkerEvents();
    CHECK(observedWeight != 37);
    CHECK(session.tareCaptureBoundary == 11);

    // Observe the exact former race window: an empty result queue must remain
    // protected until the weight has been selected from the same handoff.
    static thread_local bool held, atomicSelection, emptySeen, selectedBeforeUnlock;
    held = emptySeen = selectedBeforeUnlock = false;
    atomicSelection = true;
    weight.captureSequence = 12;
    weight.weightG = 0;
    CHECK(publishScaleEvent(weight, false));
    TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
      if (mutex != &scaleCriticalEventMux) return;
      if (!acquired && emptySeen && scaleWeightEventCount == 0)
        selectedBeforeUnlock = true;
      held = acquired;
    };
    hostAfterEmptyQueueReceive = [](QueueHandle_t queue) {
      if (queue == scaleEventQueue && scaleWeightEventCount != 0) {
        emptySeen = true;
        atomicSelection = atomicSelection && held;
      }
    };
    processScaleWorkerEvents();
    hostAfterEmptyQueueReceive = nullptr;
    TaskMutex::hostObserver = nullptr;
    CHECK(emptySeen && atomicSelection && selectedBeforeUnlock);
    CHECK(observedWeight == 0 && observedWeightSequence != 0);
  }
}

void r25_critical_scale_mailbox_never_blocks_and_keeps_latest() {
  resetHarness(false, true);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  ScaleEvent first;
  first.type = ScaleEventType::TIMER_STOP_RESULT;
  first.cycleId = 1;
  first.commandAttempted = true;
  ScaleEvent latest = first;
  latest.cycleId = 2;
  ScaleEvent filler = first;
  filler.cycleId = UINT32_MAX;
  filler.connectionGeneration = link.connectionGeneration;
  filler.disconnectSequence = link.disconnectSequence;
  for (size_t index = 0; index < SCALE_EVENT_QUEUE_LENGTH; ++index) {
    CHECK(xQueueSend(scaleEventQueue, &filler, 0) == pdTRUE);
  }
  CHECK(publishScaleEvent(first, true));
  CHECK(publishScaleEvent(latest, true));
  CHECK(scaleCriticalEventPending);
  CHECK(scaleWorkerDroppedEventCount() == 1);
  CHECK(scaleCriticalEvent.cycleId == 2);
  ScaleEvent startResult = first;
  startResult.type = ScaleEventType::TIMER_START_RESULT;
  startResult.cycleId = 3;
  CHECK(publishScaleEvent(startResult, true));
  CHECK(scaleTimerStartEventPending);
  CHECK(scaleCriticalEventPending);
  processScaleWorkerEvents();
  CHECK(!scaleCriticalEventPending);
  CHECK(!scaleTimerStartEventPending);
}

void r25b_empty_mailboxes_take_one_critical_lock() {
  resetHarness(false, true);
  processScaleWorkerEvents();
  static thread_local unsigned acquisitions = 0;
  acquisitions = 0;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &scaleCriticalEventMux && acquired) ++acquisitions;
  };
  processScaleWorkerEvents();
  processScaleWorkerEvents();
  TaskMutex::hostObserver = nullptr;
  CHECK(acquisitions == 2);
}

void r25c_refilled_critical_mailboxes_remain_live_and_bounded() {
  resetHarness(false, true);
  constexpr uint32_t batches = 2000;
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  std::atomic<bool> done{false};
  std::thread producer([&, link]() {
    for (uint32_t id = 1; id <= batches;) {
      bool published = false;
      {
        TaskLockGuard lock(scaleCriticalEventMux);
        if (!scaleCriticalEventPending && !scaleTimerStartEventPending) {
          ScaleEvent stop;
          stop.type = ScaleEventType::TIMER_STOP_RESULT;
          stop.cycleId = id;
          stop.discardedStaleConnection = true;
          stop.connectionGeneration = link.connectionGeneration;
          stop.disconnectSequence = link.disconnectSequence;
          scaleCriticalEvent = stop;
          scaleCriticalEventPending = true;
          stop.type = ScaleEventType::TIMER_START_RESULT;
          scaleTimerStartEvent = stop;
          scaleTimerStartEventPending = true;
          ++id;
          published = true;
        }
      }
      if (!published) std::this_thread::yield();
    }
    done.store(true, std::memory_order_release);
  });
  static thread_local unsigned acquisitions = 0;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &scaleCriticalEventMux && acquired) ++acquisitions;
  };
  bool bounded = true;
  do {
    acquisitions = 0;
    processScaleWorkerEvents();
    bounded = bounded && acquisitions <= SCALE_EVENT_QUEUE_LENGTH + 1;
  } while (!done.load(std::memory_order_acquire));
  TaskMutex::hostObserver = nullptr;
  producer.join();
  processScaleWorkerEvents();
  CHECK(bounded);
  CHECK(!scaleCriticalEventPending && !scaleTimerStartEventPending);
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  CHECK(count >= 2);
  CHECK(events[count - 2].code == DebugCode::SCALE_TIMER_STOP_FAILED);
  CHECK(events[count - 1].code == DebugCode::SCALE_TIMER_START_FAILED);
}

void r26_remote_timer_stop_retries_after_full_queue() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());

  ScaleCommand filler;
  filler.type = ScaleCommandType::STOP_TIMER;
  filler.cycleId = UINT32_MAX;
  for (size_t index = 0; index < SCALE_COMMAND_QUEUE_LENGTH; ++index) {
    CHECK(xQueueSend(scaleCommandQueue, &filler, 0) == pdTRUE);
  }
  WebCommand stop;
  stop.type = WebCommandType::STOP;
  processWebCommand(stop);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(session.stopTimerRequested);
  CHECK(!session.stopTimerCommandQueued);

  ScaleCommand discarded;
  while (xQueueReceive(scaleCommandQueue, &discarded, 0) == pdTRUE) {
  }
  runLoopAfter(SCALE_STOP_RETRY_INTERVAL_MS);
  CHECK(session.stopTimerCommandQueued);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
}

void r27_platform_clock_failure_prevents_circuit_close() {
  resetHarness(false, false);
  reachReadyFromBoot();
  platformClockReady = false;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().state == RelaySafetyState::LOCKOUT);
  CHECK(getRelaySafetySnapshot().fault ==
        RelaySafetyFault::INITIALIZATION_FAILED);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
}

void r28_terminal_control_result_is_retained_until_forwarded() {
  resetHarness(false, false);
  reachReadyFromBoot();
  processWebCommand(webControlCommand(WebCommandType::REMOTE_ON));
  CHECK(session.active);
  hostForwardAcceptedNetworkCommandSucceeds = false;

  WebCommand stop;
  stop.type = WebCommandType::STOP;
  stop.requestId = 91;
  processWebCommand(stop);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(controlResultPending);
  CHECK(controlResultCommand.requestId == 91);
  CHECK(controlResultCommand.resultState == CommandResultState::APPLIED);

  hostForwardAcceptedNetworkCommandSucceeds = true;
  serviceControlCommandResult();
  CHECK(!controlResultPending);
  CHECK(hostLastForwardedNetworkCommand.requestId == 91);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::APPLIED);
}

void r29_direct_threshold_stops_before_regression_is_ready() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  CHECK(shot.expectedEndS == NO_CUT_PREDICTION_S);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  CHECK(getRelaySafetySnapshot().closed);
  publishWeight(threshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  CHECK(!getRelaySafetySnapshot().closed);
}

void r30_first_abrupt_sample_uses_pre_shot_baseline() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runtimeConfig.autoTare = false;
  startCycle();
  resetWeightTrend();
  CHECK(session.hasWeightAnchor);
  CHECK(session.lastAcceptedWeightG == 0.0f);
  publishWeight(900.0f, hostMillis + 1);
  CHECK(shot.datapoints == 0);
  CHECK(session.weightControlState == WeightControlState::VALIDATING);
}

void r31_confirmed_overload_opens_without_learning() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishWeight(1500.0f);
  publishWeight(1501.0f, hostMillis + 1);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::WEIGHT_ANOMALY);
  CHECK(!session.calibrationEligible);
  CHECK(!pendingFinalize.pending);
  CHECK(!pendingFinalize.offsetAnalysis);
}

void r32_old_connection_generation_cannot_update_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t generation = getScaleLinkSnapshot().connectionGeneration;
  CHECK(generation > 0);
  const uint32_t observedBefore = observedWeightSequence;
  publishWeight(12.0f, hostMillis, generation + 1, 77);
  CHECK(observedWeightSequence == observedBefore);
  CHECK(currentWeight != 12.0f);
}

void r33_weight_mailbox_keeps_latest_without_consumer_gap() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ScaleEvent first;
  first.type = ScaleEventType::WEIGHT;
  first.receivedAtMs = hostMillis;
  first.weightG = 1.0f;
  ScaleEvent latest = first;
  latest.weightG = 2.0f;
  CHECK(publishScaleEvent(first, false));
  debugLog.clear();
  CHECK(publishScaleEvent(latest, false));
  CHECK(scalePacketGaps == 0);
  CHECK(!debugEventExists(DebugCode::SCALE_PACKET_GAP));
  latest.weightG = 3.0f;
  CHECK(publishScaleEvent(latest, false));
  CHECK(scalePacketGaps == 0);
  processScaleWorkerEvents();
  CHECK(observedWeight == 3.0f);
}

void r33b_stream_gap_counts_connected_inter_packet_silence() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ScaleEvent sample;
  sample.type = ScaleEventType::WEIGHT;
  sample.receivedAtMs = hostMillis;
  sample.weightG = 1.0f;
  CHECK(publishScaleEvent(sample, false));
  CHECK(scalePacketGaps == 0);
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == 0);

  hostMillis += SCALE_STREAM_GAP_MS;
  sample.receivedAtMs = hostMillis;
  sample.weightG = 1.1f;
  debugLog.clear();
  CHECK(publishScaleEvent(sample, false));
  CHECK(scalePacketGaps == 0);
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == 0);

  hostMillis += SCALE_STREAM_GAP_MS + 1;
  sample.receivedAtMs = hostMillis;
  sample.weightG = 1.2f;
  CHECK(publishScaleEvent(sample, false));
  CHECK(scalePacketGaps == 1);
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == SCALE_STREAM_GAP_MS + 1);
  CHECK(debugEventExists(DebugCode::SCALE_PACKET_GAP));

  debugLog.clear();
  hostMillis += SCALE_STREAM_GAP_MS + 1;
  sample.receivedAtMs = hostMillis;
  sample.weightG = 1.3f;
  CHECK(publishScaleEvent(sample, false));
  CHECK(scalePacketGaps == 2);
  CHECK(!debugEventExists(DebugCode::SCALE_PACKET_GAP));
  hostMillis += SCALE_PACKET_GAP_LOG_MIN_MS;
  sample.receivedAtMs = hostMillis;
  sample.weightG = 1.4f;
  CHECK(publishScaleEvent(sample, false));
  CHECK(debugEventExists(DebugCode::SCALE_PACKET_GAP));

  setScaleConnected(false);
  const uint32_t maxBeforeReconnect = getScaleLinkSnapshot().maxPacketGapMs;
  CHECK(maxBeforeReconnect == SCALE_PACKET_GAP_LOG_MIN_MS);
  hostMillis += 5000;
  setScaleConnected(true);
  sample.receivedAtMs = hostMillis;
  sample.weightG = 2.0f;
  const uint32_t gapsBeforeReconnect = scalePacketGaps;
  CHECK(publishScaleEvent(sample, false));
  CHECK(scalePacketGaps == gapsBeforeReconnect);
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == maxBeforeReconnect);

  sample.receivedAtMs += SCALE_STREAM_GAP_MS + 1;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == maxBeforeReconnect);
  const uint32_t gapsBeforeReset = scalePacketGaps;
  const uint32_t lastWeightBeforeReset = lastScaleWeightAtMs;
  const uint32_t intervalBeforeReset = scaleWeightUpdateIntervalMs;
  requestScaleGapMaxReset();
  markScaleWorkerProgress();
  markScaleWorkerProgress();
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == 0);
  CHECK(scalePacketGaps == gapsBeforeReset);
  CHECK(lastScaleWeightAtMs == lastWeightBeforeReset);
  CHECK(scaleWeightUpdateIntervalMs == intervalBeforeReset);
  sample.receivedAtMs += SCALE_STREAM_GAP_MS + 5;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().maxPacketGapMs == SCALE_STREAM_GAP_MS + 5);
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.scaleMaxPacketGapMs == SCALE_STREAM_GAP_MS + 5);

  processScaleWorkerEvents();
  CHECK(observedWeight == 2.0f);
}

void r33c_weight_update_interval_is_smoothed_and_reset_safely() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ScaleEvent sample;
  sample.type = ScaleEventType::WEIGHT;
  sample.receivedAtMs = hostMillis;
  sample.weightG = 1.0f;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().weightUpdateIntervalMs == 0);

  sample.receivedAtMs += 100;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().weightUpdateIntervalMs == 100);

  sample.receivedAtMs += 108;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().weightUpdateIntervalMs == 101);

  sample.receivedAtMs += SCALE_STREAM_GAP_MS + 1;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().weightUpdateIntervalMs == 0);

  setScaleConnected(false);
  setScaleConnected(true);
  sample.receivedAtMs += 100;
  CHECK(publishScaleEvent(sample, false));
  CHECK(getScaleLinkSnapshot().weightUpdateIntervalMs == 0);
}

void r34_suspended_control_recovers_after_three_attributed_samples() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  setScaleConnected(false);
  loop();
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  setScaleConnected(true);
  publishWeight(1.0f);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  publishWeight(2.0f, hostMillis + 1);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  publishWeight(3.0f, hostMillis + 2);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(session.automaticEnabled);
  loop();
  CHECK(stopperState == StopperState::BREW);

  // Also cover a disconnect/reconnect that occurs wholly between control-loop
  // iterations: the connection generation must suspend authority before the
  // new sample can enter the old trajectory.
  setScaleConnected(false);
  setScaleConnected(true);
  publishWeight(4.0f, hostMillis + 3);
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(shot.datapoints == 0);
  publishWeight(5.0f, hostMillis + 4);
  publishWeight(6.0f, hostMillis + 5);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
}

void r35_connected_without_weight_stream_is_not_available() {
  resetHarness(false, true);
  reachReadyFromBoot();
  observedWeightSequence = 0;
  setScaleWorkerTaskPresentForHost(true);
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::CONNECTED);
  // The publish no longer services weight telemetry itself (housekeeping
  // owns that in production); service it explicitly in the same order.
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.scaleAvailable);
  CHECK(status.weightStreamState == WeightStreamState::NO_SAMPLE);
  CHECK(!status.observedWeightValid);

  publishWeight(1500.0f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.observedWeightValid);
  CHECK(status.observedWeightG == 1500.0f);
  CHECK(status.weightStreamState == WeightStreamState::OVERLOAD);
  CHECK(!status.currentWeightValid);

  publishWeight(10.0f);
  setScaleConnected(false);
  setScaleConnected(true);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(!status.currentWeightValid);
}

void r36_recovered_stale_metrics_count_connected_gaps_only() {
  ControlStatusSnapshot status;

  resetHarness(false, true);
  reachReadyFromBoot();
  publishWeight(12.0f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::FRESH);
  CHECK(status.scaleRecoveredStaleCount == 0);
  CHECK(status.scaleRecoveredStaleMs == 0);

  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 100;
  markScaleWorkerProgress();
  loop();
  CHECK(telemetryWeightStreamState == WeightStreamState::STALE);
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::STALE);
  CHECK(status.scaleRecoveredStaleCount == 0);
  CHECK(status.scaleRecoveredStaleMs == 0);

  hostMillis += 250;
  markScaleWorkerProgress();
  publishWeight(12.1f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::FRESH);
  CHECK(status.scaleRecoveredStaleCount == 1);
  CHECK(status.scaleRecoveredStaleMs == 250);

  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 100;
  markScaleWorkerProgress();
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::STALE);
  CHECK(status.scaleRecoveredStaleCount == 1);
  hostMillis += 400;
  markScaleWorkerProgress();
  publishWeight(12.2f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::FRESH);
  CHECK(status.scaleRecoveredStaleCount == 2);
  CHECK(status.scaleRecoveredStaleMs == 650);

  resetHarness(false, true);
  reachReadyFromBoot();
  publishWeight(12.0f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 100;
  markScaleWorkerProgress();
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::STALE);
  setScaleConnected(false);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.scaleRecoveredStaleCount == 0);
  CHECK(status.scaleRecoveredStaleMs == 0);

  setScaleConnected(true);
  publishWeight(12.0f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::FRESH);
  CHECK(status.scaleRecoveredStaleCount == 0);

  resetHarness(false, true);
  reachReadyFromBoot();
  publishWeight(12.0f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  setScaleConnected(false);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  setScaleConnected(true);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::STALE);
  CHECK(status.scaleRecoveredStaleCount == 0);
  publishWeight(12.0f);
  serviceWeightStreamTelemetry(getScaleLinkSnapshot());
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.weightStreamState == WeightStreamState::FRESH);
  CHECK(status.scaleRecoveredStaleCount == 0);
  CHECK(status.scaleRecoveredStaleMs == 0);
}

bool debugEventExists(DebugCode code, int32_t argument1,
                      int32_t argument2) {
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  for (size_t index = 0; index < count; ++index) {
    const DebugEvent &event = events[index];
    if (event.code != code) {
      continue;
    }
    if (argument1 != INT32_MIN && event.argument1 != argument1) {
      continue;
    }
    if (argument2 != INT32_MIN && event.argument2 != argument2) {
      continue;
    }
    return true;
  }
  return false;
}

void w25b_log_levels_and_cycle_events_reach_ring() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ringRetainLogLevel = LogLevel::INFO;
  serialLogLevel = LogLevel::NONE;
  startCycle();
  CHECK(debugEventExists(DebugCode::CYCLE_STARTED));
  CHECK(debugEventExists(DebugCode::BREW_STARTED) ||
        debugEventExists(DebugCode::MANUAL_CYCLE_STARTED) ||
        debugEventExists(DebugCode::TIMER_ONLY_BREW_STARTED));
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  bool sawLevel = false;
  for (size_t index = 0; index < count; ++index) {
    if (events[index].code == DebugCode::CYCLE_STARTED) {
      CHECK(events[index].level == LogLevel::INFO);
      sawLevel = true;
    }
  }
  CHECK(sawLevel);
  char message[128] = {};
  DebugEvent circuit = {};
  circuit.code = DebugCode::CIRCUIT_ARM_FAILED;
  circuit.argument1 = static_cast<int32_t>(CircuitArmFailReason::SAFETY_LOCKOUT);
  CHECK(formatLifecycleDebugMessage(circuit, message, sizeof(message)));
  CHECK(std::string(message).find("safety lockout") != std::string::npos);
  CHECK(debugCodeDefaultLevel(DebugCode::CIRCUIT_ARM_FAILED) ==
        LogLevel::CRITICAL);
  CHECK(debugCodeDefaultLevel(DebugCode::SCALE_CONNECTING) == LogLevel::DEBUG);
  CHECK(!logLevelAtMost(LogLevel::CRITICAL, LogLevel::NONE));
  CHECK(!logLevelAtMost(LogLevel::DEBUG, LogLevel::NONE));
  CHECK(logLevelAtMost(LogLevel::WARNING, LogLevel::INFO));
  CHECK(logLevelAtMost(LogLevel::INFO, LogLevel::INFO));
  CHECK(!logLevelAtMost(LogLevel::DEBUG, LogLevel::INFO));
}

void w25c_ring_retain_none_skips_ring_and_error_filters() {
  resetHarness(false, true);
  reachReadyFromBoot();
  debugLog.clear();
  ringRetainLogLevel = LogLevel::NONE;
  serialLogLevel = LogLevel::NONE;
  addDebugEvent(DebugCategory::STATE, DebugCode::CYCLE_STARTED);
  addDebugEvent(DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
                static_cast<int32_t>(CircuitArmFailReason::SAFETY_LOCKOUT));
  DebugEvent noneEvents[DEBUG_EVENT_CAPACITY] = {};
  CHECK(copyDebugEvents(0, noneEvents, DEBUG_EVENT_CAPACITY) == 0);

  debugLog.clear();
  ringRetainLogLevel = LogLevel::ERROR;
  addDebugEvent(DebugCategory::STATE, DebugCode::CYCLE_STARTED);
  addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_CONNECTING);
  addDebugEvent(DebugCategory::RELAY, DebugCode::CIRCUIT_ARM_FAILED,
                static_cast<int32_t>(CircuitArmFailReason::SAFETY_LOCKOUT));
  DebugEvent errorEvents[DEBUG_EVENT_CAPACITY] = {};
  const size_t count =
      copyDebugEvents(0, errorEvents, DEBUG_EVENT_CAPACITY);
  CHECK(count == 1);
  CHECK(errorEvents[0].code == DebugCode::CIRCUIT_ARM_FAILED);
  CHECK(errorEvents[0].level == LogLevel::CRITICAL);
}

void w25d_ble_and_scale_lines_use_their_categories() {
  resetHarness(false, true);
  reachReadyFromBoot();
  debugLog.clear();
  ringRetainLogLevel = LogLevel::DEBUG;
  serialLogLevel = LogLevel::NONE;
  CHECK(strcmp(debugCategoryName(DebugCategory::BLE), "ble") == 0);
  CHECK(static_cast<uint8_t>(DebugCategory::BLE) ==
        static_cast<uint8_t>(DebugCategory::SYSTEM) + 1);
  CHECK(espLogCategory("NimBLE: GAP procedure initiated") == DebugCategory::BLE);
  CHECK(espLogCategory("BLE_INIT: BT controller compile version") ==
        DebugCategory::BLE);
  CHECK(espLogCategory("NimBLEx: same prefix") == DebugCategory::SYSTEM);
  CHECK(espLogCategory("phy_init: phy_version 700") == DebugCategory::SYSTEM);
  CHECK(espLogCategory("wifi:connected") == DebugCategory::SYSTEM);
  openBrewByWeightScaleLog(4, true, "ble teardown complete raw=0 at_ms=1");
  openBrewByWeightScaleLog(0, false, "ready: acaia @ AA:BB");
  selectPreferredScale("AA:BB:CC:DD:EE:FF", "LUNAR");
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  auto categoryOf = [&](const char *prefix) {
    for (size_t i = 0; i < count; ++i) {
      if (events[i].code == DebugCode::LOG_TEXT &&
          strncmp(events[i].text, prefix, strlen(prefix)) == 0) {
        return static_cast<int>(events[i].category);
      }
    }
    return -1;
  };
  CHECK(categoryOf("ble teardown") == static_cast<int>(DebugCategory::BLE));
  CHECK(categoryOf("ready: acaia") == static_cast<int>(DebugCategory::SCALE));
  CHECK(categoryOf("Preferred scale selected") ==
        static_cast<int>(DebugCategory::SCALE));
}

void r41_negative_weight_in_range_starts_automatic_cycle() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.requireCupToStart = false;
  runtimeConfig.requireCupToStart = false;
  currentWeight = -236.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(session.startedWithScale);
  CHECK(session.awaitingPostTareBaseline);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
}

void enableCupStartGuardForTest() {
  runtimeConfig.cupProtectionEnabled = true;
  runtimeConfig.requireCupToStart = true;
  runtimeConfig.cupProtectionEnabled = true;
  runtimeConfig.requireCupToStart = true;
}

void attemptBlockedCupStart() {
  CHECK(stopperState == StopperState::READY);
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(cupStartGuardHold);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!cupStartGuardHold);
  CHECK(debugEventExists(DebugCode::CUP_START_GUARD_BLOCKED));
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
}

void cp01_zero_pre_tare_weight_blocks_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  attemptBlockedCupStart();
}

void cp02_negative_pre_tare_weight_blocks_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  currentWeight = -50.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  attemptBlockedCupStart();
}

void cp03_positive_pre_tare_weight_starts_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  seedCupPresence(80.0f);
  startCycle();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
}

void cp04_require_cup_off_allows_zero_start() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(stopperState == StopperState::BREW);
}

void cp05_rinse_without_cup_still_starts() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  const uint32_t rawOnAt = hostMillis;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(cupStartGuardHold);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps);
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!cupStartGuardHold);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
}

void cp06_cup_removed_stops_during_bbw_protection() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(!session.bbwProtectionEnded);
  publishWeight(-80.0f, hostMillis, 1, 40);
  publishWeight(-81.0f, hostMillis + 1, 1, 41);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(debugEventExists(DebugCode::CUP_REMOVED_CONFIRMED));
  CHECK(shotLogStopDetailFromEndReason(EndReason::CUP_REMOVED, true, false) ==
        ShotLogStopDetail::CUP_REMOVED);
  CHECK(shotLogCutFromEndReason(EndReason::CUP_REMOVED) == ShotLogCut::AUTO);
}

void cp07_zero_after_tare_does_not_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(0.0f, hostMillis, 1, 50);
  publishWeight(0.1f, hostMillis + 1, 1, 51);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp08_stop_if_cup_removed_off_keeps_negative_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.stopIfCupRemoved = false;
  runtimeConfig.stopIfCupRemoved = false;
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-80.0f, hostMillis, 1, 40);
  publishWeight(-81.0f, hostMillis + 1, 1, 41);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp09_blocked_cup_start_is_silent_when_alerts_off() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  runtimeConfig.soundAlertsMuted = true;
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  CHECK(debugEventExists(DebugCode::CUP_START_GUARD_BLOCKED));
  CHECK(localBuzzer.acceptedRequests == beforeBeeps);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
}

void cp10_web_paddle_blocks_cup_start_with_double_beep() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  processWebCommand(webControlCommand(WebCommandType::REMOTE_ON));
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(debugEventExists(DebugCode::CUP_START_GUARD_BLOCKED));
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
}

void cp11_master_off_allows_zero_pre_tare_start() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  runtimeConfig.cupProtectionEnabled = false;
  runtimeConfig.cupProtectionEnabled = false;
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
}

void cp12_master_off_does_not_stop_on_negative_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.cupProtectionEnabled = false;
  runtimeConfig.cupProtectionEnabled = false;
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-80.0f, hostMillis, 1, 40);
  publishWeight(-81.0f, hostMillis + 1, 1, 41);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp13_pre_tare_negative_packets_do_not_abort() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.requireCupToStart = false;
  runtimeConfig.requireCupToStart = false;
  currentWeight = -236.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(session.awaitingPostTareBaseline);
  publishWeight(-236.0f, hostMillis, 1, 2);
  publishWeight(-236.0f, hostMillis + 1, 1, 3);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
  CHECK(session.awaitingPostTareBaseline);
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  publishWeight(-0.2f, hostMillis, 1, 10);
  publishWeight(-0.2f, hostMillis + 1, 1, 11);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
  publishWeight(-80.0f, hostMillis, 1, 40);
  publishWeight(-81.0f, hostMillis + 1, 1, 41);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp14_post_tare_noise_does_not_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-0.2f, hostMillis, 1, 50);
  publishWeight(-0.2f, hostMillis + 1, 1, 51);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp15_timer_only_ignores_negative_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.timerOnly = true;
  seedCupPresence(80.0f);
  startCycle();
  CHECK(session.config.timerOnly);
  publishWeight(-80.0f, hostMillis, 1, 40);
  publishWeight(-81.0f, hostMillis + 1, 1, 41);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp16_small_negative_noise_does_not_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-2.9f, hostMillis, 1, 60);
  publishWeight(-2.9f, hostMillis + 1, 1, 61);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
}

void cp17_weight_at_removed_threshold_stops() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-3.0f, hostMillis, 1, 70);
  publishWeight(-3.1f, hostMillis + 1, 1, 71);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void cp18_custom_removed_threshold_is_honored() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.cupRemovedWeightG = -10.0f;
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-5.0f, hostMillis, 1, 80);
  publishWeight(-5.0f, hostMillis + 1, 1, 81);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
  publishWeight(-10.0f, hostMillis + 2, 1, 82);
  publishWeight(-10.1f, hostMillis + 3, 1, 83);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void cp19_weight_below_present_threshold_blocks_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  currentWeight = 5.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  publishStableCupWeight(5.0f, 1);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  attemptBlockedCupStart();
}

void cp20_weight_at_present_threshold_starts_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  seedCupPresence(10.0f);
  startCycle();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
}

void cp22_zeroed_cup_lift_stops_without_placement() {
  // The cup was tared before the scale connected, so the controller never
  // saw it placed; after the shot-start zero, lifting it is still a removal.
  for (bool protect : {true, false}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.cupProtectionEnabled = protect;
    idleCup(0.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    startCycle();
    advanceToBrew();
    publishWeight(-250.0f, hostMillis + 100, 1, 70);
    publishWeight(-250.0f, hostMillis + 100, 1, 71);
    runLoopAfter(1);
    CHECK(session.endReason == (protect ? EndReason::CUP_REMOVED : EndReason::NONE));
  }
}

void cp23_empty_pan_noise_never_removes() {
  resetHarness(false, true);
  reachReadyFromBoot();
  idleCup(0.0f);
  startCycle();
  advanceToBrew();
  for (uint32_t i = 0; i < 20; ++i) {
    publishWeight((i & 1) ? -1.0f : -0.4f, hostMillis + 100, 1, 70 + i);
    runLoopAfter(1);
  }
  CHECK(session.active);
  CHECK(session.endReason == EndReason::NONE);
}

void startShotWithKnownCupMass(float cupG) {
  prepareIdleTare();
  idleCup(cupG);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(captureCupTareDiagnostics().weightValid);
  CHECK(captureCupTareDiagnostics().weightG == cupG);
  startCycle();
  while (executeNextScaleCommand()) {}
  for (int i = 0; i < 5; ++i) idleWeight(0.0f, 100);  // Continuous stream.
  CHECK(stopperState == StopperState::BREW && !session.awaitingPostTareBaseline);
  CHECK(captureCupTareDiagnostics().weightValid);
}

void cp24_cup_tilt_does_not_abort() {
  // A 250 g cup tilted by a hand reads -8 g for 300 ms: shallower than half
  // its mass, so it must persist 400 ms before it counts as a lift.
  startShotWithKnownCupMass(250.0f);
  for (int i = 0; i < 3; ++i) idleWeight(-8.0f, 100);
  idleWeight(0.0f, 100);
  runLoopAfter(1);
  CHECK(session.active);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  for (int i = 0; i < 5; ++i) idleWeight(-8.0f, 100);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void cp25_full_lift_stops_fast() {
  startShotWithKnownCupMass(250.0f);
  idleWeight(-240.0f, 100);
  idleWeight(-240.0f, 100);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void cp26_unknown_mass_keeps_legacy_depth() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  CHECK(!captureCupTareDiagnostics().weightValid);
  startCycle();
  advanceToBrew();
  publishWeight(-8.0f, hostMillis + 100, 1, 70);
  publishWeight(-8.0f, hostMillis + 100, 1, 71);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void cp27_slow_creep_below_removed_is_not_a_lift() {
  prepareIdleTare();
  idleCup(250.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresenceIsTared());
  for (int i = 1; i <= 400; ++i) idleWeight(-0.01f * i, 100);  // -0.1 g/s, 40 s.
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  idleWeight(-250.0f, 100);
  idleWeight(-250.0f, 100);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void cp21_custom_present_threshold_is_honored() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableCupStartGuardForTest();
  runtimeConfig.minimumCupWeightG = 8.0f;
  currentWeight = 7.8f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  publishStableCupWeight(7.8f, 1);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  attemptBlockedCupStart();
  publishStableCupWeight(0.0f, 10);
  seedCupPresence(8.0f);
  startCycle();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
}

void cup_fsm_boot_zero_is_absent() {
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  publishWeight(0.0f, hostMillis, 1, 1);
  publishWeight(-0.2f, hostMillis + 50, 1, 2);
  publishWeight(2.0f, hostMillis + 100, 1, 3);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void cup_fsm_stable_min_cup_is_present() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
}

void cup_fsm_noise_does_not_remove() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  notifyCupPresenceTare();
  holdCupPresenceTransitions(false);
  publishWeight(0.0f, hostMillis, 1, 20);
  publishWeight(-0.2f, hostMillis + 50, 1, 21);
  publishWeight(-0.2f, hostMillis + 51, 1, 22);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
}

void cup_fsm_spike_does_not_place() {
  resetHarness(false, true);
  reachReadyFromBoot();
  publishWeight(28.0f, hostMillis + 100, 1, 10);
  publishWeight(6.0f, hostMillis + 200, 1, 11);
  publishWeight(5.0f, hostMillis + 300, 1, 12);
  publishWeight(5.0f, hostMillis + 400, 1, 13);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void cup_fsm_untared_lift_to_zero_is_removed() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  publishWeight(0.0f, hostMillis, 1, 20);
  publishWeight(0.0f, hostMillis + 50, 1, 21);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void cup_fsm_tare_does_not_change_state() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  notifyCupPresenceTare();
  holdCupPresenceTransitions(false);
  publishStableCupWeight(0.0f, 30);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  publishWeight(0.0f, hostMillis, 1, 35);
  publishWeight(-0.2f, hostMillis + 50, 1, 36);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  publishWeight(-80.0f, hostMillis, 1, 40);
  publishWeight(-81.0f, hostMillis + 1, 1, 41);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  notifyCupPresenceTare();
  publishStableCupWeight(0.0f, 50);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void cup_fsm_tared_zero_survives_guarded_start() {
  for (bool startTare : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    enableCupStartGuardForTest();
    runtimeConfig.autoTare = startTare;
    seedCupPresence(80.0f);
    notifyCupPresenceTare();
    publishStableCupWeight(0.0f, 30);
    startCycle();
    CHECK(session.active);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(!cupStartGuardHold);
  }
}

void idleWeight(float weight, uint32_t dtMs) {
  hostMillis += dtMs;
  markScaleWorkerProgress();
  publishWeight(weight);
}

void idleCup(float weight) {
  for (uint8_t i = 0; i < runtimeConfig.retareStabilitySamples; ++i) {
    idleWeight(weight);
  }
}

void prepareIdleTare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(0.0f);
}

bool profileHasState(ScaleProfileSignal signal, uint32_t value) {
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    if (row.kind == static_cast<uint16_t>(ScaleProfileEvent::STATE_CHANGED) &&
        (row.flags & ~SCALE_PROFILE_INITIAL) == static_cast<uint16_t>(signal) &&
        row.arg2 == value) return true;
  }
  return false;
}

void sp01_profile_explains_idle_cup_tare_and_removal() {
  prepareIdleTare();
  CHECK(scaleProfiler().hostStartNoFlash(millis()));
  idleCup(80.0f);
  CHECK(profileHasState(ScaleProfileSignal::IDLE_READY_FOR_CUP, 1));
  CHECK(profileHasState(ScaleProfileSignal::IDLE_STATUS, IDLE_READY_FOR_CUP));
  CHECK(profileHasState(ScaleProfileSignal::CUP_SETTLING, 1));
  CHECK(profileHasState(ScaleProfileSignal::CUP_PRESENT, 1));
  const auto request = idleTare.requestId;
  CHECK(request != 0 && profileHasState(ScaleProfileSignal::IDLE_REQUEST_ID, request));
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(profileHasState(ScaleProfileSignal::CUP_TARED, 1));
  CHECK(profileHasState(ScaleProfileSignal::IDLE_STATUS, IDLE_TARED));
  CHECK(profileHasState(ScaleProfileSignal::IDLE_LAST_REASON,
                        static_cast<uint32_t>(IdleTareReason::EFFECT_CONFIRMED)));
  unsigned phases = 0;
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    if (row.kind == static_cast<uint16_t>(ScaleProfileEvent::TARE_PHASE) && row.arg1 == request)
      phases |= 1U << (row.arg2 & 0xff);
  }
  CHECK((phases & 14U) == 14U); // queued, writing and success survive one worker drain.
  idleWeight(-80.0f);
  idleWeight(-80.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(profileHasState(ScaleProfileSignal::CUP_PRESENT, 0));
  CHECK(scale.tareCalls == 1 && !session.active && !getRelaySafetySnapshot().closed);
  scaleProfiler().hostStop(ScaleProfilerStopReason::USER, millis());
}

void sp02_profile_captures_timer_only_timeout_and_mid_request_start() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  const auto request = idleTare.requestId;
  CHECK(scaleProfiler().hostStartNoFlash(millis()));
  serviceIdleTare();
  CHECK(profileHasState(ScaleProfileSignal::IDLE_REQUEST_ID, request));
  CHECK(profileHasState(ScaleProfileSignal::IDLE_COMMAND_PHASE,
                        static_cast<uint32_t>(IdleTarePhase::SUCCEEDED)));
  hostMillis += SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs + 1;
  serviceIdleTare();
  CHECK(idleTare.requestId == 0);
  CHECK(profileHasState(ScaleProfileSignal::IDLE_LAST_REASON,
                        static_cast<uint32_t>(IdleTareReason::EFFECT_UNCONFIRMED)));
  bool timeout = false;
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    if (row.kind == static_cast<uint16_t>(ScaleProfileEvent::TARE_BASELINE_TIMEOUT))
      timeout = row.arg1 == request;
  }
  CHECK(timeout && scale.tareCalls == 1 && !getRelaySafetySnapshot().closed);
  scaleProfiler().hostStop(ScaleProfilerStopReason::USER, millis());
}

void sp03_profile_explains_settling_failure_and_config_changes() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  CHECK(scaleProfiler().hostStartNoFlash(millis()));
  idleCup(0.0f);
  CHECK(profileHasState(ScaleProfileSignal::IDLE_STATUS, IDLE_WAITING_FOR_SETTLE));
  CHECK(profileHasState(ScaleProfileSignal::EMPTY_SETTLING, 1));
  idleWeight(80.0f);
  idleWeight(100.0f); // A moving cup restarts the placement qualification.
  idleCup(100.0f);
  scale.tareSucceeds = false;
  CHECK(executeNextScaleCommand());
  serviceIdleTare();
  CHECK(profileHasState(ScaleProfileSignal::IDLE_LAST_REASON,
                        static_cast<uint32_t>(IdleTareReason::WRITE_FAILED)));
  bool unstable = false, failed = false;
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    unstable |= row.kind == static_cast<uint16_t>(ScaleProfileEvent::CUP_QUALIFICATION_RESET) &&
        row.arg2 == static_cast<uint32_t>(IdleTareReason::UNSTABLE);
    failed |= row.kind == static_cast<uint16_t>(ScaleProfileEvent::TARE_PHASE) &&
        (row.arg2 & 0xff) == static_cast<uint32_t>(IdleTarePhase::FAILED);
  }
  CHECK(unstable && failed);
  runtimeConfig.autoTareOutsideBrew = false;
  ++runtimeConfig.revision;
  serviceIdleTare();
  CHECK(profileHasState(ScaleProfileSignal::AUTO_IDLE_TARE, 0));
  CHECK(profileHasState(ScaleProfileSignal::IDLE_STATUS,
                        static_cast<uint32_t>(IdleTareReason::FEATURE_DISABLED)));
  CHECK(!session.active && !getRelaySafetySnapshot().closed);
  scaleProfiler().hostStop(ScaleProfilerStopReason::USER, millis());
}

void it01_placement_tares_once_independently() {
  for (unsigned flags = 0; flags < 8; ++flags) {
    prepareIdleTare();
    runtimeConfig.autoTareOutsideBrew = (flags & 1) != 0;
    runtimeConfig.autoTare = (flags & 2) != 0;
    runtimeConfig.autoRetare = (flags & 4) != 0;
    idleCup(80.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (flags & 1));
    if (!(flags & 1)) continue;
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(idleTare.requestId == 0);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(scale.tareCalls == 1);
    CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
    idleCup(40.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    idleWeight(-80.0f);
    idleWeight(-80.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    idleCup(-80.0f); // Qualify stable absence before replacing the cup.
    idleCup(0.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void cw01_idle_mass_survives_coffee_and_snapshot_copy() {
  prepareIdleTare();
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(captureCupTareDiagnostics().weightValid);
  CHECK(executeNextScaleCommand());
  CHECK(captureCupTareDiagnostics().weightValid);
  idleCup(0.0f);
  CHECK(captureCupTareDiagnostics().weightValid);
  CHECK(captureCupTareDiagnostics().weightG == 80.0f);
  idleCup(35.0f);
  publishControlStatus();
  ControlStatusSnapshot snapshot;
  copyControlStatus(snapshot);
  CHECK(snapshot.cupTare.weightValid);
  CHECK(snapshot.cupTare.weightG == 80.0f);
  resetCupPresence();
  DebugExportExtras debug;
  copyDebugExportExtras(debug, snapshot);
  CHECK(debug.cupTare.weightValid);
  CHECK(debug.cupTare.weightG == 80.0f);
  CHECK(debug.cupTare.placementId == snapshot.cupTare.placementId);
  CHECK(!captureCupTareDiagnostics().weightValid);
}

void cw02_start_outcome_and_later_tare_preserve_mass() {
  for (bool combined : {false, true}) {
    for (bool success : {false, true}) {
      resetHarness(false, true);
      reachReadyFromBoot();
      runtimeConfig.canTareStartTimer = combined;
      idleCup(0.0f);
      idleCup(80.0f);
      scale.tareSucceeds = success;
      scale.tareStartTimerSucceeds = success;
      startCycle();
      CHECK(executeNextScaleCommand());
      CHECK(captureCupTareDiagnostics().weightValid == success);
      if (!success) continue;
      CHECK(captureCupTareDiagnostics().weightG == 80.0f);
      idleWeight(0.0f);
      idleWeight(95.0f);
      CHECK(requestRemoteRetare());
      CHECK(executeNextScaleCommand());
      CHECK(captureCupTareDiagnostics().weightValid);
      CHECK(captureCupTareDiagnostics().weightG == 80.0f);
      CHECK(cupPresence.emptyAnchorG == -175.0f);
    }
  }
}

void cw03_removal_replacement_and_connection_invalidate() {
  for (float replacement : {350.0f, 300.0f, 250.0f}) {
    prepareIdleTare();
    idleCup(0.0f);
    idleCup(300.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 300.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(captureCupTareDiagnostics().weightG == 300.0f);
    idleWeight(-300.0f);
    idleWeight(-300.0f);
    CHECK(!captureCupTareDiagnostics().weightValid);
    idleCup(-300.0f); // Stable empty-pan reference, with the previous tare offset.
    idleCup(replacement - 300.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == replacement);
    CHECK(cupPresence.weight.absent.valid);
    CHECK(cupPresence.weight.absent.absoluteG == -300.0f);
    CHECK(cupPresence.weight.present.valid);
    CHECK(cupPresence.weight.present.absoluteG == replacement - 300.0f);
    CHECK(cupPresence.weight.present.atMs > cupPresence.weight.absent.atMs);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == replacement);
    setScaleConnected(false);
    setScaleConnected(true);
    idleCup(80.0f); // No empty baseline on this connection.
    CHECK(!captureCupTareDiagnostics().weightValid);
  }
}

void cw04_invalid_stale_and_discontinuous_evidence_cannot_reappear() {
  for (unsigned mode = 0; mode < 6; ++mode) {
    prepareIdleTare();
    idleCup(0.0f);
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    if (mode == 0) idleWeight(std::numeric_limits<float>::quiet_NaN());
    if (mode == 1) idleWeight(MAX_PARSED_WEIGHT_G + 1.0f);
    if (mode == 2) hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 1;
    if (mode == 3) resetCupSampleEvidence();
    if (mode == 4) markCupTareReferenceUncertain();
    if (mode == 5) ++scaleEventsDropped;
    CHECK(!captureCupTareDiagnostics().weightValid);
    idleWeight(0.0f);
    CHECK(!captureCupTareDiagnostics().weightValid);
  }
}

void cw05_pending_identity_failure_and_timeout() {
  prepareIdleTare();
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  trackCupWeightTare(100);
  finishCupWeight(100, false, false); // Proven no physical effect.
  CHECK(captureCupTareDiagnostics().weightValid);
  trackCupWeightTare(101);
  finishCupWeight(100, true, false); // Old command cannot settle this request.
  CHECK(cupPresence.weight.pendingId == 101);
  finishCupWeight(101, false, true);
  CHECK(!captureCupTareDiagnostics().weightValid);
  trackCupWeightTare(102);
  idleWeight(-80.0f);
  idleWeight(-80.0f);
  idleCup(20.0f);
  finishCupWeight(102, true, false);
  CHECK(!captureCupTareDiagnostics().weightValid);
  hostMillis += SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs + 1;
  CHECK(!captureCupTareDiagnostics().weightValid);
}

void cw06_no_capture_from_missing_or_invalid_pre_tare_sample() {
  for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                        MAX_AUTOMATION_WEIGHT_G + 1.0f}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    idleCup(80.0f);
    idleWeight(invalid);
    session.config.autoTare = true;
    CHECK(requestRemoteRetare());
    CHECK(executeNextScaleCommand());
    CHECK(!captureCupTareDiagnostics().weightValid);
  }
}

void cw07_late_retare_and_disabled_automation() {
  for (bool flow : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = true;
    runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
    idleWeight(0.0f);
    startCycle();
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    idleCup(0.0f);
    session.flowDuringRetare = flow;
    idleCup(80.0f);
    CHECK(session.retarePerformed);
    CHECK(executeNextScaleCommand());
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 80.0f);
  }
  for (bool bbw : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.timerOnly = !bbw;
    runtimeConfig.autoTare = false;
    idleCup(0.0f);
    idleCup(80.0f);
    startCycle();
    CHECK(executeNextScaleCommand());
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 80.0f);
    CHECK(scale.tareCalls == 0);
    CHECK(scale.tareStartTimerCalls == 0);
  }
}

void cw08_pending_timeout_and_request_wrap() {
  prepareIdleTare();
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  nextCupWeightRequestId = UINT32_MAX;
  session.config.autoTare = true;
  CHECK(requestRemoteRetare());
  CHECK(cupPresence.weight.pendingId == 1);
  const uint32_t end = hostMillis + SCALE_ATT_TIMEOUT_MS +
                       runtimeConfig.postTareBaselineGraceMs + 150;
  while (hostMillis < end) idleWeight(0.0f);
  CHECK(!captureCupTareDiagnostics().weightValid);
  CHECK(cupPresence.weight.pendingId == 0);
  CHECK(executeNextScaleCommand()); // Late success cannot resurrect timed-out mass.
  CHECK(!captureCupTareDiagnostics().weightValid);
}

void cw09_debug_tare_invalidates_without_changing_presence() {
  for (BookooDebugAction action : {BookooDebugAction::TARE, BookooDebugAction::COMBINED}) {
    prepareIdleTare();
    idleCup(0.0f);
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    executeScaleDebugCommand(action, 0);
    processScaleWorkerEvents();
    CHECK(!captureCupTareDiagnostics().weightValid);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(!session.active);
    CHECK(!getRelaySafetySnapshot().closed);
  }
}

void cw17_debug_tare_rebases_negative_replacement() {
  for (BookooDebugAction action : {BookooDebugAction::TARE, BookooDebugAction::COMBINED}) {
    prepareIdleTare();
    idleCup(0.0f);
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    runtimeConfig.autoTareOutsideBrew = false;
    idleCup(-80.0f);
    idleCup(-80.0f);
    idleCup(-60.0f);
    CHECK(captureCupTareDiagnostics().weightG == 20.0f);
    executeScaleDebugCommand(action, 0);
    processScaleWorkerEvents();
    idleCup(0.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(!captureCupTareDiagnostics().weightValid);
    idleCup(-20.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    enableCupStartGuardForTest();
    beginCycle();
    CHECK(!session.active);
    CHECK(!getRelaySafetySnapshot().closed);
    idleCup(-20.0f);
    idleCup(0.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 20.0f);
  }
}

void cw18_debug_tare_cancels_queued_false_placement() {
  prepareIdleTare();
  idleCup(-300.0f);
  executeScaleDebugCommand(BookooDebugAction::TARE, 0);
  for (unsigned i = 0; i < 3; ++i) {
    hostMillis += 150;
    ScaleEvent event;
    event.receivedAtMs = hostMillis;
    event.weightG = 0.0f;
    CHECK(publishScaleEvent(event, false));
  }
  processScaleWorkerEvents();
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(idleTare.requestId == 0);
  while (executeNextScaleCommand()) {}
  CHECK(scale.tareCalls == 1);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(captureCupTareDiagnostics().weightG == 80.0f);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 2);
}

void cw19_stale_active_samples_cannot_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  const uint32_t capturedAt = hostMillis + 100;
  hostMillis = capturedAt + 1600;
  markScaleWorkerProgress();
  for (unsigned i = 0; i < 3; ++i) publishWeight(80.0f, capturedAt + i * 150);
  CHECK(!session.retarePerformed);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(session.retarePerformed);
}

void cw20_rejected_cup_samples_break_stability() {
  for (bool active : {false, true}) {
    for (unsigned invalid = 0; invalid < 3; ++invalid) {
      resetHarness(false, true);
      reachReadyFromBoot();
      if (active) {
        startCycle();
        CHECK(executeNextScaleCommand());
        idleCup(0.0f);
      }
      idleCup(0.0f);
      idleWeight(80.0f);
      CHECK(cupPresence.placeStabilitySamples == 1);
      ScaleEvent event;
      event.weightG = 80.0f;
      event.receivedAtMs = invalid == 0 ? hostMillis - MAX_AUTOMATION_WEIGHT_AGE_MS - 1
                          : invalid == 1 ? hostMillis + 1 : hostMillis;
      event.packetSequence = cupPresence.weight.sampleSequence + (invalid == 2 ? 0 : 1);
      CHECK(publishScaleEvent(event, false));
      processScaleWorkerEvents();
      CHECK(cupPresence.placeStabilitySamples == 0);
      idleWeight(80.0f);
      idleWeight(80.0f);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
      idleWeight(80.0f);
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    }
  }
}

void cw21_debug_failure_and_old_connection_cannot_certify_reference() {
  prepareIdleTare();
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  ScaleEvent event;
  event.type = ScaleEventType::REFERENCE_CHANGED;
  event.connectionGeneration = getScaleLinkSnapshot().connectionGeneration + 1;
  CHECK(publishScaleEvent(event, true));
  processScaleWorkerEvents();
  CHECK(captureCupTareDiagnostics().weightValid);
  // A local/ATT rejection can complete without disconnecting the scale.
  event.connectionGeneration = getScaleLinkSnapshot().connectionGeneration;
  CHECK(publishScaleEvent(event, true));
  processScaleWorkerEvents();
  CHECK(!cupPresenceIsKnown());
  CHECK(!captureCupTareDiagnostics().weightValid);
  enableCupStartGuardForTest();
  beginCycle();
  CHECK(!session.active);
}

void cw10_absence_baseline_requires_stability_and_continuity() {
  for (unsigned mode = 0; mode < 5; ++mode) {
    resetHarness(false, true);
    reachReadyFromBoot();
    if (mode == 0) idleWeight(0.0f); // One sample is not a stable baseline.
    if (mode == 1) {
      idleWeight(0.0f);
      idleWeight(2.0f);
      idleWeight(4.0f); // Accumulated drift exceeds the whole-window tolerance.
    }
    if (mode == 2) {
      idleCup(0.0f);
      idleWeight(300.0f, runtimeConfig.retareStabilityMaxGapMs + 1);
    }
    if (mode == 3) {
      idleCup(0.0f);
      resetCupSampleEvidence();
    }
    // Mode 4 boots with a cup already loaded; zero must not be invented.
    idleCup(300.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(!captureCupTareDiagnostics().weightValid);
  }
}

void cw11_removal_transient_is_not_the_empty_baseline() {
  prepareIdleTare();
  idleCup(0.0f);
  idleCup(300.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleWeight(-301.0f);
  idleWeight(-305.0f); // Removal overshoot; the detector remembers this minimum.
  idleCup(-300.0f); // Actual stable absent state, five grams above that minimum.
  idleCup(50.0f);
  CHECK(captureCupTareDiagnostics().weightValid);
  CHECK(cupPresence.weight.absent.absoluteG == -300.0f);
  CHECK(cupPresence.weight.present.absoluteG == 50.0f);
  CHECK(captureCupTareDiagnostics().weightG == 350.0f);
}

void cw12_placement_ramp_preserves_stable_absence() {
  for (bool tare : {false, true}) {
    prepareIdleTare();
    runtimeConfig.autoTareOutsideBrew = tare;
    idleCup(0.0f);
    idleWeight(5.0f);
    idleWeight(8.0f);
    idleWeight(50.0f);
    idleCup(300.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 300.0f);
    CHECK(cupPresence.weight.absent.absoluteG == 0.0f);
    if (tare) {
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      CHECK(scale.tareCalls == 1);
      CHECK(captureCupTareDiagnostics().weightValid);
      CHECK(captureCupTareDiagnostics().weightG == 300.0f);
    }
  }
}

void cw13_negative_boot_baseline_requires_known_empty_reference() {
  for (float mass : {300.0f, 522.0f}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoTareOutsideBrew = true;
    idleCup(-350.0f);
    idleCup(mass - 350.0f);
    CHECK(!captureCupTareDiagnostics().weightValid);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    // Positive absolute boot loads may establish presence, never relative mass.
    CHECK(cupPresenceState() == (mass > 350.0f ? CupPresenceState::PRESENT
                                             : CupPresenceState::ABSENT));
  }
}

void cw22_empty_movement_cannot_place_or_retare() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    for (float excursion : {-20.0f, -100.0f, -522.0f}) {
      prepareIdleTare();
      runtimeConfig.autoTareOutsideBrew = mode != 0;
      if (mode == 2) {
        runtimeConfig.autoRetare = true;
        runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
        startCycle();
        CHECK(executeNextScaleCommand());
        establishPostTareBaseline();
      }
      idleCup(0.0f);
      const size_t tares = scale.tareCalls;
      idleCup(-0.2f);
      idleCup(0.1f);
      idleCup(excursion);
      idleCup(0.1f);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
      CHECK(cupPresencePlacementId() == 0);
      CHECK(!captureCupTareDiagnostics().weightValid);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      CHECK(scale.tareCalls == tares);
      CHECK(!session.retarePerformed);
      idleCup(80.0f);
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
      CHECK(captureCupTareDiagnostics().weightValid);
      // Idle recovery retains the observed empty offset; in-shot zero is unchanged.
      const float expectedMass = mode == 2 ? 80.0f : 79.9f;
      CHECK(fabsf(captureCupTareDiagnostics().weightG - expectedMass) < 0.01f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (mode != 0 ? 1U : 0U));
    }
  }
}

void cw23_invalid_numeric_samples_break_confirmations() {
  for (float invalid : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        MAX_PARSED_WEIGHT_G + 1.0f}) {
    for (bool direct : {false, true}) {
      prepareIdleTare();
      idleCup(0.0f);
      idleWeight(80.0f);
      if (direct) recordWeightSampleWithProvenance(invalid, hostMillis, 0, 0);
      else idleWeight(invalid);
      CHECK(cupPresence.placeStabilitySamples == 0);
      idleWeight(80.0f);
      idleWeight(80.0f);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
      idleWeight(80.0f);
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
      idleWeight(0.0f);
      CHECK(cupPresence.removedConfirmations == 1);
      if (direct) recordWeightSampleWithProvenance(invalid, hostMillis, 0, 0);
      else idleWeight(invalid);
      CHECK(cupPresence.removedConfirmations == 0);
      idleWeight(0.0f);
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
      idleWeight(0.0f);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    }
  }
}

void cw24_minimum_mass_is_independent_of_stability_tolerance() {
  for (float tolerance : {0.1f, 10.0f, 20.0f}) {
    for (float load : {9.9f, 10.0f, 10.1f}) {
      prepareIdleTare();
      runtimeConfig.retareStabilityToleranceG = tolerance;
      idleCup(0.0f);
      idleCup(load);
      CHECK(cupPresenceState() == (load >= 10.0f ? CupPresenceState::PRESENT
                                                : CupPresenceState::ABSENT));
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (load >= 10.0f ? 1U : 0U));
    }
  }
}

void cw25_lost_samples_cannot_learn_a_negative_empty_reference() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    prepareIdleTare();
    idleCup(0.0f);
    if (mode == 0) idleWeight(-20.0f, runtimeConfig.retareStabilityMaxGapMs + 1);
    if (mode == 1) resetCupSampleEvidence();
    if (mode == 2) ++runtimeConfig.revision;
    if (mode == 3) {
      setScaleConnected(false);
      setScaleConnected(true);
    }
    idleCup(-20.0f);
    idleCup(0.1f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  }
}

void cw26_stable_removal_undershoot_becomes_empty_reference() {
  // After a lift the tare-translated anchor is provisional: a stable empty
  // plateau below it (something else was removed too) is the new reference,
  // so the next load is measured from it.
  prepareIdleTare();
  idleCup(300.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresence.emptyAnchorG == -300.0f);
  idleCup(-400.0f);
  idleCup(-400.0f);
  CHECK(cupPresence.weight.emptyValid);
  CHECK(cupPresence.emptyAnchorG == -400.0f);
  idleCup(-300.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(captureCupTareDiagnostics().weightG == 100.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void cw14_heavy_tare_offsets_preserve_replacement_mass() {
  for (float replacement : {300.0f, 522.0f}) {
    prepareIdleTare();
    idleCup(0.0f);
    idleCup(522.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    const uint32_t acceptedSequence = currentWeightSequence;
    idleWeight(-522.0f);
    idleWeight(-522.0f);
    idleCup(-522.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(cupPresence.weight.emptyValid);
    CHECK(cupPresence.weight.absent.absoluteG == -522.0f);
    CHECK(currentWeightSequence == acceptedSequence); // Not accepted for brew.
    CHECK(observedWeight == -522.0f); // Still available as raw evidence.
    idleCup(replacement - 522.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == replacement);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(scale.tareCalls == 2);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == replacement);
  }
}

void cw15_removal_rebound_cannot_place_or_tare_empty_pan() {
  for (float undershoot : {15.0f, 100.0f}) {
    prepareIdleTare();
    idleCup(0.0f);
    idleCup(300.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    idleWeight(-300.0f - undershoot);
    idleWeight(-300.0f - undershoot);
    idleCup(-300.0f);
    idleCup(-300.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(!captureCupTareDiagnostics().weightValid);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    CHECK(cupPresence.weight.absent.absoluteG == -300.0f);
    idleCup(0.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 300.0f);
    CHECK(executeNextScaleCommand());
    CHECK(scale.tareCalls == 2);
  }
}

void cw16_fast_replacement_reuses_known_empty_anchor() {
  for (bool enabled : {false, true}) {
    for (float replacement : {250.0f, 300.0f, 350.0f}) {
      prepareIdleTare();
      idleCup(300.0f);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      runtimeConfig.autoTareOutsideBrew = enabled;
      idleWeight(-300.0f, 50);
      idleWeight(-300.0f, 50);
      CHECK(!cupPresence.weight.emptyValid); // No new stable-empty plateau.
      idleCup(replacement - 300.0f);
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
      CHECK(captureCupTareDiagnostics().weightValid);
      CHECK(captureCupTareDiagnostics().weightG == replacement);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (enabled ? 1U : 0U));
      if (enabled) {
        CHECK(executeNextScaleCommand());
        idleCup(0.0f);
        CHECK(scale.tareCalls == 2);
      }
      idleCup(enabled ? 0.0f : replacement - 300.0f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    }
  }
}

void cw27_queued_tare_drift_does_not_accumulate() {
  prepareIdleTare();
  idleCup(300.0f);
  idleWeight(302.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresence.emptyAnchorG == -302.0f);
  idleWeight(-302.0f);
  idleWeight(-302.0f);
  idleCup(-302.0f);
  idleCup(48.0f); // Actual 350 g replacement under the 302 g tare offset.
  CHECK(captureCupTareDiagnostics().weightG == 350.0f);
  idleWeight(50.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresence.emptyAnchorG == -352.0f);
  idleWeight(-352.0f);
  idleWeight(-352.0f);
  idleCup(-352.0f);
  CHECK(cupPresence.weight.emptyValid);
  idleCup(48.0f);
  CHECK(captureCupTareDiagnostics().weightG == 400.0f);
  CHECK(idleTare.requestId != 0);
}

void cw28_observed_unload_preserves_previous_shot() {
  for (bool enabled : {false, true}) {
    for (int liftSamples : {0, 1, 2}) {
      prepareIdleTare();
      idleCup(300.0f);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      runtimeConfig.autoTareOutsideBrew = enabled;
      startCycle();
      while (executeNextScaleCommand()) {}
      establishPostTareBaseline();
      advanceToBrew();
      runLoopAfter(1000);
      for (int i = 1; i <= 36; ++i) idleWeight(static_cast<float>(i), 300);
      CHECK(finalizeCycle(EndReason::SCALE_THRESHOLD, StopperState::REQUIRES_OFF));
      CHECK(pendingFinalize.pending);
      const uint32_t endedAt = pendingFinalize.endedAtMs;
      const uint32_t dripDelay = pendingFinalize.dripDelayMs;
      const float offset = runtimeConfig.weightOffsetG;
      for (int i = 0; i < liftSamples; ++i) idleWeight(-300.0f, 50);
      CHECK(pendingFinalize.pending == (liftSamples == 0));
      idleCup(liftSamples == 0 ? 37.0f : 50.0f);
      hostMillis = endedAt + dripDelay;
      idleWeight(liftSamples == 0 ? 37.0f : 50.0f, 0);
      pendingShotFinalizeTask();
      CHECK(persistedLastShot.weightValid);
      CHECK(persistedLastShot.currentWeightG == (liftSamples == 0 ? 37.0f : 36.0f));
      ShotLogRecord record[1] = {};
      CHECK(shotLog.copyNewestFirst(record, 1) == 1);
      CHECK(record[0].actualWeightSource == static_cast<uint8_t>(
          liftSamples == 0 ? ActualWeightSource::POST_DRIP : ActualWeightSource::LAST_KNOWN));
      if (liftSamples != 0)
        CHECK(strcmp(shotLogBbwLearningApplied(record[0]), "false") == 0);
      if (liftSamples != 0) CHECK(runtimeConfig.weightOffsetG == offset);
    }
  }
}

void cw29_all_tare_paths_capture_latest_approved_load() {
  for (unsigned commandKind = 0; commandKind < 3; ++commandKind) {
    for (bool unvalidated : {false, true}) {
      prepareIdleTare();
      idleCup(80.0f);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      if (commandKind < 2) {
        runtimeConfig.canTareStartTimer = commandKind == 1;
        startCycle();
      } else {
        session.config.autoTare = true;
        CHECK(requestRemoteRetare());
      }
      idleWeight(2.0f);
      if (unvalidated) {
        ScaleEvent event;
        event.receivedAtMs = hostMillis;
        event.weightG = 3.0f;
        CHECK(publishScaleEvent(event, false));
      }
      CHECK(executeNextScaleCommand());
      CHECK(std::isfinite(cupPresence.emptyAnchorG) == !unvalidated);
      if (!unvalidated) {
        CHECK(cupPresence.emptyAnchorG == -82.0f);
        CHECK(captureCupTareDiagnostics().weightG == 80.0f);
      } else {
        CHECK(!captureCupTareDiagnostics().weightValid);
      }
    }
  }
}

void cw30_fast_unload_requires_unbroken_evidence() {
  for (bool enabled : {false, true}) {
    for (unsigned mode = 0; mode < 8; ++mode) {
      prepareIdleTare();
      idleCup(300.0f);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      runtimeConfig.autoTareOutsideBrew = enabled;
      if (mode == 0) idleCup(5.0f); // Spoon/coffee added, no unloading.
      if (mode == 1) idleWeight(-300.0f); // One unload is not a placement.
      if (mode == 2) idleCup(-5.0f); // Partial unloading, including a stable plateau.
      if (mode >= 3) {
        idleWeight(-300.0f, 50);
        idleWeight(-300.0f, mode == 3 ? runtimeConfig.retareStabilityMaxGapMs + 1 : 50);
      }
      if (mode == 4) idleWeight(NAN);
      if (mode == 5) {
        RuntimeConfig changed = runtimeConfig;
        changed.minimumCupWeightG += 1.0f;
        ++changed.revision;
        commitLiveRuntimeConfig(changed, RUNTIME_PERSIST_REASON_USER);
      }
      if (mode == 6) ++scaleEventsDropped;
      if (mode == 7) resetCupSampleEvidence();
      idleCup(50.0f);
      CHECK(cupPresencePlacementId() == 1);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    }
  }
}

void cw31_fast_unload_band_and_minimum_mass_are_independent() {
  for (float minimum : {1.0f, 10.0f, 500.0f}) {
    for (float tolerance : {0.1f, 2.0f, 20.0f}) {
      for (float residual : {0.0f, 1.0f}) {
        prepareIdleTare();
        runtimeConfig.minimumCupWeightG = minimum;
        runtimeConfig.retareStabilityToleranceG = tolerance;
        idleCup(minimum);
        CHECK(executeNextScaleCommand());
        idleCup(0.0f);
        idleWeight(-minimum + residual, 50);
        idleWeight(-minimum + residual, 50);
        idleCup(0.0f);
        CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (residual == 0.0f ? 1U : 0U));
        if (residual == 0.0f) {
          CHECK(captureCupTareDiagnostics().weightValid);
          CHECK(captureCupTareDiagnostics().weightG == minimum);
        }
      }
    }
  }
}

void cw32_fast_unload_after_partial_removal_and_wrap() {
  for (bool wrap : {false, true}) {
    prepareIdleTare();
    idleCup(300.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    if (wrap) {
      hostMillis = UINT32_MAX - 100;
      markScaleWorkerProgress();
      idleTare.lastPacketSequence = 0;
      resetCupSampleEvidence();
      publishWeight(0.0f, hostMillis, 0, UINT32_MAX - 1);
      publishWeight(-300.0f, hostMillis + 50, 0, UINT32_MAX);
      publishWeight(-300.0f, hostMillis + 100, 0, 1);
    } else {
      idleCup(-5.0f); // Generic removal happened before near-total unloading.
      idleWeight(-300.0f, 50);
      idleWeight(-300.0f, 50);
    }
    idleCup(50.0f);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == 350.0f);
    CHECK(idleTare.requestId != 0);
  }
}

void it02_idle_zero_allows_guarded_start() {
  for (bool autoStart : {false, true}) {
    prepareIdleTare();
    runtimeConfig.autoTare = autoStart;
    enableCupStartGuardForTest();
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    startCycle();
    CHECK(session.active);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(!cupStartGuardHold);
    CHECK(!debugEventExists(DebugCode::CUP_START_GUARD_BLOCKED));
  }
}

void it03_shot_end_never_tares_remaining_cup() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  startCycle();
  while (executeNextScaleCommand()) {}
  establishPostTareBaseline();
  idleWeight(36.0f);
  CHECK(finalizeCycle(EndReason::SCALE_THRESHOLD, StopperState::REQUIRES_OFF));
  const size_t before = scale.tareCalls;
  idleCup(36.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  idleWeight(-80.0f);
  idleWeight(-80.0f);
  CHECK(!pendingFinalize.pending);
  idleCup(-80.0f);
  idleCup(36.0f); // Full cup put back, with paddle still ON.
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  while (executeNextScaleCommand()) {}
  CHECK(scale.tareCalls == before + 1);
}

void it04_reconnect_and_setting_changes_do_not_place() {
  prepareIdleTare();
  setScaleConnected(false);
  setScaleConnected(true);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  runtimeConfig.autoTareOutsideBrew = false;
  serviceIdleTare();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  ++runtimeConfig.revision;
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleWeight(0.0f);
  idleWeight(0.0f);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it05_queued_tare_is_preempted_by_same_start_or_removal() {
  prepareIdleTare();
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::BUZZER_ONLY);
  idleCup(80.0f);
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE);
  const uint32_t idleRequestId = idleTare.requestId;
  const uint32_t rawOnAtMs = startCycle();
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE_START);
  CHECK(localBuzzer.pendingCue == BuzzerCue::NONE);
  CHECK(idleTare.requestId == 0);
  CHECK(idleTare.lastReason == IdleTareReason::START_REQUEST);
  CHECK(idleScaleTareStatus().phase == IdleTarePhase::NONE);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  const ScaleCommand start = queuedCommandAt(1);
  CHECK(start.idleTareRequestId == 0);
  CHECK(start.cycleId == session.id);
  CHECK(session.startedAtMs >= rawOnAtMs);
  runLoopAfter(125);
  CHECK(elapsedMs(session.startedAtMs) >= 125);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 0);
  CHECK(idleTare.lastTerminalRequestId == idleRequestId);

  prepareIdleTare();
  idleCup(80.0f);
  idleWeight(0.0f);
  idleWeight(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 0);

  prepareIdleTare();
  idleCup(80.0f);
  beginCycle(ControlSource::WEB);
  CHECK(!session.active);
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(!beginRinseCycle(ControlSource::WEB));
  CHECK(!session.active);

  prepareIdleTare();
  idleCup(80.0f);
  const ScaleCommand racing = queuedCommandAt(0);
  std::atomic<bool> go{false};
  std::atomic<bool> claimed{false};
  std::thread worker([&]() {
    while (!go.load()) std::this_thread::yield();
    claimed.store(claimIdleScaleTare(racing.idleTareRequestId));
  });
  go.store(true);
  startCycle();
  worker.join();
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  if (claimed.load()) {
    CHECK(idleTare.deferredReason == IdleTareReason::START_REQUEST);
    finishIdleScaleTare(racing.idleTareRequestId, true);
    releaseIdleTareRequest();
  }
  CHECK(idleTare.requestId == 0);
  CHECK(idleTare.lastReason == IdleTareReason::START_REQUEST);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 0);
}

void it06_writing_and_late_idle_results_cannot_enter_brew() {
  for (unsigned outcome = 0; outcome < 3; ++outcome) {
    prepareIdleTare();
    idleCup(80.0f);
    const ScaleCommand idle = queuedCommandAt(0);
    CHECK(claimIdleScaleTare(idle.idleTareRequestId));
    if (outcome == 2) finishIdleScaleTare(idle.idleTareRequestId, true);
    const uint32_t rawOnAtMs = startCycle();
    CHECK(session.active);
    CHECK(getRelaySafetySnapshot().closed);
    CHECK(session.startedAtMs >= rawOnAtMs);
    CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
    const ScaleCommand start = queuedCommandAt(1);
    CHECK(start.idleTareRequestId == 0);
    CHECK(start.cycleId == session.id);
    if (outcome != 2) {
      CHECK(!cancelIdleScaleTare(idle.idleTareRequestId));
      finishIdleScaleTare(idle.idleTareRequestId, outcome == 0);
    }
    session.awaitingPostTareBaseline = false;
    const bool retareBefore = session.retarePerformed;
    ScaleEvent late;
    late.type = ScaleEventType::TARE_RESULT;
    late.cycleId = session.id; // Even an accidentally matching cycle is isolated.
    late.idleTareRequestId = idle.idleTareRequestId;
    late.writeSucceeded = outcome != 1;
    CHECK(publishScaleEvent(late, true));
    processScaleWorkerEvents();
    if (idleTare.requestId != 0) releaseIdleTareRequest();
    CHECK(idleTare.requestId == 0);
    CHECK(idleTare.lastReason == IdleTareReason::START_REQUEST);
    CHECK(!session.awaitingPostTareBaseline);
    CHECK(session.retarePerformed == retareBefore);
    runLoopAfter(125);
    CHECK(elapsedMs(session.startedAtMs) >= 125);
    CHECK(executeNextScaleCommand());
    CHECK(scale.tareCalls == 0);
  }
}

void it07_expiry_failure_and_removal_during_write() {
  prepareIdleTare();
  idleCup(80.0f);
  runLoopAfter(MAX_AUTOMATION_WEIGHT_AGE_MS + 1);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 0);
  prepareIdleTare();
  idleCup(80.0f);
  const uint32_t id = idleTare.requestId;
  CHECK(claimIdleScaleTare(id));
  idleWeight(0.0f);
  CHECK(cupPresenceIsTared());
  finishIdleScaleTare(id, false);
  serviceIdleTare();
  CHECK(!cupPresenceIsTared());
  idleWeight(0.0f);
  idleWeight(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  prepareIdleTare();
  idleCup(80.0f);
  const uint32_t removedId = idleTare.requestId;
  CHECK(claimIdleScaleTare(removedId));
  idleWeight(-80.0f);
  idleWeight(-80.0f);
  finishIdleScaleTare(removedId, true);
  serviceIdleTare();
  CHECK(scale.tareCalls == 0);
  CHECK(idleTare.requestId == 0);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(0.0f); // A real stable put-back can now authorize a new request.
  CHECK(idleTare.requestId != removedId);
}

void it08_stability_and_ineligible_placement() {
  prepareIdleTare();
  idleCup(5.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleWeight(80.0f);
  idleWeight(100.0f);
  idleWeight(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  // Placement starts during a shot; ending it cannot authorize its tail.
  prepareIdleTare();
  startCycle();
  while (executeNextScaleCommand()) {}
  establishPostTareBaseline();
  idleWeight(80.0f);
  CHECK(finalizeCycle(EndReason::ACTIVATOR, StopperState::READY));
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void it09_invalid_samples_queue_failure_and_disconnect() {
  prepareIdleTare();
  const auto link = getScaleLinkSnapshot();
  const uint32_t at = hostMillis;
  for (unsigned i = 0; i < 5; ++i) {
    publishWeight(80.0f, at, link.connectionGeneration, 77);
  }
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleWeight(2000.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  prepareIdleTare();
  while (scaleCommandQueue->items.size() < SCALE_COMMAND_QUEUE_LENGTH) {
    ScaleCommand filler;
    CHECK(enqueueScaleCommand(filler));
  }
  idleCup(80.0f);
  CHECK(idleTare.requestId == 0);
  CHECK(idleScaleTareStatus().phase == IdleTarePhase::NONE);
  scaleCommandQueue->items.clear();
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  prepareIdleTare();
  idleCup(80.0f);
  const auto command = queuedCommandAt(0);
  CHECK(claimIdleScaleTare(command.idleTareRequestId));
  setScaleConnected(false);
  serviceIdleTare();
  setScaleConnected(true);
  finishIdleScaleTare(command.idleTareRequestId, true);
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(!cupPresenceIsTared());
}

void it10_worker_claim_and_cancel_are_mutually_exclusive() {
  prepareIdleTare();
  for (uint32_t i = 1; i <= 100; ++i) {
    ScaleCommand command;
    command.type = ScaleCommandType::TARE_ONLY;
    command.idleTareRequestId = i;
    CHECK(enqueueScaleCommand(command));
    std::atomic<bool> go{false};
    bool claimed = false;
    std::thread worker([&] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      claimed = claimIdleScaleTare(i);
    });
    go.store(true, std::memory_order_release);
    const bool cancelled = cancelIdleScaleTare(i);
    worker.join();
    CHECK(cancelled != claimed);
    if (claimed) {
      CHECK(idleScaleTareStatus().phase == IdleTarePhase::WRITING);
      finishIdleScaleTare(i, true);
      CHECK(cancelIdleScaleTare(i));
    }
    scaleCommandQueue->items.clear();
  }
}

void it11_disabling_after_write_keeps_tared_cup() {
  prepareIdleTare();
  idleCup(80.0f);
  ScaleCommand command;
  CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
  executeScaleCommand(command); // Completion has not reached control yet.
  runtimeConfig.autoTareOutsideBrew = false;
  ++runtimeConfig.revision;
  serviceIdleTare();
  CHECK(idleTare.requestId != 0); // Transport success still awaits physical evidence.
  idleCup(0.0f);
  CHECK(idleTare.requestId == 0);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(cupPresenceIsTared());
  enableCupStartGuardForTest();
  runtimeConfig.autoTare = false;
  startCycle();
  CHECK(session.active);
}

void it12_lighter_replacement_keeps_queued_tare() {
  for (bool afterShot : {false, true}) {
    prepareIdleTare();
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    if (afterShot) {
      startCycle();
      while (executeNextScaleCommand()) {}
      establishPostTareBaseline();
      idleWeight(36.0f);
      CHECK(finalizeCycle(EndReason::SCALE_THRESHOLD,
                          StopperState::REQUIRES_OFF));
    }
    idleWeight(-80.0f);
    idleWeight(-80.0f);
    idleCup(-80.0f);
    idleCup(-60.0f);
    const uint32_t id = idleTare.requestId;
    CHECK(id != 0);
    idleWeight(-60.0f);
    idleWeight(-60.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(idleTare.requestId == id);
    while (executeNextScaleCommand()) {} // A post-shot STOP can precede tare.
    idleCup(0.0f);
    CHECK(idleTare.requestId == 0);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  }
}

void it13_lighter_replacement_write_zero_is_not_another_placement() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleWeight(-80.0f);
  idleWeight(-80.0f);
  idleCup(-80.0f);
  idleCup(-60.0f);
  ScaleCommand command;
  CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
  CHECK(claimIdleScaleTare(command.idleTareRequestId));
  idleWeight(-60.0f);
  idleWeight(-60.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  finishIdleScaleTare(command.idleTareRequestId, true);
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(idleTare.requestId == 0);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void it14_accepted_heavy_cup_removal_rearms_tare() {
  for (float weight : {499.0f, 500.0f, 501.0f, 600.0f, 1000.0f}) {
    prepareIdleTare();
    idleCup(weight);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    idleWeight(-weight);
    idleWeight(-weight);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    idleCup(-weight);
    idleCup(0.0f);
    CHECK(idleTare.requestId != 0);
  }
}

void it15_batched_removal_samples_reach_cup_detector() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  for (unsigned i = 0; i < 2; ++i) {
    hostMillis += 75;
    ScaleEvent event;
    event.type = ScaleEventType::WEIGHT;
    event.receivedAtMs = hostMillis;
    event.weightG = -80.0f;
    CHECK(publishScaleEvent(event, false));
  }
  processScaleWorkerEvents();
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(-80.0f);
  idleCup(0.0f);
  CHECK(idleTare.requestId != 0);
}

void it16_prewrite_removal_cannot_become_tare_zero() {
  prepareIdleTare();
  idleCup(80.0f);
  ScaleCommand command;
  CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
  const uint32_t firstAt = hostMillis + 50;
  hostMillis += 200;
  CHECK(claimIdleScaleTare(command.idleTareRequestId));
  publishWeight(0.0f, firstAt);
  publishWeight(0.0f, firstAt + 50);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  finishIdleScaleTare(command.idleTareRequestId, true);
  idleCup(0.0f);
  CHECK(idleTare.requestId == 0);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(80.0f);
  CHECK(idleTare.requestId != 0);
}

void it17_transport_success_without_zero_preserves_old_reference() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  for (unsigned i = 0; i < 22; ++i) idleWeight(80.0f);
  serviceIdleTare();
  CHECK(idleTare.requestId == 0);
  CHECK(!cupPresenceIsTared());
  idleWeight(0.0f);
  idleWeight(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(idleTare.requestId != 0);
}

void it18_placement_tolerance_bounds_entire_window() {
  prepareIdleTare();
  idleWeight(80.0f);
  idleWeight(82.0f);
  idleWeight(84.0f);
  CHECK(idleTare.requestId == 0);
  idleWeight(84.0f);
  idleWeight(84.0f);
  CHECK(idleTare.requestId != 0);
}

void it19_changed_queued_placement_cannot_tare() {
  prepareIdleTare();
  idleCup(80.0f);
  idleWeight(100.0f);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 0);
  CHECK(idleTare.requestId == 0);
  idleCup(100.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void it20_weight_overflow_breaks_removal_evidence() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleWeight(-80.0f); // First removal confirmation must not survive overflow.
  for (size_t i = 0; i <= SCALE_WEIGHT_EVENT_CAPACITY; ++i) {
    hostMillis += 1;
    ScaleEvent event;
    event.receivedAtMs = hostMillis;
    event.weightG = i == SCALE_WEIGHT_EVENT_CAPACITY ? -80.0f : 0.0f;
    CHECK(publishScaleEvent(event, false));
  }
  processScaleWorkerEvents();
  CHECK(scaleWeightEventDrops == SCALE_WEIGHT_EVENT_CAPACITY);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(cupPresence.removedConfirmations == 1);
  idleWeight(-80.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void it21_batched_sign_reversal_does_not_remove_cup() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  for (float weight : {-80.0f, 0.0f, -80.0f}) {
    hostMillis += 25;
    ScaleEvent event;
    event.receivedAtMs = hostMillis;
    event.weightG = weight;
    CHECK(publishScaleEvent(event, false));
  }
  processScaleWorkerEvents();
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(cupPresence.removedConfirmations == 1);
}

void it22_worker_preserves_capture_time_and_zero_boundary() {
  prepareIdleTare();
  idleCup(80.0f);
  ScaleCommand command;
  CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
  CHECK(claimIdleScaleTare(command.idleTareRequestId, 0, 41));
  scale.weight = 0.0f;
  scale.weightCapturedAtMs = hostMillis;
  scale.weightCaptureSequence = 41; // Captured before claim in the same ms.
  scale.newWeightAvailableValue = true;
  hostMillis += 50;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  CHECK(!idleTare.tareNotified);
  CHECK(idleTare.lastSampleAtMs == scale.weightCapturedAtMs);
  scale.weightCaptureSequence = 42; // Genuine during-write zero in same ms.
  scale.newWeightAvailableValue = true;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  CHECK(idleTare.tareNotified);
  finishIdleScaleTare(command.idleTareRequestId, true);
  serviceIdleTare();
  CHECK(idleTare.requestId == 0);
  CHECK(cupPresenceIsTared());
  CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
}

void it23_unknown_effect_releases_slot_but_not_cup_guard() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  hostMillis += SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs;
  serviceIdleTare();
  CHECK(idleTare.requestId == 0);
  CHECK(!cupPresenceIsKnown());
  CHECK(idleTare.lastReason == IdleTareReason::EFFECT_UNCONFIRMED);
  idleCup(0.0f); // Cannot invent absence or confirmed tared presence.
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  enableCupStartGuardForTest();
  beginCycle();
  CHECK(cupStartGuardHold);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
}

void it24_claim_waits_for_control_validation_of_published_sample() {
  for (float weight : {80.0f, 100.0f}) {
    prepareIdleTare();
    idleCup(80.0f);
    hostMillis += 50;
    ScaleEvent event;
    event.receivedAtMs = hostMillis;
    event.weightG = weight;
    CHECK(publishScaleEvent(event, false)); // Pending in handoff, control has not run.
    CHECK(executeNextScaleCommand()); // Worker must requeue without writing.
    CHECK(scale.tareCalls == 0);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
    CHECK(executeNextScaleCommand());
    CHECK(scale.tareCalls == (weight == 80.0f ? 1U : 0U));
  }
}

void it25_capture_and_packet_wrap_preserve_order() {
  prepareIdleTare();
  idleCup(80.0f);
  ScaleCommand command;
  CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
  CHECK(claimIdleScaleTare(command.idleTareRequestId, 0, UINT32_MAX));
  scale.weight = 0.0f;
  scale.weightCapturedAtMs = hostMillis;
  scale.weightCaptureSequence = 1;
  scale.newWeightAvailableValue = true;
  CHECK(publishPendingScaleWeightEvent());
  processScaleWorkerEvents();
  CHECK(idleTare.tareNotified);
  finishIdleScaleTare(command.idleTareRequestId, true);
  serviceIdleTare();
  idleTare.lastPacketSequence = UINT32_MAX - 1U;
  cupPresence.weight.sampleSequence = UINT32_MAX - 1U;
  publishWeight(-80.0f, hostMillis, 0, UINT32_MAX);
  publishWeight(-80.0f, hostMillis, 0, 1);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void it26_debug_uses_committed_cup_snapshot_and_terminal_reason() {
  prepareIdleTare();
  idleCup(80.0f);
  const uint32_t id = idleTare.requestId;
  idleWeight(100.0f);
  publishControlStatus();
  ControlStatusSnapshot snapshot;
  copyControlStatus(snapshot);
  CHECK(snapshot.cupTare.lastTerminalRequestId == id);
  CHECK(snapshot.cupTare.lastReason == static_cast<uint8_t>(IdleTareReason::UNSTABLE));
  const uint32_t placementId = snapshot.cupTare.placementId;
  resetCupPresence(); // Readers must retain the prior committed cup values.
  DebugExportExtras debug;
  copyDebugExportExtras(debug, snapshot);
  CHECK(debug.cupTare.placementId == placementId);
  CHECK(debug.cupState == static_cast<uint8_t>(CupPresenceState::PRESENT));
}

void it27_late_cup_retare_rejects_accumulated_drift() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t baseMs = hostMillis + 100;
  publishWeight(80.0f, baseMs, 1, 10);
  publishWeight(82.0f, baseMs + 150, 1, 11);
  publishWeight(84.0f, baseMs + 300, 1, 12);
  CHECK(!session.retarePerformed);
}

void it28_weight_handoff_concurrent_publication_is_bounded() {
  prepareIdleTare();
  std::atomic<bool> done{false};
  std::thread producer([&] {
    for (unsigned i = 0; i < 300; ++i) {
      ScaleEvent event;
      event.receivedAtMs = hostMillis;
      event.weightG = i % 2 ? 0.0f : 5.0f;
      (void)publishScaleEvent(event, false);
    }
    done.store(true, std::memory_order_release);
  });
  while (!done.load(std::memory_order_acquire)) {
    processScaleWorkerEvents();
    std::this_thread::yield();
  }
  producer.join();
  processScaleWorkerEvents();
  CHECK(scaleWeightEventCount == 0);
  CHECK(!scaleWeightEventPending);
  CHECK(idleTare.requestId == 0);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void it29_old_or_future_captured_weight_cannot_qualify() {
  for (int32_t offset : {-1001, 1}) {
    prepareIdleTare();
    hostMillis += 2000;
    scale.weight = 80.0f;
    scale.weightCapturedAtMs = hostMillis + offset;
    for (unsigned i = 0; i < 3; ++i) {
      ++scale.weightCaptureSequence;
      scale.newWeightAvailableValue = true;
      CHECK(publishPendingScaleWeightEvent());
      processScaleWorkerEvents();
    }
    CHECK(idleTare.requestId == 0);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(idleTare.rejectedSamples == 3);
  }
}

void it30_actual_negative_replacement_removal_during_write() {
  for (float replacement : {-60.0f, 0.0f, 40.0f}) {
    prepareIdleTare();
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    idleWeight(-80.0f);
    idleWeight(-80.0f);
    idleCup(-80.0f);
    idleCup(replacement);
    const uint32_t id = idleTare.requestId;
    CHECK(claimIdleScaleTare(id));
    idleWeight(-80.0f);
    idleWeight(-80.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(idleTare.requestId == id);
    finishIdleScaleTare(id, true);
    serviceIdleTare();
    CHECK(idleTare.requestId == 0);
    CHECK(!cupPresenceIsTared());
  }
}

void it31_idle_request_and_effect_deadline_cross_clock_wrap() {
  for (bool confirm : {false, true}) {
    prepareIdleTare();
    resetCupSampleEvidence();
    hostMillis = UINT32_MAX - 1000U;
    idleTare.lastPacketSequence = 0;
    idleCup(0.0f);
    idleCup(80.0f);
    const uint32_t id = idleTare.requestId;
    CHECK(id != 0);
    CHECK(executeNextScaleCommand());
    if (confirm) {
      idleWeight(0.0f, 200);
      CHECK(cupPresenceIsTared());
    } else {
      hostMillis += SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs;
      markScaleWorkerProgress();
      serviceIdleTare();
      CHECK(!cupPresenceIsKnown());
    }
    CHECK(idleTare.requestId == 0);
    CHECK(idleTare.lastTerminalRequestId == id);
  }
}

void it32_queued_minimum_is_independent_of_tolerance() {
  for (bool relative : {false, true}) {
    prepareIdleTare();
    float emptyG = 0.0f;
    if (relative) {
      idleCup(80.0f);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      idleWeight(-80.0f);
      idleWeight(-80.0f);
      emptyG = -80.0f;
      idleCup(emptyG);
    }
    idleCup(emptyG + runtimeConfig.minimumCupWeightG);
    CHECK(idleTare.requestId != 0);
    idleWeight(emptyG + runtimeConfig.minimumCupWeightG - 0.1f);
    CHECK(idleTare.requestId == 0);
    CHECK(idleTare.lastReason == IdleTareReason::UNSTABLE);
    CHECK(executeNextScaleCommand());
    CHECK(scale.tareCalls == (relative ? 1U : 0U));
  }
}

void it33_stale_connection_gap_cannot_cancel_current_tare() {
  prepareIdleTare();
  const uint32_t oldGeneration = getScaleLinkSnapshot().connectionGeneration;
  setScaleConnected(false);
  setScaleConnected(true);
  idleCup(0.0f);
  idleCup(80.0f);
  const uint32_t id = idleTare.requestId;
  CHECK(id != 0);
  ScaleEvent stale;
  stale.receivedAtMs = hostMillis;
  stale.connectionGeneration = oldGeneration;
  stale.sampleDiscontinuity = true;
  CHECK(publishScaleEvent(stale, false));
  processScaleWorkerEvents();
  CHECK(idleTare.requestId == id);
  CHECK(idleScaleTareStatus().phase == IdleTarePhase::QUEUED);
}

void it33b_pre_disconnect_weight_cannot_cross_disconnect_epoch() {
  resetHarness(false, true);
  const ScaleLinkSnapshot before = getScaleLinkSnapshot();
  ScaleEvent stale;
  stale.type = ScaleEventType::WEIGHT;
  stale.receivedAtMs = hostMillis;
  stale.connectionGeneration = before.connectionGeneration;
  stale.weightG = 77.0f;
  CHECK(publishScaleEvent(stale, false));
  setScaleConnected(false);
  CHECK(getScaleLinkSnapshot().disconnectSequence ==
        before.disconnectSequence + 1);
  debugLog.clear();
  processScaleWorkerEvents();
  CHECK(observedWeight == 0.0f);
  CHECK(currentWeight == 0.0f);
  DebugEvent rejected[4] = {};
  const size_t count = copyDebugEvents(0, rejected, 4);
  bool found = false;
  for (size_t index = 0; index < count; ++index) {
    if (rejected[index].code != DebugCode::SCALE_STALE_EVENT_REJECTED) continue;
    found = true;
    CHECK(strstr(rejected[index].text,
                 "weight reason=link_epoch gen(e/n)=") != nullptr);
    CHECK(strstr(rejected[index].text, "disc(e/n)=") != nullptr);
    char message[128] = {};
    formatDebugEventMessage(rejected[index], message, sizeof(message), FW_VERSION);
    CHECK(strcmp(message, rejected[index].text) == 0);
  }
  CHECK(found);
}

void it33d_pre_cycle_weight_warning_identifies_timestamps() {
  resetHarness(false, true);
  session.active = true;
  session.id = 42;
  session.startedAtMs = hostMillis + 100;
  ScaleEvent stale;
  stale.type = ScaleEventType::WEIGHT;
  stale.receivedAtMs = hostMillis;
  stale.weightG = 12.0f;
  CHECK(publishScaleEvent(stale, false));
  debugLog.clear();
  processScaleWorkerEvents();
  DebugEvent events[4] = {};
  const size_t count = copyDebugEvents(0, events, 4);
  bool found = false;
  for (size_t index = 0; index < count; ++index) {
    found |= events[index].code == DebugCode::SCALE_STALE_EVENT_REJECTED &&
             strstr(events[index].text,
                    "weight reason=before cycle sample_ms=") != nullptr &&
             strstr(events[index].text, "cycle=42") != nullptr;
  }
  CHECK(found);
}

void it33c_pre_disconnect_command_result_cannot_cross_disconnect_epoch() {
  resetHarness(false, true);
  session.id = 42;
  session.active = true;
  ScaleEvent stale;
  stale.type = ScaleEventType::TIMER_START_RESULT;
  stale.cycleId = session.id;
  stale.commandAttempted = true;
  stale.writeSucceeded = true;
  CHECK(publishScaleEvent(stale, true));
  setScaleConnected(false);
  const bool settledAfterDisconnect = session.remoteTimerStartSettled;
  const bool startedAfterDisconnect = session.remoteTimerStarted;
  processScaleWorkerEvents();
  CHECK(session.remoteTimerStartSettled == settledAfterDisconnect);
  CHECK(session.remoteTimerStarted == startedAfterDisconnect);
}

void it34_final_prewrite_sample_requires_control_approval() {
  for (bool contradictory : {false, true}) {
    prepareIdleTare();
    idleCup(80.0f);
    scale.weight = contradictory ? 0.0f : 80.0f;
    scale.weightCaptureSequence = 41;
    scale.newWeightAvailableCalls = 0;
    scale.beforeWeightCheck = [] {
      if (scale.newWeightAvailableCalls != 1) return;
      ++scale.weightCaptureSequence;
      scale.newWeightAvailableValue = true;
      scale.beforeWeightCheck = nullptr;
    };
    CHECK(executeNextScaleCommand());
    CHECK(scale.tareCalls == 0);
    CHECK(!cupPresenceIsTared());
    if (contradictory) {
      CHECK(idleTare.requestId == 0);
      CHECK(idleTare.lastReason == IdleTareReason::UNSTABLE);
      CHECK(executeNextScaleCommand()); // Cancelled queue entry cannot write.
      CHECK(scale.tareCalls == 0);
    } else {
      CHECK(idleScaleTareStatus().phase == IdleTarePhase::QUEUED);
      CHECK(executeNextScaleCommand());
      CHECK(scale.tareCalls == 1);
      CHECK(idleScaleTareStatus().captureBoundary == 42);
      idleWeight(0.0f);
      CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    }
  }
}

void it35_loaded_startup_and_sample_gap_need_fresh_placement() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(80.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(idleTare.lastReason == IdleTareReason::NO_ABSENCE);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(0.0f);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);

  prepareIdleTare(); // Bookoo zeroed an 80 g load before first connection.
  idleCup(-80.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(cupPresence.emptyAnchorG == -80.0f);
  idleCup(20.0f); // Different 100 g replacement does not revisit original zero.
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);

  prepareIdleTare();
  idleWeight(80.0f, runtimeConfig.retareStabilityMaxGapMs + 1);
  idleCup(80.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(0.0f);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it36_bookoo_startup_zero_unload_rearms_relative_tare() {
  for (float initial : {100.0f, 200.0f}) {
    prepareIdleTare(); // Initial loaded pan reports zero.
    const float replacement = initial == 100.0f ? 200.0f : 100.0f;
    if (initial == 200.0f) {
      idleWeight(-1.0f);
      idleWeight(-5.0f);
      idleWeight(-20.0f); // A continuous unload may cross the minimum gradually.
    }
    idleWeight(-initial - 5.0f); // A small undershoot still permits the stable pan.
    CHECK(!cupPresence.weight.emptyValid);
    CHECK(!captureCupTareDiagnostics().emptyReferenceBlocked);
    idleCup(-initial);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(cupPresence.weight.emptyValid);
    CHECK(cupPresence.emptyAnchorG == -initial);
    CHECK(idleTare.absentObserved);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    idleCup(replacement - initial);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(captureCupTareDiagnostics().weightValid);
    CHECK(captureCupTareDiagnostics().weightG == replacement);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    CHECK(scale.tareCalls == 1);
  }
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(-100.0f); // No preceding zero: unknown offset, not observed removal.
  idleCup(100.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);

  prepareIdleTare();
  idleWeight(-2.0f);
  idleWeight(-100.0f, runtimeConfig.retareStabilityMaxGapMs + 1);
  idleCup(-100.0f);
  CHECK(!cupPresence.weight.emptyValid);
  idleCup(100.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);

  prepareIdleTare();
  idleCup(-100.0f);
  idleCup(0.0f); // Same-mass replacement looks like pan motion returning to zero.
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);

  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(0.2f);
  idleCup(-9.9f);
  CHECK(cupPresence.emptyAnchorG == -9.9f);
  idleCup(0.2f); // Initial zero may be slightly positive, not exactly 0 g.
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void it52_small_empty_offset_after_moving_bookoo_rearms_idle_tare() {
  for (float cupG : {50.0f, 100.0f, 200.0f, 400.0f}) {
    for (unsigned phase = 0; phase < 3; ++phase) {
      resetHarness(false, true);
      reachReadyFromBoot();
      runtimeConfig.autoTareOutsideBrew = true;
      if (phase != 0) idleCup(0.0f);
      if (phase == 2) {
        setScaleConnected(false);
        setScaleConnected(true);
      }
      idleCup(-3.0f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
      idleCup(cupG - 3.0f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f);
      CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    }
  }
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(-6.0f);
  idleCup(50.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void it53_unknown_empty_rejects_replacement_and_ingredient() {
  prepareIdleTare(); // Bookoo booted with an 80 g cup already zeroed.
  idleWeight(-80.0f);
  idleCup(20.0f); // A different 100 g cup arrived before empty settled.
  CHECK(std::isnan(cupPresence.emptyAnchorG));
  idleCup(40.0f); // Coffee added to the same replacement cup.
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(scale.tareCalls == 0);

  for (unsigned emptySamples = 1; emptySamples <= 4; ++emptySamples) {
    prepareIdleTare();
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    hostMillis += SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs;
    markScaleWorkerProgress();
    serviceIdleTare();
    CHECK(!cupPresenceIsKnown());
    for (unsigned i = 0; i < emptySamples; ++i) idleWeight(-80.0f);
    idleCup(20.0f);
    idleCup(40.0f);
    CHECK(std::isnan(cupPresence.emptyAnchorG));
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    CHECK(scale.tareCalls == 1);
  }
}

void it54_unknown_empty_recovers_only_with_qualified_pan() {
  prepareIdleTare();
  idleWeight(-115.0f); // 15 g rebound is ambiguous with a new cup.
  idleCup(-100.0f);
  CHECK(std::isnan(cupPresence.emptyAnchorG));
  CHECK(captureCupTareDiagnostics().emptyReferenceBlocked);
  CHECK(cupPresenceIsKnown()); // Presentation must not change cup-start policy.
  idleCup(100.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(-100.0f); // Remove the replacement before taring the empty pan.
  executeScaleDebugCommand(BookooDebugAction::TARE, 0);
  processScaleWorkerEvents();
  idleCup(0.0f);
  idleCup(100.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);

  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  hostMillis += SCALE_ATT_TIMEOUT_MS + runtimeConfig.postTareBaselineGraceMs;
  markScaleWorkerProgress();
  serviceIdleTare();
  for (unsigned i = 0; i < 5; ++i) idleWeight(-80.0f);
  CHECK(cupPresence.emptyAnchorG == -80.0f);
  idleCup(20.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void r25d_reference_change_survives_full_critical_queue() {
  prepareIdleTare();
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(-80.0f);
  CHECK(cupPresence.emptyAnchorG == -80.0f);
  ScaleEvent filler;
  filler.type = ScaleEventType::TIMER_STOP_RESULT;
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  filler.connectionGeneration = link.connectionGeneration;
  filler.disconnectSequence = link.disconnectSequence;
  for (size_t index = 0; index < SCALE_EVENT_QUEUE_LENGTH; ++index)
    CHECK(xQueueSend(scaleEventQueue, &filler, 0) == pdTRUE);
  executeScaleDebugCommand(BookooDebugAction::TARE, 0);
  CHECK(scaleReferenceChangedEventPending);
  CHECK(publishScaleEvent(filler, true));
  processScaleWorkerEvents();
  CHECK(!scaleReferenceChangedEventPending);
  CHECK(std::isnan(cupPresence.emptyAnchorG));
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it55_unknown_empty_rebound_obeys_minimum_cup_boundary() {
  for (float minimum : {1.0f, 10.0f, 100.0f}) {
    for (float tolerance : {0.1f, 20.0f}) {
      for (bool cupSizedRebound : {false, true}) {
        prepareIdleTare();
        runtimeConfig.minimumCupWeightG = minimum;
        runtimeConfig.retareStabilityToleranceG = tolerance;
        idleWeight(-100.0f - minimum + (cupSizedRebound ? 0.0f : 0.1f));
        idleCup(-100.0f);
        CHECK(std::isfinite(cupPresence.emptyAnchorG) == !cupSizedRebound);
        CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      }
    }
  }
}

void it56_transient_negative_movement_recovers_idle_readiness() {
  prepareIdleTare();
  CHECK(idleTare.absentObserved);
  for (float weight : {-200.0f, -60.0f, -150.0f, -1.0f, 0.0f})
    idleWeight(weight);
  for (unsigned i = 0; i < 200; ++i) idleWeight(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(cupPresencePlacementId() == 0);
  CHECK(cupPresence.weight.emptyValid);
  CHECK(idleTare.absentObserved);
  CHECK(!captureCupTareDiagnostics().emptyReferenceBlocked);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
  CHECK(scale.tareCalls == 1);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
}

void it57_repeated_zero_recovery_requires_configured_stability() {
  for (float minimum : {1.0f, 10.0f, 100.0f}) {
    prepareIdleTare();
    runtimeConfig.minimumCupWeightG = minimum;
    runtimeConfig.retareStabilitySamples = 5;
    runtimeConfig.retareStabilityMinDurationMs = 900;
    runtimeConfig.retareStabilityToleranceG = 0.2f;
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
      idleWeight(-200.0f);
      idleWeight(-60.0f);
      idleWeight(-150.0f);
      idleWeight(0.0f);
      for (unsigned i = 1; i < runtimeConfig.retareStabilitySamples; ++i)
        idleWeight(0.0f);
      CHECK(!cupPresence.weight.emptyValid); // Samples alone do not meet 900 ms.
      idleWeight(-0.2f);
      idleWeight(0.2f); // Spread restarts the window despite elapsed time.
      CHECK(!idleTare.absentObserved);
      for (unsigned i = 0; i < 6; ++i) {
        idleWeight(i % 2 == 0 ? -0.05f : 0.05f);
        CHECK(!cupPresence.weight.emptyValid);
      }
      idleWeight(0.0f);
      CHECK(cupPresence.weight.emptyValid);
      CHECK(idleTare.absentObserved);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
      CHECK(cupPresencePlacementId() == 0);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    }
    for (unsigned i = 0; i < 7; ++i) idleWeight(-120.0f);
    CHECK(cupPresence.weight.emptyValid);
    CHECK(cupPresence.emptyAnchorG == -120.0f); // Earlier -200 g cannot taint a new unload.
    for (unsigned i = 0; i < 7; ++i) idleWeight(minimum - 120.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it58_cup_before_zero_recovery_reuses_previous_qualified_zero() {
  for (unsigned emptySamples = 1; emptySamples < DEFAULT_RETARE_STABILITY_SAMPLES;
       ++emptySamples) {
    prepareIdleTare();
    idleWeight(-200.0f);
    idleWeight(-60.0f);
    idleWeight(-150.0f);
    for (unsigned i = 0; i < emptySamples; ++i) idleWeight(0.0f);
    CHECK(!cupPresence.weight.emptyValid);
    CHECK(!idleTare.absentObserved);
    idleCup(80.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(cupPresencePlacementId() == 1);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it64_bookoo_handling_profile_tares_first_stable_cup() {
  // Profile 3: continuous handling after a qualified zero, then cup placement.
  const struct { uint32_t atMs; float weight; } samples[] = {
      {10259, 1.0f}, {10359, 18.7f}, {10469, 24.9f}, {10589, -14.6f},
      {10679, -101.1f}, {10769, -149.6f}, {10919, -158.2f}, {10979, -157.2f},
      {11069, -156.3f}, {11189, -153.5f}, {11279, -146.5f}, {11369, -145.0f},
      {11489, -148.8f}, {11579, -152.1f}, {11669, -153.6f}, {11789, -153.2f},
      {11879, -151.3f}, {11969, -151.2f}, {12089, -155.5f}, {12179, -149.5f},
      {12299, -152.7f}, {12389, -147.6f}, {12479, -101.3f}, {12599, -8.8f},
      {12689, 0.5f}, {12809, 0.6f}, {12899, 2.7f}, {12989, 1.6f},
      {13109, -23.3f}, {13199, -52.3f}, {13290, -64.69f}, {13380, -37.0f},
      {13499, -28.2f}, {13589, -10.3f}, {13739, -4.3f}, {13799, -3.7f},
      {13889, -0.8f}, {14009, 0.0f}, {14099, 19.2f}, {14219, 78.6f},
      {14339, 162.2f}, {14429, 303.5f}, {14520, 347.5f}, {14609, 364.2f},
      {14729, 374.8f}, {14789, 377.8f}, {14909, 377.8f}, {15029, 377.8f},
      {15119, 377.8f}};
  for (unsigned mode = 0; mode < 3; ++mode) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoTareOutsideBrew = true;
    if (mode != 2) idleCup(0.0f);
    uint32_t previousMs = 10159;
    for (const auto &sample : samples) {
      const uint32_t interval = mode == 1 && sample.atMs == 14099
          ? runtimeConfig.retareStabilityMaxGapMs + 1 : sample.atMs - previousMs;
      idleWeight(sample.weight, interval);
      if (sample.atMs < 15119) CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      previousMs = sample.atMs;
    }
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (mode == 0 ? 1 : 0));
    if (mode != 0) continue; // A missing initial zero or sample gap cannot recover.
    CHECK(cupPresence.emptyAnchorG == 0.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    for (unsigned sample = 0; sample < 60; ++sample) idleWeight(0.0f);
    CHECK(scale.tareCalls == 1);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
    CHECK(!session.active);
    CHECK(!getRelaySafetySnapshot().closed);
  }
}

void it65_profile5_handling_tares_first_physical_cup() {
  // Original notification times/float32 weights, through the first stable cup.
  const struct { uint32_t atMs; float weight; } samples[] = {
      {9305, 0.0f}, {9405, 0.0f}, {9505, 0.0f}, {9605, 0.0f},
      {9705, 0.0f}, {9805, 0.0f}, {9905, 0.0f}, {10005, 0.0f},
      {10105, 0.0f}, {10205, 0.0f}, {10305, 0.0f}, {10355, 0.0f},
      {10505, 0.0f}, {10605, 0.0f}, {10705, 0.0f}, {10805, 0.0f},
      {10905, 0.0f}, {11005, 4.5f}, {11105, 29.3999996f}, {11205, 104.0f},
      {11305, 237.5f}, {11355, 327.899994f}, {11455, 294.290009f}, {11555, 333.700012f},
      {11656, 491.0f}, {11755, 355.799988f}, {11855, -82.4000015f}, {11955, -361.100006f},
      {12125, -427.0f}, {12155, -428.100006f}, {12245, -430.0f}, {12335, -431.899994f},
      {12455, -427.200012f}, {12545, -416.600006f}, {12665, -401.799988f}, {12755, -346.299988f},
      {12845, -213.800003f}, {12965, -69.5f}, {13055, 21.8999996f}, {13175, 23.1000004f},
      {13265, 5.80000019f}, {13355, 1.89999998f}, {13445, 2.5f}, {13565, -4.19000006f},
      {13655, -38.9000015f}, {13745, -54.4000015f}, {13865, 7.19999981f}, {13955, 80.3000031f},
      {14075, 124.699997f}, {14165, 129.600006f}, {14255, 83.8000031f}, {14375, 34.0f},
      {14465, 32.2999992f}, {14585, 46.2999992f}, {14675, 57.2000008f}, {14795, 48.4000015f},
      {14885, 19.7900009f}, {14975, 14.0f}, {15065, 30.1000004f}, {15155, 109.400002f},
      {15275, 254.699997f}, {15365, 367.899994f}, {15485, 431.200012f}, {15575, 463.700012f},
      {15695, 433.600006f}, {15815, 393.0f}, {15905, 379.100006f}, {15965, 377.399994f},
      {16115, 377.600006f}, {16205, 377.700012f}, {16265, 377.700012f}};
  for (unsigned mode = 0; mode < 3; ++mode) {
    for (uint32_t delayMs : {0U, 5U, 10U}) {
      resetHarness(false, true);
      reachReadyFromBoot();
      runtimeConfig.autoTareOutsideBrew = true;
      for (const auto &sample : samples) {
        if (mode == 1 && sample.atMs <= 10905) continue; // No initial zero.
        const uint32_t atMs = sample.atMs + (mode == 2 && sample.atMs >= 13865
            ? runtimeConfig.retareStabilityMaxGapMs + 1 : 0); // Interrupted return.
        hostMillis = atMs + delayMs;
        markScaleWorkerProgress();
        publishWeight(sample.weight, atMs);
        if (sample.atMs < 16205) CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      }
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (mode == 0 ? 1U : 0U));
      if (mode != 0) continue;
      CHECK(idleTare.requestedAtMs == 16205 + delayMs);
      CHECK(cupPresence.weight.weightG == 377.700012f);
      CHECK(executeNextScaleCommand());
      idleCup(0.0f); // Simulated response to the newly authorized first tare.
      CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
      CHECK(scale.tareCalls == 1);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
      CHECK(!session.active);
      CHECK(!getRelaySafetySnapshot().closed);
    }
  }
}

void it66_initial_positive_return_requires_known_zero_and_subminimum_load() {
  for (float minimum : {1.0f, 2.0f, 10.0f, 100.0f}) {
    for (bool belowMinimum : {false, true}) {
      prepareIdleTare();
      runtimeConfig.minimumCupWeightG = minimum;
      idleWeight(-200.0f);
      idleWeight(minimum - (belowMinimum ? 0.1f : 0.0f));
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      CHECK(!idleTare.absentObserved);
      idleCup(minimum + 20.0f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (belowMinimum ? 1U : 0U));
    }
  }
}

void it59_ambiguous_empty_guidance_is_idle_only() {
  prepareIdleTare();
  idleWeight(-115.0f);
  idleCup(-100.0f);
  CHECK(captureCupTareDiagnostics().emptyReferenceBlocked);
  session.active = true;
  CHECK(!captureCupTareDiagnostics().emptyReferenceBlocked);
  CHECK(cupPresenceIsKnown());
}

void it60_mixed_movement_recovers_small_zero_offsets_without_delay() {
  for (uint32_t interval : {100U, 110U}) {
    for (float empty : {-5.0f, -3.0f, -0.6f, -0.4f, 0.0f, 0.4f, 0.6f, 3.0f, 5.0f}) {
      for (float cup : {10.0f, 20.0f, 100.0f, 200.0f}) {
        prepareIdleTare();
        for (float weight : {-200.0f, 70.0f, -60.0f, 35.0f, -150.0f})
          idleWeight(weight, interval);
        for (unsigned sample = 0; sample < 3; ++sample) {
          idleWeight(empty, interval);
          CHECK(!idleTare.absentObserved);
        }
        idleWeight(empty, interval); // Original 300 ms window, no added delay.
        CHECK(idleTare.absentObserved);
        CHECK(cupPresenceState() == CupPresenceState::ABSENT);
        CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
        for (unsigned sample = 0; sample < 3; ++sample) {
          idleWeight(empty + cup, interval);
          CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
        }
        idleWeight(empty + cup, interval);
        CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
        CHECK(executeNextScaleCommand());
        idleWeight(0.0f, interval);
        CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
        CHECK(!session.active);
        CHECK(!getRelaySafetySnapshot().closed);
      }
    }
  }
}

void it61_unqualified_startup_placement_cannot_lock_zero_recovery() {
  for (float empty : {-0.6f, 0.0f, 0.6f}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoTareOutsideBrew = true;
    for (float weight : {-200.0f, 200.0f, 201.0f, 200.0f, -100.0f, -150.0f})
      idleWeight(weight);
    CHECK(cupPresencePlacementId() == 1);
    CHECK(idleTare.lastReason == IdleTareReason::NO_ABSENCE);
    idleCup(empty);
    CHECK(idleTare.absentObserved);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    idleCup(empty + 80.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it62_small_empty_shifts_cannot_accumulate_away_from_zero() {
  prepareIdleTare();
  idleCup(-3.0f);
  CHECK(cupPresence.emptyAnchorG == -3.0f);
  idleCup(-6.0f);
  idleCup(-9.0f);
  CHECK(cupPresence.emptyAnchorG == -3.0f);
  CHECK(!idleTare.absentObserved);
  for (float weight : {-200.0f, 70.0f, -100.0f}) idleWeight(weight);
  idleCup(0.0f);
  CHECK(idleTare.absentObserved);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it63_initial_recovery_respects_stability_and_zero_boundaries() {
  for (float minimum : {1.0f, 2.0f, 10.0f, 100.0f}) {
    for (bool outside : {false, true}) {
      resetHarness(false, true);
      reachReadyFromBoot();
      runtimeConfig.autoTareOutsideBrew = true;
      runtimeConfig.minimumCupWeightG = minimum;
      idleCup(200.0f); // Absolute startup presence has no qualified empty reference.
      idleWeight(-100.0f);
      idleWeight(-150.0f);
      runtimeConfig.retareStabilitySamples = 5;
      runtimeConfig.retareStabilityMinDurationMs = 900;
      const float empty = -fminf(5.0f, minimum / 2.0f) - (outside ? 0.1f : 0.0f);
      for (unsigned sample = 0; sample < 6; ++sample) {
        idleWeight(empty);
        CHECK(!idleTare.absentObserved);
      }
      idleWeight(empty);
      CHECK(idleTare.absentObserved == !outside);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      for (unsigned sample = 0; sample < 7; ++sample) idleWeight(empty + minimum);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (outside ? 0U : 1U));
    }
  }
}

void it67_offanchor_empty_plateau_requalifies_after_removal() {
  for (float empty : {-247.0f, -262.0f}) {
    prepareIdleTare();
    idleCup(250.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(cupPresence.emptyAnchorG == -250.0f);
    idleCup(empty);  // Lift, then let the empty pan settle.
    idleCup(empty);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(cupPresence.weight.emptyValid);
    CHECK(cupPresence.emptyAnchorG == empty);
    CHECK(idleTare.absentObserved);
    idleCup(empty + 250.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
    CHECK(captureCupTareDiagnostics().weightG == 250.0f);
  }
}

void it68_plateau_above_anchor_plus_mincup_still_waits() {
  prepareIdleTare();
  idleCup(250.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(-200.0f);  // Could be the pan plus a 50 g load: not an empty pan.
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(!cupPresence.weight.emptyValid);
  CHECK(cupPresence.emptyAnchorG == -250.0f);
  CHECK(captureCupTareDiagnostics().anchorDeltaG == 50.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(-300.0f);  // Lifting that load reveals an empty plateau again.
  CHECK(cupPresence.weight.emptyValid);
  CHECK(cupPresence.emptyAnchorG == -300.0f);
  CHECK(std::isnan(captureCupTareDiagnostics().anchorDeltaG));
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

uint32_t profileEventCount(ScaleProfileEvent kind, uint32_t *arg2 = nullptr) {
  uint32_t count = 0;
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    if (row.kind != static_cast<uint16_t>(kind)) continue;
    ++count;
    if (arg2 != nullptr) *arg2 = row.arg2;
  }
  return count;
}

void it73_off_anchor_wait_age_and_profiler_event() {
  // F8: a stable plateau rejected as off the empty reference reports how long
  // it has waited and marks the profiler once; requalifying clears the age.
  prepareIdleTare();
  idleCup(250.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(scaleProfiler().hostStartNoFlash(hostMillis));
  CHECK(captureCupTareDiagnostics().anchorWaitMs == 0);
  idleCup(-200.0f);  // The last sample confirms the lift and starts the wait.
  CHECK(profileEventCount(ScaleProfileEvent::CUP_ANCHOR_REJECTED) == 0);
  idleCup(-200.0f);
  CHECK(captureCupTareDiagnostics().anchorWaitMs == 450);
  idleCup(-200.0f);
  CHECK(captureCupTareDiagnostics().anchorWaitMs == 900);
  uint32_t deltaCg = 0;
  CHECK(profileEventCount(ScaleProfileEvent::CUP_ANCHOR_REJECTED, &deltaCg) == 1);
  CHECK(deltaCg == 5000);
  ScaleProfileRecord row{};
  row.kind = static_cast<uint16_t>(ScaleProfileEvent::CUP_ANCHOR_REJECTED);
  row.arg2 = static_cast<uint32_t>(-5000);
  char line[192] = {};
  CHECK(formatScaleProfileRow(line, sizeof(line), row) > 0);
  CHECK(strstr(line, "CUP_ANCHOR_REJECTED placement=0 offset_cg=-5000") != nullptr);
  idleCup(-300.0f);
  CHECK(cupPresence.weight.emptyValid);
  CHECK(captureCupTareDiagnostics().anchorWaitMs == 0);
  CHECK(profileEventCount(ScaleProfileEvent::CUP_ANCHOR_REJECTED) == 1);
}

void it69_button_zero_on_empty_pan_rearms_idle_tare() {
  prepareIdleTare();
  idleCup(250.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  // The lift reads off the anchor, so it is no fast-replacement unload; the
  // scale's button is pressed before that plateau settles.
  idleWeight(-247.0f);
  idleWeight(-247.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(!cupPresence.weight.emptyValid);
  CHECK(scaleProfiler().hostStartNoFlash(millis()));
  idleCup(0.0f);
  idleCup(0.0f);
  CHECK(cupPresence.weight.emptyValid);
  CHECK(cupPresence.emptyAnchorG == 0.0f);
  CHECK(idleTare.absentObserved);
  bool anchorEvent = false;
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    anchorEvent |= row.kind == static_cast<uint16_t>(ScaleProfileEvent::CUP_ANCHOR_CHANGED) &&
                   row.arg2 == 0;
  }
  CHECK(anchorEvent);
  scaleProfiler().hostStop(ScaleProfilerStopReason::USER, millis());
  idleCup(250.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(captureCupTareDiagnostics().weightG == 250.0f);
}

void it70_equal_weight_swap_at_zero_shows_absent() {
  // Trade-off of the external re-zero: an equal-weight cup set down before
  // the pan settled reads near the old zero and is taken for an empty pan.
  prepareIdleTare();
  idleCup(250.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleWeight(-247.0f);
  idleWeight(-247.0f);
  idleCup(3.0f);  // Equal-weight cup set down before the empty pan settled.
  idleCup(3.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(cupPresence.emptyAnchorG == 3.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(-247.0f);  // Lift it and let the pan settle below that reference.
  idleCup(-247.0f);
  CHECK(cupPresence.emptyAnchorG == -247.0f);
  idleCup(3.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it71_tare_with_coffee_then_lift_recovers() {
  // The shot-start tare zeroes the cup, so lifting it with its coffee reads
  // below the translated anchor; that plateau must still rearm idle tare.
  prepareIdleTare();
  idleCup(300.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  startCycle();
  while (executeNextScaleCommand()) {}
  establishPostTareBaseline();
  CHECK(cupPresence.emptyAnchorG == -300.0f);
  CHECK(finalizeCycle(EndReason::ACTIVATOR, StopperState::READY));
  idleCup(36.0f);
  idleCup(-336.0f);  // Lift the cup with its coffee, then let the pan settle.
  idleCup(-336.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(cupPresence.weight.emptyValid);
  CHECK(cupPresence.emptyAnchorG == -336.0f);
  idleCup(-36.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(captureCupTareDiagnostics().weightG == 300.0f);
}

void it37_accessory_retare_is_opt_in_and_once_before_shot() {
  for (bool enabled : {false, true}) {
    prepareIdleTare();
    if (enabled) runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    idleCup(5.0f); // Below the configured minimum added mass.
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    idleWeight(40.0f);
    idleWeight(44.0f); // Spread exceeds the stable-weight tolerance.
    idleCup(40.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (enabled ? 1U : 0U));
    if (!enabled) continue;
    CHECK(idleTare.accessoryRequest);
    CHECK(idleTare.requestMinimumG == 40.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    CHECK(captureCupTareDiagnostics().weightG == 120.0f);
    idleCup(30.0f); // A second stable addition does not repeat the retare.
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    startCycle();
    while (executeNextScaleCommand()) {}
    establishPostTareBaseline();
    idleCup(40.0f); // In-shot weight must not use the idle accessory path.
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    CHECK(finalizeCycle(EndReason::ACTIVATOR, StopperState::READY));
    idleCup(45.0f); // Same cup after the shot cannot rearm the option.
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  }
}

void it72_accessory_ramp_is_not_tared() {
  for (bool ramp : {true, false}) {
    prepareIdleTare();
    runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
    idleCup(80.0f);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
    if (ramp) {  // Milk poured into the tared cup: +0.4 g per reading.
      for (float w = 0.4f; w <= 15.0f; w += 0.4f) idleWeight(w, 100);
      idleCup(15.0f);
    } else {  // A spoon set down in one motion.
      idleWeight(12.0f, 100);
      idleCup(12.0f);
    }
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == (ramp ? 0U : 1U));
  }
}

void prepareTaredCupForAccessory() {
  prepareIdleTare();
  runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
  idleCup(300.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
}

void it74_accessory_landing_overshoot_is_tared() {
  for (bool slosh : {false, true}) {
    prepareTaredCupForAccessory();
    if (slosh) {  // Rocks for about two seconds before it settles.
      for (float w : {30.0f, 60.0f, 50.0f}) idleWeight(w, 100);
      for (float w : {46.0f, 40.0f, 44.0f, 41.0f, 43.0f}) idleWeight(w, 300);
    } else {  // Field trace: a hand press peaks at 2.3x a 42.2 g bar.
      const std::pair<uint32_t, float> trace[] = {{60, 4.0f}, {120, 27.9f},
          {90, 51.8f}, {120, 82.7f}, {90, 98.2f}, {90, 75.69f}, {90, 44.1f}};
      for (const auto &sample : trace) idleWeight(sample.second, sample.first);
    }
    for (int i = 0; i < 5; ++i) idleWeight(42.2f, 100);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it75_accessory_pour_shapes_are_not_tared() {
  for (int shape = 0; shape < 2; ++shape) {
    prepareTaredCupForAccessory();
    if (shape == 0) {  // Two syrup pumps: the second step starts above half the minimum.
      idleCup(8.0f);
      for (int i = 0; i < 6; ++i) idleWeight(8.0f, 100);
      idleCup(16.0f);
    } else {  // A fast pour keeps rising past its first-gap peak.
      for (float w = 2.5f; w <= 150.0f; w += 2.5f) idleWeight(w, 100);
      idleCup(150.0f);
    }
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  }
}

void it76_unstable_accessory_request_retries() {
  prepareTaredCupForAccessory();
  idleCup(42.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  idleWeight(48.0f);  // A bump before the write releases the queued request.
  CHECK(idleTare.requestId == 0);
  CHECK(idleTare.lastReason == IdleTareReason::UNSTABLE);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 1U);
  idleCup(42.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(idleTare.accessoryRequest);
}

void it38_accessory_retare_requires_continuous_confirmed_placement() {
  for (unsigned mode = 0; mode < 4; ++mode) {
    prepareIdleTare();
    runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
    idleCup(80.0f);
    if (mode == 0) scale.tareSucceeds = false;
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    if (mode == 1) {
      setScaleConnected(false);
      setScaleConnected(true);
    } else if (mode == 2) {
      RuntimeConfig changed = runtimeConfig;
      changed.retareStabilityToleranceG += 1.0f;
      ++changed.revision;
      commitLiveRuntimeConfig(changed, RUNTIME_PERSIST_REASON_USER);
    } else if (mode == 3) {
      idleWeight(0.0f, runtimeConfig.retareStabilityMaxGapMs + 1);
    }
    idleCup(40.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  }
}

void prepareTaredCupAndAccessory(float cupG, float accessoryG) {
  prepareIdleTare();
  runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
  idleCup(cupG);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(accessoryG);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(fabsf(captureCupTareDiagnostics().weightG - cupG - accessoryG) < 0.1f);
}

void it39_known_accessory_removal_retares_without_losing_cup() {
  for (const auto &masses : {std::pair{80.0f, 40.0f}, std::pair{80.0f, 120.0f}}) {
    prepareTaredCupAndAccessory(masses.first, masses.second);
    idleCup(-masses.second);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    CHECK(cupPresenceState() == CupPresenceState::PRESENT);
    CHECK(fabsf(captureCupTareDiagnostics().weightG - masses.first) < 0.1f);
    idleWeight(-masses.first);
    idleWeight(-masses.first);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    idleCup(-masses.first);
    idleCup(0.0f);
    idleCup(masses.first);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it40_accessory_unload_sequences_keep_next_placement_eligible() {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    prepareTaredCupAndAccessory(80.0f, scenario == 5 ? 80.0f : 40.0f);
    const float totalG = scenario == 5 ? 160.0f : 120.0f;
    if (scenario == 0) { // Both lifted together.
      idleWeight(-totalG);
      idleWeight(-totalG);
    } else if (scenario == 1) { // Cup lifted first, accessory remains.
      idleWeight(-80.0f);
      idleWeight(-80.0f);
      idleCup(-80.0f);
    } else if (scenario == 2) { // Accessory lifted briefly, then both.
      idleWeight(-40.0f);
      idleWeight(-totalG);
      idleWeight(-totalG);
    } else if (scenario == 3) { // Accessory put back before it settles.
      idleWeight(-40.0f);
      idleCup(0.0f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
      idleWeight(-totalG);
      idleWeight(-totalG);
    } else if (scenario == 4) { // Unmatched partial drop.
      idleWeight(-60.0f);
      idleWeight(-60.0f);
      idleCup(-60.0f);
    } else { // Equal component masses are ambiguous.
      idleWeight(-80.0f);
      idleWeight(-80.0f);
      idleCup(-80.0f);
    }
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    idleCup(-totalG);
    idleCup(0.0f);
    idleCup(80.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it41_combined_placement_partial_unload_recovers_at_empty_pan() {
  for (bool partial : {false, true}) {
    prepareIdleTare();
    runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
    idleCup(120.0f); // Cup and accessory arrive as one unknown mass.
    CHECK(executeNextScaleCommand());
    idleCup(0.0f);
    if (partial) {
      idleCup(-40.0f);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    }
    idleWeight(-120.0f);
    idleWeight(-120.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    idleCup(-120.0f);
    idleCup(0.0f);
    idleCup(80.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  }
}

void it42_accessory_can_be_readded_after_known_removal() {
  prepareTaredCupAndAccessory(80.0f, 40.0f);
  idleCup(-40.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(40.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  idleWeight(-120.0f);
  idleWeight(-120.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void it43_queued_accessory_removal_survives_fresh_negative_samples() {
  prepareTaredCupAndAccessory(80.0f, 40.0f);
  idleCup(-40.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  idleWeight(-40.2f);
  idleWeight(-40.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(idleTare.requestId != 0);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(fabsf(captureCupTareDiagnostics().weightG - 80.0f) < 0.3f);
}

void it44_interrupted_removal_recovers_after_empty_pan() {
  prepareTaredCupAndAccessory(80.0f, 40.0f);
  idleCup(-40.0f);
  idleWeight(-120.0f); // Both leave before the queued write.
  idleWeight(-120.0f);
  CHECK(executeNextScaleCommand());
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(-120.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(0.0f);
  idleCup(80.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it45_failed_accessory_removal_does_not_retry_on_unknown_zero() {
  prepareTaredCupAndAccessory(80.0f, 40.0f);
  idleCup(-40.0f);
  scale.tareSucceeds = false;
  CHECK(executeNextScaleCommand());
  idleCup(-40.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  idleCup(0.0f); // Failed writes do not certify a new scale zero.
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void it46_accessory_removal_never_uses_idle_tare_during_shot() {
  prepareTaredCupAndAccessory(80.0f, 40.0f);
  startCycle();
  while (executeNextScaleCommand()) {}
  establishPostTareBaseline();
  idleWeight(-40.0f);
  idleWeight(-40.0f);
  idleCup(-40.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void it47_stale_or_disabled_accessory_evidence_never_tares_removal() {
  for (bool enabled : {false, true}) {
    prepareTaredCupAndAccessory(80.0f, 40.0f);
    if (!enabled) {
      runtimeConfig.noScaleBbwMode &= ~IDLE_ACCESSORY_RETARE;
      ++runtimeConfig.revision;
    } else {
      idleWeight(0.0f, runtimeConfig.retareStabilityMaxGapMs + 1);
    }
    idleCup(-40.0f);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    idleCup(-120.0f);
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  }
}

void it48_gradual_accessory_or_cup_unload_waits_for_settled_mass() {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    const float accessoryG = scenario == 2 ? 120.0f : 40.0f;
    prepareTaredCupAndAccessory(80.0f, accessoryG);
    const float finalDropG = scenario == 1 ? 80.0f : accessoryG;
    for (float dropG = 10.0f; dropG < finalDropG; dropG += 10.0f) {
      idleWeight(-dropG);
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    }
    idleCup(-finalDropG);
    if (scenario == 1) {
      idleWeight(-finalDropG);
      CHECK(cupPresenceState() == CupPresenceState::ABSENT);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
    } else {
      CHECK(cupPresenceState() == CupPresenceState::PRESENT);
      CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
    }
  }
}

void it49_cancelled_accessory_removal_rearms_after_return_to_zero() {
  prepareTaredCupAndAccessory(80.0f, 40.0f);
  idleCup(-40.0f);
  CHECK(idleTare.requestId != 0);
  idleCup(0.0f); // Accessory returned before the queued write.
  CHECK(idleTare.requestId == 0);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 2);
  idleCup(-40.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it50_cancelled_accessory_addition_rearms_after_return_to_zero() {
  prepareIdleTare();
  runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(40.0f);
  CHECK(idleTare.requestId != 0);
  idleCup(0.0f); // Addition returned before the queued write.
  CHECK(idleTare.requestId == 0);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls == 1);
  idleCup(40.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void it51_failed_accessory_write_does_not_rearm_from_zero() {
  prepareIdleTare();
  runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
  idleCup(80.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(40.0f);
  scale.tareSucceeds = false;
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  idleCup(40.0f);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(!captureCupTareDiagnostics().weightValid);
}

void cup_fsm_put_back_without_tare_is_present() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  notifyCupPresenceTare();
  holdCupPresenceTransitions(false);
  idleWeight(0.0f);
  idleWeight(-80.0f);
  idleWeight(-81.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(-80.0f);
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
}

void cup_fsm_disconnect_does_not_emit_removed() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  CHECK(session.endReason == EndReason::NONE);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  setScaleConnected(false);
  runLoopAfter(1);
  CHECK(session.endReason != EndReason::CUP_REMOVED);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void cup_fsm_rinse_does_not_freeze_presence() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTare = true;
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  const uint32_t remaining =
      runtimeConfig.rinseDurationMs - elapsedMs(session.rinseStartedAtMs);
  runLoopAfter(remaining);
  CHECK(stopperState == StopperState::READY);
  seedCupPresence(80.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  enableCupStartGuardForTest();
  startCycle();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
}

void r42_weight_below_automation_min_stays_manual() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = -520.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(!session.startedWithScale);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 0);
  reachManualNoScaleState();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
}

void r43_post_tare_baseline_accepts_zero_after_pre_tare_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 236.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(session.awaitingPostTareBaseline);
  CHECK(executeNextScaleCommand());
  publishWeight(236.0f, hostMillis + 1);
  CHECK(session.awaitingPostTareBaseline);
  publishWeight(0.0f, hostMillis + 2);
  CHECK(session.receivedFreshWeightInCycle);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(session.hasWeightAnchor);
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
}

void r54_post_tare_baseline_keeps_weight_control() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 236.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(executeNextScaleCommand());
  publishWeight(236.0f, hostMillis + 1);
  CHECK(session.awaitingPostTareBaseline);
  CHECK(stopperState == StopperState::BREW);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.awaitingPostTareBaseline);
  publishWeight(0.0f, hostMillis + 2);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(session.automaticEnabled);
}

void r66_post_tare_rejected_stream_does_not_enforce_atm() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = true;
  reachReadyFromBoot();
  currentWeight = 236.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(session.awaitingPostTareBaseline);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
  hostAutoScaleWorkerProgress = false;
  for (uint32_t waited = 0; waited <= MAX_AUTOMATION_WEIGHT_AGE_MS + 50U;
       waited += 100U) {
    hostMillis += 100U;
    markScaleWorkerProgress();
    publishWeight(236.0f);
    loop();
    verifySafetyInvariants();
  }
  CHECK(session.awaitingPostTareBaseline);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
  publishWeight(0.0f);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(!session.autoToManualGuardEnforced);
}

void r67_observed_silence_enforces_atm() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = true;
  runtimeConfig.scaleLossFixedMs = 20000;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  hostAutoScaleWorkerProgress = false;
  for (uint32_t waited = 0; waited <= MAX_AUTOMATION_WEIGHT_AGE_MS + 50U;
       waited += 100U) {
    hostMillis += 100U;
    markScaleWorkerProgress();
    loop();
    verifySafetyInvariants();
  }
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(session.autoToManualGuardEnforced);
}

void r68_stable_accepted_weight_does_not_suspend() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = true;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
  hostAutoScaleWorkerProgress = false;
  for (uint32_t waited = 0; waited <= MAX_AUTOMATION_WEIGHT_AGE_MS + 50U;
       waited += 100U) {
    hostMillis += 100U;
    markScaleWorkerProgress();
    publishWeight(20.0f);
    loop();
    verifySafetyInvariants();
  }
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
}

void r69_att_command_harvests_pending_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  scale.weight = 3.25f;
  scale.newWeightAvailableValue = true;
  CHECK(executeNextScaleCommand());
  processScaleWorkerEvents();
  CHECK(fabsf(observedWeight - 3.25f) < 0.001f);
  CHECK(observedWeightIsFresh());
}

void r44_first_shot_after_reconnect_enters_brew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  setScaleConnected(false);
  setScaleConnected(true);
  const uint32_t generation = getScaleLinkSnapshot().connectionGeneration;
  CHECK(generation > 0);
  currentWeight = 236.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  currentWeightConnectionGeneration = generation;
  startCycle();
  CHECK(executeNextScaleCommand());
  publishWeight(0.0f, hostMillis + 1, generation, 10);
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.receivedFreshWeightInCycle);
}

void st01_cycle_elapsed_follows_circuit_immediately() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(cycleShotElapsedMs() == 0U);
  runLoopAfter(200);
  CHECK(cycleShotElapsedMs() >= 200U);
  runLoopAfter(50);
  CHECK(cycleShotElapsedMs() >= 250U);
}

void st02_scale_timer_stop_waits_until_display_catches_internal() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 0;
  startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(session.remoteTimerStarted);
  runLoopAfter(4000);
  publishScaleTimer(3000);

  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);
  CHECK(pendingScaleTimerStop.targetMs == 4000U);

  runLoopAfter(0);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  publishScaleTimer(4000);
  runLoopAfter(0);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
}

void st03_scale_timer_stop_when_display_already_at_internal() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 0;
  startCycle();
  CHECK(executeNextScaleCommand());
  runLoopAfter(4000);
  publishScaleTimer(4200);
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
}

void st04_scale_timer_stop_extra_delay_applies_after_catchup() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 100;
  startCycle();
  CHECK(executeNextScaleCommand());
  runLoopAfter(4000);
  publishScaleTimer(4200);
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  runLoopAfter(99);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  runLoopAfter(1);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
}

void st05_scale_timer_stop_catchup_times_out() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 0;
  startCycle();
  CHECK(executeNextScaleCommand());
  runLoopAfter(4000);
  publishScaleTimer(3000);
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  runLoopAfter(MAX_SCALE_TIMER_STOP_CATCHUP_MS - 1);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  runLoopAfter(1);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
}

void st06_scale_timer_stop_without_valid_timer_is_immediate() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 0;
  startCycle();
  CHECK(executeNextScaleCommand());
  runLoopAfter(4000);
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
}

void st07_scale_timer_stop_waits_for_start_before_queueing_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 0;
  startCycle();
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
  CHECK(!session.remoteTimerStartSettled);

  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);
  CHECK(commandCount(ScaleCommandType::START_TIMER_AND_TARE) == 1);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 0);
  CHECK(queuedCommandAt(0).type == ScaleCommandType::START_TIMER_AND_TARE);

  CHECK(executeNextScaleCommand());
  CHECK(session.remoteTimerStartSettled);
  CHECK(!session.remoteTimerStarted);
  CHECK(scale.tareStartTimerCalls == 0);
  CHECK(scale.stopTimerCalls == 0);

  runLoopAfter(0);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
  CHECK(commandCount(ScaleCommandType::STOP_TIMER) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(scale.stopTimerCalls == 1);
}

void st08_scale_timer_stop_extra_delay_applies_without_valid_timer() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.scaleTimerStopExtraDelayMs = 100;
  startCycle();
  CHECK(executeNextScaleCommand());
  finalizeCycle(EndReason::ACTIVATOR, StopperState::READY);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  runLoopAfter(99);
  CHECK(pendingScaleTimerStop.pending);
  CHECK(!session.stopTimerRequested);

  runLoopAfter(1);
  CHECK(!pendingScaleTimerStop.pending);
  CHECK(session.stopTimerRequested);
}

void rt01_late_cup_triggers_single_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  noteScaleHistory(scale.address(), scale.localName(), false);
  CHECK(setScaleFriendlyName(scale.address(), "Lunar"));
  updateWorkerLinkState();
  CHECK(strcmp(getScaleLinkSnapshot().shotScaleName, "Lunar") == 0);
  startCycle();
  CHECK(strcmp(session.scaleName, "Lunar") == 0);
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(retareWindowOpen());
  CHECK(!session.retareEnded);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  publishStableCupWeight(150.0f, 10);
  CHECK(session.retarePerformed);
  CHECK(session.retareAtMs == 0);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
  CHECK(executeNextScaleCommand());
  CHECK(session.retareAtMs != 0);
  const uint32_t shotAnchorMs = session.circuitClosedAtMs != 0U
                                    ? session.circuitClosedAtMs
                                    : session.startedAtMs;
  CHECK(static_cast<int32_t>(session.retareAtMs - shotAnchorMs) > 0);
  CHECK(session.retareEnded);
  CHECK(!session.bbwProtectionEnded);
  CHECK(scale.tareCalls == 1);
  CHECK(scale.startTimerCalls == 0);
  const uint32_t timerAnchor = session.startedAtMs;
  reachBrewState();
  CHECK(session.startedAtMs == timerAnchor);
}

void shot_scale_name_snapshot_survives_link_changes() {
  resetHarness(false, true);
  reachReadyFromBoot();
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName), "BOOKOO_SC 715097");
  noteScaleHistory(scale.address(), scale.localName(), false);
  CHECK(setScaleFriendlyName(scale.address(), ""));
  updateWorkerLinkState();
  CHECK(strcmp(getScaleLinkSnapshot().shotScaleName, "BOOKOO Themis Mini") == 0);
  startCycle();
  CHECK(strcmp(session.scaleName, "BOOKOO Themis Mini") == 0);
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
  CHECK(strcmp(pendingFinalize.scaleName, "BOOKOO Themis Mini") == 0);
  resetHarness(false, true);
  reachReadyFromBoot();
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName), "BOOKOO_SC U 90210");
  noteScaleHistory(scale.address(), scale.localName(), false);
  updateWorkerLinkState();
  CHECK(strcmp(getScaleLinkSnapshot().shotScaleName, "BOOKOO Themis Ultra") == 0);
  const char *longName = "1234567890123456789012345678901";
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName), longName);
  updateWorkerLinkState();
  CHECK(strlen(getScaleLinkSnapshot().shotScaleName) == SCALE_FRIENDLY_NAME_MAX_LEN);
  CHECK(strncmp(getScaleLinkSnapshot().shotScaleName, longName, SCALE_FRIENDLY_NAME_MAX_LEN) == 0);
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName),
              "12345678901234567890123456789\xc3\xa9");
  updateWorkerLinkState();
  CHECK(strcmp(getScaleLinkSnapshot().shotScaleName, "12345678901234567890123456789") == 0);
  memset(scale.connectedLocalName, '\1', SCALE_FRIENDLY_NAME_MAX_LEN);
  scale.connectedLocalName[SCALE_FRIENDLY_NAME_MAX_LEN] = '\0';
  updateWorkerLinkState();
  char escaped[SCALE_FRIENDLY_NAME_MAX_LEN * 6 + 1] = {};
  CHECK(escapeJsonString(getScaleLinkSnapshot().shotScaleName, escaped, sizeof(escaped)));
  CHECK(strlen(escaped) == SCALE_FRIENDLY_NAME_MAX_LEN * 6);
  CHECK(setScaleFriendlyName(scale.address(), "Kitchen scale"));
  updateWorkerLinkState();
  startCycle();
  CHECK(strcmp(session.scaleName, "Kitchen scale") == 0);
  CHECK(setScaleFriendlyName(scale.address(), "Renamed"));
  updateWorkerLinkState();
  setScaleConnected(false);
  publishControlStatus();
  CHECK(strcmp(publishedControlStatus.cycleScaleName, "Kitchen scale") == 0);
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
  CHECK(strcmp(pendingFinalize.scaleName, "Kitchen scale") == 0);

  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  startCycle();
  CHECK(session.active && !session.startedWithScale);
  CHECK(session.scaleName[0] == '\0');
  copyCString(scale.connectedLocalName, sizeof(scale.connectedLocalName), "Late scale");
  setScaleConnected(true);
  CHECK(setScaleFriendlyName(scale.address(), ""));
  updateWorkerLinkState();
  publishWeight(12.0f, hostMillis + 1);
  CHECK(strcmp(session.scaleName, "Late scale") == 0);
  setScaleConnected(false);
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
  CHECK(strcmp(pendingFinalize.scaleName, "Late scale") == 0);
}

void shot_record_snapshots_target_temp_dose_and_ratio() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  hostMachineShotTargetDeciC = 935;
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
  CHECK(pendingFinalize.targetTempDeciC == 935);
  CHECK(pendingFinalize.activePresetDoseDg ==
        activeShotPreset(presetBank).doseDg);
  session.active = false;
  commitPendingShotLog(pendingFinalize, 36.0f, true,
                       ActualWeightSource::LAST_KNOWN);
  ShotLogRecord record = {};
  CHECK(shotLog.copyNewestFirst(&record, 1) == 1);
  CHECK(record.targetTempDeciC == 935);
  CHECK(record.doseDg == activeShotPreset(presetBank).doseDg);
  // 3600 cg over a 180 dg dose: 2.00.
  CHECK(record.ratioX100 == 3600 * 10 / record.doseDg);
  CHECK(record.ratioX100 == 200);

  // No machine target and no weighed yield: temp and ratio stay missing
  // while the dose snapshot persists.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
  session.active = false;
  commitPendingShotLog(pendingFinalize, 0.0f, false, ActualWeightSource::NONE);
  CHECK(shotLog.copyNewestFirst(&record, 1) == 1);
  CHECK(record.targetTempDeciC == 0);
  CHECK(record.doseDg == activeShotPreset(presetBank).doseDg);
  CHECK(record.ratioX100 == 0);
}

void shot_tare_time_uses_successful_write_and_circuit_clock() {
  // Success, failed write, lost connection, saturated event queue, delayed
  // publication, completion after shot end, another request, and clock wrap.
  for (unsigned mode = 0; mode < 8; ++mode) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = true;
    startCycle();
    CHECK(executeNextScaleCommand());
    CHECK(session.retareAtMs == 0); // Start tare never becomes late tare.
    const uint32_t anchor = mode == 7 ? UINT32_MAX - 999U : 1000U;
    session.startedAtMs = anchor - 900U;
    session.circuitClosedAtMs = anchor;
    hostMillis = anchor + 3200U;
    CHECK(requestRemoteRetare());
    session.retarePerformed = true;
    CHECK(session.retareAtMs == 0);
    ScaleCommand command;
    CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
    scale.tareSucceeds = mode != 1;
    if (mode == 6) ++command.cupWeightRequestId;
    if (mode == 3) {
      ScaleEvent filler;
      filler.type = ScaleEventType::TIMER_STOP_RESULT;
      while (xQueueSend(scaleEventQueue, &filler, 0) == pdTRUE) {}
    }
    hostMillis = anchor + 3400U; // BLE completion, 3.4 s from circuit closure.
    executeScaleCommand(command);
    if (mode == 2) setScaleConnected(false);
    if (mode == 3) {
      ScaleEvent stop;
      stop.type = ScaleEventType::TIMER_STOP_RESULT;
      publishScaleEvent(stop, true); // Replaces the generic fallback.
    }
    const ScaleEvent completed = shotTareResult;
    if (mode == 4 || mode == 5) shotTareResult = ScaleEvent{};
    hostMillis = anchor + 14000U;
    schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
    persistLastShotSnapshot(lastShotFromEndedCycle(EndReason::ACTIVATOR, 14000));
    session.active = false;
    if (mode == 4 || mode == 5) {
      shotTareResult = completed; // Worker result becomes visible after close.
      if (mode == 5) shotTareResult.receivedAtMs = 15001;
      hostMillis = 15100;
    }
    processScaleWorkerEvents();
    const bool expected = mode != 1 && mode != 5 && mode != 6;
    CHECK(pendingFinalize.tareAtDs == (expected ? 34 : SHOT_LOG_METRIC_MISSING));
    CHECK(persistedLastShot.tareElapsedMs == (expected ? 3400U : 0U));
    commitPendingShotLog(pendingFinalize, 36.0f, true, ActualWeightSource::LAST_KNOWN);
    ShotLogRecord record{};
    CHECK(shotLog.copyNewestFirst(&record, 1) == 1);
    CHECK(record.tareAtDs == (expected ? 34 : SHOT_LOG_METRIC_MISSING));
    CHECK(shotLogProjectLastShot(record, {}).tareElapsedMs == (expected ? 3400U : 0U));
  }
}

void rt02_sub_minimum_stable_cup_is_ignored() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.minimumCupWeightG = 10.0f;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  publishStableCupWeight(7.0f, 10);
  CHECK(!session.retarePerformed);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  waitForRetareEnded();
}

void rt03_spike_without_stable_cup_does_not_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.minimumCupWeightG = 10.0f;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  publishWeight(28.0f, hostMillis + 100, 1, 10);
  publishWeight(6.0f, hostMillis + 200, 1, 11);
  publishWeight(5.0f, hostMillis + 300, 1, 12);
  publishWeight(5.0f, hostMillis + 400, 1, 13);
  publishWeight(5.0f, hostMillis + 500, 1, 14);
  CHECK(!session.retarePerformed);
  waitForRetareEnded();
}

void rt04_heavy_cup_does_not_stop_during_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.bbwProtectionEnabled);
  runLoopAfter(1000);
  CHECK(retareWindowOpen());
  const uint32_t heavyAtMs = hostMillis + 50;
  publishWeight(200.0f, heavyAtMs, 1, 20);
  CHECK(bbwWeightStopInhibited());
  CHECK(!session.directStopPending);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(stopperState == StopperState::BREW);
  publishStableCupWeight(200.0f, 30);
  CHECK(session.retarePerformed);
  CHECK(executeNextScaleCommand());
  CHECK(scale.tareCalls >= 1);
}

void rt05_bbw_protection_timeout_enables_stop_without_beep() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  waitForRetareEnded();
  waitForBbwProtectionEnded();
  reachBrewState();
  CHECK(!scaleBeepPending);
  simulateFirstDrops();
  CHECK(scaleBeepPending);
}

void rt06_first_drops_beep_after_bbw_protection_timeout() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  waitForRetareEnded();
  endBbwProtectionForTests();
  CHECK(stopperState == StopperState::BREW);
  simulateFirstDrops();
  CHECK(session.firstDropMs != 0);
  CHECK(scaleBeepPending);
}

void rt07_auto_retare_off_skips_retare_window() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = false;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(session.retareEnded);
  CHECK(!session.bbwProtectionEnded);
  publishStableCupWeight(150.0f, 10);
  CHECK(!session.retarePerformed);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void rt09_coffee_during_retare_beep_on_first_drop_not_at_retare_end() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.firstDropBeep = true;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(retareWindowOpen());
  simulateFirstDrops(0.0f, 10);
  CHECK(session.flowDuringRetare);
  CHECK(session.retareFlowFirstDetectedAtMs != 0);
  CHECK(session.firstDropMs != 0);
  CHECK(session.firstDropMs == session.retareFlowFirstDetectedAtMs);
  CHECK(!session.retarePerformed);
  CHECK(scaleBeepPending);
  waitForRetareEnded();
  CHECK(!session.bbwProtectionEnded);
  CHECK(bbwWeightStopInhibited());
  publishStableCupWeight(150.0f, 20);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
}

void rt10_first_drops_beep_during_bbw_protection() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  reachSessionElapsed(runtimeConfig.retareWindowMs + 100);
  CHECK(retareWindowOpen());  // No coffee yet: a late cup may still tare.
  CHECK(!session.bbwProtectionEnded);
  simulateFirstDrops(0.0f, 10);
  CHECK(session.firstDropMs != 0);
  CHECK(scaleBeepPending);
  CHECK(!retareWindowOpen());
  CHECK(!session.bbwProtectionEnded);
  CHECK(bbwWeightStopInhibited());
  waitForBbwProtectionEnded();
  CHECK(session.bbwProtectionEnded);
  CHECK(!bbwWeightStopInhibited());
}

void rs01_fast_samples_wait_for_min_duration() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.retareStabilityMinDurationMs = 300;
  runtimeConfig.retareStabilitySamples = 3;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t baseMs = hostMillis + 1000U;
  publishWeight(150.0f, baseMs, 1, 10);
  publishWeight(150.0f, baseMs + 40U, 1, 11);
  publishWeight(150.0f, baseMs + 80U, 1, 12);
  CHECK(!session.retarePerformed);
  publishWeight(150.0f, baseMs + 300U, 1, 13);
  CHECK(session.retarePerformed);
}

void rs02_slow_samples_meet_min_duration_at_third_sample() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.retareStabilityMinDurationMs = 300;
  runtimeConfig.retareStabilitySamples = 3;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t baseMs = hostMillis + 1000U;
  publishWeight(150.0f, baseMs, 1, 10);
  publishWeight(150.0f, baseMs + 400U, 1, 11);
  publishWeight(150.0f, baseMs + 800U, 1, 12);
  CHECK(session.retarePerformed);
}

void rs03_broken_streak_before_min_duration_does_not_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.retareStabilityMinDurationMs = 300;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  const uint32_t baseMs = hostMillis + 1000U;
  publishWeight(150.0f, baseMs, 1, 10);
  publishWeight(150.0f, baseMs + 40U, 1, 11);
  publishWeight(150.0f, baseMs + 80U, 1, 12);
  publishWeight(90.0f, baseMs + 120U, 1, 13);
  publishWeight(150.0f, baseMs + 500U, 1, 14);
  CHECK(!session.retarePerformed);
}

void rs04_zero_min_duration_retares_on_sample_count_only() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs =
      minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.retareStabilityMinDurationMs = 0;
  runtimeConfig.retareStabilitySamples = 3;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t baseMs = hostMillis + 1000U;
  publishWeight(150.0f, baseMs, 1, 10);
  publishWeight(150.0f, baseMs + 40U, 1, 11);
  publishWeight(150.0f, baseMs + 80U, 1, 12);
  CHECK(session.retarePerformed);
}

void rs05_coffee_during_min_duration_wait_allows_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.retareStabilityMinDurationMs = 300;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  simulateFirstDrops(0.0f, 20);
  CHECK(session.firstDropMs != 0);
  CHECK(session.flowDuringRetare);
  publishStableCupWeight(150.0f, 30);
  CHECK(session.retarePerformed);
  CHECK(session.retareFlowFirstDetectedAtMs != 0);
}

void rt11_late_retare_records_first_drops_and_keeps_weight_control() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.firstDropBeep = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  advanceToBrew();
  uint32_t keepAliveSequence = 2;
  while (elapsedMs(session.startedAtMs) <
         runtimeConfig.postTareBaselineGraceMs + 500) {
    publishWeight(0.0f, hostMillis, 1, keepAliveSequence++);
    runLoopAfter(200);
  }
  CHECK(retareWindowOpen());
  publishStableCupWeight(150.0f, 10);
  CHECK(session.retarePerformed);
  CHECK(executeNextScaleCommand());
  CHECK(session.awaitingPostTareBaseline);
  CHECK(session.scaleBaselineReady);
  CHECK(fabsf(session.scaleBaselineG) < 0.001f);
  publishWeight(150.0f, hostMillis + 50, 1, 20);
  CHECK(session.awaitingPostTareBaseline);
  CHECK(session.scaleBaselineReady);
  CHECK(fabsf(session.scaleBaselineG) < 0.001f);
  CHECK(session.firstFlow.phase == FirstFlowPhase::TOUCH);
  CHECK(session.firstDropMs == 0);
  const uint32_t settledAtMs = hostMillis + 100;
  publishWeight(0.0f, settledAtMs, 1, 21);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(session.scaleBaselineReady);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  publishWeight(0.35f, settledAtMs + 50, 1, 30);
  publishWeight(0.40f, settledAtMs + 150, 1, 31);
  publishWeight(0.45f, settledAtMs + 250, 1, 32);
  CHECK(session.firstDropMs != 0);
  CHECK(scaleBeepPending);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(!session.flowDuringRetare);
}

void rs06_late_retare_corrects_only_confirmed_first_drop() {
  // Replacement, no replacement, failure, timeout, buffered zero, zero during
  // write, queue overflow, wrong request/cycle, reconnect, zero after end,
  // delayed pre-end evidence, completion after end, missing result, expired
  // effect, stale/future evidence, and wraparound.
  for (unsigned mode = 0; mode < 20; ++mode) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = true;
    runtimeConfig.firstDropBeep = mode != 18;
    runtimeConfig.retareWindowMs = 4000;
    runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
    if (mode == 17) hostMillis = UINT32_MAX - 3100U;
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    const uint32_t start = session.startedAtMs;
    hostMillis = start + 2650U;
    simulateFirstDrops();
    CHECK(session.firstDropMs == start + 2700U);
    const uint32_t original = session.firstDropMs;
    const ShotCurveEvent marker = shotCurveSampler.firstDrop;
    CHECK(shotCurveEventPresent(marker));
    CHECK(hostFirstDropWebhookCount == 1);
    CHECK(session.firstDropsBeepSent == (mode != 18));
    publishStableCupWeight(80.0f, 30);
    CHECK(session.retarePerformed);
    CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
    CHECK(session.firstDropMs == original);
    ScaleCommand command;
    CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
    if (mode == 2) scale.tareSucceeds = false;
    if (mode == 7) ++command.cupWeightRequestId;
    if (mode == 8) ++command.cycleId;
    if (mode == 5) scale.duringTareWrite = [] {
      publishWeight(0.0f, hostMillis + 1, 1, 40);
      CHECK(session.firstDropMs != 0); // Effect without result is insufficient.
    };
    if (mode == 6) {
      ScaleEvent filler;
      filler.type = ScaleEventType::TIMER_STOP_RESULT;
      while (xQueueSend(scaleEventQueue, &filler, 0) == pdTRUE) {}
    }
    if (mode == 10 || mode == 11 || mode == 12) hostMillis = start + 13800U;
    if (mode == 12) hostMillis = start + 14100U;
    executeScaleCommand(command);
    scale.duringTareWrite = nullptr;
    const ScaleEvent result = copyScaleShotTareResult();
    if (mode == 13) {
      shotTareResult = ScaleEvent{};
      ScaleEvent ignored;
      while (xQueueReceive(scaleEventQueue, &ignored, 0) == pdTRUE) {}
      scaleCriticalEventPending = false;
    }
    if (mode == 9) {
      setScaleConnected(false);
      setScaleConnected(true);
    }
    if (mode == 10 || mode == 11 || mode == 12) {
      if (mode == 11) {
        ScaleEvent zero;
        zero.type = ScaleEventType::WEIGHT;
        zero.weightG = 0.0f;
        zero.receivedAtMs = start + 13900U;
        zero.captureSequence = result.captureSequence + 1U;
        zero.packetSequence = 40;
        CHECK(publishScaleEvent(zero, false));
      }
      hostMillis = start + 14000U;
      schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
      persistLastShotSnapshot(lastShotFromEndedCycle(EndReason::ACTIVATOR, 14000));
      homeCycleResult = lastShotFromEndedCycle(EndReason::ACTIVATOR, 14000);
      homeCycleResult.averageFlowValid = true;
      session.active = false;
      session.awaitingPostTareBaseline = false;
      hostMillis = start + 14200U;
    }
    processScaleWorkerEvents();
    if (mode == 19) {
      hostMillis = start + 14000U;
      CHECK(finalizeCycle(EndReason::ACTIVATOR, StopperState::READY));
      setRawPaddle(false);
      runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
      startCycle();
      while (executeNextScaleCommand()) {}
      establishPostTareBaseline();
      simulateFirstDrops();
      const uint32_t newer = session.firstDropMs;
      CHECK(newer != 0 && session.id != result.cycleId);
      CHECK(publishScaleEvent(result, true));
      processScaleWorkerEvents();
      publishWeight(0.0f, hostMillis + 1);
      CHECK(session.firstDropMs == newer);
      CHECK(shotCurveEventPresent(shotCurveSampler.firstDrop));
      continue;
    }
    if (mode != 5 && mode != 11) CHECK(session.firstDropMs == original);
    if (mode == 3 || mode == 14) {
      hostMillis = result.receivedAtMs + session.config.postTareBaselineGraceMs;
      CHECK(expirePostTareBaselineIfNeeded());
    }
    if (mode != 3 && mode != 5 && mode != 11) {
      ScaleEvent zero;
      zero.type = ScaleEventType::WEIGHT;
      zero.weightG = 0.0f;
      zero.receivedAtMs = hostMillis + 10U;
      zero.packetSequence = 40;
      zero.captureSequence = result.captureSequence + 1U;
      if (mode == 4) zero.captureSequence = result.captureSequence;
      if (mode == 15) zero.receivedAtMs = hostMillis - MAX_AUTOMATION_WEIGHT_AGE_MS - 1U;
      if (mode == 16) zero.receivedAtMs = hostMillis + 100U;
      if (mode != 16) hostMillis += 10U;
      CHECK(publishScaleEvent(zero, false));
      processScaleWorkerEvents();
    }
    const bool corrected = mode == 0 || mode == 1 || mode == 5 ||
                           mode == 6 || mode == 11 || mode == 17 || mode == 18;
    CHECK(session.firstDropMs == (corrected ? 0U : original));
    CHECK(shotCurveEventPresent(shotCurveSampler.firstDrop) == !corrected);
    CHECK(session.startedAtMs == start);
    CHECK(session.firstDropObserved);
    observeMachineSenseFromSession(getScaleLinkSnapshot());
    CHECK(machineSense.firstDropSeen);
    publishControlStatus();
    if (session.active)
      CHECK(publishedControlStatus.cycleFirstDropMs == session.firstDropMs);
    if (mode == 10 || mode == 11 || mode == 12) {
      CHECK(publishedControlStatus.homePending);
      CHECK((publishedControlStatus.homeCycle.firstDropElapsedMs == 0) == corrected);
      CHECK(publishedControlStatus.homeCycle.averageFlowValid == !corrected);
      CHECK((publishedControlStatus.homeCycle.tareElapsedMs != 0) == (mode != 12));
      persistLastShotFromFinalize(pendingFinalize, 36, true);
      CHECK((homeCycleResult.firstDropElapsedMs == 0) == corrected);
      CHECK((homeCycleResult.tareElapsedMs != 0) == (mode != 12));
      CHECK(homeCycleResult.durationMs == 14000);
    }
    CHECK(hostFirstDropWebhookCount == 1);
    CHECK(session.firstDropsBeepSent == (mode != 18));
    if (mode == 0 || mode == 17 || mode == 18) {
      simulateFirstDrops(0.0f, 50);
      CHECK(session.firstDropMs != 0 && session.firstDropMs != original);
      CHECK(shotCurveSampler.firstDrop.atMs > marker.atMs);
      CHECK(hostFirstDropWebhookCount == 1);
      applyShotTareResult(result); // Duplicate cannot erase the replacement.
      CHECK(session.firstDropMs != 0);
      CHECK(!session.awaitingPostTareBaseline);
    }
    if (mode == 0 || mode == 1 || mode == 11) {
      if (session.active) {
        hostMillis = start + 14000U;
        schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
        persistLastShotSnapshot(lastShotFromEndedCycle(EndReason::ACTIVATOR, 14000));
        session.active = false;
      }
      CHECK(pendingFinalize.tareAtDs != SHOT_LOG_METRIC_MISSING);
      CHECK((pendingFinalize.firstDropDs == SHOT_LOG_METRIC_MISSING) == (mode != 0));
      CHECK((persistedLastShot.firstDropElapsedMs == 0) == (mode != 0));
      CHECK(persistedLastShot.tareElapsedMs != 0);
      commitPendingShotLog(pendingFinalize, 36.0f, true, ActualWeightSource::LAST_KNOWN);
      ShotLogRecord saved{};
      CHECK(shotLog.copyNewestFirst(&saved, 1) == 1);
      CHECK(saved.firstDropDs == pendingFinalize.firstDropDs);
      CHECK((saved.avgFlowCgS == SHOT_LOG_METRIC_MISSING) == (mode != 0));
      CHECK(shotCurveEventPresent(pendingFinalize.curve.firstDrop) == (mode == 0));
      queueWebhookEnd(pendingFinalize, 36.0f, true);
      CHECK(hostWebhookEndEvent.firstDropValid == (mode == 0));
      CHECK(hostWebhookEndEvent.averageFlowValid == (mode == 0));
    }
  }
}

void rs07_late_retare_preserves_prerequisites_and_deadline() {
  for (unsigned mode = 0; mode < 10; ++mode) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = mode != 0;
    runtimeConfig.autoTare = mode != 1;
    runtimeConfig.timerOnly = mode == 2;
    runtimeConfig.retareWindowMs = 4000;
    runtimeConfig.retareStabilitySamples = 3;
    runtimeConfig.retareStabilityMinDurationMs = 300;
    if (mode == 9) seedCupPresence(80.0f);
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    const uint32_t start = session.startedAtMs;
    hostMillis = start + 2650U;
    simulateFirstDrops();
    const uint32_t original = session.firstDropMs;
    CHECK(original != 0);
    if (mode == 8) {
      ScaleCommand filler;
      filler.type = ScaleCommandType::STOP_TIMER;
      while (xQueueSend(scaleCommandQueue, &filler, 0) == pdTRUE) {}
    }
    if (mode == 4) hostMillis = start + 3700U; // Qualifies exactly at deadline.
    if (mode == 5) hostMillis = start + 3701U;
    if (mode == 6) hostMillis = start + 3900U; // Stability crosses deadline.
    if (mode == 7) hostMillis = start + 3699U;
    if (mode != 3) publishStableCupWeight(80.0f, 30);
    CHECK(session.retarePerformed == (mode == 7));
    CHECK(session.firstDropMs == original);
    CHECK(session.startedAtMs == start);
    CHECK(hostFirstDropWebhookCount == 1);
    if (mode != 7) {
      hostMillis = start + 14000U;
      serviceBbwProtectionPhases();
      schedulePendingShotFinalize(EndReason::ACTIVATOR, 14000);
      CHECK(pendingFinalize.firstDropDs != SHOT_LOG_METRIC_MISSING);
      CHECK(shotCurveEventPresent(pendingFinalize.curve.firstDrop));
    }
  }
}

void rs08_first_drop_while_retare_pending_keeps_stop_protections() {
  for (unsigned mode = 0; mode < 3; ++mode) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = true;
    runtimeConfig.firstDropBeep = mode != 0;
    runtimeConfig.cupProtectionEnabled = true;
    runtimeConfig.stopIfCupRemoved = true;
    runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    runLoopAfter(1000);
    publishStableCupWeight(80.0f, 10);
    CHECK(session.retarePerformed && session.firstDropMs == 0);
    publishWeight(0.0f, hostMillis + 50, 1, 20);
    simulateFirstDrops(0.0f, 21); // First event arrives after command admission.
    const uint32_t original = session.firstDropMs;
    CHECK(original != 0);
    CHECK(executeNextScaleCommand());
    CHECK(session.firstDropMs == original);
    establishPostTareBaseline();
    CHECK(session.firstDropMs == 0);
    CHECK(shouldTrackWeight());
    CHECK(bbwWeightStopInhibited());
    CHECK(!automaticScaleStopDue());
    if (mode == 0) {
      reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
      CHECK(!getRelaySafetySnapshot().closed);
      CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
    } else if (mode == 1) {
      hostMillis = session.startedAtMs + session.config.bbwProtectionMs + 1U;
      serviceBbwProtectionPhases();
      publishStableCupWeight(36.0f, 40);
      CHECK(session.firstDropMs == 0);
      CHECK(automaticScaleStopDue());
    } else {
      publishWeight(-80.0f, hostMillis + 50, 1, 40);
      publishWeight(-80.0f, hostMillis + 100, 1, 41);
      CHECK(session.cupRemovedPending);
      loop();
      CHECK(session.endReason == EndReason::CUP_REMOVED);
      CHECK(!getRelaySafetySnapshot().closed);
    }
  }
}

void rs09_retained_completion_after_reconnect_confirms_zero() {
  for (bool duringWrite : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    setScaleConnected(false);
    setScaleConnected(true);
    runtimeConfig.autoRetare = true;
    publishWeight(0.0f, hostMillis + 50);
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    for (float weight : {0.35f, 0.40f, 0.45f})
      publishWeight(weight, hostMillis + 100);
    CHECK(session.firstDropMs != 0);
    idleCup(80.0f);
    CHECK(session.retarePerformed);
    if (duringWrite) scale.duringTareWrite = [] {
      publishWeight(0.0f, hostMillis + 1);
    };
    ScaleCommand command;
    CHECK(xQueueReceive(scaleCommandQueue, &command, 0) == pdTRUE);
    executeScaleCommand(command);
    scale.duringTareWrite = nullptr;
    CHECK(copyScaleShotTareResult().connectionGeneration > 1);
    CHECK(getScaleLinkSnapshot().disconnectSequence != 0);
    ScaleEvent discarded;
    while (xQueueReceive(scaleEventQueue, &discarded, 0) == pdTRUE) {}
    scaleCriticalEventPending = false;
    processScaleWorkerEvents(); // Only the retained worker completion survives.
    if (!duringWrite) publishWeight(0.0f, hostMillis + 1);
    CHECK(session.retareAtMs != 0);
    CHECK(session.firstDropMs == 0);
    CHECK(!session.awaitingPostTareBaseline);
    CHECK(!shotCurveEventPresent(shotCurveSampler.firstDrop));
  }
}

void rt12_early_retare_then_first_drops_are_recorded() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.firstDropBeep = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  publishStableCupWeight(150.0f, 10);
  CHECK(session.retarePerformed);
  CHECK(executeNextScaleCommand());
  CHECK(session.awaitingPostTareBaseline);
  publishWeight(0.0f, hostMillis + 50, 1, 20);
  CHECK(!session.awaitingPostTareBaseline);
  simulateFirstDrops(0.0f, 30);
  CHECK(session.firstDropMs != 0);
  CHECK(scaleBeepPending);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
}

void rt13_auto_tare_off_skips_automatic_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.autoTare = false;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  publishStableCupWeight(150.0f, 10);
  CHECK(!session.retarePerformed);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 0);
  CHECK(retareWindowOpen());
}

void r45_slew_rejection_emits_specific_debug_code() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runtimeConfig.autoTare = false;
  startCycle();
  publishWeight(900.0f, hostMillis + 1);
  CHECK(debugEventExists(DebugCode::SCALE_SAMPLE_REJECTED_SLEW, 90000, 0));
}

void r46_range_rejection_emits_specific_debug_code() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  publishWeight(-520.0f, hostMillis + 1);
  CHECK(debugEventExists(DebugCode::SCALE_SAMPLE_REJECTED_RANGE, -52000,
                        weightToCentigrams(MIN_AUTOMATION_WEIGHT_G)));
}

void w38_scale_connected_led_tracks_link_and_setting() {
  resetHarness(false, false);
  initializeScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);
  CHECK(runtimeConfig.scaleConnectedLed);
  CHECK(!getScaleLinkSnapshot().connecting);

  scale.connecting = true;
  updateWorkerLinkState();
  CHECK(getScaleLinkSnapshot().connecting);
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::DISCONNECTED);
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);
  hostMillis += SCALE_LED_FAST_BLINK_MS;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);
  hostMillis += SCALE_LED_FAST_BLINK_MS;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);

  runtimeConfig.scaleConnectedLed = false;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);
  runtimeConfig.scaleConnectedLed = true;
  scale.connecting = false;
  updateWorkerLinkState();
  CHECK(!getScaleLinkSnapshot().connecting);
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);

  setScaleConnected(true);
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);
  hostMillis += SCALE_LED_FAST_BLINK_MS;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);

  setScaleConnected(false);
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);

  setScaleConnected(true);
  publishWeight(12.0f);
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);
  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 100;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);
  hostMillis += SCALE_LED_SLOW_BLINK_MS;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);
  hostMillis += SCALE_LED_SLOW_BLINK_MS;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == HIGH);

  runtimeConfig.scaleConnectedLed = false;
  serviceScaleConnectedLed();
  CHECK(hostPinLevel.at(SCALE_CONNECTED_LED_GPIO) == LOW);
}

void s01_shot_log_filters_short_and_rinse() {
  CHECK(!shotLogEligible(EndReason::SHORT_SHOT, 15000));
  CHECK(!shotLogEligible(EndReason::RINSE_COMPLETE, 15000));
  CHECK(!shotLogEligible(EndReason::UNCONFIRMED_START, 60000));
  CHECK(brewEndIsAbandonedStart(EndReason::UNCONFIRMED_START));
  CHECK(!brewEndIsAbandonedStart(EndReason::RINSE_COMPLETE));
  CHECK(!shotLogEligible(EndReason::ACTIVATOR, 9000));
  CHECK(!shotLogEligible(EndReason::ACTIVATOR, 12000));
  CHECK(shotLogEligible(EndReason::ACTIVATOR, 12001));
  CHECK(!shotLogEligible(EndReason::ACTIVATOR, 3001));
  CHECK(!shotLogBbwEligible(false, false, true));
  CHECK(!shotLogBbwEligible(true, true, true));
  CHECK(!shotLogBbwEligible(true, false, false));
  CHECK(shotLogBbwEligible(true, false, true));
  CHECK(!shotLogWeightEligible(2.0f, true));
  CHECK(shotLogWeightEligible(2.001f, true));
  CHECK(!shotLogWeightEligible(2.1f, false));
}

void s01c_mixed_shots_share_stats_and_home_authority() {
  resetHarness(false, true);
  shotLog.clear(false);
  ShotLogRecord record = {};
  record.durationDs = 120;
  record.actualWeightCg = 201;
  CHECK(!shotLogRecordEligible(record));
  record.durationDs = 121;
  record.actualWeightCg = 200;
  CHECK(!shotLogRecordEligible(record));
  record.actualWeightCg = 201;
  CHECK(shotLogRecordEligible(record));

  record.durationDs = 130;
  record.actualWeightCg = 3000;
  record.goalWeightDg = 200;
  record.avgFlowCgS = SHOT_LOG_METRIC_MISSING;
  record.shotType = static_cast<uint8_t>(ShotLogType::AUTO);
  record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::NORMAL_TARGET);
  CHECK(shotLog.append(record, false));  // Outside the newest ten eligible.
  record.shotType = static_cast<uint8_t>(ShotLogType::MANUAL);
  record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::ACTIVATOR);
  for (int i = 0; i < 8; ++i) {
    record.avgFlowCgS = i == 0 ? 100 : SHOT_LOG_METRIC_MISSING;
    if (i == 1) {
      record.shotType = static_cast<uint8_t>(ShotLogType::AUTO);
      record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::SLOW_MAX_TIME);
    }
    CHECK(shotLog.append(record, false));
    record.shotType = static_cast<uint8_t>(ShotLogType::MANUAL);
    record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::ACTIVATOR);
  }
  record.shotType = static_cast<uint8_t>(ShotLogType::AUTO);
  record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::NORMAL_TARGET);
  record.avgFlowCgS = 150;
  record.actualWeightCg = 2200;
  CHECK(shotLog.append(record, false));
  record.avgFlowCgS = SHOT_LOG_METRIC_MISSING;
  record.actualWeightCg = 1800;
  CHECK(shotLog.append(record, false));
  ShotLogRecord eligible = {};
  ShotCurveRecord curve = emptyShotCurveRecord();
  CHECK(copyHomeShot(eligible, &curve));
  const uint32_t newestId = eligible.id;
  CHECK(eligible.actualWeightCg == 1800);
  record.actualWeightCg = 150;
  CHECK(shotLog.append(record, false));  // Historical low-weight exception.
  PersistedLastShot oldManual = {};
  oldManual.valid = true;
  oldManual.cycleId = 77;
  oldManual.durationMs = 20000;
  oldManual.weightValid = true;
  oldManual.currentWeightG = 35.0f;
  persistLastShotSnapshot(oldManual);  // No safe record identity to reuse.
  CHECK(copyHomeShot(eligible, &curve));
  CHECK(eligible.id == newestId);
  CHECK(shotLog.save());
  CHECK(shotLog.load());
  CHECK(copyHomeShot(eligible, &curve));
  CHECK(eligible.id == newestId);
  const ShotStatsView stats = copyShotStats();
  CHECK(stats.shotCount == 10);
  CHECK(stats.bbwCount == 3);
  CHECK(stats.errorPctTenthsSum == 700);
  CHECK(stats.flowCount == 2);
  CHECK(stats.flowCgSx100Sum == 25000);
  CHECK(deleteShotRecord(newestId));
  CHECK(copyHomeShot(eligible, &curve));
  CHECK(eligible.actualWeightCg == 2200);
  CHECK(clearShotLog());
  CHECK(!copyHomeShot(eligible, &curve));
}

void s01e_bbw_error_uses_its_own_ten_shots() {
  resetHarness(false, true);
  shotLog.clear(false);
  ShotLogRecord record = {};
  record.durationDs = 300;
  record.actualWeightCg = 3300;
  record.goalWeightDg = 300;
  record.shotType = static_cast<uint8_t>(ShotLogType::AUTO);
  record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::NORMAL_TARGET);
  for (int i = 0; i < 12; ++i) CHECK(shotLog.append(record, false));
  for (int i = 0; i < 10; ++i) {
    record.stopDetail = static_cast<uint8_t>(
        i % 2 ? ShotLogStopDetail::EXTENDED_MAX_WEIGHT
              : ShotLogStopDetail::SLOW_MAX_TIME);
    CHECK(shotLog.append(record, false));
  }
  const ShotStatsView stats = copyShotStats();
  CHECK(stats.shotCount == 10);
  CHECK(stats.bbwCount == 10);
  CHECK(stats.errorPctTenthsSum == 1000);
}

void s01f_curve_rollover_keeps_stats_windows_and_sorted_joins() {
  resetHarness(false, true);
  ShotLogRecord record = {};
  record.durationDs = 300;
  record.goalWeightDg = 300;
  ShotCurveRecord curve = emptyShotCurveRecord();
  for (uint32_t id = 1; id <= SHOT_LOG_CAPACITY + 10; ++id) {
    record.actualWeightCg = id % 2 ? 3300 : 2700;
    record.avgFlowCgS = id % 2 ? SHOT_LOG_METRIC_MISSING : 100;
    record.shotType = static_cast<uint8_t>(id % 3 ? ShotLogType::MANUAL : ShotLogType::AUTO);
    record.stopDetail = static_cast<uint8_t>(id % 3 ? ShotLogStopDetail::ACTIVATOR : ShotLogStopDetail::NORMAL_TARGET);
    record.sideFlags = shotLogPackRating(0, id % 6);
    resetShotCurveRecord(curve);
    curve.shotId = shotLog.nextRecordId();
    CHECK(appendShotCurvePoint(curve, 0, 0, false) &&
          appendShotCurvePoint(curve, static_cast<int16_t>(curve.shotId), 500, false));
    CHECK(shotLog.append(record, false));
    CHECK(shotCurves.append(curve));
  }
  CHECK(shotLog.save() && shotLog.load() && shotCurves.load());
  CHECK(!shotCurves.containsShotId(10) && shotCurves.containsShotId(11));
  const auto before = copyShotStats();
  CHECK(before.shotCount == 10 && before.bbwCount == 10);
  CHECK(before.errorPctTenthsSum == 1000 && before.flowCount == 5);
  const size_t writes = ShotCurveLog::hostWriteBytes();
  for (const auto sort : {ShotLogSort::Date, ShotLogSort::Rating})
    for (const auto dir : {ShotLogSortDir::Asc, ShotLogSortDir::Desc}) {
      ShotLogRecord rows[SHOT_LOG_CAPACITY];
      const size_t count = copyShotRecords(rows, SHOT_LOG_CAPACITY);
      shotLogSortRecords(rows, count, sort, dir);
      for (size_t page = 0; page < count; page += 10)
        for (size_t n = page; n < page + 10 && n < count; ++n) {
          CHECK(shotCurves.copyByShotId(rows[n].id, curve));
          CHECK(curvePoint(curve, 1).cg == static_cast<int16_t>(rows[n].id));
        }
      const auto stats = copyShotStats();
      CHECK(stats.shotCount == before.shotCount && stats.bbwCount == before.bbwCount);
      CHECK(stats.errorPctTenthsSum == before.errorPctTenthsSum &&
            stats.flowCount == before.flowCount);
    }
  CHECK(ShotCurveLog::hostWriteBytes() == writes);
  CHECK(shotCurves.removeById(SHOT_LOG_CAPACITY + 10));
  CHECK(copyShotStats().shotCount == 10); // Missing curve does not remove its summary.
  ShotLogRecord rows[SHOT_LOG_CAPACITY];
  const size_t count = copyShotRecords(rows, SHOT_LOG_CAPACITY);
  size_t bbwKept = 0;
  for (size_t n = 0; n < count; ++n)
    if (shotLogBbwErrorEligible(rows[n]) && ++bbwKept > 7)
      CHECK(deleteShotRecord(rows[n].id));
  const auto fewer = copyShotStats();
  CHECK(fewer.bbwCount == 7 && fewer.errorPctTenthsSum == 700);
  CHECK(shotLog.save() && shotCurves.flush());
  CHECK(shotLog.load() && shotCurves.load());
  CHECK(copyShotStats().bbwCount == 7);
  CHECK(clearShotLog());
  CHECK(copyShotStats().shotCount == 0 && copyShotStats().bbwCount == 0);
}


void s01d_manual_timer_and_limit_share_settled_finalize() {
  struct Case {
    bool timerOnly;
    EndReason reason;
    ShotLogType type;
    ShotLogCut cut;
  };
  const Case cases[] = {
      {false, EndReason::ACTIVATOR, ShotLogType::MANUAL, ShotLogCut::MANUAL},
      {true, EndReason::WEB_STOP, ShotLogType::TIMER_ONLY, ShotLogCut::MANUAL},
      {false, EndReason::GLOBAL_LIMIT, ShotLogType::MANUAL, ShotLogCut::LIMIT},
      {false, EndReason::RELAY_SAFETY_FAILURE, ShotLogType::MANUAL,
       ShotLogCut::LIMIT},
  };
  for (const Case &sample : cases) {
    resetHarness(false, true);
    shotLog.clear(false);
    session.config = snapshotConfig(runtimeConfig);
    session.config.bbwProtectionMs = 30000;
    session.config.dripDelayMs = 0;
    session.config.timerOnly = sample.timerOnly;
    session.startedWithScale = true;
    session.hasWeightAnchor = true;
    session.lastAcceptedWeightG = 3.0f;
    session.activePresetId = presetBank.activeId;
    shot.automaticBrew = false;
    stopperState = StopperState::BREW;
    schedulePendingShotFinalize(sample.reason, 12001);
    CHECK(pendingFinalize.pending);
    CHECK(pendingFinalize.logEligible);
    pendingShotFinalizeTask();
    ShotLogRecord saved[1] = {};
    CHECK(shotLog.copyNewestFirst(saved, 1) == 1);
    CHECK(shotLogType(saved[0]) == sample.type);
    CHECK(shotLogCut(saved[0]) == sample.cut);
    CHECK(saved[0].actualWeightCg == 300);
  }
}

void s01b_shot_log_stop_detail_names_end_reasons() {
  CHECK(shotLogStopDetailFromEndReason(EndReason::ACTIVATOR, true, false) ==
        ShotLogStopDetail::ACTIVATOR);
  CHECK(shotLogStopDetailFromEndReason(EndReason::WEB_STOP, false, false) ==
        ShotLogStopDetail::WEB_STOP);
  CHECK(shotLogStopDetailFromEndReason(EndReason::PHYSICAL_OVERRIDE, false,
                                       false) ==
        ShotLogStopDetail::PHYSICAL_OVERRIDE);
  CHECK(shotLogStopDetailFromEndReason(EndReason::WEB_HEARTBEAT_TIMEOUT, false,
                                       false) ==
        ShotLogStopDetail::WEB_HEARTBEAT);
  CHECK(shotLogStopDetailFromEndReason(EndReason::GLOBAL_LIMIT, false, false) ==
        ShotLogStopDetail::HARD_LIMIT);
  CHECK(shotLogStopDetailFromEndReason(EndReason::CONFIGURED_WALL_LIMIT, false,
                                       false) ==
        ShotLogStopDetail::WALL_LIMIT);
  CHECK(shotLogStopDetailFromEndReason(EndReason::RELAY_SAFETY_FAILURE, false,
                                       false) ==
        ShotLogStopDetail::RELAY_SAFETY);
  CHECK(shotLogStopDetailFromEndReason(EndReason::WEIGHT_ANOMALY, true, true) ==
        ShotLogStopDetail::WEIGHT_ANOMALY);
  CHECK(shotLogStopDetailFromEndReason(EndReason::SCALE_THRESHOLD, true, true) ==
        ShotLogStopDetail::NORMAL_TARGET);
  CHECK(shotLogStopDetailFromEndReason(EndReason::CUP_REMOVED, true, false) ==
        ShotLogStopDetail::CUP_REMOVED);
  CHECK(strcmp(shotLogStopDetailName(ShotLogStopDetail::ACTIVATOR), "activator") == 0);
  CHECK(strcmp(shotLogStopDetailName(ShotLogStopDetail::HARD_LIMIT),
               "hard_limit") == 0);
  CHECK(strcmp(shotLogStopDetailName(ShotLogStopDetail::WALL_LIMIT),
               "wall_limit") == 0);
}

void s02_shot_log_appends_after_drip_delay() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.logEligible = true;
  pendingFinalize.startedWithScale = false;
  pendingFinalize.finalState = StopperState::MANUAL_NO_SCALE;
  pendingFinalize.endReason = EndReason::ACTIVATOR;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.durationDs = 120;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.activePresetId = presetBank.activeId;
  copyCString(pendingFinalize.activePresetName,
              sizeof(pendingFinalize.activePresetName),
              activeShotPreset(presetBank).name);
  runLoopAfter(pendingFinalize.dripDelayMs);
  CHECK(!pendingFinalize.pending);
  CHECK(shotLog.count() == 0);
}

void s02e_shot_log_appends_auto_bbw_after_drip_delay() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.logEligible = true;
  pendingFinalize.startedWithScale = true;
  pendingFinalize.timerOnly = false;
  pendingFinalize.automaticBrew = true;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.lastKnownWeightG = 36.0f;
  pendingFinalize.finalState = StopperState::BREW;
  pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.durationDs = 120;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.activePresetId = presetBank.activeId;
  copyCString(pendingFinalize.activePresetName,
              sizeof(pendingFinalize.activePresetName),
              activeShotPreset(presetBank).name);
  pendingFinalize.tareAtDs = 34;
  copyCString(pendingFinalize.scaleName, sizeof(pendingFinalize.scaleName),
              "Lunar");
  runLoopAfter(pendingFinalize.dripDelayMs);
  CHECK(!pendingFinalize.pending);
  CHECK(shotLog.count() == 1);
  ShotLogRecord records[1] = {};
  CHECK(shotLog.copyNewestFirst(records, 1) == 1);
  CHECK(records[0].durationDs == 121);
  CHECK(shotLogType(records[0]) == ShotLogType::AUTO);
  CHECK(records[0].stopDetail ==
        static_cast<uint8_t>(ShotLogStopDetail::NORMAL_TARGET));
  CHECK(shotLogCut(records[0]) == ShotLogCut::AUTO);
  CHECK(strcmp(records[0].presetName, pendingFinalize.activePresetName) == 0);
  CHECK(records[0].tareAtDs == 34);
  CHECK(strcmp(records[0].scaleName, "Lunar") == 0);
}

void s02f_shot_log_skips_sub_one_gram_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.cycleId = 5;
  pendingFinalize.logEligible = true;
  pendingFinalize.startedWithScale = true;
  pendingFinalize.timerOnly = false;
  pendingFinalize.automaticBrew = true;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.lastKnownWeightG = 0.5f;
  pendingFinalize.finalState = StopperState::BREW;
  pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.durationDs = 120;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.activePresetId = presetBank.activeId;
  copyCString(pendingFinalize.activePresetName,
              sizeof(pendingFinalize.activePresetName),
              activeShotPreset(presetBank).name);
  runLoopAfter(pendingFinalize.dripDelayMs);
  CHECK(!pendingFinalize.pending);
  CHECK(shotLog.count() == 0);
  CHECK(shotCurves.count() == 0);
  // Home last shot is independent of the 1 g history floor.
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.presetId == presetBank.activeId);
  CHECK(persistedLastShot.presetName[0] != '\0');
  CHECK(persistedLastShot.endedAtUptimeMs != 0);
  CHECK(persistedLastShot.cycleId == 5);
  CHECK(persistedLastShot.weightValid);
  CHECK(fabsf(persistedLastShot.currentWeightG - 0.5f) < 0.001f);
}

void s02g_full_cycle_sub_one_gram_updates_last_shot_not_history() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  const uint32_t rawOnAt = startCycle();
  advanceToBrew();
  currentWeight = 0.5f;
  currentWeightSequence = session.weightSequenceAtStart + 1;
  currentWeightReceivedAtMs = hostMillis;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 0.5f;
  acceptWeightIntoTrajectory(0.5f, hostMillis, currentWeightSequence);
  releaseAtPhysicalDuration(rawOnAt, 12000);
  // Last shot updates immediately at cycle end (before drip delay).
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.weightValid);
  CHECK(fabsf(persistedLastShot.currentWeightG - 0.5f) < 0.001f);
  CHECK(shotLog.count() == 0);
  runLoopAfter(runtimeConfig.dripDelayMs);
  CHECK(shotLog.count() == 0);
  CHECK(shotCurves.count() == 0);
  CHECK(persistedLastShot.valid);
  CHECK(fabsf(persistedLastShot.currentWeightG - 0.5f) < 0.001f);
}

void s02c_shot_curve_samples_on_half_second_grid_and_latches_slow() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ControlStatusSnapshot defaultStatus;
  CHECK(defaultStatus.shotCurve.count == 0);
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  session.active = true;
  session.startedWithScale = true;
  session.hasWeightAnchor = true;
  session.startedAtMs = hostMillis;
  session.circuitClosedAtMs = hostMillis;
  resetShotTrajectory(hostMillis);
  CHECK(shotCurveSampler.count == 0);
  acceptWeightIntoTrajectory(0.2f, hostMillis, 1);
  CHECK(shotCurveSampler.count == 1);
  CHECK(curvePoint(shotCurveSampler, 0).cg == 20);
  hostMillis += 2100;
  acceptWeightIntoTrajectory(8.5f, hostMillis, 2);
  CHECK(shotCurveSampler.count == 2);
  CHECK(curvePoint(shotCurveSampler, 1).cg == 850);
  CHECK(curvePoint(shotCurveSampler, 1).atMs == 2100);
  latchCutLineForTest(false, 12.0f, hostMillis);
  CHECK(shotCurveSampler.extended.atMs == 2100);
  CHECK(shotCurveSampler.extended.weightCg == 1200);
  hostMillis += 4000;
  acceptWeightIntoTrajectory(18.0f, hostMillis, 3);
  session.lastAcceptedWeightG = 18.0f;
  shot.automaticBrew = true;
  session.config.timerOnly = false;
  schedulePendingShotFinalize(EndReason::SLOW_EXTRACTION_MAX_TIME, 12500);
  CHECK(pendingFinalize.curve.count == 3);
  CHECK(pendingFinalize.curve.extended.atMs == 2100);
  CHECK(pendingFinalize.curve.extended.weightCg == 1200);
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.dripDelayMs = 0;
  runLoopAfter(0);
  CHECK(shotLog.count() == 1);
  CHECK(shotCurves.count() == 1);
  ShotCurveRecord curves[1] = {};
  CHECK(shotCurves.copyNewestFirst(curves, 1) == 1);
  CHECK(curves[0].shotId != 0);
  CHECK(curves[0].count == 3);
  CHECK(curvePoint(curves[0], 1).atMs == 2100);

  ShotCurveSampler fullLimit;
  fullLimit.reset(hostMillis);
  for (uint32_t elapsed = 0; elapsed <= HARD_MAX_CIRCUIT_CLOSED_MS; elapsed += 50)
    fullLimit.accept(42.0f, hostMillis + elapsed);
  CHECK(fullLimit.count == SHOT_CURVE_MAX_POINTS);
  CHECK(curvePoint(fullLimit, SHOT_CURVE_MAX_POINTS - 1U).cg == 4200);
}

void s02k_curve_continuity_uses_control_rejections_and_fifo_provenance() {
  for (unsigned boundary = 0; boundary < 9; ++boundary) {
    resetHarness(false, true);
    session.active = session.startedWithScale = true;
    session.config = snapshotConfig(runtimeConfig);
    session.config.avoidAccidentalTouchEnabled = false;
    session.config.autoRetare = false;
    session.weightControlState = WeightControlState::ACTIVE;
    session.ownedConnectionGeneration = getScaleLinkSnapshot().connectionGeneration;
    session.startedAtMs = hostMillis;
    stopperState = StopperState::BREW;
    resetShotTrajectory(hostMillis);
    const uint32_t start = hostMillis;
    CHECK(recordWeightSampleWithProvenance(1, start, 1,
                                           session.ownedConnectionGeneration));
    hostMillis += 100;
    switch (boundary) {
      case 0: CHECK(!recordWeightSample(NAN, hostMillis)); break;
      case 1: CHECK(!recordWeightSample(MAX_AUTOMATION_WEIGHT_G + 1, hostMillis)); break;
      case 2: CHECK(!recordWeightSample(200, hostMillis)); break;
      case 3: armPostTareBaselineWindow(); break;
      case 4: resetWeightTrend(); break;
      case 5: suspendWeightControl(); break;
      default: {
        ScaleEvent event;
        event.receivedAtMs = hostMillis;
        event.weightG = 1;
        if (boundary == 6) event.type = ScaleEventType::REFERENCE_CHANGED;
        if (boundary == 7)
          event.connectionGeneration = session.ownedConnectionGeneration + 1U;
        const size_t publications = boundary == 8 ? SCALE_WEIGHT_EVENT_CAPACITY + 1U : 1U;
        for (size_t n = 0; n < publications; ++n) CHECK(publishScaleEvent(event, false));
        processScaleWorkerEvents();
      }
    }
    if (boundary != 8) {
      CHECK(shotCurveSampler.count == 1);
      for (unsigned n = 0; n < WEIGHT_RECOVERY_CONFIRMATION_SAMPLES; ++n) {
        hostMillis += 50;
        (void)recordWeightSample(boundary == 3 ? 0 : 1, hostMillis);
        if (shotCurveSampler.count > 1) break;
      }
    }
    CHECK(shotCurveSampler.count == 2);
    ShotCurveRecord curve;
    shotCurveSampler.snapshot(curve, 1);
    CHECK(shotCurveBreakBefore(curve, 1));
    CHECK(curvePoint(curve, 0).cg == 100 && curvePoint(curve, 0).atMs == 0);
    hostMillis += 500; // Healthy slow cadence does not add a boundary.
    CHECK(recordWeightSample(1, hostMillis));
    shotCurveSampler.snapshot(curve, 1);
    CHECK(curve.count == 3 && !shotCurveBreakBefore(curve, 2));
    CHECK(curvePoint(curve, 2).atMs == hostMillis - start);
  }
}

void s02l_post_drip_curve_keeps_observations_and_rejection_gaps() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  session.active = session.startedWithScale = true;
  session.config = snapshotConfig(runtimeConfig);
  session.connectionGenerationAtStart = getScaleLinkSnapshot().connectionGeneration;
  const uint32_t generation = session.connectionGenerationAtStart;
  session.startedAtMs = session.circuitClosedAtMs = hostMillis;
  resetShotTrajectory(hostMillis);
  const uint32_t start = hostMillis;
  acceptWeightIntoTrajectory(0, start, 1);
  hostMillis = start + 28000;
  acceptWeightIntoTrajectory(36, hostMillis, 2);
  currentWeight = 36;
  currentWeightSequence = 2;
  currentWeightReceivedAtMs = hostMillis;
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 28000);
  session.active = false;
  stopperState = StopperState::READY;
  const auto sample = [&](float weight, uint32_t elapsed, uint32_t sequence) {
    hostMillis = start + elapsed;
    return recordWeightSampleWithProvenance(weight, hostMillis, sequence, generation);
  };
  CHECK(sample(36.5f, 28100, 3));
  CHECK(sample(37, 28500, 4));
  CHECK(!sample(NAN, 28600, 5));
  CHECK(sample(37.2f, 28800, 6));
  CHECK(pendingFinalize.curve.count == 5);
  CHECK(shotCurveBreakBefore(pendingFinalize.curve, 4));
  CHECK(sample(37.2f, 28800, 6)); // Duplicate must not add an observation.
  CHECK(pendingFinalize.curve.count == 5);
  CHECK(recordWeightSampleWithProvenance(38, start + 28900, 7, generation + 1));
  CHECK(pendingFinalize.curve.count == 5); // Another connection cannot extend this cup's tail.
  CHECK(sample(37.5f, 29000, 7));
  CHECK(pendingFinalize.curve.count == 6);
  CHECK(shotCurveSampler.count == 2 && shot.datapoints == 2);
  currentWeight = 37.5f;
  currentWeightSequence = 7;
  currentWeightReceivedAtMs = hostMillis;
  runLoopAfter(pendingFinalize.dripDelayMs);
  ShotCurveRecord curve;
  CHECK(shotCurves.copyNewestFirst(&curve, 1) == 1);
  CHECK(curve.ended.atMs == 28000 && curve.ended.weightCg == 3750);
  CHECK(curve.count == 6 && curvePoint(curve, 5).atMs == 29000);
  CHECK(curvePoint(curve, 1).cg == 3600 && curvePoint(curve, 5).cg == 3750);
  CHECK(shotCurveBreakBefore(curve, 4));
  CHECK(!pendingFinalize.pending);
  CHECK(sample(38, 33000, 8));
  CHECK(pendingFinalize.curve.count == 6);
}

void s02d_shot_curve_latches_first_drop_fast_and_atm() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  session.active = true;
  session.startedWithScale = true;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 0.5f;
  session.startedAtMs = hostMillis;
  session.circuitClosedAtMs = hostMillis;
  session.autoToManualGuardArmed = true;
  session.autoToManualGuardEnforced = false;
  session.weightControlState = WeightControlState::ACTIVE;
  resetShotTrajectory(hostMillis);
  const uint32_t t0 = hostMillis;
  acceptWeightIntoTrajectory(0.5f, hostMillis, 1);
  hostMillis = t0 + 4557;
  session.lastAcceptedWeightG = 4.0f;
  onFirstDropsDetected({hostMillis, 4.0f});
  CHECK(shotCurveSampler.firstDrop.atMs == 4557);
  CHECK(shotCurveSampler.firstDrop.weightCg == 400);
  hostMillis = t0 + 13313;
  session.lastAcceptedWeightG = 36.0f;
  acceptWeightIntoTrajectory(36.0f, hostMillis, 2);
  latchCutLineForTest(true, 36.0f, hostMillis);
  CHECK(shotCurveSampler.extended.atMs == 13313);
  CHECK(shotCurveSampler.extended.weightCg == 3600);
  hostMillis = t0 + 14027;
  setWeightControlState(WeightControlState::SUSPENDED);
  CHECK(shotCurveEventPresent(shotCurveSampler.atm));
  CHECK(shotCurveSampler.atm.atMs == 14027);
  CHECK(shotCurveSampler.atm.weightCg == 3600);
  hostMillis = t0 + 14543;
  setWeightControlState(WeightControlState::ACTIVE);
  CHECK(shotCurveSampler.atmClearedMs == 14543);
  publishControlStatus();
  ControlStatusSnapshot preciseStatus;
  copyControlStatus(preciseStatus);
  CHECK(preciseStatus.shotCurve.firstDrop.atMs == 4557);
  CHECK(preciseStatus.shotCurve.extended.atMs == 13313);
  CHECK(preciseStatus.shotCurve.atm.atMs == 14027);
  CHECK(preciseStatus.shotCurve.atmClearedMs == 14543);
  hostMillis = t0 + 15157;
  session.lastAcceptedWeightG = 43.7f;
  shot.automaticBrew = true;
  session.config.timerOnly = false;
  schedulePendingShotFinalize(EndReason::FAST_EXTRACTION_MAX_WEIGHT, 15157);
  CHECK(pendingFinalize.curve.firstDrop.atMs == 4557);
  CHECK(pendingFinalize.curve.extended.atMs == 13313);
  CHECK(pendingFinalize.curve.atm.atMs == 14027);
  CHECK(pendingFinalize.curve.atmClearedMs == 14543);
  CHECK(pendingFinalize.curve.ended.atMs == 15157);
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.dripDelayMs = 0;
  runLoopAfter(0);
  CHECK(shotLog.count() == 1);
  CHECK(shotCurves.count() == 1);
  ShotCurveRecord curves[1] = {};
  CHECK(shotCurves.copyNewestFirst(curves, 1) == 1);
  CHECK(curves[0].firstDrop.atMs == 4557);
  CHECK(curves[0].extended.atMs == 13313);
  CHECK(curves[0].atm.atMs == 14027);
  CHECK(curves[0].atmClearedMs == 14543);
  CHECK(curves[0].ended.atMs == 15157);

  session.extractionExtended = false;
  session.autoToManualGuardEnforced = false;
  session.autoToManualGuardArmed = true;
  session.weightControlState = WeightControlState::ACTIVE;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 28.0f;
  resetShotTrajectory(hostMillis);
  const uint32_t t1 = hostMillis;
  acceptWeightIntoTrajectory(28.0f, hostMillis, 10);
  hostMillis = t1 + 12000;
  setWeightControlState(WeightControlState::SUSPENDED);
  CHECK(shotCurveSampler.atm.atMs == 12000);
  CHECK(shotCurveSampler.atmClearedMs == SHOT_LOG_METRIC_MISSING);
  hostMillis = t1 + 18000;
  shot.automaticBrew = true;
  session.config.timerOnly = false;
  schedulePendingShotFinalize(EndReason::AUTO_TO_MANUAL_GUARD, 18000);
  CHECK(pendingFinalize.curve.atm.atMs == 12000);
  CHECK(pendingFinalize.curve.atmClearedMs == SHOT_LOG_METRIC_MISSING);
  CHECK(pendingFinalize.curve.ended.atMs == 18000);
}

void s02h_fast_guard_keeps_sampling_and_settled_weight_replaces_endpoint() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();

  session.active = true;
  session.startedWithScale = true;
  session.config = snapshotConfig(runtimeConfig);
  session.config.timerOnly = false;
  session.config.avoidAccidentalTouchEnabled = false;
  session.weightControlState = WeightControlState::ACTIVE;
  session.ownedConnectionGeneration = 1;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 34.0f;
  session.startedAtMs = hostMillis;
  session.circuitClosedAtMs = hostMillis;
  stopperState = StopperState::BREW;
  resetShotTrajectory(hostMillis);
  const uint32_t t0 = hostMillis;

  CHECK(recordWeightSampleWithProvenance(34.0f, t0, 1, 1));
  hostMillis = t0 + 25800;
  latchCutLineForTest(true, 36.0f, hostMillis);
  CHECK(shotCurveSampler.extended.atMs == 25800);
  const size_t beforeFastSample = shot.datapoints;
  CHECK(recordWeightSampleWithProvenance(38.0f, t0 + 26000, 2, 1));
  CHECK(shot.datapoints == beforeFastSample + 1U);
  CHECK(fabsf(session.lastAcceptedWeightG - 38.0f) < 0.001f);

  hostMillis = t0 + 28000;
  CHECK(recordWeightSampleWithProvenance(40.0f, hostMillis, 3, 1));
  currentWeight = 40.0f;
  currentWeightSequence = 3;
  currentWeightReceivedAtMs = hostMillis;
  shot.automaticBrew = true;
  schedulePendingShotFinalize(EndReason::FAST_EXTRACTION_MAX_WEIGHT, 28000);
  CHECK(pendingFinalize.curve.ended.atMs == 28000);
  CHECK(pendingFinalize.curve.ended.weightCg == 4000);
  CHECK(curvePoint(pendingFinalize.curve, pendingFinalize.curve.count - 1U).cg == 4000);

  session.active = false;
  stopperState = StopperState::READY;
  currentWeight = 42.1f;
  currentWeightSequence = 4;
  currentWeightReceivedAtMs = hostMillis + 1000;
  runLoopAfter(pendingFinalize.dripDelayMs);

  CHECK(!pendingFinalize.pending);
  CHECK(shotCurves.count() == 1);
  ShotCurveRecord curves[1] = {};
  CHECK(shotCurves.copyNewestFirst(curves, 1) == 1);
  CHECK(curves[0].ended.atMs == 28000);
  CHECK(curves[0].ended.weightCg == 4210);
  CHECK(curvePoint(curves[0], curves[0].count - 1U).cg == 4000);
}

void verifySettledCurveEndpointForCut(EndReason reason, bool slowExtended,
                                      uint32_t durationMs) {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();

  session.active = true;
  session.startedWithScale = true;
  session.config = snapshotConfig(runtimeConfig);
  session.config.timerOnly = false;
  session.calibrationEligible = false;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 20.0f;
  session.startedAtMs = hostMillis;
  session.circuitClosedAtMs = hostMillis;
  stopperState = StopperState::BREW;
  resetShotTrajectory(hostMillis);
  const uint32_t t0 = hostMillis;

  acceptWeightIntoTrajectory(20.0f, t0, 1);
  if (slowExtended) {
    hostMillis = t0 + durationMs - 2200U;
    acceptWeightIntoTrajectory(34.0f, hostMillis, 2);
    latchCutLineForTest(false, 34.0f, hostMillis);
  }
  hostMillis = t0 + durationMs;
  acceptWeightIntoTrajectory(36.0f, hostMillis, 3);
  currentWeight = 36.0f;
  currentWeightSequence = 3;
  currentWeightReceivedAtMs = hostMillis;
  shot.automaticBrew = true;
  schedulePendingShotFinalize(reason, durationMs);
  CHECK(pendingFinalize.pending);
  CHECK(pendingFinalize.curve.ended.atMs == durationMs);
  CHECK(pendingFinalize.curve.ended.weightCg == 3600);

  session.active = false;
  stopperState = StopperState::READY;
  currentWeight = 38.4f;
  currentWeightSequence = 4;
  currentWeightReceivedAtMs = hostMillis + 1000U;
  runLoopAfter(pendingFinalize.dripDelayMs);

  CHECK(!pendingFinalize.pending);
  CHECK(shotCurves.count() == 1);
  ShotCurveRecord curves[1] = {};
  CHECK(shotCurves.copyNewestFirst(curves, 1) == 1);
  CHECK(curves[0].ended.atMs == durationMs);
  CHECK(curves[0].ended.weightCg == 3840);
}

void s02i_normal_and_slow_cuts_use_settled_curve_endpoint() {
  // Non-grid normal end verifies that only the event vertex is revised.
  verifySettledCurveEndpointForCut(EndReason::SCALE_THRESHOLD, false, 27957);
  // Both slow-guard exits use the same settled endpoint path.
  verifySettledCurveEndpointForCut(EndReason::SLOW_EXTRACTION_MAX_TIME, true,
                                   28000);
  verifySettledCurveEndpointForCut(EndReason::SLOW_EXTRACTION_MIN_WEIGHT, true,
                                   28000);
}

void s02b_drip_delay_is_snapshotted_and_honors_boundaries() {
  resetHarness(false, true);
  reachReadyFromBoot();
  session.config = snapshotConfig(runtimeConfig);
  session.config.dripDelayMs = 1700;
  session.startedWithScale = true;
  session.config.timerOnly = false;
  shot.automaticBrew = true;
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 12001);
  CHECK(pendingFinalize.dripDelayMs == 1700);

  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.dripDelayMs = 2500;
  pendingFinalize.endedAtMs = hostMillis;

  runtimeConfig.dripDelayMs = 0;
  runLoopAfter(2499);
  CHECK(pendingFinalize.pending);
  CHECK(pendingFinalize.dripDelayMs == 2500);
  runLoopAfter(1);
  CHECK(!pendingFinalize.pending);

  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.dripDelayMs = 0;
  pendingFinalize.endedAtMs = hostMillis;
  runLoopAfter(0);
  CHECK(!pendingFinalize.pending);
}

void s17_new_cycle_commits_pending_log_as_last_known() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.logEligible = true;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.lastKnownWeightG = 43.7f;
  pendingFinalize.startedWithScale = true;
  pendingFinalize.timerOnly = false;
  pendingFinalize.automaticBrew = true;
  pendingFinalize.finalState = StopperState::BREW;
  pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.durationDs = 151;
  pendingFinalize.firstDropDs = 45;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  pendingFinalize.scaleBaselineG = 0.0f;
  pendingFinalize.endedAtMs = hostMillis;
  startCycle();
  CHECK(!pendingFinalize.pending);
  CHECK(session.active);
  CHECK(shotLog.dirty());
  CHECK(shotLog.count() == 1);
  ShotLogRecord records[1] = {};
  CHECK(shotLog.copyNewestFirst(records, 1) == 1);
  CHECK(records[0].actualWeightSource ==
        static_cast<uint8_t>(ActualWeightSource::LAST_KNOWN));
  CHECK(records[0].actualWeightCg == shotLogWeightToCentigrams(43.7f));
  CHECK(records[0].avgFlowCgS == 412);
}

void w90_save_unknown_preset_id_does_not_overwrite_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const ShotPreset *dbl =
      findShotPreset(presetBank, FACTORY_PRESET_ID_DOUBLE_ADAPTIVE);
  CHECK(dbl != nullptr);
  const uint16_t originalGoal = dbl->goalWeightDg;
  const uint8_t originalActive = presetBank.activeId;
  WebCommand save;
  save.type = WebCommandType::PRESET_OP;
  save.requestId = 90;
  save.presetAction = static_cast<uint8_t>(PresetAction::SAVE);
  save.presetId = 99;
  save.config = runtimeConfig;
  save.config.goalWeightDg = 400;
  processWebCommand(save);
  CHECK(hostLastForwardedNetworkCommand.requestId == 90);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::FAILED);
  const ShotPreset *after =
      findShotPreset(presetBank, FACTORY_PRESET_ID_DOUBLE_ADAPTIVE);
  CHECK(after != nullptr);
  CHECK(after->goalWeightDg == originalGoal);
  CHECK(presetBank.activeId == originalActive);
}

void w91_preset_persistence_serializes_and_survives_retry() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand first;
  first.type = WebCommandType::PRESET_OP;
  first.requestId = 91;
  first.presetAction = static_cast<uint8_t>(PresetAction::APPLY);
  first.presetId = FACTORY_PRESET_ID_SINGLE_ADAPTIVE;
  processWebCommand(first);
  CHECK(pendingPresetPersistence.requestId == 91);
  CHECK(pendingPresetPersistence.revision == runtimeConfig.revision);
  CHECK(hostPresetWebhookCount == 0);
  CHECK(hostQuickSettingsWebhookCount == 0);
  CHECK(hostMachineTemperatureRequestCount == 0);

  WebCommand competing = first;
  competing.requestId = 92;
  competing.presetId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  processWebCommand(competing);
  CHECK(presetBank.activeId == FACTORY_PRESET_ID_SINGLE_ADAPTIVE);
  CHECK(hostLastForwardedNetworkCommand.requestId == 92);
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::FAILED);
  CHECK(pendingPresetPersistence.requestId == 91);

  hostRuntimePersistSucceeds = false;
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  CHECK(runtimePersistPending && runtimePersistFailed);
  CHECK(pendingPresetPersistence.requestId == 91);
  CHECK(hostPresetWebhookCount == 0);
  CHECK(hostQuickSettingsWebhookCount == 0);
  CHECK(hostMachineTemperatureRequestCount == 0);
  hostRuntimePersistSucceeds = true;
  runLoopAfter(RUNTIME_PERSIST_RETRY_MS + 1);
  CHECK(!runtimePersistPending && !runtimePersistFailed);
  CHECK(pendingPresetPersistence.requestId == 0);
  CHECK(hostLastFlushedPresets.activeId == FACTORY_PRESET_ID_SINGLE_ADAPTIVE);
  CHECK(hostMachineTemperatureRequestCount == 1);
  CHECK(hostMachineTemperaturePresetId == FACTORY_PRESET_ID_SINGLE_ADAPTIVE);
  CHECK(hostMachineTemperatureGeneration == runtimeConfig.revision);
  CHECK(hostMachineTemperatureTargetDeciC ==
        activeShotPreset(presetBank).lineaMicraBrewTargetDeciC);
  CHECK(hostPresetWebhookCount == 1);
  CHECK(hostQuickSettingsWebhookCount == 1);
}

void w90d_partial_preset_save_writes_only_sent_fields() {
  resetHarness(false, false);
  reachReadyFromBoot();
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  const ShotPreset before = preset;
  WebCommand save;
  save.type = WebCommandType::PRESET_OP;
  save.requestId = 903;
  save.presetAction = static_cast<uint8_t>(PresetAction::SAVE);
  save.presetId = preset.id;
  save.config = runtimeConfig;
  save.config.goalWeightDg = static_cast<uint16_t>(before.goalWeightDg + 10);
  // Unsent fields carry different values: a patch must leave them stored.
  save.config.operationalWallMs = before.operationalWallMs + 1000;
  save.config.timerOnly = before.brewByWeight;
  save.config.fastSideEnabled = !before.fastSideEnabled;
  save.presetRecipeFields = PRESET_RECIPE_GOAL_WEIGHT;
  processWebCommand(save);
  CHECK(hostLastForwardedNetworkCommand.requestId == 903);
  CHECK(hostLastForwardedNetworkCommand.resultState != CommandResultState::FAILED);
  CHECK(preset.goalWeightDg == before.goalWeightDg + 10);
  CHECK(preset.operationalWallMs == before.operationalWallMs);
  CHECK(preset.brewByWeight == before.brewByWeight);
  CHECK(preset.fastSideEnabled == before.fastSideEnabled);
  CHECK(runtimeConfig.goalWeightDg == preset.goalWeightDg);

  // The merged recipe is validated as a whole: an invalid patch changes nothing.
  save.requestId = 904;
  save.config.goalWeightDg = 0;
  processWebCommand(save);
  CHECK(hostLastForwardedNetworkCommand.requestId == 904);
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::FAILED);
  CHECK(preset.goalWeightDg == before.goalWeightDg + 10);
}

void w90c_preset_temperature_presence_and_learning_invalidation() {
  resetHarness(false, false);
  reachReadyFromBoot();
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.lineaMicraBrewTargetDeciC = 930;
  WebCommand save;
  save.type = WebCommandType::PRESET_OP;
  save.requestId = 901;
  save.presetAction = static_cast<uint8_t>(PresetAction::SAVE);
  save.presetId = preset.id;
  save.config = runtimeConfig;
  processWebCommand(save);
  CHECK(preset.lineaMicraBrewTargetDeciC == 930);
  CHECK(hostMachineTemperatureRequestCount == 0);
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);

  auto &learning = bbwLearningBank.forPreset(preset.id, presetBank);
  const uint32_t generation = learning.generation;
  save.requestId = 902;
  save.lineaMicraBrewTargetSpecified = true;
  save.lineaMicraBrewTargetDeciC = 945;
  processWebCommand(save);
  CHECK(preset.lineaMicraBrewTargetDeciC == 945);
  CHECK(hostMachineTemperatureRequestCount == 0);
  CHECK(learning.generation != generation);
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  CHECK(hostMachineTemperatureRequestCount == 1);
  CHECK(hostMachineTemperaturePresetId == preset.id);
  CHECK(hostMachineTemperatureGeneration == runtimeConfig.revision);
  CHECK(hostMachineTemperatureTargetDeciC == 945);

  save.requestId = 903;
  save.lineaMicraBrewTargetDeciC = 1001;
  processWebCommand(save);
  CHECK(preset.lineaMicraBrewTargetDeciC == 945);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::FAILED);
  CHECK(hostMachineTemperatureRequestCount == 1);
}

// A Quick Settings patch as the Integration API builds it: the field mask in
// presetAction, the values in the command's config copy.
WebCommand quickSettingsPatch(
    uint32_t requestId, std::initializer_list<std::pair<QuickSettingField, uint8_t>> changes) {
  WebCommand command;
  command.type = WebCommandType::QUICK_SETTING;
  command.requestId = requestId;
  command.config = runtimeConfig;
  for (const auto &change : changes) {
    setQuickSettingValue(command.config, change.first, change.second);
    command.presetAction |= static_cast<uint8_t>(1U << static_cast<uint8_t>(change.first));
  }
  return command;
}

void hq01_quick_settings_are_session_only() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.noScaleBbwMode |= IDLE_ACCESSORY_RETARE;
  ShotPreset &initialPreset = mutableActiveShotPreset(presetBank);
  initialPreset.slowSideEnabled = true;
  initialPreset.fastSideEnabled = true;
  runtimeConfig.scaleLossEnabled = true;
  runtimeConfig.avoidAccidentalTouchEnabled = true;
  runtimeConfig.cupProtectionEnabled = true;
  applyShotPresetToConfig(initialPreset, runtimeConfig);
  const RuntimeConfig saved = runtimeConfig;
  const ShotPreset savedPreset = activeShotPreset(presetBank);
  const uint32_t persistAttempts = hostRuntimePersistAttempts;
  const uint32_t webhookCount = hostQuickSettingsWebhookCount;
  const struct {
    QuickSettingField field;
    uint8_t value;
  } changes[] = {
      {QuickSettingField::BREW_BY_WEIGHT, 0},
      {QuickSettingField::NO_SCALE_BBW_MODE,
       static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE)},
      {QuickSettingField::AUTO_TO_MANUAL_GUARD, 0},
      {QuickSettingField::SLOW_EXTRACTION_GUARD, 0},
      {QuickSettingField::FAST_EXTRACTION_GUARD, 0},
      {QuickSettingField::AVOID_ACCIDENTAL_TOUCH, 0},
      {QuickSettingField::CUP_PROTECTION, 0},
  };
  uint32_t requestId = 200;
  for (const auto &change : changes) {
    const WebCommand command = quickSettingsPatch(requestId++, {{change.field, change.value}});
    const uint32_t previousRevision = runtimeConfig.revision;
    processWebCommand(command);
    CHECK(runtimeConfig.revision == previousRevision + 1);
    CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::PERSISTED);
    CHECK(pendingPresetPersistence.requestId == 0);
    CHECK(!runtimePersistPending);
    runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  }
  // Live everywhere the shot reads it, mirrored to integrations, never saved.
  const RuntimeConfig effective = effectiveRuntimeConfig();
  CHECK(runtimeConfig.timerOnly && effective.timerOnly);
  CHECK(!effective.fastSideEnabled && !effective.slowSideEnabled);
  CHECK(noScaleBbwModeValue(runtimeConfig.noScaleBbwMode) ==
        static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE));
  CHECK(idleAccessoryRetareEnabled(runtimeConfig.noScaleBbwMode));
  CHECK(!runtimeConfig.scaleLossEnabled);
  CHECK(!runtimeConfig.avoidAccidentalTouchEnabled);
  CHECK(!runtimeConfig.cupProtectionEnabled);
  RecipeSnapshot recipe;
  copyRecipeSnapshot(&recipe);
  const QuickSettingsSnapshot mirrored = quickSettingsSnapshot(recipe);
  CHECK(!mirrored.brewByWeight && !mirrored.cupProtectionEnabled);
  RuntimeConfig published;
  copyRuntimeConfig(&published);
  CHECK(!published.timerOnly && published.cupProtectionEnabled &&
        published.scaleLossEnabled);
  CHECK(hostRuntimePersistAttempts == persistAttempts);
  CHECK(hostQuickSettingsWebhookCount == webhookCount + 7);
  // Settings learns which values are overridden from the published mask.
  publishControlStatus();
  ControlStatusSnapshot overrideStatus;
  copyControlStatus(overrideStatus);
  CHECK(overrideStatus.quickOverrideMask == (1U << QUICK_SETTING_FIELD_COUNT) - 1);
  // ...and the saved value each one hides, which the Settings form shows.
  for (uint8_t index = 0; index < QUICK_SETTING_FIELD_COUNT; ++index)
    CHECK(overrideStatus.quickSaved[index] ==
          quickSettingValue(saved, static_cast<QuickSettingField>(index)));
  CHECK(activeShotPreset(presetBank).brewByWeight == savedPreset.brewByWeight);
  CHECK(activeShotPreset(presetBank).fastSideEnabled);
  CHECK(activeShotPreset(presetBank).slowSideEnabled);

  // An unrelated persisted change flushes the saved values, overrides stay live.
  RuntimeConfig unrelated = runtimeConfig;
  unrelated.dripDelayMs += 100;
  unrelated.revision = runtimeConfig.revision + 1;
  commitLiveRuntimeConfig(unrelated, RUNTIME_PERSIST_REASON_USER);
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  CHECK(hostRuntimePersistAttempts == persistAttempts + 1);
  CHECK(hostLastFlushedRuntime.cupProtectionEnabled == saved.cupProtectionEnabled);
  CHECK(hostLastFlushedRuntime.noScaleBbwMode == saved.noScaleBbwMode);
  CHECK(hostLastFlushedRuntime.timerOnly == saved.timerOnly);
  CHECK(!runtimeConfig.cupProtectionEnabled && runtimeConfig.timerOnly);

  // A Settings edit built on the saved config replaces only the field it changed.
  RuntimeConfig incoming = published;
  incoming.cupProtectionEnabled = false;
  RuntimeConfig edited = runtimeConfig;
  edited.cupProtectionEnabled = incoming.cupProtectionEnabled;
  edited.avoidAccidentalTouchEnabled = incoming.avoidAccidentalTouchEnabled;
  edited.revision = runtimeConfig.revision + 1;
  commitLiveRuntimeConfig(edited, RUNTIME_PERSIST_REASON_USER, &incoming);
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  CHECK(!hostLastFlushedRuntime.cupProtectionEnabled);
  CHECK(hostLastFlushedRuntime.avoidAccidentalTouchEnabled);
  CHECK(!runtimeConfig.avoidAccidentalTouchEnabled);

  WebCommand stale = quickSettingsPatch(++requestId, {{QuickSettingField::CUP_PROTECTION, 1}});
  stale.config.revision = runtimeConfig.revision - 1;
  processWebCommand(stale);
  CHECK(!runtimeConfig.cupProtectionEnabled);
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::FAILED);

  const WebCommand unchanged =
      quickSettingsPatch(++requestId, {{QuickSettingField::CUP_PROTECTION, 0}});
  const uint32_t unchangedRevision = runtimeConfig.revision;
  processWebCommand(unchanged);
  CHECK(runtimeConfig.revision == unchangedRevision);
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::PERSISTED);

  session.active = true;
  const WebCommand active =
      quickSettingsPatch(++requestId, {{QuickSettingField::CUP_PROTECTION, 1}});
  processWebCommand(active);
  CHECK(!runtimeConfig.cupProtectionEnabled);
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::FAILED);
  session.active = false;

  // Switching presets restores every saved value.
  presetBank.activeId = presetBank.presets[1].id;
  RuntimeConfig switched = composeEffectiveConfig(runtimeConfig, presetBank);
  switched.revision = runtimeConfig.revision + 1;
  commitLiveRuntimeConfig(switched, RUNTIME_PERSIST_REASON_USER);
  CHECK(sessionQuickSettings.mask == 0);
  CHECK(runtimeConfig.timerOnly == !activeShotPreset(presetBank).brewByWeight);
  CHECK(runtimeConfig.scaleLossEnabled == saved.scaleLossEnabled);
  CHECK(runtimeConfig.avoidAccidentalTouchEnabled == saved.avoidAccidentalTouchEnabled);
  CHECK(runtimeConfig.noScaleBbwMode == saved.noScaleBbwMode);
}

void hq02_quick_settings_revert_after_duration() {
  resetHarness(false, false);
  reachReadyFromBoot();
  const RuntimeConfig saved = runtimeConfig;
  uint32_t requestId = 300;
  const auto quick = [&](WebCommand command) {
    command.requestId = requestId++;
    processWebCommand(command);
    CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::PERSISTED);
  };
  const auto revert = [&](uint8_t action, uint8_t value) {
    WebCommand command = quickSettingsPatch(0, {});
    command.presetAction = action;
    command.presetId = value;
    quick(command);
  };
  const auto policy = [&](QuickRevertPolicy value) {
    revert(QUICK_REVERT_POLICY_ACTION, static_cast<uint8_t>(value));
  };
  const auto toggle = [&](QuickSettingField field) {
    quick(quickSettingsPatch(0, {{field, !quickSettingValue(runtimeConfig, field)}}));
  };
  const auto restored = [&]() {
    return sessionQuickSettings.mask == 0 &&
           runtimeConfig.scaleLossEnabled == saved.scaleLossEnabled &&
           runtimeConfig.avoidAccidentalTouchEnabled == saved.avoidAccidentalTouchEnabled;
  };
  const uint32_t minute = 60000;

  // One shared timer, restarted by every change, reverts everything together.
  policy(QuickRevertPolicy::MINUTES_15);
  toggle(QuickSettingField::AUTO_TO_MANUAL_GUARD);
  runLoopAfter(10 * minute);
  toggle(QuickSettingField::AVOID_ACCIDENTAL_TOUCH);
  runLoopAfter(10 * minute);
  CHECK(sessionQuickSettings.mask != 0);
  CHECK(quickRevertRemainingMs(sessionQuickSettings, hostMillis) == 5 * minute);
  const uint32_t webhooks = hostQuickSettingsWebhookCount;
  runLoopAfter(5 * minute);
  CHECK(restored());
  CHECK(hostQuickSettingsWebhookCount == webhooks + 1);
  CHECK(quickRevertRemainingMs(sessionQuickSettings, hostMillis) == 0);

  // Keep has no timer; Reset now reverts at once.
  policy(QuickRevertPolicy::KEEP);
  toggle(QuickSettingField::AUTO_TO_MANUAL_GUARD);
  runLoopAfter(61 * minute);
  CHECK(sessionQuickSettings.mask != 0);
  revert(QUICK_REVERT_NOW_ACTION, 0);
  CHECK(restored());

  // A due revert waits for the cycle to end.
  policy(QuickRevertPolicy::MINUTES_15);
  toggle(QuickSettingField::AUTO_TO_MANUAL_GUARD);
  runLoopAfter(15 * minute - 1000);
  uint32_t rawOnAtMs = startCycle();
  runLoopAfter(2000);
  CHECK(session.active && sessionQuickSettings.mask != 0);
  releaseAtPhysicalDuration(rawOnAtMs, 20000);
  CHECK(!session.active && restored());

  // Next shot reverts once a shot made after the change ends.
  policy(QuickRevertPolicy::NEXT_SHOT);
  toggle(QuickSettingField::AUTO_TO_MANUAL_GUARD);
  runLoopAfter(61 * minute);
  CHECK(sessionQuickSettings.mask != 0);
  rawOnAtMs = startCycle();
  runLoopAfter(2000);
  CHECK(sessionQuickSettings.mask != 0);
  releaseAtPhysicalDuration(rawOnAtMs, 20000);
  CHECK(restored());
  CHECK(sessionQuickSettings.revertPolicy == QuickRevertPolicy::NEXT_SHOT);
}

// A patch of several settings goes live as one change, or not at all.
void hq03_quick_settings_patch_is_one_change() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.cupProtectionEnabled = true;
  runtimeConfig.avoidAccidentalTouchEnabled = true;
  const uint8_t scaleLoss = quickSettingValue(runtimeConfig, QuickSettingField::AUTO_TO_MANUAL_GUARD);
  const uint32_t revision = runtimeConfig.revision;
  const uint32_t webhooks = hostQuickSettingsWebhookCount;
  processWebCommand(quickSettingsPatch(400, {{QuickSettingField::CUP_PROTECTION, 0},
                                             {QuickSettingField::NO_SCALE_BBW_MODE, 3}}));
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::FAILED);
  CHECK(runtimeConfig.cupProtectionEnabled && runtimeConfig.revision == revision);
  CHECK(sessionQuickSettings.mask == 0 && hostQuickSettingsWebhookCount == webhooks);

  processWebCommand(quickSettingsPatch(401, {{QuickSettingField::CUP_PROTECTION, 0},
                                             {QuickSettingField::AVOID_ACCIDENTAL_TOUCH, 0},
                                             {QuickSettingField::AUTO_TO_MANUAL_GUARD, scaleLoss}}));
  CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::PERSISTED);
  CHECK(!runtimeConfig.cupProtectionEnabled && !runtimeConfig.avoidAccidentalTouchEnabled);
  CHECK(runtimeConfig.revision == revision + 1);
  CHECK(hostQuickSettingsWebhookCount == webhooks + 1);
  CHECK(sessionQuickSettings.mask ==
        ((1U << static_cast<uint8_t>(QuickSettingField::CUP_PROTECTION)) |
         (1U << static_cast<uint8_t>(QuickSettingField::AVOID_ACCIDENTAL_TOUCH))));
}

void s20_last_good_shot_advances_independently() {
  resetHarness(false, false);
  PersistedLastShot good = {};
  good.valid = true;
  good.cycleId = 1;
  good.durationMs = 13000;
  good.weightValid = true;
  good.currentWeightG = 36.0f;
  good.shotLogId = 91;
  good.rating = 4;
  persistLastShotSnapshot(good);
  serviceShotStorePersistence();
  CHECK(lastShotStore.load());
  nextCycleId = 1;

  PersistedLastShot shortShot = good;
  shortShot.cycleId = nextCycleId++;
  shortShot.durationMs = 12000;
  shortShot.currentWeightG = 1.0f;
  shortShot.shotLogId = 0;
  shortShot.rating = 0;
  persistLastShotSnapshot(shortShot);
  PendingShotFinalize collision = {};
  collision.cycleId = shortShot.cycleId;
  collision.durationDs = 120;
  collision.bootId = currentBootId();
  commitPendingShotLog(collision, 1.0f, true,
                       ActualWeightSource::POST_DRIP);
  CHECK(persistedLastShot.cycleId == 1);
  CHECK(persistedLastShot.durationMs == 12000);
  CHECK(persistedLastShot.shotLogId != 0);
  CHECK(persistedLastShot.shotLogId != 91);
  CHECK(persistedLastGoodShot.durationMs == 13000);
  CHECK(persistedLastGoodShot.shotLogId == 91);
  CHECK(persistedLastGoodShot.rating == 4);
  CHECK(!clearLastShot());
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastGoodShot.durationMs == 13000);
  CHECK(lastShotStore.getGood().shotLogId == 91);
}

void s21_controller_started_retries_after_a_shot_store_persist() {
  resetHarness(false, false);
  reachReadyFromBoot();
  shotLog.load();
  CHECK(shotLog.clear(false));
  controllerStartedPending = true;
  g_hostFlashIoMutexAvailable = false;
  serviceShotStorePersistence();
  CHECK(shotLog.dirty());
  CHECK(controllerStartedPending);
  CHECK(hostControllerStartedWebhookCount == 0);

  static thread_local bool shotStoreLocked = false;
  static bool buildGuardCalled = false;
  shotStoreLocked = false;
  buildGuardCalled = false;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &shotStoreMutex) shotStoreLocked = acquired;
  };
  hostControllerStartedWebhookBuildGuard = []() {
    buildGuardCalled = true;
    return !shotStoreLocked;
  };
  g_hostFlashIoMutexAvailable = true;
  hostMillis = shotStorePersistRetryAtMs;
  serviceShotStorePersistence();
  hostControllerStartedWebhookBuildGuard = nullptr;
  TaskMutex::hostObserver = nullptr;
  CHECK(buildGuardCalled);
  CHECK(!shotStoreLocked);
  CHECK(!shotLog.dirty());
  CHECK(!controllerStartedPending);
  CHECK(hostControllerStartedWebhookCount == 1);
  CHECK(hostControllerStartedWebhookEvent.bootId == currentBootId());
}

void s21_ip_change_reports_webhook() {
  resetHarness(false, false);
  reachReadyFromBoot();
  CHECK(hostIpChangedWebhookCount == 0);
  reportStationIpChange("192.168.1.42");
  CHECK(hostIpChangedWebhookCount == 1);
  CHECK(strcmp(hostIpChangedWebhookAddress, "192.168.1.42") == 0);
  reportStationIpChange(nullptr);
  reportStationIpChange("");
  CHECK(hostIpChangedWebhookCount == 1);
  // Deferred while the control path is critical; reported once it clears.
  latchControlCriticalLogging();
  reportStationIpChange("192.168.1.43");
  CHECK(hostIpChangedWebhookCount == 1);
  publishControlCriticalLoggingState();
  reportStationIpChange("192.168.1.43");
  CHECK(hostIpChangedWebhookCount == 2);
  CHECK(strcmp(hostIpChangedWebhookAddress, "192.168.1.43") == 0);
}

void s03_shot_log_clear_empties_records() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 100;
  CHECK(shotLog.append(record));
  CHECK(shotLog.count() == 1);
  CHECK(shotLog.clear());
  CHECK(shotLog.count() == 0);
}

void s03b_good_shot_records_stats_and_history() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  historyLog.clear();
  const uint32_t rawOnAt = startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishWeight(20.0f);
  const uint32_t duration = runtimeConfig.bbwProtectionMs + 9001U;
  releaseAtPhysicalDuration(rawOnAt, duration);
  publishWeight(36.0f, hostMillis + 1);
  runLoopAfter(runtimeConfig.dripDelayMs + 100U);
  // A good shot lands in the stats log and the activation history alike.
  CHECK(shotLog.count() == 1);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::SHOT));
  CHECK(page.records[0].durationDs > runtimeConfig.bbwProtectionMs / 100U);
  // Scale readings arrived during the cycle, so the weight flag stays clear.
  CHECK((page.records[0].flags & HISTORY_FLAG_NO_WEIGHT) == 0);
  // The control loop's deferred flush servant already persisted the record.
  CHECK(!historyLog.dirty());
}

void s03c_short_activation_is_other_in_history_only() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  historyLog.clear();
  const uint32_t rawOnAt = startCycle();
  advanceToBrew();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.bbwProtectionMs - 500U);
  runLoopAfter(runtimeConfig.dripDelayMs + 100U);
  // At or below the protection window an activation is not a shot: history
  // keeps it as `other`, stats skip it entirely.
  CHECK(shotLog.count() == 0);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::OTHER));
}

void s03d_rinse_is_rinse_in_history_only() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  historyLog.clear();
  const uint32_t rawOnAt = startCycle();
  CHECK(executeNextScaleCommand());
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  runLoopAfter(runtimeConfig.rinseDurationMs + 1);
  CHECK(stopperState == StopperState::READY);
  CHECK(shotLog.count() == 0);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::RINSE));
}

void s03e_manual_over_protection_is_shot_in_history_only() {
  resetHarness(false, false);
  reachReadyFromBoot();
  shotLog.clear();
  historyLog.clear();
  const uint32_t rawOnAt = startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.bbwProtectionMs + 9500U);
  runLoopAfter(runtimeConfig.dripDelayMs + 100U);
  // Intent qualifies the activation diary, but no yield enters Stats.
  CHECK(shotLog.count() == 0);
  CHECK(historyLog.count() == 1);
  HistoryPage page;
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.records[0].type == static_cast<uint8_t>(HistoryType::SHOT));
  // A no-scale manual shot never registered weight, so the flag is set.
  CHECK((page.records[0].flags & HISTORY_FLAG_NO_WEIGHT) != 0);
}

void s03f_abandoned_start_records_nothing() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  historyLog.clear();
  appendActivationHistory(EndReason::UNCONFIRMED_START, false, 5000);
  CHECK(brewEndIsAbandonedStart(EndReason::UNCONFIRMED_START));
  CHECK(shotLog.count() == 0);
  CHECK(historyLog.count() == 0);
  CHECK(historyLog.dirty() == false);
}

void s03g_stats_threshold_follows_protection_parameter() {
  // Default protection (12 s): the former fixed 10 s window no longer
  // qualifies, and exactly the protection time does not either.
  CHECK(!shotLogEligible(EndReason::ACTIVATOR, 10000));
  CHECK(!shotLogEligible(EndReason::ACTIVATOR, 12000));
  CHECK(shotLogEligible(EndReason::ACTIVATOR, 12100));
  // History follows the configurable protection window, independently of Stats.
  CHECK(!shotLogEligible(EndReason::ACTIVATOR, 3100));
  CHECK(historyTypeFromCycle(false, 12000, 12000) == HistoryType::OTHER);
  CHECK(historyTypeFromCycle(false, 12001, 12000) == HistoryType::SHOT);
  CHECK(historyTypeFromCycle(false, 3101, 3100) == HistoryType::SHOT);
  CHECK(historyTypeFromCycle(true, 12001, 12000) == HistoryType::RINSE);
}

void s14_last_shot_persists_manual_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  CHECK(!persistedLastShot.valid);
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs + 100);
  // Manual / short cycles update home last shot; they still skip history.
  CHECK(persistedLastShot.valid);
  runLoopAfter(runtimeConfig.dripDelayMs);
  CHECK(persistedLastShot.valid);
  CHECK(shotLog.count() == 0);
}

void s14c_last_shot_keeps_no_scale_duration() {
  resetHarness(false, false);
  reachReadyFromBoot();
  shotLog.clear();
  const uint32_t rawOnAt = startCycle();
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  releaseAtPhysicalDuration(rawOnAt, 12500);
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.durationMs >= 12000);
  CHECK(!persistedLastShot.weightValid);
  CHECK(shotLog.count() == 0);
}

void s14b_rinse_does_not_overwrite_last_shot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  PersistedLastShot retained = {};
  retained.valid = true;
  retained.cycleId = 42;
  retained.weightValid = true;
  retained.currentWeightG = 36.0f;
  persistLastShotSnapshot(retained);
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE ||
        stopperState == StopperState::READY);
  runLoopAfter(runtimeConfig.rinseDurationMs + runtimeConfig.dripDelayMs);
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.cycleId == 42);
  CHECK(fabsf(persistedLastShot.currentWeightG - 36.0f) < 0.001f);
}

void s14d_web_stop_during_rinse_does_not_overwrite_last_shot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  PersistedLastShot retained = {};
  retained.valid = true;
  retained.cycleId = 42;
  retained.weightValid = true;
  retained.currentWeightG = 36.0f;
  persistLastShotSnapshot(retained);
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  processWebCommand(webControlCommand(WebCommandType::STOP));
  CHECK(session.endReason == EndReason::WEB_STOP);
  CHECK(!session.active);
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.cycleId == 42);
  CHECK(fabsf(persistedLastShot.currentWeightG - 36.0f) < 0.001f);
}

void s15b_cup_off_after_end_keeps_last_known_actual() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  PersistedLastShot retained = {};
  retained.valid = true;
  retained.cycleId = 9;
  retained.weightValid = true;
  retained.currentWeightG = 42.1f;
  persistLastShotSnapshot(retained);

  currentWeight = 0.0f;
  currentWeightSequence = 8;
  currentWeightReceivedAtMs = hostMillis + 1;
  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.cycleId = 9;
  pendingFinalize.logEligible = true;
  pendingFinalize.startedWithScale = true;
  pendingFinalize.timerOnly = false;
  pendingFinalize.automaticBrew = true;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.lastKnownWeightG = 42.1f;
  pendingFinalize.finalState = StopperState::BREW;
  pendingFinalize.endReason = EndReason::FAST_EXTRACTION_MAX_WEIGHT;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.durationDs = 280;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.weightOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.endedWeightSequence = 7;
  pendingFinalize.extractionGuardEnabled = true;
  pendingFinalize.extractionExtended = true;
  runLoopAfter(pendingFinalize.dripDelayMs);
  CHECK(!pendingFinalize.pending);
  CHECK(shotLog.count() == 1);
  ShotLogRecord records[1] = {};
  CHECK(shotLog.copyNewestFirst(records, 1) == 1);
  CHECK(records[0].actualWeightSource ==
        static_cast<uint8_t>(ActualWeightSource::LAST_KNOWN));
  CHECK(records[0].actualWeightCg == shotLogWeightToCentigrams(42.1f));
  CHECK(fabsf(persistedLastShot.currentWeightG - 42.1f) < 0.001f);
}

void s15c_last_shot_prefers_last_accepted_over_cup_off() {
  resetHarness(false, true);
  reachReadyFromBoot();
  session.active = true;
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 42.1f;
  session.startedAtMs = hostMillis > 25000 ? hostMillis - 25000 : 1;
  session.circuitClosedAtMs = session.startedAtMs;
  session.firstDropMs = session.startedAtMs + 4500;
  session.weightSequenceAtStart = 1;
  currentWeight = 0.0f;
  currentWeightSequence = 5;
  currentWeightReceivedAtMs = hostMillis;
  persistLastShotSnapshot(lastShotFromEndedCycle(EndReason::FAST_EXTRACTION_MAX_WEIGHT, 25000));
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.weightValid);
  CHECK(fabsf(persistedLastShot.currentWeightG - 42.1f) < 0.001f);
  CHECK(persistedLastShot.firstDropElapsedMs == 4500);
}

void s15_last_shot_persists_after_drip_when_eligible() {
  resetHarness(false, true);
  reachReadyFromBoot();
  currentWeight = 36.4f;
  currentWeightSequence = 5;
  currentWeightReceivedAtMs = hostMillis + 1;
  pendingFinalize = PendingShotFinalize{};
  preparePendingBbwForTest();
  pendingFinalize.cycleId = 7;
  pendingFinalize.logEligible = true;
  pendingFinalize.startedWithScale = true;
  pendingFinalize.timerOnly = false;
  pendingFinalize.automaticBrew = true;
  pendingFinalize.finalState = StopperState::BREW;
  pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
  pendingFinalize.durationDs = 120;
  pendingFinalize.goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  pendingFinalize.bootId = currentBootId();
  pendingFinalize.endedAtMs = hostMillis;
  pendingFinalize.endedWeightSequence = 4;
  runLoopAfter(pendingFinalize.dripDelayMs);
  CHECK(!pendingFinalize.pending);
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.cycleId == 7);
  CHECK(fabsf(persistedLastShot.currentWeightG - 36.4f) < 0.001f);
}

void s16_last_shot_clear_empties_snapshot() {
  resetHarness(false, false);
  ShotLogRecord record = {};
  record.durationDs = 130;
  record.actualWeightCg = 3600;
  CHECK(shotLog.append(record));
  CHECK(clearLastShot());
  CHECK(shotLog.count() == 0);
  ShotCurveRecord curve = emptyShotCurveRecord();
  CHECK(!copyHomeShot(record, &curve));
}

void s16b_factory_reset_hides_last_shot_on_status() {
  resetHarness(false, false);
  PersistedLastShot retained = {};
  retained.valid = true;
  retained.cycleId = 11;
  persistLastShotSnapshot(retained);
  CHECK(lastShotStore.clear());
  clearLastShotRuntimeState();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(!persistedLastShot.valid);
  CHECK(!status.lastShot.valid);
}

void s18_last_shot_keeps_no_scale_guard_from_cycle() {
  resetHarness(false, true);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  advanceToBrew();
  ScaleEvent fresh{};
  fresh.type = ScaleEventType::WEIGHT;
  fresh.weightG = 18.0f;
  fresh.receivedAtMs = hostMillis;
  fresh.packetSequence = ++currentWeightSequence;
  publishScaleEvent(fresh, false);
  processScaleWorkerEvents();
  acceptWeightIntoTrajectory(18.0f, hostMillis, currentWeightSequence);
  session.lastAcceptedWeightG = 18.0f;
  session.hasWeightAnchor = true;
  releaseAtPhysicalDuration(rawOnAt, 12000);
  runLoopAfter(runtimeConfig.dripDelayMs);
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.noScaleShotGuardEnabled);
  CHECK(persistedLastShot.noScaleShotGuardArmed);
  CHECK(noScaleShotGuardArmed);
  consumeNoScaleShotGuard();
  CHECK(!noScaleShotGuardArmed);
  CHECK(persistedLastShot.noScaleShotGuardArmed);
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(!status.noScaleShotGuardArmed);
  CHECK(status.lastShot.noScaleShotGuardArmed);
}

void n01_wall_clock_tracks_utc_from_anchor() {
  g_wallClock.reset();
  g_wallClock.setSyncing("pool.ntp.org", 1000);
  g_wallClock.queueSyncFromCallback(1'700'000'000U);
  CHECK(g_wallClock.applyPendingSync(1000));
  CHECK(g_wallClock.nowUtcSec(1000) == 1'700'000'000U);
  CHECK(g_wallClock.nowUtcSec(6000) == 1'700'000'005U);
}

void n01c_wall_clock_cancel_syncing_restores_anchor() {
  g_wallClock.reset();
  g_wallClock.setSyncing("pool.ntp.org", 1000);
  g_wallClock.queueSyncFromCallback(1'700'000'000U);
  CHECK(g_wallClock.applyPendingSync(1000));
  CHECK(g_wallClock.synced());
  g_wallClock.setSyncing("time.google.com", 5000);
  CHECK(g_wallClock.snapshot(5000).state == TimeSyncState::SYNCING);
  CHECK(g_wallClock.snapshot(5000).lastSyncUtcSec == 1'700'000'000U);
  g_wallClock.cancelSyncing();
  const TimeStatusSnapshot restored = g_wallClock.snapshot(5000);
  CHECK(restored.state == TimeSyncState::SYNCED);
  CHECK(restored.lastSyncUtcSec == 1'700'000'000U);
  CHECK(g_wallClock.synced());
  CHECK(g_wallClock.nowUtcSec(5000) == 1'700'000'004U);
  CHECK(restored.consecutiveFailures == 0);

  // A callback racing with an RF gate must be discardable without losing the
  // last good wall-clock anchor.
  g_wallClock.queueSyncFromCallback(1'800'000'000U);
  g_wallClock.cancelPendingSync();
  CHECK(!g_wallClock.applyPendingSync(5001));
  CHECK(g_wallClock.synced());
  CHECK(g_wallClock.nowUtcSec(5001) == 1'700'000'004U);
  g_wallClock.markFailed(5001, 5);
  CHECK(g_wallClock.snapshot(5001).lastSyncUtcSec == 1'700'000'000U);

  g_wallClock.reset();
  g_wallClock.setSyncing("pool.ntp.org", 100);
  g_wallClock.cancelSyncing();
  CHECK(g_wallClock.snapshot(100).state == TimeSyncState::OFF);
  CHECK(g_wallClock.snapshot(100).lastSyncUtcSec == 0);
  CHECK(!g_wallClock.synced());
}

void n01d_wall_clock_keeps_time_through_failures_and_rebase() {
  g_wallClock.reset();
  g_wallClock.queueSyncFromCallback(1'700'000'000U);
  CHECK(g_wallClock.applyPendingSync(1000));
  g_wallClock.setSyncing("pool.ntp.org", 3601000);
  CHECK(g_wallClock.nowUtcSec(3601000) == 1'700'003'600U);
  g_wallClock.markFailed(3611000, 5);
  CHECK(g_wallClock.synced());
  CHECK(g_wallClock.snapshot(3611000).utcSec == 1'700'003'610U);
  CHECK(g_wallClock.snapshot(3611000).state == TimeSyncState::FAILED);
  g_wallClock.markDisabled();
  CHECK(g_wallClock.nowUtcSec(3621000) == 1'700'003'620U);
  const uint32_t later = 1000U + (NTP_ANCHOR_REBASE_SEC + 5U) * 1000U;
  const uint32_t expected = 1'700'000'000U + NTP_ANCHOR_REBASE_SEC + 5U;
  g_wallClock.rebaseAnchor(later);
  CHECK(g_wallClock.nowUtcSec(later) == expected);
  CHECK(g_wallClock.nowUtcSec(later + 30U * 86400U * 1000U) ==
        expected + 30U * 86400U);
  CHECK(g_wallClock.snapshot(later).lastSyncUtcSec == 1'700'000'000U);
  CHECK(g_wallClock.snapshot(later).lastSyncAgeMs > NTP_STALE_AFTER_MS);
}

void n01b_wall_clock_survives_millis_wrap() {
  g_wallClock.reset();
  g_wallClock.setSyncing("pool.ntp.org", 1000);
  g_wallClock.queueSyncFromCallback(1'700'000'000U);
  CHECK(g_wallClock.applyPendingSync(1000));
  constexpr uint32_t afterWrap = 50;
  const uint32_t elapsedMs = static_cast<uint32_t>(afterWrap - 1000U);
  CHECK(g_wallClock.nowUtcSec(afterWrap) ==
        1'700'000'000U + elapsedMs / 1000U);
  CHECK(g_wallClock.synced());
  const TimeStatusSnapshot snap = g_wallClock.snapshot(afterWrap);
  CHECK(snap.utcSec == 1'700'000'000U + elapsedMs / 1000U);
  CHECK(snap.lastSyncAgeMs == elapsedMs);
  CHECK(snap.state == TimeSyncState::STALE);

  g_wallClock.reset();
  g_wallClock.markFailed(UINT32_MAX - 1000U, 5);
  CHECK(g_wallClock.snapshot(UINT32_MAX - 500U).nextRetryInMs == 14500U);
  CHECK(g_wallClock.snapshot(100U).nextRetryInMs == 13899U);
}

void n02_ntp_hostname_validation() {
  CHECK(validNtpHostname("pool.ntp.org"));
  CHECK(validNtpHostname("time.google.com"));
  CHECK(!validNtpHostname(""));
  CHECK(!validNtpHostname("-bad.example"));
  CHECK(!validNtpHostname("bad space"));
}

void n03_unsynced_retry_is_fifteen_seconds() {
  CHECK(NTP_UNSYNCED_RETRY_MS == 15'000U);
  CHECK(ntpRetryDelayMs(0) == NTP_UNSYNCED_RETRY_MS);
  CHECK(ntpRetryDelayMs(5) == NTP_UNSYNCED_RETRY_MS);
}

void rf01_coex_is_always_bt() {
  CHECK(ensureRfCoexBt());
  CHECK(snapshotRfCoexPreference() == RfCoexPreference::BT);
  CHECK(strcmp(rfCoexPreferenceName(RfCoexPreference::BT), "BT") == 0);
  CHECK(strcmp(rfCoexPreferenceName(RfCoexPreference::UNKNOWN), "UNKNOWN") ==
        0);
  CHECK(ensureRfCoexBt());
  CHECK(snapshotRfCoexPreference() == RfCoexPreference::BT);
}

void n04_brew_start_requests_ntp_when_unsynced() {
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(hostNtpSyncRequestCount == 0);
  startCycle();
  CHECK(hostNtpSyncRequestCount == 1);
}

void n05_rinse_start_requests_ntp_when_unsynced() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  CHECK(hostNtpSyncRequestCount == 1);
  hostNtpSyncRequestCount = 0;
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(hostNtpSyncRequestCount == 1);
}

void n06_synced_clock_skips_activity_ntp_request() {
  resetHarness(false, true);
  g_wallClock.setSyncing("pool.ntp.org", hostMillis);
  g_wallClock.queueSyncFromCallback(1'700'000'000U);
  CHECK(g_wallClock.applyPendingSync(hostMillis));
  reachReadyFromBoot();
  startCycle();
  CHECK(hostNtpSyncRequestCount == 0);

  resetHarness(false, true);
  g_wallClock.setSyncing("pool.ntp.org", hostMillis);
  g_wallClock.queueSyncFromCallback(1'700'000'000U);
  CHECK(g_wallClock.applyPendingSync(hostMillis));
  reachReadyFromBoot();
  WebCommand rinse = webControlCommand(WebCommandType::RINSE);
  processWebCommand(rinse);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(hostNtpSyncRequestCount == 0);
}

void n07_syncing_clock_skips_activity_ntp_request() {
  resetHarness(false, true);
  g_wallClock.setSyncing("pool.ntp.org", hostMillis);
  reachReadyFromBoot();
  startCycle();
  CHECK(hostNtpSyncRequestCount == 0);
}

void n08_web_rinse_requests_ntp_when_unsynced() {
  resetHarness(false, true);
  reachReadyFromBoot();
  WebCommand rinse = webControlCommand(WebCommandType::RINSE);
  processWebCommand(rinse);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(hostNtpSyncRequestCount == 1);
}

void feedSerial(const char *text) {
  Serial.inject(text);
  size_t guard = 0;
  while (Serial.available() > 0 && guard < 40) {
    runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
    ++guard;
  }
  runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
}

bool serialTxContains(const char *text) {
  return Serial.tx.find(text) != std::string::npos;
}

void sc01_hello_replies_how_are_you() {
  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("hello\n");
  CHECK(serialTxContains("how are you"));
}

void sc01b_heap_dumps_internal_free_blocks() {
  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("heap\n");
  CHECK(serialTxContains("HEAP"));
  CHECK(serialTxContains("freeBlockUnit=bytes"));
  CHECK(serialTxContains("internalFree=200000"));
  CHECK(serialTxContains("internalLargest=100000"));
  CHECK(serialTxContains("freeBlocksWalked=3"));
  CHECK(serialTxContains("freeBlock0Size=100000"));
  CHECK(serialTxContains("freeBlock0Start=0x3fc88000"));
  CHECK(serialTxContains("freeBlock2Size=40000"));
  CHECK(!serialTxContains("freeBlocksTruncated="));
}

void sc02_factory_reset_rejected_while_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  feedSerial("FACTORY_RESET\n");
  CHECK(serialTxContains("ERR not ready"));
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
}

void sc03_set_wifi_queues_save_network() {
  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("SET_WIFI CafeLAN CafePass1\n");
  CHECK(serialTxContains("OK queued SET_WIFI"));
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS);
  CHECK(hostLastForwardedNetworkCommand.type == WebCommandType::SAVE_NETWORK);
  CHECK(strcmp(hostLastForwardedNetworkCommand.network.ssid, "CafeLAN") == 0);
  CHECK(strcmp(hostLastForwardedNetworkCommand.network.password, "CafePass1") == 0);
  CHECK(!hostLastForwardedNetworkCommand.network.openNetwork);
  CHECK(hostLastForwardedNetworkCommand.network.commitConfirmed);
}

void sc04_clear_shots_empties_log() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 100;
  CHECK(shotLog.append(record));
  CHECK(shotLog.count() == 1);
  feedSerial("CLEAR_SHOTS\n");
  CHECK(serialTxContains("OK shots cleared"));
  CHECK(shotLog.count() == 0);
}

void sc05_serial_cli_parser_covers_supported_commands() {
  SerialCliRequest request;
  CHECK(serialCliParseLine("HELP", request));
  CHECK(request.verb == SerialCliVerb::HELP);
  CHECK(serialCliParseLine("HELLO", request));
  CHECK(request.verb == SerialCliVerb::HELLO);
  CHECK(serialCliParseLine("REBOOT", request));
  CHECK(request.verb == SerialCliVerb::REBOOT);
  CHECK(serialCliParseLine("SET_DEVICE_PASSWORD password1234", request));
  CHECK(request.verb == SerialCliVerb::SET_DEVICE_PASSWORD);
  CHECK(strcmp(request.arg1, "password1234") == 0);
  CHECK(serialCliParseLine("SET_WIFI ssid_de_wifi pass_del_wifi", request));
  CHECK(request.verb == SerialCliVerb::SET_WIFI);
  CHECK(strcmp(request.arg1, "ssid_de_wifi") == 0);
  CHECK(strcmp(request.arg2, "pass_del_wifi") == 0);
  CHECK(!request.openNetwork);
  CHECK(serialCliParseLine("SET_WIFI \"Cafe LAN\" SecretPass1", request));
  CHECK(request.verb == SerialCliVerb::SET_WIFI);
  CHECK(strcmp(request.arg1, "Cafe LAN") == 0);
  CHECK(strcmp(request.arg2, "SecretPass1") == 0);
  CHECK(serialCliParseLine("SET_WIFI OpenNet", request));
  CHECK(request.verb == SerialCliVerb::SET_WIFI);
  CHECK(request.openNetwork);
  CHECK(serialCliParseLine("FACTORY_RESET", request));
  CHECK(request.verb == SerialCliVerb::FACTORY_RESET);
  CHECK(serialCliParseLine("RESET_DEVICE_PASSWORD", request));
  CHECK(request.verb == SerialCliVerb::RESET_DEVICE_PASSWORD);
  CHECK(serialCliParseLine("CLEAR_WIFI", request));
  CHECK(request.verb == SerialCliVerb::CLEAR_WIFI);
  CHECK(serialCliParseLine("RESET_NETWORK_AP", request));
  CHECK(request.verb == SerialCliVerb::RESET_NETWORK_AP);
  CHECK(serialCliParseLine("SERIAL_DEBUG_ON", request));
  CHECK(request.verb == SerialCliVerb::SERIAL_DEBUG_ON);
  CHECK(serialCliParseLine("SERIAL_DEBUG_OFF", request));
  CHECK(request.verb == SerialCliVerb::SERIAL_DEBUG_OFF);
  CHECK(serialCliParseLine("DEBUG_FULL", request));
  CHECK(request.verb == SerialCliVerb::DEBUG_FULL);
  CHECK(serialCliParseLine("DEBUG_OFF", request));
  CHECK(request.verb == SerialCliVerb::DEBUG_OFF);
  CHECK(serialCliParseLine("DEBUG_STATUS", request));
  CHECK(request.verb == SerialCliVerb::DEBUG_STATUS);
  CHECK(serialCliParseLine("WIFI_CONNECT", request));
  CHECK(request.verb == SerialCliVerb::WIFI_CONNECT);
  CHECK(serialCliParseLine("WIFI_DISCONNECT", request));
  CHECK(request.verb == SerialCliVerb::WIFI_DISCONNECT);
  CHECK(serialCliParseLine("WIFI_RESTART", request));
  CHECK(request.verb == SerialCliVerb::WIFI_RESTART);
  CHECK(serialCliParseLine("WIFI_STATUS", request));
  CHECK(request.verb == SerialCliVerb::WIFI_STATUS);
  CHECK(serialCliParseLine("AP_START", request));
  CHECK(request.verb == SerialCliVerb::AP_START);
  CHECK(serialCliParseLine("AP_STOP", request));
  CHECK(request.verb == SerialCliVerb::AP_STOP);
  CHECK(serialCliParseLine("AP_STATUS", request));
  CHECK(request.verb == SerialCliVerb::AP_STATUS);
  CHECK(serialCliParseLine("WEBUI_START", request));
  CHECK(request.verb == SerialCliVerb::WEBUI_START);
  CHECK(serialCliParseLine("WEBUI_STOP", request));
  CHECK(request.verb == SerialCliVerb::WEBUI_STOP);
  CHECK(serialCliParseLine("WEBUI_RESTART", request));
  CHECK(request.verb == SerialCliVerb::WEBUI_RESTART);
  CHECK(serialCliParseLine("WEBUI_STATUS", request));
  CHECK(request.verb == SerialCliVerb::WEBUI_STATUS);
  CHECK(serialCliParseLine("NET_STATUS", request));
  CHECK(request.verb == SerialCliVerb::NET_STATUS);
  CHECK(serialCliParseLine("LOG_DUMP", request));
  CHECK(request.verb == SerialCliVerb::LOG_DUMP);
  CHECK(serialCliParseLine("HEALTH", request));
  CHECK(request.verb == SerialCliVerb::HEALTH);
  CHECK(serialCliParseLine("SCALE_STATUS", request));
  CHECK(request.verb == SerialCliVerb::SCALE_STATUS);
  CHECK(serialCliParseLine("NTP_STATUS", request));
  CHECK(request.verb == SerialCliVerb::NTP_STATUS);
  CHECK(serialCliParseLine("HEAP", request));
  CHECK(request.verb == SerialCliVerb::HEAP);
  CHECK(!serialCliParseLine("HELP extra", request));
  CHECK(request.verb == SerialCliVerb::INVALID_ARGS);
  CHECK(!serialCliParseLine("REBOOT extra", request));
  CHECK(request.verb == SerialCliVerb::INVALID_ARGS);
  CHECK(!serialCliParseLine("SERIAL_DEBUG_ON extra", request));
  CHECK(request.verb == SerialCliVerb::INVALID_ARGS);
  CHECK(serialCliParseLine("SET_DEVICE_PASSWORD ineedacoffee", request));
  CHECK(request.verb == SerialCliVerb::INVALID_ARGS);
  CHECK(serialCliParseLine("SET_DEVICE_PASSWORD oldpass newpass", request));
  CHECK(request.verb == SerialCliVerb::INVALID_ARGS);
  CHECK(serialCliParseLine("SET_WIFI Cafe short", request));
  CHECK(request.verb == SerialCliVerb::INVALID_ARGS);
  CHECK(serialCliParseLine("not_a_command", request));
  CHECK(request.verb == SerialCliVerb::UNKNOWN);
  CHECK(!serialCliParseLine("   ", request));
}

void sc06_serial_cli_feed_completes_on_crlf() {
  SerialCliParser parser;
  SerialCliRequest request;
  bool ready = false;
  const char *line = "HELLO\r\n";
  for (size_t index = 0; line[index] != '\0'; ++index) {
    if (serialCliFeed(parser, line[index], request)) {
      CHECK(!ready);
      CHECK(request.verb == SerialCliVerb::HELLO);
      ready = true;
    }
  }
  CHECK(ready);
}

void bc05_ble_scan_intensity_applies_live_without_restart() {
  resetHarness(false, false);
  reachReadyFromBoot();
  CHECK(liveBleScanIntensity() == BleScanIntensity::BALANCED);

  persistBleScanIntensity(BleScanIntensity::RELAXED);
  CHECK(liveBleScanIntensity() == BleScanIntensity::RELAXED);
  publishControlStatus();
  ControlStatusSnapshot control;
  copyControlStatus(control);
  CHECK(control.bleScanIntensity ==
        static_cast<uint8_t>(BleScanIntensity::RELAXED));

  WebCommand command = webControlCommand(WebCommandType::BLE_SCAN_INTENSITY);
  command.bleScan.specified |= BleScanCommandPayload::INTENSITY;
  command.bleScan.intensity =
      static_cast<uint8_t>(BleScanIntensity::AGGRESSIVE);
  processWebCommand(command);
  CHECK(liveBleScanIntensity() == BleScanIntensity::AGGRESSIVE);
  // The durable write is deferred: the loop's persistence servicer reports
  // the held command as PERSISTED only after the staged flush completes.
  CHECK(hostLastForwardedNetworkCommand.requestId == 0);
  runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
  CHECK(hostLastForwardedNetworkCommand.type ==
        WebCommandType::MAINTENANCE_COMPLETE);
  CHECK(hostLastForwardedNetworkCommand.requestId == 1);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::PERSISTED);
  CHECK(!bleScanPersistPending);

  // Mid-cycle the command is rejected: no staged flash write is accepted.
  startCycle();
  command.requestId = 2;
  command.bleScan.intensity =
      static_cast<uint8_t>(BleScanIntensity::RELAXED);
  processWebCommand(command);
  CHECK(liveBleScanIntensity() == BleScanIntensity::AGGRESSIVE);
  CHECK(hostLastForwardedNetworkCommand.requestId == 2);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::FAILED);
  CHECK(!bleScanPersistPending);
}

void bc06_micra_scan_override_selects_discovery_duty() {
  resetHarness(false, false);
  reachReadyFromBoot();
  applyLiveBleScanIntensity(BleScanIntensity::RELAXED);

  // An applicable machine-power override wins over the saved mode without
  // rewriting it.
  applyLiveBleScanOverride(BleScanIntensity::AGGRESSIVE);
  CHECK(discoveryScanIntensity() == BleScanIntensity::AGGRESSIVE);
  CHECK(liveBleScanIntensity() == BleScanIntensity::RELAXED);
  CHECK(startScaleDiscoveryScan(nullptr, false));
  uint16_t interval = 0, window = 0;
  bleScanHciParams(BleScanIntensity::AGGRESSIVE, interval, window);
  CHECK(scale.lastScanInterval == interval && scale.lastScanWindow == window);

  // The opposite override demotes even a saved Aggressive preference.
  applyLiveBleScanIntensity(BleScanIntensity::AGGRESSIVE);
  applyLiveBleScanOverride(BleScanIntensity::RELAXED);
  CHECK(discoveryScanIntensity() == BleScanIntensity::RELAXED);
  CHECK(liveBleScanIntensity() == BleScanIntensity::AGGRESSIVE);
  // The running scan reconfigures only when the effective duty changes.
  serviceScaleScanIntensity();
  bleScanHciParams(BleScanIntensity::RELAXED, interval, window);
  CHECK(scale.lastScanInterval == interval && scale.lastScanWindow == window);
  const size_t restarts = scale.startScanCalls;
  serviceScaleScanIntensity();
  CHECK(scale.startScanCalls == restarts);

  // BALANCED clears the override: the saved mode decides again, in every CPU
  // power profile. Scan duty never follows the ESP32 power profile alone.
  applyLiveBleScanOverride(BleScanIntensity::BALANCED);
  powerAppliedProfile.store(PowerProfile::IDLE);
  CHECK(discoveryScanIntensity() == BleScanIntensity::AGGRESSIVE);
  powerAppliedProfile.store(PowerProfile::OFF);
  serviceScaleScanIntensity();
  bleScanHciParams(BleScanIntensity::AGGRESSIVE, interval, window);
  CHECK(scale.lastScanInterval == interval && scale.lastScanWindow == window);

  // Time without scale evidence never demotes discovery on its own.
  hostMillis += 3600UL * 1000UL;
  serviceScaleScanIntensity();
  CHECK(discoveryScanIntensity() == BleScanIntensity::AGGRESSIVE);
}

void bc07_ble_scan_relaxed_with_disabled_switch_is_api_valid() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand command = webControlCommand(WebCommandType::BLE_SCAN_INTENSITY);
  command.bleScan.specified |= BleScanCommandPayload::INTENSITY |
                               BleScanCommandPayload::ENABLED;
  command.bleScan.intensity = static_cast<uint8_t>(BleScanIntensity::RELAXED);
  command.bleScan.enabled = 0;
  processWebCommand(command);
  CHECK(liveBleScanIntensity() == BleScanIntensity::RELAXED);
  CHECK(!liveBleEnabled());
  publishControlStatus();
  ControlStatusSnapshot control;
  copyControlStatus(control);
  CHECK(control.bleScanEnabled == false);
  // Both fields share one staged flush.
  runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
  CHECK(hostLastForwardedNetworkCommand.requestId == 1);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::PERSISTED);
  CHECK(!bleScanEnabledPersistPending);
}

void bc08_ble_scan_superseded_requests_all_report_persisted() {
  resetHarness(false, false);
  reachReadyFromBoot();
  // Two requests accepted before the deferred flush share one combined save;
  // both ids must reach PERSISTED, not just the newest one.
  WebCommand command = webControlCommand(WebCommandType::BLE_SCAN_INTENSITY);
  command.bleScan.specified |= BleScanCommandPayload::INTENSITY;
  command.bleScan.intensity = static_cast<uint8_t>(BleScanIntensity::RELAXED);
  processWebCommand(command);
  command.requestId = 2;
  command.bleScan.intensity = static_cast<uint8_t>(BleScanIntensity::AGGRESSIVE);
  processWebCommand(command);
  CHECK(liveBleScanIntensity() == BleScanIntensity::AGGRESSIVE);
  const uint32_t callsBefore = hostForwardAcceptedNetworkCommandCalls;
  runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
  CHECK(hostForwardAcceptedNetworkCommandCalls - callsBefore == 2);
  CHECK(hostLastForwardedNetworkCommand.requestId == 2);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::PERSISTED);
  CHECK(!bleScanPersistPending);
}

void bc10_ble_master_switch_quiesces_scale_link() {
  resetHarness(false, true);
  reachReadyFromBoot();
  setScaleConnected(true);
  CHECK(liveBleEnabled());
  CHECK(!serviceBleMasterSwitch());

  applyLiveBleEnabled(false);
  CHECK(!liveBleEnabled());
  CHECK(serviceBleMasterSwitch());
  CHECK(!scale.isConnected());
  CHECK(!scale.isScanning());
  CHECK(getScaleLinkSnapshot().state == ScaleLinkState::DISCONNECTED);
  CHECK(!scaleLinkAvailable(getScaleLinkSnapshot()));
  // Scanning cannot restart while the switch stays off.
  CHECK(!startScaleDiscoveryScan(nullptr, false));
  CHECK(serviceBleMasterSwitch());

  applyLiveBleEnabled(true);
  CHECK(liveBleEnabled());
  CHECK(!serviceBleMasterSwitch());

  setScaleConnected(false);
  scale.connecting = true;
  scaleLoggedGattConnecting = true;
  updateWorkerLinkState();
  applyLiveBleEnabled(false);
  CHECK(serviceBleMasterSwitch());
  CHECK(!scaleLoggedGattConnecting);
  const size_t connectionChecks = scale.isConnectedCalls;
  const size_t disconnects = scale.disconnectCalls;
  CHECK(serviceBleMasterSwitch());
  CHECK(scale.isConnectedCalls == connectionChecks + 1);
  CHECK(scale.disconnectCalls == disconnects);
}

void bc11_ble_master_switch_command_persists_live_without_restart() {
  resetHarness(false, false);
  reachReadyFromBoot();
  CHECK(liveBleEnabled());

  WebCommand command = webControlCommand(WebCommandType::BLE_SCAN_INTENSITY);
  command.bleScan.specified |= BleScanCommandPayload::ENABLED;
  command.bleScan.enabled = 0;
  processWebCommand(command);
  CHECK(!liveBleEnabled());
  publishControlStatus();
  ControlStatusSnapshot control;
  copyControlStatus(control);
  CHECK(!control.bleScanEnabled);
  // The durable write is deferred like the other scan fields: PERSISTED only
  // after the staged flush completes.
  CHECK(hostLastForwardedNetworkCommand.requestId == 0);
  runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
  CHECK(hostLastForwardedNetworkCommand.requestId == 1);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::PERSISTED);
  CHECK(!bleScanEnabledPersistPending);

  // Re-enabling shares the same combined save path.
  command.requestId = 2;
  command.bleScan.enabled = 1;
  processWebCommand(command);
  CHECK(liveBleEnabled());
  runLoopAfter(CONTROL_HOUSEKEEPING_INTERVAL_MS);
  CHECK(hostLastForwardedNetworkCommand.requestId == 2);
  CHECK(hostLastForwardedNetworkCommand.resultState ==
        CommandResultState::PERSISTED);
  CHECK(!bleScanEnabledPersistPending);
  applyLiveBleEnabled(true);
}

void sc07_reset_device_password_and_clear_wifi_queue() {
  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("RESET_DEVICE_PASSWORD\n");
  CHECK(serialTxContains("OK queued RESET_DEVICE_PASSWORD"));
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS);
  CHECK(hostLastForwardedNetworkCommand.type ==
        WebCommandType::RESET_DEVICE_PASSWORD);

  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("CLEAR_WIFI\n");
  CHECK(serialTxContains("OK queued CLEAR_WIFI"));
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS);
  CHECK(hostLastForwardedNetworkCommand.type ==
        WebCommandType::FORGET_NETWORK);
}

void sc08_set_device_password_queues_change() {
  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("SET_DEVICE_PASSWORD password1234\n");
  CHECK(serialTxContains("OK queued SET_DEVICE_PASSWORD"));
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS);
  CHECK(hostLastForwardedNetworkCommand.type ==
        WebCommandType::CHANGE_DEVICE_PASSWORD);
  CHECK(strcmp(hostLastForwardedNetworkCommand.network.password, "password1234") == 0);
}

void sc09_serial_debug_toggles_without_ready() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.active);
  CHECK(serialLogLevelFromRuntime(runtimeConfig) == LogLevel::NONE);
  CHECK(serialLogLevel == LogLevel::NONE);
  Serial.tx.clear();
  addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED);
  serialTrace(LogLevel::WARNING, "Scale name scan: no advertisement");
  CHECK(!serialTxContains("configuration accepted"));
  CHECK(!serialTxContains("Scale name scan: no advertisement"));
  feedSerial("SERIAL_DEBUG_ON\n");
  CHECK(serialTxContains("OK serial debug on"));
  CHECK(serialLogLevelFromRuntime(runtimeConfig) == LogLevel::INFO);
  CHECK(serialLogLevel == LogLevel::INFO);
  CHECK(runtimePersistPending);
  Serial.tx.clear();
  addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED);
  serialTrace(LogLevel::WARNING, "Scale name scan: no advertisement");
  CHECK(!serialTxContains("configuration accepted"));
  CHECK(!serialTxContains("Scale name scan: no advertisement"));
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  bool sawAccepted = false;
  for (size_t i = 0; i < count; ++i) {
    if (events[i].code == DebugCode::CONFIG_ACCEPTED) {
      sawAccepted = true;
    }
  }
  CHECK(sawAccepted);
  feedSerial("SERIAL_DEBUG_OFF\n");
  CHECK(serialTxContains("OK serial debug off"));
  CHECK(serialLogLevelFromRuntime(runtimeConfig) == LogLevel::NONE);
  CHECK(serialLogLevel == LogLevel::NONE);
  Serial.tx.clear();
  addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED);
  serialTrace(LogLevel::WARNING, "Scale name scan: no advertisement");
  CHECK(!serialTxContains("configuration accepted"));
  CHECK(!serialTxContains("Scale name scan: no advertisement"));
}

void sc10_help_prints_one_line_per_command() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.active);
  Serial.tx.clear();
  feedSerial("help\n");
  CHECK(serialTxContains("HELP  list commands"));
  CHECK(serialTxContains("HELLO  probe CLI"));
  CHECK(serialTxContains("REBOOT  restart firmware"));
  CHECK(serialTxContains("FACTORY_RESET  wipe Wi-Fi"));
  CHECK(serialTxContains("RESET_DEVICE_PASSWORD  restore device password"));
  CHECK(serialTxContains("SET_DEVICE_PASSWORD <password>"));
  CHECK(serialTxContains("SET_WIFI <ssid> [password]"));
  CHECK(serialTxContains("CLEAR_WIFI  forget STA Wi-Fi"));
  CHECK(serialTxContains("CLEAR_SHOTS  clear shot history"));
  CHECK(serialTxContains("RESET_NETWORK_AP  forget STA Wi-Fi"));
  CHECK(serialTxContains("SERIAL_DEBUG_ON  enable USB debug"));
  CHECK(serialTxContains("SERIAL_DEBUG_OFF  disable USB debug"));
  CHECK(serialTxContains("DEBUG_FULL  serial debug + ring DEBUG"));
  CHECK(serialTxContains("DEBUG_OFF  serial off and ring none"));
  CHECK(serialTxContains("DEBUG_STATUS  show serialLogLevel"));
  CHECK(serialTxContains("WIFI_CONNECT  associate saved STA"));
  CHECK(serialTxContains("WIFI_DISCONNECT  drop STA"));
  CHECK(serialTxContains("WIFI_RESTART  drop then reconnect"));
  CHECK(serialTxContains("WIFI_STATUS  dump STA"));
  CHECK(serialTxContains("AP_START  raise SoftAP (stays up with STA)"));
  CHECK(serialTxContains("AP_STOP  stop SoftAP"));
  CHECK(serialTxContains("AP_STATUS  dump SoftAP"));
  CHECK(serialTxContains("WEBUI_START  start HTTP"));
  CHECK(serialTxContains("WEBUI_STOP  stop HTTP"));
  CHECK(serialTxContains("WEBUI_RESTART  bounce HTTP"));
  CHECK(serialTxContains("WEBUI_STATUS  dump HTTP"));
  CHECK(serialTxContains("NET_STATUS  WIFI + AP + WEBUI"));
  CHECK(serialTxContains(
      "LOG_DUMP  print RAM debug ring (deferred in brew/machine circuit)"));
  CHECK(serialTxContains("HEALTH  heap, loop gap, cpu load"));
    CHECK(serialTxContains("AUDIT_DUMP [n]  curated audit log"));
    CHECK(serialTxContains("AUDIT_VERBOSE [on|off]  30-minute verbose hunt"));
  CHECK(serialTxContains("SCALE_STATUS  BLE scale link"));
  CHECK(serialTxContains("NTP_STATUS  wall clock"));
  CHECK(serialTxContains("HEAP  internal heap free-block layout"));
  CHECK(serialTxContains("TASK_PROFILE [on|off]  task stacks/CPU"));
  CHECK(serialTxContains("e.g. SET_WIFI CafeLAN CafePass1"));
  CHECK(Serial.tx.size() <= SERIAL_CLI_OUTPUT_CAPACITY);
  CHECK(session.active);
}

void sc11_reboot_queues_restart_when_ready() {
  resetHarness(false, false);
  reachReadyFromBoot();
  feedSerial("REBOOT\n");
  CHECK(serialTxContains("OK queued REBOOT"));
  runLoopAfter(MAINTENANCE_LEASE_SETTLE_MS);
  CHECK(hostLastForwardedNetworkCommand.type == WebCommandType::RESTART);
}

void sc12_reboot_waits_while_active() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  feedSerial("REBOOT\n");
  CHECK(serialTxContains("OK queued REBOOT"));
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(plannedRestartHeld);
  CHECK(!maintenanceLease.active);
}

void sc13_debug_full_and_off_during_cycle() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.active);
  Serial.tx.clear();
  feedSerial("DEBUG_FULL\n");
  CHECK(serialTxContains("OK debug full"));
  CHECK(serialLogLevelFromRuntime(runtimeConfig) == LogLevel::DEBUG);
  CHECK(runtimeConfig.ringRetainLogLevel ==
        static_cast<uint8_t>(LogLevel::DEBUG));
  CHECK(serialLogLevel == LogLevel::DEBUG);
  CHECK(ringRetainLogLevel == LogLevel::DEBUG);
  CHECK(runtimePersistPending);
  Serial.tx.clear();
  addDebugEvent(DebugCategory::SCALE, DebugCode::SCALE_CONNECTING);
  CHECK(!serialTxContains("scale connecting"));
  DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
  const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
  bool sawConnecting = false;
  for (size_t i = 0; i < count; ++i) {
    if (events[i].code == DebugCode::SCALE_CONNECTING) {
      sawConnecting = true;
    }
  }
  CHECK(sawConnecting);
  feedSerial("DEBUG_OFF\n");
  CHECK(serialTxContains("OK debug off"));
  CHECK(serialLogLevelFromRuntime(runtimeConfig) == LogLevel::NONE);
  CHECK(runtimeConfig.ringRetainLogLevel ==
        static_cast<uint8_t>(LogLevel::NONE));
  CHECK(serialLogLevel == LogLevel::NONE);
  CHECK(ringRetainLogLevel == LogLevel::NONE);
}

void sc14_network_actions_queue_without_ready() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  Serial.tx.clear();
  feedSerial("WIFI_CONNECT\n");
  CHECK(serialTxContains("WARN cycle active; proceeding"));
  CHECK(serialTxContains("OK queued WIFI_CONNECT"));
  CHECK(hostLastForwardedNetworkCommand.type == WebCommandType::WIFI_CONNECT);
  feedSerial("WEBUI_STOP\n");
  CHECK(serialTxContains("OK queued WEBUI_STOP"));
  CHECK(hostLastForwardedNetworkCommand.type == WebCommandType::WEBUI_STOP);
  feedSerial("AP_START\n");
  CHECK(serialTxContains("OK queued AP_START"));
  CHECK(hostLastForwardedNetworkCommand.type == WebCommandType::AP_START);
}

void sc15_status_printers_use_dump_views() {
  SerialCliNetworkDump dump;
  dump.wifiConfigured = true;
  dump.staOpen = false;
  strncpy(dump.iface.staSsid, "CafeLAN", sizeof(dump.iface.staSsid) - 1);
  dump.staState = 2;
  dump.wifiStatus = 3;
  dump.wifiMode = 1;
  dump.httpActive = true;
  dump.apActive = true;
  dump.staReconnectHeld = true;
  dump.apStartHeld = false;
  dump.httpStartHeld = true;
  dump.wifiPs = WifiPsLive::NONE;
  strncpy(dump.iface.staIp, "192.168.1.20", sizeof(dump.iface.staIp) - 1);
  Serial.tx.clear();
  serialCliPrintNetStatus(dump);
  CHECK(serialTxContains("WIFI_STATUS"));
  CHECK(serialTxContains("ssid=CafeLAN"));
  CHECK(serialTxContains("staState=CONNECTED"));
  CHECK(serialTxContains("wifiStatus=3 CONNECTED"));
  CHECK(serialTxContains("staReconnectHeld=true"));
  CHECK(serialTxContains("AP_STATUS"));
  CHECK(serialTxContains("ssid=OpenBrewByWeightAP"));
  CHECK(serialTxContains("wifiPs=NONE"));
  CHECK(serialTxContains("WEBUI_STATUS"));
  CHECK(serialTxContains("httpActive=true"));
  CHECK(serialTxContains("httpStartHeld=true"));

  strncpy(dump.iface.apSsid, "OpenBrewByWeightAP-75fcbf0d", sizeof(dump.iface.apSsid) - 1);
  Serial.tx.clear();
  serialCliPrintApStatus(dump);
  CHECK(serialTxContains("ssid=OpenBrewByWeightAP-75fcbf0d"));

  SerialCliHealthDump health;
  health.freeHeapBytes = 80000;
  health.internalHeapAllocatedBlocks = 120;
  health.internalHeapFreeBlocks = 8;
  health.internalHeapFragmentationPermille = 375;
  health.loopMaxGapMs = 12;
  health.healthIntervalMaxGapMs = 5;
  health.networkStackMinBytes = 400;
  health.cpuLoadValid = true;
  health.cpuMhz = 80;
  health.cpuLoad5s = 0.42f;
  health.cpuLoad1m = 0.55f;
  health.cpuLoad5m = 0.61f;
  health.cpu0Busy = 0.20f;
  health.cpu1Busy = 0.22f;
  health.tempValid = true;
  health.tempC = 42.5f;
  health.tempPeakC = 47.0f;
  Serial.tx.clear();
  serialCliPrintHealth(health);
  CHECK(serialTxContains("stackUnit=bytes stackUnavailable=4294967295"));
  CHECK(serialTxContains("HEALTH"));
  CHECK(serialTxContains("heapFree=80000"));
  CHECK(serialTxContains("heapAllocatedBlocks=120"));
  CHECK(serialTxContains("heapFreeBlocks=8"));
  CHECK(serialTxContains("heapFragmentationPermille=375"));
  CHECK(serialTxContains("loopMaxGapMs=12"));
  CHECK(serialTxContains("loopIntervalGapMs=5"));
  CHECK(serialTxContains("psramSize=0"));
  CHECK(serialTxContains("psramFree=0"));
  CHECK(serialTxContains("psramLargest=0"));
  CHECK(serialTxContains("workBufExternal=false"));
  CHECK(serialTxContains("jsonArenaExternal=false"));
  CHECK(serialTxContains("allocExternalFallback=0"));
  CHECK(serialTxContains("stackNetwork=400"));
  CHECK(serialTxContains("cpuMhz=80"));
  CHECK(serialTxContains("cpuLoad5s=0.42"));
  CHECK(serialTxContains("cpuLoad1m=0.55"));
  CHECK(serialTxContains("cpuLoad5m=0.61"));
  CHECK(serialTxContains("cpu0Busy=0.20"));
  CHECK(serialTxContains("cpu1Busy=0.22"));
  CHECK(serialTxContains("tempValid=true"));
  CHECK(serialTxContains("tempC=42.5"));
  CHECK(serialTxContains("tempPeakC=47.0"));
  CHECK(Serial.tx.size() <= SERIAL_CLI_OUTPUT_CAPACITY);

  Serial.tx.clear();
  serialCliPrintHeap(sampleHeapCaps(), sampleInternalFreeBlocks());
  CHECK(serialTxContains("internalTotal=327680"));
  CHECK(serialTxContains("internalFree=200000"));
  CHECK(serialTxContains("internalMinimum=180000"));
  CHECK(serialTxContains("internalLargest=100000"));
  CHECK(serialTxContains("internalFragmentationPermille=500"));
  CHECK(serialTxContains("freeBlocksWalked=3"));
  CHECK(serialTxContains("freeBlock0Size=100000"));
  CHECK(serialTxContains("freeBlock2Size=40000"));
  CHECK(serialTxContains("freeBlock2Start=0x3fce0000"));
  HeapFreeBlockList truncated;
  truncated.walkedFreeBlocks = HEAP_FREE_BLOCK_SAMPLE_CAPACITY + 5;
  truncated.listedCount = HEAP_FREE_BLOCK_SAMPLE_CAPACITY;
  for (size_t i = 0; i < HEAP_FREE_BLOCK_SAMPLE_CAPACITY; ++i) {
    truncated.blocks[i] = HeapFreeBlockInfo{4096, 0x3FC88000u + i * 4096u};
  }
  Serial.tx.clear();
  serialCliPrintHeap(sampleHeapCaps(), truncated);
  CHECK(serialTxContains("freeBlocksTruncated=5"));
  CHECK(Serial.tx.size() <= SERIAL_CLI_OUTPUT_CAPACITY);

  SerialCliScaleDump scale;
  scale.state = "CONNECTED";
  scale.protocolName = "bookoo";
  copyCString(scale.preferredMac, sizeof(scale.preferredMac),
              "AA:BB:CC:DD:EE:FF");
  scale.weightFresh = true;
  scale.currentWeightG = 18.5f;
  Serial.tx.clear();
  serialCliPrintScaleStatus(scale);
  CHECK(serialTxContains("SCALE_STATUS"));
  CHECK(serialTxContains("state=CONNECTED"));
  CHECK(serialTxContains("preferredMac=AA:BB:CC:DD:EE:FF"));
  CHECK(serialTxContains("recoveredStaleCount=0"));
  CHECK(serialTxContains("recoveredStaleMs=0"));
  CHECK(serialTxContains("rssi=-"));
  CHECK(serialTxContains("weightG=18.50"));
  CHECK(serialTxContains("scanIntensity=balanced"));
  CHECK(!serialTxContains("scanBackoffMin"));
  CHECK(!serialTxContains("scanBoostMin"));

  scale.rssiValid = true;
  scale.rssi = -62;
  Serial.tx.clear();
  serialCliPrintScaleStatus(scale);
  CHECK(serialTxContains("rssi=-62"));

  SerialCliNtpDump ntp;
  ntp.state = TimeSyncState::SYNCED;
  ntp.utcSec = 1700000000;
  strncpy(ntp.activeServer, "pool.ntp.org", sizeof(ntp.activeServer) - 1);
  ntp.staUp = false;
  Serial.tx.clear();
  serialCliPrintNtpStatus(ntp);
  CHECK(serialTxContains("NTP_STATUS"));
  CHECK(serialTxContains("state=SYNCED"));
  CHECK(serialTxContains("ntp cannot arm without STA"));
}

void sc16_debug_status_and_log_dump() {
  resetHarness(false, false);
  reachReadyFromBoot();
  ringRetainLogLevel = LogLevel::INFO;
  runtimeConfig.ringRetainLogLevel = static_cast<uint8_t>(LogLevel::INFO);
  Serial.tx.clear();
  feedSerial("DEBUG_STATUS\n");
  CHECK(serialTxContains("DEBUG_STATUS"));
  CHECK(serialTxContains("serialLogLevel=none"));
  CHECK(serialTxContains("ringRetainLogLevel=info"));
  addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED);
  Serial.tx.clear();
  feedSerial("LOG_DUMP\n");
  CHECK(serialTxContains("LOG_DUMP"));
  CHECK(serialTxContains("events="));
  CHECK(serialTxContains("configuration accepted"));
  ringRetainLogLevel = LogLevel::NONE;
  runtimeConfig.ringRetainLogLevel = static_cast<uint8_t>(LogLevel::NONE);
  Serial.tx.clear();
  feedSerial("LOG_DUMP\n");
  CHECK(serialTxContains("log ring retain is none"));
  startCycle();
  Serial.tx.clear();
  feedSerial("LOG_DUMP\n");
  CHECK(serialTxContains("ERR LOG dump deferred; circuit/cycle active"));
  CHECK(!serialTxContains("events="));
}

void s04_shot_log_remove_by_id() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord first = {};
  first.durationDs = 100;
  CHECK(shotLog.append(first));
  ShotLogRecord second = {};
  second.durationDs = 200;
  CHECK(shotLog.append(second));
  CHECK(shotLog.count() == 2);
  ShotLogRecord records[2] = {};
  CHECK(shotLog.copyNewestFirst(records, 2) == 2);
  CHECK(records[0].durationDs == 200);
  CHECK(records[1].durationDs == 100);
  const uint32_t deleteId = records[0].id;
  CHECK(deleteId != 0);
  CHECK(shotLog.removeById(deleteId));
  CHECK(shotLog.count() == 1);
  CHECK(shotLog.copyNewestFirst(records, 2) == 1);
  CHECK(records[0].durationDs == 100);
  CHECK(!shotLog.removeById(deleteId));
  CHECK(!shotLog.removeById(99999U));
}

void s04c_delete_shot_record_removes_log_and_curve() {
  resetHarness(false, true);
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  ShotLogRecord record = {};
  record.durationDs = 130;
  record.actualWeightCg = 3600;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  const uint32_t id = stored[0].id;
  CHECK(id != 0);
  ShotCurveRecord curve = emptyShotCurveRecord();
  curve.shotId = id;
  CHECK(shotCurves.append(curve));
  PersistedLastShot good = {};
  good.valid = true;
  good.cycleId = 44;
  good.durationMs = 25000;
  good.weightValid = true;
  good.currentWeightG = 36.5f;
  good.shotLogId = id;
  good.presetId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  persistLastShotSnapshot(good);
  CHECK(rateShotRecord(id, 4));
  CHECK(persistedLastGoodShot.rating == 0);
  uint32_t bootId = 0;
  PersistedLastShot last = {}, publishedGood = {};
  HistoryRecord newestActivation = {};
  bool hasNewestActivation = false;
  CHECK(copyShotStoreStatus(bootId, last, publishedGood, newestActivation,
                            hasNewestActivation, true));
  CHECK(publishedGood.rating == 4);
  CHECK(shotLog.containsId(id));
  CHECK(shotCurves.containsShotId(id));
  CHECK(deleteShotRecord(id));
  CHECK(!shotLog.containsId(id));
  CHECK(!shotCurves.containsShotId(id));
  CHECK(shotLog.count() == 0);
  CHECK(shotCurves.count() == 0);
  CHECK(persistedLastGoodShot.cycleId == 44);
  CHECK(fabsf(persistedLastGoodShot.currentWeightG - 36.5f) < 0.001f);
  CHECK(!copyShotStoreStatus(bootId, last, publishedGood, newestActivation,
                             hasNewestActivation, true));
  CHECK(!rateLastShot(5));
}

void s04d_delete_shot_record_ok_without_curve() {
  resetHarness(false, true);
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  ShotLogRecord record = {};
  record.durationDs = 80;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  const uint32_t id = stored[0].id;
  CHECK(!shotCurves.containsShotId(id));
  CHECK(deleteShotRecord(id));
  CHECK(!shotLog.containsId(id));
  CHECK(shotLog.count() == 0);
}

void s04e_delete_shot_record_is_ram_only_when_flash_is_busy() {
  resetHarness(false, true);
  shotLog.clear();
  ShotCurveLog::resetHostStorage();
  shotCurves.load();
  ShotLogRecord record = {};
  record.durationDs = 90;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  const uint32_t id = stored[0].id;
  ShotCurveRecord curve = emptyShotCurveRecord();
  curve.shotId = id;
  CHECK(shotCurves.append(curve));
  g_hostFlashIoMutexAvailable = false;
  const bool deleted = deleteShotRecord(id);
  g_hostFlashIoMutexAvailable = true;
  CHECK(deleted);
  CHECK(!shotLog.containsId(id));
  CHECK(!shotCurves.containsShotId(id));
  CHECK(shotLog.dirty());
  CHECK(shotCurves.dirty());
}

void b01_scale_worker_requires_ble_stack() {
  resetHarness(false, true);
  vQueueDelete(scaleCommandQueue);
  vQueueDelete(scaleEventQueue);
  scaleCommandQueue = nullptr;
  scaleEventQueue = nullptr;
  setScaleWorkerBleReadyForHost(false);
  CHECK(!initializeScaleWorker());
  publishControlStatus();
  CHECK(!publishedControlStatus.scaleWorkerReady);
  setScaleWorkerBleReadyForHost(true);
  publishControlStatus();
  CHECK(publishedControlStatus.scaleWorkerReady == scaleWorkerReady());
}

void b02_setup_degrades_without_ble() {
  resetHarness(false, true);
  setScaleWorkerBleReadyForHost(false);
  deleteHostResources();
  setup();
  CHECK(bootDegraded);
  CHECK(!firmwareInitializationComplete);
  CHECK(publishedControlStatus.bootDegraded);
  CHECK(!publishedControlStatus.bootComplete);
  CHECK(!publishedControlStatus.scaleWorkerReady);
}

#if OPEN_BREW_BY_WEIGHT_ENABLE_JTAG == 1
void b03_jtag_build_starts_serial_without_jumper() {
  resetHarness(false, false);
  CHECK(!usbConsoleJumperPresent());
  deleteHostResources();
  setup();
  CHECK(Serial.beginCalls == 1);
  CHECK(usbSerialEnableSource == UsbSerialEnableSource::COMPILE_FLAG);
  CHECK(publishedControlStatus.usbSerialEnableSource ==
        UsbSerialEnableSource::COMPILE_FLAG);
  CHECK(!publishedControlStatus.usbConsoleIo4Closed);
}
#else
void b03_usb_console_stays_off_without_jumper() {
  resetHarness(false, false);
  CHECK(!usbConsoleJumperPresent());
  CHECK(hostPinMode[USB_CONSOLE_GPIO] == INPUT_PULLUP);
  deleteHostResources();
  setup();
  CHECK(Serial.beginCalls == 0);
  CHECK(usbSerialEnableSource == UsbSerialEnableSource::OFF);
  CHECK(publishedControlStatus.usbSerialEnableSource ==
        UsbSerialEnableSource::OFF);
  CHECK(!publishedControlStatus.usbConsoleIo4Closed);
}
#endif

void b04_usb_console_starts_when_jumper_held() {
  resetHarness(false, false);
  hostPinLevel[USB_CONSOLE_GPIO] = LOW;
  CHECK(usbConsoleJumperPresent());
  deleteHostResources();
  setup();
  CHECK(Serial.beginCalls == 1);
#if OPEN_BREW_BY_WEIGHT_ENABLE_JTAG == 1
  CHECK(usbSerialEnableSource == UsbSerialEnableSource::COMPILE_FLAG);
#else
  CHECK(usbSerialEnableSource == UsbSerialEnableSource::JUMPER);
#endif
  CHECK(publishedControlStatus.usbConsoleIo4Closed);
  CHECK(publishedControlStatus.usbSerialEnableSource ==
        usbSerialEnableSource);
}

void m08_recipe_copies_match_published_state() {
  resetHarness(false, true);
  RuntimeConfig copied = {};
  ShotPresetBank copiedBank = {};
  copyRuntimeConfig(&copied);
  copyPresetBank(&copiedBank);
  CHECK(copied.goalWeightDg == runtimeConfig.goalWeightDg);
  CHECK(copiedBank.activeId == presetBank.activeId);
  runtimeConfig.goalWeightDg = 440;
  mutableActiveShotPreset(presetBank).goalWeightDg = 440;
  commitLiveRuntimeConfig(runtimeConfig, RUNTIME_PERSIST_REASON_USER);
  RuntimeConfig after = {};
  ShotPresetBank afterBank = {};
  copyRuntimeConfig(&after);
  copyPresetBank(&afterBank);
  CHECK(after.goalWeightDg == 440);
  CHECK(afterBank.activeId == presetBank.activeId);
  CHECK(findShotPreset(afterBank, afterBank.activeId) != nullptr);
  CHECK(findShotPreset(afterBank, afterBank.activeId)->goalWeightDg == 440);
}

void m09_snapshot_mutexes_preserve_concurrent_invariants() {
  resetHarness(false, true);
  constexpr uint32_t kIterations = 2000;
  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  std::atomic<uint32_t> violations{0};

  auto publishInvariant = [](uint32_t generation) {
    maintenanceLease.active = true;
    maintenanceLease.id = generation;
    session.active = (generation & 1U) != 0U;
    session.id = generation;
    session.source = ControlSource::WEB;
    runtimeConfig.revision = generation;
    runtimeConfig.goalWeightDg = static_cast<uint16_t>(200U + generation % 400U);
    runtimeConfig.dripDelayMs = generation ^ 0x5a5a5a5aU;
    ShotPreset &active = mutableActiveShotPreset(presetBank);
    active.goalWeightDg = runtimeConfig.goalWeightDg;
    active.operationalWallMs = 10000U + active.goalWeightDg;
    shotCurveSampler.reset(0);
    shotCurveSampler.accept(static_cast<float>(generation % 100), generation);
    publishRecipeState();
    publishControlStatus();

    TaskProfilerSnapshot profiler;
    profiler.sampleCount = generation;
    profiler.elapsedMs = generation * 2U;
    profiler.rowCount = 1;
    profiler.rows[0].taskNumber = generation;
    taskProfiler.publishForHost(profiler);
  };

  static thread_local bool publicationLocked = false;
  static thread_local unsigned nestedAcquisitions = 0;
  publicationLocked = false;
  nestedAcquisitions = 0;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &controlStatusMutex) publicationLocked = acquired;
    else if (acquired && publicationLocked) ++nestedAcquisitions;
  };
  publishInvariant(1);
  TaskMutex::hostObserver = nullptr;
  CHECK(!publicationLocked);
  CHECK(nestedAcquisitions == 0);
  auto reader = [&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    do {
      ControlStatusSnapshot status;
      copyControlStatus(status);
      if (status.activeCycle &&
          (status.shotCurve.count != 1 || curvePoint(status.shotCurve, 0).atMs != status.cycleId ||
           curvePoint(status.shotCurve, 0).cg != shotLogWeightToCentigrams(
               static_cast<float>(status.cycleId % 100)))) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
      if ((status.activeCycle &&
           (status.cycleId == 0 || status.source != ControlSource::WEB)) ||
          (!status.activeCycle &&
           (status.cycleId != 0 || status.source != ControlSource::NONE))) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }

      ControlGateSnapshot gate;
      copyControlGate(gate);
      if (!gate.maintenanceLeaseActive || gate.maintenanceLeaseId == 0 ||
          (gate.activeCycle && gate.source != ControlSource::WEB) ||
          (!gate.activeCycle && gate.source != ControlSource::NONE)) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }

      RuntimeConfig config;
      copyRuntimeConfig(&config);
      if (config.goalWeightDg != 200U + config.revision % 400U ||
          config.dripDelayMs != (config.revision ^ 0x5a5a5a5aU)) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }

      ShotPresetBank bank;
      copyPresetBank(&bank);
      const ShotPreset *active = findShotPreset(bank, bank.activeId);
      if (active == nullptr ||
          active->operationalWallMs != 10000U + active->goalWeightDg) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }

      RecipeSnapshot recipe;
      copyRecipeSnapshot(&recipe);
      const ShotPreset *recipeActive =
          findShotPreset(recipe.presets, recipe.presets.activeId);
      if (recipeActive == nullptr ||
          recipeActive->goalWeightDg != recipe.runtime.goalWeightDg) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }

      TaskProfilerSnapshot profiler;
      taskProfiler.copySnapshot(profiler);
      if (profiler.rowCount != 1 ||
          profiler.elapsedMs != profiler.sampleCount * 2U ||
          profiler.rows[0].taskNumber != profiler.sampleCount) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
    } while (!done.load(std::memory_order_acquire));
  };

  std::thread firstReader(reader);
  std::thread secondReader(reader);
  std::thread writer([&]() {
    start.store(true, std::memory_order_release);
    for (uint32_t generation = 2; generation <= kIterations; ++generation) {
      publishInvariant(generation);
    }
    done.store(true, std::memory_order_release);
  });
  writer.join();
  firstReader.join();
  secondReader.join();
  CHECK(violations.load(std::memory_order_relaxed) == 0);

  ControlStatusSnapshot status;
  controlStatusPublishRequested = true;
  copyControlStatus(status);
  CHECK(status.snapshotStale);
  controlStatusPublishRequested = false;
  copyControlStatus(status);
  CHECK(!status.snapshotStale);
}

void f03_safety_event_flags_preserve_consumed_requests() {
  resetHarness(false, true);
  constexpr uint32_t kIterations = 5000;

  auto exerciseConsumedSignal = [](auto publish, auto pending, auto consume) {
    std::atomic<bool> start{false};
    uint32_t consumed = 0;

    std::thread producer([&]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (uint32_t i = 0; i < kIterations; ++i) {
        while (pending()) {
          std::this_thread::yield();
        }
        publish();
      }
    });
    std::thread consumer([&]() {
      start.store(true, std::memory_order_release);
      while (consumed < kIterations) {
        if (consume()) {
          ++consumed;
        } else {
          std::this_thread::yield();
        }
      }
    });

    producer.join();
    consumer.join();
    return consumed;
  };

  CHECK(exerciseConsumedSignal(
            []() { requestSafeRestart(); },
            []() { return safeRestartPending(); },
            []() { return consumeSafeRestartRequest(); }) == kIterations);
  safetyEventFlags.clear(SAFETY_EVENT_CRITICAL_TASK_WATCHDOG);
  std::atomic<bool> startFaultWriters{false};
  auto faultWriter = [&]() {
    while (!startFaultWriters.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (uint32_t i = 0; i < kIterations; ++i) {
      reportTaskWatchdogFault();
    }
  };
  std::thread firstFaultWriter(faultWriter);
  std::thread secondFaultWriter(faultWriter);
  startFaultWriters.store(true, std::memory_order_release);
  firstFaultWriter.join();
  secondFaultWriter.join();
  CHECK(criticalTaskWatchdogFaulted());
}

void f04_gptimer_state_serializes_stop_against_expiry() {
  resetHarness(false, true);
  IndependentSafetyTimer timer;
  std::atomic<uint32_t> callbacks{0};
  CHECK(timer.begin(
      [](void *context) {
        static_cast<std::atomic<uint32_t> *>(context)->fetch_add(
            1U, std::memory_order_relaxed);
      },
      &callbacks));
  CHECK(timer.ready());

  constexpr uint32_t kIterations = 5000;
  std::atomic<uint32_t> generation{0};
  std::atomic<uint32_t> workersDone{0};
  bool invariantFailure = false;

  std::thread stopper([&]() {
    for (uint32_t expected = 1; expected <= kIterations; ++expected) {
      while (generation.load(std::memory_order_acquire) < expected) {
        std::this_thread::yield();
      }
      (void)timer.stop();
      workersDone.fetch_add(1U, std::memory_order_release);
    }
  });
  std::thread expiry([&]() {
    for (uint32_t expected = 1; expected <= kIterations; ++expected) {
      while (generation.load(std::memory_order_acquire) < expected) {
        std::this_thread::yield();
      }
      timer.serviceForHostAt(std::numeric_limits<uint64_t>::max());
      // A delivered hardware event must never dispatch the callback twice.
      timer.serviceForHostAt(std::numeric_limits<uint64_t>::max());
      workersDone.fetch_add(1U, std::memory_order_release);
    }
  });

  for (uint32_t current = 1; current <= kIterations; ++current) {
    const uint32_t expectedDone = (current - 1U) * 2U;
    while (workersDone.load(std::memory_order_acquire) < expectedDone) {
      std::this_thread::yield();
    }
    const uint32_t before = callbacks.load(std::memory_order_relaxed);
    if (!timer.arm(1)) {
      invariantFailure = true;
    }
    generation.store(current, std::memory_order_release);
    while (workersDone.load(std::memory_order_acquire) < current * 2U) {
      std::this_thread::yield();
    }
    const uint32_t after = callbacks.load(std::memory_order_relaxed);
    if ((after != before && after != before + 1U) || timer.running()) {
      invariantFailure = true;
    }
  }

  stopper.join();
  expiry.join();
  CHECK(!invariantFailure);

  // Without a competing stop, every arm expires exactly once.
  for (uint32_t i = 0; i < 100; ++i) {
    const uint32_t before = callbacks.load(std::memory_order_relaxed);
    CHECK(timer.arm(1));
    timer.serviceForHostAt(std::numeric_limits<uint64_t>::max());
    timer.serviceForHostAt(std::numeric_limits<uint64_t>::max());
    CHECK(callbacks.load(std::memory_order_relaxed) == before + 1U);
  }
}

void f04b_gptimer_callback_runs_unlocked_and_blocks_rearm() {
  resetHarness(false, true);
  IndependentSafetyTimer timer;
  struct CallbackState {
    IndependentSafetyTimer *timer;
    bool stopSucceeded;
    bool armSucceeded;
  } state{&timer, true, true};
  CHECK(timer.begin(
      [](void *context) {
        auto &state = *static_cast<CallbackState *>(context);
        state.stopSucceeded = state.timer->stop();
        state.armSucceeded = state.timer->arm(1);
      },
      &state));
  CHECK(timer.arm(1));
  timer.serviceForHostAt(std::numeric_limits<uint64_t>::max());
  CHECK(!state.stopSucceeded);
  CHECK(!state.armSucceeded);
  CHECK(!timer.running());
  CHECK(timer.arm(1));
}

void f05_settings_persistence_init_is_transactional() {
  resetHarness(false, false);
  reachReadyFromBoot();
  releaseSettingsPersistenceWorkerForHost();

  hostSettingsPersistQueueCreateSucceeds = false;
  CHECK(!initializeSettingsPersistenceWorker());
  CHECK(settingsPersistQueue == nullptr);
  CHECK(settingsPersistTaskHandle == nullptr);
  CHECK(!settingsPersistenceAvailable());
  CHECK(hostSettingsPersistRollbackDeletes == 0);

  hostSettingsPersistQueueCreateSucceeds = true;
  hostSettingsPersistTaskCreateSucceeds = false;
  CHECK(!initializeSettingsPersistenceWorker());
  CHECK(settingsPersistQueue == nullptr);
  CHECK(settingsPersistTaskHandle == nullptr);
  CHECK(!settingsPersistenceAvailable());
  CHECK(hostSettingsPersistRollbackDeletes == 1);

  const RuntimeConfig before = runtimeConfig;
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.requestId = 505;
  update.config = before;
  update.config.goalWeightDg = before.goalWeightDg + 1U;
  hostForwardAcceptedNetworkCommandSucceeds = false;
  processWebCommand(update);
  CHECK(runtimeConfig.revision == before.revision);
  CHECK(runtimeConfig.goalWeightDg == before.goalWeightDg);
  CHECK(controlResultPending);
  CHECK(controlResultCommand.requestId == update.requestId);
  CHECK(controlResultCommand.resultState == CommandResultState::FAILED);

  controlResultPending = false;
  WebCommand clearPreferred;
  clearPreferred.type = WebCommandType::CLEAR_PREFERRED_SCALE;
  clearPreferred.requestId = 506;
  processWebCommand(clearPreferred);
  CHECK(controlResultPending);
  CHECK(controlResultCommand.requestId == clearPreferred.requestId);
  CHECK(controlResultCommand.resultState == CommandResultState::FAILED);

  hostSettingsPersistTaskCreateSucceeds = true;
  CHECK(initializeSettingsPersistenceWorker());
  CHECK(settingsPersistenceAvailable());
  CHECK(settingsPersistQueue != nullptr);
  CHECK(settingsPersistTaskHandle != nullptr);
  const QueueHandle_t readyQueue = settingsPersistQueue;
  const TaskHandle_t readyTask = settingsPersistTaskHandle;
  CHECK(initializeSettingsPersistenceWorker());
  CHECK(settingsPersistQueue == readyQueue);
  CHECK(settingsPersistTaskHandle == readyTask);
  CHECK(settingsPersistenceAvailable());
}

void f06_boot_capability_policy_is_fail_closed() {
  BootCapabilities healthy;
  healthy.evaluated = true;
  healthy.platformClock = true;
  healthy.relaySafetyTimers = true;
  healthy.taskWatchdog = true;
  healthy.persistentStorage = true;
  healthy.settingsLoaded = true;
  healthy.settingsPersistenceWorker = true;
  healthy.scaleWorker = true;
  healthy.webCommandQueue = true;
  healthy.network = true;
  healthy.psram = true;
  CHECK(healthy.state() == BootState::READY);

  bool BootCapabilities::*const mandatory[] = {
      &BootCapabilities::platformClock,
      &BootCapabilities::relaySafetyTimers,
      &BootCapabilities::taskWatchdog,
      &BootCapabilities::persistentStorage,
      &BootCapabilities::settingsLoaded,
      &BootCapabilities::settingsPersistenceWorker,
      &BootCapabilities::scaleWorker,
  };
  for (bool BootCapabilities::*capability : mandatory) {
    BootCapabilities failed = healthy;
    failed.*capability = false;
    CHECK(failed.state() == BootState::DEGRADED_SAFE);
  }

  BootCapabilities offline = healthy;
  offline.webCommandQueue = false;
  offline.network = false;
  offline.psram = false;
  CHECK(offline.state() == BootState::READY);
  CHECK(!offline.optionalConnectivityReady());

  BootCapabilities faulted = healthy;
  faulted.criticalFaultLatched = true;
  CHECK(faulted.state() == BootState::FAULT_LATCHED);
  CHECK(BootCapabilities{}.state() == BootState::BOOTING);

  auto bootRefusesRelayClose = [](BootState expected) {
    deleteHostResources();
    setup();
    const bool stateMatches =
        bootState == expected && publishedControlStatus.bootState == expected &&
        !firmwareInitializationComplete && bootDegraded;
    const bool rejected = !setMachineCircuitClosed(true, 5000);
    return stateMatches && rejected && !getRelaySafetySnapshot().closed;
  };

  resetHarness(false, false);
  hostCpuFrequencySetSucceeds = false;
  CHECK(bootRefusesRelayClose(BootState::FAULT_LATCHED));

  resetHarness(false, false);
  hostEspTimerCreateSucceeds = false;
  CHECK(bootRefusesRelayClose(BootState::FAULT_LATCHED));

  resetHarness(false, false);
  hostTaskWatchdogOperationsSucceed = false;
  CHECK(bootRefusesRelayClose(BootState::FAULT_LATCHED));

  resetHarness(false, false);
  EEPROM.beginSucceeds = false;
  CHECK(bootRefusesRelayClose(BootState::DEGRADED_SAFE));

  resetHarness(false, false);
  hostSettingsPersistTaskCreateSucceeds = false;
  CHECK(bootRefusesRelayClose(BootState::DEGRADED_SAFE));
  CHECK(settingsPersistQueue == nullptr);
  CHECK(settingsPersistTaskHandle == nullptr);

  resetHarness(false, false);
  setScaleWorkerBleReadyForHost(false);
  CHECK(bootRefusesRelayClose(BootState::DEGRADED_SAFE));

  resetHarness(false, false);
  reportTaskWatchdogFault();
  CHECK(bootRefusesRelayClose(BootState::FAULT_LATCHED));
}

void f06b_control_gate_derives_effective_boot_state() {
  resetHarness(false, false);
  bootState = BootState::READY;
  bootCapabilities.evaluated = false;
  reportTaskWatchdogFault();
  publishControlGate();
  CHECK(publishedControlGate.bootState == BootState::BOOTING);

  bootCapabilities.evaluated = true;
  safetyEventFlags.clear(SAFETY_EVENT_CRITICAL_TASK_WATCHDOG);
  publishControlGate();
  CHECK(publishedControlGate.bootState == BootState::READY);

  reportTaskWatchdogFault();
  publishControlGate();
  CHECK(publishedControlGate.bootState == BootState::FAULT_LATCHED);

  resetHarness(false, false);
  relaySafetyState = RelaySafetyState::LOCKOUT;
  publishControlGate();
  CHECK(publishedControlGate.bootState == BootState::FAULT_LATCHED);
}

void f18_guard_early_rejection_matches_original_predicates() {
  resetHarness(false, true);
  GuardInputs inputs;
  inputs.stablyOff = true;
  for (unsigned bankCase = 0; bankCase != 4; ++bankCase) {
    seedDefaultShotPresetBank(presetBank);
    presetBank.count = bankCase == 3 ? 0 : 2;
    presetBank.activeId = bankCase < 2 ? presetBank.presets[bankCase].id : 255;
    for (unsigned mask = 0; mask != 256; ++mask) {
      runtimeConfig.timerOnly = (mask & 1U) != 0;
      presetBank.presets[0].brewByWeight = (mask & 2U) != 0;
      runtimeConfig.cupProtectionEnabled = (mask & 4U) != 0;
      runtimeConfig.requireCupToStart = (mask & 8U) != 0;
      presetBank.presets[1].brewByWeight = !presetBank.presets[0].brewByWeight;
      inputs.scaleUsable = (mask & 16U) != 0;
      noScaleShotGuardArmed = (mask & 32U) != 0;
      noScaleShotGuardHold = (mask & 64U) != 0;
      session.active = (mask & 128U) != 0;
      for (uint8_t mode = 0; mode != 3; ++mode) {
        runtimeConfig.noScaleBbwMode = mode;
        const RuntimeConfig effective = effectiveRuntimeConfig();
        const bool oldNoScale = noScaleBbwEnabled(mode) && !effective.timerOnly &&
                                !inputs.scaleUsable && noScaleShotGuardArmed;
        for (unsigned cup = 0; cup != 3; ++cup) {
          inputs.cup = static_cast<CupPresenceState>(cup);
          const bool oldCup = effective.cupProtectionEnabled &&
                              effective.requireCupToStart && !effective.timerOnly &&
                              inputs.scaleUsable && inputs.cup != CupPresenceState::PRESENT;
          CHECK(noScaleShotGuardWouldBlock(inputs) == oldNoScale);
          CHECK(cupStartGuardWouldBlock(inputs) == oldCup);
          CHECK(guardsWouldBlockActivatorDrive(inputs) ==
                (noScaleShotGuardHold || oldNoScale || oldCup));
        }
        resetNoScaleRequireBypassGesture();
        serviceNoScaleRequireBypassGesture(inputs);
        const bool oldEligible = noScaleBbwRequiresScale(mode) &&
                                 !effective.timerOnly && !inputs.scaleUsable &&
                                 !session.active && noScaleShotGuardArmed;
        CHECK(noScaleRequireBypassReady == oldEligible);
      }
    }
  }
}

void f14_relay_timer_initialization_rolls_back_partial_handles() {
  deleteHostResources();
  hostEspTimerCreateSucceeds = true;
  hostEspTimerCreateCalls = 0;
  hostEspTimerCreateFailAtCall = 2;
  CHECK(!initializeRelaySafetyTimer());
  CHECK(relaySafetyTimer == nullptr);
  CHECK(operationalLimitTimer == nullptr);
  CHECK(!independentSafetyTimer.ready());
  hostEspTimerCreateFailAtCall = 0;
  CHECK(initializeRelaySafetyTimer());
  CHECK(relaySafetyTimer != nullptr);
  CHECK(operationalLimitTimer != nullptr);
  CHECK(independentSafetyTimer.ready());
}

void f13_schedule_contract_and_snapshot_evidence_are_explicit() {
  CHECK(TASK_SCHEDULE_CONTRACT_COUNT == 10);
  CHECK(strcmp(TASK_SCHEDULE_CONTRACTS[9].name, "ota_worker") == 0);
  CHECK(TASK_SCHEDULE_CONTRACTS[9].configuredStackBytes ==
        TASK_SCHEDULE_CONTRACTS[5].configuredStackBytes);
  CHECK(!TASK_SCHEDULE_CONTRACTS[9].watchdogSubscribed);
  CHECK(strcmp(TASK_SCHEDULE_CONTRACTS[0].name, "control") == 0);
  CHECK(TASK_SCHEDULE_CONTRACTS[0].core == CONTROL_TASK_CORE);
  CHECK(TASK_SCHEDULE_CONTRACTS[0].serviceDeadlineMs ==
        CONTROL_SERVICE_DEADLINE_MS);
  CHECK(TASK_SCHEDULE_CONTRACTS[0].configuredStackBytes == 8192);
  CHECK(TASK_SCHEDULE_CONTRACTS[0].executionBudgetUs ==
        CONTROL_EXECUTION_BUDGET_US);
  CHECK(TASK_SCHEDULE_CONTRACTS[1].core == SCALE_WORKER_TASK_CORE);
  CHECK(TASK_SCHEDULE_CONTRACTS[1].serviceDeadlineMs ==
        SCALE_SERVICE_DEADLINE_MS);
  CHECK(TASK_SCHEDULE_CONTRACTS[2].priorityOffset == 0);
  CHECK(TASK_SCHEDULE_CONTRACTS[2].watchdogSubscribed);
  CHECK(TASK_SCHEDULE_CONTRACTS[2].core == PERSISTENCE_TASK_CORE);
  CHECK(strcmp(TASK_SCHEDULE_CONTRACTS[3].name, "health") == 0);
  CHECK(TASK_SCHEDULE_CONTRACTS[3].core == 0);
  CHECK(strcmp(TASK_SCHEDULE_CONTRACTS[5].name, "httpd") == 0);
  CHECK(TASK_SCHEDULE_CONTRACTS[5].configuredStackBytes == 11264);
  CHECK(strcmp(TASK_SCHEDULE_CONTRACTS[7].name, "micra_cloud") == 0);
  CHECK(TASK_SCHEDULE_CONTRACTS[7].maxBlockingMs == 10000);
  CHECK(TASK_SCHEDULE_CONTRACTS[8].maxBlockingMs ==
        TASK_BLOCKING_UNBOUNDED_MS);

  resetHarness(false, true);
  publishControlStatus();
  ControlStatusSnapshot first;
  copyControlStatus(first);
  loopDeadlineMisses = 3;
  publishControlStatus();
  ControlStatusSnapshot second;
  copyControlStatus(second);
  CHECK(static_cast<uint32_t>(second.snapshotVersion - first.snapshotVersion) ==
        1U);
  CHECK(second.uptimeMs >= first.uptimeMs);
  CHECK(second.loopDeadlineMisses == 3);

  // The deadline bounds lateness beyond the requested no-scale delay.
  resetHarness(false, false);
  runLoopAfter(LOOP_NO_SCALE_DELAY_MS);
  loopDeadlineMisses = 0;
  runLoopAfter(LOOP_NO_SCALE_DELAY_MS + CONTROL_SERVICE_DEADLINE_MS);
  CHECK(loopDeadlineMisses == 0);
  runLoopAfter(LOOP_NO_SCALE_DELAY_MS + CONTROL_SERVICE_DEADLINE_MS + 1);
  CHECK(loopDeadlineMisses == 1);

  resetScaleWorkerMetricsForHost();
  hostTaskNotifyGiveCalls = 0;
  setScaleWorkerTaskPresentForHost(true);
  RuntimeConfig policy = {};
  publishScaleWorkerPolicy(policy, true);
  CHECK(hostTaskNotifyGiveCalls == 1);
}

void f13b_status_curve_publishes_live_samples_and_clears_tail() {
  resetHarness(false, false);
  pendingFinalize = PendingShotFinalize{};
  pendingFinalize.cycleId = 77;
  homeCycleResult = {};
  homeCycleResult.valid = true;
  homeCycleResult.cycleId = 77;
  session.active = false;
  ControlStatusSnapshot status;
  for (const uint16_t points : {5, 2}) {
    resetShotCurveRecord(pendingFinalize.curve);
    for (uint16_t i = 0; i < points; ++i)
      CHECK(appendShotCurveObservation(pendingFinalize.curve, 1.0f + i,
                                       i * 50U, false));
    publishControlStatus();
    copyControlStatus(status);
    CHECK(status.shotCurve.count == points &&
          status.shotCurve.bytes == pendingFinalize.curve.bytes);
    for (uint16_t i = 0; i < points; ++i)
      CHECK(curvePoint(status.shotCurve, i).cg == curvePoint(pendingFinalize.curve, i).cg &&
            curvePoint(status.shotCurve, i).atMs == curvePoint(pendingFinalize.curve, i).atMs);
  }
  // Idle (no live or just-ended activity) publishes no curve at all.
  homeCycleResult.valid = false;
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.shotCurve.count == 0 && status.shotCurve.bytes == 0);
}

void f17_health_counters_are_atomic_and_monotonic() {
  constexpr uint32_t kWriters = 4;
  constexpr uint32_t kIterations = 500;
  const uint32_t allocBefore = allocExternalOkCount();
  const uint32_t flashBefore = flashIoLockTimeouts();
  std::atomic<bool> start{false};
  std::atomic<uint32_t> finished{0};
  std::atomic<bool> monotonic{true};

  std::thread reader([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    uint32_t lastAlloc = allocBefore;
    uint32_t lastFlash = flashBefore;
    while (finished.load(std::memory_order_acquire) != kWriters) {
      const uint32_t alloc = allocExternalOkCount();
      const uint32_t flash = flashIoLockTimeouts();
      if (alloc < lastAlloc || flash < lastFlash) monotonic = false;
      lastAlloc = alloc;
      lastFlash = flash;
    }
  });
  std::thread writers[kWriters];
  for (std::thread &writer : writers) {
    writer = std::thread([&]() {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (uint32_t i = 0; i < kIterations; ++i) {
        void *block = allocExternal(8);
        heapCapsFree(block);
        flashIoLockTimeoutCount().fetch_add(1, std::memory_order_relaxed);
      }
      finished.fetch_add(1, std::memory_order_release);
    });
  }
  start.store(true, std::memory_order_release);
  for (std::thread &writer : writers) writer.join();
  reader.join();
  CHECK(monotonic.load());
  CHECK(allocExternalOkCount() == allocBefore + kWriters * kIterations);
  CHECK(flashIoLockTimeouts() == flashBefore + kWriters * kIterations);
}


void s04b_shot_log_page_slice() {
  size_t start = 99;
  size_t pageCount = 99;
  CHECK(shotLogPageSlice(120, 0, 10, start, pageCount) == true);
  CHECK(start == 0);
  CHECK(pageCount == 10);
  CHECK(shotLogPageSlice(120, 110, 10, start, pageCount) == false);
  CHECK(start == 110);
  CHECK(pageCount == 10);
  CHECK(shotLogPageSlice(120, 115, 10, start, pageCount) == false);
  CHECK(start == 115);
  CHECK(pageCount == 5);
  CHECK(shotLogPageSlice(5, 0, 10, start, pageCount) == false);
  CHECK(start == 0);
  CHECK(pageCount == 5);
  CHECK(shotLogPageSlice(0, 0, 10, start, pageCount) == false);
  CHECK(start == 0);
  CHECK(pageCount == 0);
  CHECK(shotLogPageSlice(20, 50, 10, start, pageCount) == false);
  CHECK(start == 20);
  CHECK(pageCount == 0);
  CHECK(shotLogClampPageLimit(0) == 1);
  CHECK(shotLogClampPageLimit(10) == 10);
  CHECK(shotLogClampPageLimit(100) == 100);
  CHECK(shotLogClampPageLimit(999) == SHOT_LOG_CAPACITY);
  CHECK(SHOT_LOG_PAGE_DEFAULT == 10);
}

void s04c_shot_list_filter_groups() {
  ShotLogRecord timed = {}, manual = {};
  timed.arrivalDs = 280;
  timed.verdict = 1;  // fast
  shotLogSetPresetId(timed, 3);
  snprintf(timed.scaleName, sizeof(timed.scaleName), "%s", "Lunar");
  manual.arrivalDs = SHOT_LOG_METRIC_MISSING;
  shotLogSetPresetId(manual, 200);
  CHECK(shotLogResult(timed) == 1 && shotLogResult(manual) == 3);
  ShotListFilter filter;
  CHECK(shotListFilterMatches(filter, timed) && shotListFilterMatches(filter, manual));
  // A result filter leaves out every shot without a time to target.
  filter.results = 1U << 1;
  CHECK(shotListFilterMatches(filter, timed) && !shotListFilterMatches(filter, manual));
  filter.results = 1U << 0;
  CHECK(!shotListFilterMatches(filter, timed));
  // Options within a group OR; groups AND.
  filter = {};
  filter.presets[filter.presetCount++] = 7;
  filter.presets[filter.presetCount++] = 200;
  CHECK(!shotListFilterMatches(filter, timed) && shotListFilterMatches(filter, manual));
  filter.presets[0] = 3;
  filter.scales[filter.scaleCount++] = shotLogScaleKey(timed);
  CHECK(shotListFilterMatches(filter, timed) && !shotListFilterMatches(filter, manual));
  // List pages leave out stored yields under 1 g; a missing yield stays.
  ShotListFilter listed;
  listed.hideLowYield = true;
  timed.actualWeightCg = 99;
  manual.actualWeightCg = SHOT_LOG_WEIGHT_MISSING;
  CHECK(!shotListFilterMatches(listed, timed) && shotListFilterMatches(listed, manual));
  CHECK(shotListFilterMatches(ShotListFilter{}, timed));
  timed.actualWeightCg = 100;
  CHECK(shotListFilterMatches(listed, timed));
  CHECK(shotListFilterHash(listed) != shotListFilterHash(ShotListFilter{}));
  // Equal filters share an identity; group boundaries are part of it.
  ShotListFilter same = filter, moved = {};
  moved.presets[moved.presetCount++] = 3;
  moved.presets[moved.presetCount++] = 200;
  CHECK(shotListFilterHash(same) == shotListFilterHash(filter));
  CHECK(shotListFilterHash(moved) != shotListFilterHash(filter));
  CHECK(shotListFilterHash(ShotListFilter{}) != shotListFilterHash(moved));
}

void s04f_shot_log_sort_date_and_rating() {
  CHECK(shotLogSortFromName("rating") == ShotLogSort::Rating);
  CHECK(shotLogSortFromName("date") == ShotLogSort::Date);
  CHECK(shotLogSortFromName(nullptr) == ShotLogSort::Date);
  CHECK(shotLogSortFromName("nope") == ShotLogSort::Date);
  CHECK(shotLogSortDirFromName("asc") == ShotLogSortDir::Asc);
  CHECK(shotLogSortDirFromName("desc") == ShotLogSortDir::Desc);
  CHECK(shotLogSortDirFromName(nullptr) == ShotLogSortDir::Desc);

  ShotLogRecord recs[5] = {};
  recs[0].id = 5;
  recs[0].sideFlags = shotLogPackRating(0, 3);
  recs[1].id = 4;
  recs[1].sideFlags = shotLogPackRating(0, 5);
  recs[2].id = 3;
  recs[2].sideFlags = shotLogPackRating(0, 0);
  recs[3].id = 2;
  recs[3].sideFlags = shotLogPackRating(0, 5);
  recs[4].id = 1;
  recs[4].sideFlags = shotLogPackRating(0, 1);

  ShotLogRecord dateDesc[5];
  memcpy(dateDesc, recs, sizeof(recs));
  shotLogSortRecords(dateDesc, 5, ShotLogSort::Date, ShotLogSortDir::Desc);
  CHECK(dateDesc[0].id == 5);
  CHECK(dateDesc[4].id == 1);

  ShotLogRecord dateAsc[5];
  memcpy(dateAsc, recs, sizeof(recs));
  shotLogSortRecords(dateAsc, 5, ShotLogSort::Date, ShotLogSortDir::Asc);
  CHECK(dateAsc[0].id == 1);
  CHECK(dateAsc[4].id == 5);

  ShotLogRecord ratingDesc[5];
  memcpy(ratingDesc, recs, sizeof(recs));
  shotLogSortRecords(ratingDesc, 5, ShotLogSort::Rating, ShotLogSortDir::Desc);
  CHECK(ratingDesc[0].id == 4);
  CHECK(shotLogRating(ratingDesc[0].sideFlags) == 5);
  CHECK(ratingDesc[1].id == 2);
  CHECK(shotLogRating(ratingDesc[1].sideFlags) == 5);
  CHECK(ratingDesc[2].id == 5);
  CHECK(shotLogRating(ratingDesc[2].sideFlags) == 3);
  CHECK(ratingDesc[3].id == 1);
  CHECK(shotLogRating(ratingDesc[3].sideFlags) == 1);
  CHECK(ratingDesc[4].id == 3);
  CHECK(shotLogRating(ratingDesc[4].sideFlags) == 0);

  ShotLogRecord ratingAsc[5];
  memcpy(ratingAsc, recs, sizeof(recs));
  shotLogSortRecords(ratingAsc, 5, ShotLogSort::Rating, ShotLogSortDir::Asc);
  CHECK(ratingAsc[0].id == 1);
  CHECK(shotLogRating(ratingAsc[0].sideFlags) == 1);
  CHECK(ratingAsc[1].id == 5);
  CHECK(shotLogRating(ratingAsc[1].sideFlags) == 3);
  CHECK(ratingAsc[2].id == 4);
  CHECK(shotLogRating(ratingAsc[2].sideFlags) == 5);
  CHECK(ratingAsc[3].id == 2);
  CHECK(shotLogRating(ratingAsc[3].sideFlags) == 5);
  CHECK(ratingAsc[4].id == 3);
  CHECK(shotLogRating(ratingAsc[4].sideFlags) == 0);

  shotLogSortRecords(nullptr, 5, ShotLogSort::Rating, ShotLogSortDir::Desc);
  shotLogSortRecords(recs, 0, ShotLogSort::Rating, ShotLogSortDir::Desc);
  shotLogSortRecords(recs, 1, ShotLogSort::Rating, ShotLogSortDir::Desc);
  CHECK(recs[0].id == 5);
}

void s06_shot_log_local_sec_from_utc() {
  CHECK(shotLogLocalSecFromUtc(1'700'000'000U, -240) ==
        1'700'000'000U - 14400U);
  CHECK(shotLogLocalSecFromUtc(1'700'000'000U, 60) == 1'700'000'000U + 3600U);
}

void s07_shot_log_stores_fixed_wall_time() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 120;
  record.hasWallTime = 1;
  record.endedAtUnixSec = 1'700'000'000U;
  record.endedAtLocalSec = shotLogLocalSecFromUtc(1'700'000'000U, -240);
  record.timezoneOffsetMinutesAtCommit = -240;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(stored[0].hasWallTime == 1);
  CHECK(stored[0].endedAtLocalSec ==
        shotLogLocalSecFromUtc(1'700'000'000U, -240));
  CHECK(stored[0].timezoneOffsetMinutesAtCommit == -240);
}

void s08_shot_log_without_sync_has_no_wall_time() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 120;
  record.bootId = currentBootId();
  record.endedAtMs = 45000;
  record.hasWallTime = 0;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(stored[0].hasWallTime == 0);
  CHECK(stored[0].endedAtLocalSec == 0);
  CHECK(stored[0].endedAtUnixSec == 0);
}

void s11_shot_log_record_includes_preset_snapshot() {
  CHECK(sizeof(ShotLogRecord) == 124);
  CHECK(sizeof(ShotLogStore) % 4 == 0);
  // A fast weight cut records its own cut parameters, cut line and verdict.
  resetHarness(false, true);
  shotLog.clear();
  PendingShotFinalize snapshot = {};
  snapshot.cycleId = 9;
  snapshot.durationDs = 245;
  snapshot.firstDropDs = 60;
  snapshot.goalWeightDg = 360;
  snapshot.startedWithScale = true;
  snapshot.automaticBrew = true;
  snapshot.scaleBaselineReady = true;
  snapshot.finalState = StopperState::BREW;
  snapshot.endReason = EndReason::FAST_EXTRACTION_MAX_WEIGHT;
  snapshot.extractionGuardEnabled = true;
  snapshot.slowExtractionGuardEnabled = true;
  snapshot.extractionExtended = true;
  snapshot.fastMarginDg = 40;
  snapshot.slowMarginDg = 30;
  snapshot.fastBeforeMs = 27000;
  snapshot.slowAfterMs = 34000;
  snapshot.protectionMs = 12000;
  snapshot.wallMs = 50000;
  snapshot.dripDelayMs = 0;
  snapshot.rateAtStopX100 = 2108;  // 2.108 g/s at the stop command.
  commitPendingShotLog(snapshot, 39.0f, true, ActualWeightSource::POST_DRIP);
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  const ShotLogRecord &r = stored[0];
  CHECK(r.cutLineCg == 4000);  // Ceiling before t1 (step guards, D = 0).
  CHECK(r.fastBeforeDs == 270 && r.slowAfterDs == 340 && r.delayDs == 120);
  CHECK(r.wallDs == 500 && r.fastMarginDg == 40 && r.slowMarginDg == 30);
  CHECK(r.cutRampDs == 0 && shotLogFastGuardEnabled(r.sideFlags));
  CHECK(r.dripDelayCode == 1);  // A 0 s drip delay stays distinct from "not recorded".
  CHECK(corridor::verdictOf(r.verdict) == corridor::Verdict::FAST);
  CHECK(r.arrivalDs == 226);  // 24.5 s - 4 g / 2.108 g/s.
  CHECK(shotLogFastExtended(r.extractionExtended) && !shotLogSlowExtended(r.extractionExtended));
  CHECK(r.errorCg == -100);  // 39 g against the 40 g cut line, not the target.
  // Channeling caps the line at the target and marks the record.
  shotLog.clear();
  snapshot.channeling = true;
  commitPendingShotLog(snapshot, 39.0f, true, ActualWeightSource::POST_DRIP);
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(stored[0].cutLineCg == 3600 && corridor::channelingOf(stored[0].verdict));
  snapshot.channeling = false;
  // A latched weight cut records its exact line and judges the arrival from
  // its exact moment, not the decisecond duration: 24.75 s at 2.0 g/s against
  // the 40 g step ceiling arrives at 22.75 s (the duration alone gives 22.6 s).
  shotLog.clear();
  snapshot.cutStopMs = 24750;
  snapshot.cutLineDg = 395;
  snapshot.rateAtStopX100 = 2000;
  commitPendingShotLog(snapshot, 39.0f, true, ActualWeightSource::POST_DRIP);
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(stored[0].cutLineCg == 3950);
  CHECK(stored[0].arrivalDs == 227);
  snapshot.cutStopMs = 0;
  snapshot.cutLineDg = 0;
  snapshot.rateAtStopX100 = 2108;
  // A wall stop is not classified.
  shotLog.clear();
  snapshot.endReason = EndReason::CONFIGURED_WALL_LIMIT;
  commitPendingShotLog(snapshot, 39.0f, true, ActualWeightSource::POST_DRIP);
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(corridor::verdictOf(stored[0].verdict) == corridor::Verdict::UNCLASSIFIED);
  CHECK(stored[0].arrivalDs == SHOT_LOG_METRIC_MISSING);
}

void s12_shot_rating_pack_preserves_guards() {
  const uint8_t flags = shotLogPackGuardFlags(true, true);
  CHECK(shotLogFastGuardEnabled(flags));
  CHECK(shotLogSlowGuardEnabled(flags));
  CHECK(shotLogRating(flags) == 0);
  const uint8_t rated = shotLogPackRating(flags, 4);
  CHECK(shotLogFastGuardEnabled(rated));
  CHECK(shotLogSlowGuardEnabled(rated));
  CHECK(shotLogRating(rated) == 4);
  CHECK(shotLogPackRating(rated, 0) == flags);
  CHECK(shotLogRating(shotLogPackRating(flags, 9)) == 5);
}

void s12b_shot_log_update_rating() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 120;
  record.sideFlags = shotLogPackGuardFlags(true, false);
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(shotLog.updateRating(stored[0].id, 3));
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(shotLogRating(stored[0].sideFlags) == 3);
  CHECK(shotLogFastGuardEnabled(stored[0].sideFlags));
  CHECK(!shotLogSlowGuardEnabled(stored[0].sideFlags));
  CHECK(!shotLog.updateRating(stored[0].id, 6));
  CHECK(!shotLog.updateRating(99999U, 1));
}

void s12c_finalize_links_exact_history_row() {
  resetHarness(false, true);
  shotLog.clear();
  PendingShotFinalize snapshot = {};
  snapshot.cycleId = 7;
  snapshot.durationDs = 300;
  snapshot.goalWeightDg = 360;
  snapshot.startedWithScale = true;
  snapshot.automaticBrew = true;
  snapshot.finalState = StopperState::READY;
  snapshot.endReason = EndReason::SCALE_THRESHOLD;
  snapshot.bootId = currentBootId();
  snapshot.logEligible = true;
  snapshot.firstDropDs = 50;

  snapshot.endedAtHasWallTime = true;
  snapshot.endedAtUtcSec = 1775357999U;
  snapshot.endedAtOffsetMinutes = -180;
  // Finalization occurs after the DST boundary, an NTP correction and a zone edit.
  strcpy(runtimeConfig.timezoneId, "Asia/Tokyo");
  g_wallClock.queueSyncFromCallback(1775358100U);
  CHECK(g_wallClock.applyPendingSync(hostMillis));
  persistLastShotFromFinalize(snapshot, 36.1f, true);
  commitPendingShotLog(snapshot, 36.1f, true, ActualWeightSource::POST_DRIP);
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(shotLogRating(stored[0].sideFlags) == 0);
  CHECK(persistedLastShot.shotLogId == stored[0].id);
  CHECK(persistedLastGoodShot.shotLogId == stored[0].id);
  CHECK(stored[0].endedAtUnixSec == snapshot.endedAtUtcSec);
  CHECK(stored[0].timezoneOffsetMinutesAtCommit == -180);
  CHECK(persistedLastShot.endedAtLocalSec == snapshot.endedAtUtcSec - 10800U);
  CHECK(stored[0].endedAtLocalSec == persistedLastShot.endedAtLocalSec);
}

void s12d_rate_last_shot_and_history() {
  resetHarness(false, true);
  shotLog.clear();
  CHECK(!rateLastShot(3));
  PersistedLastShot retained = {};
  retained.valid = true;
  retained.cycleId = 1;
  retained.durationMs = 13000;
  retained.weightValid = true;
  retained.currentWeightG = 36.0f;
  retained.presetId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  persistLastShotSnapshot(retained);
  CHECK(!rateLastShot(3));

  ShotLogRecord record = {};
  record.durationDs = 130;
  record.actualWeightCg = 3600;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  PersistedLastShot linked = persistedLastShot;
  linked.shotLogId = stored[0].id;
  persistLastShotSnapshot(linked);
  CHECK(rateLastShot(5));
  CHECK(persistedLastGoodShot.rating == 0);
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(shotLogRating(stored[0].sideFlags) == 5);
  CHECK(rateShotRecord(stored[0].id, 2));
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  CHECK(shotLogRating(stored[0].sideFlags) == 2);
  uint32_t bootId = 0;
  PersistedLastShot last = {}, good = {};
  HistoryRecord newestActivation = {};
  bool hasNewestActivation = false;
  CHECK(copyShotStoreStatus(bootId, last, good, newestActivation,
                            hasNewestActivation, true));
  CHECK(good.rating == 2);
  CHECK(good.shotLogId == stored[0].id);
}

void s12e_history_rating_wins_after_last_shot_save_failure_and_reboot() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 130;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);

  PersistedLastShot good = {};
  good.valid = true;
  good.cycleId = 3;
  good.durationMs = 13000;
  good.weightValid = true;
  good.currentWeightG = 37.2f;
  good.shotLogId = stored[0].id;
  good.rating = 1;
  good.presetId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  lastShotStore.advance(good);
  CHECK(lastShotStore.save());
  CHECK(rateShotRecord(stored[0].id, 4));

  PersistedLastShot staleWrite = good;
  staleWrite.rating = 4;
  lastShotStore.advance(staleWrite);
  LastShotStore::setHostSaveSucceeds(false);
  CHECK(!lastShotStore.save());
  LastShotStore::setHostSaveSucceeds(true);
  CHECK(lastShotStore.load());
  CHECK(persistedLastGoodShot.rating == 1);

  uint32_t bootId = 0;
  PersistedLastShot last = {}, publishedGood = {};
  HistoryRecord newestActivation = {};
  bool hasNewestActivation = false;
  CHECK(copyShotStoreStatus(bootId, last, publishedGood, newestActivation, hasNewestActivation, true));
  CHECK(publishedGood.rating == 4);
  CHECK(publishedGood.cycleId == good.cycleId);
  CHECK(fabsf(publishedGood.currentWeightG - good.currentWeightG) < 0.001f);
  publishControlStatus();
  ControlStatusSnapshot status = {};
  copyControlStatus(status);
  CHECK(status.lastGoodShotHistoryLinked);
  CHECK(status.lastGoodShot.rating == 4);
  CHECK(status.lastGoodShot.cycleId == good.cycleId);
  CHECK(fabsf(status.lastGoodShot.currentWeightG - good.currentWeightG) <
        0.001f);
}

void s12f_shot_store_snapshot_serializes_rating_and_finalize() {
  resetHarness(false, true);
  shotLog.clear();
  ShotLogRecord record = {};
  record.durationDs = 130;
  CHECK(shotLog.append(record));
  ShotLogRecord stored[1] = {};
  CHECK(shotLog.copyNewestFirst(stored, 1) == 1);
  PersistedLastShot initial = {};
  initial.valid = true;
  initial.cycleId = 1;
  initial.durationMs = 13001;
  initial.weightValid = true;
  initial.currentWeightG = 31.0f;
  initial.shotLogId = stored[0].id;
  initial.presetId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  persistLastShotSnapshot(initial);

  std::atomic<bool> start{false}, done{false};
  std::atomic<uint32_t> violations{0};
  std::thread rater([&]() {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (uint32_t n = 0; n < 500; ++n)
      if (!rateShotRecord(stored[0].id, static_cast<uint8_t>(n % 6U)))
        violations.fetch_add(1, std::memory_order_relaxed);
  });
  std::thread reader([&]() {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    do {
      uint32_t bootId = 0;
      PersistedLastShot last = {}, good = {};
      HistoryRecord newestActivation = {};
      bool hasNewestActivation = false;
      if (!copyShotStoreStatus(bootId, last, good, newestActivation,
                               hasNewestActivation, true) ||
          good.durationMs != 13000U + good.cycleId ||
          good.currentWeightG !=
              30.0f + static_cast<float>(good.cycleId % 5U))
        violations.fetch_add(1, std::memory_order_relaxed);
    } while (!done.load(std::memory_order_acquire));
  });
  start.store(true, std::memory_order_release);
  for (uint32_t generation = 2; generation <= 500; ++generation) {
    PersistedLastShot next = initial;
    next.cycleId = generation;
    next.durationMs = 13000U + generation;
    next.currentWeightG = 30.0f + static_cast<float>(generation % 5U);
    persistLastShotSnapshot(next);
    serviceShotStorePersistence();
  }
  rater.join();
  done.store(true, std::memory_order_release);
  reader.join();
  CHECK(violations.load(std::memory_order_relaxed) == 0);
}

void s12g_worker_acknowledgement_rechecks_generation_under_lock() {
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    resetHarness(false, true);
    CHECK(initializeSettingsPersistenceWorker());
    ShotLogRecord record = {};
    record.durationDs = 130;
    record.actualWeightCg = 3600;
    CHECK(shotLog.append(record, false));
    ShotCurveRecord curve = emptyShotCurveRecord();
    curve.shotId = 1;
    CHECK(appendShotCurvePoint(curve, 0, 0, false));
    CHECK(shotCurves.append(curve, false));
    activationStores.capturePersistenceImage(*shotStorePersistImage);
    shotStorePersistImageGeneration = shotStoreDirtyGeneration.load();
    persistShotStoreImage();
    CHECK(shotStorePersistResultReady && shotStorePersistResultOk);
    session.active = true; // Hold the next flush while checking acknowledgement.
    static thread_local unsigned selected;
    selected = mutation;
    TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
      if (mutex != &shotStoreMutex || !acquired) return;
      TaskMutex::hostObserver = nullptr;
      // Emulate a Web mutation that completed just before control got the lock.
      if (selected == 0) {
        CHECK(shotCurves.clear(false) && shotLog.clear(false));
      } else if (selected == 1) {
        CHECK(shotCurves.removeById(1, false) && shotLog.removeById(1, false));
      } else {
        CHECK(shotLog.updateRating(1, 5, false));
      }
      shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
    };
    serviceShotStorePersistence();
    TaskMutex::hostObserver = nullptr;
    CHECK(shotLog.dirty());
    CHECK(mutation == 2 || shotCurves.dirty());
    session.active = false;
    serviceShotStorePersistence();
    CHECK(!shotLog.dirty() && !shotCurves.dirty());
    CHECK(shotLog.load() && shotCurves.load());
    CHECK(shotLog.count() == (mutation == 2 ? 1U : 0U));
    CHECK(shotCurves.count() == (mutation == 2 ? 1U : 0U));
    if (mutation == 2) {
      CHECK(shotLog.copyNewestFirst(&record, 1) == 1);
      CHECK(shotLogRating(record.sideFlags) == 5);
    }
  }
}

void s12h_capped_curve_batch_keeps_generation_dirty_until_drained() {
  resetHarness(false, true);
  CHECK(initializeSettingsPersistenceWorker());
  constexpr uint32_t backlog = 2U * SHOT_CURVE_PERSIST_BATCH + 1U;
  ShotCurveRecord curve = emptyShotCurveRecord();
  CHECK(appendShotCurvePoint(curve, 0, 0, false));
  for (uint32_t id = 1; id <= backlog; ++id) {
    curve.shotId = id;
    CHECK(shotCurves.append(curve, false));
  }
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  for (int call = 0; call < 8 && shotCurves.dirty(); ++call) {
    serviceShotStorePersistence();
  }
  CHECK(!shotCurves.dirty());
  CHECK(shotStorePersistCleanGeneration == shotStoreDirtyGeneration.load());
  CHECK(shotCurves.load() && shotCurves.count() == backlog);
}

void s13_persist_debug_messages_identify_origin() {
  char message[128] = {};
  DebugEvent shotLogEvent = {};
  shotLogEvent.code = DebugCode::SHOT_LOG_PERSIST_FAILED;
  shotLogEvent.argument1 = 72;
  shotLogEvent.argument2 = 0;
  CHECK(formatPersistDebugMessage(shotLogEvent, message, sizeof(message)));
  CHECK(strstr(message, "shot history") != nullptr);
  CHECK(strstr(message, "shotlog/records") != nullptr);
  CHECK(strstr(message, "ShotLog.save") != nullptr);

  DebugEvent runtimeEvent = {};
  runtimeEvent.code = DebugCode::RUNTIME_PERSIST_FAILED;
  runtimeEvent.argument1 = 9;
  runtimeEvent.argument2 = RUNTIME_PERSIST_REASON_USER;
  CHECK(formatPersistDebugMessage(runtimeEvent, message, sizeof(message)));
  CHECK(strstr(message, "PERSIST_RUNTIME") != nullptr);
  CHECK(strstr(message, "settingsA/B") != nullptr);
  CHECK(strstr(message, "runtime config") != nullptr);
  CHECK(strstr(message, "learned weight offset") == nullptr);

  runtimeEvent.argument2 = RUNTIME_PERSIST_REASON_USER | RUNTIME_PERSIST_REASON_OFFSET;
  CHECK(formatPersistDebugMessage(runtimeEvent, message, sizeof(message)));
  CHECK(strstr(message, "learned weight offset") != nullptr);
  CHECK(strlen(message) < sizeof(message));
}

void sh01_activation_store_mutex_serializes_append_read_flush() {
  resetHarness(false, true);
  constexpr uint32_t kIterations = 400;
  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  std::atomic<uint32_t> violations{0};

  // Control-loop style append: O(1) PSRAM write under the store mutex.
  auto appender = [&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (uint32_t i = 0; i < kIterations; ++i) {
      HistoryRecord record = {};
      record.durationDs = static_cast<uint16_t>(100 + (i % 50));
      record.type = static_cast<uint8_t>(i % 3U);
      TaskLockGuard lock(shotStoreMutex);
      if (!historyLog.append(record, false)) {
        violations.fetch_add(1, std::memory_order_relaxed);
      }
      shotStoreDirtyGeneration.fetch_add(1, std::memory_order_relaxed);
    }
  };
  // HTTP-style page read: a page must never observe a torn record ordering.
  auto reader = [&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (!done.load(std::memory_order_acquire)) {
      HistoryPage page;
      TaskLockGuard lock(shotStoreMutex);
      historyLog.copyPage(page, 0, HISTORY_PAGE_DEFAULT,
                          ShotLogSortDir::Desc);
      for (size_t i = 1; i < page.count; ++i) {
        if (page.records[i - 1].id <= page.records[i].id) {
          violations.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      std::this_thread::yield();
    }
  };
  // Deferred flush servant over the same three owned stores.
  auto flusher = [&]() {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    while (!done.load(std::memory_order_acquire)) {
      TaskLockGuard lock(shotStoreMutex);
      (void)activationStores.serviceStep(0, addDebugEvent);
      std::this_thread::yield();
    }
  };

  std::thread appendThread(appender);
  std::thread readThread(reader);
  std::thread flushThread(flusher);
  start.store(true, std::memory_order_release);
  appendThread.join();
  done.store(true, std::memory_order_release);
  readThread.join();
  flushThread.join();
  CHECK(violations.load(std::memory_order_relaxed) == 0);
  TaskLockGuard lock(shotStoreMutex);
  CHECK(historyLog.count() == kIterations);
  HistoryPage page;
  historyLog.copyPage(page, 0, HISTORY_PAGE_DEFAULT, ShotLogSortDir::Desc);
  CHECK(page.total == kIterations);
  CHECK(page.records[0].id == kIterations);
}

void sh02_stats_snapshot_captures_one_store_generation() {
  resetHarness(false, true);
  ShotLogRecord record{};
  record.durationDs = 150;
  record.actualWeightCg = 3000;
  CHECK(shotLog.append(record, false));
  CHECK(copyShotRecords(&record, 1) == 1);
  ShotCurveRecord curve = emptyShotCurveRecord();
  curve.shotId = record.id;
  CHECK(appendShotCurvePoint(curve, record.actualWeightCg, 0, false));
  CHECK(shotCurves.append(curve, false));
  auto snapshot = std::make_unique<ShotStatsRecordsSnapshot>();
  static unsigned acquisitions;
  acquisitions = 0;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &shotStoreMutex && acquired) ++acquisitions;
  };
  copyShotStatsRecords(*snapshot);
  TaskMutex::hostObserver = nullptr;
  CHECK(acquisitions == 1);
  CHECK(snapshot->epoch == copyShotLogEpoch());
  CHECK(snapshot->count == 1);
  // The network aggregates its selected window from the copied records; at
  // ten shots that equals the store's own integration aggregate.
  const ShotStatsView windowed = shotLogStatsView(snapshot->count, SHOT_LOG_STATS_WINDOW,
      [&](size_t n) -> const ShotLogRecord & { return snapshot->records[n]; });
  CHECK(windowed.shotCount == 1 && windowed.actualCgSum == 3000);
  CHECK(copyShotStats().actualCgSum == windowed.actualCgSum);
  CHECK(snapshot->records[0].id == record.id);
  // Row curves are fetched on demand under one short lock each.
  ShotCurveRecord fetched = emptyShotCurveRecord();
  CHECK(copyShotCurveById(record.id, fetched));
  CHECK(fetched.shotId == record.id);
  CHECK(curvePoint(fetched, 0).cg == record.actualWeightCg);
  CHECK(rateShotRecord(record.id, 5));
  CHECK(snapshot->epoch != copyShotLogEpoch());
  CHECK(shotLogRating(snapshot->records[0].sideFlags) == 0);
  copyShotStatsRecords(*snapshot);
  CHECK(shotLogRating(snapshot->records[0].sideFlags) == 5);
}

void s19_shot_store_persist_failure_logs_once_until_success() {
  resetHarness(false, true);
  ShotLogRecord record = {};
  record.durationDs = 120;
  record.bootId = currentBootId();
  CHECK(shotLog.append(record, false));
  CHECK(shotLog.dirty());
  maintenanceLease.active = true;
  serviceShotStorePersistence();
  CHECK(shotLog.dirty());
  maintenanceLease.active = false;
  g_hostFlashIoMutexAvailable = false;
  debugLog.clear();
  serviceShotStorePersistence();
  CHECK(debugEventExists(DebugCode::SHOT_LOG_PERSIST_FAILED,
                         static_cast<int32_t>(shotLog.count()), 0));
  CHECK(shotLog.dirty());
  CHECK(shotStorePersistIoRetryMs == 0);
  debugLog.clear();
  serviceShotStorePersistence();
  CHECK(!debugEventExists(DebugCode::SHOT_LOG_PERSIST_FAILED));
  g_hostFlashIoMutexAvailable = true;
  serviceShotStorePersistence();
  CHECK(shotLog.dirty());
  hostMillis += SHOT_STORE_PERSIST_RETRY_MS;
  serviceShotStorePersistence();
  CHECK(!shotLog.dirty());
  CHECK(!activationStores.anyPersistFailLatched());
}

void s19b_shot_store_io_backoff_resets_for_new_dirty_data() {
  resetHarness(false, true);
  PersistedLastShot shot = {};
  shot.valid = true;
  shot.cycleId = 1;
  persistLastShotSnapshot(shot);
  LastShotStore::setHostSaveSucceeds(false);
  serviceShotStorePersistence();
  CHECK(shotStorePersistIoRetryMs == SHOT_STORE_PERSIST_RETRY_MS);
  hostMillis = shotStorePersistRetryAtMs;
  serviceShotStorePersistence();
  CHECK(shotStorePersistIoRetryMs == 2U * SHOT_STORE_PERSIST_RETRY_MS);

  shot.cycleId = 2;
  persistLastShotSnapshot(shot);
  serviceShotStorePersistence();
  CHECK(shotStorePersistIoRetryMs == SHOT_STORE_PERSIST_RETRY_MS);
  LastShotStore::setHostSaveSucceeds(true);
  hostMillis = shotStorePersistRetryAtMs;
  serviceShotStorePersistence();
  CHECK(!lastShotNvsDirty);
  CHECK(shotStorePersistIoRetryMs == 0);
}

void h01_health_threshold_alerts_fire_once_per_crossing() {
  resetHarness(false, true);
  healthSnapshotValid = true;
  freeHeapBytes = HEALTH_HEAP_FREE_ALERT_BYTES - 1;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_CLEAR_BYTES;
  loopStackMinBytes = HEALTH_STACK_MIN_CLEAR_BYTES;
  setScaleWorkerStackMinBytesForHost(HEALTH_STACK_MIN_CLEAR_BYTES);
  serviceHealthThresholdAlerts(0);
  CHECK(debugEventExists(DebugCode::HEALTH_HEAP_LOW,
                         static_cast<int32_t>(freeHeapBytes),
                         static_cast<int32_t>(largestFreeHeapBlockBytes)));
  debugLog.clear();
  serviceHealthThresholdAlerts(0);
  CHECK(!debugEventExists(DebugCode::HEALTH_HEAP_LOW));

  freeHeapBytes = HEALTH_HEAP_FREE_CLEAR_BYTES;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_CLEAR_BYTES;
  serviceHealthThresholdAlerts(0);
  CHECK(!healthHeapAlertLatched);

  loopStackMinBytes = HEALTH_STACK_MIN_ALERT_BYTES - 1;
  serviceHealthThresholdAlerts(0);
  CHECK(debugEventExists(DebugCode::HEALTH_STACK_LOW));
  debugLog.clear();
  serviceHealthThresholdAlerts(0);
  CHECK(!debugEventExists(DebugCode::HEALTH_STACK_LOW));

  loopStackMinBytes = HEALTH_STACK_MIN_CLEAR_BYTES;
  serviceHealthThresholdAlerts(0);
  CHECK(!healthStackAlertLatched);

  loopStackMinBytes = 0;
  serviceHealthThresholdAlerts(0);
  CHECK(healthStackAlertLatched);
  loopStackMinBytes = UINT32_MAX;
  serviceHealthThresholdAlerts(0);
  CHECK(!healthStackAlertLatched);

  healthIntervalGapCpuUs = 1500;
  healthIntervalGapIdleUs = 190000;
  healthIntervalGapFlash = true;
  serviceHealthThresholdAlerts(HEALTH_LOOP_GAP_ALERT_MS);
  CHECK(debugEventExists(DebugCode::HEALTH_LOOP_GAP,
                         static_cast<int32_t>(HEALTH_LOOP_GAP_ALERT_MS), 1500));
  {
    DebugEvent events[DEBUG_EVENT_CAPACITY] = {};
    const size_t count = copyDebugEvents(0, events, DEBUG_EVENT_CAPACITY);
    CHECK(count > 0 && strstr(events[count - 1].text,
                              "idle=190000 us flash=1") != nullptr);
  }
  debugLog.clear();
  serviceHealthThresholdAlerts(HEALTH_LOOP_GAP_ALERT_MS);
  CHECK(!debugEventExists(DebugCode::HEALTH_LOOP_GAP));
  serviceHealthThresholdAlerts(HEALTH_LOOP_GAP_CLEAR_MS);
  CHECK(!healthLoopGapAlertLatched);

  noteFlashIoHeld(true, 1000);
  noteFlashIoHeld(true, 1001);
  noteFlashIoHeld(false, 1002);  // Recursive release keeps the hold open.
  CHECK(flashIoActiveSince(5000));
  noteFlashIoHeld(false, 1010);
  CHECK(flashIoActiveSince(1010) && !flashIoActiveSince(1011));
  noteFlashIoHeld(false, 1020);  // Unbalanced release is ignored.
  CHECK(!flashIoActiveSince(1011));
  flashIoActivity().releasedAtMs.store(0);
}

void h01b_health_heap_low_restarts_only_when_ready_and_sustained() {
  resetHarness(false, true);
  healthSnapshotValid = true;
  reachReadyFromBoot();
  freeHeapBytes = HEALTH_HEAP_FREE_ALERT_BYTES - 1;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_ALERT_BYTES - 1;
  loopStackMinBytes = HEALTH_STACK_MIN_CLEAR_BYTES;
  setScaleWorkerStackMinBytesForHost(HEALTH_STACK_MIN_CLEAR_BYTES);
  serviceHealthThresholdAlerts(0);
  CHECK(healthHeapAlertLatched);
  CHECK(!safeRestartPending());
  CHECK(!debugEventExists(DebugCode::HEALTH_HEAP_RESTART));

  hostMillis += HEALTH_HEAP_LOW_RESTART_MS - 1;
  serviceHealthThresholdAlerts(0);
  CHECK(!safeRestartPending());

  hostMillis += 1;
  healthSampleQueue = xQueueCreate(1, sizeof(HealthWorkerSample));
  HealthWorkerSample restartSample;
  restartSample.version = 1;
  restartSample.sampledAtMs = hostMillis;
  restartSample.heapValid = true;
  restartSample.restartRequested = true;
  restartSample.heap.internalFree = freeHeapBytes;
  restartSample.heap.internalLargest = largestFreeHeapBlockBytes;
  CHECK(xQueueOverwrite(healthSampleQueue, &restartSample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(safeRestartPending());
  CHECK(debugEventExists(DebugCode::HEALTH_HEAP_RESTART,
                         static_cast<int32_t>(freeHeapBytes),
                         static_cast<int32_t>(largestFreeHeapBlockBytes)));
  CHECK(healthHeapRestartLatched);

  safetyEventFlags.clear(SAFETY_EVENT_SAFE_RESTART);
  debugLog.clear();
  serviceHealthThresholdAlerts(0);
  CHECK(!debugEventExists(DebugCode::HEALTH_HEAP_RESTART));
  CHECK(!safeRestartPending());

  resetHarness(false, true);
  reachReadyFromBoot();
  freeHeapBytes = HEALTH_HEAP_FREE_ALERT_BYTES - 1;
  healthSnapshotValid = true;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_ALERT_BYTES - 1;
  loopStackMinBytes = HEALTH_STACK_MIN_CLEAR_BYTES;
  setScaleWorkerStackMinBytesForHost(HEALTH_STACK_MIN_CLEAR_BYTES);
  serviceHealthThresholdAlerts(0);
  hostMillis += HEALTH_HEAP_LOW_RESTART_MS / 2;
  serviceHealthThresholdAlerts(0);
  freeHeapBytes = HEALTH_HEAP_FREE_CLEAR_BYTES;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_CLEAR_BYTES;
  serviceHealthThresholdAlerts(0);
  CHECK(!healthHeapAlertLatched);
  hostMillis += HEALTH_HEAP_LOW_RESTART_MS;
  healthSampleQueue = xQueueCreate(1, sizeof(HealthWorkerSample));
  restartSample.sampledAtMs = hostMillis;
  restartSample.restartRequested = false;
  CHECK(xQueueOverwrite(healthSampleQueue, &restartSample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(!safeRestartPending());

  resetHarness(false, true);
  reachReadyFromBoot();
  (void)startCycle();
  CHECK(session.active);
  healthSnapshotValid = true;
  freeHeapBytes = HEALTH_HEAP_FREE_ALERT_BYTES - 1;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_ALERT_BYTES - 1;
  loopStackMinBytes = HEALTH_STACK_MIN_CLEAR_BYTES;
  setScaleWorkerStackMinBytesForHost(HEALTH_STACK_MIN_CLEAR_BYTES);
  serviceHealthThresholdAlerts(0);
  hostMillis += HEALTH_HEAP_LOW_RESTART_MS;
  healthSampleQueue = xQueueCreate(1, sizeof(HealthWorkerSample));
  restartSample.sampledAtMs = hostMillis;
  restartSample.restartRequested = true;
  CHECK(xQueueOverwrite(healthSampleQueue, &restartSample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(healthHeapAlertLatched);
  CHECK(!safeRestartPending());
  CHECK(!debugEventExists(DebugCode::HEALTH_HEAP_RESTART));

  resetHarness(false, true);
  reachReadyFromBoot();
  maintenanceLease.active = true;
  freeHeapBytes = HEALTH_HEAP_FREE_ALERT_BYTES - 1;
  healthSnapshotValid = true;
  largestFreeHeapBlockBytes = HEALTH_HEAP_LARGEST_ALERT_BYTES - 1;
  loopStackMinBytes = HEALTH_STACK_MIN_CLEAR_BYTES;
  setScaleWorkerStackMinBytesForHost(HEALTH_STACK_MIN_CLEAR_BYTES);
  serviceHealthThresholdAlerts(0);
  hostMillis += HEALTH_HEAP_LOW_RESTART_MS;
  serviceHealthThresholdAlerts(0);
  CHECK(!safeRestartPending());
  CHECK(!debugEventExists(DebugCode::HEALTH_HEAP_RESTART));
}

void h01c_health_zero_is_exhaustion_not_missing_telemetry() {
  resetHarness(false, true);
  reachReadyFromBoot();
  healthSampleQueue = xQueueCreate(1, sizeof(HealthWorkerSample));
  HealthWorkerSample sample;
  sample.version = 1;
  sample.sampledAtMs = hostMillis;
  sample.restartRequested = true;
  // Invalid samples must not become valid merely because their age is fresh.
  CHECK(xQueueOverwrite(healthSampleQueue, &sample) == pdTRUE);
  serviceHealthWorkerSample();
  serviceHealthWorkerSample();
  CHECK(!healthSnapshotValid && !healthHeapAlertLatched && !safeRestartPending());
  sample.heapValid = true;
  sample.sampledAtMs = hostMillis - HEALTH_SAMPLE_STALE_MS - 1;
  CHECK(xQueueOverwrite(healthSampleQueue, &sample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(!healthSnapshotValid && !safeRestartPending());
  sample.sampledAtMs = hostMillis;
  sample.restartRequested = false;
  sample.heap.internalFree = HEALTH_HEAP_FREE_CLEAR_BYTES;
  // A zero largest block also represents exhaustion even with a free total.
  CHECK(xQueueOverwrite(healthSampleQueue, &sample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(healthSnapshotValid && healthHeapAlertLatched && !safeRestartPending());
  sample.heap.internalFree = 0;
  sample.restartRequested = true;
  CHECK(xQueueOverwrite(healthSampleQueue, &sample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(healthHeapAlertLatched && healthHeapRestartLatched && safeRestartPending());
  sample.heapValid = false;
  sample.heap.internalFree = HEALTH_HEAP_FREE_CLEAR_BYTES;
  sample.heap.internalLargest = HEALTH_HEAP_LARGEST_CLEAR_BYTES;
  CHECK(xQueueOverwrite(healthSampleQueue, &sample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(healthHeapAlertLatched && healthHeapRestartLatched);
  sample.heapValid = true;
  sample.restartRequested = false;
  CHECK(xQueueOverwrite(healthSampleQueue, &sample) == pdTRUE);
  serviceHealthWorkerSample();
  CHECK(!healthHeapAlertLatched && !healthHeapRestartLatched);
}

void h02_hwmon_cpu_load_uses_refreshed_idle_and_ema() {
  Hwmon monitor;
  monitor.begin();

  // Fully idle both cores over 5s → load 0.0
  monitor.hostSetIdleAccumUs(5000000ULL, 5000000ULL);
  HwmonSnapshot snap = monitor.sample(5000);
  CHECK(snap.cpuLoadValid);
  CHECK(snap.cpu0Busy >= 0.0f && snap.cpu0Busy <= 0.01f);
  CHECK(snap.cpu1Busy >= 0.0f && snap.cpu1Busy <= 0.01f);
  CHECK(snap.cpuLoad5s >= 0.0f && snap.cpuLoad5s <= 0.01f);
  CHECK(snap.cpuLoad1m >= 0.0f && snap.cpuLoad1m <= 0.01f);
  CHECK(snap.cpuMhz == 80U);

  // Fully busy both cores → load 2.0
  monitor.hostSetIdleAccumUs(0, 0);
  snap = monitor.sample(5000);
  CHECK(snap.cpuLoadValid);
  CHECK(snap.cpu0Busy >= 0.99f && snap.cpu0Busy <= 1.0f);
  CHECK(snap.cpu1Busy >= 0.99f && snap.cpu1Busy <= 1.0f);
  CHECK(snap.cpuLoad5s >= 1.99f && snap.cpuLoad5s <= 2.0f);
  // EMA moves toward 2 but stays below after one 5s step from 0
  CHECK(snap.cpuLoad1m > 0.05f && snap.cpuLoad1m < 2.0f);
  CHECK(snap.cpuLoad5m > 0.0f && snap.cpuLoad5m < snap.cpuLoad1m + 0.001f);

  // Half idle each core → load 1.0
  monitor.hostSetIdleAccumUs(2500000ULL, 2500000ULL);
  snap = monitor.sample(5000);
  CHECK(snap.cpuLoad5s >= 0.99f && snap.cpuLoad5s <= 1.01f);
  CHECK(snap.cpu0Busy >= 0.49f && snap.cpu0Busy <= 0.51f);
  CHECK(snap.cpu1Busy >= 0.49f && snap.cpu1Busy <= 0.51f);

  // A failed remote refresh must not publish stale IDLE counters as new load.
  monitor.hostSetIdleAccumUs(0, 0, false);
  snap = monitor.sample(5000);
  CHECK(!snap.cpuLoadValid);
  CHECK(snap.cpuLoad5s == snap.cpuLoad1m);
}

void h03_task_profiler_start_stop_updates_snapshot() {
  resetHarness(false, false);
  TaskProfilerSnapshot snap;
  copyTaskProfiler(snap);
  CHECK(snap.state == TaskProfilerState::NEVER);
  CHECK(snap.stopReason == TaskProfilerStopReason::NONE);
  CHECK(snap.rowCount == 0);

  WebCommand start = {};
  start.type = WebCommandType::TASK_PROFILER_START;
  processWebCommand(start);
  serviceHealthProfiler(millis());
  copyTaskProfiler(snap);
  CHECK(snap.state == TaskProfilerState::FAILED);
  CHECK(snap.stopReason == TaskProfilerStopReason::CAPTURE_FAILED);

  WebCommand stop = {};
  stop.type = WebCommandType::TASK_PROFILER_STOP;
  processWebCommand(stop);
  serviceHealthProfiler(millis());
  copyTaskProfiler(snap);
  CHECK(snap.state == TaskProfilerState::FAILED);
  CHECK(snap.stopReason == TaskProfilerStopReason::CAPTURE_FAILED);
}

void h04_loop_phase_profiler_publishes_window_and_session_totals() {
  LoopPhaseProfiler profiler;
  LoopPhaseProfilerSnapshot snap;
  profiler.beginIteration(false, 1000U);
  profiler.record(LoopPhase::CONTROL_STATE_MACHINE, 150U, 1150U);
  profiler.capturePeakGap(8U, 8050U, 7000U, 400U);
  profiler.captureIntervalGap(8050U, 7000U, 400U);
  profiler.publishIntervalGap(8U);
  profiler.beginIteration(false, 2000U);
  profiler.record(LoopPhase::CONTROL_STATE_MACHINE, 90U, 1002000U);
  profiler.captureIntervalGap(5100U, 4200U, 300U);
  profiler.publishIntervalGap(5U);
  profiler.copySnapshot(snap);
  CHECK(snap.recentGapMs == 5U);
  CHECK(snap.recentGapUs == 5100U);
  CHECK(snap.recentDelayUs == 4200U);
  CHECK(snap.recentDispatchUs == 300U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)]
            .recentGapExecutionUs == 90U);
  profiler.beginIteration(false, 1003000U);
  profiler.record(LoopPhase::DIAGNOSTICS, 10U, 2003000U);
  profiler.copySnapshot(snap);
  CHECK(snap.peakGapMs == 8U);
  CHECK(snap.peakGapUs == 8050U);
  CHECK(snap.peakDelayUs == 7000U);
  CHECK(snap.peakDispatchUs == 400U);
  CHECK(snap.recentGapMs == 5U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)].lastExecutionUs ==
        90U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)].maxExecutionUs ==
        150U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)]
            .peakGapExecutionUs == 150U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)]
            .recentGapExecutionUs == 90U);
  profiler.requestReset();
  CHECK(profiler.consumeReset());
  profiler.copySnapshot(snap);
  CHECK(snap.peakGapMs == 0U);
  CHECK(snap.peakGapUs == 0U);
  CHECK(snap.peakDelayUs == 0U);
  CHECK(snap.peakDispatchUs == 0U);
  CHECK(snap.recentGapMs == 5U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)].maxExecutionUs ==
        0U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)].lastExecutionUs ==
        90U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)]
            .recentGapExecutionUs == 90U);
  profiler.beginIteration(true, 1000U);
  profiler.copySnapshot(snap);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)].lastExecutionUs ==
        90U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)].sampleCount == 0U);
  profiler.record(LoopPhase::SAFETY_HEALTH, 100U, 1100U);
  profiler.record(LoopPhase::CONTROL_STATE_MACHINE, 200U, 1300U);
  // Housekeeping only records on iterations that run the gated block. The
  // status publish splits into three contiguous sub-phases.
  profiler.record(LoopPhase::HOUSEKEEPING_SHOT_PERSISTENCE, 70U, 1400U);
  profiler.record(LoopPhase::HOUSEKEEPING_STATUS_GATHER, 12U, 1412U);
  profiler.record(LoopPhase::HOUSEKEEPING_STATUS_STORE_COPY, 10U, 1422U);
  profiler.record(LoopPhase::HOUSEKEEPING_STATUS_COMMIT, 8U, 1430U);
  profiler.record(LoopPhase::DIAGNOSTICS, 50U, 1001000U);
  profiler.copySnapshot(snap);
  CHECK(snap.rowCount == LOOP_PHASE_COUNT);
  CHECK(strcmp(snap.rows[0].name, "safety/health") == 0);
  CHECK(snap.rows[0].sampleCount == 1U);
  CHECK(snap.rows[0].averageExecutionUs == 100U);
  CHECK(snap.rows[0].maxExecutionUs == 100U);
  CHECK(snap.rows[0].currentCpuPct > 0.009f);
  CHECK(snap.rows[0].currentCpuPct < 0.011f);
  CHECK(snap.rowCount == 28U);
  CHECK(strcmp(snap.rows[1].name, "input/backflush poll") == 0);
  CHECK(strcmp(snap.rows[2].name, "input/drive permission") == 0);
  CHECK(strcmp(snap.rows[3].name, "input/scale events") == 0);
  CHECK(strcmp(snap.rows[4].name, "input/machine sense") == 0);
  CHECK(strcmp(snap.rows[5].name, "input/machine service") == 0);
  CHECK(strcmp(snap.rows[6].name, "guards/brew start") == 0);
  CHECK(strcmp(snap.rows[7].name, "machine integration") == 0);
  CHECK(strcmp(snap.rows[8].name, "control/state machine") == 0);
  CHECK(strcmp(snap.rows[13].name, "commands/gate+maintenance") == 0);
  CHECK(strcmp(snap.rows[static_cast<uint8_t>(
                    LoopPhase::HOUSEKEEPING_STATUS_GATHER)]
                    .name,
               "status/gather") == 0);
  CHECK(strcmp(snap.rows[static_cast<uint8_t>(
                    LoopPhase::HOUSEKEEPING_STATUS_STORE_COPY)]
                    .name,
               "status/store copy") == 0);
  CHECK(strcmp(snap.rows[static_cast<uint8_t>(
                    LoopPhase::HOUSEKEEPING_STATUS_COMMIT)]
                    .name,
               "status/lock+commit") == 0);
  const auto &history = snap.rows[static_cast<uint8_t>(
      LoopPhase::HOUSEKEEPING_SHOT_PERSISTENCE)];
  CHECK(strcmp(history.name, "housekeeping/history") == 0);
  CHECK(history.sampleCount == 1U);
  CHECK(history.maxExecutionUs == 70U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::HOUSEKEEPING_STATUS_GATHER)]
            .averageExecutionUs == 12U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::HOUSEKEEPING_STATUS_STORE_COPY)]
            .averageExecutionUs == 10U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::HOUSEKEEPING_STATUS_COMMIT)]
            .averageExecutionUs == 8U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::DIAGNOSTICS)]
            .averageExecutionUs == 50U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::FINAL_SCALE_DRAIN)]
            .averageExecutionUs == 0U);
  profiler.captureIntervalGap(1500U, 900U, 150U);
  profiler.publishIntervalGap(2U);
  profiler.beginIteration(true, 1002000U, 1002U);
  profiler.record(LoopPhase::DIAGNOSTICS, 10U, 2002000U);
  profiler.capturePeakGap(1U, 1000U, 900U, 80U);
  profiler.record(LoopPhase::DIAGNOSTICS, 10U, 3002000U);
  profiler.copySnapshot(snap);
  CHECK(snap.peakGapAtMs == 1002U);  // The gap opened at that iteration's start.
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::HOUSEKEEPING_STATUS_COMMIT)]
            .recentGapExecutionUs == 8U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::HOUSEKEEPING_STATUS_GATHER)]
            .peakGapExecutionUs == 0U);
  CHECK(snap.rows[static_cast<uint8_t>(LoopPhase::CONTROL_STATE_MACHINE)]
            .averageExecutionUs == 200U);

  profiler.beginIteration(false, 3501000U);
  profiler.copySnapshot(snap);
  CHECK(snap.rows[0].averageCpuPct > 0.002f);
  CHECK(snap.rows[0].averageCpuPct < 0.003f);
}

void h05_loop_phase_profiler_pairs_cpu_with_wall_maxima() {
  LoopPhaseProfiler profiler;
  LoopPhaseProfilerSnapshot snap;
  // Default cpuUs equals the wall duration (callers without a counter read).
  profiler.beginIteration(true, 1000U, 5U);
  profiler.record(LoopPhase::INPUT_SCALE_EVENTS, 500U, 1500U);
  profiler.record(LoopPhase::INPUT_SCALE_EVENTS, 300U, 1800U);
  // Cross the 1 s window so the snapshot actually publishes.
  profiler.record(LoopPhase::DIAGNOSTICS, 0U, 1001000U);
  profiler.copySnapshot(snap);
  const auto &scaleEvents =
      snap.rows[static_cast<uint8_t>(LoopPhase::INPUT_SCALE_EVENTS)];
  CHECK(scaleEvents.maxExecutionUs == 500U);
  CHECK(scaleEvents.maxCpuUs == 500U);
  CHECK(scaleEvents.maxAtMs == 5U);
  // A larger wall sample with a SMALLER cpu share replaces the pair: the
  // stored cpu time and iteration uptime belong to the iteration that set
  // the maximum.
  profiler.beginIteration(true, 2000U, 9U);
  profiler.record(LoopPhase::INPUT_SCALE_EVENTS, 900U, 2200U, 120U);
  profiler.record(LoopPhase::INPUT_SCALE_EVENTS, 400U, 2600U, 390U);
  profiler.record(LoopPhase::DIAGNOSTICS, 0U, 2001000U);
  profiler.copySnapshot(snap);
  CHECK(scaleEvents.maxExecutionUs == 900U);
  CHECK(scaleEvents.maxCpuUs == 120U);
  CHECK(scaleEvents.maxAtMs == 9U);
  // The kernel folds own-CPU at switch-out: a fold that lands in this phase
  // can exceed its wall time, and the stored pair must clamp to the wall so
  // wall - cpu never goes negative.
  profiler.record(LoopPhase::INPUT_MACHINE_SERVICE, 250U, 3000000U, 900U);
  profiler.record(LoopPhase::DIAGNOSTICS, 0U, 3001000U);
  profiler.copySnapshot(snap);
  const auto &machineService =
      snap.rows[static_cast<uint8_t>(LoopPhase::INPUT_MACHINE_SERVICE)];
  CHECK(machineService.maxExecutionUs == 250U);
  CHECK(machineService.maxCpuUs == 250U);
  // Reset clears the paired maximum with the rest.
  profiler.requestReset();
  CHECK(profiler.consumeReset());
  profiler.copySnapshot(snap);
  CHECK(scaleEvents.maxExecutionUs == 0U);
  CHECK(scaleEvents.maxCpuUs == 0U);
  CHECK(scaleEvents.maxAtMs == 0U);
}

void h06_loop_phase_input_subphases_report_individually() {
  LoopPhaseProfiler profiler;
  LoopPhaseProfilerSnapshot snap;
  profiler.beginIteration(true, 1000U);
  profiler.record(LoopPhase::INPUT_BACKFLUSH, 100U, 1100U);
  profiler.record(LoopPhase::INPUT_DRIVE_PERMISSION, 50U, 1150U);
  profiler.record(LoopPhase::INPUT_SCALE_EVENTS, 700U, 1850U);
  profiler.record(LoopPhase::INPUT_MACHINE_SERVICE, 200U, 2050U);
  profiler.record(LoopPhase::DIAGNOSTICS, 0U, 1001000U);
  profiler.copySnapshot(snap);
  const auto sub = [&](LoopPhase phase) -> const LoopPhaseProfilerRow & {
    return snap.rows[static_cast<uint8_t>(phase)];
  };
  CHECK(strcmp(sub(LoopPhase::INPUT_BACKFLUSH).name, "input/backflush poll") ==
        0);
  CHECK(strcmp(sub(LoopPhase::INPUT_MACHINE_SERVICE).name,
               "input/machine service") == 0);
  CHECK(sub(LoopPhase::INPUT_BACKFLUSH).lastExecutionUs == 100U);
  CHECK(sub(LoopPhase::INPUT_SCALE_EVENTS).maxExecutionUs == 700U);
  // The four sub-phases each sampled once; their CPU percentages sum to the
  // same share one aggregate phase with the same total would report.
  const float aggregateUs = 100U + 50U + 700U + 200U;
  float aggregatePct = 0.0f;
  float summedPct = 0.0f;
  for (uint8_t i = static_cast<uint8_t>(LoopPhase::INPUT_BACKFLUSH);
       i <= static_cast<uint8_t>(LoopPhase::INPUT_MACHINE_SERVICE); ++i) {
    summedPct += snap.rows[i].currentCpuPct;
  }
  aggregatePct = aggregateUs * 100.0f / 1000000.0f;  // exact 1 s window
  CHECK(summedPct > aggregatePct - 0.001f);
  CHECK(summedPct < aggregatePct + 0.001f);
}

void h08_lock_wait_stats_flow_into_the_profiler_snapshot() {
  resetHarness(false, false);
  noteMutexWaitUs(scaleCriticalEventWaitLastUsStat(),
                 scaleCriticalEventWaitMaxUsStat(), 650U);
  noteMutexWaitUs(micraBackflushWaitLastUsStat(),
                  micraBackflushWaitMaxUsStat(), 25U);
  noteMutexWaitUs(shotStoreWaitLastUs, shotStoreWaitMaxUs, 700U);
  noteMutexWaitUs(controlStatusWaitLastUs, controlStatusWaitMaxUs, 150U);
  TaskProfilerSnapshot snap;
  copyTaskProfiler(snap);
  CHECK(snap.scaleEventsWaitLastUs == 650U);
  CHECK(snap.scaleEventsWaitMaxUs == 650U);
  CHECK(snap.micraBackflushWaitLastUs == 25U);
  CHECK(snap.micraBackflushWaitMaxUs == 25U);
  CHECK(snap.shotStoreWaitLastUs == 700U);
  CHECK(snap.shotStoreWaitMaxUs == 700U);
  CHECK(snap.controlStatusWaitLastUs == 150U);
  CHECK(snap.controlStatusWaitMaxUs == 150U);
}

void h10_newest_history_record_matches_first_desc_page_row() {
  resetHarness(false, true);
  HistoryRecord newest = {};
  HistoryPage page;
  // Empty store: both reads agree that nothing is there.
  CHECK(!historyLog.copyNewest(newest));
  historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
  CHECK(page.count == 0);
  // Fill past capacity so writeIndex wraps; every step must agree.
  for (uint32_t n = 1; n <= HISTORY_CAPACITY + 5U; ++n) {
    appendHistoryRecord(HistoryType::SHOT, n * 100U, n % 2 == 0, nullptr);
    historyLog.copyPage(page, 0, 1, ShotLogSortDir::Desc);
    CHECK(historyLog.copyNewest(newest));
    CHECK(page.count == 1);
    CHECK(memcmp(&newest, &page.records[0], sizeof(HistoryRecord)) == 0);
  }
  // The merged publish gather carries the same record through the single
  // shotStoreMutex critical section.
  uint32_t bootId = 0;
  PersistedLastShot last = {}, good = {};
  HistoryRecord gathered = {};
  bool hasGathered = false;
  copyShotStoreStatus(bootId, last, good, gathered, hasGathered, true);
  CHECK(hasGathered);
  CHECK(memcmp(&gathered, &newest, sizeof(HistoryRecord)) == 0);
}

void h11_status_store_cache_generation_guard() {
  resetHarness(false, true);
  ControlStatusSnapshot status;

  // A valid home cycle makes the store curve dead data, so the publish's
  // generation guard may hold across publishes in this state.
  homeCycleResult = lastShotFromEndedCycle(EndReason::ACTIVATOR, 1500);
  homeCycleResolvedAtMs = hostMillis;
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.homeCycle.valid);
  CHECK(status.lastActivationId == 0);

  // Pinned contract: a direct store mutation without a wrapper does not
  // bump the generation and must NOT reach the next publish while the
  // guard holds.
  HistoryRecord direct = {};
  direct.durationDs = 42;
  direct.type = static_cast<uint8_t>(HistoryType::OTHER);
  CHECK(historyLog.append(direct, false));
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.lastActivationId != historyLog.nextRecordId() - 1U);
  // A generation bump is the documented invalidation for direct mutations.
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_relaxed);
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.lastActivationId == historyLog.nextRecordId() - 1U);
  CHECK(status.lastActivationDurationDs == 42);

  // Wrapper mutations bump and propagate, including a rating-only change.
  ShotLogRecord shot = {};
  shot.durationDs = 250;
  shot.goalWeightDg = 360;
  shot.actualWeightCg = 3650;
  CHECK(shotLog.append(shot, false));
  ShotLogRecord storedShot = {};
  CHECK(shotLog.copyNewestFirst(&storedShot, 1) == 1);
  PersistedLastShot good = {};
  good.valid = true;
  good.cycleId = 44;
  good.durationMs = 25000;
  good.weightValid = true;
  good.currentWeightG = 36.5f;
  good.shotLogId = storedShot.id;
  good.presetId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  persistLastShotSnapshot(good);
  CHECK(rateShotRecord(storedShot.id, 4));
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.lastGoodShotHistoryLinked);
  CHECK(status.lastGoodShot.rating == 4);
  CHECK(rateShotRecord(storedShot.id, 2));
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.lastGoodShot.rating == 2);

  // Curve precedence is unchanged while the guard holds: a matching
  // pendingFinalize owns the published curve, not the store gather.
  pendingFinalize.pending = true;
  pendingFinalize.cycleId = homeCycleResult.cycleId;
  appendShotCurveObservation(pendingFinalize.curve, 36.5f, 15000, false);
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.shotCurve.count == pendingFinalize.curve.count);
  CHECK(curvePoint(status.shotCurve, 0).atMs == 15000);
}

void h12_persistence_fast_path_skips_clean_store_mutex() {
  resetHarness(false, true);
  // Drive one full persist cycle so the clean latch catches up with the
  // acknowledged image (the host drain persists synchronously).
  persistLastShotSnapshot(PersistedLastShot{});
  serviceShotStorePersistence();
  serviceShotStorePersistence();
  static thread_local int acquisitions = 0;
  acquisitions = 0;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &shotStoreMutex && acquired) ++acquisitions;
  };
  serviceShotStorePersistence();
  TaskMutex::hostObserver = nullptr;
  CHECK(acquisitions == 0);
  // A wrapper mutation bumps the generation: the locked path flows again
  // and the service returns to its clean fast path afterwards.
  persistLastShotSnapshot(PersistedLastShot{});
  acquisitions = 0;
  TaskMutex::hostObserver = [](const TaskMutex *mutex, bool acquired) {
    if (mutex == &shotStoreMutex && acquired) ++acquisitions;
  };
  serviceShotStorePersistence();
  serviceShotStorePersistence();
  TaskMutex::hostObserver = nullptr;
  CHECK(acquisitions > 0);
}

void h07_mutex_wait_accounting_tracks_last_and_max() {
  std::atomic<uint32_t> lastUs{0};
  std::atomic<uint32_t> maxUs{0};
  noteMutexWaitUs(lastUs, maxUs, 40U);
  noteMutexWaitUs(lastUs, maxUs, 90U);
  noteMutexWaitUs(lastUs, maxUs, 10U);
  CHECK(lastUs.load() == 10U);
  CHECK(maxUs.load() == 90U);
  // The live stats are readable from the profiler snapshot's side (the
  // Micra backflush accessor is exercised by the micra host tests).
  CHECK(scaleCriticalEventWaitLastUsStat().load() == 0U);
  CHECK(micraBackflushWaitLastUsStat().load() == 0U);
}

void r51_auto_to_manual_guard_fires_while_scale_lost() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = true;
  runtimeConfig.scaleLossFixedMs = 15000;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  CHECK(session.autoToManualGuardArmed);
  CHECK(!session.autoToManualGuardEnforced);
  setScaleConnected(false);
  loop();
  CHECK(session.weightControlState == WeightControlState::SUSPENDED);
  CHECK(session.autoToManualGuardEnforced);
  reachSessionElapsed(15000);
  CHECK(session.endReason == EndReason::AUTO_TO_MANUAL_GUARD);
  CHECK(!getRelaySafetySnapshot().closed);
}

void r52_auto_to_manual_guard_clears_on_scale_recovery() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = true;
  runtimeConfig.scaleLossFixedMs = 20000;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(session.autoToManualGuardArmed);
  setScaleConnected(false);
  loop();
  CHECK(session.autoToManualGuardEnforced);
  // The deadline is chosen at the loss; a fixed time gives the same one again.
  const uint32_t deadline = session.autoToManualGuardDeadlineAtMs;
  CHECK(session.scaleLossSource == static_cast<uint8_t>(corridor::LossSource::FIXED));
  setScaleConnected(true);
  publishWeight(1.0f);
  publishWeight(2.0f, hostMillis + 1);
  publishWeight(3.0f, hostMillis + 2);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(!session.autoToManualGuardEnforced);
  CHECK(session.autoToManualGuardDeadlineAtMs == deadline);
  setScaleConnected(false);
  loop();
  CHECK(session.autoToManualGuardEnforced);
  CHECK(session.autoToManualGuardDeadlineAtMs == deadline);
}

void r53_auto_to_manual_guard_disabled_does_not_cut_early() {
  resetHarness(false, true);
  runtimeConfig.scaleLossEnabled = false;
  runtimeConfig.scaleLossFixedMs = 10000;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  CHECK(!session.autoToManualGuardArmed);
  setScaleConnected(false);
  loop();
  reachSessionElapsed(15000);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(session.endReason == EndReason::NONE);
}

void r48_guard_disabled_stops_at_target_normally() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = false;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void r49_guard_extends_and_stops_at_max_weight() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.fastMarginDg = 65;  // 42.5 g at the 36 g target
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  runLoopAfter(22000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  CHECK(session.extractionExtended);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL_FAST);
  CHECK(localBuzzer.activePulseRate ==
        static_cast<uint8_t>(ExtendedPulseRate::FAST));
  const float maxThreshold = ceilingStopThresholdForTest();
  publishWeight(maxThreshold + 0.1f);
  publishWeight(maxThreshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::FAST_EXTRACTION_MAX_WEIGHT);
  CHECK(localBuzzer.activeCue != BuzzerCue::ABNORMAL_FAST);
}

void r51_extended_pulse_respects_alert_flag_and_scale_only() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.buzzerExtendedPulseRate =
      static_cast<uint8_t>(ExtendedPulseRate::OFF);
  runtimeConfig.fastMarginDg = 65;  // 42.5 g at the 36 g target
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  runLoopAfter(22000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  CHECK(session.extractionExtended);
  CHECK(!localBuzzer.looping);

  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.buzzerExtendedPulseRate =
      static_cast<uint8_t>(ExtendedPulseRate::FAST);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  runtimeConfig.fastMarginDg = 65;  // 42.5 g at the 36 g target
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  runLoopAfter(22000);
  const float scaleOnlyThreshold = effectiveStopThreshold();
  publishWeight(scaleOnlyThreshold + 0.1f);
  publishWeight(scaleOnlyThreshold + 0.2f);
  CHECK(session.extractionExtended);
  CHECK(!localBuzzer.looping);
}

void r57_guard_max_weight_cut_from_predicted_time() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.fastMarginDg = 65;  // 42.5 g at the 36 g target
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  runLoopAfter(22000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  CHECK(session.extractionExtended);
  CHECK(getRelaySafetySnapshot().closed);
  shot.expectedEndS = 0.0f;
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::FAST_EXTRACTION_MAX_WEIGHT);
}

void r59_slow_guard_on_time_bbw_is_scale_threshold() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.slowSideEnabled = true;
  runtimeConfig.slowMarginDg = 60;  // 30 g at the 36 g target
  runtimeConfig.slowAfterMs = 44000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  reachSessionElapsed(32000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  CHECK(!session.slowExtractionExtended);
}

void r61b_slow_extended_pulse_uses_slow_rate_setting() {
  // The lowered cut line latches on the first sample below the target once f
  // has left it, and the slow pulse uses its own rate setting.
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.slowSideEnabled = true;
  runtimeConfig.buzzerExtendedPulseRate =
      static_cast<uint8_t>(ExtendedPulseRate::FAST);
  runtimeConfig.buzzerSlowExtendedPulseRate =
      static_cast<uint8_t>(ExtendedPulseRate::RAPID);
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishWeight(20.0f);
  reachSessionElapsed(session.config.slowAfterMs + 1000);
  CHECK(!session.slowExtractionExtended);
  publishWeight(20.2f);
  CHECK(session.slowExtractionExtended);
  CHECK(!session.extractionExtended);
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL);
  CHECK(localBuzzer.activePulseRate ==
        static_cast<uint8_t>(ExtendedPulseRate::RAPID));
}

void tt01_smooth_cut_truth_table() {
  // Plan §4.6: a weight cut reports the segment of f at the stop command:
  // raised (13), target (3) or lowered (16), for both the direct backstop
  // and the timed law; a side that is off is the target line.
  struct Row {
    uint32_t atMs;
    bool fastOn, slowOn, timed;
    EndReason reason;
  } rows[] = {
      {20000, true, true, false, EndReason::FAST_EXTRACTION_MAX_WEIGHT},
      {24000, true, true, false, EndReason::FAST_EXTRACTION_MAX_WEIGHT},
      {30000, true, true, false, EndReason::SCALE_THRESHOLD},
      {38000, true, true, false, EndReason::SLOW_EXTRACTION_MIN_WEIGHT},
      {41000, true, true, false, EndReason::SLOW_EXTRACTION_MIN_WEIGHT},
      {20000, false, true, false, EndReason::SCALE_THRESHOLD},
      {41000, true, false, false, EndReason::SCALE_THRESHOLD},
      {20000, true, true, true, EndReason::FAST_EXTRACTION_MAX_WEIGHT},
      {30000, true, true, true, EndReason::SCALE_THRESHOLD},
      {41000, true, true, true, EndReason::SLOW_EXTRACTION_MIN_WEIGHT},
  };
  for (const Row &row : rows) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.fastSideEnabled = row.fastOn;
    runtimeConfig.slowSideEnabled = row.slowOn;
    startCycle();
    advanceToBrew();
    endBbwProtectionForTests();
    publishWeight(10.0f);
    reachSessionElapsed(row.atMs);
    if (row.timed) {
      publishWeight(10.2f);
      shot.expectedEndS = 0.0f;
      loop();
    } else {
      const float threshold = effectiveStopThreshold();
      publishWeight(threshold + 0.1f);
      publishWeight(threshold + 0.2f);
      loop();
    }
    CHECK(stopperState == StopperState::REQUIRES_OFF);
    CHECK(session.endReason == row.reason);
  }
  // Below the backstop threshold nothing stops, at any time.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  reachSessionElapsed(41000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold - 0.3f);
  publishWeight(threshold - 0.2f);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
}

void r61c_extended_pulse_resumes_after_scale_lost_alert() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.scaleLossEnabled = false;
  runtimeConfig.fastMarginDg = 65;  // 42.5 g at the 36 g target
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  runLoopAfter(22000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  CHECK(session.extractionExtended);
  CHECK(session.active);
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL_FAST);
  setScaleConnected(false);
  CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_LOST);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    hostMillis += 40;
    hostServiceEspTimer(localBuzzer.phaseTimer);
    localBuzzer.service();
  }
  CHECK(!localBuzzer.busy());
  CHECK(session.active);
  CHECK(session.extractionExtended);
  serviceExtendedPulseAlert();
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL_FAST);
}

void r61d_extended_pulse_pending_cleared_when_unwanted() {
  resetHarness(false, false);
  const BuzzerToneCommand tare =
      deriveBuzzerTone(AlertEvent::TARE, false, 0, 0);
  CHECK(localBuzzer.requestTone(tare));
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE);
  CHECK(startExtendedPulseTrain(0));
  CHECK(localBuzzer.pendingCue == BuzzerCue::ABNORMAL_FAST);
  runtimeConfig.alertOutputChannel =
      static_cast<uint8_t>(AlertOutputChannel::SCALE_ONLY);
  session.active = true;
  session.extractionExtended = true;
  serviceExtendedPulseAlert();
  CHECK(localBuzzer.pendingCue == BuzzerCue::NONE);
  CHECK(localBuzzer.activeCue == BuzzerCue::TARE);
  drainLocalBuzzer();
  CHECK(!localBuzzer.busy());
  CHECK(localBuzzer.activeCue == BuzzerCue::NONE);

  resetHarness(false, false);
  CHECK(localBuzzer.requestTone(tare));
  CHECK(startExtendedPulseTrain(0));
  CHECK(localBuzzer.pendingCue == BuzzerCue::ABNORMAL_FAST);
  runtimeConfig.buzzerExtendedPulseRate =
      static_cast<uint8_t>(ExtendedPulseRate::OFF);
  session.active = true;
  session.extractionExtended = true;
  serviceExtendedPulseAlert();
  CHECK(localBuzzer.pendingCue == BuzzerCue::NONE);
  drainLocalBuzzer();
  CHECK(!localBuzzer.busy());
}

void r61e_extended_pulse_restarts_when_rate_setting_changes() {
  resetHarness(false, false);
  CHECK(startExtendedPulseTrain(0));
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL_FAST);
  CHECK(localBuzzer.activePulseRate ==
        static_cast<uint8_t>(ExtendedPulseRate::FAST));
  runtimeConfig.buzzerExtendedPulseRate =
      static_cast<uint8_t>(ExtendedPulseRate::RAPID);
  session.active = true;
  session.extractionExtended = true;
  serviceExtendedPulseAlert();
  CHECK(localBuzzer.activeCue == BuzzerCue::ABNORMAL_FAST);
  CHECK(localBuzzer.activePulseRate ==
        static_cast<uint8_t>(ExtendedPulseRate::RAPID));
}

void r63_slow_guard_disabled_continues_past_max_time() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.slowSideEnabled = false;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishWeight(20.0f);
  publishWeight(20.1f);
  session.lastAcceptedWeightG = 20.1f;
  currentWeight = 20.1f;
  reachSessionElapsed(44000);
  CHECK(stopperState == StopperState::BREW);
  CHECK(!session.slowExtractionExtended);
  CHECK(session.endReason == EndReason::NONE);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void enableAccidentalTouchForTest() {
  runtimeConfig.avoidAccidentalTouchEnabled = true;
  runtimeConfig.avoidAccidentalTouchEnabled = true;
}

void publishControlRamp(float startG, float endG, float stepG, uint32_t intervalMs,
                        uint32_t sequence) {
  (void)sequence;
  float weight = startG;
  while (weight <= endG + 0.0001f) {
    hostMillis += intervalMs;
    markScaleWorkerProgress();
    publishWeight(weight);
    weight += stepG;
  }
}

void prepareTouchFallbackTest() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  mutableActiveShotPreset(presetBank).goalWeightDg = 360;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  hostMillis += 12000;  // Keep fallback endings eligible for the real finalizer.
  publishControlRamp(1.0f, 20.0f, 1.0f, 200, 10);
  CHECK(session.config.touchStopFallbackEnabled);
}

void at11_touch_fallback_only_when_both_enabled() {
  for (bool parent : {false, true}) for (bool child : {false, true}) {
    prepareTouchFallbackTest();
    session.config.avoidAccidentalTouchEnabled = parent;
    session.config.touchStopFallbackEnabled = child;
    // Live settings cannot change the captured choice during this shot.
    runtimeConfig.touchStopFallbackEnabled = !child;
    for (unsigned i = 0; i <= 10 && session.active; ++i) {
      hostMillis += 100;
      publishWeight(40.0f + 2.0f * i);
      loop();
      if (parent && i < 10) CHECK(session.active);
    }
    if (!parent) CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
    else if (!child) CHECK(session.active);
    else {
      CHECK(!session.active);
      CHECK(session.endReason == EndReason::TOUCH_WEIGHT_FALLBACK);
      CHECK(shotCompletionGetsLongBeep(session.endReason));
      checkLocalCompletionTone();
      CHECK(!pendingFinalize.offsetAnalysis);
      CHECK(pendingFinalize.lastKnownWeightG == 20.0f);
      CHECK(shotLogStopDetailFromEndReason(session.endReason, false, false) ==
            ShotLogStopDetail::TOUCH_WEIGHT_FALLBACK);
      CHECK(strcmp(shotLogStopDetailName(ShotLogStopDetail::TOUCH_WEIGHT_FALLBACK),
                   "touch_weight_fallback") == 0);
      ShotLogRecord fallbackRecord = {};
      fallbackRecord.shotType = static_cast<uint8_t>(ShotLogType::AUTO);
      fallbackRecord.goalWeightDg = 360;
      fallbackRecord.stopDetail = static_cast<uint8_t>(
          ShotLogStopDetail::TOUCH_WEIGHT_FALLBACK);
      CHECK(!shotLogBbwErrorEligible(fallbackRecord));
      for (uint8_t algorithm : {0, 1}) {
        pendingFinalize.bbwAlgorithm = algorithm;
        CHECK(!learnPendingBbw(pendingFinalize, 36.0f, true));
      }
    }
  }
}

void at12_touch_fallback_duration_and_evidence_resets() {
  prepareTouchFallbackTest();
  hostMillis += 100;
  publishWeight(40.0f);
  const uint32_t first = hostMillis;
  publishWeight(46.0f, first + 500);
  publishWeight(52.0f, first + 999);
  CHECK(!automaticScaleStopDue());
  publishWeight(54.0f, first + 1000);
  CHECK(automaticScaleStopDue());

  // Interruption of evidence must not leave an old ready stop behind.
  for (unsigned scenario = 0; scenario < 12; ++scenario) {
    prepareTouchFallbackTest();
    hostMillis += 100;
    publishWeight(40.0f);
    publishWeight(44.0f, hostMillis + 200);
    CHECK(session.touchStop.samples == 2);
    const auto last = session.touchStop;
    switch (scenario) {
      case 0: hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 1; break;
      case 1: publishWeight(NAN, hostMillis + 1); break;
      case 2: publishWeight(25.0f, hostMillis + 100); break;
      case 3: publishWeight(46.0f, last.lastAtMs, last.connectionGeneration,
                            last.packetSequence); break;
      case 4: publishWeight(46.0f, last.lastAtMs - 1, last.connectionGeneration,
                            last.packetSequence + 1); break;
      case 5: suspendWeightControl(); break;
      case 6: armPostTareBaselineWindow(); break;
      case 7: publishWeight(200.0f, hostMillis + 100); break; // Slew filter.
      case 8: publishWeight(46.0f, hostMillis + 100, last.connectionGeneration,
                            last.packetSequence + 2); break;
      case 9: publishWeight(46.0f, hostMillis + 100,
                            last.connectionGeneration + 1, last.packetSequence + 1); break;
      case 10: publishWeight(64.0f, hostMillis + 1001); break;
      case 11: recordWeightSampleWithProvenance(46.0f, hostMillis + 100,
                   last.packetSequence + 1, last.connectionGeneration); break;
    }
    CHECK(!automaticScaleStopDue());
    if (scenario != 0) CHECK(session.touchStop.samples <= 1);
  }

  prepareTouchFallbackTest();
  hostMillis += 100;
  publishWeight(40.0f);
  const uint32_t heldAt = hostMillis;
  publishWeight(40.1f, hostMillis + 100);
  publishWeight(40.2f, hostMillis + 100); // A flat plateau above f stays held.
  publishWeight(40.3f, hostMillis + 100);
  loop();
  CHECK(session.active && session.accidentalTouchHolding);
  for (uint32_t at = hostMillis + 100; at <= heldAt + TOUCH_STOP_FALLBACK_MS; at += 100)
    publishWeight(40.3f, at);
  loop();
  CHECK(session.endReason == EndReason::TOUCH_WEIGHT_FALLBACK);
}

void at13_touch_fallback_guard_thresholds() {
  // The touch fallback fires on the segment of f at the sample: an
  // unreachable ceiling never fires; raised, target and lowered segments do.
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    prepareTouchFallbackTest();
    corridor::Params &p = session.cut.params;
    const int32_t now = static_cast<int32_t>(cycleShotElapsedMs());
    p.delayMs = 0;
    p.fastOn = scenario <= 1;
    p.slowOn = scenario == 3;
    p.fastMarginDg = scenario == 0 ? 340 : 65;  // 70 g / 42.5 g ceilings
    p.slowMarginDg = 20;                        // 34 g floor
    p.fastBeforeMs = scenario <= 1 ? now + 20000 : now - 8000;
    p.slowAfterMs = scenario <= 1 ? now + 22000 : now - 6000;
    for (unsigned i = 0; i <= 15; ++i) {  // >= 1 s of evidence above 41 g
      hostMillis += 100;
      publishWeight(40.0f + 2.0f * i);
    }
    if (scenario == 0) {
      CHECK(!automaticScaleStopDue());
      continue;
    }
    CHECK(session.touchStop.policy ==
          (scenario == 1 ? EndReason::FAST_EXTRACTION_MAX_WEIGHT
           : scenario == 3 ? EndReason::SLOW_EXTRACTION_MIN_WEIGHT
                           : EndReason::SCALE_THRESHOLD));
    CHECK(automaticScaleStopDue());
    CHECK(session.directStopReason == EndReason::TOUCH_WEIGHT_FALLBACK);
  }

  // A segment change restarts the evidence.
  prepareTouchFallbackTest();
  corridor::Params &p = session.cut.params;
  p.delayMs = 0;
  p.rampMs = 0;
  p.fastOn = true;
  p.fastBeforeMs = static_cast<int32_t>(cycleShotElapsedMs()) + 450;
  publishWeight(44.0f, hostMillis + 100);
  publishWeight(48.0f, hostMillis + 300);
  CHECK(session.touchStop.samples == 2);
  publishWeight(52.0f, hostMillis + 100);  // Past t1: the target segment.
  CHECK(session.touchStop.samples == 1);
  CHECK(!automaticScaleStopDue());
}

void at14_touch_fallback_respects_inhibitions_and_rollover() {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    prepareTouchFallbackTest();
    if (scenario == 0) session.bbwProtectionEnded = false;
    if (scenario == 1) session.config.timerOnly = true;
    if (scenario == 2) stopperState = StopperState::RINSE;
    if (scenario == 3) stopperState = StopperState::MANUAL_NO_SCALE;
    if (scenario == 4) session.awaitingPostTareBaseline = true;
    if (scenario == 5) machineCyclePaddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
    for (unsigned i = 0; i <= 10; ++i) {
      // A pending baseline is consumed by its first plausible sample; keep it
      // pending so the whole held burst runs under the inhibition.
      if (scenario == 4) session.awaitingPostTareBaseline = true;
      publishWeight(40.0f + 2.0f * i, hostMillis + 100);
    }
    CHECK(session.touchStop.samples == 0);
    CHECK(session.endReason == EndReason::NONE);
  }
  prepareTouchFallbackTest();
  hostMillis = UINT32_MAX - 600;
  session.startedAtMs = hostMillis - 16000;
  shot.startMs = session.startedAtMs;
  resetWeightTrend();
  session.lastAcceptedWeightAtMs = hostMillis;
  hostMillis += 100;
  markScaleWorkerProgress();
  publishWeight(session.lastAcceptedWeightG);  // Re-seed the rebased trend.
  for (unsigned i = 0; i <= 10; ++i) {
    hostMillis += 100;
    markScaleWorkerProgress();
    publishWeight(40.0f + 2.0f * i);
  }
  CHECK(session.touchStop.samples >= 3);
  CHECK(automaticScaleStopDue());
  CHECK(session.directStopReason == EndReason::TOUCH_WEIGHT_FALLBACK);
}

void at16_touch_fallback_uses_receive_time_for_guard_boundaries() {
  for (unsigned scenario = 0; scenario < 2; ++scenario) {
    const bool fast = scenario == 0;
    prepareTouchFallbackTest();
    corridor::Params &p = session.cut.params;
    p.delayMs = 0;
    p.rampMs = 0;  // Steps make the boundary exact.
    p.fastOn = fast;
    p.slowOn = !fast;
    p.fastMarginDg = 340;  // 70 g ceiling
    if (!fast) {
      p.goalDg = 600;
      p.slowMarginDg = 260;  // 34 g floor
    }
    hostMillis += 700;
    const int32_t boundary = static_cast<int32_t>(cycleShotElapsedMs()) - 300;
    p.fastBeforeMs = boundary;
    p.slowAfterMs = fast ? boundary + 10000 : boundary;
    if (!fast) p.fastBeforeMs = boundary - 10000;
    // Fresh but delayed delivery must not backdate the newly eligible policy.
    publishWeight(fast ? 40.0f : 35.0f, hostMillis - 500);
    CHECK(session.accidentalTouchHolding);
    CHECK(session.touchStop.samples == 0);
    publishWeight(fast ? 46.0f : 41.0f, hostMillis - 100);
    CHECK(session.touchStop.samples == 1);
    CHECK(session.touchStop.firstAtMs == hostMillis - 100);
  }
}

void at15_touch_fallback_preset_presence() {
  // Touch-stop fallback is device-level: the v1 preset form still carries it
  // and the save writes the shared runtime value, never the preset.
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.touchStopFallbackEnabled = false;
  const ShotPreset &preset = activeShotPreset(presetBank);
  WebCommand save;
  save.type = WebCommandType::PRESET_OP;
  save.presetAction = static_cast<uint8_t>(PresetAction::SAVE);
  save.presetId = preset.id;
  save.requestId = 950;
  save.config = runtimeConfig;
  processWebCommand(save);
  CHECK(!runtimeConfig.touchStopFallbackEnabled);
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  save.requestId = 951;
  save.config.touchStopFallbackEnabled = true;
  processWebCommand(save);
  CHECK(runtimeConfig.touchStopFallbackEnabled);
  CHECK(!runtimeConfig.avoidAccidentalTouchEnabled);
}

void ff01_classifier_seeking_touch_and_release() {
  FirstFlowState state;
  CHECK(stepFirstFlow(state, 0.35f, 100, 1, 0.0f) == FirstFlowClass::CANDIDATE);
  CHECK(stepFirstFlow(state, 0.40f, 200, 2, 0.0f) == FirstFlowClass::FIRE);
  CHECK(state.candidateMs == 100);
  CHECK(fabsf(state.candidateWeightG - 0.35f) < 0.001f);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 0.4f, 100, 1, 0.0f) == FirstFlowClass::CANDIDATE);
  CHECK(stepFirstFlow(state, 0.9f, 200, 2, 0.0f) == FirstFlowClass::FIRE);
  CHECK(stepFirstFlow(state, 1.5f, 300, 3, 0.0f) == FirstFlowClass::FIRE);
  CHECK(stepFirstFlow(state, 2.4f, 400, 4, 0.0f) == FirstFlowClass::FIRE);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 3.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 3.1f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 3.05f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(state.phase == FirstFlowPhase::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 0.0f, 200, 2, 0.0f) == FirstFlowClass::NONE);
  CHECK(stepFirstFlow(state, 0.05f, 300, 3, 0.0f) == FirstFlowClass::NONE);
  CHECK(state.phase == FirstFlowPhase::SEEKING);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.2f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 0.0f, 300, 3, 0.0f) == FirstFlowClass::NONE);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.2f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.3f, 300, 3, 0.0f) == FirstFlowClass::FIRE);
  CHECK(state.candidateMs == 200);
  CHECK(fabsf(state.candidateWeightG - 1.2f) < 0.001f);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 4.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 5.9f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 6.1f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 6.2f, 400, 4, 0.0f) == FirstFlowClass::FIRE);
  CHECK(state.candidateMs == 100);
  CHECK(fabsf(state.candidateWeightG - 4.0f) < 0.001f);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 200.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 200.2f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 199.8f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 160.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 150.0f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 150.1f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 150.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 150.4f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 150.8f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 3.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 3.4f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 3.8f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 6.0f, 200, 2, 0.0f) == FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 6.1f, 300, 3, 0.0f) == FirstFlowClass::TOUCH);
}

void ff12_cupmin_parameter_gates_touch_and_residual() {
  FirstFlowState state;
  CHECK(stepFirstFlow(state, 15.0f, 100, 1, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 15.4f, 200, 2, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 15.8f, 300, 3, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 18.0f, 100, 1, 0.0f, 15.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 18.4f, 200, 2, 0.0f, 15.0f) ==
        FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 12.0f, 200, 2, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 15.0f, 300, 3, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.2f, 200, 2, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.3f, 300, 3, 0.0f, 10.0f) ==
        FirstFlowClass::FIRE);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f, 15.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.2f, 200, 2, 0.0f, 15.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.3f, 300, 3, 0.0f, 15.0f) ==
        FirstFlowClass::FIRE);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 8.0f, 100, 1, 0.0f, 5.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.2f, 200, 2, 0.0f, 5.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.3f, 300, 3, 0.0f, 5.0f) ==
        FirstFlowClass::TOUCH);

  resetFirstFlowState(state);
  CHECK(stepFirstFlow(state, 15.0f, 100, 1, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.2f, 200, 2, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
  CHECK(stepFirstFlow(state, 1.3f, 300, 3, 0.0f, 10.0f) ==
        FirstFlowClass::TOUCH);
}

void ff13_rinse_first_drop_goes_through_stopper() {
  resetHarness(false, true);
  reachReadyFromBoot();
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(session.active);
  CHECK(session.startedWithScale);
  CHECK(session.firstDropMs == 0);
  publishWeight(0.4f, hostMillis + 50, 1, 20);
  publishWeight(0.9f, hostMillis + 150, 1, 21);
  CHECK(session.firstDropMs != 0);
}

void ff14_no_scale_first_drop_does_not_skip_stopper() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  reachManualNoScaleState();
  CHECK(!session.startedWithScale);
  CHECK(session.firstDropMs == 0);
  (void)recordWeightSampleWithProvenance(0.4f, hostMillis + 50, 20, 1);
  (void)recordWeightSampleWithProvenance(0.9f, hostMillis + 150, 21, 1);
  CHECK(session.firstDropMs == 0);
}

void ff15_orchestrate_post_tare_holds_cup_transitions() {
  resetHarness(false, true);
  reachReadyFromBoot();
  seedCupPresence(80.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  startCycle();
  CHECK(session.awaitingPostTareBaseline);
  publishWeight(0.0f, hostMillis, 1, 20);
  publishWeight(-80.0f, hostMillis + 50, 1, 40);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
}

void at01_classifier_startup_and_trend_math() {
  // 1.5 g/s from 10 g sampled at 10 Hz; the anchor is the newest point.
  float times[20];
  float weights[20];
  for (size_t i = 0; i < 20; ++i) {
    times[i] = 0.1f * static_cast<float>(i);
    weights[i] = 10.0f + 1.5f * times[i];
  }
  auto residual = [&](size_t n, float epochS, float weight, float nowS) {
    return accidentalTouchResidual(times, weights, n, epochS, weights[n - 1],
                                   times[n - 1], weight, nowS);
  };
  AccidentalTouchResidual r = residual(20, 0.0f, 13.0f, 2.0f);
  CHECK(r.frozen && fabsf(r.residualG) < 0.01f);
  CHECK(fabsf(residual(20, 0.0f, 16.0f, 2.0f).residualG - 3.0f) < 0.01f);
  CHECK(fabsf(residual(20, 0.0f, 11.0f, 2.0f).residualG + 2.0f) < 0.01f);
  r = residual(3, 0.0f, 10.45f, 0.3f);  // Young: flow from the whole history.
  CHECK(!r.frozen && fabsf(r.residualG) < 0.01f);
  CHECK(!residual(20, 1.55f, 13.0f, 2.0f).frozen);  // Level shift at 1.55 s.
  r = accidentalTouchResidual(times, weights, 0, 0.0f, 0.0f, -0.3f, 2.0f, 0.0f);
  CHECK(!r.frozen && r.residualG == 0.0f);  // Pre-cycle anchor: no trend.

  CHECK(classifyAccidentalTouch(1.4f, false, 0, nullptr, 0) ==
        AccidentalTouchClass::OK);
  CHECK(classifyAccidentalTouch(1.5f, false, 0, nullptr, 0) ==
        AccidentalTouchClass::TOUCH);
  CHECK(classifyAccidentalTouch(-1.5f, false, 0, nullptr, 0) ==
        AccidentalTouchClass::TOUCH);
  CHECK(classifyAccidentalTouch(1.2f, true, 300, nullptr, 0) ==
        AccidentalTouchClass::TOUCH);  // Hysteresis keeps the hold.
  CHECK(classifyAccidentalTouch(0.9f, true, 300, nullptr, 0) ==
        AccidentalTouchClass::OK);
  CHECK(classifyAccidentalTouch(28.0f, true, ACCIDENTAL_TOUCH_MAX_HOLD_MS,
                                nullptr, 0) == AccidentalTouchClass::SUSTAINED);
  float flat[ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES - 1U];
  for (float &value : flat) value = 3.0f;
  CHECK(classifyAccidentalTouch(3.2f, true, 900, flat, 8) ==
        AccidentalTouchClass::TOUCH);
  CHECK(classifyAccidentalTouch(3.2f, true, 900, flat, 9) ==
        AccidentalTouchClass::SUSTAINED);
  flat[0] = 4.5f;  // Not flat: still a contact.
  CHECK(classifyAccidentalTouch(3.2f, true, 900, flat, 9) ==
        AccidentalTouchClass::TOUCH);
  for (float &value : flat) value = 8.0f;  // A flat resting load.
  CHECK(classifyAccidentalTouch(8.0f, true, 900, flat, 9) ==
        AccidentalTouchClass::TOUCH);
}

void at02_early_spike_skips_control_trajectory() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  startCycle();
  advanceToBrew();
  publishControlRamp(0.2f, 5.0f, 0.4f, 100, 10);
  CHECK(!session.accidentalTouchHolding);
  const size_t before = shot.datapoints;
  const float expectedEnd = shot.expectedEndS;
  const float lastAccepted = session.lastAcceptedWeightG;
  hostMillis += 100;
  publishWeight(lastAccepted + 8.0f);
  CHECK(session.accidentalTouchHolding);
  CHECK(shot.datapoints == before);
  CHECK(fabsf(shot.expectedEndS - expectedEnd) < 0.001f);
  CHECK(fabsf(session.lastAcceptedWeightG - lastAccepted) < 0.001f);
  CHECK(stopperState == StopperState::BREW);
}

void at03_late_spike_does_not_cut_then_recovers() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  runtimeConfig.goalWeightDg = 360;
  mutableActiveShotPreset(presetBank).goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 20.0f, 1.0f, 200, 10);
  CHECK(session.accidentalTouchPhase == AccidentalTouchPhase::TREND);
  const float threshold = effectiveStopThreshold();
  hostMillis += 100;
  publishWeight(threshold + 8.0f);
  hostMillis += 100;
  publishWeight(threshold + 8.1f);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
  CHECK(session.accidentalTouchHolding);
  hostMillis += 100;
  publishWeight(21.5f);  // Back on the 5 g/s trend.
  CHECK(!session.accidentalTouchHolding);
  CHECK(stopperState == StopperState::BREW);
}

void at04_smooth_approach_cuts_on_two_samples() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  runtimeConfig.goalWeightDg = 360;
  mutableActiveShotPreset(presetBank).goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  const float threshold = effectiveStopThreshold();
  publishControlRamp(1.0f, threshold - 0.4f, 0.4f, 100, 10);
  hostMillis += 100;
  publishWeight(threshold + 0.1f);
  hostMillis += 100;
  publishWeight(threshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void at05_spike_before_min_bbw_brew_time_does_not_enter_fast_extended() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  mutableActiveShotPreset(presetBank).fastSideEnabled = true;
  mutableActiveShotPreset(presetBank).fastBeforeMs = 26000;
  mutableActiveShotPreset(presetBank).goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 20.0f, 1.0f, 200, 10);
  const float threshold = effectiveStopThreshold();
  hostMillis += 100;
  publishWeight(threshold + 6.0f);
  CHECK(!session.extractionExtended);
  CHECK(session.accidentalTouchHolding);
  CHECK(stopperState == StopperState::BREW);
}

void at06_fast_extended_spike_does_not_cut_until_sustained() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.avoidAccidentalTouchEnabled = false;
  runtimeConfig.fastSideEnabled = true;
  runtimeConfig.fastMarginDg = 65;  // 42.5 g at the 36 g target
  runtimeConfig.fastBeforeMs = 26000;
  runtimeConfig.goalWeightDg = 360;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  runLoopAfter(22000);
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  CHECK(session.extractionExtended);
  session.config.avoidAccidentalTouchEnabled = true;
  runtimeConfig.avoidAccidentalTouchEnabled = true;
  const float maxThreshold = ceilingStopThresholdForTest();
  hostMillis += 100;
  publishWeight(maxThreshold + 0.5f);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.endReason == EndReason::NONE);
  CHECK(session.accidentalTouchHolding);
  // A held plateau above the ceiling is touch evidence: it ends the shot
  // only through the labelled fallback after one second on one segment.
  const uint32_t first = hostMillis;
  while (stopperState == StopperState::BREW && hostMillis < first + 3000U) {
    hostMillis += 100;
    publishWeight(maxThreshold + 0.6f);
    loop();
  }
  CHECK(hostMillis >= first + TOUCH_STOP_FALLBACK_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::TOUCH_WEIGHT_FALLBACK);
  CHECK(!session.calibrationEligible);
}

void at07_feature_off_matches_legacy_threshold_cut() {
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(!runtimeConfig.avoidAccidentalTouchEnabled);
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  const float threshold = effectiveStopThreshold();
  publishWeight(threshold + 0.1f);
  publishWeight(threshold + 0.2f);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void at08_retare_and_cup_remove_still_work_with_touch_guard() {
  resetHarness(false, true);
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  enableAccidentalTouchForTest();
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(retareWindowOpen());
  publishStableCupWeight(150.0f, 10);
  CHECK(session.retarePerformed);

  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  seedCupPresence(80.0f);
  startCycle();
  advanceToBrew();
  publishWeight(-3.0f, hostMillis, 1, 70);
  publishWeight(-3.1f, hostMillis + 1, 1, 71);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void publishSequencedWeight(float weight, uint32_t &sequence,
                            uint32_t intervalMs = 100) {
  hostMillis += intervalMs;
  markScaleWorkerProgress();
  publishWeight(weight, hostMillis, 0, ++sequence);
}

void at09_slew_recovery_advances_control_weight() {
  // A single implausible packet followed by samples that continue the flow
  // recovers control onto the advancing trajectory, not onto the spike.
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  startCycle();
  advanceToBrew();
  publishControlRamp(0.2f, 5.0f, 0.4f, 100, 10);
  const float lastAccepted = session.lastAcceptedWeightG;
  const uint32_t seq = session.lastAcceptedPacketSequence;
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  hostMillis += 100;
  publishWeight(lastAccepted + 35.0f, hostMillis, 0, seq + 1);
  CHECK(session.weightControlState == WeightControlState::VALIDATING);
  for (uint32_t i = 2; i <= 4; ++i) {
    hostMillis += 100;
    publishWeight(lastAccepted + 0.4f * i, hostMillis, 0, seq + i);
  }
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(!session.accidentalTouchHolding);
  CHECK(session.lastAcceptedWeightG > lastAccepted + 1.5f);
  hostMillis += 100;
  publishWeight(lastAccepted + 2.0f, hostMillis, 0, seq + 5);
  CHECK(session.lastAcceptedWeightG > lastAccepted + 1.9f);
}

void at18_heavy_touch_is_held_then_fallback() {
  // A heavy hand on the cup trips the slew guard first; once the stream is
  // coherent again the step still goes through the touch classifier, so it
  // can only end the shot through the labelled touch fallback.
  prepareTouchFallbackTest();
  const float base = session.lastAcceptedWeightG;
  uint32_t seq = session.lastAcceptedPacketSequence;
  CHECK(base + 35.0f > effectiveStopThreshold());
  bool due = false;
  for (int i = 0; i < 20 && !due; ++i) {
    publishSequencedWeight(base + 35.0f + ((i & 1) ? 1.5f : 0.0f), seq);
    due = automaticScaleStopDue();
  }
  CHECK(due);
  CHECK(session.directStopReason == EndReason::TOUCH_WEIGHT_FALLBACK);
  CHECK(session.touchStop.lastAtMs - session.touchStop.firstAtMs >=
        TOUCH_STOP_FALLBACK_MS);
  CHECK(!session.calibrationEligible);
}

void at11_pre_cycle_anchor_does_not_treat_first_pour_as_touch() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  runtimeConfig.autoTare = false;
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  startCycle();
  advanceToBrew();
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 0.0f;
  session.lastAcceptedWeightAtMs = session.startedAtMs - ACTIVATOR_DEBOUNCE_MS;
  shot.datapoints = 0;  // The anchor predates the cycle's trajectory.
  hostMillis += 300;
  publishWeight(2.0f);
  CHECK(!session.accidentalTouchHolding);
  CHECK(session.lastAcceptedWeightG > 1.5f);
}

void startFirstFlowBrew() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
}

void ff17_noise_adaptive_threshold_and_rate_gate() {
  // Table-driven first-drop fuzz with a sigma-hat column. A fixed LCG seed
  // keeps the noise case deterministic across platforms.
  uint32_t seed = 0xBEEF;
  auto noise = [&seed]() {
    seed = seed * 1664525U + 1013904223U;
    return ((seed >> 8) & 0xFFFFU) / 65535.0f * 2.0f - 1.0f;  // [-1, 1)
  };
  // 200 near-N(0, 0.3 g) samples (sum of three uniforms) never fire, and
  // the noise-adaptive floor engages after warmup.
  {
    FirstFlowState state;
    bool fired = false;
    uint32_t atMs = 1000;
    for (int i = 0; i < 200; ++i, atMs += 100) {
      const float sample = (noise() + noise() + noise()) * 0.30f;
      if (stepFirstFlow(state, sample, atMs, i + 1, 0.0f) ==
          FirstFlowClass::FIRE) {
        fired = true;
      }
    }
    CHECK(!fired);
    CHECK(state.noiseSamples >= FIRST_DROP_NOISE_WARMUP_SAMPLES);
    CHECK(firstFlowEffectiveThresholdG(state) > FIRST_DROP_THRESHOLD_G);
    CHECK(fabsf(sqrtf(state.noiseVarHatG) - 0.3f) < 0.1f);
  }
  // A 1 g/s ramp fires on the second packet past the threshold.
  {
    FirstFlowState state;
    FirstFlowClass classified = FirstFlowClass::NONE;
    uint32_t atMs = 1000;
    for (int i = 1; i <= 6 && classified != FirstFlowClass::FIRE;
         ++i, atMs += 100) {
      classified = stepFirstFlow(state, 0.1f * i, atMs, i, 0.0f);
    }
    CHECK(classified == FirstFlowClass::FIRE);
  }
  // A gentle 0.15 g/s onset still fires.
  {
    FirstFlowState state;
    FirstFlowClass classified = FirstFlowClass::NONE;
    uint32_t atMs = 1000;
    for (int i = 1; i <= 40 && classified != FirstFlowClass::FIRE;
         ++i, atMs += 100) {
      classified = stepFirstFlow(state, 0.015f * i, atMs, i, 0.0f);
    }
    CHECK(classified == FirstFlowClass::FIRE);
  }
  // 0.03 g/s thermal creep for a full minute never fires.
  {
    FirstFlowState state;
    bool fired = false;
    uint32_t atMs = 1000;
    for (int i = 1; i <= 600; ++i, atMs += 100) {
      if (stepFirstFlow(state, 0.003f * i, atMs, i, 0.0f) ==
          FirstFlowClass::FIRE) {
        fired = true;
      }
    }
    CHECK(!fired);
  }
  // A gentle onset riding vibration (the rise-freeze alone cannot contain
  // σ̂ here; the deviation cap must) still fires.
  {
    uint32_t seed = 42;
    auto jitter = [&seed]() {
      seed = seed * 1664525U + 1013904223U;
      return ((seed >> 8) & 0xFFFFU) / 65535.0f * 0.1f - 0.05f;
    };
    FirstFlowState state;
    FirstFlowClass classified = FirstFlowClass::NONE;
    uint32_t atMs = 1000;
    int q = 0;
    for (int i = 0; i < 30 && classified != FirstFlowClass::FIRE;
         ++i, ++q, atMs += 100) {
      classified = stepFirstFlow(state, jitter() * 0.6f, atMs, q + 1, 0.0f);
    }
    for (int i = 1; i <= 150 && classified != FirstFlowClass::FIRE;
         ++i, ++q, atMs += 100) {
      classified = stepFirstFlow(state, 0.015f * i + jitter(), atMs, q + 1,
                                 0.0f);
    }
    CHECK(classified == FirstFlowClass::FIRE);
    CHECK(firstFlowEffectiveThresholdG(state) <= 0.91f);  // Cap holds.
  }
  // A slowly creeping load after a touch (through path) cannot latch a
  // first drop: its mean rate from the jump stays under the rate gate.
  {
    FirstFlowState state;
    bool fired = false;
    uint32_t atMs = 1000;
    for (int i = 0; i < 260; ++i, atMs += 100) {
      // Jump to 5 g, then creep +0.005 g/sample (0.05 g/s) to ~+1.3 g.
      const float weight = i == 0 ? 5.0f : 5.0f + 0.005f * i;
      if (stepFirstFlow(state, weight, atMs, i + 1, 0.0f) ==
          FirstFlowClass::FIRE) {
        fired = true;
      }
    }
    CHECK(!fired);
    CHECK(state.phase == FirstFlowPhase::TOUCH);
  }
}

void ff21_first_flow_hardening() {
  // F25: a creeping streak above threshold never wraps its confirmation
  // counter, so its candidate time stays the first sample of the streak.
  {
    FirstFlowState state;
    bool fired = false;
    uint32_t atMs = 1000;
    for (int i = 0; i < 600; ++i, atMs += 100) {
      fired |= stepFirstFlow(state, 0.4f + 0.0003f * i, atMs, i + 1, 0.0f) ==
               FirstFlowClass::FIRE;
    }
    CHECK(!fired);
    CHECK(state.confirmations == FIRST_DROP_CONFIRMATION_SAMPLES);
    CHECK(state.candidateMs == 1000);
  }
  // F24: 6 g that accumulated over a 2 s packet gap is flow, not a finger;
  // the same step within 100 ms is a touch.
  for (uint32_t gapMs : {2000U, 100U}) {
    FirstFlowState state;
    CHECK(stepFirstFlow(state, 0.0f, 1000, 1, 0.0f) == FirstFlowClass::NONE);
    const FirstFlowClass jump = stepFirstFlow(state, 6.0f, 1000 + gapMs, 2, 0.0f);
    CHECK((jump == FirstFlowClass::TOUCH) == (gapMs == 100U));
    if (gapMs == 2000U) {
      CHECK(state.phase == FirstFlowPhase::SEEKING);
      CHECK(stepFirstFlow(state, 6.3f, 1100 + gapMs, 3, 0.0f) == FirstFlowClass::FIRE);
    }
  }
  // F23: a vibration burst alternating around an offset never confirms;
  // a monotone onset still fires on its second sample.
  {
    FirstFlowState state;
    bool fired = false;
    uint32_t atMs = 1000;
    int seq = 1;
    for (float w : {0.5f, 0.4f, 0.9f, 0.4f, 0.9f, 0.4f, 0.9f, 0.4f}) {
      fired |= stepFirstFlow(state, w, atMs, seq++, 0.0f) == FirstFlowClass::FIRE;
      atMs += 100;
    }
    CHECK(!fired);
    FirstFlowState onset;
    CHECK(stepFirstFlow(onset, 0.4f, 1000, 1, 0.0f) == FirstFlowClass::CANDIDATE);
    CHECK(stepFirstFlow(onset, 0.6f, 1100, 2, 0.0f) == FirstFlowClass::FIRE);
  }
}

void ff22_small_cup_min_gush_fires_through_touch() {
  // F24: with a 2 g minimum cup, a fast gush or cup step enters TOUCH;
  // coffee rising on top still fires, a flat finger or resting cup never does.
  for (float rise : {0.3f, 0.0f}) {
    for (float jumpG : {8.0f, 12.0f}) {
      FirstFlowState state;
      CHECK(stepFirstFlow(state, 0.0f, 1000, 1, 0.0f, 2.0f) == FirstFlowClass::NONE);
      CHECK(stepFirstFlow(state, jumpG, 1100, 2, 0.0f, 2.0f) == FirstFlowClass::TOUCH);
      bool fired = false;
      for (int i = 1; i <= 12; ++i) {
        fired |= stepFirstFlow(state, jumpG + rise * i, 1100 + 100 * i, 2 + i,
                               0.0f, 2.0f) == FirstFlowClass::FIRE;
      }
      CHECK(fired == (rise > 0.0f));
    }
  }
}

void ff18_duplicate_timestamp_pairs() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  const size_t before = shot.datapoints;
  const uint32_t atMs = hostMillis + 100;
  publishWeight(20.0f, atMs, 1, 100);
  const size_t afterFirst = shot.datapoints;
  CHECK(afterFirst == before + 1);
  session.receivedFreshWeightInCycle = false;
  publishWeight(20.5f, atMs, 1, 101);  // Same millisecond, next packet.
  CHECK(shot.datapoints == afterFirst);  // Inserted once.
  CHECK(session.receivedFreshWeightInCycle);
  CHECK(session.lastAcceptedPacketSequence == 101);
  CHECK(fabsf(session.lastAcceptedWeightG - 20.5f) < 1e-4f);

  // A zero-span pair above the stop threshold cannot confirm a direct stop.
  endBbwProtectionForTests();
  session.config.avoidAccidentalTouchEnabled = false;  // Keep the jump plain.
  resetDirectStopConfirmation();
  const float threshold = effectiveStopThreshold();
  const uint32_t pairMs = hostMillis + 10;
  publishWeight(threshold + 1.0f, pairMs, 1, 102);
  CHECK(session.thresholdConfirmations == 1);
  publishWeight(threshold + 1.2f, pairMs, 1, 103);  // Same millisecond.
  CHECK(session.thresholdConfirmations == 1);  // dt=0 restarts, never confirms.
  // SCALE_THRESHOLD_CONFIRMED names its firing law: 1 = confirmed direct
  // samples, 2 = predicted end time. Capture right before the confirming
  // sample so only the new event is inspected.
  const uint32_t beforeDirect = debugLogLatestSequence();
  publishWeight(threshold + 1.4f, pairMs + 110, 1, 104);
  CHECK(session.thresholdConfirmations == 2);
  DebugEvent direct[4] = {};
  const size_t directCount = copyDebugEvents(beforeDirect, direct, 4);
  bool sawDirectLaw = false;
  for (size_t i = 0; i < directCount; ++i) {
    sawDirectLaw |= direct[i].code == DebugCode::SCALE_THRESHOLD_CONFIRMED &&
                    direct[i].argument2 == 1;
  }
  CHECK(sawDirectLaw);
  debugLog.clear();
  resetDirectStopConfirmation();  // Isolate the predicted-end law.
  shot.expectedEndS = 0.0f;
  CHECK(automaticScaleStopDue());
  DebugEvent predicted[2] = {};
  const size_t predictedCount = copyDebugEvents(0, predicted, 2);
  CHECK(predictedCount > 0 &&
        predicted[predictedCount - 1].code ==
            DebugCode::SCALE_THRESHOLD_CONFIRMED &&
        predicted[predictedCount - 1].argument2 == 2);
}

void at16_resting_finger_contained_by_direct_stop_law() {
  // A gently drifting extra load keeps the classifier holding TOUCH: the
  // direct stop law must refuse it, and only the explicitly touch-labeled
  // fallback may end the shot, calibration-ineligible.
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  session.config.touchStopFallbackEnabled = false;  // Isolate the direct law.
  hostMillis += 100;
  publishWeight(40.0f);
  CHECK(session.accidentalTouchHolding);
  for (int i = 0; i < 6; ++i) {
    hostMillis += 200;
    publishWeight(44.0f + 0.5f * i);  // Resting finger, gently drifting.
    CHECK(session.accidentalTouchHolding);
    CHECK(!automaticScaleStopDue());
  }
  CHECK(session.active);

  // Fallback enabled: the sustained hold ends the shot only through the
  // touch-labeled path and never feeds offset learning.
  prepareTouchFallbackTest();
  hostMillis += 100;
  publishWeight(40.0f);
  const uint32_t first = hostMillis;
  publishWeight(46.0f, first + 500);
  publishWeight(50.0f, first + 999);
  CHECK(!automaticScaleStopDue());
  publishWeight(54.0f, first + 1000);
  CHECK(automaticScaleStopDue());
  CHECK(session.directStopReason == EndReason::TOUCH_WEIGHT_FALLBACK);
  CHECK(!session.calibrationEligible);
}

void at17_heavy_touch_release_recovers_control() {
  // A heavy step held for a second is accepted as a plateau; releasing it
  // must return control to the original trajectory instead of locking
  // VALIDATING behind the recovery drop floor.
  resetHarness(false, true);
  reachReadyFromBoot();
  mutableActiveShotPreset(presetBank).goalWeightDg = 1000;
  runtimeConfig.goalWeightDg = 1000;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 20.0f, 0.5f, 100, 10);
  const float base = session.lastAcceptedWeightG;
  uint32_t seq = session.lastAcceptedPacketSequence;
  for (int i = 0; i < 10; ++i) publishSequencedWeight(base + 35.0f, seq);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(session.lastAcceptedWeightG > base + 30.0f);
  int recoveredAt = -1;
  for (int i = 0; i < 6 && recoveredAt < 0; ++i) {
    publishSequencedWeight(base + 0.1f * i, seq);
    if (session.weightControlState == WeightControlState::ACTIVE &&
        session.lastAcceptedWeightG < base + 1.0f) recoveredAt = i;
  }
  CHECK(recoveredAt >= 1 && recoveredAt <= 3);
  CHECK(session.active);
  const float threshold = effectiveStopThreshold();
  for (float w = base + 1.0f; w < threshold - 1.0f; w += 1.0f)
    publishSequencedWeight(w, seq);
  publishSequencedWeight(threshold + 0.1f, seq);
  publishSequencedWeight(threshold + 0.2f, seq);
  loop();
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void at19_late_cup_step_held() {
  // A cup set down after the retare window is a 250 g step: the slew guard
  // rejects it until it becomes plausible (~2.3 s), then it is held as a
  // touch and ends the shot only through the labelled fallback.
  prepareTouchFallbackTest();
  CHECK(!retareWindowOpen());
  const float base = session.lastAcceptedWeightG;
  uint32_t seq = session.lastAcceptedPacketSequence;
  bool due = false;
  for (int i = 0; i < 40 && !due; ++i) {
    publishSequencedWeight(base + 250.0f, seq);
    due = automaticScaleStopDue();
  }
  CHECK(due);
  CHECK(session.directStopReason == EndReason::TOUCH_WEIGHT_FALLBACK);
  CHECK(!session.retarePerformed);
  CHECK(!session.calibrationEligible);
}

void startSlowFlowForTouch(float goalG) {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  mutableActiveShotPreset(presetBank).goalWeightDg = gramsToDg(goalG);
  runtimeConfig.goalWeightDg = gramsToDg(goalG);
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  publishControlRamp(0.15f, 12.0f, 0.15f, 100, 10);  // 1.5 g/s at 10 Hz.
  CHECK(session.accidentalTouchPhase == AccidentalTouchPhase::TREND);
}

void at20_resting_load_under_flow_stays_held() {
  // A hand resting on the cup while coffee keeps flowing is a flat-ish
  // +28 g plateau: it stays a held touch, never a SUSTAINED new weight.
  startSlowFlowForTouch(100.0f);
  const float base = session.lastAcceptedWeightG;
  uint32_t seq = session.lastAcceptedPacketSequence;
  for (int i = 1; i <= 10; ++i) {
    publishSequencedWeight(base + 28.0f + 0.15f * i, seq);
    CHECK(session.accidentalTouchHolding);
    CHECK(session.accidentalTouchClass == AccidentalTouchClass::TOUCH);
    CHECK(!automaticScaleStopDue());
  }
  CHECK(session.lastAcceptedWeightG < base + 0.01f);
  CHECK(session.active);
}

size_t shotCurveBitCount(const uint8_t *bits, size_t from = 1) {
  size_t set = 0;
  for (size_t i = from; i < shotCurveSampler.count; ++i)
    set += (bits[i / 8U] >> (i % 8U)) & 1U;
  return set;
}

size_t shotCurveBreakCount() {
  return shotCurveBitCount(shotCurveSampler.breakBefore);
}

// Feeds a contact on top of a steady flow: `profile` holds the extra grams of
// each 100 ms sample, followed by ten samples `tailG` off the trend.
struct HandContactResult {
  int leakedSamples;     // accepted more than 0.3 g above the trend
  float worstLeakG;
  bool anchorMovedWhileHeld;
  size_t breaks;
};

HandContactResult runHandContact(float cupG, float flowGS, const float *profile,
                                 size_t profileCount, float tailG = 0.0f) {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  mutableActiveShotPreset(presetBank).goalWeightDg = 1000;
  runtimeConfig.goalWeightDg = 1000;
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  const float step = flowGS * 0.1f;
  publishControlRamp(step, cupG, step, 100, 10);
  HandContactResult result = {0, 0.0f, false, 0};
  CHECK_VALUE(session.accidentalTouchPhase == AccidentalTouchPhase::TREND, result);
  CHECK_VALUE(session.calibrationEligible, result);
  const float base = session.lastAcceptedWeightG;
  const uint32_t baseAtMs = session.lastAcceptedWeightAtMs;
  const size_t breaksBefore = shotCurveBreakCount();
  for (size_t i = 1; i <= profileCount + 10U; ++i) {
    const float trendG = base + step * static_cast<float>(i);
    const float extraG = i <= profileCount ? profile[i - 1U] : tailG;
    const uint32_t anchorAtMs = session.lastAcceptedWeightAtMs;
    const float expectedEnd = shot.expectedEndS;
    const bool wasHolding = session.accidentalTouchHolding;
    hostMillis += 100;
    markScaleWorkerProgress();
    publishWeight(trendG + extraG);
    loop();
    CHECK_VALUE(stopperState == StopperState::BREW, result);
    if (session.lastAcceptedWeightAtMs != anchorAtMs) {
      const float acceptedTrendG =
          base + flowGS * (session.lastAcceptedWeightAtMs - baseAtMs) / 1000.0f;
      const float leakG = session.lastAcceptedWeightG - acceptedTrendG;
      if (fabsf(leakG) > 0.3f) ++result.leakedSamples;
      result.worstLeakG = fmaxf(result.worstLeakG, fabsf(leakG));
    } else if (wasHolding &&
               fabsf(shot.expectedEndS - expectedEnd) > 0.001f) {
      result.anchorMovedWhileHeld = true;
    }
  }
  CHECK_VALUE(!session.accidentalTouchHolding, result);
  result.breaks = shotCurveBreakCount() - breaksBefore;
  return result;
}

void at21_spurt_plateau_is_accepted_without_learning() {
  // A persistent +3 g step riding on the flow is a real level shift: it is
  // accepted after a one-second flat residual, calibration-ineligible.
  startSlowFlowForTouch(100.0f);
  CHECK(session.calibrationEligible);
  const float base = session.lastAcceptedWeightG;
  const size_t breaksBefore = shotCurveBreakCount();
  uint32_t seq = session.lastAcceptedPacketSequence;
  for (int i = 1; i < ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES; ++i) {
    publishSequencedWeight(base + 3.0f + 0.15f * i, seq);
    CHECK(session.accidentalTouchHolding);
  }
  publishSequencedWeight(base + 3.0f + 0.15f * ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES,
                         seq);
  CHECK(session.accidentalTouchClass == AccidentalTouchClass::SUSTAINED);
  CHECK(!session.accidentalTouchHolding);
  CHECK(fabsf(session.lastAcceptedWeightG - (base + 4.5f)) < 0.01f);
  CHECK(!session.calibrationEligible);
  CHECK(shotCurveBreakCount() == breaksBefore + 1U);  // A new segment...
  const size_t last = shotCurveSampler.count - 1U;  // ...on the accepted shift,
  CHECK((shotCurveSampler.breakBefore[last / 8U] >> (last % 8U)) & 1U);
  CHECK(shotCurveBitCount(shotCurveSampler.held, 0) ==  // after its held run.
        ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES - 1U);
}

void at22_held_touch_below_line_releases_after_bounded_hold() {
  // Below the cut line a touch that never lifts is accepted as weight once
  // the hold reaches its bound; the direct law can then stop on it, so the
  // extra mass ends the shot early rather than letting it overflow.
  startSlowFlowForTouch(100.0f);
  const float base = session.lastAcceptedWeightG;
  uint32_t seq = session.lastAcceptedPacketSequence;
  int releasedAt = -1;
  for (int i = 1; i <= 80 && releasedAt < 0; ++i) {
    publishSequencedWeight(base + 10.0f + 0.15f * i, seq);
    if (!session.accidentalTouchHolding) releasedAt = i;
  }
  CHECK(releasedAt == ACCIDENTAL_TOUCH_MAX_HOLD_MS / 100 + 1);
  CHECK(session.accidentalTouchClass == AccidentalTouchClass::SUSTAINED);
  CHECK(session.lastAcceptedWeightG > base + 10.0f);
  CHECK(!session.calibrationEligible);
}

void at23_hand_contact_is_absorbed_at_low_and_high_cup_weight() {
  // +3 g rising over two samples, resting 0.5 s, released: neither a 4 g cup
  // on a 0.4 g/s shot nor a 20 g cup at 1.5 g/s may take the contact as flow.
  // Only the leading sample can reach the gate (+1.5 g), and nothing after it.
  const float contact[] = {1.5f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.5f};
  const float shotShapes[2][2] = {{4.0f, 0.4f}, {20.0f, 1.5f}};
  for (const auto &shape : shotShapes) {
    const HandContactResult r = runHandContact(shape[0], shape[1], contact, 8);
    CHECK(r.leakedSamples <= 1);
    CHECK(r.worstLeakG <= ACCIDENTAL_TOUCH_RESIDUAL_G + 0.01f);
    CHECK(!r.anchorMovedWhileHeld);
    CHECK(r.breaks == 0);
    CHECK(session.calibrationEligible);
  }
  // A leading edge just under the gate is accepted once, but kept out of the
  // trend: it must not later pose as the reference and hold clean flow.
  const float edge[] = {1.4f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 1.5f};
  const HandContactResult edged = runHandContact(4.0f, 0.4f, edge, 8);
  CHECK(edged.leakedSamples == 1);
  CHECK(edged.breaks == 0);
  CHECK(session.calibrationEligible);
  // A step rising in one sample is held from its first sample.
  const float sharp[] = {3.0f, 3.0f, 3.0f, 3.0f, 3.0f, 3.0f};
  const HandContactResult r = runHandContact(4.0f, 0.4f, sharp, 6);
  CHECK(r.leakedSamples == 0);
  CHECK(r.breaks == 0);
  CHECK(shotCurveBitCount(shotCurveSampler.held, 0) == 6);  // Recorded, flagged.
}

void at24_ramped_contact_and_lift_are_held() {
  // +1 g per sample (10 g/s) is no espresso flow: held once 1.5 g off trend.
  const float ramp[] = {1.0f, 2.0f, 3.0f, 3.0f, 3.0f, 1.5f};
  HandContactResult r = runHandContact(4.0f, 0.4f, ramp, 6);
  CHECK(r.leakedSamples <= 1);
  CHECK(r.worstLeakG < ACCIDENTAL_TOUCH_RESIDUAL_G);
  CHECK(!r.anchorMovedWhileHeld);
  // Lifting the chilling ball briefly: -2 g held, resumes on the trend.
  const float lift[] = {-2.0f, -2.0f, -2.0f, -2.0f, -2.0f};
  r = runHandContact(4.0f, 0.4f, lift, 5);
  CHECK(r.leakedSamples == 0);
  CHECK(r.breaks == 0);
}

void at25_real_spurt_is_accepted_as_flow() {
  // A +1.4 g spurt over 0.2 s stays under the gate: accepted sample by sample.
  const float spurt[] = {0.7f};
  const HandContactResult r = runHandContact(20.0f, 1.5f, spurt, 1, 1.4f);
  CHECK(r.breaks == 0);
  CHECK(session.accidentalTouchClass == AccidentalTouchClass::OK);
  CHECK(session.calibrationEligible);
}

void at26_first_drop_onset_is_flow_not_touch() {
  // Recorded Themis Mini shot: flat at -0.3 g, then coffee at up to 3 g/s.
  // The flat pre-flow trend predicts no flow; the onset is not a touch.
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  startCycle();
  advanceToBrew();
  const uint32_t baseMs = hostMillis;
  const int onset[][2] = {
      {4938, -3}, {5028, -2}, {5088, 1}, {5268, 2}, {5298, 5}, {5388, 8},
      {5508, 11}, {5598, 13}, {5688, 15}, {5808, 17}, {5898, 18}, {5988, 19},
      {6108, 22}, {6198, 23}, {6318, 23}, {6408, 24}, {6498, 25}, {6618, 25},
      {6708, 26}, {6828, 27}, {6948, 27}, {7008, 28}, {7098, 28}, {7218, 28},
      {7308, 28}, {7398, 28}, {7518, 28}, {7608, 31}, {7758, 30}, {7818, 30},
      {7908, 29}, {8028, 29}, {8118, 30}, {8238, 30}, {8328, 30}, {8418, 30}};
  for (int i = 1; i < 49; ++i) publishWeight(-0.3f, baseMs + 100U * i);
  for (const auto &sample : onset) {
    markScaleWorkerProgress();
    publishWeight(sample[1] / 10.0f, baseMs + sample[0]);
    loop();
    CHECK(!session.accidentalTouchHolding);
  }
  CHECK(session.firstDropMs != 0);
  CHECK(session.calibrationEligible);
  CHECK(shotCurveBitCount(shotCurveSampler.held, 0) == 0);
  publishWeight(11.0f, baseMs + 8538);  // A real step is still held.
  CHECK(session.accidentalTouchHolding);
}

void tr04_light_cup_pre_effect_packet_recovers() {
  // A post-boundary packet from a light cup (40 g) arrives before the zero:
  // control must settle on the real zero instead of locking VALIDATING.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline(40.0f);
  CHECK(!session.awaitingPostTareBaseline);
  uint32_t seq = session.lastAcceptedPacketSequence;
  for (int i = 0; i < 4; ++i) publishSequencedWeight(0.0f, seq);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(fabsf(session.lastAcceptedWeightG) < 0.5f);
}

void tr05_light_cup_pre_effect_packet_is_not_a_baseline() {
  // The start tare zeroed a 40 g cup: a post-boundary 40 g packet is the
  // pre-effect load, not the new zero; the baseline waits for the real 0 g.
  resetHarness(false, true);
  reachReadyFromBoot();
  idleCup(40.0f);
  startCycle();
  CHECK(executeNextScaleCommand());
  CHECK(fabsf(session.preTareWeightG - 40.0f) < 0.2f);
  establishPostTareBaseline(40.0f);
  CHECK(session.awaitingPostTareBaseline);
  establishPostTareBaseline(0.0f);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(session.weightControlState == WeightControlState::ACTIVE);
  CHECK(fabsf(session.lastAcceptedWeightG) < 0.01f);
  simulateFirstDrops(0.0f, session.lastAcceptedPacketSequence + 1U);
  CHECK(session.firstDropMs != 0);
}

// A 250 g cup sits untared on the scale when the shot starts and the start
// tare fails; grace then expires with the cup still on the scale.
void startShotWithFailedTare(float cupG) {
  resetHarness(false, true);
  reachReadyFromBoot();
  idleCup(cupG);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  startCycle();
  scale.tareSucceeds = false;
  CHECK(executeNextScaleCommand());
  CHECK(session.awaitingPostTareBaseline);
  const uint32_t deadline = session.postTareBaselineDeadlineMs;
  while (static_cast<int32_t>(hostMillis - deadline) < 0) {
    runLoopAfter(100);
    publishWeight(cupG);
  }
  runLoopAfter(1);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(fabsf(session.softwareZeroG - cupG) < 0.2f);
  CHECK(!cupPresence.holdTransitions);
  CHECK(!cupPresenceIsTared());
}

void tr01_failed_start_tare_uses_software_zero() {
  startShotWithFailedTare(250.0f);
  CHECK(session.firstDropMs == 0);
  for (float w : {250.35f, 250.40f, 250.45f}) idleWeight(w, 100);
  CHECK(session.firstDropMs != 0);
  endBbwProtectionForTests();
  CHECK(session.active);
  const float threshold = effectiveStopThreshold();
  for (float w = 1.0f; w < threshold - 0.5f; w += 0.5f) {
    idleWeight(250.0f + w, 100);
    loop();
    CHECK(session.active);
  }
  idleWeight(250.0f + threshold + 0.1f, 100);
  idleWeight(250.0f + threshold + 0.2f, 100);
  loop();
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
}

void tr02_late_zero_after_grace_is_ignored() {
  // Once the software zero is applied, a later zero is only a reading: no
  // second baseline is taken and the software zero stays in force.
  startShotWithFailedTare(250.0f);
  session.config.stopIfCupRemoved = false;
  publishWeight(0.0f, hostMillis + 100);
  publishWeight(0.0f, hostMillis + 100);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(fabsf(session.softwareZeroG - 250.0f) < 0.2f);
  CHECK(session.scaleBaselineG == 0.0f);
  CHECK(session.active);
}

void tr03_failed_start_tare_cup_lift_stops() {
  startShotWithFailedTare(250.0f);
  publishWeight(250.2f, hostMillis + 100);
  publishWeight(0.1f, hostMillis + 100);
  publishWeight(0.0f, hostMillis + 100);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

void sk01_refused_start_releases_cup_hold() {
  // A start refused by relay safety must not leave the cup FSM held or
  // believing that the never-sent start tare zeroed the cup.
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(0.0f);
  idleCup(250.0f);
  CHECK(executeNextScaleCommand());
  idleCup(0.0f);
  CHECK(idleTare.lastReason == IdleTareReason::EFFECT_CONFIRMED);
  runtimeConfig.autoTareOutsideBrew = false;
  hostEspTimerStartSucceeds = false;
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::RELAY_SAFETY_FAILURE);
  CHECK(!cupPresence.holdTransitions);
  hostEspTimerStartSucceeds = true;
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  runtimeConfig.autoTareOutsideBrew = true;
  idleCup(-250.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  idleCup(0.0f);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void sk02_overload_stops_while_touch_is_held() {
  resetHarness(false, true);
  reachReadyFromBoot();
  enableAccidentalTouchForTest();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  hostMillis += 100;
  publishWeight(40.0f, hostMillis, 1, 70);
  CHECK(session.accidentalTouchHolding);
  publishWeight(MAX_AUTOMATION_WEIGHT_G + 100.0f, hostMillis + 100, 1, 71);
  publishWeight(MAX_AUTOMATION_WEIGHT_G + 100.0f, hostMillis + 100, 1, 72);
  CHECK(session.accidentalTouchHolding);
  loop();
  CHECK(session.endReason == EndReason::WEIGHT_ANOMALY);
  CHECK(!getRelaySafetySnapshot().closed);
}

void sk03_late_zero_after_grace_releases_cup_hold() {
  // The late-retare zero arrives after the grace: expiry on the sample path
  // must release the cup hold so a later lift still stops the shot.
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.retareWindowMs = 4000;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  hostMillis = session.startedAtMs + 1500U;
  publishStableCupWeight(80.0f, 30);
  CHECK(session.retarePerformed);
  CHECK(executeNextScaleCommand());
  CHECK(cupPresence.holdTransitions);
  hostMillis = session.postTareBaselineDeadlineMs + 10U;
  markScaleWorkerProgress();
  publishWeight(0.0f, hostMillis, 1, 50);
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(!cupPresence.holdTransitions);
  publishWeight(-80.0f, hostMillis + 100, 1, 51);
  publishWeight(-80.0f, hostMillis + 100, 1, 52);
  runLoopAfter(1);
  CHECK(session.endReason == EndReason::CUP_REMOVED);
}

// ---- Shot predictor (SAP) host tests ---------------------------------------
// Structural invariants of the live hero-tint predictor. Full corpus parity
// (owner CSV replay, pills) lives in temp/ai_temp_hero_forecast_backtest
// (BLEND_FREEZE.md); these assert the same behavior on synthetic curves.

corridor::CorridorState spArmed(int32_t goalDg = 360, int32_t t1 = 27000,
                                int32_t t2 = 36000) {
  corridor::CorridorState cut{};
  cut.params.goalDg = goalDg;
  cut.params.fastMarginDg = 60;
  cut.params.slowMarginDg = 30;
  cut.params.fastOn = true;
  cut.params.slowOn = true;
  cut.params.delayMs = 12000;
  cut.params.rampMs = 5000;
  cut.params.fastBeforeMs = t1;
  cut.params.slowAfterMs = t2;
  cut.params.wallMs = 50000;
  cut.tauMs = 650;
  cut.offsetDg = 15;  // 1.46 g drip offset in dg
  return cut;
}

void sp01_phi_table_matches_erf() {
  double maxErr = 0;
  for (int zm = -3999; zm <= 3999; zm += 7) {
    const double z = zm / 1000.0;
    const double exact = 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
    const double got = predictor::phiPermille(zm) / 1000.0;
    const double err = got > exact ? got - exact : exact - got;
    if (err > maxErr) maxErr = err;
  }
  CHECK(maxErr <= 0.002);
  CHECK(predictor::phiPermille(-4000) == 0);
  CHECK(predictor::phiPermille(4000) == 1000);
}

void sp02_isqrt_exact_on_squares() {
  for (int64_t v : {0LL, 1LL, 3LL, 4LL, 8LL, 9LL, 1000000LL, 999999999LL,
                    4000000000000LL}) {
    const int32_t r = predictor::isqrt64(v);
    CHECK(static_cast<int64_t>(r) * r <= v);
    CHECK(static_cast<int64_t>(r + 1) * (r + 1) > v);
  }
}

void sp03_plateau_flow_median() {
  const auto rowAt = [](size_t) -> corridor::ArrivalRecord { return {}; };
  corridor::StatsScope scope;
  scope.presetId = 1;
  scope.fastBeforeMs = 27000;
  scope.slowAfterMs = 36000;
  scope.goalDg = 360;
  corridor::ArrivalRecord rows[3];
  for (int i = 0; i < 3; ++i) {
    rows[i].recordId = 100 + i;
    rows[i].presetId = 1;
    rows[i].endReason = static_cast<uint8_t>(EndReason::SCALE_THRESHOLD);
    rows[i].verdict = 0;
    rows[i].arrivalMs = 31000 + i * 100;
    rows[i].firstDropMs = 5200;
    rows[i].fastBeforeMs = 27000;
    rows[i].slowAfterMs = 36000;
    rows[i].goalDg = 360;
    rows[i].durationMs = 31500 + i * 100;
    rows[i].finalWeightDg = 355;
    rows[i].avgFlowCgS = 130 + static_cast<uint16_t>(i) * 10;
  }
  const auto at = [&](size_t i) { return rows[i]; };
  // avgFlow 130/140/150 cg/s -> median 140 -> x10 x4/3 = 1866 rateX100.
  CHECK(corridor::plateauFlowX100(3, at, scope) == 1866);
  CHECK(corridor::plateauFlowX100(0, rowAt, scope) == 0);
  rows[1].avgFlowCgS = 0;  // missing flow row drops out of the median
  CHECK(corridor::plateauFlowX100(3, at, scope) == 1866);
}

void sp04_forecast_constant_rate_matches_analytic() {
  corridor::CorridorState cut = spArmed();
  const int32_t qX100 = 1900;  // 1.9 g/s
  for (int t = 6000; t <= 10000; t += 100)
    CHECK(cut.rate.push(t, 500 + (t - 6000) / 10 * 19 / 10) ==
          corridor::RateEstimator::Result::ACCEPTED);
  CHECK(cut.rate.valid);
  const int32_t w = 1256;  // dg at t = 10000
  const predictor::Forecast f = predictor::forecastBasis(cut, 10000, w, 1, 0, 0);
  CHECK(!f.censored && f.tMs > 0);
  // Analytic: (goal - offset - W - lead)/q + now, lead = q*tau/1e5 dg.
  const int32_t lead = static_cast<int32_t>(static_cast<int64_t>(qX100) * 650 / 100000);
  const double qcDgps = qX100 / 100.0;
  const double expectS = 10000 + (360.0 - 15.0 - w - lead) / qcDgps;
  CHECK(f.tMs > expectS - 60 && f.tMs < expectS + 60);
  const predictor::Forecast m = predictor::forecastBasis(cut, 10000, w, 0, 0, 0);
  CHECK(m.tMs == f.tMs);  // a = 0: mean basis equals raw
  CHECK(m.sigmaMs > 0 && m.sigmaMs < 20000);
}

void sp05_quadratic_matches_reference() {
  corridor::CorridorState cut = spArmed();
  // Ramp at a = +0.1 g/s^2 from 1.0 g/s; window fed over the last 3 s.
  double q = 1000, w = 0;  // w in dg
  for (int t = 6000; t <= 12000; t += 100) {
    w += q / 1000.0;  // rateX100 -> dg per 100 ms
    q += 10;          // +100 rateX100 per second
    if (t >= 9000)
      CHECK(cut.rate.push(t, static_cast<int32_t>(w)) ==
            corridor::RateEstimator::Result::ACCEPTED);
  }
  CHECK(cut.rate.valid);
  const int32_t now = 12000, wDg = static_cast<int32_t>(w);
  // Plain crossing must land inside the goal plateau for the closed form.
  const double qhat = cut.rate.rateX100 / 100.0;  // dg/s (window midpoint)
  const double qc = qhat + 1.5, aD = 1.0;         // + a * T_w/2, dg/s^2
  const double lead = qc * 0.65;
  const double D = 360.0 - 15.0 - wDg - lead;
  const double h = (-qc + std::sqrt(qc * qc + 2 * aD * D)) / aD;
  const double plain = now + (360.0 - 15.0 - wDg - qhat * 0.65) / qhat * 1000;
  CHECK(plain >= 27000 && plain <= 36000);
  const predictor::Forecast f =
      predictor::forecastBasis(cut, now, wDg, 0, 100, 100);
  CHECK(!f.censored);
  CHECK(f.tMs > now + h * 1000 - 150 && f.tMs < now + h * 1000 + 150);
}

void sp06_sigma_finite_at_extremes() {
  corridor::CorridorState cut = spArmed();
  cut.params.wallMs = 50000;
  for (int t = 6000; t <= 10000; t += 100)
    CHECK(cut.rate.push(t, 20 + (t - 6000) * 6 / 1000) ==  // 0.6 g/s = 6 dg/s
          corridor::RateEstimator::Result::ACCEPTED);
  // 6 g in the cup at 0.6 g/s: the goal is unreachable before the wall.
  const predictor::Forecast f =
      predictor::forecastBasis(cut, 10000, 60, 1, 0, 0);
  CHECK(f.censored);
  CHECK(f.sigmaMs == (50000 - 10000) / 4);
  const predictor::Forecast m =
      predictor::forecastBasis(cut, 10000, 60, 0, 150, 150);
  CHECK(m.censored || (m.sigmaMs > 0 && m.sigmaMs <= 20000));
}

// Synthetic green shape: first drop 5.2 s, ramp to ~1.9 g/s by 18 s.
int spGreenW(int tMs) {
  const double fd = 5200;
  if (tMs < static_cast<int>(fd)) return 0;
  static double w = 0;
  static int last = static_cast<int>(fd);
  if (tMs < last) {  // reset for a fresh replay
    w = 0;
    last = static_cast<int>(fd);
  }
  while (last < tMs) {
    last += 10;
    const double ss = (last - fd) / 1000.0;
    const double qq = 0.5 + 1.4 * (ss < 13.0 ? ss / 13.0 : 1.0);
    w += qq / 100.0;
  }
  return static_cast<int>(w * 10);
}

void spReplay(corridor::CorridorState &cut, predictor::ShotPredictor &pr,
              int (*wAt)(int), predictor::PredictorInputs &in, int tEnd) {
  for (int t = 5000; t <= tEnd; t += 100) {
    const int32_t w = wAt(t);
    if (cut.rate.push(t, w) != corridor::RateEstimator::Result::ACCEPTED)
      continue;
    in.nowMs = t;
    in.weightDg = w;
    in.firstDropMs = 5200;
    predictor::updateShotPredictor(pr, in);
  }
}

void sp07_green_replay_reaches_full_on_recipe() {
  corridor::CorridorState cut = spArmed();
  cut.history.n = 3;
  cut.history.medianMs = 31800;
  cut.history.sigmaMs = 800;
  predictor::ShotPredictor pr;
  predictor::armShotPredictor(pr);
  predictor::PredictorInputs in;
  in.cut = &cut;
  in.qPX100 = 1800;
  in.autoMode = true;
  in.scaleOk = true;
  in.sampleDtMs = 100;
  spReplay(cut, pr, spGreenW, in, 31000);
  // Arrival of the synthetic curve is just past 31 s: full green by ~T-3.5.
  CHECK(pr.tintClass == ShotTint::ON_RECIPE);
  CHECK(pr.tintPct >= 95);
}

void sp08_stall_without_history_stays_neutral() {
  // F10: the censored (stalled) path requires the preset's plateau-flow
  // gate; with no history the card stays neutral.
  corridor::CorridorState cut = spArmed();
  predictor::ShotPredictor pr;
  predictor::armShotPredictor(pr);
  predictor::PredictorInputs in;
  in.cut = &cut;
  in.qPX100 = 0;  // first shot of the preset: no history
  in.autoMode = true;
  in.scaleOk = true;
  in.sampleDtMs = 100;
  spReplay(cut, pr, spGreenW, in, 12000);  // normal start...
  const int stallW0 = spGreenW(12000);
  for (int t = 12100; t <= 30000; t += 100) {  // ...then a hard stall
    const int32_t w = stallW0 + (t - 12000) * 42 / 100000;
    CHECK(cut.rate.push(t, w) == corridor::RateEstimator::Result::ACCEPTED);
    in.nowMs = t;
    in.weightDg = w;
    in.firstDropMs = 5200;
    predictor::updateShotPredictor(pr, in);
  }
  CHECK(pr.tintClass == ShotTint::NEUTRAL);
  CHECK(pr.tintPct == 0);
  // With history the gate opens and the tint goes blue.
  corridor::CorridorState cut2 = spArmed();
  cut2.history.n = 3;
  cut2.history.medianMs = 31800;
  cut2.history.sigmaMs = 800;
  predictor::ShotPredictor pr2;
  predictor::armShotPredictor(pr2);
  predictor::PredictorInputs in2 = in;
  in2.cut = &cut2;
  in2.qPX100 = 1800;
  spReplay(cut2, pr2, spGreenW, in2, 12000);
  for (int t = 12100; t <= 30000; t += 100) {
    const int32_t w = stallW0 + (t - 12000) * 42 / 100000;
    CHECK(cut2.rate.push(t, w) == corridor::RateEstimator::Result::ACCEPTED);
    in2.nowMs = t;
    in2.weightDg = w;
    in2.firstDropMs = 5200;
    predictor::updateShotPredictor(pr2, in2);
  }
  CHECK(pr2.tintClass == ShotTint::SLOW);
  CHECK(pr2.tintPct > 30);
}

void sp09_transient_spike_does_not_flip_fast() {
  // A one-sample +8 g scale spike (disturbance, not a sustained surge):
  // the monotonic guard plus MAD gate must keep any fast tint away.
  corridor::CorridorState cut = spArmed();
  cut.history.n = 3;
  cut.history.medianMs = 31800;
  cut.history.sigmaMs = 800;
  predictor::ShotPredictor pr;
  predictor::armShotPredictor(pr);
  predictor::PredictorInputs in;
  in.cut = &cut;
  in.qPX100 = 1800;
  in.autoMode = true;
  in.scaleOk = true;
  in.sampleDtMs = 100;
  spReplay(cut, pr, spGreenW, in, 20000);
  const ShotTint before = pr.tintClass;
  for (int t = 20100; t <= 22000; t += 100) {
    const int32_t w = spGreenW(t) + (t == 20200 ? 80 : 0);  // one 8 g spike
    cut.rate.push(t, w);
    in.nowMs = t;
    in.weightDg = w;
    in.firstDropMs = 5200;
    predictor::updateShotPredictor(pr, in);
    CHECK(pr.tintClass != ShotTint::FAST);
  }
  CHECK(before == ShotTint::ON_RECIPE || before == ShotTint::NEUTRAL);
}

void sp10_blend_monotone_within_class() {
  corridor::CorridorState cut = spArmed();
  cut.history.n = 3;
  cut.history.medianMs = 31800;
  cut.history.sigmaMs = 800;
  predictor::ShotPredictor pr;
  predictor::armShotPredictor(pr);
  predictor::PredictorInputs in;
  in.cut = &cut;
  in.qPX100 = 1800;
  in.autoMode = true;
  in.scaleOk = true;
  in.sampleDtMs = 100;
  int lastPct = -1;
  ShotTint cls = ShotTint::NEUTRAL;
  bool full = false;
  for (int t = 5000; t <= 30000; t += 100) {
    const int32_t w = spGreenW(t);
    if (cut.rate.push(t, w) != corridor::RateEstimator::Result::ACCEPTED)
      continue;
    in.nowMs = t;
    in.weightDg = w;
    in.firstDropMs = 5200;
    predictor::updateShotPredictor(pr, in);
    if (pr.tintClass != ShotTint::NEUTRAL) {
      if (pr.tintClass == cls)
        CHECK(pr.tintPct + 1 >= lastPct);  // non-decreasing within the run
      cls = pr.tintClass;
      lastPct = pr.tintPct;
      if (pr.tintPct >= 100) full = true;
    }
  }
  CHECK(full);
}

void sp11_disturbance_guard_fades_to_neutral() {
  corridor::CorridorState cut = spArmed();
  predictor::ShotPredictor pr;
  predictor::armShotPredictor(pr);
  predictor::PredictorInputs in;
  in.cut = &cut;
  in.qPX100 = 1800;
  in.autoMode = true;
  in.scaleOk = true;
  in.sampleDtMs = 100;
  spReplay(cut, pr, spGreenW, in, 24000);
  const int w = spGreenW(24000);
  for (int t = 24100; t <= 26500; t += 100) {  // scale drops 2 g mid-shot
    const int32_t bad = w + (t - 24000) * 19 / 1000 - 200;
    (void)cut.rate.push(t, bad);  // MAD may reject; either way the guard sees it
    in.nowMs = t;
    in.weightDg = bad;
    in.firstDropMs = 5200;
    predictor::updateShotPredictor(pr, in);
  }
  // During/after the drop the tint must not show a committed full color.
  CHECK(pr.tintPct < 100);
}

void fw01_median_final_weight_filters_dip() {
  // Post-stop drip tail with one -1.9 g dip constituent: the median of the
  // last five plausible samples swallows it, and learning consumes the
  // median (not the last sample).
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  session.startedWithScale = true;
  session.scaleBaselineReady = true;
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 30.0f, 0.5f, 100, 10);  // Stay below the cut.
  schedulePendingShotFinalize(EndReason::SCALE_THRESHOLD, 30000);
  session.active = false;
  pendingFinalize.dripDelayMs = 2500;
  pendingFinalize.lastKnownWeightG = 30.0f;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.endedAtMs = hostMillis;
  uint32_t sequence = pendingFinalize.curveLastSequence + 1U;
  const float tail[] = {36.0f, 36.1f, 34.3f, 36.15f, 36.2f, 36.25f};
  uint32_t atMs = pendingFinalize.endedAtMs + 100;
  for (float sample : tail) {
    hostMillis = atMs;
    markScaleWorkerProgress();
    publishWeight(sample, atMs, pendingFinalize.scaleConnectionGeneration,
                  sequence++);
    atMs += 300;
  }
  CHECK(pendingFinalize.tailCount == POST_STOP_TAIL_SAMPLE_COUNT);
  hostMillis = pendingFinalize.endedAtMs + pendingFinalize.dripDelayMs + 1;
  const float seedOffset = activeShotPreset(presetBank).bbwEwmaOffsetG;
  pendingShotFinalizeTask();
  ShotLogRecord record[1] = {};
  CHECK(shotLog.copyNewestFirst(record, 1) == 1);

  // Median of the last five (36.1, 34.3, 36.15, 36.2, 36.25) is 36.15.
  CHECK(fabsf(record[0].actualWeightCg / 100.0f - 36.15f) < 0.02f);
  const ShotPreset &preset = activeShotPreset(presetBank);
  CHECK(fabsf(preset.bbwEwmaOffsetG -
              (seedOffset + (preset.bbwEwmaAlpha / 100.0f) * (36.15f - 36.0f))) <
        0.01f);

  // A wide-span tail where dipped samples form the majority: the median
  // guard falls back to the running max instead of letting the dip win.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  session.startedWithScale = true;
  session.scaleBaselineReady = true;
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 30.0f, 0.5f, 100, 10);
  schedulePendingShotFinalize(EndReason::SCALE_THRESHOLD, 30000);
  session.active = false;
  pendingFinalize.dripDelayMs = 2500;
  pendingFinalize.lastKnownWeightG = 34.0f;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.endedAtMs = hostMillis;
  sequence = pendingFinalize.curveLastSequence + 1U;
  {
    const float wideTail[] = {34.05f, 34.0f, 37.0f, 34.05f, 34.0f, 34.02f};
    uint32_t wideAtMs = pendingFinalize.endedAtMs + 100;
    for (float sample : wideTail) {
      hostMillis = wideAtMs;
      markScaleWorkerProgress();
      publishWeight(sample, wideAtMs,
                    pendingFinalize.scaleConnectionGeneration, sequence++);
      wideAtMs += 300;
    }
  }
  CHECK(pendingFinalize.tailCount == POST_STOP_TAIL_SAMPLE_COUNT);
  hostMillis = pendingFinalize.endedAtMs + pendingFinalize.dripDelayMs + 1;
  pendingShotFinalizeTask();
  CHECK(shotLog.copyNewestFirst(record, 1) == 1);
  CHECK(fabsf(record[0].actualWeightCg / 100.0f - 37.0f) < 0.02f);

  // Sparse tail (two samples spanning 0.2 s): running-max fallback.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  session.startedWithScale = true;
  session.scaleBaselineReady = true;
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 30.0f, 0.5f, 100, 10);
  schedulePendingShotFinalize(EndReason::SCALE_THRESHOLD, 30000);
  session.active = false;
  pendingFinalize.dripDelayMs = 800;
  pendingFinalize.lastKnownWeightG = 30.0f;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.endedAtMs = hostMillis;
  sequence = pendingFinalize.curveLastSequence + 1U;
  hostMillis = pendingFinalize.endedAtMs + 100;
  markScaleWorkerProgress();
  publishWeight(36.05f, hostMillis,
                pendingFinalize.scaleConnectionGeneration, sequence++);
  hostMillis = pendingFinalize.endedAtMs + 400;
  markScaleWorkerProgress();
  publishWeight(36.1f, hostMillis,
                pendingFinalize.scaleConnectionGeneration, sequence++);
  CHECK(pendingFinalize.tailCount == 2);
  hostMillis = pendingFinalize.endedAtMs + pendingFinalize.dripDelayMs + 1;
  pendingShotFinalizeTask();
  CHECK(shotLog.copyNewestFirst(record, 1) == 1);
  CHECK(fabsf(record[0].actualWeightCg / 100.0f - 36.1f) < 0.02f);  // Max.
}

void fw02_offset_learns_below_sensor_lag() {
  // The cut lead q x tau already carries the sensor lag, so learning is not
  // floored by it: a 690 ms lag must not pin the offset at 0.55 g.
  resetHarness(false, true);
  reachReadyFromBoot();
  scale.connectedFeatures.sensorLagMs = 690;
  setScaleConnected(false);
  setScaleConnected(true);
  CHECK(getScaleLinkSnapshot().features.sensorLagMs == 690);
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  for (const float finalG : {36.05f, 35.0f}) {  // 0.215 g, then clamped to 0.
    preset.bbwEwmaOffsetG = 0.20f;
    preparePendingBbwForTest();
    pendingFinalize.offsetAnalysis = true;
    pendingFinalize.startedWithScale = pendingFinalize.automaticBrew = true;
    pendingFinalize.scaleBaselineReady = true;
    pendingFinalize.scaleConnectionGeneration =
        getScaleLinkSnapshot().connectionGeneration;
    pendingFinalize.weightOffsetG = 0.20f;
    pendingFinalize.goalWeightDg = 360;
    pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
    pendingFinalize.endedAtMs = hostMillis;
    pendingFinalize.durationDs = 300;
    pendingFinalize.firstDropDs = 100;
    currentWeight = finalG;
    currentWeightConnectionGeneration = pendingFinalize.scaleConnectionGeneration;
    ++currentWeightSequence;
    currentWeightReceivedAtMs = ++hostMillis;
    CHECK(learnPendingBbw(pendingFinalize, currentWeight, true));
    CHECK(fabsf(preset.bbwEwmaOffsetG -
                fmaxf(0.0f, 0.20f + (preset.bbwEwmaAlpha / 100.0f) *
                                        (finalG - 36.0f))) < 0.005f);
  }
  // A cut without a valid rate had no q x tau lead: its error is not drip.
  preset.bbwEwmaOffsetG = 0.20f;
  preparePendingBbwForTest();
  pendingFinalize.weightOffsetG = 0.20f;
  pendingFinalize.rateAtStopX100 = 0;
  currentWeight = 37.0f;
  ++currentWeightSequence;
  currentWeightReceivedAtMs = ++hostMillis;
  CHECK(!learnPendingBbw(pendingFinalize, currentWeight, true));
  CHECK(preset.bbwEwmaOffsetG == 0.20f);
}

void fw05_set_drip_quick_starts_and_three_shots_decide() {
  resetHarness(false, true);
  reachReadyFromBoot();
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  const auto state = [&] {
    return quickStartEntry(bbwQuickStart, presetBank, preset.id, false)->state;
  };
  WebCommand set;
  set.type = WebCommandType::RESET_WEIGHT_OFFSET;
  set.bbwSetDrip = true;
  set.config.weightOffsetBaselineG = 1.2f;
  processWebCommand(set);
  CHECK(preset.bbwEwmaOffsetG == 1.2f && preset.weightOffsetBaselineG == 1.2f);
  CHECK(state() == static_cast<uint8_t>(QuickStartState::ARMED));
  CHECK(bbwQuickStartPersistPending);
  // Set learning speed alone keeps the drip and its quick start.
  WebCommand speed;
  speed.type = WebCommandType::RESET_WEIGHT_OFFSET;
  speed.bbwAlphaBaseline = 50;
  processWebCommand(speed);
  CHECK(preset.bbwEwmaAlpha == 50 && preset.bbwAlphaBaseline == 50 &&
        preset.bbwAlphaLearned == 0 && preset.bbwEwmaOffsetG == 1.2f);
  CHECK(state() == static_cast<uint8_t>(QuickStartState::ARMED));
  // Three shots that needed 0.48, 0.52 and 0.50 g: the third sets 0.50 g.
  for (const float needed : {0.48f, 0.52f, 0.50f}) {
    preparePendingBbwForTest();
    pendingFinalize.offsetAnalysis = pendingFinalize.logEligible = true;
    pendingFinalize.startedWithScale = pendingFinalize.automaticBrew = true;
    pendingFinalize.scaleBaselineReady = true;
    pendingFinalize.scaleConnectionGeneration =
        getScaleLinkSnapshot().connectionGeneration;
    pendingFinalize.weightOffsetG = preset.bbwEwmaOffsetG;
    pendingFinalize.goalWeightDg = 360;
    pendingFinalize.endReason = EndReason::SCALE_THRESHOLD;
    pendingFinalize.endedAtMs = hostMillis;
    pendingFinalize.durationDs = 300;
    pendingFinalize.firstDropDs = 100;
    currentWeight = 36.0f - preset.bbwEwmaOffsetG + needed;
    currentWeightConnectionGeneration = pendingFinalize.scaleConnectionGeneration;
    ++currentWeightSequence;
    currentWeightReceivedAtMs = ++hostMillis;
    CHECK(learnPendingBbw(pendingFinalize, currentWeight, true));
  }
  CHECK(fabsf(preset.bbwEwmaOffsetG - 0.50f) < 0.005f);
  CHECK(state() == static_cast<uint8_t>(QuickStartState::APPLIED));
  // Restore quick starts again and marks where the factory drip came back,
  // so the chart fades the shots pulled before it.
  WebCommand restore;
  restore.type = WebCommandType::PRESET_OP;
  restore.presetAction = static_cast<uint8_t>(PresetAction::RESTORE_FACTORY_VALUES);
  restore.presetId = preset.id;
  processWebCommand(restore);
  const QuickStartEntry *restored = quickStartEntry(bbwQuickStart, presetBank, preset.id, false);
  CHECK(restored->state == static_cast<uint8_t>(QuickStartState::ARMED));
  CHECK(restored->setAtId != 0 && restored->setAtId == shotStatisticsEpochNow());
}

void fw06_status_carries_recent_drips_and_markers() {
  // Six logged shots of the active preset (the fifth did not train the drip)
  // and a drip set before the fourth: all six (the window holds ten), newest
  // first, with three of them after the set.
  resetHarness(false, true);
  reachReadyFromBoot();
  const ShotPreset &preset = activeShotPreset(presetBank);
  uint32_t setAtId = 0;
  for (int16_t i = 0; i < 6; ++i) {
    ShotLogRecord record = {};
    record.durationDs = 300;
    record.goalWeightDg = 360;
    record.cutLineCg = 3600;
    record.offsetUsedCg = 100;
    record.actualWeightCg = static_cast<int16_t>(3550 + i);  // needed 0.50 + i cg
    shotLogSetPresetId(record, preset.id);
    shotLogSetBbw(record, 1, BBW_PROFILE_VERSION, 30, i != 4);
    TaskLockGuard lock(shotStoreMutex);
    if (i == 3) setAtId = shotLog.nextRecordId();
    CHECK(shotLog.append(record, false));
  }
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  armQuickStart(bbwQuickStart, presetBank, preset.id, 1.2f, setAtId);
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.bbwRecentCount == 6 && status.bbwRecentSkipped == 2);
  CHECK(status.bbwRecentCg[0] == 55 && status.bbwRecentCg[2] == 53 &&
        status.bbwRecentCg[4] == 51);
  CHECK(status.bbwRecentAfterSet == 3 && status.bbwRecentAfterQuick == 0xFF);
  CHECK(status.bbwQuickState == static_cast<uint8_t>(QuickStartState::ARMED) &&
        status.bbwQuickSetCg == 120);
  char json[256];
  CHECK(formatDripChartJson(json, sizeof(json), status) > 0);
  CHECK(strcmp(json, ",\"bbwQuickStart\":{\"state\":\"armed\",\"count\":0,\"setG\":1.20,"
                     "\"quickG\":0.00},\"bbwRecentAfterSet\":3,\"bbwRecentAfterQuick\":-1,"
                     "\"bbwRecentG\":[0.55,null,0.53,0.52,0.51,0.50]") == 0);
  // Another preset's shots never reach this preset's chart.
  CHECK(setActiveShotPreset(presetBank, presetBank.presets[1].id));
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.bbwRecentCount == 0 && status.bbwQuickState == 0);
}

void fw07_quick_start_save_retries_and_reports_once() {
  resetHarness(false, true);
  reachReadyFromBoot();
  armQuickStart(bbwQuickStart, presetBank, presetBank.activeId, 0.5f, 7);
  stageBbwQuickStart();
  hostQuickStartSaveSucceeds = false;
  debugLog.clear();
  serviceBbwQuickStartPersist();
  CHECK(bbwQuickStartPersistPending);  // kept for the next idle pass
  CHECK(debugEventExists(DebugCode::RUNTIME_PERSIST_FAILED, 0,
                         RUNTIME_PERSIST_REASON_BBW_QUICK_START));
  debugLog.clear();
  serviceBbwQuickStartPersist();
  CHECK(bbwQuickStartPersistPending);
  CHECK(!debugEventExists(DebugCode::RUNTIME_PERSIST_FAILED));  // once per run
  hostQuickStartSaveSucceeds = true;
  serviceBbwQuickStartPersist();
  CHECK(!bbwQuickStartPersistPending);
  CHECK(memcmp(hostLastSavedQuickStart.entries, bbwQuickStart.entries,
               sizeof(bbwQuickStart.entries)) == 0);
  serviceBbwQuickStartPersist();  // nothing staged: nothing to save
  CHECK(!bbwQuickStartPersistPending);
}

void rt24_late_cup_step_uses_shot_coordinates() {
  // After a failed start tare the trajectory is software-zeroed: the same
  // 300 g cup set back down over 5 g of coffee is not a late-cup step.
  resetHarness(false, true);
  reachReadyFromBoot();
  shot.startMs = hostMillis;
  shot.datapoints = 0;
  for (int i = 0; i < 10; ++i) {
    shot.timeS[shot.datapoints] = 1.0f + 0.1f * i;
    shot.weight[shot.datapoints++] = 4.0f + 0.1f * i;
  }
  const uint32_t placementStartedAtMs = shot.startMs + 2100;
  session.softwareZeroG = 300.0f;
  CHECK(!cupPlacementIsStep(305.2f, placementStartedAtMs));
  session.softwareZeroG = 0.0f;
  CHECK(cupPlacementIsStep(305.2f, placementStartedAtMs));  // A real new cup.
}

void ff19_foreign_generation_cannot_latch_first_drop() {
  // First-flow detection is fenced behind the connection generation that
  // owns the cycle: a reconnecting scale's samples must never latch a first
  // drop, while the owned connection still fires on the same shape.
  startFirstFlowBrew();
  CHECK(session.ownedConnectionGeneration != 0);
  const uint32_t foreign = session.ownedConnectionGeneration + 1U;
  publishWeight(0.40f, hostMillis + 100, foreign, 900);
  publishWeight(0.50f, hostMillis + 100, foreign, 901);
  CHECK(session.firstDropMs == 0);
  CHECK(session.firstFlow.phase == FirstFlowPhase::SEEKING);
  CHECK(session.firstFlow.confirmations == 0);  // FSM never stepped.
  publishWeight(0.40f, hostMillis + 100);
  publishWeight(0.50f, hostMillis + 100);
  CHECK(session.firstDropMs != 0);
}

void ff20_seeking_fa_telemetry_args_and_rate_limit() {
  // A streak that dies sub-threshold emits FIRST_DROP_SEEKING_FA carrying
  // the effective threshold (cg) and sigma-hat (cg), and never more than
  // one event per second — a noisy counter must not evict the real drop.
  startFirstFlowBrew();
  CHECK(scaleProfiler().hostStartNoFlash(hostMillis));
  for (int i = 0; i < 8; ++i) {  // Warm sigma-hat with +/-0.2 g idle noise.
    publishWeight(i % 2 ? -0.2f : 0.2f, hostMillis + 100);
  }
  CHECK(session.firstFlow.noiseSamples == FIRST_DROP_NOISE_WARMUP_SAMPLES);
  publishWeight(0.7f, hostMillis + 100);  // Streak starts above 3*sigma-hat.
  publishWeight(0.0f, hostMillis + 100);  // Death -> FA #1.
  publishWeight(0.7f, hostMillis + 100);  // Second streak dies inside 1 s...
  publishWeight(0.0f, hostMillis + 100);  // ...rate-limited away.
  publishWeight(0.7f, hostMillis + 1000);
  publishWeight(0.0f, hostMillis + 100);  // Death >= 1 s later -> FA #2.
  CHECK(session.firstDropMs == 0);        // Nothing here is a real drop.
  uint32_t faEvents = 0;
  uint32_t arg1 = 0, arg2 = 0;
  for (uint32_t i = 0; i < scaleProfiler().hostRecordCount(); ++i) {
    const auto row = scaleProfiler().hostRecord(i);
    if (row.kind == static_cast<uint16_t>(ScaleProfileEvent::FIRST_DROP_SEEKING_FA)) {
      ++faEvents;
      arg1 = row.arg1;
      arg2 = row.arg2;
    }
  }
  CHECK(faEvents == 2);
  // Warmup window is the idle zero plus seven +/-0.2 g samples
  // (sigma-hat ~0.187 g), then one EWMA step (~0.188 g): the floor engages
  // at ~3 x 0.188 g = 56 cg with sigma-hat 18 cg — never the fixed 30 cg.
  CHECK(arg1 >= 55 && arg1 <= 57);
  CHECK(arg2 >= 17 && arg2 <= 19);
  scaleProfiler().hostStop(ScaleProfilerStopReason::USER, hostMillis);
}

void fw03_dense_tail_decimates_to_reachable_median() {
  // Dense ~10 Hz streams are decimated to one accepted sample per
  // POST_STOP_TAIL_SAMPLE_SPACING_MS (250 ms), so a tail of at least ~1.2 s
  // fills the ring and the median engages at the reference cadence; a short
  // dense burst that cannot reach the coverage window still falls back to
  // the running max. Pin both sides so neither constant moves silently.
  {
    // Short 0.6 s burst at 10 Hz: only three decimated samples fit.
    PendingShotFinalize burst;
    const float shortTail[] = {36.0f, 36.4f, 34.6f, 34.7f, 34.8f,
                               34.9f, 35.0f};
    uint32_t atMs = 10000;
    for (float sample : shortTail) {
      notePostStopTailSample(burst, sample, atMs);
      atMs += 100;
    }
    CHECK(burst.tailCount == 3);
    CHECK(fabsf(postStopFinalWeightG(burst, 0.0f) - 36.4f) < 0.01f);
    // 1.5 s at 10 Hz: accepted every 300 ms -> five samples spanning 1.2 s,
    // and a single splash sample cannot own the result.
    PendingShotFinalize dense;
    const float longTail[] = {35.0f, 36.4f, 35.0f, 34.8f, 35.0f, 35.0f,
                              34.9f, 35.0f, 35.0f, 35.1f, 35.0f, 35.0f,
                              35.2f, 35.2f, 35.2f, 35.2f};
    atMs = 10000;
    for (float sample : longTail) {
      notePostStopTailSample(dense, sample, atMs);
      atMs += 100;
    }
    CHECK(dense.tailCount == POST_STOP_TAIL_SAMPLE_COUNT);
    // Accepted every 300 ms and the ring keeps sliding: 34.8, 34.9, 35.1,
    // 35.2, 35.2 -> median 35.1, not the 36.4 splash the running max
    // would report.
    CHECK(fabsf(postStopFinalWeightG(dense, 0.0f) - 35.1f) < 0.01f);
  }
  // Integration: the same 1.5 s dense tail through the real drip hold.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  session.startedWithScale = true;
  session.scaleBaselineReady = true;
  endBbwProtectionForTests();
  publishControlRamp(1.0f, 30.0f, 0.5f, 100, 10);
  schedulePendingShotFinalize(EndReason::SCALE_THRESHOLD, 30000);
  session.active = false;
  pendingFinalize.dripDelayMs = 1800;
  pendingFinalize.lastKnownWeightG = 34.5f;
  pendingFinalize.lastKnownWeightValid = true;
  pendingFinalize.endedAtMs = hostMillis;
  uint32_t sequence = pendingFinalize.curveLastSequence + 1U;
  const float tail[] = {35.0f, 36.4f, 35.0f, 34.8f, 35.0f, 35.0f,
                        34.9f, 35.0f, 35.0f, 35.1f, 35.0f, 35.0f,
                        35.2f, 35.2f, 35.2f, 35.2f};
  uint32_t atMs = pendingFinalize.endedAtMs + 100;
  for (float sample : tail) {
    hostMillis = atMs;
    markScaleWorkerProgress();
    publishWeight(sample, atMs, pendingFinalize.scaleConnectionGeneration,
                  sequence++);
    atMs += 100;  // 10 Hz.
  }
  CHECK(pendingFinalize.tailCount == POST_STOP_TAIL_SAMPLE_COUNT);
  hostMillis = pendingFinalize.endedAtMs + pendingFinalize.dripDelayMs + 1;
  pendingShotFinalizeTask();
  ShotLogRecord record[1] = {};
  CHECK(shotLog.copyNewestFirst(record, 1) == 1);
  CHECK(fabsf(record[0].actualWeightCg / 100.0f - 35.1f) < 0.02f);
}

void ff02_chorrito_fires_on_second_sample() {
  startFirstFlowBrew();
  const uint32_t firstAtMs = hostMillis + 50;
  publishWeight(0.4f, firstAtMs, 1, 20);
  CHECK(session.firstDropMs == 0);
  publishWeight(0.9f, hostMillis + 150, 1, 21);
  CHECK(session.firstDropMs == firstAtMs);
  CHECK(shotCurveSampler.firstDrop.atMs ==
        static_cast<uint16_t>(firstAtMs - shot.startMs));
  CHECK(shotCurveSampler.firstDrop.weightCg == 40);
  publishWeight(1.5f, hostMillis + 250, 1, 22);
  publishWeight(2.4f, hostMillis + 350, 1, 23);
  CHECK(session.firstDropMs != 0);
}

void ff03_finger_hold_does_not_fire() {
  startFirstFlowBrew();
  publishWeight(3.0f, hostMillis + 50, 1, 20);
  publishWeight(3.1f, hostMillis + 150, 1, 21);
  publishWeight(3.05f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs == 0);
  CHECK(session.firstFlow.phase == FirstFlowPhase::TOUCH);
}

void ff04_finger_release_to_zero_does_not_fire() {
  startFirstFlowBrew();
  publishWeight(8.0f, hostMillis + 50, 1, 20);
  publishWeight(0.0f, hostMillis + 150, 1, 21);
  publishWeight(0.05f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs == 0);
}

void ff05_bounce_through_residual_does_not_fire() {
  startFirstFlowBrew();
  publishWeight(8.0f, hostMillis + 50, 1, 20);
  publishWeight(1.2f, hostMillis + 150, 1, 21);
  publishWeight(0.0f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs == 0);
}

void ff06_touch_during_drops_fires_on_residual() {
  startFirstFlowBrew();
  publishWeight(8.0f, hostMillis + 50, 1, 20);
  CHECK(session.firstDropMs == 0);
  const uint32_t residualAtMs = hostMillis + 150;
  publishWeight(1.2f, residualAtMs, 1, 21);
  CHECK(session.firstDropMs == 0);
  publishWeight(1.3f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs == residualAtMs);
  CHECK(shotCurveSampler.firstDrop.weightCg == 120);
}

void ff07_gush_then_flow_backdates_to_jump() {
  startFirstFlowBrew();
  const uint32_t jumpAtMs = hostMillis + 50;
  publishWeight(4.0f, jumpAtMs, 1, 20);
  publishWeight(5.9f, jumpAtMs + 100, 1, 21);
  publishWeight(6.1f, jumpAtMs + 200, 1, 22);
  CHECK(session.firstDropMs == 0);
  publishWeight(6.2f, jumpAtMs + 300, 1, 23);
  CHECK(session.firstDropMs == jumpAtMs);
  CHECK(shotCurveSampler.firstDrop.weightCg == 400);
}

void ff16_packet_gap_restarts_confirmation() {
  startFirstFlowBrew();
  publishWeight(0.4f, hostMillis + 50, 1, 20);
  const uint32_t restartedAtMs = hostMillis + 150;
  publishWeight(0.9f, restartedAtMs, 1, 22);
  CHECK(session.firstDropMs == 0);
  CHECK(session.firstFlow.candidateMs == restartedAtMs);
  publishWeight(1.0f, hostMillis + 100, 1, 23);
  CHECK(session.firstDropMs == restartedAtMs);
  CHECK(shotCurveSampler.firstDrop.weightCg == 90);
}

void ff08_coffee_during_post_tare_grace_fires() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  CHECK(session.awaitingPostTareBaseline || session.scaleBaselineReady);
  CHECK(executeNextScaleCommand());
  // The pre-command empty-pan zero cannot settle the new tare reference.
  CHECK(session.awaitingPostTareBaseline);
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(session.scaleBaselineReady);
  publishWeight(0.4f, hostMillis + 50, 1, 20);
  publishWeight(0.8f, hostMillis + 150, 1, 21);
  publishWeight(1.2f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs != 0);
  CHECK(session.scaleBaselineG < FIRST_DROP_THRESHOLD_G);
}

void ff09_stream_past_half_goal_still_fires() {
  startFirstFlowBrew();
  runtimeConfig.goalWeightDg = 360;
  session.config.goalWeightDg = 360;
  publishWeight(0.4f, hostMillis + 50, 1, 20);
  publishWeight(0.9f, hostMillis + 150, 1, 21);
  CHECK(session.firstDropMs != 0);
  const uint32_t firstDropMs = session.firstDropMs;
  publishWeight(19.0f, hostMillis + 250, 1, 22);
  publishWeight(19.4f, hostMillis + 350, 1, 23);
  CHECK(session.firstDropMs == firstDropMs);
}

void ff10_control_ramp_records_first_flow() {
  startFirstFlowBrew();
  publishControlRamp(0.2f, 5.0f, 0.4f, 100, 10);
  CHECK(session.firstDropMs != 0);
}

void ff11_cup_and_finger_are_not_first_drop() {
  startFirstFlowBrew();
  publishWeight(200.0f, hostMillis + 50, 1, 20);
  publishWeight(200.2f, hostMillis + 150, 1, 21);
  publishWeight(199.8f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs == 0);
  CHECK(session.firstFlow.phase == FirstFlowPhase::TOUCH);

  startFirstFlowBrew();
  publishWeight(3.0f, hostMillis + 50, 1, 20);
  publishWeight(3.4f, hostMillis + 150, 1, 21);
  publishWeight(3.8f, hostMillis + 250, 1, 22);
  CHECK(session.firstDropMs == 0);
  CHECK(session.firstFlow.phase == FirstFlowPhase::TOUCH);
}

void rt14_cup_overshoot_still_retares() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t baseMs = hostMillis + 1000U;
  publishWeight(160.0f, baseMs, 1, 10);
  publishWeight(150.0f, baseMs + 100U, 1, 11);
  publishWeight(150.1f, baseMs + 200U, 1, 12);
  CHECK(session.firstDropMs == 0);
  publishStableCupWeight(150.0f, 20);
  CHECK(session.retarePerformed);
  CHECK(session.firstDropMs == 0);
  CHECK(!session.flowDuringRetare);
}

void rt15_cup_settle_creep_still_retares() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  const uint32_t baseMs = hostMillis + 1000U;
  publishWeight(150.0f, baseMs, 1, 10);
  publishWeight(150.4f, baseMs + 150U, 1, 11);
  publishWeight(150.8f, baseMs + 300U, 1, 12);
  CHECK(session.firstDropMs == 0);
  CHECK(session.retarePerformed);
  CHECK(!session.flowDuringRetare);
}

void prepareOpenRetareWindow(float minCupG) {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  runtimeConfig.minimumCupWeightG = minCupG;
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
}

void expectRetareWithoutFirstDrop() {
  CHECK(session.retarePerformed);
  CHECK(session.firstDropMs == 0);
  CHECK(!session.flowDuringRetare);
}

void rt16_configured_min_cup_weight_retares() {
  prepareOpenRetareWindow(10.0f);
  publishStableCupWeight(15.0f, 10);
  expectRetareWithoutFirstDrop();

  prepareOpenRetareWindow(10.0f);
  publishStableCupWeight(20.0f, 10);
  expectRetareWithoutFirstDrop();

  prepareOpenRetareWindow(10.0f);
  publishStableCupWeight(10.5f, 10);
  expectRetareWithoutFirstDrop();

  prepareOpenRetareWindow(15.0f);
  publishStableCupWeight(18.0f, 10);
  expectRetareWithoutFirstDrop();

  prepareOpenRetareWindow(15.0f);
  publishStableCupWeight(12.0f, 10);
  CHECK(!session.retarePerformed);
  CHECK(session.firstDropMs == 0);

  prepareOpenRetareWindow(20.0f);
  publishStableCupWeight(25.0f, 10);
  expectRetareWithoutFirstDrop();
}

void rt17_light_cup_landing_and_overshoot_retares() {
  prepareOpenRetareWindow(10.0f);
  const uint32_t landMs = hostMillis + 1000U;
  publishWeight(12.0f, landMs, 1, 10);
  publishWeight(15.0f, landMs + 100U, 1, 11);
  CHECK(session.firstDropMs == 0);
  publishStableCupWeight(15.0f, 20);
  expectRetareWithoutFirstDrop();

  prepareOpenRetareWindow(10.0f);
  const uint32_t overshootMs = hostMillis + 1000U;
  publishWeight(18.0f, overshootMs, 1, 10);
  publishWeight(15.0f, overshootMs + 100U, 1, 11);
  publishWeight(15.1f, overshootMs + 200U, 1, 12);
  CHECK(session.firstDropMs == 0);
  publishStableCupWeight(15.0f, 20);
  expectRetareWithoutFirstDrop();
}

void rt22_inshot_coffee_ramp_is_not_a_late_cup() {
  // No cup at start: coffee filling the pan at 3 g/s crosses the minimum cup
  // weight looking stable, but it is a ramp, not a placement step.
  for (bool ramp : {true, false}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = true;
    runtimeConfig.retareWindowMs = 8000;
    runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    CHECK(cupPresenceState() == CupPresenceState::ABSENT);
    if (ramp) {
      for (int i = 1; i <= 50; ++i) idleWeight(0.3f * i, 100);
      CHECK(retareWindowOpen());
      CHECK(!session.retarePerformed);
    } else {
      for (int i = 0; i < 10; ++i) idleWeight(0.0f, 100);
      publishStableCupWeight(250.0f, cupPresence.weight.sampleSequence + 1U);
      CHECK(session.retarePerformed);
    }
  }
}

void rt23_late_cup_before_first_drop_retares() {
  // Pre-infusion: no coffee by 6 s, past the 4 s late-cup time. A cup placed
  // now is still tared; once first drop was seen, it no longer is.
  for (bool drops : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.autoRetare = true;
    runtimeConfig.retareWindowMs = 4000;
    runtimeConfig.bbwProtectionMs = 12000;
    startCycle();
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    if (drops) {
      reachSessionElapsed(5000);
      simulateFirstDrops(0.0f, 10);
      CHECK(session.firstDropMs != 0);
    }
    reachSessionElapsed(6000);
    CHECK(retareWindowOpen() == !drops);
    publishStableCupWeight(250.0f, 30);
    CHECK(session.retarePerformed == !drops);
    if (drops) continue;
    CHECK(executeNextScaleCommand());
    establishPostTareBaseline();
    endBbwProtectionForTests();
    const float threshold = effectiveStopThreshold();
    for (float w = 0.5f; w < threshold - 0.5f; w += 0.5f) {
      idleWeight(w, 100);
      loop();
      CHECK(session.active);
    }
    idleWeight(threshold + 0.1f, 100);
    idleWeight(threshold + 0.2f, 100);
    loop();
    CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  }
}

void rt18_late_combined_start_does_not_rearm_after_retare() {
  prepareOpenRetareWindow(10.0f);
  publishStableCupWeight(15.0f, 10);
  expectRetareWithoutFirstDrop();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(!session.awaitingPostTareBaseline);
  ScaleEvent startResult;
  startResult.type = ScaleEventType::TIMER_START_RESULT;
  startResult.cycleId = session.id;
  startResult.commandAttempted = true;
  startResult.writeSucceeded = true;
  startResult.usedCombinedTareStart = true;
  CHECK(publishScaleEvent(startResult, true));
  processScaleWorkerEvents();
  CHECK(!session.awaitingPostTareBaseline);
}

void rt19_late_cup_then_zero_lift_allows_next_shot_retare() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  waitForRetareEnded();
  CHECK(!session.retarePerformed);
  publishStableCupWeight(150.0f, 10);
  CHECK(!session.retarePerformed);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  reachBrewState();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!session.active);
  publishWeight(0.0f, hostMillis, 1, 80);
  publishWeight(0.0f, hostMillis + 50, 1, 81);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  publishStableCupWeight(150.0f, 90);
  CHECK(session.retarePerformed);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void rt20_late_combined_start_does_not_rearm_when_cup_absent() {
  prepareOpenRetareWindow(10.0f);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(!session.awaitingPostTareBaseline);
  ScaleEvent startResult;
  startResult.type = ScaleEventType::TIMER_START_RESULT;
  startResult.cycleId = session.id;
  startResult.commandAttempted = true;
  startResult.writeSucceeded = true;
  startResult.usedCombinedTareStart = true;
  CHECK(publishScaleEvent(startResult, true));
  processScaleWorkerEvents();
  CHECK(!session.awaitingPostTareBaseline);
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  publishStableCupWeight(15.0f, 10);
  expectRetareWithoutFirstDrop();
}

void rt21_empty_pan_at_cycle_start_resyncs_stuck_present() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.autoRetare = true;
  runtimeConfig.bbwProtectionMs = minimumBbwProtectionMs(runtimeConfig);
  startCycle();
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  waitForRetareEnded();
  publishStableCupWeight(150.0f, 10);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  CHECK(!session.retarePerformed);
  reachBrewState();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!session.active);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  startCycle();
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
  CHECK(executeNextScaleCommand());
  establishPostTareBaseline();
  CHECK(retareWindowOpen());
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  publishStableCupWeight(150.0f, 90);
  CHECK(session.retarePerformed);
  CHECK(commandCount(ScaleCommandType::TARE_ONLY) == 1);
}

void pm01_original_bbw_release_after_rinse_keeps_machine_running() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(machineHidesPhysicalStop());
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(machineHidesPhysicalStop());
}

void pm02_original_bbw_release_inside_rinse_is_rinse() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
}

void pm03_original_no_scale_paddle_off_ends_shot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  startCycle();
  reachManualNoScaleState();
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm04_original_timer_only_paddle_off_ends_shot() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  runtimeConfig.timerOnly = true;
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm05_original_bbw_promote_then_off_ends_like_natural() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  startCycle();
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(machineCyclePromotedToNaturalForTest());
  CHECK(!machineHidesPhysicalStop());
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm06_original_bbw_hold_blocks_auto_stop_until_release() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm07_original_bbw_hard_limit_while_held() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  startCycle();
  advanceToBrew();
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::GLOBAL_LIMIT);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm08_natural_paddle_off_after_rinse_still_ends_shot() {
  resetHarness(false, true);
  reachReadyFromBoot();
  CHECK(runtimeConfig.paddleMode == static_cast<uint8_t>(PaddleMode::NATURAL));
  startCycle();
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm09_apply_config_accepts_original_paddle_mode() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  processWebCommand(update);
  CHECK(runtimeConfig.paddleMode == static_cast<uint8_t>(PaddleMode::ORIGINAL));
}

void configureOriginalBbwShortWall() {
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  runtimeConfig.operationalWallMs = 10000;
  runtimeConfig.scaleLossEnabled = false;
}

void advanceToOriginalBbwOperationalWall() {
  const uint32_t wallAtMs =
      session.circuitClosedAtMs + session.config.operationalWallMs;
  CHECK(hostMillis <= wallAtMs);
  runLoopAfter(wallAtMs - hostMillis);
}

void pm10_original_bbw_hold_skips_operational_wall() {
  resetHarness(false, true);
  reachReadyFromBoot();
  configureOriginalBbwShortWall();
  startCycle();
  advanceToBrew();
  CHECK(machineCycleHardMaxArmedForTest());
  CHECK(getRelaySafetySnapshot().operationalLimitMs == HARD_MAX_CIRCUIT_CLOSED_MS);
  advanceToOriginalBbwOperationalWall();
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(session.endReason == EndReason::NONE);
}

void pm11_original_bbw_release_honors_operational_wall() {
  resetHarness(false, true);
  reachReadyFromBoot();
  configureOriginalBbwShortWall();
  startCycle();
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(machineHidesPhysicalStop());
  advanceToOriginalBbwOperationalWall();
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(session.endReason == EndReason::CONFIGURED_WALL_LIMIT);
  runLoopAfter(1);
  CHECK(stopperState == StopperState::READY);
}

void pm12_auto_bbw_release_after_rinse_keeps_machine_running() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(machineHidesPhysicalStop());
  CHECK(!machineCycleHardMaxArmedForTest());
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(machineHidesPhysicalStop());
}

void pm13_auto_bbw_hold_allows_auto_stop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm14_auto_bbw_off_on_off_stays_brewing() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  startCycle();
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(machineHidesPhysicalStop());
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(!machineCyclePromotedToNaturalForTest());
  CHECK(machineHidesPhysicalStop());
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(machineHidesPhysicalStop());
}

void pm15_auto_no_scale_paddle_off_ends_shot() {
  resetHarness(false, false);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  startCycle();
  reachManualNoScaleState();
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm16_auto_timer_only_paddle_off_ends_shot() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  runtimeConfig.timerOnly = true;
  startCycle();
  advanceToBrew();
  CHECK(stopperState == StopperState::BREW);
  CHECK(!machineHidesPhysicalStop());
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm17_auto_bbw_release_inside_rinse_is_rinse() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  const uint32_t rawOnAt = startCycle();
  releaseAtPhysicalDuration(rawOnAt, runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
}

void pm18_auto_bbw_auto_stop_with_paddle_off_goes_ready() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  startCycle();
  advanceToBrew();
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::SCALE_THRESHOLD);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm19_apply_config_accepts_auto_paddle_mode() {
  resetHarness(false, false);
  reachReadyFromBoot();
  WebCommand update;
  update.type = WebCommandType::APPLY_CONFIG;
  update.config = runtimeConfig;
  update.config.paddleMode = static_cast<uint8_t>(PaddleMode::AUTO);
  processWebCommand(update);
  CHECK(runtimeConfig.paddleMode == static_cast<uint8_t>(PaddleMode::AUTO));
}

void pm20_original_on_off_on_promotes_once_without_extra_polls() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.paddleMode = static_cast<uint8_t>(PaddleMode::ORIGINAL);
  startCycle();
  advanceToBrew();
  CHECK(!machineCyclePromotedToNaturalForTest());
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::BREW);
  CHECK(!machineCyclePromotedToNaturalForTest());
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(machineCyclePromotedToNaturalForTest());
  CHECK(machineLastIntention().holdActive);
  (void)machineLastIntention();
  (void)machineLastIntention();
  CHECK(machineCyclePromotedToNaturalForTest());
  CHECK(stopperState == StopperState::BREW);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
  CHECK(session.endReason == EndReason::ACTIVATOR);
  CHECK(!getRelaySafetySnapshot().closed);
}

void pm21_hold_with_scale_keeps_k1_closed_for_five_seconds() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
  runLoopAfter(5000);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
}

void pm22_already_closed_rewrites_pin_if_gpio_dropped() {
  resetHarness(false, false);
  reachReadyFromBoot();
  CHECK(setMachineCircuitClosed(true, 5000));
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
  hostPinLevel[RELAY_GPIO] = RELAY_OPEN_LEVEL;
  CHECK(setMachineCircuitClosed(true, 5000));
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
  CHECK(debugEventExists(DebugCode::RELAY_GPIO_DESYNC));
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().fault == RelaySafetyFault::NONE);

  hostPinLevel[RELAY_GPIO] = RELAY_OPEN_LEVEL;
  serviceRelaySafety();
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().fault == RelaySafetyFault::NONE);
}

void pm23_weight_cut_does_not_reclose_while_paddle_held() {
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  endBbwProtectionForTests();
  shot.expectedEndS = 1.0f;
  runLoopAfter(1000);
  loop();
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0
  CHECK(paddleReturnReminderActive);
#endif
  const size_t closedWrites = hostRelayClosedWrites;
  runLoopAfter(200);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == closedWrites);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
}

void pm24_no_scale_hard_limit_does_not_reclose_while_held() {
  resetHarness(false, false);
  reachReadyFromBoot();
  startCycle();
  reachManualNoScaleState();
  runLoopAfter(runtimeConfig.rinseGestureMs + 1);
  reachSessionElapsed(HARD_MAX_CIRCUIT_CLOSED_MS);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  const size_t closedWrites = hostRelayClosedWrites;
  runLoopAfter(200);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == closedWrites);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(stopperState == StopperState::READY);
}

void pm25_web_start_paddle_off_keeps_k1_closed_with_scale() {
  resetHarness(false, true);
  reachReadyFromBoot();
  WebCommand on = webControlCommand(WebCommandType::REMOTE_ON);
  processWebCommand(on);
  CHECK(session.active);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
  runLoopAfter(500);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_CLOSED_LEVEL);
}

void pm26_blocked_no_scale_hold_does_not_close_k1() {
  resetHarness(false, false);
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  attemptBlockedNoScaleStart();
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
}

using TestFunction = void (*)();

void bbw03_shared_guard_parity() {
  {
    for (auto test : {t06_paddle_off_during_brew,
                     t11_ble_loss_suspends_brew_without_late_stop,
                     t12_global_limit_opens_manual_and_brew_cycles,
                     t23_prediction_triggers_after_bbw_protection_ends,
                     r06_hard_timer_opens_circuit_without_control_loop,
                     r18_watchdog_fault_opens_circuit_and_requests_safe_restart,
                     r29_direct_threshold_stops_before_regression_is_ready,
                     r51_auto_to_manual_guard_fires_while_scale_lost,
                     r52_auto_to_manual_guard_clears_on_scale_recovery,
                     r57_guard_max_weight_cut_from_predicted_time,
                     cp06_cup_removed_stops_during_bbw_protection,
                     pm06_original_bbw_hold_blocks_auto_stop_until_release,
                     pm07_original_bbw_hard_limit_while_held,
                     pm11_original_bbw_release_honors_operational_wall}) test();
  }
}

void pow01_scale_disconnect_grace_and_rinse_clock() {
  resetHarness(false, true);
  runtimeConfig.powerManagementEnabled = true;
  reachReadyFromBoot();
  CHECK(servicePowerManagement());
  CHECK(powerAppliedProfile.load() == PowerProfile::WAITING);
  startCycle();
  CHECK(session.active);
  CHECK(powerAppliedProfile.load() == PowerProfile::WORKING);
  setScaleConnected(false);
  session.weightControlState = WeightControlState::INACTIVE;
  CHECK(servicePowerManagement());
  CHECK(powerAppliedProfile.load() == PowerProfile::WORKING);
  machineRequestStop();
  machineEndCycle();
  session.active = false;
  setRawPaddle(false);
  machineSampleInput();
  CHECK(servicePowerManagement());
  CHECK(powerAppliedProfile.load() == PowerProfile::WAITING);
  CHECK(powerCooldownRemainingMs.load() == 300000);
  hostMillis += 30000;
  CHECK(servicePowerManagement());
  CHECK(powerAppliedProfile.load() == PowerProfile::COOLDOWN);
  CHECK(powerCooldownRemainingMs.load() == 270000);

  resetHarness(false, true);
  runtimeConfig.powerManagementEnabled = true;
  reachReadyFromBoot();
  startCycle();
  CHECK(enterRinse());
  CHECK(servicePowerManagement());
  CHECK(powerAppliedProfile.load() == PowerProfile::WORKING);

  resetHarness(false, false);
  runtimeConfig.powerManagementEnabled = true;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  reachReadyFromBoot();
  CHECK(beginRinseCycle(ControlSource::PHYSICAL));
  CHECK(powerAppliedProfile.load() == PowerProfile::MANUAL);
  runtimeConfig.powerManagementEnabled = false;
  CHECK(servicePowerManagement());
  CHECK(powerAppliedProfile.load() == PowerProfile::OFF);
}

void pow02_idle_scan_preserves_saved_preference() {
  resetHarness(false, false);
  applyLiveBleScanIntensity(BleScanIntensity::AGGRESSIVE);
  // The saved duty decides in every CPU power profile: ESP32 power
  // management no longer demotes an idle discovery scan to Relaxed.
  powerAppliedProfile.store(PowerProfile::IDLE);
  CHECK(startScaleDiscoveryScan(nullptr, false));
  uint16_t interval = 0, window = 0;
  bleScanHciParams(BleScanIntensity::AGGRESSIVE, interval, window);
  CHECK(scale.lastScanInterval == interval && scale.lastScanWindow == window);
  CHECK(discoveryScanIntensity() == BleScanIntensity::AGGRESSIVE);
  CHECK(liveBleScanIntensity() == BleScanIntensity::AGGRESSIVE);
  // Hours without any scale evidence keep the saved duty too; only the
  // Micra override or a saved-mode change may alter discovery.
  hostMillis += 2UL * 3600UL * 1000UL;
  serviceScaleScanIntensity();
  CHECK(scale.lastScanInterval == interval && scale.lastScanWindow == window);
  powerAppliedProfile.store(PowerProfile::OFF);
  serviceScaleScanIntensity();
  CHECK(scale.lastScanInterval == interval && scale.lastScanWindow == window);
  // Radio sleep still follows the power policy and scale activity: the
  // hardware path keeps working while scan duty stays independent.
  CHECK(syncScalePower());
  CHECK(!powerScaleBusy.load());
  setScaleConnected(true);
  CHECK(syncScalePower());
  CHECK(powerScaleBusy.load());
}

void pow06_machine_use_never_changes_scan_duty() {
  resetHarness(false, false);
  applyLiveBleScanIntensity(BleScanIntensity::RELAXED);
  powerAppliedProfile.store(PowerProfile::IDLE);
  reachReadyFromBoot();
  CHECK(discoveryScanIntensity() == BleScanIntensity::RELAXED);

  // A machine activation (paddle ON edge) no longer arms any scan window.
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(discoveryScanIntensity() == BleScanIntensity::RELAXED);
  hostMillis += 5UL * 60UL * 1000UL;
  runLoopAfter(1);
  CHECK(discoveryScanIntensity() == BleScanIntensity::RELAXED);

  // The same holds with a connected scale.
  resetHarness(false, true);
  applyLiveBleScanIntensity(BleScanIntensity::RELAXED);
  reachReadyFromBoot();
  setScaleConnected(true);
  CHECK(scaleLinkAvailable(getScaleLinkSnapshot()));
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS);
  CHECK(discoveryScanIntensity() == BleScanIntensity::RELAXED);
  CHECK(scaleLinkAvailable(getScaleLinkSnapshot()));
}

void pow03_ble_wake_without_link_is_bounded() {
  resetHarness(false, false);
  hostBtSleeping = true; // Initial controller wake is asynchronous too.
  CHECK(!syncScalePower());
  hostMillis += 50;
  hostBtSleeping = false;
  CHECK(syncScalePower());
  CHECK(!criticalTaskWatchdogFaulted());

  resetHarness(false, false);
  powerAppliedProfile.store(PowerProfile::IDLE);
  CHECK(syncScalePower());
  hostBtSleeping = true;
  powerAppliedProfile.store(PowerProfile::WEB);
  hostMillis = UINT32_MAX - 50;
  CHECK(!syncScalePower());
  CHECK(hostBtWakeRequests == 1);
  CHECK(!criticalTaskWatchdogFaulted());
  hostMillis += 99;
  CHECK(!syncScalePower());
  CHECK(!criticalTaskWatchdogFaulted());
  ++hostMillis;
  CHECK(!syncScalePower());
  CHECK(powerBleError.load() == ESP_ERR_TIMEOUT);
  CHECK(criticalTaskWatchdogFaulted());
}

void pow04_ble_policy_failure_recovery() {
  resetHarness(false, false);
  CHECK(syncScalePower());
  powerAppliedProfile.store(PowerProfile::IDLE);
  hostBtEnableError = ESP_ERR_INVALID_STATE;
  CHECK(syncScalePower()); // Optional savings rejected; awake service survives.
  CHECK(powerBleError.load() == ESP_ERR_INVALID_STATE);
  CHECK(!powerBleSleeping.load());
  CHECK(hostBtDisableCalls == 2);
  CHECK(!criticalTaskWatchdogFaulted());
  CHECK(applyPowerProfile(PowerProfile::IDLE));
  CHECK(powerAppliedProfile.load() == PowerProfile::OFF);

  resetHarness(false, false);
  powerAppliedProfile.store(PowerProfile::IDLE);
  CHECK(syncScalePower());
  powerAppliedProfile.store(PowerProfile::WAITING);
  hostBtDisableError = ESP_ERR_INVALID_STATE;
  CHECK(!syncScalePower());
  CHECK(criticalTaskWatchdogFaulted());
  serviceRelaySafety();
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(safeRestartPending());

  resetHarness(false, true);
  CHECK(setMachineCircuitClosed(true, 5000));
  hostBtSleeping = true;
  CHECK(!syncScalePower());
  hostMillis += 100;
  CHECK(!syncScalePower());
  CHECK(powerBleError.load() == ESP_ERR_TIMEOUT);
  serviceRelaySafety();
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(safeRestartPending());
}

void pow05_power_config_command_and_persistence() {
  resetHarness(false, false);
  reachReadyFromBoot();
  for (bool enabled : {true, false}) {
    WebCommand update;
    update.type = WebCommandType::APPLY_CONFIG;
    update.requestId = enabled ? 41 : 42;
    update.config = runtimeConfig;
    update.config.powerManagementEnabled = enabled;
    processWebCommand(update);
    CHECK(runtimeConfig.powerManagementEnabled == enabled);
    CHECK(runtimeConfig.revision == update.config.revision + 1);
    CHECK(runtimePersistPending);
    CHECK(hostLastForwardedNetworkCommand.requestId == update.requestId);
    CHECK(hostLastForwardedNetworkCommand.resultState == CommandResultState::APPLIED);
    hostRuntimePersistSucceeds = false;
    runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
    CHECK(publishedControlStatus.configPersistFailed);
    CHECK(runtimeConfig.powerManagementEnabled == enabled);
    hostRuntimePersistSucceeds = true;
    runLoopAfter(RUNTIME_PERSIST_RETRY_MS + 1);
    CHECK(!runtimePersistPending && !runtimePersistFailed);
    CHECK(hostLastFlushedRuntime.powerManagementEnabled == enabled);
    CHECK(hostLastFlushedRuntime.revision == runtimeConfig.revision);
    CHECK(publishedControlStatus.config.powerManagementEnabled == enabled);
    CHECK(!publishedControlStatus.configPersistPending);
  }
  WebCommand blocked;
  blocked.type = WebCommandType::APPLY_CONFIG;
  blocked.config = runtimeConfig;
  blocked.config.powerManagementEnabled = true;
  session.active = true;
  processWebCommand(blocked);
  CHECK(!runtimeConfig.powerManagementEnabled);
  CHECK(runtimeConfig.revision == blocked.config.revision);
}

struct TestCase {
  const char *id;
  TestFunction function;
};

void enterCompatibilityMode() {
  firmwareModeRaw =
      static_cast<uint8_t>(openbrewbyweight::FirmwareMode::COMPATIBILITY);
}

void cm01_compatibility_mirror_follows_activator() {
  resetHarness(false, false);
  enterCompatibilityMode();
  reachReadyFromBoot();
  CHECK(stopperState == StopperState::READY);
  for (int cycle = 0; cycle < 2; ++cycle) {
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
    CHECK(getRelaySafetySnapshot().closed);
    CHECK(stopperState == StopperState::READY);
    CHECK(!session.active);
    // A held brew stays closed with no BBW feature wall cutting it.
    runLoopAfter(2000);
    CHECK(getRelaySafetySnapshot().closed);
    setRawPaddle(false);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
    CHECK(!getRelaySafetySnapshot().closed);
    CHECK(!session.active);
  }
}

void cm02_compatibility_has_no_duration_cutoff() {
  for (uint32_t startAt : {0U, UINT32_MAX - 30000U}) {
    resetHarness(false, false);
    enterCompatibilityMode();
    reachReadyFromBoot();
    hostMillis = startAt;
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
    const RelaySafetySnapshot started = getRelaySafetySnapshot();
    CHECK(started.closed && started.hardLimitMs == 0 && started.operationalLimitMs == 0);
    CHECK(!independentSafetyTimer.running() && !relaySafetyTimer->active &&
          !operationalLimitTimer->active);
    const size_t openWrites = hostRelayOpenWrites;
    for (uint32_t duration : {59999U, 60000U, 60001U, 90000U, 180000U}) {
      runLoopAfter(duration - elapsedMs(started.closedAtMs));
      // Even an obsolete queued deadline callback must not cut the mirror.
      relaySafetyTimerCallback(nullptr);
      operationalLimitTimerCallback(nullptr);
      independentSafetyTimerCallback(nullptr);
      const RelaySafetySnapshot held = getRelaySafetySnapshot();
      CHECK(held.closed && held.generation == started.generation);
      CHECK(held.closedAtMs == started.closedAtMs && held.fault == RelaySafetyFault::NONE);
      CHECK(stopperState == StopperState::READY && !session.active);
      CHECK(hostRelayOpenWrites == openWrites);
    }
    setRawPaddle(false);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
    CHECK(!getRelaySafetySnapshot().closed);
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
    CHECK(getRelaySafetySnapshot().closed);
  }
}

void cm09_compatibility_preserves_fault_and_readiness_interlocks() {
  for (int failure = 0; failure < 4; ++failure) {
    resetHarness(false, false);
    enterCompatibilityMode();
    reachReadyFromBoot();
    if (failure == 0) taskWatchdogReady = false;
    if (failure == 1) relaySafetyTimersReady = false;
    if (failure == 2) hostCircuitArmBeforeCommitHook = [] {
      tripRelaySafety(RelaySafetyFault::TASK_WATCHDOG_FAILURE);
    };
    setRawPaddle(true);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
    if (failure == 3) {
      CHECK(getRelaySafetySnapshot().closed);
      safetyEventFlags.set(SAFETY_EVENT_CRITICAL_TASK_WATCHDOG);
      serviceRelaySafety();
    }
    CHECK(!getRelaySafetySnapshot().closed);
    CHECK(getRelaySafetySnapshot().state == RelaySafetyState::LOCKOUT);
    CHECK(hostPinLevel[RELAY_GPIO] == RELAY_OPEN_LEVEL);
    if (failure != 3) CHECK(stopperState == StopperState::REQUIRES_OFF);
  }
}

void cm03_compatibility_rejects_actuation_commands() {
  resetHarness(false, false);
  enterCompatibilityMode();
  reachReadyFromBoot();
  hostForwardAcceptedNetworkCommandSucceeds = false;
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
  CHECK(getRelaySafetySnapshot().closed);
  const WebCommandType blocked[] = {
      WebCommandType::REMOTE_ON,   WebCommandType::REMOTE_OFF,
      WebCommandType::RINSE,       WebCommandType::STOP,
      WebCommandType::STOP_HEARTBEAT, WebCommandType::FORCE_SWITCH_PULSE,
      WebCommandType::STATE_OVERRIDE_ON, WebCommandType::STATE_OVERRIDE_OFF,
  };
  for (WebCommandType type : blocked) {
    controlResultPending = false;
    controlResultCommand = WebCommand{};
    processWebCommand(webControlCommand(type));
    CHECK(controlResultPending);
    CHECK(controlResultCommand.resultState == CommandResultState::FAILED);
    CHECK(!controlResultCommand.succeeded);
    CHECK(!session.active);
    // A rejected web stop must never open the mirrored circuit.
    CHECK(getRelaySafetySnapshot().closed);
  }
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
  // Non-actuation commands keep answering.
  controlResultPending = false;
  controlResultCommand = WebCommand{};
  processWebCommand(webControlCommand(WebCommandType::TASK_PROFILER_STOP));
  CHECK(controlResultCommand.resultState == CommandResultState::APPLIED);
  hostForwardAcceptedNetworkCommandSucceeds = true;
}

void cm04_compatibility_blocks_ble_enable_command() {
  resetHarness(false, false);
  enterCompatibilityMode();
  reachReadyFromBoot();
  hostForwardAcceptedNetworkCommandSucceeds = false;
  WebCommand command = webControlCommand(WebCommandType::BLE_SCAN_INTENSITY);
  command.bleScan.specified = BleScanCommandPayload::ENABLED;
  command.bleScan.enabled = 1;
  controlResultPending = false;
  controlResultCommand = WebCommand{};
  processWebCommand(command);
  CHECK(controlResultPending);
  CHECK(controlResultCommand.resultState == CommandResultState::FAILED);
  hostForwardAcceptedNetworkCommandSucceeds = true;
}

void cm05_compatibility_mutes_alerts_except_recovery() {
  resetHarness(false, false);
  CHECK(soundAlertsEnabled());
  enterCompatibilityMode();
  CHECK(!soundAlertsEnabled());
  const AlertChannelContext ctx = currentAlertChannelContext();
  CHECK(!ctx.soundAlertsEnabled);
  CHECK(ctx.buzzerSupportEnabled);
  CHECK(selectAlertSink(AlertKind::Independent, AlertEvent::FIRST_DROP,
                        ctx) == AlertSink::None);
  CHECK(selectAlertSink(AlertKind::Recovery, AlertEvent::FIRST_DROP,
                        ctx) == AlertSink::Buzzer);
}

void cm07_compatibility_boots_parked_with_activator_held() {
  resetHarness(true, false);
  enterCompatibilityMode();
  // Boot state is REQUIRES_OFF with the activator held: the mirror must
  // never close a circuit the boot found already held.
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 500);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed);
  runLoopAfter(2000);
  CHECK(!getRelaySafetySnapshot().closed);
  // The mirror arms only after the activator returns stably idle.
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 50);
  CHECK(stopperState == StopperState::READY);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
  CHECK(getRelaySafetySnapshot().closed);
}

void cm06_compatibility_command_gate_scope() {
  resetHarness(false, false);
  // Full mode by default: the mode flag itself is what gates the denylist.
  CHECK(!firmwareCompatibilityMode());
  enterCompatibilityMode();
  CHECK(firmwareCompatibilityMode());
  CHECK(webCommandBlockedInCompatibilityMode(WebCommandType::REMOTE_ON));
  CHECK(webCommandBlockedInCompatibilityMode(WebCommandType::RINSE));
  CHECK(webCommandBlockedInCompatibilityMode(WebCommandType::BLE_SCAN_INTENSITY));
  CHECK(!webCommandBlockedInCompatibilityMode(WebCommandType::RESTART));
  CHECK(!webCommandBlockedInCompatibilityMode(WebCommandType::FACTORY_RESET));
  CHECK(!webCommandBlockedInCompatibilityMode(WebCommandType::APPLY_CONFIG));
  CHECK(!webCommandBlockedInCompatibilityMode(WebCommandType::SAVE_NETWORK));
}

void cm08_compatibility_mirror_holds_against_guard_drive_deny() {
  resetHarness(false, false);
  enterCompatibilityMode();
  // A brew guard that would deny the activator drive in full mode (no-scale
  // BBW armed, scale never usable while BLE is parked) must not fight the
  // transparent mirror into an open/close pair per control loop.
  enableNoScaleShotGuardForTest();
  reachReadyFromBoot();
  CHECK(stopperState == StopperState::READY);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  const size_t closedWrites = hostRelayClosedWrites;
  const size_t openWrites = hostRelayOpenWrites;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS * 4);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == closedWrites);
  CHECK(hostRelayOpenWrites == openWrites);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 5);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayOpenWrites == openWrites + 1);
}

void home01_stream_lifecycle_uses_canonical_final_record() {
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear(false);
  shotCurves.clear(false);
  session.active = session.startedWithScale = true;
  session.id = 41;
  session.config = snapshotConfig(runtimeConfig);
  session.config.dripDelayMs = 3000;
  session.connectionGenerationAtStart = getScaleLinkSnapshot().connectionGeneration;
  session.startedAtMs = session.circuitClosedAtMs = hostMillis;
  const uint32_t start = hostMillis;
  resetShotTrajectory(start);
  acceptWeightIntoTrajectory(0, start, 1);
  hostMillis = start + 14010;
  acceptWeightIntoTrajectory(36, hostMillis, 2);
  currentWeight = 36;
  currentWeightSequence = 2;
  currentWeightReceivedAtMs = hostMillis;
  schedulePendingShotFinalize(EndReason::ACTIVATOR, 14010);
  homeCycleResult = lastShotFromEndedCycle(EndReason::ACTIVATOR, 14010);
  homeCycleResolvedAtMs = hostMillis;
  session.active = false;
  publishControlStatus();
  ControlStatusSnapshot status;
  copyControlStatus(status);
  CHECK(status.homePending);
  CHECK(status.homeCycle.cycleId == 41);
  CHECK(status.homeCycle.durationMs == 14010);
  CHECK(status.shotCurve.ended.atMs == 14010);
  CHECK(appendShotCurveObservation(pendingFinalize.curve, 36.5f, 15000, false));
  hostMillis = start + 15000;
  currentWeight = 36.5f;
  currentWeightSequence = 3;
  currentWeightReceivedAtMs = hostMillis;
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.homeCycle.durationMs == 14010);
  CHECK(status.shotCurve.count == 3);
  CHECK(curvePoint(status.shotCurve, 2).atMs == 15000);
  hostMillis = start + 17010;
  pendingShotFinalizeTask();
  publishControlStatus();
  copyControlStatus(status);
  ShotLogRecord record;
  ShotCurveRecord curve;
  CHECK(copyHomeShot(record, &curve));
  CHECK(!status.homePending);
  CHECK(status.homeResolvedAtMs == hostMillis);
  CHECK(status.homeCycle.shotLogId == record.id);
  CHECK(status.homeCycle.durationMs == record.durationDs * 100U);
  CHECK(status.homeCycle.currentWeightG == record.actualWeightCg / 100.0f);
  CHECK(status.shotCurve.count == curve.count);
  // A newer activation's presentation survives late predecessor finalization.
  homeCycleResult.cycleId = 42;
  persistLastShotFromFinalize(pendingFinalize, 37, true);
  CHECK(homeCycleResult.cycleId == 42);
  // Immediate rinse outcomes are available without changing persistent Stats.
  session.id = 43;
  stopperState = StopperState::RINSE;
  homeCycleResult = lastShotFromEndedCycle(EndReason::ACTIVATOR, 2000);
  homeCycleResolvedAtMs = hostMillis;
  publishControlStatus();
  copyControlStatus(status);
  CHECK(status.homeCycle.cycleId == 43);
  CHECK(status.homeCycle.shotType == static_cast<uint8_t>(LastShotType::RINSE));
  CHECK(status.homeCycle.shotLogId == 0);
  CHECK(shotLog.count() == 1);
  // A fragment carries its reference point, the stream from there (15000 ms
  // escapes: FF 98 3A, then +50 cg) and global discontinuity indices.
  char json[SHOT_CURVE_JSON_CAPACITY];
  curve.breakBefore[0] |= 4;
  CHECK(formatShotCurveJsonBody(json, sizeof(json), curve, 2));
  CHECK(strstr(json, "\"wRef\":[14010,3600],\"wD\":\"/5g6Mg==\"") == json);
  CHECK(strstr(json, "\"wBreakBefore\":[2]") != nullptr);
  CHECK(!formatShotCurveJsonBody(json, sizeof(json), curve, curve.count + 1));
}

void home02_nonqualifying_resolution_preserves_cutoff_and_endpoint() {
  for (const bool postDrip : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    shotLog.clear(false);
    session.active = session.startedWithScale = true;
    session.id = 401;
    session.config = snapshotConfig(runtimeConfig);
    session.config.dripDelayMs = 3000;
    session.connectionGenerationAtStart = getScaleLinkSnapshot().connectionGeneration;
    session.startedAtMs = session.circuitClosedAtMs = hostMillis;
    const uint32_t start = hostMillis;
    resetShotTrajectory(start);
    acceptWeightIntoTrajectory(0, start, 1);
    hostMillis = start + 10057;
    acceptWeightIntoTrajectory(20, hostMillis, 2);
    currentWeight = 20;
    currentWeightSequence = 2;
    currentWeightReceivedAtMs = hostMillis;
    session.firstDropMs = start + 1057;
    session.retareAtMs = start + 57;
    shot.automaticBrew = session.calibrationEligible = true;
    schedulePendingShotFinalize(EndReason::ACTIVATOR, 10057);
    CHECK(pendingFinalize.pending && !pendingFinalize.logEligible);
    homeCycleResult = lastShotFromEndedCycle(EndReason::ACTIVATOR, 10057);
    session.active = false;
    if (postDrip) {
      CHECK(appendShotCurveObservation(pendingFinalize.curve, 21, 11000, false));
      currentWeight = 21;
      currentWeightSequence = 3;
      currentWeightReceivedAtMs = start + 11000;
    }
    hostMillis = start + 13057;
    setScaleConnected(true);
    pendingShotFinalizeTask();
    publishControlStatus();
    ControlStatusSnapshot status;
    copyControlStatus(status);
    CHECK(!status.homePending && shotLog.count() == 0);
    CHECK(status.homeCycle.shotLogId == 0);
    CHECK(status.homeCycle.durationMs == 10057);
    CHECK(status.homeCycle.firstDropElapsedMs == 1057);
    CHECK(status.homeCycle.tareElapsedMs == 57);
    CHECK(status.homeResolvedAtMs == hostMillis);
    CHECK(status.homeCycle.currentWeightG == (postDrip ? 21 : 20));
    CHECK(status.shotCurve.ended.atMs == 10057);
    CHECK(status.shotCurve.ended.weightCg == (postDrip ? 2100 : 2000));
    CHECK(curvePoint(status.shotCurve, status.shotCurve.count - 1).atMs == (postDrip ? 11000 : 10057));
  }
  for (const bool weighted : {false, true}) {
    resetHarness(false, true);
    reachReadyFromBoot();
    shotLog.clear(false);
    session.id = 402;
    session.active = true;
    session.config = snapshotConfig(runtimeConfig);
    session.startedAtMs = session.circuitClosedAtMs = hostMillis;
    const uint32_t start = hostMillis;
    resetShotTrajectory(start);
    hostMillis += 2057;
    if (weighted) {
      acceptWeightIntoTrajectory(2, hostMillis, 2);
      currentWeight = 2;
      currentWeightSequence = 2;
      currentWeightReceivedAtMs = hostMillis;
    }
    stopperState = StopperState::RINSE;
    schedulePendingShotFinalize(EndReason::ACTIVATOR, 2057);
    homeCycleResult = lastShotFromEndedCycle(EndReason::ACTIVATOR, 2057);
    session.active = false;
    publishControlStatus();
    ControlStatusSnapshot status;
    copyControlStatus(status);
    CHECK(!status.homePending && shotLog.count() == 0);
    CHECK(status.homeCycle.durationMs == 2057);
    CHECK(status.shotCurve.ended.atMs == (weighted ? 2057 : SHOT_LOG_METRIC_MISSING));
    CHECK(status.shotCurve.ended.weightCg == (weighted ? 200 : SHOT_LOG_WEIGHT_MISSING));
    CHECK(!shotCurveSampler.active);
  }
}

void home03_active_flow_requires_fresh_baseline_and_confirmed_drop() {
  resetHarness(false, true);
  reachReadyFromBoot();
  setScaleConnected(true);
  startCycle();
  const uint32_t start = session.circuitClosedAtMs != 0 ? session.circuitClosedAtMs : session.startedAtMs;
  hostMillis = start + 12000;
  session.scaleBaselineReady = true;
  session.scaleBaselineG = 80;
  session.firstDropMs = start + 2000;
  acceptWeightIntoTrajectory(100, hostMillis, 2);
  currentWeight = 100;
  currentWeightSequence = 2;
  currentWeightReceivedAtMs = hostMillis;
  markScaleWorkerProgress();
  publishControlStatus();
  CHECK(publishedControlStatus.homeCycle.weightValid);
  CHECK(publishedControlStatus.homeCycle.averageFlowValid);
  CHECK(std::fabs(publishedControlStatus.homeCycle.averageFlowGps - 2) < 0.001f);
  session.scaleBaselineReady = false;
  publishControlStatus();
  CHECK(!publishedControlStatus.homeCycle.averageFlowValid);
  session.scaleBaselineReady = true;
  session.firstDropMs = 0;
  publishControlStatus();
  CHECK(!publishedControlStatus.homeCycle.averageFlowValid);
  session.firstDropMs = start + 2000;
  hostMillis += MAX_AUTOMATION_WEIGHT_AGE_MS + 1;
  publishControlStatus();
  CHECK(!publishedControlStatus.homeCycle.averageFlowValid);
}

// Live-vs-replay equivalence: the control loop's smooth-cut decision equals a
// kernel-only replay of the samples it accepted (same law order per sample:
// estimator, direct backstop, then the timed law).
void eq01_live_cut_equals_kernel_replay() {
  const struct { uint32_t firstDropMs; int32_t rateDgPerS; } shots[] = {
      {6000, 26}, {6000, 15}, {7000, 11}, {5000, 34}, {6000, 21}, {8000, 9}};
  for (const auto &c : shots) {
    resetHarness(false, true);
    reachReadyFromBoot();
    runtimeConfig.avoidAccidentalTouchEnabled = false;
    runtimeConfig.fastSideEnabled = true;
    runtimeConfig.slowSideEnabled = true;
    startCycle();
    advanceToBrew();
    std::vector<std::pair<int32_t, int32_t>> samples;
    int32_t liveCutMs = -1;
    while (stopperState == StopperState::BREW && samples.size() < 600) {
      hostMillis += 100;
      markScaleWorkerProgress();
      int32_t shotMs = 0;
      shotTimeAtMs(hostMillis, shotMs);
      const int32_t w = shotMs < static_cast<int32_t>(c.firstDropMs)
          ? 0 : c.rateDgPerS * (shotMs - static_cast<int32_t>(c.firstDropMs)) / 1000;
      publishWeight(w / 10.0f);
      samples.push_back({shotMs, w});
      loop();
      if (stopperState != StopperState::BREW) liveCutMs = shotMs;
    }
    const EndReason live = session.endReason;
    // Kernel replay with the shot's own parameters.
    const corridor::CorridorState &armed = session.cut;
    corridor::RateEstimator rate;
    uint8_t confirmations = 0;
    int32_t replayCutMs = -1;
    EndReason replay = EndReason::NONE;
    for (const auto &k : samples) {
      const int32_t t = k.first, w = k.second;
      if (t < armed.params.delayMs) {
        rate.push(t, w);
        continue;
      }
      int32_t star = INT32_MAX;
      if (rate.push(t, w) == corridor::RateEstimator::Result::ACCEPTED && rate.valid) {
        int32_t crossing = 0;
        if (corridor::crossingMs(armed.params, t, w, rate.rateX100, armed.tauMs,
                                 armed.offsetDg, crossing) &&
            crossing < armed.params.wallMs)
          star = crossing;
      }
      confirmations = w >= corridor::backstopThresholdDg(armed.params, t, armed.offsetDg)
                          ? confirmations + 1 : 0;
      if (confirmations >= DIRECT_STOP_CONFIRMATION_SAMPLES || t >= star) {
        replayCutMs = t;
        replay = corridor::cutReason(armed.params, corridor::cutLineDg(armed.params, t));
        break;
      }
    }
    CHECK(liveCutMs > 0 && liveCutMs == replayCutMs);
    CHECK(live == replay);
  }
}

void ch01_channeling_caps_the_cut_line_at_the_target() {
  resetHarness(false, true);
  reachReadyFromBoot();
  runtimeConfig.fastSideEnabled = true;
  startCycle();
  advanceToBrew();
  CHECK(session.cut.params.fastOn);
  session.cut.rate.channeling = true;  // As latched by the estimator.
  float weight = 0.0f;
  for (unsigned i = 0; i < 30; ++i) {
    hostMillis += 100;
    markScaleWorkerProgress();
    publishWeight(weight += 0.2f);
  }
  CHECK(!session.cut.params.fastOn);
  CHECK(cutLineNowDg() == session.cut.params.goalDg);
}

// Scale-loss backup v2 (plan §4.9): the deadline regime chosen at the loss.
int32_t scaleLossDeadlineShotMsForTest() {
  int32_t nowMs = 0;
  shotTimeAtMs(millis(), nowMs);
  return nowMs + static_cast<int32_t>(session.autoToManualGuardDeadlineAtMs - millis());
}

void sl01_scale_loss_regimes() {
  // R1: a valid flow projects the cut at most 3 s ahead.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  session.firstDropObserved = true;
  float weight = 10.0f;
  for (unsigned i = 0; i < 40; ++i) {
    hostMillis += 100;
    markScaleWorkerProgress();
    publishWeight(weight += 0.5f);  // 5 g/s, ends at 30 g
  }
  const bool projectable = session.cut.crossingValid;
  const int32_t projected = session.cut.crossingMs;
  setScaleConnected(false);
  loop();
  CHECK(session.autoToManualGuardEnforced);
  CHECK(projectable);
  CHECK(session.scaleLossSource == static_cast<uint8_t>(corridor::LossSource::PROJECTION));
  CHECK(!session.scaleLossBlind);
  CHECK(scaleLossDeadlineShotMsForTest() == projected);
  reachSessionElapsed(static_cast<uint32_t>(projected) + 50);
  CHECK(session.endReason == EndReason::AUTO_TO_MANUAL_GUARD);
  CHECK(pendingFinalize.scaleLossSource ==
        static_cast<uint8_t>(corridor::LossSource::PROJECTION));

  // R3: lost before the first drops and no history: the window midpoint.
  resetHarness(false, true);
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  setScaleConnected(false);
  loop();
  CHECK(session.scaleLossSource == static_cast<uint8_t>(corridor::LossSource::WINDOW));
  CHECK(session.scaleLossBlind);
  CHECK(scaleLossDeadlineShotMsForTest() ==
        (session.cut.params.fastBeforeMs + session.cut.params.slowAfterMs) / 2);

  // R2: three recent arrivals of this preset since its epoch, judged against
  // its current window and target.
  resetHarness(false, true);
  reachReadyFromBoot();
  shotLog.clear(false);
  ShotLogRecord record = {};
  record.durationDs = 300;
  record.goalWeightDg = activeShotPreset(presetBank).goalWeightDg;
  record.fastBeforeDs = static_cast<uint16_t>(activeShotPreset(presetBank).fastBeforeMs / 100);
  record.slowAfterDs = static_cast<uint16_t>(activeShotPreset(presetBank).slowAfterMs / 100);
  record.actualWeightCg = 3600;
  record.stopDetail = static_cast<uint8_t>(ShotLogStopDetail::NORMAL_TARGET);
  shotLogSetPresetId(record, presetBank.activeId);
  for (uint16_t arrival : {290, 300, 310}) {
    record.arrivalDs = arrival;
    CHECK(shotLog.append(record, false));
  }
  startCycle();
  advanceToBrew();
  CHECK(session.cut.history.n == 3 && session.cut.history.medianMs == 30000);
  setScaleConnected(false);
  loop();
  CHECK(session.scaleLossSource == static_cast<uint8_t>(corridor::LossSource::HISTORY));
  CHECK(scaleLossDeadlineShotMsForTest() ==
        30000 + 2 * session.cut.history.sigmaMs / 3 + corridor::HISTORY_MARGIN_MS);

  // Fixed time below the weight-cut delay is clamped to the delay.
  resetHarness(false, true);
  runtimeConfig.scaleLossFixedMs = 1000;
  reachReadyFromBoot();
  startCycle();
  advanceToBrew();
  setScaleConnected(false);
  loop();
  CHECK(session.scaleLossSource == static_cast<uint8_t>(corridor::LossSource::FIXED));
  CHECK(scaleLossDeadlineShotMsForTest() >= session.cut.params.delayMs);
}

void sl02_new_coffee_restarts_the_statistics_epoch() {
  resetHarness(false, false);
  reachReadyFromBoot();
  shotLog.clear(false);
  ShotLogRecord record = {};
  record.durationDs = 300;
  CHECK(shotLog.append(record, false));
  CHECK(shotLog.append(record, false));
  WebCommand command;
  command.type = WebCommandType::PRESET_OP;
  command.requestId = 970;
  command.presetAction = static_cast<uint8_t>(PresetAction::NEW_COFFEE);
  command.presetId = presetBank.activeId;
  processWebCommand(command);
  CHECK(activeShotPreset(presetBank).learningEpoch == shotLog.nextRecordId());
  runLoopAfter(RUNTIME_PERSIST_DEBOUNCE_MS + 1);
  // A window or target edit keeps it: arrivals filter by each shot's own
  // window and target instead, and first drops do not depend on them.
  const uint32_t epoch = activeShotPreset(presetBank).learningEpoch;
  CHECK(shotLog.append(record, false));
  WebCommand save;
  save.type = WebCommandType::PRESET_OP;
  save.requestId = 971;
  save.presetAction = static_cast<uint8_t>(PresetAction::SAVE);
  save.presetId = presetBank.activeId;
  save.config = runtimeConfig;
  save.config.goalWeightDg = runtimeConfig.goalWeightDg + 20;
  processWebCommand(save);
  CHECK(activeShotPreset(presetBank).learningEpoch == epoch);
  CHECK(epoch != shotLog.nextRecordId());
}

const TestCase testCases[] = {
    {"HOME03", home03_active_flow_requires_fresh_baseline_and_confirmed_drop},
    {"HOME02", home02_nonqualifying_resolution_preserves_cutoff_and_endpoint},
    {"HOME01", home01_stream_lifecycle_uses_canonical_final_record},
    {"TF01", at11_touch_fallback_only_when_both_enabled},
    {"TF02", at12_touch_fallback_duration_and_evidence_resets},
    {"TF03", at13_touch_fallback_guard_thresholds},
    {"TF04", at14_touch_fallback_respects_inhibitions_and_rollover},
    {"TF05", at15_touch_fallback_preset_presence},
    {"TF06", at16_touch_fallback_uses_receive_time_for_guard_boundaries},
    {"POW01", pow01_scale_disconnect_grace_and_rinse_clock},
    {"POW02", pow02_idle_scan_preserves_saved_preference},
    {"POW03", pow03_ble_wake_without_link_is_bounded},
    {"POW06", pow06_machine_use_never_changes_scan_duty},
    {"POW04", pow04_ble_policy_failure_recovery},
    {"POW05", pow05_power_config_command_and_persistence},
    {"T01", t01_boot_with_paddle_off},
    {"T02", t02_boot_with_activator_on},
    {"T02B", t02b_off_wake_bypasses_brew_guards_and_records_power_on},
    {"T02C", t02c_wake_hard_limit_requires_release_before_rearming},
    {"T02D", t02d_wake_timer_arm_failure_stays_open},
    {"T02E", t02e_wake_restores_suppressed_held_paddle},
    {"T03", t03_sustained_on_enters_brew_once},
    {"T04", t04_exact_rinse_boundary_and_duration},
    {"T04B", t04b_rinse_disabled_short_on_off_is_not_rinse},
    {"T04C", t04c_rinse_demote_does_not_learn_offset},
    {"T04D", t04d_rinse_demote_anchors_duration_to_activation},
    {"T29", t29_rinse_demote_begin_fail_does_not_enter_rinse},
    {"T05", t05_release_between_rinse_and_brew_is_short_shot},
    {"T06", t06_paddle_off_during_brew},
    {"T07", t07_scale_prediction_requires_release_after_stop},
    {"T08", t08_on_during_rinse_is_ignored},
    {"T09", t09_rinse_ending_on_requires_off},
    {"T10", t10_paddle_bounce_does_not_start_cycle},
    {"T11", t11_ble_loss_suspends_brew_without_late_stop},
    {"T12", t12_global_limit_opens_manual_and_brew_cycles},
    {"T13", t13_reset_path_starts_with_relay_open},
    {"T14", t14_automatic_stop_stays_open_while_activator_on},
    {"T15", t15_repeated_rinse_and_brew_reset_session_state},
    {"T16", t16_only_micra_states_are_compiled},
    {"T17", t17_simultaneous_global_limit_and_paddle_off_is_idempotent},
    {"T18", t18_rinse_and_short_shot_each_request_one_stop},
    {"T19", t19_manual_cycle_without_scale_has_no_timer_commands},
    {"T20", t20_rinse_without_scale},
    {"T21", t21_global_limit_without_scale_rearms_after_release},
    {"T22", t22_scale_connection_does_not_promote_manual_cycle},
    {"T23", t23_prediction_triggers_after_bbw_protection_ends},
    {"T24", t24_paddle_off_during_brew_is_immediate},
    {"T25", t25_ble_loss_during_rinse_window_preserves_classification},
    {"T26", t26_reconnected_suspended_cycle_sends_one_stop_on_release},
    {"T27", t27_configuration_is_rejected_while_cycle_is_active},
    {"T28", t28_paddle_motion_cannot_cancel_or_extend_rinse},
    {"PM01", pm01_original_bbw_release_after_rinse_keeps_machine_running},
    {"PM02", pm02_original_bbw_release_inside_rinse_is_rinse},
    {"PM03", pm03_original_no_scale_paddle_off_ends_shot},
    {"PM04", pm04_original_timer_only_paddle_off_ends_shot},
    {"PM05", pm05_original_bbw_promote_then_off_ends_like_natural},
    {"PM06", pm06_original_bbw_hold_blocks_auto_stop_until_release},
    {"PM07", pm07_original_bbw_hard_limit_while_held},
    {"PM08", pm08_natural_paddle_off_after_rinse_still_ends_shot},
    {"PM09", pm09_apply_config_accepts_original_paddle_mode},
    {"PM10", pm10_original_bbw_hold_skips_operational_wall},
    {"PM11", pm11_original_bbw_release_honors_operational_wall},
    {"PM12", pm12_auto_bbw_release_after_rinse_keeps_machine_running},
    {"PM13", pm13_auto_bbw_hold_allows_auto_stop},
    {"PM14", pm14_auto_bbw_off_on_off_stays_brewing},
    {"PM15", pm15_auto_no_scale_paddle_off_ends_shot},
    {"PM16", pm16_auto_timer_only_paddle_off_ends_shot},
    {"PM17", pm17_auto_bbw_release_inside_rinse_is_rinse},
    {"PM18", pm18_auto_bbw_auto_stop_with_paddle_off_goes_ready},
    {"PM19", pm19_apply_config_accepts_auto_paddle_mode},
    {"PM20", pm20_original_on_off_on_promotes_once_without_extra_polls},
    {"PM21", pm21_hold_with_scale_keeps_k1_closed_for_five_seconds},
    {"PM22", pm22_already_closed_rewrites_pin_if_gpio_dropped},
    {"PM23", pm23_weight_cut_does_not_reclose_while_paddle_held},
    {"PM24", pm24_no_scale_hard_limit_does_not_reclose_while_held},
    {"PM25", pm25_web_start_paddle_off_keeps_k1_closed_with_scale},
    {"PM26", pm26_blocked_no_scale_hold_does_not_close_k1},
    {"R01", r01_transient_disconnect_suspends_weight_control},
    {"R02", r02_stalled_scale_worker_suspends_until_validated},
    {"R03", r03_non_finite_weights_cannot_corrupt_state_or_offset},
    {"R04", r04_scale_commands_execute_once_and_report_results},
    {"R05", r05_regression_uses_last_ten_valid_samples},
    {"R06", r06_hard_timer_opens_circuit_without_control_loop},
    {"R07", r07_timing_remains_correct_across_millis_wrap},
    {"R08", r08_full_command_queue_forces_safe_manual_cycle},
    {"R09", r09_stop_is_not_retried_after_disconnect_before_execution},
    {"R09b", r09b_old_commands_are_discarded_after_reconnect_but_new_stop_runs},
    {"R10", r10_relay_cannot_close_when_hard_timer_cannot_arm},
    {"R11", r11_final_shot_analysis_updates_only_valid_offset},
    {"BBW01", bbw01_snapshots_and_isolated_finalization},
    {"BBW02", bbw02_freshness_reset_and_safety},
    {"BBW03", bbw03_shared_guard_parity},
    {"R12", r12_scale_worker_service_publishes_weight_and_detects_failure},
    {"R12e", r12e_rejected_heartbeat_preserves_connected_epoch},
    {"R12f", r12f_first_weight_of_a_new_link_carries_its_generation},
    {"R12d", r12d_command_does_not_drain_buffered_weight},
    {"R12b", r12b_discovery_clears_stale_connected_link_snapshot},
    {"R12c", r12c_connected_link_rssi_samples_and_clears},
    {"R12d", r12d_scale_signal_level_keeps_hysteresis_across_publishes},
    {"R13", r13_full_queue_prevents_stop_without_delaying_relay_open},
    {"R14", r14_invalid_runtime_configuration_is_transactionally_rejected},
    {"R15", r15_gptimer_opens_circuit_without_arduino_or_esp_timer_tasks},
    {"R15b", r15b_isr_trip_without_session_clears_close_marker},
    {"R15c", r15c_stack_overflow_hook_opens_circuit_then_aborts},
    {"R16", r16_timeout_during_arm_transaction_can_never_close_circuit},
    {"R17", r17_gptimer_arm_failure_prevents_relay_energization},
    {"R18", r18_watchdog_fault_opens_circuit_and_requests_safe_restart},
    {"R18b", r18b_lockout_mid_shot_finalizes_session},
    {"R19", r19_reset_during_close_reopens_without_recovery_lockout},
    {"R19b", r19b_panic_boot_is_ready_for_webui_and_next_circuit_cycle},
    {"R20", r20_three_unsafe_resets_are_latched_as_a_boot_loop},
    {"R20b", r20b_reset_history_keeps_reason_and_previous_uptime},
    {"R20c", r20c_reset_uptime_checkpoint_is_no_more_frequent_than_one_minute},
    {"R20d", r20d_clear_reset_history_keeps_current_reset_reason},
    {"R20e", r20e_uptime_forced_invalid_and_wrapped_checkpoints},
    {"R20f", r20f_reset_history_clear_waits_for_configuration_safe},
    {"R20g", r20g_boot_recovery_stops_replaying_after_a_crash_loop},
    {"R20h", r20h_boot_recovery_stack_reads_as_a_sentence},
    {"R21", r21_automatic_control_requires_fresh_weight},
    {"R22", r22_confirmed_implausible_weight_does_not_stop},
    {"R23", r23_maintenance_is_canceled_fail_open_by_physical_paddle},
    {"R23b", r23b_maintenance_waits_for_shot_store_persist},
    {"R23d", r23d_restart_waits_for_unpersisted_shot_records},
    {"R23c", r23c_micra_settings_save_without_a_lease},
    {"R24", r24_web_control_is_available_without_session_owner},
    {"R25", r25_critical_scale_mailbox_never_blocks_and_keeps_latest},
    {"RACE01", race01_idle_tare_validates_after_command_spacing},
    {"RACE02", race02_retare_result_preserves_post_reference_weights},
    {"RACE03", race03_ended_cycle_cancels_queued_and_paced_retare},
    {"RACE04", race04_preferred_ack_preserves_newer_history},
    {"RACE05", race05_shutdown_during_spacing_blocks_all_operational_writes},
    {"RACE06", race06_connection_cleanup_preserves_new_shutdown},
    {"RACE07", race07_result_and_weight_selection_is_atomic},
    {"R25b", r25b_empty_mailboxes_take_one_critical_lock},
    {"R25c", r25c_refilled_critical_mailboxes_remain_live_and_bounded},
    {"R25d", r25d_reference_change_survives_full_critical_queue},
    {"R26", r26_remote_timer_stop_retries_after_full_queue},
    {"R27", r27_platform_clock_failure_prevents_circuit_close},
    {"R28", r28_terminal_control_result_is_retained_until_forwarded},
    {"R29", r29_direct_threshold_stops_before_regression_is_ready},
    {"R30", r30_first_abrupt_sample_uses_pre_shot_baseline},
    {"R31", r31_confirmed_overload_opens_without_learning},
    {"R48", r48_guard_disabled_stops_at_target_normally},
    {"R49", r49_guard_extends_and_stops_at_max_weight},
    {"R51", r51_extended_pulse_respects_alert_flag_and_scale_only},
    {"R51", r51_auto_to_manual_guard_fires_while_scale_lost},
    {"R52", r52_auto_to_manual_guard_clears_on_scale_recovery},
    {"R53", r53_auto_to_manual_guard_disabled_does_not_cut_early},
    {"R57", r57_guard_max_weight_cut_from_predicted_time},
    {"R58", r58_extended_shot_does_not_learn_weight_offset},
    {"R59", r59_slow_guard_on_time_bbw_is_scale_threshold},
    {"R61b", r61b_slow_extended_pulse_uses_slow_rate_setting},
    {"TT01", tt01_smooth_cut_truth_table},
    {"EQ01", eq01_live_cut_equals_kernel_replay},
    {"CH01", ch01_channeling_caps_the_cut_line_at_the_target},
    {"SL01", sl01_scale_loss_regimes},
    {"SL02", sl02_new_coffee_restarts_the_statistics_epoch},
    {"R61c", r61c_extended_pulse_resumes_after_scale_lost_alert},
    {"R61d", r61d_extended_pulse_pending_cleared_when_unwanted},
    {"R61e", r61e_extended_pulse_restarts_when_rate_setting_changes},
    {"R63", r63_slow_guard_disabled_continues_past_max_time},
    {"FF01", ff01_classifier_seeking_touch_and_release},
    {"AT01", at01_classifier_startup_and_trend_math},
    {"AT02", at02_early_spike_skips_control_trajectory},
    {"AT03", at03_late_spike_does_not_cut_then_recovers},
    {"AT04", at04_smooth_approach_cuts_on_two_samples},
    {"AT05", at05_spike_before_min_bbw_brew_time_does_not_enter_fast_extended},
    {"AT06", at06_fast_extended_spike_does_not_cut_until_sustained},
    {"AT07", at07_feature_off_matches_legacy_threshold_cut},
    {"AT08", at08_retare_and_cup_remove_still_work_with_touch_guard},
    {"AT09", at09_slew_recovery_advances_control_weight},
    {"AT11", at11_pre_cycle_anchor_does_not_treat_first_pour_as_touch},
    {"FF02", ff02_chorrito_fires_on_second_sample},
    {"FF03", ff03_finger_hold_does_not_fire},
    {"FF04", ff04_finger_release_to_zero_does_not_fire},
    {"FF05", ff05_bounce_through_residual_does_not_fire},
    {"FF06", ff06_touch_during_drops_fires_on_residual},
    {"FF07", ff07_gush_then_flow_backdates_to_jump},
    {"FF08", ff08_coffee_during_post_tare_grace_fires},
    {"FF09", ff09_stream_past_half_goal_still_fires},
    {"FF10", ff10_control_ramp_records_first_flow},
    {"FF11", ff11_cup_and_finger_are_not_first_drop},
    {"FF12", ff12_cupmin_parameter_gates_touch_and_residual},
    {"FF17", ff17_noise_adaptive_threshold_and_rate_gate},
    {"FF18", ff18_duplicate_timestamp_pairs},
    {"FF19", ff19_foreign_generation_cannot_latch_first_drop},
    {"FF20", ff20_seeking_fa_telemetry_args_and_rate_limit},
    {"FF21", ff21_first_flow_hardening},
    {"FF22", ff22_small_cup_min_gush_fires_through_touch},
    {"AT16", at16_resting_finger_contained_by_direct_stop_law},
    {"AT17", at17_heavy_touch_release_recovers_control},
    {"AT18", at18_heavy_touch_is_held_then_fallback},
    {"AT19", at19_late_cup_step_held},
    {"AT20", at20_resting_load_under_flow_stays_held},
    {"AT21", at21_spurt_plateau_is_accepted_without_learning},
    {"AT22", at22_held_touch_below_line_releases_after_bounded_hold},
    {"AT23", at23_hand_contact_is_absorbed_at_low_and_high_cup_weight},
    {"AT24", at24_ramped_contact_and_lift_are_held},
    {"AT25", at25_real_spurt_is_accepted_as_flow},
    {"AT26", at26_first_drop_onset_is_flow_not_touch},
    {"TR01", tr01_failed_start_tare_uses_software_zero},
    {"TR02", tr02_late_zero_after_grace_is_ignored},
    {"TR03", tr03_failed_start_tare_cup_lift_stops},
    {"TR04", tr04_light_cup_pre_effect_packet_recovers},
    {"TR05", tr05_light_cup_pre_effect_packet_is_not_a_baseline},
    {"SK01", sk01_refused_start_releases_cup_hold},
    {"SK02", sk02_overload_stops_while_touch_is_held},
    {"SK03", sk03_late_zero_after_grace_releases_cup_hold},
    {"SP01", sp01_phi_table_matches_erf},
    {"SP02", sp02_isqrt_exact_on_squares},
    {"SP03", sp03_plateau_flow_median},
    {"SP04", sp04_forecast_constant_rate_matches_analytic},
    {"SP05", sp05_quadratic_matches_reference},
    {"SP06", sp06_sigma_finite_at_extremes},
    {"SP07", sp07_green_replay_reaches_full_on_recipe},
    {"SP08", sp08_stall_without_history_stays_neutral},
    {"SP09", sp09_transient_spike_does_not_flip_fast},
    {"SP10", sp10_blend_monotone_within_class},
    {"SP11", sp11_disturbance_guard_fades_to_neutral},
    {"FW01", fw01_median_final_weight_filters_dip},
    {"FW02", fw02_offset_learns_below_sensor_lag},
    {"FW03", fw03_dense_tail_decimates_to_reachable_median},
    {"FF13", ff13_rinse_first_drop_goes_through_stopper},
    {"FF14", ff14_no_scale_first_drop_does_not_skip_stopper},
    {"FF15", ff15_orchestrate_post_tare_holds_cup_transitions},
    {"FF16", ff16_packet_gap_restarts_confirmation},
    {"R65", r65_non_weight_cut_does_not_learn_weight_offset},
    {"R32", r32_old_connection_generation_cannot_update_weight},
    {"R33", r33_weight_mailbox_keeps_latest_without_consumer_gap},
    {"R33b", r33b_stream_gap_counts_connected_inter_packet_silence},
    {"R33c", r33c_weight_update_interval_is_smoothed_and_reset_safely},
    {"R34", r34_suspended_control_recovers_after_three_attributed_samples},
    {"R35", r35_connected_without_weight_stream_is_not_available},
    {"R36", r36_recovered_stale_metrics_count_connected_gaps_only},
    {"R41", r41_negative_weight_in_range_starts_automatic_cycle},
    {"CP01", cp01_zero_pre_tare_weight_blocks_brew},
    {"CP02", cp02_negative_pre_tare_weight_blocks_brew},
    {"CP03", cp03_positive_pre_tare_weight_starts_brew},
    {"CP04", cp04_require_cup_off_allows_zero_start},
    {"CP05", cp05_rinse_without_cup_still_starts},
    {"CP06", cp06_cup_removed_stops_during_bbw_protection},
    {"CP07", cp07_zero_after_tare_does_not_stop},
    {"CP08", cp08_stop_if_cup_removed_off_keeps_negative_weight},
    {"CP09", cp09_blocked_cup_start_is_silent_when_alerts_off},
    {"CP10", cp10_web_paddle_blocks_cup_start_with_double_beep},
    {"CP11", cp11_master_off_allows_zero_pre_tare_start},
    {"CP12", cp12_master_off_does_not_stop_on_negative_weight},
    {"CP13", cp13_pre_tare_negative_packets_do_not_abort},
    {"CP14", cp14_post_tare_noise_does_not_stop},
    {"CP15", cp15_timer_only_ignores_negative_weight},
    {"CP16", cp16_small_negative_noise_does_not_stop},
    {"CP17", cp17_weight_at_removed_threshold_stops},
    {"CP18", cp18_custom_removed_threshold_is_honored},
    {"CP19", cp19_weight_below_present_threshold_blocks_brew},
    {"CP20", cp20_weight_at_present_threshold_starts_brew},
    {"CP21", cp21_custom_present_threshold_is_honored},
    {"CP22", cp22_zeroed_cup_lift_stops_without_placement},
    {"CP23", cp23_empty_pan_noise_never_removes},
    {"CP24", cp24_cup_tilt_does_not_abort},
    {"CP25", cp25_full_lift_stops_fast},
    {"CP26", cp26_unknown_mass_keeps_legacy_depth},
    {"CP27", cp27_slow_creep_below_removed_is_not_a_lift},
    {"CF01", cup_fsm_boot_zero_is_absent},
    {"CF02", cup_fsm_stable_min_cup_is_present},
    {"CF03", cup_fsm_noise_does_not_remove},
    {"CF04", cup_fsm_spike_does_not_place},
    {"CF05", cup_fsm_tare_does_not_change_state},
    {"CF10", cup_fsm_tared_zero_survives_guarded_start},
    {"IT01", it01_placement_tares_once_independently},
    {"CW01", cw01_idle_mass_survives_coffee_and_snapshot_copy},
    {"CW02", cw02_start_outcome_and_later_tare_preserve_mass},
    {"CW03", cw03_removal_replacement_and_connection_invalidate},
    {"CW04", cw04_invalid_stale_and_discontinuous_evidence_cannot_reappear},
    {"CW05", cw05_pending_identity_failure_and_timeout},
    {"CW06", cw06_no_capture_from_missing_or_invalid_pre_tare_sample},
    {"CW07", cw07_late_retare_and_disabled_automation},
    {"CW08", cw08_pending_timeout_and_request_wrap},
    {"CW09", cw09_debug_tare_invalidates_without_changing_presence},
    {"CW10", cw10_absence_baseline_requires_stability_and_continuity},
    {"CW11", cw11_removal_transient_is_not_the_empty_baseline},
    {"CW12", cw12_placement_ramp_preserves_stable_absence},
    {"CW13", cw13_negative_boot_baseline_requires_known_empty_reference},
    {"CW14", cw14_heavy_tare_offsets_preserve_replacement_mass},
    {"CW15", cw15_removal_rebound_cannot_place_or_tare_empty_pan},
    {"CW16", cw16_fast_replacement_reuses_known_empty_anchor},
    {"CW27", cw27_queued_tare_drift_does_not_accumulate},
    {"CW28", cw28_observed_unload_preserves_previous_shot},
    {"CW29", cw29_all_tare_paths_capture_latest_approved_load},
    {"CW30", cw30_fast_unload_requires_unbroken_evidence},
    {"CW31", cw31_fast_unload_band_and_minimum_mass_are_independent},
    {"CW32", cw32_fast_unload_after_partial_removal_and_wrap},
    {"CW17", cw17_debug_tare_rebases_negative_replacement},
    {"CW18", cw18_debug_tare_cancels_queued_false_placement},
    {"CW19", cw19_stale_active_samples_cannot_retare},
    {"CW20", cw20_rejected_cup_samples_break_stability},
    {"CW21", cw21_debug_failure_and_old_connection_cannot_certify_reference},
    {"CW22", cw22_empty_movement_cannot_place_or_retare},
    {"CW23", cw23_invalid_numeric_samples_break_confirmations},
    {"CW24", cw24_minimum_mass_is_independent_of_stability_tolerance},
    {"CW25", cw25_lost_samples_cannot_learn_a_negative_empty_reference},
    {"CW26", cw26_stable_removal_undershoot_becomes_empty_reference},
    {"IT02", it02_idle_zero_allows_guarded_start},
    {"IT03", it03_shot_end_never_tares_remaining_cup},
    {"IT04", it04_reconnect_and_setting_changes_do_not_place},
    {"IT05", it05_queued_tare_is_preempted_by_same_start_or_removal},
    {"IT06", it06_writing_and_late_idle_results_cannot_enter_brew},
    {"IT07", it07_expiry_failure_and_removal_during_write},
    {"IT08", it08_stability_and_ineligible_placement},
    {"IT09", it09_invalid_samples_queue_failure_and_disconnect},
    {"IT10", it10_worker_claim_and_cancel_are_mutually_exclusive},
    {"IT11", it11_disabling_after_write_keeps_tared_cup},
    {"IT12", it12_lighter_replacement_keeps_queued_tare},
    {"IT13", it13_lighter_replacement_write_zero_is_not_another_placement},
    {"IT14", it14_accepted_heavy_cup_removal_rearms_tare},
    {"IT15", it15_batched_removal_samples_reach_cup_detector},
    {"IT16", it16_prewrite_removal_cannot_become_tare_zero},
    {"IT17", it17_transport_success_without_zero_preserves_old_reference},
    {"IT18", it18_placement_tolerance_bounds_entire_window},
    {"IT19", it19_changed_queued_placement_cannot_tare},
    {"IT20", it20_weight_overflow_breaks_removal_evidence},
    {"IT21", it21_batched_sign_reversal_does_not_remove_cup},
    {"IT22", it22_worker_preserves_capture_time_and_zero_boundary},
    {"IT23", it23_unknown_effect_releases_slot_but_not_cup_guard},
    {"IT24", it24_claim_waits_for_control_validation_of_published_sample},
    {"IT25", it25_capture_and_packet_wrap_preserve_order},
    {"IT26", it26_debug_uses_committed_cup_snapshot_and_terminal_reason},
    {"IT27", it27_late_cup_retare_rejects_accumulated_drift},
    {"IT28", it28_weight_handoff_concurrent_publication_is_bounded},
    {"IT29", it29_old_or_future_captured_weight_cannot_qualify},
    {"IT30", it30_actual_negative_replacement_removal_during_write},
    {"IT31", it31_idle_request_and_effect_deadline_cross_clock_wrap},
    {"IT32", it32_queued_minimum_is_independent_of_tolerance},
    {"IT33", it33_stale_connection_gap_cannot_cancel_current_tare},
    {"IT33B", it33b_pre_disconnect_weight_cannot_cross_disconnect_epoch},
    {"IT33C", it33c_pre_disconnect_command_result_cannot_cross_disconnect_epoch},
    {"IT33D", it33d_pre_cycle_weight_warning_identifies_timestamps},
    {"IT34", it34_final_prewrite_sample_requires_control_approval},
    {"IT35", it35_loaded_startup_and_sample_gap_need_fresh_placement},
    {"IT36", it36_bookoo_startup_zero_unload_rearms_relative_tare},
    {"IT37", it37_accessory_retare_is_opt_in_and_once_before_shot},
    {"IT38", it38_accessory_retare_requires_continuous_confirmed_placement},
    {"IT39", it39_known_accessory_removal_retares_without_losing_cup},
    {"IT40", it40_accessory_unload_sequences_keep_next_placement_eligible},
    {"IT41", it41_combined_placement_partial_unload_recovers_at_empty_pan},
    {"IT42", it42_accessory_can_be_readded_after_known_removal},
    {"IT43", it43_queued_accessory_removal_survives_fresh_negative_samples},
    {"IT44", it44_interrupted_removal_recovers_after_empty_pan},
    {"IT45", it45_failed_accessory_removal_does_not_retry_on_unknown_zero},
    {"SP01", sp01_profile_explains_idle_cup_tare_and_removal},
    {"SP02", sp02_profile_captures_timer_only_timeout_and_mid_request_start},
    {"SP03", sp03_profile_explains_settling_failure_and_config_changes},
    {"IT46", it46_accessory_removal_never_uses_idle_tare_during_shot},
    {"IT47", it47_stale_or_disabled_accessory_evidence_never_tares_removal},
    {"IT48", it48_gradual_accessory_or_cup_unload_waits_for_settled_mass},
    {"IT49", it49_cancelled_accessory_removal_rearms_after_return_to_zero},
    {"IT50", it50_cancelled_accessory_addition_rearms_after_return_to_zero},
    {"IT51", it51_failed_accessory_write_does_not_rearm_from_zero},
    {"IT52", it52_small_empty_offset_after_moving_bookoo_rearms_idle_tare},
    {"IT53", it53_unknown_empty_rejects_replacement_and_ingredient},
    {"IT54", it54_unknown_empty_recovers_only_with_qualified_pan},
    {"IT55", it55_unknown_empty_rebound_obeys_minimum_cup_boundary},
    {"IT56", it56_transient_negative_movement_recovers_idle_readiness},
    {"IT57", it57_repeated_zero_recovery_requires_configured_stability},
    {"IT58", it58_cup_before_zero_recovery_reuses_previous_qualified_zero},
    {"IT59", it59_ambiguous_empty_guidance_is_idle_only},
    {"IT60", it60_mixed_movement_recovers_small_zero_offsets_without_delay},
    {"IT61", it61_unqualified_startup_placement_cannot_lock_zero_recovery},
    {"IT62", it62_small_empty_shifts_cannot_accumulate_away_from_zero},
    {"IT63", it63_initial_recovery_respects_stability_and_zero_boundaries},
    {"IT64", it64_bookoo_handling_profile_tares_first_stable_cup},
    {"IT65", it65_profile5_handling_tares_first_physical_cup},
    {"IT66", it66_initial_positive_return_requires_known_zero_and_subminimum_load},
    {"IT67", it67_offanchor_empty_plateau_requalifies_after_removal},
    {"IT68", it68_plateau_above_anchor_plus_mincup_still_waits},
    {"IT69", it69_button_zero_on_empty_pan_rearms_idle_tare},
    {"IT70", it70_equal_weight_swap_at_zero_shows_absent},
    {"IT71", it71_tare_with_coffee_then_lift_recovers},
    {"IT72", it72_accessory_ramp_is_not_tared},
    {"IT73", it73_off_anchor_wait_age_and_profiler_event},
    {"IT74", it74_accessory_landing_overshoot_is_tared},
    {"IT75", it75_accessory_pour_shapes_are_not_tared},
    {"IT76", it76_unstable_accessory_request_retries},
    {"CF06", cup_fsm_put_back_without_tare_is_present},
    {"CF07", cup_fsm_disconnect_does_not_emit_removed},
    {"CF08", cup_fsm_rinse_does_not_freeze_presence},
    {"CF09", cup_fsm_untared_lift_to_zero_is_removed},
    {"R42", r42_weight_below_automation_min_stays_manual},
    {"R43", r43_post_tare_baseline_accepts_zero_after_pre_tare_weight},
    {"R54", r54_post_tare_baseline_keeps_weight_control},
    {"R66", r66_post_tare_rejected_stream_does_not_enforce_atm},
    {"R67", r67_observed_silence_enforces_atm},
    {"R68", r68_stable_accepted_weight_does_not_suspend},
    {"R69", r69_att_command_harvests_pending_weight},
    {"R44", r44_first_shot_after_reconnect_enters_brew},
    {"R45", r45_slew_rejection_emits_specific_debug_code},
    {"ST01", st01_cycle_elapsed_follows_circuit_immediately},
    {"ST02", st02_scale_timer_stop_waits_until_display_catches_internal},
    {"ST03", st03_scale_timer_stop_when_display_already_at_internal},
    {"ST04", st04_scale_timer_stop_extra_delay_applies_after_catchup},
    {"ST05", st05_scale_timer_stop_catchup_times_out},
    {"ST06", st06_scale_timer_stop_without_valid_timer_is_immediate},
    {"ST07", st07_scale_timer_stop_waits_for_start_before_queueing_stop},
    {"ST08", st08_scale_timer_stop_extra_delay_applies_without_valid_timer},
    {"RT01", rt01_late_cup_triggers_single_retare},
    {"SHOT_SCALE", shot_scale_name_snapshot_survives_link_changes},
    {"SHOT_SNAP", shot_record_snapshots_target_temp_dose_and_ratio},
    {"SHOT_TARE", shot_tare_time_uses_successful_write_and_circuit_clock},
    {"RT02", rt02_sub_minimum_stable_cup_is_ignored},
    {"RT03", rt03_spike_without_stable_cup_does_not_retare},
    {"RT04", rt04_heavy_cup_does_not_stop_during_retare},
    {"RT05", rt05_bbw_protection_timeout_enables_stop_without_beep},
    {"RT06", rt06_first_drops_beep_after_bbw_protection_timeout},
    {"RT07", rt07_auto_retare_off_skips_retare_window},
    {"RT09", rt09_coffee_during_retare_beep_on_first_drop_not_at_retare_end},
    {"RT10", rt10_first_drops_beep_during_bbw_protection},
    {"RT11", rt11_late_retare_records_first_drops_and_keeps_weight_control},
    {"RT12", rt12_early_retare_then_first_drops_are_recorded},
    {"RT13", rt13_auto_tare_off_skips_automatic_retare},
    {"RT14", rt14_cup_overshoot_still_retares},
    {"RT15", rt15_cup_settle_creep_still_retares},
    {"RT16", rt16_configured_min_cup_weight_retares},
    {"RT17", rt17_light_cup_landing_and_overshoot_retares},
    {"RT18", rt18_late_combined_start_does_not_rearm_after_retare},
    {"RT19", rt19_late_cup_then_zero_lift_allows_next_shot_retare},
    {"RT20", rt20_late_combined_start_does_not_rearm_when_cup_absent},
    {"RT22", rt22_inshot_coffee_ramp_is_not_a_late_cup},
    {"RT23", rt23_late_cup_before_first_drop_retares},
    {"RT24", rt24_late_cup_step_uses_shot_coordinates},
    {"FW05", fw05_set_drip_quick_starts_and_three_shots_decide},
    {"FW06", fw06_status_carries_recent_drips_and_markers},
    {"FW07", fw07_quick_start_save_retries_and_reports_once},
    {"RT21", rt21_empty_pan_at_cycle_start_resyncs_stuck_present},
    {"RS01", rs01_fast_samples_wait_for_min_duration},
    {"RS02", rs02_slow_samples_meet_min_duration_at_third_sample},
    {"RS03", rs03_broken_streak_before_min_duration_does_not_retare},
    {"RS04", rs04_zero_min_duration_retares_on_sample_count_only},
    {"RS05", rs05_coffee_during_min_duration_wait_allows_retare},
    {"RS06", rs06_late_retare_corrects_only_confirmed_first_drop},
    {"RS07", rs07_late_retare_preserves_prerequisites_and_deadline},
    {"RS08", rs08_first_drop_while_retare_pending_keeps_stop_protections},
    {"RS09", rs09_retained_completion_after_reconnect_confirms_zero},
    {"R46", r46_range_rejection_emits_specific_debug_code},
    {"R47", r47_reset_reason_name_maps_known_codes},
    {"W01", w01_default_runtime_configuration_is_valid},
    {"W02", w02_each_runtime_field_is_validated},
    {"W03", w03_runtime_timing_relations_are_transactional},
    {"W04", w04_wifi_credentials_have_strict_bounds},
    {"W04b", w04b_select_best_sta_ap_prefers_strongest_matching_bssid},
    {"W04c", w04c_setup_onboarding_phase_scenarios},
    {"W05", w05_config_is_blocked_while_brewing_early},
    {"W06", w06_config_is_blocked_while_brewing},
    {"W07", w07_config_is_blocked_while_rinsing},
    {"W08", w08_config_is_blocked_during_manual_cycle},
    {"W09", w09_valid_config_applies_only_from_ready},
    {"W10", w10_cycle_configuration_snapshot_is_immutable},
    {"W11", w11_operational_timer_opens_without_control_loop},
    {"W11b", w11b_timer_only_natural_skips_operational_wall},
    {"W11c", w11c_timer_only_original_skips_operational_wall},
    {"W11d", w11d_timer_only_auto_skips_operational_wall},
    {"W12", w12_hard_limit_cannot_be_configured_above_sixty_seconds},
    {"W45", w45_bbw_protection_retare_relation_is_validated},
    {"W46", w46_status_reports_uptime_since_boot},
    {"W47", w47_status_reports_live_scale_weight_and_timer},
    {"W13", w13_virtual_paddle_uses_normal_state_machine},
    {"W14", w14_physical_motion_overrides_web_control},
    {"W15", w15_web_rinse_starts_scale_timer},
    {"W16", w16_web_stop_during_rinse_preserves_rearm},
    {"W17", w17_web_session_stop_is_a_safe_stop},
    {"W18", w18_web_stop_can_end_a_physical_brew_only_by_opening},
    {"W19", w19_web_start_is_rejected_outside_ready},
    {"W20", w20_restart_waits_while_active},
    {"W20b", w20b_planned_esp_restart_waits_for_shot},
    {"W20c", w20c_scheduled_restart_uses_the_planned_restart_lease},
    {"W21", w21_network_change_is_rejected_while_active},
    {"W21b", w21b_override_maintenance_mid_shot_records_the_shot},
    {"W22", w22_timer_only_disables_predictive_stop},
    {"W23", w23_combined_tare_command_uses_cycle_snapshot},
    {"W24", w24_debug_ring_is_bounded_and_ordered},
    {"W25", w25_weight_samples_do_not_fill_debug_log},
    {"W25B", w25b_log_levels_and_cycle_events_reach_ring},
    {"W25C", w25c_ring_retain_none_skips_ring_and_error_filters},
    {"W25D", w25d_ble_and_scale_lines_use_their_categories},
    {"W26", w26_status_is_a_copied_snapshot},
    {"W27", w27_stale_weight_is_not_presented_as_current},
    {"W28", w28_web_command_queue_is_bounded},
    {"W29", w29_operational_wall_of_60001_is_rejected},
    {"W30", w30_last_cycle_weight_must_belong_to_that_cycle},
    {"W31", w31_unsupported_scale_never_uses_tare_as_a_beep},
    {"W32", w32_full_scale_queue_cannot_block_brew_start},
    {"W33", w33_first_drop_beep_can_be_disabled},
    {"W34", w34_calibration_reset_restores_baseline_and_cancels_analysis},
    {"W35", w35_status_reports_the_live_physical_paddle_gpio},
    {"W36", w36_paddle_return_reminder_beeps_at_configured_interval_only_while_open},
    {"W37", w37_factory_reset_is_rejected_while_control_is_active},
    {"W39", w39_history_mutation_blocked_while_brew_rf_active},
    {"W38", w38_scale_connected_led_tracks_link_and_setting},
    {"W44", w44_paddle_return_reminder_stops_after_fifteen_minutes},
    {"W50", w50_local_buzzer_plays_triple_pattern_non_blocking},
    {"W50b", w50b_buzzer_phase_timer_holds_triple_rhythm_without_loop},
    {"W50f", w50f_buzzer_timer_failures_drop_audio_without_affecting_relay},
    {"W50e", w50e_buzzer_phase_timer_holds_double_as_truncated_triple},
    {"W50c", w50c_buzzer_phase_timer_advances_despite_loop_stall},
    {"W50d", w50d_recovery_buzzer_patterns_have_exact_timings},
    {"W51", w51_local_buzzer_echo_inverted_on_scale_lost_during_bbw},
    {"W51b", w51b_scale_lost_echo_inverted_when_idle},
    {"W51c", w51c_scale_lost_silent_when_flag_off},
    {"W52", w52_local_buzzer_triple_on_manual_bbw_without_scale},
    {"W53", w53_local_buzzer_silent_when_bbw_off_without_scale},
    {"NS01", ns01_armed_blocks_first_bbw_no_scale_shot},
    {"NS02", ns02_idle_allows_second_bbw_no_scale_shot},
    {"NS03", ns03_bbw_off_does_not_block},
    {"NS04", ns04_scale_available_rearms},
    {"NS05", ns05_cooldown_rearms_from_idle},
    {"NS06", ns06_finished_shot_extends_cooldown},
    {"NS07", ns07_web_rinse_consumes_guard},
    {"NS07B", ns07b_web_rinse_allowed_while_armed_runs},
    {"NS08", ns08_blocked_beep_respects_alert_checkbox},
    {"NS09", ns09_armed_rinse_gesture_consumes_guard},
    {"NS09B", ns09b_allow_rinse_while_armed_runs},
    {"NS10", ns10_idle_rinse_gesture_does_not_rearm},
    {"NS11", ns11_web_rinse_with_scale_keeps_armed},
    {"NS12", ns12_failed_rinse_does_not_consume_guard},
    {"NS13", ns13_require_scale_blocks_repeated_starts},
    {"NS14", ns14_require_scale_blocks_rinse},
    {"NS15", ns15_require_scale_is_inactive_when_bbw_off},
    {"NS16", ns16_require_scale_reconnect_while_held_needs_new_gesture},
    {"NS17", ns17_require_scale_triple_cycle_temporarily_allows},
    {"NS17b", ns17b_require_scale_double_cycle_stays_blocked},
    {"NS18", ns18_require_scale_triple_cycle_must_fit_window},
    {"W54", w54_local_buzzer_triple_on_auto_to_manual_guard_end},
    {"W55", w55_local_buzzer_queues_second_triple_while_busy},
    {"W56", w56_atm_beep_queued_when_scale_lost_after_deadline},
    {"W57", w57_paddle_return_reminder_falls_back_to_scale_when_piezo_not_ready},
    {"W58", w58_paddle_return_reminder_does_not_advance_interval_when_muted},
    {"W59", w59_local_buzzer_plays_short_long_and_double_patterns},
    {"W60", w60_web_buzzer_test_plays_requested_pattern},
    {"W61", w61_web_buzzer_test_rejected_while_active},
    {"W62", w62_local_buzzer_drive_matches_compile_flag},
    {"W99", w99_rtttl_parser_decodes_notes_and_rests},
    {"W99b", w99b_passive_cue_drives_ledc_note_frequency},
    {"W99c", w99c_select_alert_sink_preserves_channel_rules},
    {"W99d", w99d_pending_rtttl_starts_from_preparsed_catalog},
    {"W99e", w99e_recovery_cue_bypasses_mute_via_pipeline},
    {"W99f", w99f_bullseye_rtttl_is_bounded_and_plays_without_allocation},
    {"W99g", w99g_bullseye_requires_one_second_of_exact_fresh_samples},
    {"W99h", w99h_bullseye_service_runs_only_in_buzzer_only_mode},
    {"W99i", w99i_bullseye_test_never_queues_mutable_custom_notes},
    {"W63", w63_scale_priority_paddle_uses_scale_when_connected},
    {"W64", w64_buzzer_only_first_drop_uses_local_buzzer},
    {"W65", w65_scale_only_mutes_scale_lost},
    {"W66", w66_web_bookoo_debug_dispatches_actions},
    {"W67", w67_web_bookoo_debug_rejected_while_active},
    {"W68", w68_web_bookoo_debug_rejected_when_disconnected},
    {"W69", w69_web_bookoo_debug_rejected_when_not_generic},
    {"W70", w70_bookoo_connect_mutes_in_buzzer_only},
    {"W71", w71_bookoo_connect_sets_volume_in_scale_priority},
    {"W72", w72_bookoo_connect_mutes_disabled_volume_and_skips_non_bookoo},
    {"W73", w73_apply_config_buzzer_only_sends_bookoo_silence},
    {"W74", w74_apply_config_enabling_mute_sends_silence_only_in_buzzer_only},
    {"W74b", w74b_sound_alert_master_mutes_and_cancels_all_routes},
    {"W74c", w74c_unrelated_config_save_does_not_repeat_live_effects},
    {"W74d", w74d_settings_dispatches_each_matching_subscriber_once},
    {"W75", w75_bookoo_discovery_connect_applies_beep_policy},
    {"W75b", w75b_old_debug_volume_and_beeps_do_not_cross_connections},
    {"W76", w76_buzzer_only_start_beeps_at_circuit_not_ble_result},
    {"W77", w77_scale_priority_disconnected_beeps_on_circuit_without_ble},
    {"W78", w78_scale_priority_connected_start_beeps_at_circuit},
    {"W79", w79_buzzer_only_stop_beeps_before_timer_stop_result},
    {"W79b", w79b_stop_beeps_at_circuit_while_scale_timer_stop_still_pending},
    {"W79c", w79c_rinse_without_scale_beeps_at_circuit_open_and_close},
    {"W79d", w79d_scale_only_start_and_stop_still_use_local_circuit_beeps},
    {"W80", w80_buzzer_only_retare_beeps_before_tare_result},
    {"W81", w81_scale_priority_failed_start_falls_back_after_disconnect},
    {"W94", w94_eclair_scale_priority_uses_local_alerts},
    {"W95", w95_eclair_scale_only_omits_unsupported_alerts},
    {"W97", w97_eclair_buzzer_only_never_queues_scale_beeps},
    {"W82", w82_pulse_train_loops_until_deadline_or_stopIf},
    {"W83", w83_web_buzzer_test_pulse_uses_same_train_for_3s},
    {"W84", w84_pulse_train_yields_to_triple},
    {"W84b", w84b_echo_inverted_upgrades_weak_pending},
    {"W85", w85_debug_pulse_rates_use_same_on_ms_and_3s},
    {"W86", w86_config_applies_to_ram_immediately_and_coalesces},
    {"TZ01", tz01_first_auto_zone_is_once_and_estimated_after_sync},
    {"TZ02", tz02_unknown_zone_preserves_utc_with_zero_offset},
    {"W87", w87_nvs_fail_keeps_ram_and_requeues},
    {"W87B", w87b_runtime_persist_failure_is_logged_once_per_episode},
    {"W88", w88_save_network_flush_includes_live_runtime},
    {"W89", w89_restart_flush_is_best_effort},
    {"W91", w91_chime_sequence_uses_irregular_note_timings},
    {"W92", w92_parse_sequence_pattern_ids},
    {"W93", w93_scale_connected_echo_on_rising_edge},
    {"W93B", w93b_buzzer_phase_uses_current_time_and_wraps},
    {"W94", w94_scale_connected_silent_when_flag_off_or_scale_only},
    {"F01A", f01_link_side_effects_run_only_on_control},
    {"F01B", f01_worker_uses_only_published_policy},
    {"W95", w95_web_buzzer_test_plays_chime_sequence},
    {"W96", w96_echo_inverted_uses_long_bookend_tones},
    {"W98", w98_buzzer_sequences_start_and_end_with_sound},
    {"D01", d01_idle_scan_stays_enabled_between_ticks},
    {"D13", d13_idle_delays_relax_without_scale},
    {"D13B", d13b_scale_power_off_is_terminal_for_connection_generation},
    {"D13D", d13d_known_mini_rejects_shutdown_before_terminal_barrier},
    {"D13E", d13e_ultra_volume_rejection_preserves_saved_level},
    {"D13C", d13c_disconnect_silence_clears_connecting_snapshot},
    {"D15", d15_control_housekeeping_has_wrap_safe_10ms_cadence},
    {"BF01", bf01_confirmed_bounds_and_final_history},
    {"BF02", bf02_unconfirmed_loss_and_ordering},
    {"BF03", bf03_promotion_races_and_absolute_cap},
    {"BF04", bf04_deadline_boundaries_and_physical_release},
    {"BF05", bf05_no_adoption_reuse_or_false_history},
    {"BF06", bf06_normal_flow_matches_absent_backflush},
    {"BF07", bf07_configuration_retirement_opens_and_rearms_normally},
    {"D16", d16_cloud_pause_requires_a_real_cycle},
    {"D14", d14_control_status_publishes_on_cycle_edge},
    {"D02", d02_first_mode_uses_name_scan},
    {"D02b", d02b_only_without_preferred_bootstraps_and_adopts_on_connect},
    {"D02c", d02c_prefer_adopts_an_already_connected_scale},
    {"D02d", d02d_apply_only_without_preferred_is_accepted},
    {"D02e", d02e_mode_change_restarts_an_active_scan_with_the_new_filter},
    {"D02f", d02f_only_mode_disconnects_a_nonpreferred_live_scale},
    {"D02g", d02g_clear_preferred_disconnects_on_the_ble_worker},
    {"D03", d03_scan_start_failed_retries_immediately},
    {"D04", d04_full_cache_keeps_directed_scan},
    {"D05", d05_hci_watchdog_force_restarts_same_filter},
    {"D05b", d05b_scan_intensity_change_restarts_gap},
    {"D05c", d05c_scan_intensity_hci_presets},
    {"D16", d16_scale_connect_debug_reports_phases},
    {"D06", d06_forget_pauses_discovery_for_30s},
    {"D07", d07_prefer_falls_back_after_grace},
    {"D08", d08_select_none_clears_without_pause},
    {"D09", d09_first_mode_connects_seen_advertisement},
    {"D12", d12_advertisement_history_does_not_dirty_persist},
    {"D10c", d10c_softap_yields_discovery},
    {"D11", d11_select_preferred_is_noop_when_unchanged},
    {"D13", d13_scale_friendly_name_set_clear_and_preserve},
    {"D13a", d13a_auto_friendly_name_for_themis_models},
    {"D13b", d13b_friendly_names_are_per_scale},
    {"S01", s01_shot_log_filters_short_and_rinse},
    {"S01c", s01c_mixed_shots_share_stats_and_home_authority},
    {"S01e", s01e_bbw_error_uses_its_own_ten_shots},
    {"S01f", s01f_curve_rollover_keeps_stats_windows_and_sorted_joins},
    {"S12g", s12g_worker_acknowledgement_rechecks_generation_under_lock},
    {"S12h", s12h_capped_curve_batch_keeps_generation_dirty_until_drained},
    {"S02k", s02k_curve_continuity_uses_control_rejections_and_fifo_provenance},
    {"S01d", s01d_manual_timer_and_limit_share_settled_finalize},
    {"S01b", s01b_shot_log_stop_detail_names_end_reasons},
    {"S02", s02_shot_log_appends_after_drip_delay},
    {"S02E", s02e_shot_log_appends_auto_bbw_after_drip_delay},
    {"S02F", s02f_shot_log_skips_sub_one_gram_weight},
    {"S02G", s02g_full_cycle_sub_one_gram_updates_last_shot_not_history},
    {"S02C", s02c_shot_curve_samples_on_half_second_grid_and_latches_slow},
    {"S02L", s02l_post_drip_curve_keeps_observations_and_rejection_gaps},
    {"S02D", s02d_shot_curve_latches_first_drop_fast_and_atm},
    {"S02H", s02h_fast_guard_keeps_sampling_and_settled_weight_replaces_endpoint},
    {"S02I", s02i_normal_and_slow_cuts_use_settled_curve_endpoint},
    {"S02B", s02b_drip_delay_is_snapshotted_and_honors_boundaries},
    {"S17", s17_new_cycle_commits_pending_log_as_last_known},
    {"W90", w90_save_unknown_preset_id_does_not_overwrite_active},
    {"W90b", w91_preset_persistence_serializes_and_survives_retry},
    {"W90c", w90c_preset_temperature_presence_and_learning_invalidation},
    {"W90d", w90d_partial_preset_save_writes_only_sent_fields},
    {"W87c", w87c_runtime_persist_io_backoff_is_bounded_and_resets},
    {"HQ01", hq01_quick_settings_are_session_only},
    {"HQ02", hq02_quick_settings_revert_after_duration},
    {"HQ03", hq03_quick_settings_patch_is_one_change},
    {"S03", s03_shot_log_clear_empties_records},
    {"S03B", s03b_good_shot_records_stats_and_history},
    {"S03C", s03c_short_activation_is_other_in_history_only},
    {"S03D", s03d_rinse_is_rinse_in_history_only},
    {"S03E", s03e_manual_over_protection_is_shot_in_history_only},
    {"S03F", s03f_abandoned_start_records_nothing},
    {"S03G", s03g_stats_threshold_follows_protection_parameter},
    {"S14", s14_last_shot_persists_manual_cycle},
    {"S14c", s14c_last_shot_keeps_no_scale_duration},
    {"S14b", s14b_rinse_does_not_overwrite_last_shot},
    {"S14d", s14d_web_stop_during_rinse_does_not_overwrite_last_shot},
    {"S15", s15_last_shot_persists_after_drip_when_eligible},
    {"S15b", s15b_cup_off_after_end_keeps_last_known_actual},
    {"S15c", s15c_last_shot_prefers_last_accepted_over_cup_off},
    {"S16", s16_last_shot_clear_empties_snapshot},
    {"S16b", s16b_factory_reset_hides_last_shot_on_status},
    {"S18", s18_last_shot_keeps_no_scale_guard_from_cycle},
    {"S20", s20_last_good_shot_advances_independently},
    {"S21", s21_controller_started_retries_after_a_shot_store_persist},
    {"S21", s21_ip_change_reports_webhook},
    {"S04", s04_shot_log_remove_by_id},
    {"S04c", s04c_delete_shot_record_removes_log_and_curve},
    {"S04d", s04d_delete_shot_record_ok_without_curve},
    {"S04e", s04e_delete_shot_record_is_ram_only_when_flash_is_busy},
    {"B01", b01_scale_worker_requires_ble_stack},
    {"B02", b02_setup_degrades_without_ble},
#if OPEN_BREW_BY_WEIGHT_ENABLE_JTAG == 1
    {"B03", b03_jtag_build_starts_serial_without_jumper},
#else
    {"B03", b03_usb_console_stays_off_without_jumper},
#endif
    {"B04", b04_usb_console_starts_when_jumper_held},
    {"M08", m08_recipe_copies_match_published_state},
    {"M09", m09_snapshot_mutexes_preserve_concurrent_invariants},
    {"F03", f03_safety_event_flags_preserve_consumed_requests},
    {"F04", f04_gptimer_state_serializes_stop_against_expiry},
    {"F04b", f04b_gptimer_callback_runs_unlocked_and_blocks_rearm},
    {"F05", f05_settings_persistence_init_is_transactional},
    {"F06", f06_boot_capability_policy_is_fail_closed},
    {"F06b", f06b_control_gate_derives_effective_boot_state},
    {"F13", f13_schedule_contract_and_snapshot_evidence_are_explicit},
    {"F13b", f13b_status_curve_publishes_live_samples_and_clears_tail},
    {"F14", f14_relay_timer_initialization_rolls_back_partial_handles},
    {"F17", f17_health_counters_are_atomic_and_monotonic},
    {"F18", f18_guard_early_rejection_matches_original_predicates},
    {"S04b", s04b_shot_log_page_slice},
    {"S04c", s04c_shot_list_filter_groups},
    {"S04f", s04f_shot_log_sort_date_and_rating},
    {"S06", s06_shot_log_local_sec_from_utc},
    {"S07", s07_shot_log_stores_fixed_wall_time},
    {"S08", s08_shot_log_without_sync_has_no_wall_time},
    {"S11", s11_shot_log_record_includes_preset_snapshot},
    {"S12", s12_shot_rating_pack_preserves_guards},
    {"S12b", s12b_shot_log_update_rating},
    {"S12c", s12c_finalize_links_exact_history_row},
    {"S12d", s12d_rate_last_shot_and_history},
    {"S12e", s12e_history_rating_wins_after_last_shot_save_failure_and_reboot},
    {"S12f", s12f_shot_store_snapshot_serializes_rating_and_finalize},
    {"S13", s13_persist_debug_messages_identify_origin},
    {"SH01", sh01_activation_store_mutex_serializes_append_read_flush},
    {"SH02", sh02_stats_snapshot_captures_one_store_generation},
    {"S19", s19_shot_store_persist_failure_logs_once_until_success},
    {"S19b", s19b_shot_store_io_backoff_resets_for_new_dirty_data},
    {"H01", h01_health_threshold_alerts_fire_once_per_crossing},
    {"H01b", h01b_health_heap_low_restarts_only_when_ready_and_sustained},
    {"H01c", h01c_health_zero_is_exhaustion_not_missing_telemetry},
    {"H02", h02_hwmon_cpu_load_uses_refreshed_idle_and_ema},
    {"H03", h03_task_profiler_start_stop_updates_snapshot},
    {"H04", h04_loop_phase_profiler_publishes_window_and_session_totals},
    {"H05", h05_loop_phase_profiler_pairs_cpu_with_wall_maxima},
    {"H06", h06_loop_phase_input_subphases_report_individually},
    {"H07", h07_mutex_wait_accounting_tracks_last_and_max},
    {"H08", h08_lock_wait_stats_flow_into_the_profiler_snapshot},
    {"H10", h10_newest_history_record_matches_first_desc_page_row},
    {"H11", h11_status_store_cache_generation_guard},
    {"H12", h12_persistence_fast_path_skips_clean_store_mutex},
    {"N01", n01_wall_clock_tracks_utc_from_anchor},
    {"N01b", n01b_wall_clock_survives_millis_wrap},
    {"N01d", n01d_wall_clock_keeps_time_through_failures_and_rebase},
    {"N01c", n01c_wall_clock_cancel_syncing_restores_anchor},
    {"N02", n02_ntp_hostname_validation},
    {"N03", n03_unsynced_retry_is_fifteen_seconds},
    {"RF01", rf01_coex_is_always_bt},
    {"N04", n04_brew_start_requests_ntp_when_unsynced},
    {"N05", n05_rinse_start_requests_ntp_when_unsynced},
    {"N06", n06_synced_clock_skips_activity_ntp_request},
    {"N07", n07_syncing_clock_skips_activity_ntp_request},
    {"N08", n08_web_rinse_requests_ntp_when_unsynced},
    {"SC01", sc01_hello_replies_how_are_you},
    {"SC01B", sc01b_heap_dumps_internal_free_blocks},
    {"SC02", sc02_factory_reset_rejected_while_active},
    {"SC03", sc03_set_wifi_queues_save_network},
    {"SC04", sc04_clear_shots_empties_log},
    {"SC05", sc05_serial_cli_parser_covers_supported_commands},
    {"SC06", sc06_serial_cli_feed_completes_on_crlf},
    {"SC07", sc07_reset_device_password_and_clear_wifi_queue},
    {"SC08", sc08_set_device_password_queues_change},
    {"SC09", sc09_serial_debug_toggles_without_ready},
    {"SC10", sc10_help_prints_one_line_per_command},
    {"SC11", sc11_reboot_queues_restart_when_ready},
    {"SC12", sc12_reboot_waits_while_active},
    {"SC13", sc13_debug_full_and_off_during_cycle},
    {"SC14", sc14_network_actions_queue_without_ready},
    {"SC15", sc15_status_printers_use_dump_views},
    {"SC16", sc16_debug_status_and_log_dump},
    {"BC05", bc05_ble_scan_intensity_applies_live_without_restart},
    {"BC06", bc06_micra_scan_override_selects_discovery_duty},
    {"BC07", bc07_ble_scan_relaxed_with_disabled_switch_is_api_valid},
    {"BC08", bc08_ble_scan_superseded_requests_all_report_persisted},
    {"BC10", bc10_ble_master_switch_quiesces_scale_link},
    {"BC11", bc11_ble_master_switch_command_persists_live_without_restart},
    {"CM01", cm01_compatibility_mirror_follows_activator},
    {"CM02", cm02_compatibility_has_no_duration_cutoff},
    {"CM03", cm03_compatibility_rejects_actuation_commands},
    {"CM04", cm04_compatibility_blocks_ble_enable_command},
    {"CM05", cm05_compatibility_mutes_alerts_except_recovery},
    {"CM06", cm06_compatibility_command_gate_scope},
    {"CM07", cm07_compatibility_boots_parked_with_activator_held},
    {"CM08", cm08_compatibility_mirror_holds_against_guard_drive_deny},
    {"CM09", cm09_compatibility_preserves_fault_and_readiness_interlocks},
};

}  // namespace

int main(int argc, char **argv) {
  for (const TestCase &test : testCases) {
    if (argc > 1 && strcmp(argv[1], test.id) != 0) {
      continue;
    }
    const int failuresBefore = failures;
    test.function();
    ++testsRun;
    std::cout << test.id << (failures == failuresBefore ? " PASS" : " FAIL")
              << "\n";
  }

  deleteHostResources();
  if (testsRun == 0) {
    std::cerr << "No matching test\n";
    return EXIT_FAILURE;
  }
  std::cout << testsRun << " tests, " << failures << " failures\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

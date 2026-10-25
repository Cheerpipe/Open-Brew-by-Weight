#define OPEN_BREW_BY_WEIGHT_HOST_TEST
#define ARDUINO_ESP32S3_DEV
#define OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL 1
#ifndef OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER
#define OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER 1
#endif

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "../openBrewByWeight.cpp"

namespace {

int failures = 0;
int testsRun = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << __func__ << ":" << __LINE__ << ": check failed: "       \
                << #condition << "\n";                                         \
      ++failures;                                                              \
      return;                                                                  \
    }                                                                          \
  } while (false)

void deleteHostResources() {
  releaseSettingsPersistenceWorkerForHost();
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

void resetMomentaryHarness() {
  deleteHostResources();
  firmwareModeRaw = static_cast<uint8_t>(openbrewbyweight::FirmwareMode::FULL);
  Serial.reset();
  resetSerialCliState();
  hostMillis = 0;
  hostPinLevel.fill(HIGH);
  hostPinMode.fill(0);
  hostTrackedRelayPin = RELAY_GPIO;
  hostTrackedRelayOpenLevel = RELAY_OPEN_LEVEL;
  hostTrackedRelayClosedLevel = RELAY_CLOSED_LEVEL;
  hostRelayOpenWrites = 0;
  hostRelayClosedWrites = 0;
  hostEspTimerCreateSucceeds = true;
  hostEspTimerStartSucceeds = true;
  hostEspTimerStopSucceeds = true;
  hostGptimerCreateSucceeds = true;
  hostGptimerArmSucceeds = true;
  hostTaskWatchdogOperationsSucceed = true;
  hostSettingsPersistQueueCreateSucceeds = true;
  hostSettingsPersistTaskCreateSucceeds = true;
  EEPROM.beginSucceeds = true;
  BLE.beginSucceeds = true;
  resetSafetyResetGuardForHost();

  stopperState = StopperState::REQUIRES_OFF;
  shot = ShotTrajectory{};
  session = CycleSession{};
  resetCupPresence();
  idleTare = IdleTareRuntime{};
  workerIdleTare = IdleTareStatus{};
  pendingCupRemovedSettle = false;
  scaleWeightEventPending = false;
  scaleWeightEventHead = 0;
  scaleWeightEventCount = 0;
  scaleWeightEventDrops = 0;
  runtimeConfig = RuntimeConfig{};
  bbwLearningBank = BbwLearningBank{};
  bbwQuickStart = QuickStartRecord{};
  runtimeConfig.autoTareOutsideBrew = false;
  runtimeConfig.requireCupToStart = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  runtimeConfig.fastSideEnabled = false;
  runtimeConfig.slowSideEnabled = false;
  platformClockReady = true;
  persistenceReady = true;
  setScaleWorkerBleReadyForHost(true);
  firmwareInitializationComplete = true;
  publishScaleWorkerPolicy(runtimeConfig, firmwareInitializationComplete);
  lastScaleWeightAtMs = 0;
  scaleWeightUpdateIntervalMs = 0;
  lastScalePacketGapLogMs = 0;
  telemetryWeightStreamState = WeightStreamState::NO_SAMPLE;
  publishedControlGate = ControlGateSnapshot{};
  controlStatusPublishRequested = false;
  circuitClosed = false;
  relaySafetyTripped = false;
  operationalLimitTripped = false;
  circuitClosedAtMs = 0;
  operationalLimitAtArmMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  relaySafetyState = RelaySafetyState::OPEN;
  relaySafetyFault = RelaySafetyFault::NONE;
  relaySafetyGeneration = 0;
  safetyEventFlags.clear(SAFETY_EVENT_CRITICAL_TASK_WATCHDOG);
  activatorOn = false;
  rawActivatorOn = false;
  activatorTurnedOn = false;
  activatorTurnedOff = false;
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = 0;
  currentWeightSequence = 0;
  currentWeightConnectionGeneration = 0;
  observedWeight = 0.0f;
  observedWeightReceivedAtMs = 0;
  observedWeightSequence = 0;
  observedWeightConnectionGeneration = 0;
  pendingScaleConnectIdleSync = false;
  scale.connected = true;
  updateWorkerLinkState();

  scaleCommandQueue =
      xQueueCreate(SCALE_COMMAND_QUEUE_LENGTH, sizeof(ScaleCommand));
  scaleEventQueue =
      xQueueCreate(SCALE_EVENT_QUEUE_LENGTH, sizeof(ScaleEvent));
  webCommandQueue =
      xQueueCreate(WEB_COMMAND_QUEUE_LENGTH, sizeof(WebCommand));
  CHECK(initializeSettingsPersistenceWorker());
  CHECK(initializeRelaySafetyTimer());
  relaySafetyTimersReady = true;
  taskWatchdogReady = configureTaskWatchdog() &&
                      subscribeCurrentTaskToWatchdog();
  digitalWrite(RELAY_GPIO, RELAY_OPEN_LEVEL);
  hostPinLevel[ACTIVATOR_GPIO] = !ACTIVATOR_ACTIVE_LEVEL;
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  hostPinLevel[REED_GPIO] = !REED_ACTIVE_LEVEL;
#endif
  initializeActivatorInput();
  machineOnActivatorReady();
  machineSetActivatorDriveAllowed(true);
  machineNoteActivatorReleased();
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
  momentaryInferredState = MachineRunState::IDLE;
  momentarySawScale = false;
  momentaryEspressoConfirmed = false;
  momentaryHadEspressoConfirm = false;
  momentarySawEspresso = false;
  momentaryStopAwaitingAck = false;
  momentaryStopRetryPending = false;
  momentaryStartAwaitingAck = false;
  momentaryStartBaselineSinceMs = 0;
  momentaryOrphanRun = false;
  momentaryStaleSinceMs = 0;
  momentaryLowFlowSinceMs = 0;
  momentaryRisingSinceMs = 0;
  momentaryQuietSinceMs = 0;
  momentaryLastWeightSequence = 0;
  momentaryLastWeightG = 0.0f;
  momentaryLastWeightAtMs = 0;
  momentaryShotBaselineG = 0.0f;
  momentaryQuietBaselineG = 0.0f;
  momentaryLogicalRunActive = false;
  momentaryLogicalRunStartedAtMs = 0;
  clearMomentaryElapsedLatch();
  momentaryFirmwareCutPending = false;
  momentarySettledWeightCutArmed = false;
  momentaryStopSettling = false;
  momentaryNoFlowIdlePending = false;
#endif
  momentarySkipFirmwareStopPulse = false;
  pulseOutputActive = false;
  pulseOutputKind = FirmwarePulseKind::STOP;
  firmwarePulsePending = false;
  firmwarePulsePendingKind = FirmwarePulseKind::FORCED;
  firmwarePulsePendingReadyAtMs = 0;
  rinseActuationActive = false;
  rinseClear();
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  reedRawOn = false;
  reedOn = false;
  reedSawStableOff = false;
  reedAssume = ReedAssume::NONE;
  reedChangedAtMs = millis();
  momentaryFirmwareStopIssued = false;
#endif
  localBuzzer.begin(BUZZER_GPIO);
  seedDefaultShotPresetBank(presetBank);
  presetBank.activeId = FACTORY_PRESET_ID_DOUBLE_ADAPTIVE;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.requireCupToStart = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  hostRelayClosedWrites = 0;
  hostRelayOpenWrites = 0;
  lastCycle = LastCycleSummary{};
  LastShotStore::setHostSaveSucceeds(true);
  lastShotStore.clear();
  noScaleShotGuardArmed = true;
  noScaleShotGuardActivityAtMs = 0;
  noScaleShotGuardScaleWasAvailable = false;
  noScaleShotGuardHold = false;
  noScaleShotGuardHoldAtMs = 0;
  noScaleShotGuardNeedsFreshActivator = false;
  resetNoScaleRequireBypassGesture();
  noScaleRequireBypassCompletedThisLoop = false;
}

void runLoopAfter(uint32_t deltaMs) {
  hostMillis += deltaMs;
  if (scale.connected) {
    markScaleWorkerProgress();
    if (currentWeightSequence > 0) {
      observedWeight = currentWeight;
      observedWeightReceivedAtMs = currentWeightReceivedAtMs;
      observedWeightSequence = currentWeightSequence;
      const uint32_t gen = getScaleLinkSnapshot().connectionGeneration;
      observedWeightConnectionGeneration = gen != 0 ? gen : 1;
    }
  }
  hostServiceEspTimer(relaySafetyTimer);
  hostServiceEspTimer(operationalLimitTimer);
  independentSafetyTimer.serviceForHost();
  loop();
}

void seedFreshScaleWeight(float weight) {
  currentWeight = weight;
  currentWeightReceivedAtMs = hostMillis;
  if (currentWeightSequence == 0) {
    currentWeightSequence = 1;
  } else {
    ++currentWeightSequence;
  }
  observedWeight = currentWeight;
  observedWeightReceivedAtMs = currentWeightReceivedAtMs;
  observedWeightSequence = currentWeightSequence;
  const uint32_t gen = getScaleLinkSnapshot().connectionGeneration;
  observedWeightConnectionGeneration = gen != 0 ? gen : 1;
  runLoopAfter(50);
}

void setRawPaddle(bool on) {
  hostPinLevel[ACTIVATOR_GPIO] = on ? ACTIVATOR_ACTIVE_LEVEL
                                : !ACTIVATOR_ACTIVE_LEVEL;
  loop();
}

void shortPress(uint32_t heldMs) {
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  runLoopAfter(heldMs);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
}

void pressDown() {
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
}

void releaseUp() {
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
}

void publishIdlePlacementWeight(float weight) {
  hostMillis += 150;
  ScaleEvent event;
  event.type = ScaleEventType::WEIGHT;
  event.receivedAtMs = hostMillis;
  event.weightG = weight;
  CHECK(publishScaleEvent(event, false));
  runLoopAfter(0);
}

void prepareQueuedIdleTare() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  runtimeConfig.autoTareOutsideBrew = true;
  for (float weight : {0.0f, 80.0f}) {
    for (uint8_t i = 0; i < runtimeConfig.retareStabilitySamples; ++i)
      publishIdlePlacementWeight(weight);
  }
  CHECK(idleTare.requestId != 0);
  CHECK(idleScaleTareStatus().phase == IdleTarePhase::QUEUED);
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
void setRawReed(bool on) {
  hostPinLevel[REED_GPIO] = on ? REED_ACTIVE_LEVEL : !REED_ACTIVE_LEVEL;
  loop();
  runLoopAfter(REED_DEBOUNCE_MS + 1);
}
#endif

void t_short_press_mirrors_then_opens() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(stopperState == StopperState::READY);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(session.active);
  releaseUp();
  CHECK(session.active);
  CHECK(!getRelaySafetySnapshot().closed);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
  CHECK(machineIsRunning());
#endif
}

void t_default_starts_on_press() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(session.active);
  CHECK(stopperState != StopperState::READY || session.active);
  CHECK(getRelaySafetySnapshot().closed);
  releaseUp();
  CHECK(session.active);
}

void t_release_mode_starts_on_release_not_press() {
  resetMomentaryHarness();
  runtimeConfig.momentaryStartOnPress = false;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(!session.active);
  CHECK(stopperState == StopperState::READY);
  CHECK(getRelaySafetySnapshot().closed);
  releaseUp();
  CHECK(session.active);
}

void t_idle_tare_yields_to_configured_start_edge() {
  for (bool startOnPress : {true, false}) {
    prepareQueuedIdleTare();
    runtimeConfig.momentaryStartOnPress = startOnPress;
    const size_t closedBefore = hostRelayClosedWrites;
    pressDown();
    CHECK(getRelaySafetySnapshot().closed);
    CHECK(session.active == startOnPress);
    CHECK(idleTare.requestId == 0);
    CHECK(idleTare.lastReason == IdleTareReason::START_REQUEST);
    releaseUp();
    CHECK(session.active);
    CHECK(idleTare.requestId == 0);
    CHECK(idleTare.lastReason == IdleTareReason::START_REQUEST);
    CHECK(hostRelayClosedWrites == closedBefore + 1);
    CHECK(!pulseOutputActive);
    CHECK(!firmwarePulsePending);
    size_t startCommands = 0;
    for (const std::vector<uint8_t> &bytes : scaleCommandQueue->items) {
      ScaleCommand command;
      std::memcpy(&command, bytes.data(), sizeof(command));
      startCommands += command.type == ScaleCommandType::START_TIMER_AND_TARE;
    }
    CHECK(startCommands == 1);
    runLoopAfter(125);
    CHECK(machineElapsedMs() >= 125);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
    setRawReed(true);
    CHECK(machineRunState() == MachineRunState::RUNNING);
#endif
  }
}

void t_second_short_press_stops_without_rinse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(180);
  CHECK(session.active);
  CHECK(stopperState != StopperState::RINSE);
  shortPress(180);
  CHECK(stopperState != StopperState::RINSE);
  CHECK(!session.active || stopperState == StopperState::READY);
}

void t_noscale_last_shot_keeps_logical_duration() {
  resetMomentaryHarness();
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  currentWeightSequence = 0;
  shotLog.clear();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  setRawReed(true);
#endif
  runLoopAfter(12000);
  CHECK(machineElapsedMs() >= 11000);
  shortPress(150);
  CHECK(!session.active);
  CHECK(persistedLastShot.valid);
  CHECK(persistedLastShot.durationMs >= 11000);
  CHECK(!persistedLastShot.weightValid);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
  CHECK(!machineIsRunning());
  CHECK(machineElapsedMs() >= 11000);
#endif
  CHECK(shotLog.count() == 0);
}

void t_guard_reject_does_not_mirror() {
  resetMomentaryHarness();
  runtimeConfig.cupProtectionEnabled = true;
  runtimeConfig.requireCupToStart = true;
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  const size_t closedBefore = hostRelayClosedWrites;
  pressDown();
  CHECK(hostRelayClosedWrites == closedBefore);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  CHECK(stopperState == StopperState::READY);
  releaseUp();
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t_no_scale_bbw_armed_does_not_mirror_then_idle_allows() {
  resetMomentaryHarness();
  runtimeConfig.timerOnly = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  mutableActiveShotPreset(presetBank).brewByWeight = true;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  noScaleShotGuardArmed = true;
  noScaleShotGuardHold = false;
  noScaleShotGuardActivityAtMs = 0;
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(stopperState == StopperState::READY);
  CHECK(noScaleShotGuardArmed);
  const size_t closedBefore = hostRelayClosedWrites;
  pressDown();
  CHECK(hostRelayClosedWrites == closedBefore);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  CHECK(stopperState == StopperState::READY);
  CHECK(!noScaleShotGuardArmed);
  CHECK(noScaleShotGuardHold);
  releaseUp();
  CHECK(!noScaleShotGuardHold);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(session.active);
}

void t_no_scale_bbw_release_mode_short_press_consumes_warning() {
  resetMomentaryHarness();
  runtimeConfig.momentaryStartOnPress = false;
  runtimeConfig.timerOnly = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  mutableActiveShotPreset(presetBank).brewByWeight = true;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  noScaleShotGuardArmed = true;
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);

  pressDown();
  CHECK(noScaleShotGuardArmed);
  CHECK(!session.active);
  releaseUp();
  CHECK(!noScaleShotGuardArmed);
  CHECK(noScaleShotGuardHold);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);

  // The next idle loop releases the safety latch. A fresh press/release is
  // then allowed to start a manual no-scale shot.
  runLoopAfter(1);
  CHECK(!noScaleShotGuardHold);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  releaseUp();
  CHECK(session.active);
}

void t_require_scale_triple_press_release_never_starts_on_final_release() {
  for (bool startOnPress : {true, false}) {
    resetMomentaryHarness();
    runtimeConfig.momentaryStartOnPress = startOnPress;
    runtimeConfig.timerOnly = false;
    runtimeConfig.noScaleBbwMode =
        static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
    mutableActiveShotPreset(presetBank).brewByWeight = true;
    runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
    runtimeConfig.noScaleBbwMode =
        static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
    scale.connected = false;
    setScaleLinkState(ScaleLinkState::DISCONNECTED);
    runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);

    for (int cycle = 0; cycle < 3; ++cycle) {
      shortPress(50);
      CHECK(!session.active);
      CHECK(!getRelaySafetySnapshot().closed);
      if (cycle < 2) {
        CHECK(noScaleShotGuardArmed);
        CHECK(noScaleRequireBypassCycles == cycle + 1);
      }
    }

    CHECK(!noScaleShotGuardArmed);
    CHECK(stopperState == StopperState::READY);
    CHECK(localBuzzer.activeCue == BuzzerCue::SCALE_CONNECTED);

    shortPress(50);
    CHECK(session.active);
    CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  }
}

void t_user_stop_without_session_does_not_leave_orphan_run() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(!session.active);
  CHECK(!machineIsRunning());
  momentaryUserStopThisCycle = true;
  momentarySkipFirmwareStopPulse = true;
  serviceMachine();
  CHECK(!machineIsRunning());
  CHECK(!momentaryLogicalRunActive);
}

void t_long_press_mirrors_from_first_instant() {
  resetMomentaryHarness();
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  runtimeConfig.reedConfirmTimeoutHundredMs = 50;
#endif
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(session.active);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  runLoopAfter(COMPILED_MAX_SINGLE_PRESS_MS + 20);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!machineIsRunning());
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!session.active);
}

void t_release_mode_long_press_ignored_as_start() {
  resetMomentaryHarness();
  runtimeConfig.momentaryStartOnPress = false;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  runLoopAfter(COMPILED_MAX_SINGLE_PRESS_MS + 20);
  CHECK(!session.active);
  CHECK(getRelaySafetySnapshot().closed);
  setRawPaddle(false);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!session.active);
}

void enableFirmwareRinseForTest() {
  runtimeConfig.rinseEnabled = true;
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  runtimeConfig.reedConfirmTimeoutHundredMs = 50;
#endif
}

void t_rinse_press_demotes_and_pulses() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(session.active);
  CHECK(stopperState != StopperState::RINSE);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(rinseActuationActive);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputIsStart);
  // The demoted rinse keeps the press-started run's clock: the gesture window
  // is already consumed at classification (loop slack tolerated for the ms
  // boundary between the press edge and the run start).
  CHECK(elapsedMs(session.rinseStartedAtMs) + 5 >=
        runtimeConfig.rinseGestureMs);
  runLoopAfter(runtimeStopPulseMs(runtimeConfig) + 1);
  CHECK(!pulseOutputActive);
  CHECK(stopperState == StopperState::RINSE);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  // A reed confirm during the rinse must not shrink the elapsed back to the
  // pulse-restart time.
  setRawReed(true);
  CHECK(rinseActuationActive);
  CHECK(machineElapsedMs() + 5 >= runtimeConfig.rinseGestureMs);
  setRawReed(false);
#endif
  runLoopAfter(runtimeConfig.rinseDurationMs -
               elapsedMs(session.rinseStartedAtMs));
  CHECK(pulseOutputActive);
  CHECK(!pulseOutputIsStart);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!rinseActuationActive);
}

void t_rinse_release_mode_starts_directly() {
  prepareQueuedIdleTare();
  enableFirmwareRinseForTest();
  runtimeConfig.momentaryStartOnPress = false;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(!session.active);
  CHECK(stopperState == StopperState::READY);
  runLoopAfter(runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(session.active);
  CHECK(idleTare.requestId == 0);
  CHECK(idleTare.lastReason == IdleTareReason::START_REQUEST);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputIsStart);
}

void t_rinse_does_not_start_during_running_shot() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runtimeConfig.momentaryStartOnPress = false;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  setRawReed(true);
#endif
  CHECK(session.active);
  CHECK(stopperState != StopperState::RINSE);
  pressDown();
  CHECK(session.active);
  runLoopAfter(runtimeConfig.rinseGestureMs + 50);
  CHECK(stopperState != StopperState::RINSE);
  CHECK(session.active);
}

void t_rinse_armed_noscale_long_press_consumes_guard() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runtimeConfig.timerOnly = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  mutableActiveShotPreset(presetBank).brewByWeight = true;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  runtimeConfig.rinseEnabled = true;
  runtimeConfig.buzzerManualNoScaleBeep = true;
  noScaleShotGuardArmed = true;
  noScaleShotGuardHold = false;
  noScaleShotGuardActivityAtMs = 0;
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(noScaleShotGuardArmed);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  pressDown();
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
  CHECK(noScaleShotGuardHold);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  runLoopAfter(runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!noScaleShotGuardArmed);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  releaseUp();
  CHECK(!noScaleShotGuardHold);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(session.active);
}

void t_rinse_armed_noscale_long_press_allowed_runs() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runtimeConfig.timerOnly = false;
  runtimeConfig.noScaleBbwMode = packNoScaleBbwMode(
      static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE), true);
  mutableActiveShotPreset(presetBank).brewByWeight = true;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.noScaleBbwMode = packNoScaleBbwMode(
      static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE), true);
  runtimeConfig.rinseEnabled = true;
  runtimeConfig.buzzerManualNoScaleBeep = true;
  noScaleShotGuardArmed = true;
  noScaleShotGuardHold = false;
  noScaleShotGuardActivityAtMs = 0;
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(noScaleShotGuardArmed);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  pressDown();
  CHECK(stopperState == StopperState::READY);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!noScaleShotGuardArmed);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
  runLoopAfter(runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(!noScaleShotGuardArmed);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests >= beforeBeeps + 2);
}

void t_rinse_armed_noscale_release_mode_does_not_run() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runtimeConfig.momentaryStartOnPress = false;
  runtimeConfig.timerOnly = false;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  mutableActiveShotPreset(presetBank).brewByWeight = true;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
  runtimeConfig.rinseEnabled = true;
  runtimeConfig.buzzerManualNoScaleBeep = true;
  noScaleShotGuardArmed = true;
  noScaleShotGuardHold = false;
  noScaleShotGuardActivityAtMs = 0;
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(noScaleShotGuardArmed);
  for (uint32_t step = 0; step < 80 && localBuzzer.busy(); ++step) {
    runLoopAfter(40);
  }
  const uint32_t beforeBeeps = localBuzzer.acceptedRequests;
  pressDown();
  CHECK(noScaleShotGuardArmed);
  CHECK(!session.active);
  CHECK(!getRelaySafetySnapshot().closed);
  runLoopAfter(runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::READY);
  CHECK(!session.active);
  CHECK(!noScaleShotGuardArmed);
  CHECK(noScaleShotGuardHold);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(localBuzzer.acceptedRequests == beforeBeeps + 1);
}

void t_rinse_release_mode_mid_hold_is_native_not_shot() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runtimeConfig.momentaryStartOnPress = false;
  setRuntimeMaxSinglePressMs(runtimeConfig, 400);
  runtimeConfig.rinseGestureMs = 1000;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(!session.active);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(500);
  CHECK(!session.active);
  CHECK(stopperState != StopperState::RINSE);
  releaseUp();
  CHECK(!session.active);
  CHECK(stopperState != StopperState::RINSE);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t_rinse_end_aborts_start_pulse_for_stop_pulse() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runtimeConfig.momentaryStartOnPress = false;
  runtimeConfig.rinseDurationMs = 500;
  setRuntimeStopPulseMs(runtimeConfig, 1000);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  runLoopAfter(runtimeConfig.rinseGestureMs);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputIsStart);
  runLoopAfter(runtimeConfig.rinseDurationMs -
               elapsedMs(session.rinseStartedAtMs));
  CHECK(pulseOutputActive);
  CHECK(!pulseOutputIsStart);
  CHECK(!rinseActuationActive);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
}

void t_rinse_web_over_running_shot_keeps_full_duration() {
  resetMomentaryHarness();
  enableFirmwareRinseForTest();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(session.active);
  runLoopAfter(1500);
  // A web rinse over a running shot restarts the run: the full duration runs
  // from the command and the shot's elapsed time is not inherited.
  WebCommand rinse;
  rinse.type = WebCommandType::RINSE;
  rinse.unsafeWebUiOverride = true;
  processWebCommand(rinse);
  CHECK(stopperState == StopperState::RINSE);
  CHECK(session.active);
  CHECK(rinseActuationActive);
  CHECK(elapsedMs(session.rinseStartedAtMs) < 50);
  runLoopAfter(runtimeConfig.rinseDurationMs + 1);
  CHECK(!rinseActuationActive);
  CHECK(stopperState != StopperState::RINSE);
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
void t_only_auto_cut_needs_running() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  const size_t closedQuiet = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedQuiet);

  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    currentWeight += 0.45f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    runLoopAfter(100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
  const size_t closedAtConfirm = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites > closedAtConfirm);
}

void t_quiet_pan_does_not_force_cut_when_unknown() {
  resetMomentaryHarness();
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  noScaleShotGuardArmed = false;
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  const size_t closedAtStart = hostRelayClosedWrites;
  runLoopAfter(1200);
  CHECK(machineRequestStop());
  (void)closedAtStart;
}
#endif

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
void t_reed_off_blocks_firmware_cut() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  CHECK(!reedIsOn());
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t_reed_on_allows_firmware_cut() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  setRawReed(true);
  CHECK(reedIsOn());
  CHECK(machineIsRunning());
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites > closedBefore);
}
#endif

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
void t_tare_rebases_flow_signature() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    currentWeight += 0.45f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    runLoopAfter(100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  runLoopAfter(50);
  CHECK(machineRunState() != MachineRunState::RUNNING);
  const size_t closedQuiet = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedQuiet);
}

void t_fast_control_loop_still_confirms_running() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    currentWeight += 0.45f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    for (int tick = 0; tick < 10; ++tick) {
      runLoopAfter(10);
    }
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(machineAllowsFirmwareStopPulse());
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore + 1);
}

void t_start_tare_drop_rebases_then_confirms() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 40.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(100);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  runLoopAfter(100);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  for (int step = 0; step < 8; ++step) {
    currentWeight += 0.45f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    runLoopAfter(100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore + 1);
}

void t_user_stop_does_not_extra_pulse_when_running_unconfirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  CHECK(machineIsRunning());
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_wall_with_quiet_pan_does_not_auto_pulse() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 800;
  seedFreshScaleWeight(0.0f);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  const size_t closedAtStart = hostRelayClosedWrites;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  runLoopAfter(900);
  CHECK(!momentaryLogicalRunActive || getRelaySafetySnapshot().operationalTripped ||
        operationalLimitTripped);
  CHECK(hostRelayClosedWrites == closedAtStart);
  CHECK(!machineAllowsFirmwareStopPulse());
}

void t_settings_lock_follows_logical_run() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(controlAllowsConfigurationNow());
  shortPress(150);
  CHECK(machineIsRunning());
  CHECK(!controlAllowsConfigurationNow());
}

void t_polarity_resyncs_to_running_machine() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(50);
  CHECK(machineIsRunning());
  shortPress(180);
  CHECK(momentaryUserStopThisCycle || !machineIsRunning() || !session.active);
}

void t_user_press_wins_over_firmware_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  momentaryInferredState = MachineRunState::RUNNING;
  CHECK(machineRequestStop());
  CHECK(pulseOutputActive || getRelaySafetySnapshot().closed);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!pulseOutputActive);
  releaseUp();
  CHECK(!getRelaySafetySnapshot().closed);
}
#endif

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
void t_reed_boot_on_is_running() {
  resetMomentaryHarness();
  hostPinLevel[REED_GPIO] = REED_ACTIVE_LEVEL;
  reedRawOn = false;
  reedOn = false;
  reedSawStableOff = false;
  reedChangedAtMs = millis();
  loop();
  runLoopAfter(REED_DEBOUNCE_MS + 1);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(machineIsRunning());
  momentaryUserStopThisCycle = true;
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_reed_safety_trip_follows_reed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  setRawReed(true);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  relaySafetyState = RelaySafetyState::TRIPPED;
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(machineRunState() != MachineRunState::UNKNOWN);
  relaySafetyState = RelaySafetyState::LOCKOUT;
  CHECK(machineRunState() == MachineRunState::RUNNING);
  setRawReed(false);
  relaySafetyState = RelaySafetyState::LOCKOUT;
  CHECK(machineRunState() == MachineRunState::IDLE);
  relaySafetyState = RelaySafetyState::TRIPPED;
  CHECK(machineRunState() == MachineRunState::IDLE);
}

void t_reed_off_wins_over_rising_weight() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    currentWeight += 0.45f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    runLoopAfter(100);
  }
  CHECK(!reedIsOn());
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_running_unconfirmed_offers_web_stop() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(momentaryLogicalRunActive);
  CHECK(!reedIsOn());
  CHECK(machineIsRunning());
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
}
#endif

// SAF-02: the boot recovery grace samples a button held at power-on before
// initialization completes. Releasing it in the grace must leave K1 untouched
// (no INITIALIZATION_FAILED lockout) so startup still reaches READY.
void t_boot_hold_released_in_grace_never_drives_relay() {
  resetMomentaryHarness();
  firmwareInitializationComplete = false;
  hostPinLevel[ACTIVATOR_GPIO] = ACTIVATOR_ACTIVE_LEVEL;
  machineInitialize();
  machineSampleInput();
  CHECK(momentaryPhysicalOn);
  hostPinLevel[ACTIVATOR_GPIO] = !ACTIVATOR_ACTIVE_LEVEL;
  machineSampleInput();
  hostMillis += ACTIVATOR_DEBOUNCE_MS + 1;
  machineSampleInput();
  CHECK(!momentaryPhysicalOn);
  CHECK(hostRelayClosedWrites == 0);
  CHECK(getRelaySafetySnapshot().state == RelaySafetyState::OPEN);
  CHECK(getRelaySafetySnapshot().fault == RelaySafetyFault::NONE);
  machineOnActivatorReady();
  firmwareInitializationComplete = true;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(stopperState == StopperState::READY);
  shortPress(150);
  CHECK(session.active);
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
void dripFreshWeight(float weight, uint32_t dtMs) {
  currentWeight = weight;
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  runLoopAfter(dtMs);
}

// Enqueue a BLE weight packet and let loop() run processScaleWorkerEvents →
// observeMachineSenseFromSession → serviceMachine (cup REMOVED wiring).
void dripQueuedWeight(float weight, uint32_t dtMs) {
  ScaleEvent event;
  event.type = ScaleEventType::WEIGHT;
  event.receivedAtMs = hostMillis;
  event.weightG = weight;
  CHECK(publishScaleEvent(event, false));
  runLoopAfter(dtMs);
}

void seedPresentCup(float weight = 80.0f) {
  const uint8_t sampleCount = runtimeConfig.retareStabilitySamples;
  const uint32_t minDurationMs = runtimeConfig.retareStabilityMinDurationMs;
  const uint32_t stepMs =
      sampleCount > 1U && minDurationMs > 0U
          ? minDurationMs / static_cast<uint32_t>(sampleCount - 1U)
          : 100U;
  const uint32_t intervalMs = stepMs > 0U ? stepMs : 100U;
  for (uint8_t index = 0; index < sampleCount; ++index) {
    dripQueuedWeight(weight + (index == 1 ? 0.1f : 0.0f), intervalMs);
  }
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
}

void liftPresentCup() {
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  for (uint8_t sample = 0; sample < DIRECT_STOP_CONFIRMATION_SAMPLES; ++sample) {
    dripQueuedWeight(runtimeConfig.cupRemovedWeightG - 1.0f, 50);
  }
  CHECK(cupPresenceState() == CupPresenceState::ABSENT);
}

void confirmEspressoFlow() {
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
}

void holdStableWeight(uint32_t totalMs) {
  const float held = currentWeight;
  for (uint32_t waited = 0; waited < totalMs; waited += 100) {
    dripFreshWeight(held, 100);
  }
}

void armLiveScaleTimerOnlyStart(float weight) {
  runtimeConfig.timerOnly = true;
  ShotPreset &preset = mutableActiveShotPreset(presetBank);
  preset.brewByWeight = false;
  preset.operationalWallMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  runtimeConfig.scaleLossEnabled = false;
  runtimeConfig = composeEffectiveConfig(runtimeConfig, presetBank);
  seedFreshScaleWeight(weight);
}

void dripUntilHardCap(float (*weightAtStep)(uint32_t step)) {
  uint32_t step = 0;
  const uint32_t startMs = momentaryLogicalRunStartedAtMs != 0
                               ? momentaryLogicalRunStartedAtMs
                               : session.startedAtMs;
  while (elapsedMs(startMs) <= HARD_MAX_CIRCUIT_CLOSED_MS) {
    dripFreshWeight(weightAtStep(step), 200);
    ++step;
  }
}

void t_start_ack_stays_assumed_without_flow() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  runLoopAfter(800);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  CHECK(!machineAllowsFirmwareStopPulse());
}

void t_running_expires_without_flow() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  const float held = currentWeight;
  for (int step = 0; step < 6; ++step) {
    dripFreshWeight(held, 100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  const size_t closedQuiet = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedQuiet);
}

void t_stop_ack_quiet_confirms_idle() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRequestStop());
  CHECK(machineIsRunning());
  const float held = currentWeight;
  for (int step = 0; step < 4; ++step) {
    dripFreshWeight(held, 100);
  }
  CHECK(machineIsRunning());
  for (int step = 0; step < 8; ++step) {
    dripFreshWeight(held, 100);
  }
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!machineIsRunning());
}

void t_stop_ack_timeout_without_quiet_stays_idle_unconfirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRequestStop());
  CHECK(machineIsRunning());
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  for (int step = 0; step < 4; ++step) {
    dripFreshWeight(currentWeight + 0.6f, 600);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(!machineIsRunning());
  CHECK(machineRunState() != MachineRunState::IDLE);
}

void t_start_preinfusion_quiet_stays_assumed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 40; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  CHECK(machineIsRunning());
}

void t_start_nack_long_baseline_confirms_idle() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 61; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(!machineIsRunning());
}

void t_start_nack_timeout_follows_setting() {
  resetMomentaryHarness();
  runtimeConfig.shotReactTimeoutS = 3;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 16; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
}

void t_flow_after_idle_unconfirmed_confirms_running_without_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 61; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  const size_t closedBefore = hostRelayClosedWrites;
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  ++currentWeightSequence;
  runLoopAfter(50);
  for (int step = 0; step < 12; ++step) {
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_start_nack_quiet_does_not_idle() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 61; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(!momentaryStopSettling);
  holdStableWeight(2000);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(machineRunState() != MachineRunState::IDLE);
}

float noFlowIdleFlatWeight(uint32_t) { return 0.0f; }

float noFlowIdleNoiseWeight(uint32_t step) {
  return (step % 2U) == 0U ? 0.8f : 0.0f;
}

float noFlowIdleDripWeight(uint32_t step) {
  const float mass = static_cast<float>(step) * 0.1f;
  return mass > 1.5f ? 1.5f : mass;
}

void t_no_flow_hard_cap_idles_after_nack() {
  resetMomentaryHarness();
  shotLog.clear();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  armLiveScaleTimerOnlyStart(0.0f);
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.config.timerOnly);
  CHECK(session.timerStartCommandQueued);
  const uint32_t beepsAfterStart = localBuzzer.acceptedRequests;
  const size_t closedBefore = hostRelayClosedWrites;
  dripUntilHardCap(noFlowIdleFlatWeight);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
  CHECK(!machineIsRunning());
  CHECK(lastCycle.endReason == EndReason::UNCONFIRMED_START);
  CHECK(!persistedLastShot.valid);
  CHECK(shotLog.count() == 0);
  CHECK(hostRelayClosedWrites == closedBefore);
  CHECK(localBuzzer.acceptedRequests == beepsAfterStart);
  seedFreshScaleWeight(0.0f);
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::BREW);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  CHECK(session.timerStartCommandQueued);
}

void t_no_flow_hard_cap_idles_running_unconfirmed_noise() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  armLiveScaleTimerOnlyStart(0.8f);
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::BREW);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  const size_t closedBefore = hostRelayClosedWrites;
  dripUntilHardCap(noFlowIdleNoiseWeight);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
  CHECK(!machineIsRunning());
  CHECK(lastCycle.endReason == EndReason::UNCONFIRMED_START);
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_no_flow_hard_cap_skips_when_espresso_confirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  armLiveScaleTimerOnlyStart(0.0f);
  shortPress(150);
  CHECK(stopperState == StopperState::BREW);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  const float held = currentWeight;
  while (elapsedMs(momentaryLogicalRunStartedAtMs) + 1000U <
         HARD_MAX_CIRCUIT_CLOSED_MS) {
    dripFreshWeight(held, 200);
  }
  CHECK(session.active);
  CHECK(machineRunState() != MachineRunState::IDLE);
  while (elapsedMs(momentaryLogicalRunStartedAtMs) < HARD_MAX_CIRCUIT_CLOSED_MS) {
    dripFreshWeight(held, 200);
  }
  dripFreshWeight(held, 50);
  CHECK(lastCycle.endReason != EndReason::UNCONFIRMED_START);
}

void t_no_flow_hard_cap_skips_after_espresso_pan_returns() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  armLiveScaleTimerOnlyStart(0.0f);
  shortPress(150);
  CHECK(stopperState == StopperState::BREW);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  dripFreshWeight(0.0f, 200);
  dripFreshWeight(0.0f, 400);
  CHECK(machineRunState() != MachineRunState::RUNNING);
  CHECK(momentaryHadEspressoConfirm);
  dripUntilHardCap(noFlowIdleFlatWeight);
  CHECK(lastCycle.endReason != EndReason::UNCONFIRMED_START);
}

void t_no_flow_hard_cap_skips_when_mass_above_band() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  armLiveScaleTimerOnlyStart(0.0f);
  shortPress(150);
  CHECK(stopperState == StopperState::BREW);
  dripUntilHardCap(noFlowIdleDripWeight);
  CHECK(lastCycle.endReason != EndReason::UNCONFIRMED_START);
  CHECK(machineRunState() != MachineRunState::IDLE ||
        lastCycle.endReason == EndReason::GLOBAL_LIMIT);
}

void t_no_flow_hard_cap_skips_when_stale() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  armLiveScaleTimerOnlyStart(0.0f);
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::BREW);
  dripFreshWeight(0.0f, 50);
  runLoopAfter(3000);
  CHECK(session.active);
  CHECK(machineRunState() != MachineRunState::IDLE);
  CHECK(lastCycle.endReason != EndReason::UNCONFIRMED_START);
}

void pushCupRemovedSettleEdge() {
  MachineSense sense = machineSense;
  sense.cupRemovedEdge = true;
  sense.weightFresh = true;
  machineObserveSense(sense);
  serviceMomentaryRunSensors();
}

void t_cup_removed_settles_idle_unconfirmed_after_stop() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRequestStop());
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(momentaryStopSettling);
  pushCupRemovedSettleEdge();
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!momentaryStopSettling);
  CHECK(!machineIsRunning());
}

void t_cup_removed_settles_start_nack_idle_unconfirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  seedPresentCup();
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 70; ++step) {
    dripFreshWeight(80.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(!momentaryStopSettling);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  liftPresentCup();
  CHECK(machineRunState() == MachineRunState::IDLE);
}

void t_cup_removed_event_settles_idle_unconfirmed_after_stop() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  seedPresentCup();
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRequestStop());
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(momentaryStopSettling);
  CHECK(cupPresenceState() == CupPresenceState::PRESENT);
  runLoopAfter(runtimeConfig.postTareBaselineGraceMs + 50);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  liftPresentCup();
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!momentaryStopSettling);
  CHECK(!machineIsRunning());
}

void t_cup_removed_ignored_while_running_unconfirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  pushCupRemovedSettleEdge();
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
}

void t_cup_removed_ignored_while_running() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  pushCupRemovedSettleEdge();
  CHECK(machineRunState() == MachineRunState::RUNNING);
}

void t_firmware_cut_settles_off_after_drip() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  machineArmSettledWeightCutOff();
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore + 1);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(momentaryStopSettling);
  for (int step = 0; step < 4; ++step) {
    dripFreshWeight(currentWeight + 0.6f, 600);
  }
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(!machineIsRunning());
  CHECK(momentarySettledWeightCutArmed);
  holdStableWeight(1200);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!momentarySettledWeightCutArmed);
  CHECK(!momentaryStopSettling);
  CHECK(hostRelayClosedWrites == closedBefore + 1);
}

void t_firmware_cut_settles_off_without_pending_finalize() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  session.extractionExtended = true;
  session.startedWithScale = true;
  shot.automaticBrew = true;
  const size_t closedBefore = hostRelayClosedWrites;
  finalizeCycle(EndReason::FAST_EXTRACTION_MAX_WEIGHT, StopperState::READY);
  CHECK(brewWeightCutSettlesMachineOff(EndReason::FAST_EXTRACTION_MAX_WEIGHT));
  CHECK(brewWeightCutSettlesMachineOff(EndReason::SLOW_EXTRACTION_MIN_WEIGHT));
  CHECK(!brewWeightCutSettlesMachineOff(EndReason::WEB_STOP));
  CHECK(!pendingFinalize.pending);
  CHECK(momentarySettledWeightCutArmed);
  CHECK(hostRelayClosedWrites == closedBefore + 1);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  for (int step = 0; step < 4; ++step) {
    dripFreshWeight(currentWeight + 0.6f, 600);
  }
  CHECK(!machineIsRunning());
  holdStableWeight(1200);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(hostRelayClosedWrites == closedBefore + 1);
}

void t_firmware_cut_stale_weight_stays_idle_unconfirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  machineArmSettledWeightCutOff();
  CHECK(machineRequestStop());
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  runLoopAfter(3000);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(machineRunState() != MachineRunState::UNKNOWN);
}

void t_gusher_flow_confirms_running() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    dripFreshWeight(currentWeight + 1.0f, 100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
}

void t_override_sets_inferred_idle_and_brewing_without_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  const size_t closedBefore = hostRelayClosedWrites;
  WebCommand brewing;
  brewing.type = WebCommandType::STATE_OVERRIDE_ON;
  processWebCommand(brewing);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(machineIsRunning());
  WebCommand idle;
  idle.type = WebCommandType::STATE_OVERRIDE_OFF;
  processWebCommand(idle);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!machineIsRunning());
  CHECK(hostRelayClosedWrites == closedBefore);
}

// NET-06: forcing "brewing" must not cancel a firmware stop still in flight.
void t_override_on_refused_during_firmware_stop() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRequestStop());
  CHECK(momentaryFirmwareCutPending);
  WebCommand brewing;
  brewing.type = WebCommandType::STATE_OVERRIDE_ON;
  processWebCommand(brewing);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(momentaryFirmwareCutPending && momentaryStopSettling);
  momentaryFirmwareCutPending = false;
  momentaryStopRetryPending = true;
  processWebCommand(brewing);
  CHECK(momentaryStopRetryPending);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
}

void t_remote_start_emits_synthetic_start_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS, true));
  CHECK(pulseOutputActive);
  CHECK(pulseOutputIsStart);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(runtimeStopPulseMs(runtimeConfig) + 1);
  CHECK(!pulseOutputActive);
  CHECK(!getRelaySafetySnapshot().closed);
}

void t_web_stop_emits_pulse_while_running_unconfirmed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  seedFreshScaleWeight(0.0f);
  WebCommand start;
  start.type = WebCommandType::REMOTE_ON;
  processWebCommand(start);
  CHECK(session.active);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputKind == FirmwarePulseKind::START);
  runLoopAfter(runtimeStopPulseMs(runtimeConfig) + 1);
  for (int sample = 0; sample < 60; ++sample) {
    seedFreshScaleWeight(0.0f);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  const size_t closedBeforeStop = hostRelayClosedWrites;
  WebCommand stop;
  stop.type = WebCommandType::STOP;
  processWebCommand(stop);
  CHECK(!session.active);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputKind == FirmwarePulseKind::STOP);
  CHECK(hostRelayClosedWrites == closedBeforeStop + 1);
}

void t_scale_connect_settles_idle_when_idle() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  CHECK(session.active);
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(10);
  setScaleLinkState(ScaleLinkState::CONNECTED);
  runLoopAfter(10);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  session.active = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  runLoopAfter(10);
  setScaleLinkState(ScaleLinkState::CONNECTED);
  runLoopAfter(10);
  CHECK(machineRunState() == MachineRunState::IDLE);
}

void t_settled_weight_cut_confirms_idle() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  holdStableWeight(1200);
  machineArmSettledWeightCutOff();
  machineNoteSettledWeightCutOff();
  CHECK(machineRunState() == MachineRunState::IDLE);
}

void t_settled_weight_cut_stays_armed_while_pouring() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  machineArmSettledWeightCutOff();
  machineNoteSettledWeightCutOff();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(momentarySettledWeightCutArmed);
  for (int step = 0; step < 8; ++step) {
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
  CHECK(machineRunState() != MachineRunState::IDLE);
  CHECK(momentarySettledWeightCutArmed);
  holdStableWeight(1200);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!momentarySettledWeightCutArmed);
}

void t_settled_weight_cut_flow_is_not_polarity() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  machineArmSettledWeightCutOff();
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore);
  CHECK(momentarySettledWeightCutArmed);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 12; ++step) {
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
  CHECK(machineRunState() != MachineRunState::RUNNING);
  CHECK(momentarySettledWeightCutArmed);
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_weight_cut_pulses_after_confirmed_expires_to_assumed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  holdStableWeight(600);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  machineArmSettledWeightCutOff();
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore + 1);
}

void t_weight_cut_pulses_on_running_unconfirmed_after_first_drop() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  session.firstDropMs = millis();
  runLoopAfter(20);
  CHECK(!machineAllowsFirmwareStopPulse());
  machineArmSettledWeightCutOff();
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore + 1);
}

void t_user_stop_after_preinfusion_does_not_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  for (int step = 0; step < 41; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  momentaryUserStopThisCycle = true;
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(machineRequestStop());
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_accidental_touch_does_not_confirm_on() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  session.accidentalTouchHolding = true;
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
  CHECK(machineRunState() != MachineRunState::RUNNING);
}

void t_gap_skip_does_not_confirm_from_step() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  dripFreshWeight(2.2f, 600);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
}

void t_accepted_weight_drives_confirm_on() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  session.hasWeightAnchor = true;
  session.lastAcceptedWeightG = 0.2f;
  currentWeight = 40.0f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    session.lastAcceptedWeightG += 0.45f;
    currentWeightReceivedAtMs = hostMillis;
    ++currentWeightSequence;
    runLoopAfter(100);
  }
  CHECK(machineRunState() == MachineRunState::RUNNING);
}

void t_logical_elapsed_after_start_pulse_opens() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(machineElapsedMs() > 0);
  publishControlStatus();
  CHECK(publishedControlStatus.circuitElapsedMs > 0);
  CHECK(publishedControlStatus.machineRunning);
}

void t_orphan_wall_does_not_auto_pulse_user_still_mirrors() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 9000;
  seedFreshScaleWeight(0.0f);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  const size_t closedAtStart = hostRelayClosedWrites;
  for (int step = 0; step < 46; ++step) {
    dripFreshWeight(0.0f, 200);
  }
  CHECK(hostRelayClosedWrites == closedAtStart);
  CHECK(!machineAllowsFirmwareStopPulse());
  const size_t closedBeforeStop = hostRelayClosedWrites;
  pressDown();
  CHECK(hostRelayClosedWrites > closedBeforeStop);
  CHECK(getRelaySafetySnapshot().closed);
  releaseUp();
  CHECK(!getRelaySafetySnapshot().closed);
}

void t_brief_stale_demotes_confirmed_not_unknown() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  runLoopAfter(1200);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  CHECK(machineRunState() != MachineRunState::UNKNOWN);
  runLoopAfter(1600);
  CHECK(machineRunState() == MachineRunState::UNKNOWN);
}

void t_small_delta_does_not_confirm_on() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  currentWeight = 0.2f;
  currentWeightReceivedAtMs = hostMillis;
  currentWeightSequence = 1;
  runLoopAfter(50);
  for (int step = 0; step < 8; ++step) {
    dripFreshWeight(currentWeight + 0.08f, 100);
  }
  CHECK(machineRunState() != MachineRunState::RUNNING);
}

void t_confirmed_wall_pulses_once_and_ends_session() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 2000;
  seedFreshScaleWeight(0.0f);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(session.active);
  const size_t closedAtConfirm = hostRelayClosedWrites;
  for (int step = 0; step < 14; ++step) {
    if (!session.active) {
      break;
    }
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
  CHECK(!session.active);
  CHECK(hostRelayClosedWrites == closedAtConfirm + 1);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 80);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == closedAtConfirm + 1);
}

void t_noscale_hard_cap_idles_without_pulse_and_next_press_starts() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 2000;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  currentWeightSequence = 0;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  const size_t closedBeforeWall = hostRelayClosedWrites;
  runLoopAfter(2200);
  CHECK(session.active);
  CHECK(hostRelayClosedWrites == closedBeforeWall);
  while (elapsedMs(momentaryLogicalRunStartedAtMs) < HARD_MAX_CIRCUIT_CLOSED_MS) {
    runLoopAfter(200);
  }
  runLoopAfter(50);
  CHECK(!session.active);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!momentaryOrphanRun);
  CHECK(!machineIsRunning());
  CHECK(hostRelayClosedWrites == closedBeforeWall);
  CHECK(lastCycle.endReason == EndReason::UNCONFIRMED_START);
  CHECK(stopperState == StopperState::READY);
  CHECK(controlAllowsConfigurationNow());
  shortPress(150);
  CHECK(session.active);
  CHECK(machineIsRunning());
}

void t_timer_only_confirmed_skips_operational_wall_pulses_at_hard_cap() {
  resetMomentaryHarness();
  armLiveScaleTimerOnlyStart(0.0f);
  runtimeConfig.operationalWallMs = 2000;
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  confirmEspressoFlow();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(session.active);
  CHECK(session.config.timerOnly);
  const size_t closedAtConfirm = hostRelayClosedWrites;
  for (int step = 0; step < 14; ++step) {
    if (!session.active) {
      break;
    }
    dripFreshWeight(currentWeight + 0.45f, 100);
  }
  CHECK(session.active);
  CHECK(hostRelayClosedWrites == closedAtConfirm);
  while (elapsedMs(momentaryLogicalRunStartedAtMs) < HARD_MAX_CIRCUIT_CLOSED_MS) {
    dripFreshWeight(currentWeight + 0.45f, 200);
  }
  dripFreshWeight(currentWeight + 0.45f, 50);
  CHECK(!session.active);
  CHECK(hostRelayClosedWrites == closedAtConfirm + 1);
  CHECK(lastCycle.endReason == EndReason::GLOBAL_LIMIT);
}

void t_timer_only_unconfirmed_idles_at_hard_cap_without_pulse() {
  resetMomentaryHarness();
  armLiveScaleTimerOnlyStart(0.0f);
  runtimeConfig.operationalWallMs = 2000;
  shortPress(150);
  CHECK(session.active);
  CHECK(stopperState == StopperState::BREW);
  CHECK(session.config.timerOnly);
  const size_t closedBefore = hostRelayClosedWrites;
  dripUntilHardCap(noFlowIdleFlatWeight);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
  CHECK(!machineIsRunning());
  CHECK(lastCycle.endReason == EndReason::UNCONFIRMED_START);
  CHECK(hostRelayClosedWrites == closedBefore);
}
#endif

void t_forced_pulse_has_no_logical_side_effects() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  const StopperState stateBefore = stopperState;
  const MachineRunState machineBefore = machineRunState();
  const uint32_t sessionIdBefore = session.id;
  const bool sessionActiveBefore = session.active;
  WebCommand force;
  force.type = WebCommandType::FORCE_SWITCH_PULSE;
  processWebCommand(force);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputKind == FirmwarePulseKind::FORCED);
  CHECK(stopperState == stateBefore);
  CHECK(machineRunState() == machineBefore);
  CHECK(session.id == sessionIdBefore);
  CHECK(session.active == sessionActiveBefore);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
  CHECK(!momentaryFirmwareCutPending);
#endif
}

void t_forced_pulse_queues_once_behind_active_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS, true));
  CHECK(pulseOutputKind == FirmwarePulseKind::START);
  CHECK(machineRequestForcedPulse());
  CHECK(firmwarePulsePending);
  CHECK(firmwarePulsePendingKind == FirmwarePulseKind::FORCED);
  CHECK(!machineRequestForcedPulse());
  runLoopAfter(runtimeStopPulseMs(runtimeConfig) + 1);
  CHECK(!pulseOutputActive);
  CHECK(firmwarePulsePending);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS - 1);
  CHECK(!pulseOutputActive);
  runLoopAfter(2);
  CHECK(pulseOutputActive);
  CHECK(pulseOutputKind == FirmwarePulseKind::FORCED);
}

void t_physical_press_cancels_pending_forced_pulse() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS, true));
  CHECK(machineRequestForcedPulse());
  CHECK(firmwarePulsePending);
  setRawPaddle(true);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(!firmwarePulsePending);
  releaseUp();
}

void t_forced_pulse_respects_relay_lockout() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  relaySafetyState = RelaySafetyState::LOCKOUT;
  const size_t closedBefore = hostRelayClosedWrites;
  CHECK(!machineRequestForcedPulse());
  CHECK(!pulseOutputActive);
  CHECK(hostRelayClosedWrites == closedBefore);
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
void t_reed_polarity_stop_when_running() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  setRawReed(true);
  activatorOn = false;
  CHECK(machineIsRunning());
  shortPress(180);
  CHECK(!momentaryStartEdgeThisCycle);
  CHECK(momentaryUserStopThisCycle || !activatorOn);
}

void t_reed_stays_running_unconfirmed_until_timeout_then_confirms_idle() {
  resetMomentaryHarness();
  runtimeConfig.reedConfirmTimeoutHundredMs = 2;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  CHECK(session.active);
  CHECK(!reedIsOn());
  runLoopAfter(100);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  const size_t closedBefore = hostRelayClosedWrites;
  runLoopAfter(150);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
  CHECK(!machineIsRunning());
  CHECK(hostRelayClosedWrites == closedBefore);
}

void t_reed_on_during_window_confirms_immediately() {
  resetMomentaryHarness();
  runtimeConfig.reedConfirmTimeoutHundredMs = 20;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  setRawReed(true);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(reedIsOn());
}

void t_reed_stop_idle_unconfirmed_until_reed_matches() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  setRawReed(true);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  releaseUp();
  pressDown();
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(reedIsOn());
  setRawReed(false);
  CHECK(machineRunState() == MachineRunState::IDLE);
}

void t_reed_stop_timeout_confirms_actual_on() {
  resetMomentaryHarness();
  runtimeConfig.reedConfirmTimeoutHundredMs = 2;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  setRawReed(true);
  releaseUp();
  pressDown();
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  runLoopAfter(250);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(reedIsOn());
}

void t_reed_outside_assumed_follows_reed() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(machineRunState() == MachineRunState::IDLE);
  setRawReed(true);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  setRawReed(false);
  CHECK(machineRunState() == MachineRunState::IDLE);
}

void t_reed_release_mode_assume_clock_starts_on_release() {
  resetMomentaryHarness();
  runtimeConfig.momentaryStartOnPress = false;
  runtimeConfig.reedConfirmTimeoutHundredMs = 5;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
  runLoopAfter(400);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
  releaseUp();
  CHECK(session.active);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  runLoopAfter(150);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  runLoopAfter(400);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!session.active);
}

void t_reed_release_mode_stop_assume_starts_on_release() {
  resetMomentaryHarness();
  runtimeConfig.momentaryStartOnPress = false;
  runtimeConfig.reedConfirmTimeoutHundredMs = 20;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  setRawReed(true);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(session.active);
  pressDown();
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(reedIsOn());
  releaseUp();
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(reedIsOn());
}

void t_reed_disqualified_press_grace_then_reed_canonical() {
  resetMomentaryHarness();
  runtimeConfig.maxSinglePressHundredMs = 2;
  runtimeConfig.reedConfirmTimeoutHundredMs = 3;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(session.active);
  CHECK(machineRunState() == MachineRunState::RUNNING_UNCONFIRMED);
  runLoopAfter(250);
  CHECK(!session.active);
  CHECK(machineRunState() == MachineRunState::IDLE);
  CHECK(!machineIsRunning());
  setRawReed(true);
  CHECK(machineRunState() == MachineRunState::IDLE_UNCONFIRMED);
  CHECK(!machineIsRunning());
  runLoopAfter(400);
  CHECK(machineRunState() == MachineRunState::RUNNING);
  CHECK(machineIsRunning());
  CHECK(!session.active);
}

void t_reed_does_not_arm_no_flow_idle() {
  resetMomentaryHarness();
  CHECK(!machineTakeNoFlowIdle());
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(HARD_MAX_CIRCUIT_CLOSED_MS + 100);
  CHECK(!machineTakeNoFlowIdle());
}

void t_reed_wall_pulses_once_and_ends_session() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 2000;
  seedFreshScaleWeight(0.0f);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  setRawReed(true);
  CHECK(session.active);
  const size_t closedBeforeWall = hostRelayClosedWrites;
  runLoopAfter(2200);
  CHECK(!session.active);
  CHECK(hostRelayClosedWrites == closedBeforeWall + 1);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 80);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == closedBeforeWall + 1);
}

void t_reed_manual_skips_operational_wall_pulses_at_hard_cap() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 2000;
  runtimeConfig.noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  scale.connected = false;
  setScaleLinkState(ScaleLinkState::DISCONNECTED);
  currentWeightSequence = 0;
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 50);
  setRawReed(true);
  CHECK(session.active);
  CHECK(stopperState == StopperState::MANUAL_NO_SCALE);
  const size_t closedBeforeWall = hostRelayClosedWrites;
  runLoopAfter(2200);
  CHECK(session.active);
  CHECK(hostRelayClosedWrites == closedBeforeWall);
  while (elapsedMs(momentaryLogicalRunStartedAtMs) < HARD_MAX_CIRCUIT_CLOSED_MS) {
    runLoopAfter(200);
  }
  runLoopAfter(50);
  CHECK(!session.active);
  CHECK(hostRelayClosedWrites == closedBeforeWall + 1);
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(COMPILED_STOP_PULSE_MS + 80);
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(hostRelayClosedWrites == closedBeforeWall + 1);
}
#endif

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
void t_reed_getters_use_sampled_state() {
  resetMomentaryHarness();
  hostPinLevel[REED_GPIO] = REED_ACTIVE_LEVEL;
  machineSampleInput();
  hostMillis += REED_DEBOUNCE_MS + 1;
  machineSampleInput();
  CHECK(reedOn);
  CHECK(machineIsRunning());
  CHECK(machineRunState() == MachineRunState::RUNNING);
  hostPinLevel[REED_GPIO] = !REED_ACTIVE_LEVEL;
  CHECK(reedOn);
  CHECK(machineIsRunning());
  CHECK(machineRunState() == MachineRunState::RUNNING);
  serviceMomentaryRunSensors();
  hostMillis += REED_DEBOUNCE_MS + 1;
  serviceMomentaryRunSensors();
  CHECK(!reedOn);
  CHECK(!machineIsRunning());
  CHECK(machineRunState() == MachineRunState::IDLE);
}
#endif

void t_logical_wall_trips_existing_flags() {
  resetMomentaryHarness();
  runtimeConfig.operationalWallMs = 2000;
  seedFreshScaleWeight(0.0f);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  shortPress(150);
  CHECK(session.active);
  CHECK(momentaryLogicalRunActive);
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  setRawReed(true);
  CHECK(momentaryLogicalRunActive);
#endif
  runLoopAfter(2200);
  CHECK(getRelaySafetySnapshot().operationalTripped ||
        operationalLimitTripped || !momentaryLogicalRunActive);
}

void t_compatibility_mode_mirrors_without_session() {
  resetMomentaryHarness();
  firmwareModeRaw =
      static_cast<uint8_t>(openbrewbyweight::FirmwareMode::COMPATIBILITY);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(stopperState == StopperState::READY);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  const size_t openWrites = hostRelayOpenWrites;
  const RelaySafetySnapshot held = getRelaySafetySnapshot();
  runLoopAfter(90000);
  CHECK(getRelaySafetySnapshot().closed);
  CHECK(getRelaySafetySnapshot().generation == held.generation);
  CHECK(getRelaySafetySnapshot().hardLimitMs == 0);
  CHECK(!independentSafetyTimer.running() && !relaySafetyTimer->active);
  CHECK(hostRelayOpenWrites == openWrites && !session.active && !pulseOutputActive);
  releaseUp();
  CHECK(!getRelaySafetySnapshot().closed);
  CHECK(!session.active);
  // Web actuation is rejected while the firmware is disabled.
  hostForwardAcceptedNetworkCommandSucceeds = false;
  controlResultPending = false;
  controlResultCommand = WebCommand{};
  WebCommand remoteOn;
  remoteOn.type = WebCommandType::REMOTE_ON;
  remoteOn.requestId = 7;
  processWebCommand(remoteOn);
  CHECK(controlResultPending);
  CHECK(controlResultCommand.resultState == CommandResultState::FAILED);
  hostForwardAcceptedNetworkCommandSucceeds = true;
}

void t_compatibility_boot_hold_waits_for_stable_off() {
  resetMomentaryHarness();
  firmwareModeRaw = static_cast<uint8_t>(FirmwareMode::COMPATIBILITY);
  hostPinLevel[ACTIVATOR_GPIO] = ACTIVATOR_ACTIVE_LEVEL;
  initializeActivatorInput();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  CHECK(!getRelaySafetySnapshot().closed && hostRelayClosedWrites == 0);
  runLoopAfter(90000);
  CHECK(!getRelaySafetySnapshot().closed && hostRelayClosedWrites == 0);
  releaseUp();
  CHECK(stopperState == StopperState::READY);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  releaseUp();
  CHECK(!getRelaySafetySnapshot().closed);
}

void t_compatibility_trip_stays_open_until_release() {
  resetMomentaryHarness();
  firmwareModeRaw = static_cast<uint8_t>(FirmwareMode::COMPATIBILITY);
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  tripRelaySafety(RelaySafetyFault::GPIO_DESYNC, true, false, false);
  const size_t closedWrites = hostRelayClosedWrites;
  runLoopAfter(0);
  CHECK(!getRelaySafetySnapshot().closed && hostRelayClosedWrites == closedWrites);
  CHECK(stopperState == StopperState::REQUIRES_OFF);
  runLoopAfter(90000);
  CHECK(!getRelaySafetySnapshot().closed && hostRelayClosedWrites == closedWrites);
  releaseUp();
  CHECK(stopperState == StopperState::READY);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
}

void t_full_mode_held_button_stays_open_after_hard_limit() {
  resetMomentaryHarness();
  runLoopAfter(ACTIVATOR_DEBOUNCE_MS + 1);
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
  runLoopAfter(HARD_MAX_CIRCUIT_CLOSED_MS + 10);
  CHECK(!getRelaySafetySnapshot().closed);
  runLoopAfter(90000);
  CHECK(!getRelaySafetySnapshot().closed);
  releaseUp();
  pressDown();
  CHECK(getRelaySafetySnapshot().closed);
}

struct TestCase {
  const char *id;
  void (*function)();
};

const TestCase kTests[] = {
    {"P01", t_short_press_mirrors_then_opens},
    {"P36", t_default_starts_on_press},
    {"P40", t_release_mode_starts_on_release_not_press},
    {"IT05", t_idle_tare_yields_to_configured_start_edge},
    {"P38", t_second_short_press_stops_without_rinse},
    {"P52", t_noscale_last_shot_keeps_logical_duration},
    {"P02", t_guard_reject_does_not_mirror},
    {"P50", t_no_scale_bbw_armed_does_not_mirror_then_idle_allows},
    {"P50B", t_no_scale_bbw_release_mode_short_press_consumes_warning},
    {"P66", t_require_scale_triple_press_release_never_starts_on_final_release},
    {"P39", t_user_stop_without_session_does_not_leave_orphan_run},
    {"P03", t_long_press_mirrors_from_first_instant},
    {"P41", t_release_mode_long_press_ignored_as_start},
    {"P60", t_rinse_press_demotes_and_pulses},
    {"P61", t_rinse_release_mode_starts_directly},
    {"P62", t_rinse_does_not_start_during_running_shot},
    {"P63", t_rinse_armed_noscale_long_press_consumes_guard},
    {"P63B", t_rinse_armed_noscale_long_press_allowed_runs},
    {"P63C", t_rinse_armed_noscale_release_mode_does_not_run},
    {"P64", t_rinse_release_mode_mid_hold_is_native_not_shot},
    {"P65", t_rinse_end_aborts_start_pulse_for_stop_pulse},
    {"P68", t_rinse_web_over_running_shot_keeps_full_duration},
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 1
    {"P04", t_only_auto_cut_needs_running},
    {"P05", t_quiet_pan_does_not_force_cut_when_unknown},
    {"P09", t_tare_rebases_flow_signature},
    {"P09B", t_fast_control_loop_still_confirms_running},
    {"P09C", t_start_tare_drop_rebases_then_confirms},
    {"P10", t_user_stop_does_not_extra_pulse_when_running_unconfirmed},
    {"P11", t_wall_with_quiet_pan_does_not_auto_pulse},
    {"P12", t_settings_lock_follows_logical_run},
    {"P13", t_polarity_resyncs_to_running_machine},
    {"P14", t_user_press_wins_over_firmware_pulse},
    {"P18", t_start_ack_stays_assumed_without_flow},
    {"P21", t_running_expires_without_flow},
    {"P22", t_stop_ack_quiet_confirms_idle},
    {"P23", t_stop_ack_timeout_without_quiet_stays_idle_unconfirmed},
    {"P24", t_start_preinfusion_quiet_stays_assumed},
    {"P25", t_start_nack_long_baseline_confirms_idle},
    {"P25B", t_start_nack_timeout_follows_setting},
    {"P25C", t_flow_after_idle_unconfirmed_confirms_running_without_pulse},
    {"P25I", t_start_nack_quiet_does_not_idle},
    {"P25I7", t_no_flow_hard_cap_idles_after_nack},
    {"P25I8", t_no_flow_hard_cap_idles_running_unconfirmed_noise},
    {"P25I9", t_no_flow_hard_cap_skips_when_espresso_confirmed},
    {"P25I12", t_no_flow_hard_cap_skips_after_espresso_pan_returns},
    {"P25I10", t_no_flow_hard_cap_skips_when_mass_above_band},
    {"P25I11", t_no_flow_hard_cap_skips_when_stale},
    {"P25I2", t_cup_removed_settles_idle_unconfirmed_after_stop},
    {"P25I3", t_cup_removed_settles_start_nack_idle_unconfirmed},
    {"P25I4", t_cup_removed_ignored_while_running_unconfirmed},
    {"P25I5", t_cup_removed_ignored_while_running},
    {"P25I6", t_cup_removed_event_settles_idle_unconfirmed_after_stop},
    {"P25J", t_firmware_cut_settles_off_after_drip},
    {"P25K", t_firmware_cut_settles_off_without_pending_finalize},
    {"P25L", t_firmware_cut_stale_weight_stays_idle_unconfirmed},
    {"P25M", t_gusher_flow_confirms_running},
    {"P25D", t_override_sets_inferred_idle_and_brewing_without_pulse},
    {"P25D2", t_override_on_refused_during_firmware_stop},
    {"P25R", t_remote_start_emits_synthetic_start_pulse},
    {"P25S", t_web_stop_emits_pulse_while_running_unconfirmed},
    {"P25E", t_scale_connect_settles_idle_when_idle},
    {"P25F", t_settled_weight_cut_confirms_idle},
    {"P25G", t_settled_weight_cut_stays_armed_while_pouring},
    {"P25H", t_settled_weight_cut_flow_is_not_polarity},
    {"P25N", t_weight_cut_pulses_after_confirmed_expires_to_assumed},
    {"P25O", t_weight_cut_pulses_on_running_unconfirmed_after_first_drop},
    {"P26", t_user_stop_after_preinfusion_does_not_pulse},
    {"P27", t_accidental_touch_does_not_confirm_on},
    {"P28", t_gap_skip_does_not_confirm_from_step},
    {"P29", t_accepted_weight_drives_confirm_on},
    {"P30", t_logical_elapsed_after_start_pulse_opens},
    {"P31", t_orphan_wall_does_not_auto_pulse_user_still_mirrors},
    {"P32", t_brief_stale_demotes_confirmed_not_unknown},
    {"P33", t_small_delta_does_not_confirm_on},
    {"P34", t_confirmed_wall_pulses_once_and_ends_session},
    {"P51", t_noscale_hard_cap_idles_without_pulse_and_next_press_starts},
    {"P53", t_timer_only_confirmed_skips_operational_wall_pulses_at_hard_cap},
    {"P54", t_timer_only_unconfirmed_idles_at_hard_cap_without_pulse},
#endif
    {"CM10", t_compatibility_mode_mirrors_without_session},
    {"CM11", t_compatibility_boot_hold_waits_for_stable_off},
    {"CM12", t_compatibility_trip_stays_open_until_release},
    {"CM13", t_full_mode_held_button_stays_open_after_hard_limit},
    {"P25T", t_forced_pulse_has_no_logical_side_effects},
    {"P25U", t_forced_pulse_queues_once_behind_active_pulse},
    {"P25V", t_physical_press_cancels_pending_forced_pulse},
    {"P25W", t_forced_pulse_respects_relay_lockout},
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
    {"P06", t_reed_off_blocks_firmware_cut},
    {"P07", t_reed_on_allows_firmware_cut},
    {"P15", t_reed_boot_on_is_running},
    {"P15B", t_reed_safety_trip_follows_reed},
    {"P16", t_reed_off_wins_over_rising_weight},
    {"P17", t_running_unconfirmed_offers_web_stop},
    {"P19", t_reed_polarity_stop_when_running},
    {"P35", t_reed_wall_pulses_once_and_ends_session},
    {"P57", t_reed_manual_skips_operational_wall_pulses_at_hard_cap},
    {"P42", t_reed_stays_running_unconfirmed_until_timeout_then_confirms_idle},
    {"P43", t_reed_on_during_window_confirms_immediately},
    {"P44", t_reed_stop_idle_unconfirmed_until_reed_matches},
    {"P45", t_reed_stop_timeout_confirms_actual_on},
    {"P46", t_reed_outside_assumed_follows_reed},
    {"P47", t_reed_release_mode_assume_clock_starts_on_release},
    {"P48", t_reed_release_mode_stop_assume_starts_on_release},
    {"P49", t_reed_disqualified_press_grace_then_reed_canonical},
    {"P49B", t_reed_does_not_arm_no_flow_idle},
    {"P70", t_reed_getters_use_sampled_state},
#endif
    {"P20", t_boot_hold_released_in_grace_never_drives_relay},
    {"P08", t_logical_wall_trips_existing_flags},
};

}  // namespace

int main() {
  static_assert(OPEN_BREW_BY_WEIGHT_MACHINE_TYPE != 0,
                "momentary host tests require OPEN_BREW_BY_WEIGHT_MACHINE_TYPE 1 or 2");
  for (const TestCase &test : kTests) {
    const int failuresBefore = failures;
    test.function();
    ++testsRun;
    std::cout << test.id << (failures == failuresBefore ? " PASS" : " FAIL")
              << "\n";
  }
  deleteHostResources();
  std::cout << testsRun << " tests, " << failures << " failures\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Arduino entry points
// ---------------------------------------------------------------------------

bool usbConsoleJumperPresent() {
  if (!USB_CONSOLE_JUMPER_PRESENT) {
    return false;
  }
  pinMode(USB_CONSOLE_GPIO, INPUT_PULLUP);
  return digitalRead(USB_CONSOLE_GPIO) == USB_CONSOLE_ACTIVE_LEVEL;
}

void serviceHealthProfiler(uint32_t now) {
  const HealthProfilerRequest profiler =
      healthProfilerRequest.exchange(HealthProfilerRequest::NONE,
                                     std::memory_order_acq_rel);
  if (profiler == HealthProfilerRequest::START) {
    (void)taskProfiler.start(now);
  } else if (profiler == HealthProfilerRequest::STOP) {
    (void)taskProfiler.stop(now);
  }
  taskProfiler.service(now);
}

// Bounded capture-context snapshot taken when a recording starts. Only
// lock-protected snapshots are read; control characters are stripped so user
// text can never inject extra TXT lines.
uint32_t composeScaleProfileContext(char *text, size_t capacity) {
  if (text == nullptr || capacity == 0) return 0;
  // The ~7.4 KiB snapshot fits neither the 4 KiB health stack (it overflowed
  // into adjacent heap TCBs; see
  // docs/audits/2026-10-02-scale-profile-start-panic.md) nor the internal-DRAM
  // budget as a static, so stage it in PSRAM for this once-per-Start call.
  // An empty context is the accepted degradation when PSRAM is exhausted.
  auto *control = static_cast<ControlStatusSnapshot *>(allocExternal(
      sizeof(ControlStatusSnapshot), AllocationOwner::PROFILER));
  if (control == nullptr) return 0;
  copyControlStatus(*control);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  int used = snprintf(text, capacity,
                      "firmware=%s\nboard=%s\nmachine=%s / %s\n", FW_VERSION,
                      OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID,
                      OPEN_BREW_BY_WEIGHT_MACHINE_BRAND, OPEN_BREW_BY_WEIGHT_MACHINE_MODEL);
  if (used > 0 && static_cast<size_t>(used) < capacity) {
    used += snprintf(text + used, capacity - used, "scale=%s name=%s\n",
                     link.protocolName, link.shotScaleName);
  }
  if (used > 0 && static_cast<size_t>(used) < capacity) {
    used += snprintf(
        text + used, capacity - used,
        "settings=goalG:%.1f offsetG:%d autoTare:%d autoRetare:%d cupProt:%d "
        "stopIfRemoved:%d requireCup:%d touch:%d graceMs:%lu\n",
        static_cast<double>(goalWeightDgToGrams(control->config.goalWeightDg)),
        static_cast<int>(control->config.weightOffsetG),
        control->config.autoTare ? 1 : 0, control->config.autoRetare ? 1 : 0,
        control->config.cupProtectionEnabled ? 1 : 0,
        control->config.stopIfCupRemoved ? 1 : 0,
        control->config.requireCupToStart ? 1 : 0,
        control->config.avoidAccidentalTouchEnabled ? 1 : 0,
        static_cast<unsigned long>(control->config.postTareBaselineGraceMs));
  }
  if (used > 0 && static_cast<size_t>(used) < capacity) {
    snprintf(text + used, capacity - used,
             "state=cupPresent:%d weightControl:%d\n",
             control->cupPresent ? 1 : 0,
             static_cast<int>(control->weightControlState));
  }
  heapCapsFree(control);
  for (char *c = text; *c != '\0'; ++c) {
    // Keep newlines: the TXT emitter controls line boundaries and splits on
    // them. Strip every other control character.
    if (*c != '\n' &&
        (static_cast<unsigned char>(*c) < 0x20 || *c == 0x7f)) {
      *c = ' ';
    }
  }
  return g_wallClock.synced() ? g_wallClock.nowUtcSec(millis()) : 0;
}

void serviceScaleProfiler(uint32_t now) {
  const ScaleProfilerRequest request = scaleProfileRequest.exchange(
      ScaleProfilerRequest::NONE, std::memory_order_acq_rel);
  const ScaleProfilerWork work = scaleProfiler().service(request, now);
  if (work != ScaleProfilerWork::NONE && !dispatchScaleProfileWork()) {
    scaleProfiler().noteWorkDispatchFailed();
  }
}

void healthWorkerTask(void *) {
  hwmon.begin();
  uint32_t version = 0;
  uint32_t sampledAtMs = 0;
  uint32_t heapLowSinceMs = 0;
  for (;;) {
    const uint32_t now = millis();
    serviceBootHeap(now);
    serviceHealthProfiler(now);
    serviceScaleProfiler(now);
    if (sampledAtMs == 0 || elapsedMs(sampledAtMs) >= HEALTH_TELEMETRY_INTERVAL_MS) {
      HealthWorkerSample sample;
      sample.version = ++version;
      if (sample.version == 0) sample.version = ++version;
      sample.sampledAtMs = now;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
      sample.heap = sampleHeapCaps();
      sample.heapValid = true;
      const OpenBrewByWeightBleHealth ble = openBrewByWeightBleRuntimeHealth(false);
      sample.bleRuntimeState = static_cast<uint8_t>(ble.state);
      sample.bleRuntimeLastError = ble.lastError;
      sample.bleRuntimeLastResetReason = ble.lastResetReason;
      sample.bleRuntimeSyncGeneration = ble.syncGeneration;
      sample.bleRuntimeResetCount = ble.resetCount;
      sample.bleRuntimeHostStackMinBytes = ble.hostTaskStackHighWaterBytes;
      sample.hwmon = hwmon.sample(
          sampledAtMs == 0 ? HEALTH_TELEMETRY_INTERVAL_MS
                           : static_cast<uint32_t>(now - sampledAtMs),
          &sample.heap);
#else
      sample.heapValid = true;
      sample.heap.internalFree = freeHeapBytes;
      sample.heap.internalLargest = largestFreeHeapBlockBytes;
      sample.hwmon = hwmon.sample(HEALTH_TELEMETRY_INTERVAL_MS);
#endif
      const bool heapLow =
          sample.heapValid &&
          (sample.heap.internalFree < HEALTH_HEAP_FREE_ALERT_BYTES ||
           sample.heap.internalLargest < HEALTH_HEAP_LARGEST_ALERT_BYTES);
      if (heapLow) {
        if (heapLowSinceMs == 0) heapLowSinceMs = now;
        sample.restartRequested =
            elapsedMs(heapLowSinceMs) >= HEALTH_HEAP_LOW_RESTART_MS;
      } else {
        heapLowSinceMs = 0;
      }
      (void)xQueueOverwrite(healthSampleQueue, &sample);
      sampledAtMs = now;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool initializeHealthWorker() {
  if (healthSampleQueue != nullptr && healthTaskHandle != nullptr) return true;
  healthSampleQueue = xQueueCreate(1, sizeof(HealthWorkerSample));
  if (healthSampleQueue == nullptr) return false;
  if (xTaskCreatePinnedToCore(healthWorkerTask, "health", 4096, nullptr,
                              tskIDLE_PRIORITY + HEALTH_TASK_PRIORITY_OFFSET,
                              &healthTaskHandle, 0) != pdPASS ||
      healthTaskHandle == nullptr) {
    vQueueDelete(healthSampleQueue);
    healthSampleQueue = nullptr;
    return false;
  }
  return true;
}

void serviceHealthWorkerSample() {
  HealthWorkerSample sample;
  if (healthSampleQueue != nullptr &&
      xQueueReceive(healthSampleQueue, &sample, 0) == pdTRUE) {
    healthSnapshotVersion = sample.version;
    healthSnapshotAtMs = sample.sampledAtMs;
    healthSnapshotValid = sample.heapValid && sample.version != 0 &&
        elapsedMs(sample.sampledAtMs) <= HEALTH_SAMPLE_STALE_MS;
    freeHeapBytes = sample.heap.internalFree;
    minimumFreeHeapBytes = sample.heap.internalMinimum;
    largestFreeHeapBlockBytes = sample.heap.internalLargest;
    internalHeapAllocatedBlocks = sample.heap.internalAllocatedBlocks;
    internalHeapFreeBlocks = sample.heap.internalFreeBlocks;
    internalHeapFragmentationPermille = sample.heap.internalFragmentationPermille;
    psramSizeBytes = sample.heap.psramTotal;
    psramFreeBytes = sample.heap.psramFree;
    psramMinimumFreeBytes = sample.heap.psramMinimum;
    psramLargestFreeBlockBytes = sample.heap.psramLargest;
    bleRuntimeState = sample.bleRuntimeState;
    bleRuntimeLastError = sample.bleRuntimeLastError;
    bleRuntimeLastResetReason = sample.bleRuntimeLastResetReason;
    bleRuntimeSyncGeneration = sample.bleRuntimeSyncGeneration;
    bleRuntimeResetCount = sample.bleRuntimeResetCount;
    bleRuntimeHostStackMinBytes = sample.bleRuntimeHostStackMinBytes;
    hwmonSnapshot = sample.hwmon;
    loopStackMinBytes =
        static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
    serviceHealthThresholdAlerts(healthIntervalMaxGapMs);
    loopIntervalGapMs = healthIntervalMaxGapMs;
    loopPhaseProfiler.publishIntervalGap(loopIntervalGapMs);
    healthIntervalMaxGapMs = 0;
    if (healthSnapshotValid && sample.restartRequested && !healthHeapRestartLatched &&
        controlAllowsConfigurationNow() && !circuitClosed) {
      healthHeapRestartLatched = true;
      addDebugEvent(DebugCategory::SYSTEM, DebugCode::HEALTH_HEAP_RESTART,
                    static_cast<int32_t>(freeHeapBytes),
                    static_cast<int32_t>(largestFreeHeapBlockBytes));
      requestSafeRestart();
    }
  }
  healthSnapshotValid =
      healthSnapshotValid &&
      elapsedMs(healthSnapshotAtMs) <= HEALTH_SAMPLE_STALE_MS;
  if (!healthSnapshotValid) hwmonSnapshot.cpuLoadValid = false;
}

void setup() {
  bootStartedAtMs = millis();
  bootCapabilities = BootCapabilities{};
  bootState = BootState::BOOTING;
  bootDegraded = false;
  firmwareInitializationComplete = false;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  // setup() and loop() run in loopTask; raise it before any worker exists.
  vTaskPrioritySet(nullptr, tskIDLE_PRIORITY + CONTROL_TASK_PRIORITY_OFFSET);
  // cJSON hooks are process-wide; install the external allocator before any
  // worker task can parse concurrently.
  initJsonParser();
#endif
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  // Host tests may invoke setup() after constructing a ready harness. Its task
  // stub never runs, so tear down that synthetic worker before boot injection.
  releaseSettingsPersistenceWorkerForHost();
  delete relaySafetyTimer;
  delete operationalLimitTimer;
  relaySafetyTimer = nullptr;
  operationalLimitTimer = nullptr;
  independentSafetyTimer.resetForHost();
#endif
  // Safe OPEN before Serial, EEPROM or BLE. Arduino-ESP32 3.x rejects
  // digitalWrite until the pad is a GPIO, so the OPEN level is latched by
  // direct register write before the pin becomes an output.
  machineInitialize();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  crashArchivePrepareBoot();
#endif
  initializeScaleConnectedLed();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  set_arduino_panic_handler(openBrewByWeightPanicHandler, nullptr);
#endif
  safetyResetStatus = beginSafetyResetGuard();

  if (SPEAKER_PRESENT) {
    localBuzzer.begin(BUZZER_GPIO);
  }

  platformClockReady = setCpuFrequencyMhz(80) &&
                       applyPowerProfile(PowerProfile::OFF);
  relaySafetyTimersReady = initializeRelaySafetyTimer();
  taskWatchdogReady =
      configureTaskWatchdog() && subscribeCurrentTaskToWatchdog();
  initializeRelaySafetyStateAfterBoot();

  if (EXTERNAL_SAFETY_HARDWARE_PRESENT && readCircuitFeedbackClosed()) {
    tripRelaySafety(RelaySafetyFault::FEEDBACK_STUCK_CLOSED);
  }

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  // A pending destructive operation always resumes before normal storage,
  // BLE, Wi-Fi or HTTP startup. A new gesture is accepted only on cold power.
  initializeSettingsSchemaWriteAdmission();
  resumePendingBootRecovery();
  maybeRunBootRecoveryGesture();
  machineOnActivatorReady();
#endif

  // Default builds keep USB Serial/JTAG off until GPIO4 is jumpered to GND.
  // Serial is HWCDC (native USB), not UART0 — see OpenBrewByWeightUsbConsole.h.
  // -DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1 starts CDC at boot (OpenOCD + CLI, no jumper).
  // Latch the enable source for Diagnostic; live IO4 is re-read each status.
  if (OPEN_BREW_BY_WEIGHT_ENABLE_JTAG == 1) {
    usbSerialEnableSource = UsbSerialEnableSource::COMPILE_FLAG;
  } else if (usbConsoleJumperPresent()) {
    usbSerialEnableSource = UsbSerialEnableSource::JUMPER;
  }
  const bool usbSerial = usbSerialEnableSource != UsbSerialEnableSource::OFF;
  if (usbSerial) Serial.begin(SERIAL_BAUD);
  installEspLogSink(usbSerial);

  persistenceReady = EEPROM.begin(EEPROM_SIZE);
  bool flashIoReady = true;
  bool settingsLoaded = persistenceReady;
  bool settingsDurable = persistenceReady;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  flashIoReady = ensureFlashIoMutex();
  if (!flashIoReady) {
    addDebugEvent(DebugCategory::CONFIG, DebugCode::INITIALIZATION_FAILED,
                  BOOT_SUBSYSTEM_PERSISTENCE);
  }
  // RTC captures the prior uptime immediately across warm resets; NVS keeps
  // the history when RTC memory is lost (for example after power loss).
  persistResetHistoryAfterBoot(safetyResetStatus);
  settingsLoaded = false;
  settingsDurable = false;
  bool freshSettings = false;
  if (persistenceReady && loadPersistedSettings(persistedSettings)) {
    settingsLoaded = true;
    settingsDurable = true;
    noteDurableStorageRevision(persistedSettings.storageRevision);
    resolveSetupIntent(persistedSettings);
  } else if (persistenceReady) {
    if (initializeDefaultSettings(persistedSettings)) {
      settingsLoaded = true;
      freshSettings = true;
      settingsDurable = savePersistedSettings(persistedSettings);
      if (!settingsDurable) {
        addDebugEvent(DebugCategory::CONFIG, DebugCode::INITIALIZATION_FAILED,
                      BOOT_SUBSYSTEM_SETTINGS_SAVE);
      }
    }
  }
  // Master switch: read before any subsystem init so BLE, Micra, NTP, and the
  // control machine all see the final boot-scoped mode.
  firmwareModeRaw = static_cast<uint8_t>(loadFirmwareMode());
  if (!loadBleScanSettings(bleScanPersistedSettings)) {
    bleScanPersistedSettings = BleScanPersistedSettings{};
    if (!saveBleScanSettings(bleScanPersistedSettings)) {
      addDebugEvent(DebugCategory::CONFIG, DebugCode::INITIALIZATION_FAILED,
                    BOOT_SUBSYSTEM_SETTINGS_SAVE);
    }
  }
  applyLiveBleScanIntensity(clampBleScanIntensity(
      bleScanPersistedSettings.scanIntensity));
  // Compatibility mode keeps the BLE stack parked regardless of the persisted
  // Bluetooth setting; re-enabling the firmware restores it untouched.
  applyLiveBleEnabled(!firmwareCompatibilityMode() &&
                      bleScanPersistedSettings.enabled != 0);
  if (settingsLoaded) {
    runtimeConfig = persistedSettings.runtime;
    bullseyeMelodyConfig = persistedSettings.bullseyeMelody;
    presetBank = persistedSettings.presets;
    ensureShotPresetBank(presetBank, runtimeConfig.retareWindowMs,
                         runtimeConfig.autoRetare);
    QuickStartRecord storedQuickStart;
    if (bootQuickStart(bbwQuickStart, freshSettings, presetBank,
                       loadQuickStart(storedQuickStart) ? &storedQuickStart : nullptr))
      stageBbwQuickStart();
    // Stop mode follows the active preset only.
    runtimeConfig.timerOnly = !activeShotPreset(presetBank).brewByWeight;
    scaleWorkerLoadPreferred(persistedSettings.preferredScaleMac,
                             persistedSettings.preferredScaleName,
                             persistedSettings.scaleHistory);
    publishMachineIntegrationConfig(persistedSettings,
                                    persistedSettings.storageRevision);
  }
#else
  runtimeConfig = RuntimeConfig{};
  bullseyeMelodyConfig = BullseyeMelodyConfig{};
  seedDefaultShotPresetBank(presetBank);
#endif
  (void)localBuzzer.configureBullseyeRtttl(bullseyeMelodyConfig.rtttl);
  publishLogLevels(serialLogLevelFromRuntime(runtimeConfig),
                   static_cast<LogLevel>(runtimeConfig.ringRetainLogLevel));
  configureEspLogRuntime();
  publishRecipeState();
  publishScaleWorkerPolicy(runtimeConfig, false);

  logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_RESET_REASON,
          static_cast<int32_t>(safetyResetStatus.reasonCode));
  logEmit(relaySafetyTimersReady ? LogLevel::INFO : LogLevel::CRITICAL,
          DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
          BOOT_SUBSYSTEM_RELAY_TIMERS, relaySafetyTimersReady ? 1 : 0);
  logEmit(platformClockReady ? LogLevel::INFO : LogLevel::CRITICAL,
          DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM, BOOT_SUBSYSTEM_CPU,
          platformClockReady ? 1 : 0);
  if (!platformClockReady) {
    addDebugEvent(DebugCategory::SECURITY, DebugCode::INITIALIZATION_FAILED,
                  BOOT_SUBSYSTEM_CPU);
  }
  logEmit(taskWatchdogReady ? LogLevel::INFO : LogLevel::CRITICAL,
          DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
          BOOT_SUBSYSTEM_TASK_WDT, taskWatchdogReady ? 1 : 0);

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  bool bootIdentityDurable = false;
  {
    // ActivationStores owns the shot log, curves, and activation history:
    // finish every load before the network task can serve endpoints.
    TaskLockGuard storeLock(shotStoreMutex);
    bootIdentityDurable = activationStores.begin();
    lastShotStore.load();
    lastShotNvsDirty = false;
  }
  if (!bootIdentityDurable)
    addDebugEvent(DebugCategory::CONFIG, DebugCode::INITIALIZATION_FAILED,
                  BOOT_SUBSYSTEM_PERSISTENCE);
#else
  {
    TaskLockGuard storeLock(shotStoreMutex);
    activationStores.begin();
    lastShotStore.load();
    lastShotNvsDirty = false;
  }
#endif

  addDebugEvent(DebugCategory::BOOT, DebugCode::BOOT_BANNER,
                static_cast<int32_t>(activationStores.bootId()));
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  crashArchiveSetBootId(bootIdentityDurable ? activationStores.bootId() : 0);
  crashArchivePromote(bootIdentityDurable ? activationStores.bootId() : 0,
                      safetyResetStatus.reasonCode);
  // Read only the profiler's durable header at boot; records stay in flash
  // until a download streams them.
  scaleProfiler().bootInit();
  // Recover the durable audit window into the RAM latch before any producer
  // can emit; a missing partition degrades to latch-only auditing.
  {
    uint32_t auditPendingAfter = 0;
    auditStoreBootOpen(auditLog, auditPendingAfter);
    if (auditStoreStatus().state == AuditStoreState::UNSUPPORTED) {
      logEmit(LogLevel::INFO, DebugCategory::SYSTEM,
              DebugCode::BOOT_SUBSYSTEM, BOOT_SUBSYSTEM_PERSISTENCE, 2);
    }
  }
#endif
  scaleProfiler().setContextProvider(&composeScaleProfileContext);

  if (!persistenceReady) {
    addDebugEvent(DebugCategory::CONFIG, DebugCode::INITIALIZATION_FAILED,
                  BOOT_SUBSYSTEM_PERSISTENCE);
    logEmit(LogLevel::ERROR, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_PERSISTENCE, 0);
  } else {
    logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_PERSISTENCE, 1);
  }

  addDebugEvent(DebugCategory::BOOT, DebugCode::BOOT_RUNTIME_CONFIG,
                weightToCentigrams(
                    goalWeightDgToGrams(runtimeConfig.goalWeightDg)),
                weightToCentigrams(runtimeConfig.weightOffsetG));

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  ScaleWorkerBridgeCallbacks scaleBridge;
  scaleBridge.notifyAcquisition = notifyScaleAcquisition;
  scaleBridge.syncNetworkRf = syncScaleWorkerNetworkRf;
  scaleBridge.notifyWeightSample = [] { networkManager.noteScaleSample(); };
  const bool scaleBridgeOk = configureScaleWorkerBridge(scaleBridge);
#else
  const bool scaleBridgeOk = true;
#endif
  const bool machineIntegrationOk = initializeMachineIntegration();
  const bool scaleWorkerOk = scaleBridgeOk && initializeScaleWorker();
  if (!machineIntegrationOk) {
    logEmit(LogLevel::WARNING, DebugCategory::BOOT,
            DebugCode::INITIALIZATION_FAILED,
            BOOT_SUBSYSTEM_MACHINE_INTEGRATION);
  }
  if (!scaleWorkerOk) {
    logEmit(LogLevel::ERROR, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_SCALE_WORKER, 0);
  } else {
    logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_SCALE_WORKER, 1);
  }

  webCommandQueue =
      xQueueCreate(WEB_COMMAND_QUEUE_LENGTH, sizeof(WebCommand));
  const bool webQueueOk = webCommandQueue != nullptr;
  if (!webQueueOk) {
    logEmit(LogLevel::ERROR, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_WEB_QUEUE, 0);
  } else {
    logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_WEB_QUEUE, 1);
  }

  const bool settingsPersistenceOk = initializeSettingsPersistenceWorker();
  if (!settingsPersistenceOk) {
    logEmit(LogLevel::WARNING, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_SETTINGS_SAVE, 0);
  } else {
    logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_SETTINGS_SAVE, 1);
  }

  const bool healthWorkerOk = initializeHealthWorker();
  if (!healthWorkerOk) {
    logEmit(LogLevel::WARNING, DebugCategory::BOOT,
            DebugCode::INITIALIZATION_FAILED, BOOT_SUBSYSTEM_CPU);
  }
  publishControlStatus();
  bool networkOk = true;
  bool psramOk = true;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  // Runs before the network task so the OTA slot pair and this boot's
  // pending-verify state are known the first time the Web UI is served.
  openbrewbyweight::OpenBrewByWeightOta::instance().begin();
  if (settingsLoaded && webCommandQueue != nullptr) {
    NetworkBridgeCallbacks callbacks;
    callbacks.copyControlStatus = copyControlStatus;
    callbacks.copyControlGate = copyControlGate;
    callbacks.refreshControlStatus = refreshControlStatus;
    callbacks.enqueueWebCommand = enqueueWebCommand;
    callbacks.copyDebugEvents = copyDebugEvents;
    callbacks.copyAuditEvents = copyAuditEvents;
    callbacks.auditLogLatestSequence = auditLogLatestSequence;
    callbacks.debugLogLatestSequence = debugLogLatestSequence;
    callbacks.copyAuditStoreStatus = copyAuditStoreStatus;
    callbacks.setAuditVerbose = setAuditVerbose;
    callbacks.auditVerboseDeadline = auditVerboseDeadline;
    callbacks.addDebugEvent = addDebugEvent;
    callbacks.reportTaskWatchdogFault = reportTaskWatchdogFault;
    callbacks.requestSafeRestart = requestSafeRestart;
    callbacks.copyShotRecords = copyShotRecords;
    callbacks.copyShotStatsRecords = copyShotStatsRecords;
    callbacks.copyShotCurveById = copyShotCurveById;
    callbacks.copyHomeShot = copyHomeShot;
    callbacks.shotLogSavePending = shotLogSavePending;
    callbacks.copyHistoryPage = copyHistoryPage;
    callbacks.historyEpoch = copyHistoryEpoch;
    callbacks.shotLogEpoch = copyShotLogEpoch;
    callbacks.copyShotStats = copyShotStats;
    callbacks.deleteHistoryRecord = deleteHistoryRecord;
    callbacks.clearHistoryLog = clearHistoryLog;
    callbacks.deleteShotRecord = deleteShotRecord;
    callbacks.rateShotRecord = rateShotRecord;
    callbacks.rateLastShot = rateLastShot;
    callbacks.clearShotLog = clearShotLog;
    callbacks.clearLastShot = clearLastShot;
    callbacks.clearResetHistory = clearPersistedResetHistory;
    callbacks.resetAllDurableStores = resetAllDurableStoresForNetwork;
    callbacks.releaseNvsSpaceForFactoryReset =
        releaseNvsSpaceForFactoryResetForNetwork;
    callbacks.copyPreferredScaleMac = copyPreferredScaleMac;
    callbacks.copyPreferredScaleName = copyPreferredScaleName;
    callbacks.copyScaleHistory = copyScaleHistory;
    callbacks.copyRecipeSnapshot = copyRecipeSnapshot;
    callbacks.copyPresetBank = copyPresetBank;
    callbacks.copyPresetShotStats = copyPresetShotStats;
    callbacks.copyRuntimeConfig = copyRuntimeConfig;
    callbacks.copyBullseyeConfig = copyBullseyeConfig;
    callbacks.stageBullseyeConfig = stageBullseyeConfig;
    callbacks.copyDebugExportExtras = copyDebugExportExtras;
    callbacks.copyTaskProfiler = copyTaskProfiler;
    callbacks.copyScaleProfilerStatus = copyScaleProfilerStatus;
    callbacks.requestLoopMaxReset = requestLoopMaxReset;
    callbacks.requestScaleGapMaxReset = requestScaleGapMaxReset;
    callbacks.reportStationIpChange = reportStationIpChange;
    if (!networkManager.begin(persistedSettings, callbacks)) {
      networkOk = false;
      logEmit(LogLevel::WARNING, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
              BOOT_SUBSYSTEM_NETWORK, 0);
    } else {
      logEmit(LogLevel::INFO, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
              BOOT_SUBSYSTEM_NETWORK, 1);
    }
  } else if (!settingsLoaded) {
    networkOk = false;
    logEmit(LogLevel::WARNING, DebugCategory::BOOT, DebugCode::BOOT_SUBSYSTEM,
            BOOT_SUBSYSTEM_NETWORK, 0);
  }
  psramOk = psramFound() && (!settingsLoaded || workBufIsExternal());
  logEmit(psramOk ? LogLevel::INFO : LogLevel::CRITICAL, DebugCategory::BOOT,
          DebugCode::BOOT_SUBSYSTEM, BOOT_SUBSYSTEM_PSRAM, psramOk ? 1 : 0);
#endif

  bootCapabilities.evaluated = true;
  bootCapabilities.platformClock = platformClockReady;
  bootCapabilities.relaySafetyTimers = relaySafetyTimersReady;
  bootCapabilities.taskWatchdog = taskWatchdogReady;
  bootCapabilities.persistentStorage = persistenceReady && flashIoReady;
  bootCapabilities.settingsLoaded = settingsLoaded && settingsDurable;
  bootCapabilities.settingsPersistenceWorker = settingsPersistenceOk;
  bootCapabilities.scaleWorker = scaleWorkerOk;
  bootCapabilities.webCommandQueue = webQueueOk;
  bootCapabilities.network = networkOk;
  bootCapabilities.psram = psramOk;
  bootCapabilities.criticalFaultLatched =
      criticalTaskWatchdogFaulted() ||
      relaySafetyState == RelaySafetyState::LOCKOUT;
  bootState = bootCapabilities.state();
  bootDegraded = bootState == BootState::DEGRADED_SAFE ||
                 bootState == BootState::FAULT_LATCHED;
  firmwareInitializationComplete = bootState == BootState::READY;
  publishScaleWorkerPolicy(runtimeConfig, firmwareInitializationComplete);
  if (firmwareInitializationComplete) {
    addDebugEvent(DebugCategory::BOOT, DebugCode::BOOT_READY);
  } else {
    int32_t failedSubsystem = BOOT_SUBSYSTEM_SCALE_WORKER;
    if (!bootCapabilities.mandatorySafetyReady()) {
      failedSubsystem = !bootCapabilities.platformClock
                            ? BOOT_SUBSYSTEM_CPU
                            : (!bootCapabilities.relaySafetyTimers
                                   ? BOOT_SUBSYSTEM_RELAY_TIMERS
                                   : BOOT_SUBSYSTEM_TASK_WDT);
    } else if (!bootCapabilities.mandatoryDurabilityReady()) {
      failedSubsystem = !bootCapabilities.persistentStorage
                            ? BOOT_SUBSYSTEM_PERSISTENCE
                            : BOOT_SUBSYSTEM_SETTINGS_SAVE;
    }
    addDebugEvent(DebugCategory::BOOT, DebugCode::INITIALIZATION_FAILED,
                  failedSubsystem, static_cast<int32_t>(bootState));
  }
  publishControlStatus();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  // The started event carries the boot identity: only a durable one is sent.
  controllerStartedPending = networkOk && settingsLoaded && bootIdentityDurable;
  (void)enqueueControllerStartedWebhook();
  // Boot-time reads/writes are complete. Any later flash call from loopTask
  // is an ownership violation and must fail rather than stall control.
  forbidFlashIoFromCurrentTask();
#endif
  serviceScaleConnectedLed();
}

void serviceHealthThresholdAlerts(uint32_t intervalMaxGapMs) {
  const bool heapLow =
      healthSnapshotValid &&
      (freeHeapBytes < HEALTH_HEAP_FREE_ALERT_BYTES ||
       largestFreeHeapBlockBytes < HEALTH_HEAP_LARGEST_ALERT_BYTES);
  const bool heapClear =
      healthSnapshotValid &&
      freeHeapBytes >= HEALTH_HEAP_FREE_CLEAR_BYTES &&
      largestFreeHeapBlockBytes >= HEALTH_HEAP_LARGEST_CLEAR_BYTES;
  if (heapLow) {
    if (!healthHeapAlertLatched) {
      healthHeapAlertLatched = true;
      addDebugEvent(DebugCategory::SYSTEM, DebugCode::HEALTH_HEAP_LOW,
                    static_cast<int32_t>(freeHeapBytes),
                    static_cast<int32_t>(largestFreeHeapBlockBytes));
    }
  } else if (heapClear) {
    healthHeapAlertLatched = false;
    healthHeapRestartLatched = false;
  }

  uint32_t worstStackBytes = UINT32_MAX;
  if (loopStackMinBytes < worstStackBytes) {
    worstStackBytes = loopStackMinBytes;
  }
  const uint32_t scaleStackMinBytes = scaleWorkerStackMinBytesValue();
  if (scaleStackMinBytes < worstStackBytes) {
    worstStackBytes = scaleStackMinBytes;
  }
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const uint32_t networkStack = networkManager.snapshot().taskStackMinBytes;
  if (networkStack < worstStackBytes) {
    worstStackBytes = networkStack;
  }
#endif
  const bool stackLow =
      worstStackBytes != UINT32_MAX &&
      worstStackBytes < HEALTH_STACK_MIN_ALERT_BYTES;
  const bool stackClear =
      worstStackBytes == UINT32_MAX ||
      worstStackBytes >= HEALTH_STACK_MIN_CLEAR_BYTES;
  if (stackLow && !healthStackAlertLatched) {
    healthStackAlertLatched = true;
    addDebugEvent(DebugCategory::SYSTEM, DebugCode::HEALTH_STACK_LOW,
                  static_cast<int32_t>(loopStackMinBytes),
                  static_cast<int32_t>(scaleStackMinBytes));
  } else if (stackClear) {
    healthStackAlertLatched = false;
  }

  const bool gapHigh = intervalMaxGapMs >= HEALTH_LOOP_GAP_ALERT_MS;
  const bool gapClear = intervalMaxGapMs <= HEALTH_LOOP_GAP_CLEAR_MS;
  if (gapHigh && !healthLoopGapAlertLatched) {
    healthLoopGapAlertLatched = true;
    char detail[24];
    snprintf(detail, sizeof(detail), "idle=%lu us flash=%u",
             static_cast<unsigned long>(healthIntervalGapIdleUs),
             healthIntervalGapFlash ? 1U : 0U);
    addDebugEventDetail(DebugCategory::SYSTEM, DebugCode::HEALTH_LOOP_GAP,
                        static_cast<int32_t>(intervalMaxGapMs),
                        static_cast<int32_t>(healthIntervalGapCpuUs), detail);
  } else if (gapClear) {
    healthLoopGapAlertLatched = false;
  }
}

bool controlHousekeepingDue(uint32_t now) {
  if (lastControlHousekeepingAtMs != UINT32_MAX &&
      now - lastControlHousekeepingAtMs < CONTROL_HOUSEKEEPING_INTERVAL_MS) {
    return false;
  }
  lastControlHousekeepingAtMs = now;
  return true;
}

void loop() {
  static uint32_t delayStartedAtUs = 0, delayReturnedAtUs = 0;
  static uint32_t lastLoopStartedAtUs = 0, requestedDelayMs = 0;
  const uint32_t loopStartedAtMs = millis();
  const uint32_t loopStartedAtUs = micros();
  const uint32_t gapUs = loopStartedAtUs - lastLoopStartedAtUs;
  lastLoopStartedAtUs = loopStartedAtUs;
  const uint32_t delayUs = delayReturnedAtUs - delayStartedAtUs;
  const uint32_t dispatchUs = delayStartedAtUs == 0
                                  ? 0 : loopStartedAtUs - delayReturnedAtUs;
  const bool resetLoopMax = loopPhaseProfiler.consumeReset();
  if (resetLoopMax) loopMaxGapMs = 0;
  const bool profileLoopPhases = taskProfiler.loopPhaseProfilingEnabled();
  loopPhaseStartedAtUs = loopStartedAtUs;
  uint32_t gapCpuUs = 0, gapIdleUs = 0;
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  static uint32_t lastLoopCpuUs = 0, lastLoopIdleUs = 0;
  loopPhaseCpuStartedUs = loopTaskCpuUs();
  const uint32_t loopIdleUs = loopCoreIdleUs();
  gapCpuUs = loopPhaseCpuStartedUs - lastLoopCpuUs;
  gapIdleUs = loopIdleUs - lastLoopIdleUs;
  lastLoopCpuUs = loopPhaseCpuStartedUs;
  lastLoopIdleUs = loopIdleUs;
#endif
  recordResetUptime(loopStartedAtMs);
  if (lastLoopAtMs != 0) {
    const uint32_t gap = loopStartedAtMs - lastLoopAtMs;
    if (!resetLoopMax && gap > loopMaxGapMs) {
      loopMaxGapMs = gap;
      loopPhaseProfiler.capturePeakGap(gap, gapUs, delayUs, dispatchUs,
                                       gapCpuUs, gapIdleUs);
    }
    // The deadline bounds lateness beyond the requested delay; early-return
    // iterations request none.
    if (gap > requestedDelayMs + CONTROL_SERVICE_DEADLINE_MS) {
      ++loopDeadlineMisses;
    }
    if (gap > healthIntervalMaxGapMs) {
      healthIntervalMaxGapMs = gap;
      healthIntervalGapCpuUs = gapCpuUs;
      healthIntervalGapIdleUs = gapIdleUs;
      healthIntervalGapFlash = flashIoActiveSince(lastLoopAtMs);
      loopPhaseProfiler.captureIntervalGap(gapUs, delayUs, dispatchUs);
    }
  }
  lastLoopAtMs = loopStartedAtMs;
  requestedDelayMs = 0;
  loopPhaseProfiler.beginIteration(profileLoopPhases, loopStartedAtUs,
                                   loopStartedAtMs);
  // Relay and paddle control never wait for BLE. The worker owns every scale,
  // heartbeat, packet, timer and connection operation. Restart before heap
  // walks: a failed Wi-Fi stop can leave TLSF unwalkable.
  serviceRelaySafety();
  if (safeRestartPending()) {
    const bool faultRestart =
        criticalTaskWatchdogFaulted();
    if (faultRestart ||
        (!session.active && !relayCircuitClosed())) {
      machineRequestStop();
      serviceSafetyHeartbeat(false);
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
      recordResetUptime(millis(), true);
      ESP.restart();
#else
      (void)consumeSafeRestartRequest();
#endif
      return;
    }
    // Planned restart waits for the shot. Fault paths above still open K1
    // and reset immediately so a hung firmware cannot keep the group on.
  }
  serviceHealthWorkerSample();
  finishLoopPhase(LoopPhase::SAFETY_HEALTH);
  // One link snapshot per iteration; the two event drains below keep their
  // own fresh fetch by contract and must not take this copy.
  const ScaleLinkSnapshot loopScaleLink = getScaleLinkSnapshot();
  backflushObservation = machineIntegrationBackflush(true);
  if (backflushObservation.busy() && !session.active) {
    cancelPendingFinalize("Drip analysis ended before backflush", 2);
    setScaleCommandCycle(0);
    pendingScaleTimerStop = {};
    session.stopTimerRequested = false;
    cancelScaleCompletionBeep();
  }
  finishLoopPhase(LoopPhase::INPUT_BACKFLUSH);
  pushActivatorDrivePermission(loopScaleLink);
  machineSampleInput();
  finishLoopPhase(LoopPhase::INPUT_DRIVE_PERMISSION);
  processScaleWorkerEvents();
  finishLoopPhase(LoopPhase::INPUT_SCALE_EVENTS);
  observeMachineSenseFromSession(loopScaleLink);
  finishLoopPhase(LoopPhase::INPUT_MACHINE_SENSE);
  serviceMachine();
  fillLoopGuardsFromIntention(machinePollIntention(), loopScaleLink);
  finishLoopPhase(LoopPhase::INPUT_MACHINE_SERVICE);
  const MachineIntention powerIntention = machineLastIntention();
  const bool wakeGestureConsumed = serviceMachineWakeGesture(powerIntention);
  if (!wakeGestureConsumed) {
    serviceNoScaleShotGuard(loopGuardInputs);
    serviceCupStartGuard(loopGuardInputs);
  }
  finishLoopPhase(LoopPhase::GUARDS_BREW_START);
  // Machine integrations observe the scale link once per loop; the Micra owns
  // its scale power policy (explicit disconnect, grace, relay guard, and the
  // confirmed machine power edge).
  const bool loopMachineBusy =
      relayCircuitClosed() || backflushObservation.busy();
  const bool loopScaleConnected =
      loopScaleLink.state == ScaleLinkState::CONNECTED;
  serviceMachineIntegrationScaleLink(
      loopStartedAtMs, loopScaleConnected, loopScaleLink.disconnectSequence,
      loopScaleLink.lastDisconnectReason, loopMachineBusy);
  serviceMachineIntegrationMachinePower(
      loopScaleConnected, loopScaleLink.features.has(ScaleFeaturePowerOff),
      loopMachineBusy);
  finishLoopPhase(LoopPhase::MACHINE_INTEGRATION);
  stateMachineTask();
  finishLoopPhase(LoopPhase::CONTROL_STATE_MACHINE);
  publishControlCriticalLoggingState();
  serviceWebhookBrewStart();
  machineOnBrewOutcome(session.active);
  finishLoopPhase(LoopPhase::CONTROL_EFFECTS);
  serviceExtendedPulseAlert();
  localBuzzer.service();
  serviceScaleCompletionBeep();
  finishLoopPhase(LoopPhase::ALERTS_AUDIO);
  servicePendingScaleTimerStop();
  serviceRemoteTimerStopRetry();
  serviceBullseyeMelody();
  finishLoopPhase(LoopPhase::ALERTS_TIMERS);
  pendingShotFinalizeTask();
  finishLoopPhase(LoopPhase::SHOT_FINALIZE);
  publishControlGate();
  serviceMaintenanceCancellation();
  finishLoopPhase(LoopPhase::COMMANDS_GATE_MAINTENANCE);
  processWebCommands();
  serviceQuickSettingsRevert();
  serviceControlCommandResult();
  serviceMaintenanceLease();
  finishLoopPhase(LoopPhase::COMMANDS_WEB);
  // The iteration's link snapshot: it only selects the CPU power profile.
  servicePowerManagement(
      !wakeGestureConsumed &&
      (powerIntention.turnedOn || powerIntention.turnedOff), loopScaleLink);
  servicePendingResetHistoryClear();
  servicePendingPlannedRestart();
  serviceScheduledRestart();
  finishLoopPhase(LoopPhase::COMMANDS_POWER_RESTART);
  if (controlHousekeepingDue(loopStartedAtMs)) {
    serviceSerialLogDump();
    finishLoopPhase(LoopPhase::HOUSEKEEPING_SERIAL_LOG);
    serviceSerialCli();
    finishLoopPhase(LoopPhase::HOUSEKEEPING_SERIAL_CLI);
    serviceRuntimePersistence(loopScaleLink);
    finishLoopPhase(LoopPhase::HOUSEKEEPING_RUNTIME_PERSISTENCE);
    serviceShotStorePersistence();
    finishLoopPhase(LoopPhase::HOUSEKEEPING_SHOT_PERSISTENCE);
    servicePreferredScaleMacPersistence();
    finishLoopPhase(LoopPhase::HOUSEKEEPING_SCALE_PERSISTENCE);
    serviceWeightStreamTelemetry(loopScaleLink);
    finishLoopPhase(LoopPhase::HOUSEKEEPING_WEIGHT_TELEMETRY);
    // The status publish records its own contiguous sub-phases (gather,
    // store copy, lock+commit) on the shared phase timeline; a pass with no
    // publish records nothing, keeping the sub-phase averages comparable.
    serviceControlStatusPublish();
    serviceScaleConnectedLed(loopScaleLink);
    finishLoopPhase(LoopPhase::HOUSEKEEPING_LED);
  }
  static uint32_t watchdogFedAtMs = 0;
  if (watchdogFedAtMs == 0 ||
      elapsedMs(watchdogFedAtMs) >= TASK_WATCHDOG_FEED_INTERVAL_MS) {
    watchdogFedAtMs = loopStartedAtMs;
    if (!feedCurrentTaskWatchdog()) {
      reportTaskWatchdogFault();
      tripRelaySafety(RelaySafetyFault::TASK_WATCHDOG_FAILURE);
      requestSafeRestart();
      serviceSafetyHeartbeat(false);
      return;
    }
  }
  serviceSafetyHeartbeat(true);
  finishLoopPhase(LoopPhase::DIAGNOSTICS);
  processScaleWorkerEvents();
  finishLoopPhase(LoopPhase::FINAL_SCALE_DRAIN);
  const uint32_t executionUs = micros() - loopStartedAtUs;
  if (executionUs > loopMaxExecutionUs) {
    loopMaxExecutionUs = executionUs;
  }
  delayStartedAtUs = micros();
  requestedDelayMs = controlLoopTickDelayMs(loopScaleLink);
  vTaskDelay(pdMS_TO_TICKS(requestedDelayMs));
  delayReturnedAtUs = micros();
}

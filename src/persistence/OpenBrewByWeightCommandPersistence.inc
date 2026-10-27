// Web/control bridge
// ---------------------------------------------------------------------------

// Shot store access for the network task: every read and mutation holds
// shotStoreMutex.
size_t copyShotRecords(ShotLogRecord *output, size_t capacity) {
  TaskLockGuard lock(shotStoreMutex);
  return shotLog.copyNewestFirst(output, capacity);
}

void copyPresetShotStats(const ShotPresetBank &bank, PresetShotStats *out) {
  TaskLockGuard lock(shotStoreMutex);
  for (uint8_t i = 0; i < bank.count; ++i) {
    const ShotPreset &p = bank.presets[i];
    out[i].stats = shotLog.arrivalStats(p, out[i].arrivalsMs);
    out[i].firstDrop = shotLog.firstDropStats(p);
    if (out[i].firstDrop.n == 0)
      out[i].firstDrop.medianMs = shotLog.firstDropStats(p, true).medianMs;
    if (p.grindHintsEnabled) out[i].grindHint = shotLog.grindHint(p);
  }
}

void copyShotStatsRecords(ShotStatsRecordsSnapshot &output) {
  TaskLockGuard lock(shotStoreMutex);
  output.count = shotLog.copyNewestFirst(output.records, SHOT_LOG_CAPACITY);
  output.epoch = shotLog.epoch();
}

bool copyShotCurveById(uint32_t shotId, ShotCurveRecord &curve) {
  const uint32_t lockStartedUs = micros();
  TaskLockGuard lock(shotStoreMutex);
  noteMutexWaitUs(shotStoreWaitLastUs, shotStoreWaitMaxUs,
                  micros() - lockStartedUs);
  resetShotCurveRecord(curve);
  return shotCurves.copyByShotId(shotId, curve);
}

// A null curve copies the record alone (the integration snapshot).
bool copyHomeShot(ShotLogRecord &record, ShotCurveRecord *curve) {
  TaskLockGuard lock(shotStoreMutex);
  if (!shotLog.copyNewestEligible(record)) {
    if (curve) resetShotCurveRecord(*curve);
    return false;
  }
  // Curves mutate only with the shot-log epoch (ShotLog::epoch): the idle
  // Home card keeps its copy of this shot's curve instead of re-reading it on
  // every dispatch. A reset or foreign copy carries another shotId.
  static uint32_t copiedEpoch = 0;
  const uint32_t epoch = shotLog.epoch();
  if (curve && (curve->shotId != record.id || epoch != copiedEpoch)) {
    resetShotCurveRecord(*curve);
    (void)shotCurves.copyByShotId(record.id, *curve);
    copiedEpoch = epoch;
  }
  return true;
}

bool shotLogSavePending() {
  TaskLockGuard lock(shotStoreMutex);
  return shotLog.dirty() || shotCurves.dirty();
}

void copyHistoryPage(HistoryPage &page, size_t offset, size_t limit,
                     ShotLogSortDir dir, uint8_t types) {
  const uint32_t lockStartedUs = micros();
  TaskLockGuard lock(shotStoreMutex);
  noteMutexWaitUs(shotStoreWaitLastUs, shotStoreWaitMaxUs,
                  micros() - lockStartedUs);
  historyLog.copyPage(page, offset, limit, dir, types);
}

uint32_t copyHistoryEpoch() {
  TaskLockGuard lock(shotStoreMutex);
  return historyLog.epoch();
}

uint32_t copyShotLogEpoch() {
  TaskLockGuard lock(shotStoreMutex);
  return shotLog.epoch();
}

bool deleteHistoryRecord(uint32_t id) {
  TaskLockGuard lock(shotStoreMutex);
  const bool changed = historyLog.removeById(id, false);
  if (changed) shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return changed;
}

bool clearHistoryLog() {
  TaskLockGuard lock(shotStoreMutex);
  const bool changed = historyLog.clear(false);
  if (changed) shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return changed;
}

ShotStatsView copyShotStats() {
  TaskLockGuard lock(shotStoreMutex);
  return shotLog.statsView();
}

bool copyShotStoreStatus(uint32_t &bootId, PersistedLastShot &last,
                         PersistedLastShot &good, HistoryRecord &newestActivation,
                         bool &hasNewestActivation, bool includeGoodHistory) {
  const uint32_t lockStartedUs = micros();
  TaskLockGuard lock(shotStoreMutex);
  noteMutexWaitUs(shotStoreWaitLastUs, shotStoreWaitMaxUs,
                  micros() - lockStartedUs);
  bootId = activationStores.bootId();
  // Newest activation record rides the same critical section so the status
  // publish needs no second shotStoreMutex acquisition.
  hasNewestActivation = historyLog.copyNewest(newestActivation);
  last = persistedLastShot;
  // The star rating lives in the shot-log record once the shot is committed;
  // mirror it into the status aggregate so the WebUI and HA see one value.
  if (last.valid && last.shotLogId != 0) {
    (void)shotLog.copyRatingById(last.shotLogId, last.rating);
  }
  good = persistedLastGoodShot;
  good.rating = 0;
  uint8_t rating = 0;
  if (!includeGoodHistory || !publishableLastShot(good) ||
      good.shotLogId == 0 ||
      !shotLog.copyRatingById(good.shotLogId, rating))
    return false;
  good.rating = rating;
  return true;
}

bool deleteShotRecord(uint32_t id) {
  TaskLockGuard lock(shotStoreMutex);
  const bool hadCurve = shotCurves.containsShotId(id);
  const bool hadLog = shotLog.containsId(id);
  if (!hadLog && !hadCurve) {
    return false;
  }
  if (hadCurve && !shotCurves.removeById(id, false)) {
    return false;
  }
  if (hadLog && !shotLog.removeById(id, false)) {
    return false;
  }
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return true;
}

bool clearShotLog() {
  TaskLockGuard lock(shotStoreMutex);
  if (!shotCurves.clear(false)) {
    return false;
  }
  if (!shotLog.clear(false)) return false;
  // The pool clears before this clear is persisted; the unsaved trace of a
  // cleared shot is not written.
  shotTrace().consumeCompleted();
  shotTracePoolClearRequested.store(true, std::memory_order_release);
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return true;
}

bool clearLastShot() {
  TaskLockGuard lock(shotStoreMutex);
  ShotLogRecord latest = {};
  if (!shotLog.copyNewestEligible(latest)) return false;
  if (shotCurves.containsShotId(latest.id) &&
      !shotCurves.removeById(latest.id, false)) return false;
  if (!shotLog.removeById(latest.id, false)) return false;
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return true;
}

void clearLastShotRuntimeState() {
  lastShotNvsDirty = false;
}

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool resetAllDurableStoresForNetwork(PersistedSettings &settings) {
  TaskLockGuard lock(shotStoreMutex);
  TaskLockGuard poolWriter(shotTracePoolWriterMutex);
  if (!resetAllDurableStores(settings, bleScanPersistedSettings, shotLog,
                             historyLog, lastShotStore, shotCurves, &shotTracePool())) {
    return false;
  }
  shotTrace().consumeCompleted();
  shotTracePoolClearRequested.store(false, std::memory_order_release);
  // The wiped stores are a mutation like any other: bump so the status
  // gather guard re-fetches and the persistence fast path keeps flowing
  // until the reset state is acknowledged as persisted.
  shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  // Factory reset includes the saved scale profile: drop its durable header
  // and any frozen RAM copy left by an unsaved session.
  if (!scaleProfiler().clearForFactoryReset()) {
    return false;
  }
  scaleProfileRequest.store(ScaleProfilerRequest::NONE,
                            std::memory_order_release);
  // Drop transient dirty state only after the durable factory reset succeeds.
  // A staged scan setting must not survive the reset's default write.
  clearLastShotRuntimeState();
  bleScanPersistMux.lock();
  bleScanPersistPending = false;
  bleScanEnabledPersistPending = false;
  pendingBleScanRequestIdCount = 0;
  bleScanPersistFailLatched = false;
  bleScanPersistMux.unlock();
  // The next boot re-arms the factory presets; no staged copy may land first.
  bbwQuickStartMux.lock();
  bbwQuickStartPersistPending = false;
  bbwQuickStartMux.unlock();
  return true;
}

bool releaseNvsSpaceForFactoryResetForNetwork() {
  TaskLockGuard lock(shotStoreMutex);
  return releaseNvsSpaceForFactoryReset(lastShotStore);
}
#endif

bool rateShotRecord(uint32_t id, uint8_t rating) {
  TaskLockGuard lock(shotStoreMutex);
  if (id == 0 || rating > SHOT_LOG_RATING_MAX) {
    return false;
  }
  const bool changed = shotLog.updateRating(id, rating, false);
  if (changed) shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return changed;
}

bool rateLastShot(uint8_t rating) {
  TaskLockGuard lock(shotStoreMutex);
  ShotLogRecord latest = {};
  if (rating > SHOT_LOG_RATING_MAX || !shotLog.copyNewestEligible(latest))
    return false;
  const bool changed = shotLog.updateRating(latest.id, rating, false);
  if (changed) shotStoreDirtyGeneration.fetch_add(1, std::memory_order_release);
  return changed;
}

// Recipe and Bullseye publication: the control task commits, the network
// task copies under recipeMutex / bullseyeConfigMux.
bool enqueueWebCommand(const WebCommand &command) {
  return webCommandQueue != nullptr &&
         xQueueSend(webCommandQueue, &command, 0) == pdTRUE;
}

void publishRecipeState() {
  TaskLockGuard lock(recipeMutex);
  publishedRuntimeConfig = overlaySessionQuickSettings(sessionQuickSettings, runtimeConfig, true);
  publishedSessionQuickSettings = sessionQuickSettings;
  publishedPresetBank = presetBank;
}

void copyRecipeSnapshot(RecipeSnapshot *out) {
  if (out == nullptr) return;
  TaskLockGuard lock(recipeMutex);
  out->presets = publishedPresetBank;
  out->runtime = overlaySessionQuickSettings(publishedSessionQuickSettings, publishedRuntimeConfig);
}

void copyPresetBank(ShotPresetBank *out) {
  if (out == nullptr) return;
  TaskLockGuard lock(recipeMutex);
  *out = publishedPresetBank;
}

void copyRuntimeConfig(RuntimeConfig *out) {
  if (out == nullptr) return;
  TaskLockGuard lock(recipeMutex);
  *out = publishedRuntimeConfig;
}

void copyBullseyeConfig(BullseyeMelodyConfig *out) {
  if (out == nullptr) return;
  bullseyeConfigMux.lock();
  *out = bullseyeMelodyConfig;
  bullseyeConfigMux.unlock();
}

bool stageBullseyeConfig(const BullseyeMelodyConfig &config,
                         uint32_t requestId) {
  if (requestId == 0 || !validBullseyeMelodyConfig(config)) return false;
  bullseyeConfigMux.lock();
  stagedBullseyeMelodyConfig = config;
  stagedBullseyeRequestId = requestId;
  bullseyeConfigMux.unlock();
  return true;
}

bool takeStagedBullseyeConfig(uint32_t requestId,
                              BullseyeMelodyConfig &out) {
  bool matched = false;
  bullseyeConfigMux.lock();
  if (requestId != 0 && stagedBullseyeRequestId == requestId) {
    out = stagedBullseyeMelodyConfig;
    stagedBullseyeRequestId = 0;
    matched = true;
  }
  bullseyeConfigMux.unlock();
  return matched;
}

void commitLiveBullseyeConfig(const BullseyeMelodyConfig &config) {
  bullseyeConfigMux.lock();
  const bool changed = bullseyeMelodyConfig.enabled != config.enabled ||
      strncmp(bullseyeMelodyConfig.rtttl, config.rtttl,
              sizeof(config.rtttl)) != 0;
  if (changed) bullseyeMelodyConfig = config;
  bullseyeConfigMux.unlock();
  if (!changed) return;
  (void)localBuzzer.configureBullseyeRtttl(bullseyeMelodyConfig.rtttl);
  if (!bullseyeMelodyConfig.enabled) bullseyeTracker.clear();
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  networkManager.syncLiveBullseye(bullseyeMelodyConfig);
#endif
}

void maybeRequestNtpSyncOnActivity() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  networkManager.requestNtpSyncIfNeeded();
#else
  if (wallClockNeedsActivityNtpSync(g_wallClock, hostMillis)) {
    ++hostNtpSyncRequestCount;
  }
#endif
}
bool controlAllowsConfigurationNow() {
  return stopperState == StopperState::READY && !session.active &&
         !machineIsRunning() && !machineLastIntention().holdActive &&
         !maintenanceLease.active;
}

uint32_t nextPersistenceIoRetry(uint32_t currentMs, uint32_t baseMs) {
  if (currentMs == 0) return baseMs;
  return currentMs >= PERSIST_IO_RETRY_MAX_MS / 2U
             ? PERSIST_IO_RETRY_MAX_MS
             : currentMs * 2U;
}

void beginWebRinse() {
  if (!beginRinseCycle(ControlSource::WEB)) {
    return;
  }
  (void)enterRinse();
}

bool forwardAcceptedNetworkCommand(const WebCommand &command) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (!networkManager.enqueueAcceptedCommand(command)) {
    addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_REJECTED,
                  static_cast<int32_t>(command.type));
    return false;
  }
  return true;
#else
  ++hostForwardAcceptedNetworkCommandCalls;
  hostLastForwardedNetworkCommand = command;
  return hostForwardAcceptedNetworkCommandSucceeds;
#endif
}

void reportControlCommandResult(const WebCommand &command,
                                CommandResultState state) {
  if (command.requestId == 0 || controlResultPending) {
    return;
  }
  WebCommand result;
  result.type = WebCommandType::MAINTENANCE_COMPLETE;
  result.requestId = command.requestId;
  result.succeeded = state == CommandResultState::APPLIED ||
                     state == CommandResultState::PERSISTED;
  result.resultState = state;
  if (!forwardAcceptedNetworkCommand(result)) {
    controlResultCommand = result;
    controlResultPending = true;
  }
}

void serviceControlCommandResult() {
  if (controlResultPending &&
      forwardAcceptedNetworkCommand(controlResultCommand)) {
    controlResultPending = false;
    controlResultCommand = WebCommand{};
  }
}

void serviceMaintenanceCancellation() {
  if (maintenanceCancellationPending &&
      forwardAcceptedNetworkCommand(maintenanceCancellationCommand)) {
    maintenanceCancellationPending = false;
    maintenanceCancellationCommand = WebCommand{};
  }
}

bool beginMaintenanceLease(const WebCommand &networkCommand,
                           bool applyRuntimeOnSuccess) {
  if (!controlAllowsConfigurationNow() && !networkCommand.unsafeWebUiOverride) {
    return false;
  }
  // A planned restart must never cut a shot. Override cannot change that.
  if (networkCommand.type == WebCommandType::RESTART &&
      (session.active || relayCircuitClosed() ||
       machineLastIntention().holdActive)) {
    return false;
  }
  // An override reaching here mid-shot ends that shot, so it is recorded.
  if (session.active) {
    finalizeCycle(EndReason::WEB_STOP, nextStateForUserHold(machineLastIntention()));
  } else {
    machineRequestStop();
  }
  maintenanceLease = MaintenanceLease{};
  maintenanceLease.active = true;
  outboundMaintenance.store(true, std::memory_order_release);
  inhibitMachineIntegrationCloud();
  maintenanceLease.applyRuntimeOnSuccess = applyRuntimeOnSuccess;
  maintenanceLease.id = networkCommand.requestId;
  if (maintenanceLease.id == 0) {
    maintenanceLease.id = nextInternalRequestId++;
  }
  maintenanceLease.startedAtMs = millis();
  maintenanceLease.command = networkCommand;
  maintenanceLease.command.requestId = maintenanceLease.id;
  maintenanceLease.command.maintenanceLeaseId = maintenanceLease.id;
  addDebugEvent(DebugCategory::SECURITY, DebugCode::MAINTENANCE_RESERVED,
                static_cast<int32_t>(maintenanceLease.id),
                static_cast<int32_t>(networkCommand.type));
  return true;
}

bool webCommandAllowsUnsafeConfiguration(const WebCommand &command) {
  return controlAllowsConfigurationNow() || command.unsafeWebUiOverride;
}

bool persistentConfigurationMutationAllowed(const WebCommand &command) {
  return settingsPersistenceAvailable() &&
         webCommandAllowsUnsafeConfiguration(command);
}

// A planned restart waits for the shot to end and, for a bounded time, for
// its drip-delayed finalize and shot stores to reach flash, so a restart
// right after a shot keeps it. A failing store never holds it.
bool plannedRestartMustWait() {
  if (!controlAllowsConfigurationNow() || session.active ||
      relayCircuitClosed() || machineLastIntention().holdActive) {
    return true;
  }
  if (elapsedMs(plannedRestartHeldAtMs) >= PLANNED_RESTART_RECORD_WAIT_MS ||
      shotStorePersistIoRetryMs != 0) {
    return false;
  }
  if (pendingFinalize.pending) return true;
  TaskLockGuard lock(shotStoreMutex);
  return shotLog.dirty() || shotCurves.dirty() || historyLog.dirty() ||
         lastShotNvsDirty;
}

void servicePendingPlannedRestart();

void holdOrBeginPlannedRestart(const WebCommand &command) {
  pendingPlannedRestart = command;
  pendingPlannedRestart.type = WebCommandType::RESTART;
  pendingPlannedRestart.unsafeWebUiOverride = false;
  plannedRestartHeld = true;
  plannedRestartHeldAtMs = millis();
  servicePendingPlannedRestart();
}

void servicePendingPlannedRestart() {
  if (!plannedRestartHeld || maintenanceLease.active || plannedRestartMustWait()) {
    return;
  }
  const WebCommand restart = pendingPlannedRestart;
  plannedRestartHeld = false;
  pendingPlannedRestart = WebCommand{};
  if (!beginMaintenanceLease(restart, false)) {
    rejectWebCommand(restart);
  }
}

// Once a second the planner picks the moment; the restart then takes the
// planned-restart lease like an Admin restart, after the durable audit log
// has written the FIRED record (the flush pauses under a lease).
void serviceScheduledRestart() {
  const uint32_t now = millis();
  const bool firing =
      scheduledRestartPlanner.state() == ScheduledRestartState::FIRING;
  if (!firing && elapsedMs(scheduledRestartStepAtMs) < 1000) return;
  scheduledRestartStepAtMs = now;
  const TimeStatusSnapshot clock = g_wallClock.snapshot(now);
  ScheduledRestartInput in;
  in.enabled = runtimeConfig.scheduledRestartEnabled != 0;
  in.minuteOfDay = runtimeConfig.scheduledRestartMinuteOfDay;
  in.zone = runtimeConfig.timezoneId;
  in.utcSec = clock.utcSec;
  in.clockStale = clock.state == TimeSyncState::STALE;
  in.nowMs = now;
  in.shotActive = session.active || relayCircuitClosed() ||
                  machineLastIntention().holdActive;
  readScheduledRestartOta(in);
  in.busy = !controlAllowsConfigurationNow() || maintenanceLease.active ||
            plannedRestartHeld || safeRestartPending();
  const ScheduledRestartLog log = scheduledRestartPlanner.step(in);
  if (log.emit) {
    addDebugEvent(DebugCategory::SYSTEM, log.code,
                  static_cast<int32_t>(log.cause), in.minuteOfDay);
    if (log.code == DebugCode::SCHEDULED_RESTART_FIRED) {
      scheduledRestartFiredAtMs = now;
    }
  }
  if (scheduledRestartPlanner.state() != ScheduledRestartState::FIRING) return;
  if (elapsedMs(scheduledRestartFiredAtMs) < 10000) {
    TaskLockGuard lock(debugLogMutex);
    if (auditLog.hasPending()) return;
  }
  WebCommand restart;
  restart.type = WebCommandType::RESTART;
  restart.requestId = nextInternalRequestId++;
  scheduledRestartRequestId = restart.requestId;
  holdOrBeginPlannedRestart(restart);
  if (maintenanceLease.active && maintenanceLease.id == restart.requestId) {
    scheduledRestartPlanner.submitted();
    return;
  }
  // Never left to the generic hold, which would restart right after a shot.
  if (plannedRestartHeld &&
      pendingPlannedRestart.requestId == restart.requestId) {
    plannedRestartHeld = false;
    pendingPlannedRestart = WebCommand{};
  }
  scheduledRestartPlanner.aborted(now);
}

void holdOrBeginResetHistoryClear(const WebCommand &command) {
  if (resetHistoryClearHeld) {
    rejectWebCommand(command);
    return;
  }
  WebCommand clear = command;
  clear.unsafeWebUiOverride = false;
  clear.resetHistoryUnsafeCount = safetyResetStatus.unsafeResetCount;
  if (!controlAllowsConfigurationNow() || session.active ||
      relayCircuitClosed() || machineLastIntention().holdActive) {
    pendingResetHistoryClear = clear;
    resetHistoryClearHeld = true;
    return;
  }
  if (!beginMaintenanceLease(clear, false)) rejectWebCommand(clear);
}

void servicePendingResetHistoryClear() {
  if (!resetHistoryClearHeld || !controlAllowsConfigurationNow() ||
      session.active || relayCircuitClosed() ||
      machineLastIntention().holdActive) {
    return;
  }
  const WebCommand clear = pendingResetHistoryClear;
  if (!beginMaintenanceLease(clear, false)) return;
  resetHistoryClearHeld = false;
  pendingResetHistoryClear = WebCommand{};
}

void serviceMaintenanceLease() {
  if (!maintenanceLease.active || maintenanceLease.forwarded ||
      !machineLastIntention().stablyOff ||
      elapsedMs(maintenanceLease.startedAtMs) <
          MAINTENANCE_LEASE_SETTLE_MS) {
    return;
  }
  // The worker abandons a paused shot-store image under a lease; forward only
  // after its result is acknowledged so maintenance never overlaps the write.
  shotStorePersistMux.lock();
  const bool shotStoreBusy =
      shotStorePersistInFlight || shotStorePersistResultReady;
  shotStorePersistMux.unlock();
  if (shotStoreBusy) return;
  if (forwardAcceptedNetworkCommand(maintenanceLease.command)) {
    maintenanceLease.forwarded = true;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    WebCommand result;
    result.type = WebCommandType::MAINTENANCE_COMPLETE;
    result.requestId = maintenanceLease.command.requestId;
    result.maintenanceLeaseId = maintenanceLease.id;
    result.succeeded = true;
    const WebCommandType forwardedType = maintenanceLease.command.type;
    if (forwardedType == WebCommandType::SAVE_NETWORK ||
        forwardedType == WebCommandType::FORGET_NETWORK ||
        forwardedType == WebCommandType::CHANGE_DEVICE_PASSWORD ||
        forwardedType == WebCommandType::RESET_DEVICE_PASSWORD ||
        forwardedType == WebCommandType::RESET_NETWORK_AP ||
        forwardedType == WebCommandType::RESTART) {
      hostLastFlushedRuntime = overlaySessionQuickSettings(sessionQuickSettings, runtimeConfig, true);
      hostLastFlushedPresets = presetBank;
      hostLastFlushIncludedLive = true;
      ++hostRuntimePersistAttempts;
      if (!hostRuntimePersistSucceeds) {
        result.succeeded = forwardedType == WebCommandType::RESTART;
        runtimePersistFailed = true;
        runtimePersistPending = true;
        runtimePersistIoRetryMs = nextPersistenceIoRetry(
            runtimePersistIoRetryMs, RUNTIME_PERSIST_RETRY_MS);
        runtimePersistRetryAtMs = millis() + runtimePersistIoRetryMs;
      } else {
        runtimePersistPending = false;
        runtimePersistReasonBits = 0;
        runtimePersistFailed = false;
        runtimePersistIoRetryMs = 0;
      }
    }
    hostLastMaintenanceSucceeded = result.succeeded;
    completeMaintenanceLease(result);
#endif
  }
}

void queueRuntimePersist(int32_t reasonBits) {
  runtimePersistRevision = runtimeConfig.revision;
  runtimePersistPending = true;
  runtimePersistReasonBits |= reasonBits;
  runtimePersistIoRetryMs = 0;
  runtimePersistRetryAtMs = millis() + RUNTIME_PERSIST_DEBOUNCE_MS;
}

// reasonBits == 0 commits a session-only change: live and published, never
// queued for flash. savedEdit is the saved config a Settings edit was built on.
void commitLiveRuntimeConfig(const RuntimeConfig &composed, int32_t reasonBits,
                             const RuntimeConfig *savedEdit) {
  const RuntimeConfig previous = runtimeConfig;
  runtimeConfig = composed;
  reconcileSessionQuickSettings(sessionQuickSettings, activeShotPreset(presetBank),
                                savedEdit, runtimeConfig);
  dispatchRuntimeSettingsChanges(previous, runtimeConfig,
                                 runtimeSettingsSubscribers);
  addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED,
                static_cast<int32_t>(runtimeConfig.revision));
  scaleProfileNoteEvent(ScaleProfileEvent::SETTINGS_CHANGED, millis(), 0, 0,
                        NAN, runtimeConfig.revision, 0);
  if (reasonBits != 0) queueRuntimePersist(reasonBits);
  publishRecipeState();
}

bool serviceBleScanPersist();
void serviceBbwQuickStartPersist();

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool shotStoreWorkerWriteAllowed(bool &maintenance) {
  ControlGateSnapshot control;
  copyControlGate(control);
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  maintenance = control.maintenanceLeaseActive;
  return !maintenance &&
         durableFlashWriteAllowed(control.activeCycle || control.relayClosed ||
                                      control.machineRunning ||
                                      control.physicalActivatorOn,
                                  link.connecting);
}

// With abandonOnMaintenance, returns false once a maintenance lease closes the
// gate: a paused shot-store write must not resume over a factory reset.
bool waitForShotStoreWriteGate(bool abandonOnMaintenance = false) {
  bool maintenance = false;
  while (!shotStoreWorkerWriteAllowed(maintenance)) {
    if (abandonOnMaintenance && maintenance) return false;
    feedOrTripCurrentTaskWatchdog();
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  return true;
}
#endif

void persistShotStoreImage() {
  const uint32_t lockTimeoutsBefore = flashIoLockTimeouts();
  ActivationStoresFlushReport report;
  do {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    if (!waitForShotStoreWriteGate(true)) {
      report.anyFail = true;
      break;
    }
#endif
    // A clear all lands in the shot trace pool before the shot log's.
    if (!landShotTracePoolClearForShotStores()) {
      report.anyFail = true;
      break;
    }
    report = shotStorePersistImage->serviceStep(FLASH_IO_LOCK_TIMEOUT_MS,
                                                addDebugEvent);
    if (!report.complete && !report.anyFail) yieldFlashIo();
  } while (!report.complete && !report.anyFail);
  bool ok = !report.anyFail;
  if (ok && shotStorePersistImageLastShotDirty) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    ok = waitForShotStoreWriteGate(true);
    yieldFlashIo();
#endif
    if (!ok || !lastShotPersistImage->save(FLASH_IO_LOCK_TIMEOUT_MS)) ok = false;
  }
  shotStorePersistMux.lock();
  shotStorePersistResultReady = true;
  shotStorePersistResultOk = ok;
  shotStorePersistResultIoFail =
      !ok && flashIoLockTimeouts() == lockTimeoutsBefore;
  shotStorePersistResultGeneration = shotStorePersistImageGeneration;
  shotStorePersistInFlight = false;
  shotStorePersistMux.unlock();
}

// Scale-profile save/invalidate on this worker: one cache-off operation per
// step, re-checked against the safe-write gate between steps. The frozen
// PSRAM generation stays immutable while this runs.
void persistScaleProfileImage() {
  FlashStoreStepResult result = FlashStoreStepResult::MORE;
  do {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    waitForShotStoreWriteGate();
#endif
    result = scaleProfiler().serviceFlashStep();
    if (result == FlashStoreStepResult::MORE) yieldFlashIo();
  } while (result == FlashStoreStepResult::MORE);
  scaleProfiler().publishFlashResult(result == FlashStoreStepResult::COMPLETE);
}

// Health-worker side: hand the profiler's pending flash work to this worker.
bool dispatchScaleProfileWork() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (!settingsPersistenceAvailable()) return false;
  const PersistenceWork work = PersistenceWork::SCALE_PROFILE;
  return xQueueSend(settingsPersistQueue, &work, 0) == pdTRUE;
#else
  return true;
#endif
}

// Audit flush on this worker (single owner of the flash lock after boot):
// drain latch records marked pending, batched at ≤8 records or after a 2 s
// window, one durable header commit per drained batch. A failed append
// leaves its record pending, so the next idle dispatch retries.
void persistAuditLogImage() {
  auditFlushBusy = true;
  for (;;) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    waitForShotStoreWriteGate();
#endif
    AuditRecord batch[8];
    size_t count;
    {
      TaskLockGuard lock(debugLogMutex);
      count = auditLog.copyPending(sizeof(batch) / sizeof(batch[0]), batch);
    }
    if (count == 0) break;
    bool failed = false;
    for (size_t index = 0; index < count && !failed; ++index) {
      if (!auditStoreAppend(batch[index])) {
        failed = true;
        break;
      }
      TaskLockGuard lock(debugLogMutex);
      auditLog.markFlushedThrough(batch[index].sequence);
    }
    if (failed) break;
  }
  {
    TaskLockGuard lock(debugLogMutex);
    auditFlushPendingSinceMs = 0;
  }
  auditFlushBusy = false;
}

// True when the latch holds records worth flushing now (batch size or the
// coalescing window elapsed). Called from this worker's idle branch.
bool auditFlushDue() {
  TaskLockGuard lock(debugLogMutex);
  if (!auditLog.hasPending()) {
    auditFlushPendingSinceMs = 0;
    return false;
  }
  const uint32_t now = millis();
  if (auditFlushPendingSinceMs == 0) auditFlushPendingSinceMs = now;
  return auditLog.pendingCount() >= 8 || now - auditFlushPendingSinceMs >= 2000;
}

bool dispatchAuditFlushWork() {
  if (auditFlushBusy) return false;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (!settingsPersistenceAvailable()) return false;
  const PersistenceWork work = PersistenceWork::AUDIT_FLUSH;
  return xQueueSend(settingsPersistQueue, &work, 0) == pdTRUE;
#else
  return true;
#endif
}

// Explicit clear, handed to the flash-owning worker: the serial CLI runs on
// the control loop, which is forbidden from taking the flash lock post-boot.
bool dispatchAuditClearWork() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (!settingsPersistenceAvailable()) return false;
  const PersistenceWork work = PersistenceWork::AUDIT_CLEAR;
  return xQueueSend(settingsPersistQueue, &work, 0) == pdTRUE;
#else
  return true;
#endif
}

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
void settingsPersistTask(void *) {}
#else
void settingsPersistTask(void *parameter) {
  (void)parameter;
  if (!subscribeCurrentTaskToWatchdog()) {
    reportTaskWatchdogFault();
  }
  for (;;) {
    // Must not block forever: this task is subscribed to the TWDT.
    PersistenceWork work = PersistenceWork::SETTINGS;
    if (xQueueReceive(settingsPersistQueue, &work,
                      pdMS_TO_TICKS(SETTINGS_PERSIST_IDLE_WAIT_MS)) !=
        pdTRUE) {
      feedOrTripCurrentTaskWatchdog();
      ControlGateSnapshot control;
      copyControlGate(control);
      const ScaleLinkSnapshot link = getScaleLinkSnapshot();
      if (!control.maintenanceLeaseActive &&
          durableFlashWriteAllowed(control.activeCycle || control.relayClosed ||
                                   control.machineRunning ||
                                   control.physicalActivatorOn,
                                   link.connecting)) {
        (void)persistResetUptimeCheckpoint(millis());
        serviceBleScanPersist();
        serviceBbwQuickStartPersist();
        serviceShotTracePool();
        if (auditFlushDue()) (void)dispatchAuditFlushWork();
      }
      continue;
    }
    yieldSettingsNvs();
    feedOrTripCurrentTaskWatchdog();
    if (work == PersistenceWork::SHOT_STORES) {
      persistShotStoreImage();
      feedOrTripCurrentTaskWatchdog();
      continue;
    }
    if (work == PersistenceWork::SCALE_PROFILE) {
      persistScaleProfileImage();
      feedOrTripCurrentTaskWatchdog();
      continue;
    }
    if (work == PersistenceWork::AUDIT_FLUSH) {
      persistAuditLogImage();
      feedOrTripCurrentTaskWatchdog();
      continue;
    }
    if (work == PersistenceWork::AUDIT_CLEAR) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
      waitForShotStoreWriteGate();
#endif
      (void)clearAuditStores();
      feedOrTripCurrentTaskWatchdog();
      continue;
    }
    // The producer owns the mailbox until enqueue; this worker owns it until
    // publishing the result. Only the control task consumes that result.
    // Control admitted the write, but a shot can start while this idle-priority
    // worker waits for CPU: recheck, and never resume over a factory reset.
    const bool admitted = waitForShotStoreWriteGate(true);
    const uint32_t lockTimeoutsBefore = flashIoLockTimeouts();
    const bool ok = admitted && savePersistedSettings(settingsPersistRequest.blob);
    const bool lockContended =
        !ok && flashIoLockTimeouts() != lockTimeoutsBefore;
    feedOrTripCurrentTaskWatchdog();
    settingsPersistMux.lock();
    settingsPersistResultReady = true;
    settingsPersistResultOk = ok;
    settingsPersistResultLockContended = lockContended;
    settingsPersistResultRuntimeRevision =
        settingsPersistRequest.runtimeRevision;
    settingsPersistResultStorageRevision =
        ok ? settingsPersistRequest.blob.storageRevision : 0;
    settingsPersistMux.unlock();
  }
}
#endif

bool settingsPersistenceAvailable() {
  return settingsPersistenceReady && settingsPersistQueue != nullptr &&
         settingsPersistTaskHandle != nullptr;
}

bool initializeSettingsPersistenceWorker() {
  if (settingsPersistenceAvailable()) {
    return true;
  }
  settingsPersistenceReady = false;
  if (settingsPersistTaskHandle != nullptr) {
    return false;
  }
  if (settingsPersistQueue != nullptr) {
    vQueueDelete(settingsPersistQueue);
    settingsPersistQueue = nullptr;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    ++hostSettingsPersistRollbackDeletes;
#endif
  }
  if (shotStorePersistImage == nullptr) {
    void *block = allocExternal(sizeof(ActivationStoresPersistImage));
    if (block == nullptr) return false;
    shotStorePersistImage = new (block) ActivationStoresPersistImage();
  }
  if (lastShotPersistImage == nullptr) {
    void *block = allocExternal(sizeof(LastShotStore));
    if (block == nullptr) return false;
    lastShotPersistImage = new (block) LastShotStore();
  }

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  QueueHandle_t queue = hostSettingsPersistQueueCreateSucceeds
                            ? xQueueCreate(PERSISTENCE_WORK_QUEUE_DEPTH,
                                           sizeof(PersistenceWork))
                            : nullptr;
#else
  QueueHandle_t queue =
      xQueueCreate(PERSISTENCE_WORK_QUEUE_DEPTH, sizeof(PersistenceWork));
#endif
  if (queue == nullptr) {
    return false;
  }

  // Publish the queue before task creation: a newly scheduled worker may run
  // immediately and must never observe a null receive queue.
  settingsPersistQueue = queue;
  TaskHandle_t task = nullptr;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const BaseType_t created =
      hostSettingsPersistTaskCreateSucceeds
          ? xTaskCreatePinnedToCore(
                settingsPersistTask, "settings_persist",
                SETTINGS_PERSIST_TASK_STACK_SIZE, nullptr,
                tskIDLE_PRIORITY, &task, PERSISTENCE_TASK_CORE)
          : pdFALSE;
#else
  const BaseType_t created = xTaskCreatePinnedToCore(
      settingsPersistTask, "settings_persist",
      SETTINGS_PERSIST_TASK_STACK_SIZE, nullptr, tskIDLE_PRIORITY, &task,
      PERSISTENCE_TASK_CORE);
#endif
  if (created != pdPASS || task == nullptr) {
    // Task creation failed, so no consumer can still reference the queue.
    settingsPersistQueue = nullptr;
    vQueueDelete(queue);
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    ++hostSettingsPersistRollbackDeletes;
#endif
    return false;
  }

  settingsPersistTaskHandle = task;
  settingsPersistenceReady = true;
  return true;
}

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
void releaseSettingsPersistenceWorkerForHost() {
  if (settingsPersistQueue != nullptr) {
    vQueueDelete(settingsPersistQueue);
  }
  settingsPersistQueue = nullptr;
  settingsPersistTaskHandle = nullptr;
  settingsPersistenceReady = false;
  if (shotStorePersistImage != nullptr) {
    shotStorePersistImage->~ActivationStoresPersistImage();
    free(shotStorePersistImage);
    shotStorePersistImage = nullptr;
  }
  if (lastShotPersistImage != nullptr) {
    lastShotPersistImage->~LastShotStore();
    free(lastShotPersistImage);
    lastShotPersistImage = nullptr;
  }
}
#endif

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST

void serviceSettingsPersistResult() {
  bool ready = false;
  bool ok = false;
  bool lockContended = false;
  uint32_t runtimeRevision = 0;
  uint32_t storageRevision = 0;
  settingsPersistMux.lock();
  ready = settingsPersistResultReady;
  if (ready) {
    settingsPersistResultReady = false;
    ok = settingsPersistResultOk;
    lockContended = settingsPersistResultLockContended;
    runtimeRevision = settingsPersistResultRuntimeRevision;
    storageRevision = settingsPersistResultStorageRevision;
    settingsPersistInFlight = false;
  }
  settingsPersistMux.unlock();
  if (!ready) {
    return;
  }
  if (ok) {
    persistedSettings.storageRevision = storageRevision;
    networkManager.syncDurableStorageRevision(storageRevision);
    runtimePersistFailed = false;
    runtimePersistIoRetryMs = 0;
    addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_PERSISTED,
                  static_cast<int32_t>(runtimeRevision));
    if (pendingPresetPersistence.requestId != 0 &&
        static_cast<int32_t>(runtimeRevision -
                             pendingPresetPersistence.revision) >= 0) {
      const bool notifyPresets = pendingPresetPersistence.notifyPresets;
      const bool applyMachineTemperature =
          pendingPresetPersistence.applyMachineTemperature;
      WebCommand persisted;
      persisted.requestId = pendingPresetPersistence.requestId;
      reportControlCommandResult(persisted, CommandResultState::PERSISTED);
      pendingPresetPersistence = {};
      if (applyMachineTemperature) requestActivePresetMachineTemperature();
      if (notifyPresets) queueWebhookPresetsChanged();
    }
    if (!scaleWorkerClearPreferredDirty(persistedSettings.preferredScaleMac,
                                       persistedSettings.preferredScaleName,
                                       persistedSettings.scaleHistory)) {
      const ScaleLinkSnapshot link = getScaleLinkSnapshot();
      if (scaleMacNvsWriteAllowed(link.connecting,
                                  link.state == ScaleLinkState::CONNECTED)) {
        runtimePersistPending = true;
        runtimePersistIoRetryMs = 0;
        runtimePersistReasonBits |= RUNTIME_PERSIST_REASON_SCALE_MAC;
      }
    }
    if (static_cast<int32_t>(runtimePersistRevision - runtimeRevision) > 0) {
      runtimePersistPending = true;
      runtimePersistIoRetryMs = 0;
      runtimePersistRetryAtMs = millis();
    } else if (!runtimePersistPending) {
      runtimePersistReasonBits = 0;
    }
    if (static_cast<int32_t>(runtimePersistRevision - runtimeRevision) <= 0 &&
        quickSettingsPersistPending) {
      quickSettingsPersistPending = false;
      queueWebhookQuickSettingsChanged();
    }
  } else {
    if (!runtimePersistFailed) {
      addDebugEvent(DebugCategory::CONFIG, DebugCode::RUNTIME_PERSIST_FAILED,
                    static_cast<int32_t>(runtimeRevision),
                    runtimePersistReasonBits);
    }
    runtimePersistFailed = true;
    runtimePersistPending = true;
    if (!lockContended) {
      runtimePersistIoRetryMs = nextPersistenceIoRetry(
          runtimePersistIoRetryMs, RUNTIME_PERSIST_RETRY_MS);
    }
    runtimePersistRetryAtMs =
        millis() + (lockContended ? RUNTIME_PERSIST_RETRY_MS
                                  : runtimePersistIoRetryMs);
  }
}

bool dispatchSettingsPersist() {
  if (!settingsSchemaWritesAdmitted.load(std::memory_order_acquire)) return false;
  if (!settingsPersistenceAvailable() || settingsPersistInFlight) {
    if (!settingsPersistenceAvailable()) {
      runtimePersistFailed = true;
    }
    return false;
  }
  SettingsPersistRequest &request = settingsPersistRequest;
  networkManager.copySettings(request.blob);
  request.blob.runtime = overlaySessionQuickSettings(sessionQuickSettings, runtimeConfig, true);
  request.blob.bullseyeMelody = bullseyeMelodyConfig;
  request.blob.presets = presetBank;
  copyPreferredScaleMac(request.blob.preferredScaleMac,
                        sizeof(request.blob.preferredScaleMac));
  copyPreferredScaleName(request.blob.preferredScaleName,
                         sizeof(request.blob.preferredScaleName));
  copyScaleHistory(request.blob.scaleHistory);
  memcpy(persistedSettings.preferredScaleMac, request.blob.preferredScaleMac,
         sizeof(persistedSettings.preferredScaleMac));
  memcpy(persistedSettings.preferredScaleName, request.blob.preferredScaleName,
         sizeof(persistedSettings.preferredScaleName));
  memcpy(persistedSettings.scaleHistory, request.blob.scaleHistory,
         sizeof(persistedSettings.scaleHistory));
  request.runtimeRevision = runtimeConfig.revision;
  // Publish ownership before waking a worker that may finish immediately.
  settingsPersistMux.lock();
  settingsPersistInFlight = true;
  settingsPersistMux.unlock();
  const PersistenceWork work = PersistenceWork::SETTINGS;
  if (xQueueSend(settingsPersistQueue, &work, 0) != pdTRUE) {
    settingsPersistMux.lock();
    settingsPersistInFlight = false;
    settingsPersistMux.unlock();
    return false;
  }
  runtimePersistPending = false;
  return true;
}
#endif

// Durable flush of the staged BLE scan settings. Runs on the settings_persist
// worker (never the Arduino control loop); the worker's 1 s idle cadence is
// the retry backoff on failure.
bool serviceBleScanPersist() {
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Host harness: no NVS layer; a staged change completes immediately.
  bleScanPersistMux.lock();
  const bool pending = bleScanPersistPending ||
                       bleScanEnabledPersistPending;
  bleScanPersistPending = false;
  bleScanEnabledPersistPending = false;
  bleScanPersistResultOk = true;
  bleScanPersistResultReady = pending;
  bleScanPersistMux.unlock();
  return true;
#else
  bleScanPersistMux.lock();
  const bool pending = bleScanPersistPending;
  const bool enabledPending = bleScanEnabledPersistPending;
  // An unstaged field keeps its durable value, so both share one save.
  const uint8_t intensity = pending ? bleScanPersistIntensity
                                    : bleScanPersistedSettings.scanIntensity;
  const uint8_t enabled = enabledPending ? bleScanPersistEnabled
                                         : bleScanPersistedSettings.enabled;
  bleScanPersistMux.unlock();
  if (!pending && !enabledPending) {
    return true;
  }
  const bool ok =
      persistBleScanSettings(bleScanPersistedSettings, intensity, enabled);
  bleScanPersistMux.lock();
  if (ok && pending && bleScanPersistIntensity == intensity) {
    // A newer staged value stays pending for the next idle pass.
    bleScanPersistPending = false;
  }
  if (ok && enabledPending && bleScanPersistEnabled == enabled) {
    bleScanEnabledPersistPending = false;
  }
  bleScanPersistResultOk = ok;
  bleScanPersistResultReady = true;
  bleScanPersistMux.unlock();
  return ok;
#endif
}

// Control side: hand the quick start record to the settings_persist worker.
void stageBbwQuickStart() {
  bbwQuickStartMux.lock();
  bbwQuickStartStaged = bbwQuickStart;
  bbwQuickStartPersistPending = true;
  bbwQuickStartMux.unlock();
}

// Worker side, on the idle cadence that is also the retry backoff.
void serviceBbwQuickStartPersist() {
  bbwQuickStartMux.lock();
  const bool pending = bbwQuickStartPersistPending;
  const QuickStartRecord record = bbwQuickStartStaged;
  bbwQuickStartPersistPending = false;
  bbwQuickStartMux.unlock();
  if (!pending) return;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const bool saved = saveQuickStart(record);
#else
  const bool saved = hostQuickStartSaveSucceeds;
  if (saved) hostLastSavedQuickStart = record;
#endif
  if (saved) {
    bbwQuickStartPersistFailLatched = false;
    return;
  }
  if (!bbwQuickStartPersistFailLatched)
    addDebugEvent(DebugCategory::CONFIG, DebugCode::RUNTIME_PERSIST_FAILED, 0,
                  RUNTIME_PERSIST_REASON_BBW_QUICK_START);
  bbwQuickStartPersistFailLatched = true;
  bbwQuickStartMux.lock();
  bbwQuickStartPersistPending = true;
  bbwQuickStartMux.unlock();
}

// Control-loop side: consume the worker's flush result, report the held
// BLE scan command as PERSISTED, and surface failures once per latch.
void serviceBleScanPersistResult() {
  bool ready = false;
  bool ok = false;
  bleScanPersistMux.lock();
  ready = bleScanPersistResultReady;
  if (ready) {
    bleScanPersistResultReady = false;
    ok = bleScanPersistResultOk;
  }
  bleScanPersistMux.unlock();
  if (!ready) {
    return;
  }
  if (ok) {
    bleScanPersistFailLatched = false;
    // The combined dual-slot save covers every id accepted since the last
    // flush; report each one, oldest first, outside the staging mutex.
    uint32_t reportedIds[PENDING_BLE_SCAN_REQUEST_CAPACITY];
    uint8_t reportedCount = 0;
    bleScanPersistMux.lock();
    reportedCount = pendingBleScanRequestIdCount;
    for (uint8_t i = 0; i < reportedCount; ++i) {
      reportedIds[i] = pendingBleScanRequestIds[i];
    }
    pendingBleScanRequestIdCount = 0;
    bleScanPersistMux.unlock();
    for (uint8_t i = 0; i < reportedCount; ++i) {
      WebCommand persisted;
      persisted.requestId = reportedIds[i];
      reportControlCommandResult(persisted, CommandResultState::PERSISTED);
    }
  } else if (!bleScanPersistFailLatched) {
    bleScanPersistFailLatched = true;
    addDebugEvent(DebugCategory::CONFIG, DebugCode::RUNTIME_PERSIST_FAILED,
                  0, RUNTIME_PERSIST_REASON_BLE_SCAN);
  }
}

void serviceShotStorePersistence() {
  bool resultReady = false;
  bool resultOk = false;
  bool resultIoFail = false;
  uint32_t resultGeneration = 0;
  shotStorePersistMux.lock();
  resultReady = shotStorePersistResultReady;
  if (resultReady) {
    shotStorePersistResultReady = false;
    resultOk = shotStorePersistResultOk;
    resultIoFail = shotStorePersistResultIoFail;
    resultGeneration = shotStorePersistResultGeneration;
  }
  const bool inFlight = shotStorePersistInFlight;
  shotStorePersistMux.unlock();
  if (resultReady) {
    {
      TaskLockGuard lock(shotStoreMutex);
      const bool current = shotStoreDirtyGeneration.load(std::memory_order_acquire) ==
                           resultGeneration;
      activationStores.acknowledgePersisted(*shotStorePersistImage,
                                            resultOk && current);
      if (resultOk && current) {
        // A capped curve batch keeps the generation dirty for the remainder.
        if (!shotCurves.dirty()) shotStorePersistCleanGeneration = resultGeneration;
        if (shotStorePersistImageLastShotDirty) {
          lastShotNvsDirty = false;
        }
      }
    }
    if (resultOk) {
      shotStorePersistIoRetryMs = 0;
      shotStorePersistRetryAtMs = 0;
      if (controllerStartedPending) (void)enqueueControllerStartedWebhook();
    } else {
      if (resultIoFail) {
        shotStorePersistIoRetryMs = nextPersistenceIoRetry(
            shotStorePersistIoRetryMs, SHOT_STORE_PERSIST_RETRY_MS);
      }
      shotStorePersistRetryAtMs =
          millis() + (resultIoFail ? shotStorePersistIoRetryMs
                                  : SHOT_STORE_PERSIST_RETRY_MS);
    }
  }
  if (inFlight || !settingsPersistenceAvailable()) return;
  if (maintenanceLease.active) return;
  // Clean common case at 100 Hz: nothing mutated since the acknowledged
  // image and no retry in flight — skip the snapshot fetch and both mutex
  // round trips below. Every wrapper mutation bumps the generation, so a
  // failed or superseded persist (dirty flags still set) keeps this false.
  if (shotStoreDirtyGeneration.load(std::memory_order_acquire) ==
      shotStorePersistCleanGeneration) {
    return;
  }
  const ScaleLinkSnapshot link = getScaleLinkSnapshot();
  if (!durableFlashWriteAllowed(session.active || machineIsRunning(),
                                link.connecting)) {
    return;
  }
  const uint32_t dirtyGeneration =
      shotStoreDirtyGeneration.load(std::memory_order_relaxed);
  if (dirtyGeneration != shotStoreObservedDirtyGeneration) {
    shotStoreObservedDirtyGeneration = dirtyGeneration;
    shotStorePersistIoRetryMs = 0;
    shotStorePersistRetryAtMs = 0;
  }
  if (static_cast<int32_t>(millis() - shotStorePersistRetryAtMs) < 0) {
    return;
  }
  {
    TaskLockGuard lock(shotStoreMutex);
    if (!shotLog.dirty() && !shotCurves.dirty() && !historyLog.dirty() &&
        !lastShotNvsDirty) {
      return;
    }
    activationStores.capturePersistenceImage(*shotStorePersistImage);
    if (lastShotNvsDirty) *lastShotPersistImage = lastShotStore;
    shotStorePersistImageLastShotDirty = lastShotNvsDirty;
    shotStorePersistImageGeneration = dirtyGeneration;
  }
  shotStorePersistMux.lock();
  shotStorePersistInFlight = true;
  shotStorePersistMux.unlock();
  const PersistenceWork work = PersistenceWork::SHOT_STORES;
  if (xQueueSend(settingsPersistQueue, &work, 0) != pdTRUE) {
    shotStorePersistMux.lock();
    shotStorePersistInFlight = false;
    shotStorePersistMux.unlock();
    return;
  }
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  PersistenceWork queued = PersistenceWork::SETTINGS;
  if (xQueueReceive(settingsPersistQueue, &queued, 0) == pdTRUE &&
      queued == PersistenceWork::SHOT_STORES) {
    persistShotStoreImage();
    serviceShotStorePersistence();
  }
#endif
}

void serviceRuntimePersistence(
    const ScaleLinkSnapshot &link = getScaleLinkSnapshot()) {
  if (networkSettingsPersistRequested.exchange(false, std::memory_order_acq_rel))
    queueRuntimePersist(RUNTIME_PERSIST_REASON_USER);
  if (runtimeConfig.firstTimezoneAutoClockQuality == 1 &&
      runtimeConfig.firstTimezoneAutoBootId == currentBootId()) {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    const uint64_t nowUs = static_cast<uint64_t>(esp_timer_get_time());
#else
    const uint64_t nowUs = static_cast<uint64_t>(millis()) * 1000U;
#endif
    const uint64_t thenUs = runtimeConfig.firstTimezoneAutoUptimeUs;
    const uint32_t nowUtc = g_wallClock.nowUtcSec(millis());
    const uint64_t elapsedSec = nowUs >= thenUs ? (nowUs - thenUs) / 1000000U
                                              : UINT64_MAX;
    if (nowUtc != 0 && elapsedSec < nowUtc) {
      runtimeConfig.firstTimezoneAutoUtcSec =
          nowUtc - static_cast<uint32_t>(elapsedSec);
      runtimeConfig.firstTimezoneAutoClockQuality = 3;
      ++runtimeConfig.revision;
      if (runtimeConfig.revision == 0) runtimeConfig.revision = 1;
      queueRuntimePersist(RUNTIME_PERSIST_REASON_USER);
      publishRecipeState();
    }
  }
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  serviceSettingsPersistResult();
  serviceBleScanPersistResult();
  if (!runtimePersistPending) return;
  bool inFlight = false;
  settingsPersistMux.lock();
  inFlight = settingsPersistInFlight;
  settingsPersistMux.unlock();
  if (inFlight || maintenanceLease.active ||
      static_cast<int32_t>(millis() - runtimePersistRetryAtMs) < 0 ||
      !controlAllowsConfigurationNow() ||
      !durableFlashWriteAllowed(false, link.connecting)) {
    return;
  }
  if (!dispatchSettingsPersist()) {
    runtimePersistRetryAtMs = millis() + RUNTIME_PERSIST_RETRY_MS;
  }
#else
  serviceBleScanPersist();  // host: staged flush completes immediately
  serviceBleScanPersistResult();
  if (!settingsPersistenceAvailable()) {
    if (runtimePersistPending) {
      runtimePersistFailed = true;
    }
    return;
  }
  if (!runtimePersistPending || maintenanceLease.active ||
      static_cast<int32_t>(millis() - runtimePersistRetryAtMs) < 0 ||
      !controlAllowsConfigurationNow() ||
      !durableFlashWriteAllowed(false, link.connecting)) {
    return;
  }
  ++hostRuntimePersistAttempts;
  if (!hostRuntimePersistSucceeds) {
    if (!runtimePersistFailed) {
      addDebugEvent(DebugCategory::CONFIG, DebugCode::RUNTIME_PERSIST_FAILED,
                    static_cast<int32_t>(runtimeConfig.revision),
                    runtimePersistReasonBits);
    }
    runtimePersistFailed = true;
    runtimePersistPending = true;
    runtimePersistIoRetryMs = nextPersistenceIoRetry(
        runtimePersistIoRetryMs, RUNTIME_PERSIST_RETRY_MS);
    runtimePersistRetryAtMs = millis() + runtimePersistIoRetryMs;
    return;
  }
  hostLastFlushedRuntime = overlaySessionQuickSettings(sessionQuickSettings, runtimeConfig, true);
  hostLastFlushedPresets = presetBank;
  hostLastFlushIncludedLive = true;
  runtimePersistPending = false;
  runtimePersistReasonBits = 0;
  runtimePersistFailed = false;
  runtimePersistIoRetryMs = 0;
  if (pendingPresetPersistence.requestId != 0 &&
      static_cast<int32_t>(runtimeConfig.revision -
                           pendingPresetPersistence.revision) >= 0) {
    const bool notifyPresets = pendingPresetPersistence.notifyPresets;
    const bool applyMachineTemperature =
        pendingPresetPersistence.applyMachineTemperature;
    pendingPresetPersistence = {};
    if (applyMachineTemperature) requestActivePresetMachineTemperature();
    if (notifyPresets) queueWebhookPresetsChanged();
  }
  if (quickSettingsPersistPending) {
    quickSettingsPersistPending = false;
    queueWebhookQuickSettingsChanged();
  }
#endif
}

void persistBleScanIntensity(BleScanIntensity intensity);

void completeMaintenanceLease(const WebCommand &result) {
  if (!maintenanceLease.active ||
      result.maintenanceLeaseId != maintenanceLease.id) {
    addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_REJECTED,
                  static_cast<int32_t>(result.type));
    return;
  }
  if (result.succeeded && maintenanceLease.applyRuntimeOnSuccess) {
    // Live runtime is apply-first. Never copy persist results over RAM.
    addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED,
                  static_cast<int32_t>(runtimeConfig.revision));
    if (maintenanceLease.command.type == WebCommandType::PERSIST_RUNTIME) {
      runtimePersistReasonBits = 0;
    }
    requestBookooSilenceIfConfigured();
  }
  if (result.succeeded &&
      maintenanceLease.command.type == WebCommandType::CLEAR_RESET_HISTORY) {
    safetyResetStatus.resetHistoryCount = 0;
    for (ResetHistoryEntry &entry : safetyResetStatus.resetHistory)
      entry = ResetHistoryEntry{};
  }
  if (!result.succeeded &&
      maintenanceLease.command.type == WebCommandType::PERSIST_RUNTIME &&
      maintenanceLease.id == pendingPresetPersistence.requestId) {
    if (!runtimePersistFailed) {
      addDebugEvent(
          DebugCategory::CONFIG, DebugCode::RUNTIME_PERSIST_FAILED,
          static_cast<int32_t>(maintenanceLease.command.config.revision),
          runtimePersistReasonBits);
    }
    runtimePersistFailed = true;
    // RUNTIME_PERSIST_FAILED already formats a detailed Serial line.
    runtimePersistPending = true;
    runtimePersistIoRetryMs = nextPersistenceIoRetry(
        runtimePersistIoRetryMs, RUNTIME_PERSIST_RETRY_MS);
    runtimePersistRetryAtMs = millis() + runtimePersistIoRetryMs;
  }
  addDebugEvent(DebugCategory::SECURITY,
                result.succeeded ? DebugCode::MAINTENANCE_COMPLETED
                                 : DebugCode::COMMAND_FAILED,
                static_cast<int32_t>(maintenanceLease.id));
  maintenanceLease = MaintenanceLease{};
  outboundMaintenance.store(false, std::memory_order_release);
  if (machineLastIntention().holdActive) {
    transitionTo(StopperState::REQUIRES_OFF);
  }
}

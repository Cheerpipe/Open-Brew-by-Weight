namespace {
// Stable field order gives each value one bounded fingerprint. Scratch and TX
// reuse the HTTP workspace; no JSON tree, event queue or new task is allocated.
struct HomeStreamDelta {
  uint32_t *hashes = nullptr;
  size_t capacity = 0, index = 0, used = 0, changed = 0;
  bool snapshot = false, ok = true;
  // The next field updates its fingerprint without being sent.
  bool skipNext = false;

  // Integral values fingerprint themselves, tagged so a zeroed slot never
  // matches, and are formatted only when emitted.
  static constexpr uint32_t kIntegralTag = 0x9E3779B9U;
  bool emits(uint32_t hash) const { return (snapshot || hashes[index] != hash) && !skipNext; }

  void field(const char *path, bool value) __attribute__((noinline)) {
    hashedField(path, kIntegralTag ^ (value ? 1U : 0U), value ? "true" : "false");
  }
  void field(const char *path, float value) __attribute__((noinline)) {
    if (std::isfinite(value)) field(path, "%.1f", static_cast<double>(value));
    else field(path, "null");
  }
  void field(const char *path, unsigned long value) __attribute__((noinline)) {
    const uint32_t hash = kIntegralTag ^ static_cast<uint32_t>(value);
    char text[24] = "";
    if (ok && index < capacity && emits(hash)) snprintf(text, sizeof(text), "%lu", value);
    hashedField(path, hash, text);
  }

  void field(const char *path, const char *format, ...) __attribute__((format(printf, 3, 4))) {
    if (!ok || index >= capacity) { ok = false; return; }
    // Fields are short; hash from stack scratch and keep the PSRAM work
    // buffer out of the steady-state read path. Overflow falls back to the
    // full buffer, so the hashed and emitted bytes are identical either way.
    char scratch[256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(scratch, sizeof(scratch), format, args);
    va_end(args);
    const char *value = scratch;
    if (length < 0) { ok = false; return; }
    if (static_cast<size_t>(length) >= sizeof(scratch)) {
      va_start(args, format);
      length = vsnprintf(g_work->jsonItem, NetworkWorkBuf::kJsonItem, format, args);
      va_end(args);
      value = g_work->jsonItem;
      if (length < 0 || static_cast<size_t>(length) >= NetworkWorkBuf::kJsonItem) {
        ok = false;
        return;
      }
    }
    hashedField(path, crc32Update(0xFFFFFFFFU,
        reinterpret_cast<const uint8_t *>(value), length), value);
  }

  // A value whose fingerprint is already known: no re-format, no re-hash.
  void hashedField(const char *path, uint32_t hash, const char *value) {
    if (!ok || index >= capacity) { ok = false; return; }
    if (emits(hash)) {
      ok = statusJsonAppend(&used, "%s\"%s\":%s", changed ? "," : "", path, value);
      ++changed;
    }
    hashes[index] = hash;
    skipNext = false;
    ++index;
  }
};
}  // namespace

// This bounded presentation projection favors flash size over loop unrolling;
// control and safety keep the profile's normal performance optimization.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendHomeStream(UiStreamSession &session,
                                      const ControlStatusSnapshot &control,
                                      const NetworkStatusSnapshot &network) {
  uiStreamSendKind = "home";
  const bool initial = session.homeResync || session.homeBoot != control.bootId;
  auto &hashes = workBuf_->uiFingerprints[&session - uiStreams_];
  HomeStreamDelta delta{hashes.homeHashes, UiStreamFingerprints::kHomeFields, 0, 0, 0, initial};
  // A snapshot names its identity; an increment continues the last one.
  delta.ok = initial
      ? statusJsonAppend(&delta.used,
            "{\"v\":1,\"type\":\"home\",\"boot\":%lu,\"snapshot\":true,\"changes\":{",
            static_cast<unsigned long>(control.bootId))
      : statusJsonAppend(&delta.used, "{\"type\":\"home\",\"changes\":{");
  bool unlocked = DEVELOPMENT_BUILD, override = false;
  {
    TaskLockGuard lock(dataMux_);
    unlocked |= adminUnlocked_ && strcmp(adminUnlockClientId_, session.clientId) == 0 &&
        static_cast<int32_t>(millis() - adminUnlockUntilMs_) < 0;
    override = webUiOverrideActive_ && static_cast<int32_t>(millis() - webUiOverrideUntilMs_) < 0;
  }
  unlocked |= session.fromSoftAp;
  delta.field("firmwareVersion", "\"%s\"", FW_VERSION);
  delta.field("bootId", static_cast<unsigned long>(control.bootId));
  delta.field("configMutable", bool(controlAllowsConfiguration(control)));
  delta.field("snapshotStale", bool(control.snapshotStale));
  delta.field("webUiOverrideActive", bool(override));
  delta.field("liveShot", bool(control.activeCycle || control.relayClosed));
  delta.field("machineType", "\"%s\"", compiledMachineTypeId());
  delta.field("diagnosticPageVisible", bool(diagnosticPageEnabled()));
  delta.field("compatibilityMode", bool(firmwareCompatibilityMode()));
  delta.field("adminUnlocked", bool(unlocked));
  delta.field("development", bool(DEVELOPMENT_BUILD));
  {
    // Optional login: the Home notice and a pending Home Assistant pairing
    // request, which every view shows as a prompt.
    bool login = false, notice = false;
    uint32_t pairId = 0, pairRemainingS = 0;
    char pairCode[5] = {};
    {
      TaskLockGuard lock(dataMux_);
      const uint32_t now = millis();
      webAccessTickPairing(g_webAccess, now);
      login = g_webAccess.stored.enabled != 0 && !passwordIsFactoryDefault(settings_);
      notice = !login && g_webAccess.stored.noticeDismissed == 0 && !session.fromSoftAp;
      if (login && g_webAccess.pairing.state == WebAccessPairing::PENDING) {
        pairId = g_webAccess.pairing.id;
        pairRemainingS = (g_webAccess.pairing.untilMs - now + 999U) / 1000U;
        memcpy(pairCode, g_webAccess.pairing.code, sizeof(pairCode));
      }
    }
    delta.field("access.login", login);
    delta.field("access.notice", notice);
    delta.field("access.pairId", static_cast<unsigned long>(pairId));
    delta.field("access.pairCode", "\"%s\"", pairCode);
    delta.field("access.pairRemainingS", static_cast<unsigned long>(pairRemainingS));
  }
  delta.field("lastCommand.requestId", static_cast<unsigned long>(network.lastCommandRequestId));
  delta.field("lastCommand.state", "\"%s\"", commandResultStateName(network.lastCommandState));
  const auto &config = control.config;
  const auto clock = g_wallClock.snapshot(millis());
  // Rebase only on connection or clock synchronization; ticking time is local.
  if (initial || session.homeClockSync != clock.lastSyncUtcSec) {
    session.homeClockSync = clock.lastSyncUtcSec;
    session.homeClockUtc = clock.utcSec;
  }
  int16_t timezoneOffset = 0;
  timeZoneOffsetAtOrUtc(config.timezoneId, clock.utcSec, timezoneOffset);
  delta.field("timeUtcSec", static_cast<unsigned long>(session.homeClockUtc));
  delta.field("config.appliedTimezoneOffsetMinutes", "%d", static_cast<int>(timezoneOffset));
  delta.field("config.revision", static_cast<unsigned long>(config.revision));
  delta.field("config.timezoneId", "\"%s\"", config.timezoneId);
  delta.field("config.timezoneAutomatic", bool(config.timezoneAutomatic));
  delta.field("config.timezoneInitialized", bool(durableTimezoneSaved().load()));
  delta.field("config.soundAlertsEnabled", !bool(config.soundAlertsMuted));
  delta.field("config.brewByWeight", !bool(config.timerOnly));
  delta.field("config.goalWeightG", goalWeightDgToGrams(config.goalWeightDg));
  delta.field("config.operationalWallMs", static_cast<unsigned long>(config.operationalWallMs));
  delta.field("config.fastBeforeS", config.fastBeforeMs / 1000.0f);
  delta.field("config.slowAfterS", config.slowAfterMs / 1000.0f);
  delta.field("config.fastMarginG", goalWeightDgToGrams(config.fastMarginDg));
  delta.field("config.slowMarginG", goalWeightDgToGrams(config.slowMarginDg));
  delta.field("config.fastRampS", config.fastRampMs / 1000.0f);
  delta.field("config.slowRampS", config.slowRampMs / 1000.0f);
  delta.field("config.fastCurve", "\"%s\"", rampCurveName(config.fastCurve));
  delta.field("config.slowCurve", "\"%s\"", rampCurveName(config.slowCurve));
  delta.field("config.fastCurveHardness", "\"%s\"", rampHardnessName(config.fastCurveHardness));
  delta.field("config.slowCurveHardness", "\"%s\"", rampHardnessName(config.slowCurveHardness));
  delta.field("config.fastExtractionGuardEnabled", bool(config.fastSideEnabled));
  delta.field("config.slowExtractionGuardEnabled", bool(config.slowSideEnabled));
  delta.field("config.autoToManualGuardEnabled", bool(config.scaleLossEnabled));
  delta.field("config.cupProtectionEnabled", bool(config.cupProtectionEnabled));
  delta.field("config.stopIfCupRemoved", bool(config.stopIfCupRemoved));
  delta.field("config.requireCupToStart", bool(config.requireCupToStart));
  delta.field("config.avoidAccidentalTouchEnabled", bool(config.avoidAccidentalTouchEnabled));
  delta.field("config.noScaleBbwMode", "\"%s\"", noScaleBbwModeId(config.noScaleBbwMode));
  delta.field("config.scaleMacCacheMode", "\"%s\"", scaleMacCacheModeId(config.scaleMacCacheMode));
  delta.field("config.rinseEnabled", bool(config.rinseEnabled));
  delta.field("quickRevert.after", "\"%s\"", quickRevertPolicyId(control.quickRevertPolicy));
  delta.field("quickRevert.overrides", "%u", static_cast<unsigned>(control.quickOverrideMask));
  // Whole minutes, rounded up: the header panel shows ceil(remainingS / 60),
  // so a per-second value would only re-hash and resend the field every tick.
  delta.field("quickRevert.remainingS",
              static_cast<unsigned long>((control.quickRevertRemainingMs + 59999) / 60000 * 60));
  delta.field("state", "\"%s\"", stopperStateName(control.state));
  delta.field("stateLabel", "\"%s\"", stateLabel(control.state));
  delta.field("machineState", "\"%s\"", machineRunStateName(control.machineRunState));
  delta.field("machineRunning", bool(control.machineRunning));
  delta.field("physicalActivatorOn", bool(control.physicalActivatorOn));
  delta.field("remoteControlEnabled", bool(control.remoteControlEnabled));
  delta.field("safety.state", "\"%s\"", relaySafetyStateName(control.safetyState));
  delta.field("backflush.waiting", bool(control.backflushWaiting));
  delta.field("backflush.ready", bool(control.backflushReady));
  delta.field("backflush.unresolved", bool(control.backflushUnresolved));
  delta.field("backflush.stopReason", "\"%s\"", control.backflushStopReason);
  delta.field("scale.available", bool(control.scaleAvailable));
  delta.field("scale.streamState", "\"%s\"", weightStreamStateName(control.weightStreamState));
  // Disconnected readings are never inspected. One null transition clears UI.
  if (control.scaleAvailable && control.observedWeightValid && std::isfinite(control.observedWeightG))
    delta.field("scale.observedWeightG", control.observedWeightG);
  else if (control.scaleAvailable && control.currentWeightValid && std::isfinite(control.currentWeightG))
    delta.field("scale.observedWeightG", control.currentWeightG);
  else delta.field("scale.observedWeightG", "null");
  if (control.scaleAvailable && control.currentTimerValid)
    delta.field("scale.timerMs", static_cast<unsigned long>(control.currentTimerMs / 100 * 100));
  else delta.field("scale.timerMs", "null");
  if (callbacks_.copyScaleHistory) callbacks_.copyScaleHistory(g_work->scaleHistory);
  else memset(&g_work->scaleHistory, 0, sizeof(g_work->scaleHistory));
  char friendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  findScaleHistoryFriendlyName(g_work->scaleHistory, control.preferredScaleMac, friendly, sizeof(friendly));
  escapeJsonString(friendly, g_work->historyJson, NetworkWorkBuf::kHistoryJson);
  delta.field("scale.preferredFriendlyName", "\"%s\"", g_work->historyJson);
  delta.field("scale.preferredMac", "\"%s\"", control.preferredScaleMac);
  escapeJsonString(control.preferredScaleName, g_work->historyJson, NetworkWorkBuf::kHistoryJson);
  delta.field("scale.preferredName", "\"%s\"", g_work->historyJson);
  // Whole-second ceiling (UI resolution): the delta hash must change only
  // when the displayed countdown does, not on every stream tick.
  delta.field("scale.macCachePauseRemainingMs",
              static_cast<unsigned long>((control.scaleMacCachePauseRemainingMs + 999U) / 1000U * 1000U));
  delta.field("connections.wifiConnected", bool(network.staState == StaState::CONNECTED));
  delta.field("connections.wifiLevel", "%s", connectionSignalLevelJson(network.staSignalLevel));
  delta.field("connections.bluetoothLevel", "%s", connectionSignalLevelJson(control.scaleSignalLevel));
  escapeJsonString(network.staState == StaState::CONNECTED ? network.iface.staSsid : "", g_work->historyJson, NetworkWorkBuf::kHistoryJson);
  delta.field("connections.wifiName", "\"%s\"", g_work->historyJson);
  escapeConnectedScaleName(control, g_work->scaleHistory, g_work->historyJson, NetworkWorkBuf::kHistoryJson);
  delta.field("connections.bluetoothName", "\"%s\"", g_work->historyJson);
  const auto &tare = control.cupTare;
  const uint8_t idleCode = idleTarePresentationCode(tare, control.cupPresent);
  delta.field("cupPresence.state", "\"%s\"", control.scaleAvailable
      ? cupPresenceStateName(control.cupPresenceState) : "UNKNOWN");
  delta.field("cupPresence.present", bool(control.cupPresent));
  const bool cupWeightValid = control.cupPresent && tare.weightValid && std::isfinite(tare.weightG);
  delta.field("cupPresence.weightG", cupWeightValid ? tare.weightG : NAN);
  delta.field("cupPresence.weightValid", cupWeightValid);
  delta.field("cupPresence.idleTare", "\"%s\"", idleCode == IDLE_WAITING_FOR_SETTLE ? "empty" :
      idleCode == IDLE_READY_FOR_CUP ? "ready" : idleTarePresentationName(idleCode));
  delta.field("cupPresence.accessory", "\"%s\"", accessoryStateName(tare.accessoryState));
  delta.field("cupPresence.accessoryG",
              tare.accessoryState == AccessoryState::PRESENT ? tare.accessoryG : NAN);
  delta.field("cupPresence.anchorDeltaG", tare.anchorDeltaG);
  delta.field("cupPresence.anchorWaitS", static_cast<unsigned long>(tare.anchorWaitMs / 1000U));
  delta.field("cycle.active", bool(control.activeCycle));
  delta.field("noScaleShotGuard.enabled", bool(control.noScaleShotGuardEnabled));
  delta.field("noScaleShotGuard.armed", bool(control.noScaleShotGuardArmed));
  if (callbacks_.copyPresetBank) callbacks_.copyPresetBank(&g_work->presetBank);
  else memset(&g_work->presetBank, 0, sizeof(g_work->presetBank));
  // Rebuild the slim JSON only when the render changed or another handler
  // reused the shared buffer (presetsJsonHome). Single-threaded: httpd dispatch.
  uint32_t &renderedPresetsHash = g_work->renderedPresetsHash;
  refreshRenderedPresets(callbacks_, g_work->presetBank);
  if (!g_work->presetsJsonHome) {
    buildSlimPresetsJson();
    renderedPresetsHash = crc32Update(0xFFFFFFFFU,
        reinterpret_cast<const uint8_t *>(g_work->presetsJson),
        strlen(g_work->presetsJson));
    g_work->presetsJsonHome = true;
  }
  // A reconnecting page that still holds this exact presets JSON gets only
  // its tag and keeps its copy; any later snapshot sends them in full.
  delta.skipNext = initial && session.knownPresetsTag != 0 &&
      session.knownPresetsTag == renderedPresetsHash;
  session.knownPresetsTag = 0;
  delta.hashedField("presets", renderedPresetsHash, g_work->presetsJson);
  delta.field("presetsTag", static_cast<unsigned long>(renderedPresetsHash));
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  const auto micra = machineIntegrationStatus();
  const auto ws = machineIntegrationWebSocketStatus(false);
  uint8_t options, connection;
  { TaskLockGuard lock(dataMux_); options = settings_.lineaMicra.options; connection = settings_.lineaMicra.connectionType; }
  delta.field("machineIntegration", "\"linea_micra_cloud\"");
  delta.field("lineaMicra.powerState", "\"%s\"", lineaMicraPowerStateName(micra.powerState));
  delta.field("lineaMicra.quality", "\"%s\"", lineaMicraObservationQualityName(micra.quality));
  delta.field("lineaMicra.optimisticOn", bool(micra.optimisticOn));
  delta.field("lineaMicra.optimisticOff", bool(micra.optimisticOff));
  delta.field("lineaMicra.accountConfigured", bool(micra.accountConfigured));
  delta.field("lineaMicra.observeState", bool(options & LINEA_MICRA_OBSERVE_STATE));
  delta.field("lineaMicra.connectionType", "\"%s\"", micraConnectionTypeName(connection));
  delta.field("lineaMicra.websocket.cleaningAvailable", bool(ws.cleaningAvailable));
  delta.field("lineaMicra.websocket.cleaning", "\"%s\"", ws.cleaning == MicraCleaningState::OFF ? "inactive" :
      ws.cleaning == MicraCleaningState::REQUESTED ? "waiting_for_paddle" :
      ws.cleaning == MicraCleaningState::CLEANING ? "cleaning" : "unknown");
  escapeJsonString(ws.cleaningLabel, g_work->historyJson, NetworkWorkBuf::kHistoryJson);
  delta.field("lineaMicra.websocket.cleaningLabel", "\"%s\"", g_work->historyJson);
  delta.field("lineaMicra.readiness", "\"%s\"",
              lineaMicraReadinessName(lineaMicraReadiness(micra)));
  delta.field("lineaMicra.boiler.coffee.state", "\"%s\"",
              lineaMicraBoilerStateName(micra.coffeeBoiler));
  delta.field("lineaMicra.boiler.coffee.readyAtUtcSec",
              static_cast<unsigned long>(micra.coffeeReadyAtUtcSec));
  // 0 when the machine has not reported a brew target; the Home row hides it.
  delta.field("lineaMicra.boiler.coffee.targetDeciC", "%u",
              static_cast<unsigned>(micra.targetValid ? micra.targetDeciC : 0));
  delta.field("lineaMicra.boiler.steam.state", "\"%s\"",
              lineaMicraBoilerStateName(micra.steamBoiler));
  delta.field("lineaMicra.boiler.steam.readyAtUtcSec",
              static_cast<unsigned long>(micra.steamReadyAtUtcSec));
#else
  delta.field("machineIntegration", "\"none\"");
#endif
  if (!delta.ok || !statusJsonAppend(&delta.used, "}}")) return false;
  if (!delta.changed) return true;
  // The delta hashes were already consumed for a deferred frame, so only a
  // full snapshot can repair the client.
  bool deferred;
  if (!sendUiStreamFrame(session, delta.used, deferred)) return false;
  if (deferred) {
    session.homeResync = true;
    return true;
  }
  session.homeBoot = control.bootId;
  session.homeResync = false;
  return true;
}

namespace {
// Frame budget shared by every multi-section sender: bounded well under the
// client's 28672-byte message guard with headroom for escaping.
constexpr size_t kUiStreamFrameBudget = 20480;
// Diagnostic telemetry shares one evaluation gate: sampled producers (heap,
// CPU, NVS stats, profiler copies, steady-state RSSI) never run per dispatch
// tick, matching the retired REST poll cost. Emission still requires a value
// change, so an unchanged section costs no frame.
constexpr uint32_t kDiagSlowMs = 4000;
}  // namespace

// Frame delivery shared by every diagnostic/log pass: ownership is re-checked
// immediately before the send, and a clean header abort defers by arming the
// caller's retry flag instead of tearing down the owned socket.
bool OpenBrewByWeightNetwork::sendUiStreamFrame(UiStreamSession &session,
                                           size_t used, bool &deferArm) {
  bool owner;
  { TaskLockGuard lock(dataMux_); owner = strcmp(session.clientId, activeWebUiClientId_) == 0; }
  if (!owner) return false;
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t *>(g_work->statusJson);
  frame.len = used;
  deferArm = false;
  if (httpd_ws_send_frame_async(server_, session.fd, &frame) != ESP_OK) {
    // Same repair rule as the Home stream: defer with a retry armed.
    if (!uiStreamSendCleanAbort) return false;
    deferArm = true;
  }
  return true;
}

// Same bounded projection contract as sendHomeStream: the diagnostic page
// subscribes per view, so a session without the flag never reaches this path.
// This projection is the page's only data source. Event-driven sections ride
// the per-dispatch delta frame; sampled sections (health, NVS, network
// measurements, Micra traffic, profilers) share one 4 s evaluation gate so
// their producers cost no more than the retired REST poll. Every field still
// reaches the wire only when its fingerprint changes, and follow-up frames
// carry snapshot:false deltas that the client merges in order.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendDiagnosticStream(UiStreamSession &session,
                                              const ControlStatusSnapshot &control,
                                              const NetworkStatusSnapshot &network) {
  uiStreamSendKind = "diagnostic";
  const uint32_t now = millis();
  const bool initial = session.diagResync || session.diagBoot != control.bootId;
  const bool slow = initial || now - session.diagSlowAtMs >= kDiagSlowMs;
  auto &hashes = workBuf_->uiFingerprints[&session - uiStreams_];
  HomeStreamDelta delta{hashes.diagHashes, UiStreamFingerprints::kDiagFields, 0, 0, 0, initial};
  delta.ok = initial
      ? statusJsonAppend(&delta.used,
            "{\"v\":1,\"type\":\"diagnostic\",\"boot\":%lu,\"snapshot\":true,\"changes\":{",
            static_cast<unsigned long>(control.bootId))
      : statusJsonAppend(&delta.used, "{\"type\":\"diagnostic\",\"changes\":{");
  // One owner per path: the Home frame, first in every dispatch, carries the
  // shared state (machine, cup, safety state, scale weight and timer, guard
  // switches, identity, timezone, Micra summary) that the page reads from it.
  delta.field("backflush.hardLimitMs", static_cast<unsigned long>(control.safetyHardLimitMs));
  delta.field("backflush.remainingMs", static_cast<unsigned long>(control.safetyRemainingMs));
  delta.field("reedOn", bool(control.reedOn));
  delta.field("relayClosed", bool(control.relayClosed));
  delta.field("controlSource", "\"%s\"", controlSourceName(control.source));
  delta.field("safety.fault", "\"%s\"", relaySafetyFaultName(control.safetyFault));
  delta.field("safety.taskWatchdogReady", bool(control.taskWatchdogReady));
  delta.field("safety.externalHardware", bool(control.externalSafetyPresent));
  delta.field("scale.controlState", "\"%s\"", weightControlStateName(control.weightControlState));
  if (control.scaleRssiValid)
    delta.field("scale.rssi", "%d", static_cast<int>(control.scaleRssi));
  else delta.field("scale.rssi", "null");
  if (control.weightStreamState == WeightStreamState::FRESH &&
      control.scaleWeightUpdateIntervalMs != 0)
    delta.field("scale.weightUpdateIntervalMs",
                static_cast<unsigned long>(control.scaleWeightUpdateIntervalMs));
  else delta.field("scale.weightUpdateIntervalMs", "null");
  // sendHomeStream filled g_work->scaleHistory earlier in this same dispatch.
  char connectedFriendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  findScaleHistoryFriendlyName(g_work->scaleHistory, control.connectedScaleMac,
                               connectedFriendly, sizeof(connectedFriendly));
  char safeConnectedMac[PREFERRED_SCALE_MAC_CAPACITY * 2] = {};
  char safeConnectedFriendly[PREFERRED_SCALE_NAME_CAPACITY * 2] = {};
  sanitizeJsonEmbed(control.connectedScaleMac, safeConnectedMac,
                    sizeof(safeConnectedMac));
  sanitizeJsonEmbed(connectedFriendly, safeConnectedFriendly,
                    sizeof(safeConnectedFriendly));
  delta.field("scale.connectedMac", "\"%s\"", safeConnectedMac);
  delta.field("scale.connectedFriendlyName", "\"%s\"", safeConnectedFriendly);
  delta.field("scale.packetGaps", static_cast<unsigned long>(control.scalePacketGaps));
  delta.field("scale.maxPacketGapMs", static_cast<unsigned long>(control.scaleMaxPacketGapMs));
  delta.field("scale.rejectedPackets", static_cast<unsigned long>(control.scaleRejectedPackets));
  delta.field("scale.reconnects", static_cast<unsigned long>(control.scaleReconnects));
  delta.field("scale.eventsDropped", static_cast<unsigned long>(control.scaleEventsDropped));
  delta.field("scale.recoveredStaleCount", static_cast<unsigned long>(control.scaleRecoveredStaleCount));
  delta.field("scale.recoveredStaleMs", static_cast<unsigned long>(control.scaleRecoveredStaleMs));
  delta.field("scale.lastDisconnectReasonName", "\"%s\"",
              scaleDisconnectReasonName(control.scaleLastDisconnectReason));
  const ScaleBleDiagnostics &ble = control.scaleBleDiagnostics;
  char disconnectSummary[144] = {}, commandSummary[64] = {};
  if (ble.disconnectSequence != 0) {
    snprintf(disconnectSummary, sizeof(disconnectSummary), "%s · %s · %s %ld · %lus ago",
             scaleDisconnectReasonName(ble.disconnectReason),
             scaleBleOriginName(ble.disconnectOrigin),
             scaleBleStatusDomain(ble.disconnectStatus, ble.disconnectOrigin),
             static_cast<long>(ble.disconnectStatus),
             static_cast<unsigned long>(static_cast<uint32_t>(millis() - ble.disconnectAtMs) / 1000));
  }
  if (ble.commandFailureSequence != 0) {
    snprintf(commandSummary, sizeof(commandSummary), "%s · status %ld · %lums",
             scaleBleCommandName(ble.command), static_cast<long>(ble.commandStatus),
             static_cast<unsigned long>(ble.commandElapsedMs));
  }
  delta.field("scale.lastDisconnect.summary", "\"%s\"", disconnectSummary);
  delta.field("scale.lastCommandFailure.summary", "\"%s\"", commandSummary);
  delta.field("scale.model", "\"%s\"", scaleModelName(control.scaleModel));
  delta.field("scale.supportedCommandsKnown",
              bool(control.scaleModel != ScaleModel::Unknown));
  {
    char commandsJson[320] = "[";
    size_t commandBytes = 1;
    ScaleCommandInfo commandInfo;
    for (size_t i = 0;
         scaleBookooCommandAt(control.scaleModel, i, &commandInfo); ++i) {
      const int n = snprintf(commandsJson + commandBytes,
                             sizeof(commandsJson) - commandBytes,
                             "%s{\"name\":\"%s\",\"code\":\"0x%02X\"}",
                             i == 0 ? "" : ",", commandInfo.name,
                             static_cast<unsigned>(commandInfo.code));
      if (n < 0 || static_cast<size_t>(n) >=
                       sizeof(commandsJson) - commandBytes) {
        commandBytes = 1;
        break;
      }
      commandBytes += static_cast<size_t>(n);
    }
    commandsJson[commandBytes++] = ']';
    commandsJson[commandBytes] = '\0';
    delta.field("scale.supportedCommands", "%s", commandsJson);
  }
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  {
    const auto micra = machineIntegrationStatus();
    const bool sampleValid = micra.sampleAtMs != 0 &&
        (micra.quality == LineaMicraObservationQuality::CURRENT ||
         micra.quality == LineaMicraObservationQuality::STALE ||
         micra.quality == LineaMicraObservationQuality::UNSUPPORTED);
    delta.field("lineaMicra.observedMode", "\"%s\"", lineaMicraObservedModeName(micra.observedMode));
    delta.field("lineaMicra.sampleValid", bool(sampleValid));
    // Whole seconds: the UI renders integer age and interpolates locally, so
    // a raw millisecond age would force one frame every dispatch tick.
    delta.field("lineaMicra.sampleAgeMs",
                static_cast<unsigned long>(micra.sampleAtMs == 0 ? 0 : static_cast<uint32_t>(millis() - micra.sampleAtMs) / 1000 * 1000));
    delta.field("lineaMicra.freshnessPolicy", "\"%s\"",
                micra.connectionFreshness ? "connection" : "time");
    delta.field("lineaMicra.freshnessMs",
                static_cast<unsigned long>(micra_timing::kStateFreshnessMs));
    delta.field("lineaMicra.boiler.steam.level", "\"%s\"",
                lineaMicraSteamLevelName(micra.steamLevel));
    // Whole seconds, like sampleAgeMs: the browser interpolates the boiler
    // age and estimate countdowns locally instead of costing delta frames.
    delta.field("lineaMicra.boiler.sampleAgeMs",
                static_cast<unsigned long>(micra.temperatureAtMs == 0
                    ? 0
                    : static_cast<uint32_t>(millis() - micra.temperatureAtMs) / 1000 * 1000));
    delta.field("lineaMicra.powerSource", "\"%s\"", micraPowerSourceName(micra.powerSource));
    // Settings owns these four while subscribed; compatibility mode has no
    // Settings stream, so the slots stay and only their emission is skipped.
    delta.skipNext = session.settingsOn;
    delta.field("lineaMicra.phase", "\"%s\"", lineaMicraPhaseName(micra.phase));
    delta.field("lineaMicra.temperatureState", "\"%s\"", lineaMicraTemperatureStateName(micra.temperatureState));
    delta.field("lineaMicra.temperatureError", "\"%s\"", lineaMicraErrorName(micra.temperatureError));
    char safeEmail[LINEA_MICRA_USERNAME_CAPACITY * 2] = {};
    char safeSerial[LINEA_MICRA_SERIAL_CAPACITY * 2] = {};
    char safeName[LINEA_MICRA_NAME_CAPACITY * 2] = {};
    { TaskLockGuard lock(dataMux_);
      sanitizeJsonEmbed(settings_.lineaMicra.username, safeEmail, sizeof(safeEmail));
      sanitizeJsonEmbed(settings_.lineaMicra.selectedSerial, safeSerial, sizeof(safeSerial));
      sanitizeJsonEmbed(settings_.lineaMicra.selectedName, safeName, sizeof(safeName)); }
    delta.skipNext = session.settingsOn;
    delta.field("lineaMicra.email", "\"%s\"", safeEmail);
    delta.skipNext = session.settingsOn;
    delta.field("lineaMicra.selectedSerial", "\"%s\"", safeSerial);
    delta.skipNext = session.settingsOn;
    delta.field("lineaMicra.selectedName", "\"%s\"", safeName);
    const LineaMicraCloudCall call = machineIntegrationCloudCall();
    if (call.api == nullptr)
      delta.field("lineaMicra.cloudCall", "null");
    else
      delta.field("lineaMicra.cloudCall",
                  "{\"api\":\"%s\",\"method\":\"%s\",\"result\":\"%s\","
                  "\"startedAtUtcSec\":%lu,\"durationMs\":%lu,"
                  "\"httpStatus\":%u,\"transportStatus\":%ld}",
                  call.api, call.method, call.result,
                  static_cast<unsigned long>(call.startedAtUtcSec),
                  static_cast<unsigned long>(call.durationMs),
                  static_cast<unsigned>(call.httpStatus),
                  static_cast<long>(call.transportStatus));
  }
#endif
  // Event-driven sections migrated from the REST diagnostic payload; paths
  // keep their REST names so the shared painters apply unchanged.
  const auto &config = control.config;
  const auto timeStatus = g_wallClock.snapshot(now);
  int16_t timezoneOffset = 0, lastSyncTimezoneOffset = 0;
  const bool timezoneOffsetKnown = timeStatus.utcSec != 0 &&
      timeZoneOffsetAtOrUtc(config.timezoneId, timeStatus.utcSec, timezoneOffset);
  const bool lastSyncOffsetKnown = timeStatus.lastSyncUtcSec != 0 &&
      timeZoneOffsetAtOrUtc(config.timezoneId, timeStatus.lastSyncUtcSec,
                            lastSyncTimezoneOffset);
  delta.field("config.serialLogLevel", "\"%s\"",
              logLevelName(serialLogLevelFromRuntime(config)));
  delta.field("config.ringRetainLogLevel", "\"%s\"",
              logLevelName(static_cast<LogLevel>(config.ringRetainLogLevel)));
  delta.field("config.timezoneOffsetKnown", bool(timezoneOffsetKnown));
  delta.field("config.timezoneResolution", "\"%s\"",
              timeZoneResolution(config.timezoneId, timeStatus.utcSec));
  delta.field("config.lastSyncAppliedTimezoneOffsetMinutes", "%d",
              static_cast<int>(lastSyncTimezoneOffset));
  delta.field("config.lastSyncOffsetKnown", bool(lastSyncOffsetKnown));
  delta.field("maintenance.active", bool(control.maintenanceLeaseActive));
  delta.field("maintenance.leaseId", static_cast<unsigned long>(control.maintenanceLeaseId));
  delta.field("maintenance.persistPending", bool(control.configPersistPending));
  delta.field("maintenance.persistFailed", bool(control.configPersistFailed));
  delta.field("time.state", "\"%s\"", timeSyncStateName(timeStatus.state));
  char safeServer[80] = {};
  sanitizeJsonEmbed(timeStatus.activeServer, safeServer, sizeof(safeServer));
  delta.field("time.activeServer", "\"%s\"", safeServer);
  delta.field("time.lastSyncUtcSec", static_cast<unsigned long>(timeStatus.lastSyncUtcSec));
  char safeStaSsid[80] = {}, safeApSsid[80] = {};
  sanitizeJsonEmbed(network.iface.staSsid, safeStaSsid, sizeof(safeStaSsid));
  sanitizeJsonEmbed(network.iface.apSsid, safeApSsid, sizeof(safeApSsid));
  delta.field("network.apActive", bool(network.apActive));
  delta.field("network.apSsid", "\"%s\"", safeApSsid);
  delta.field("network.apIp", "\"%s\"", network.iface.apIp);
  delta.field("network.wifiConfigured", bool(network.wifiConfigured));
  delta.field("network.ssid", "\"%s\"", safeStaSsid);
  delta.field("network.staState", "\"%s\"", staStateName(network.staState));
  delta.field("network.wifiPs", "\"%s\"", wifiPsLiveName(network.wifiPs));
  delta.field("network.wifiCoex", "\"%s\"", rfCoexPreferenceName(network.wifiCoex));
  if (network.channel != 0)
    delta.field("network.channel", "%u", static_cast<unsigned>(network.channel));
  else delta.field("network.channel", "null");
  delta.field("network.staIp", "\"%s\"", network.iface.staIp);
  delta.field("network.ipMode", "\"%s\"", staIpModeName(network.staIpMode));
  delta.field("network.configState", "\"%s\"", staConfigStateName(network.staConfigState));
  // Whole seconds: the UI ceils the remaining window for display.
  delta.field("network.confirmRemainingMs",
              static_cast<unsigned long>(network.confirmRemainingMs / 1000 * 1000));
  {
    const CrashArchiveStatus crash = crashArchiveStatus();
    delta.field("crashCount", "%u", static_cast<unsigned>(crash.count));
    delta.field("crashState", "%u", static_cast<unsigned>(crash.state));
  }
  delta.field("serial.io4", "\"%s\"",
              usbConsoleIo4StateId(control.usbConsoleIo4Closed));
  delta.field("serial.state", "\"%s\"", usbSerialStateId(control.usbSerialEnableSource));
  delta.field("compileFlags.buzzer", "\"%s\"", compiledBuzzerModeId());
  delta.field("compileFlags.remoteMachineControl",
              bool(REMOTE_MACHINE_CONTROL_ENABLED));
  delta.field("compileFlags.arch", "\"%s\"", FW_BOARD_ARCH_STRING);
  delta.field("compileFlags.hardwareProfile", "\"%s\"", OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID);
  delta.field("compileFlags.machineProfile", "\"%s\"", OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID);
  delta.field("compileFlags.machineBrand", "\"%s\"", OPEN_BREW_BY_WEIGHT_MACHINE_BRAND);
  delta.field("compileFlags.machineModel", "\"%s\"", OPEN_BREW_BY_WEIGHT_MACHINE_MODEL);
  {
    // Static within a boot; one unit-hash blob replaces the REST array.
    size_t used = 0;
    g_work->historyJson[used++] = '[';
    for (uint8_t i = 0; i < control.resetHistoryCount; ++i) {
      const int n = snprintf(g_work->historyJson + used,
                             NetworkWorkBuf::kHistoryJson - used,
                             "%s{\"reason\":\"%s\",\"uptimeMs\":%lu}",
                             i ? "," : "",
                             safetyResetReasonName(control.resetHistory[i].reasonCode),
                             static_cast<unsigned long>(control.resetHistory[i].uptimeMs));
      if (n < 0 || static_cast<size_t>(n) >= NetworkWorkBuf::kHistoryJson - used) break;
      used += static_cast<size_t>(n);
    }
    g_work->historyJson[used++] = ']';
    g_work->historyJson[used] = '\0';
    delta.field("resetHistory", "%s", g_work->historyJson);
  }
  {
    // Guard state machines are event-driven; countdowns hash at whole-second
    // (whole-minute cooldown) resolution so they tick no faster than the
    // display while transitions still land within one dispatch.
    // The guard switches are the Home config.* and noScaleShotGuard.* paths;
    // shared readings (stop weight, early target) have one slot each.
    const bool scaleUsable = control.scaleAvailable &&
        control.weightStreamState == WeightStreamState::FRESH;
    const bool lastShotCupRemoved = control.lastShot.valid &&
        control.lastShot.endReason == EndReason::CUP_REMOVED;
    delta.field("guards.scaleUsable", bool(scaleUsable));
    delta.field("guards.lastShotCupRemoved", bool(lastShotCupRemoved));
    delta.field("guards.targetReachedEarly", bool(control.cycleTargetReachedEarly));
    delta.field("guards.activeStopWeightG", static_cast<float>(control.cycleActiveStopWeightG));
    delta.field("guards.noScale.hold", bool(control.noScaleShotGuardHold));
    delta.field("guards.noScale.cooldownRemainingMs",
                static_cast<unsigned long>(control.noScaleShotGuardCooldownRemainingMs / 60000 * 60000));
    delta.field("guards.noScale.scaleWasAvailable",
                bool(control.noScaleShotGuardScaleWasAvailable));
    delta.field("guards.atm.armed", bool(control.cycleAutoToManualGuardArmed));
    delta.field("guards.atm.enforced", bool(control.cycleAutoToManualGuardEnforced));
    delta.field("guards.atm.remainingMs",
                static_cast<unsigned long>(control.cycleAutoToManualGuardRemainingMs / 1000 * 1000));
    delta.field("guards.slowExtraction.extended", bool(control.cycleSlowExtractionExtended));
    delta.field("guards.fastExtraction.extended", bool(control.cycleExtractionExtended));
    delta.field("guards.fastExtraction.minBbwBrewTimeRemainingMs",
                static_cast<unsigned long>(control.cycleMinBbwBrewTimeRemainingMs / 1000 * 1000));
    delta.field("guards.accidentalTouch.holding", bool(control.cycleAccidentalTouchHolding));
    delta.field("guards.accidentalTouch.phase", "\"%s\"",
                accidentalTouchPhaseName(static_cast<AccidentalTouchPhase>(control.cycleAccidentalTouchPhase)));
    delta.field("guards.accidentalTouch.class", "\"%s\"",
                accidentalTouchClassName(static_cast<AccidentalTouchClass>(control.cycleAccidentalTouchClass)));
    delta.field("guards.accidentalTouch.pendingCount", "%u",
                static_cast<unsigned>(control.cycleAccidentalTouchPendingCount));
    delta.field("guards.cupProtection.startHold", bool(control.cupStartGuardHold));
    delta.field("guards.cupProtection.removedPending", bool(control.cycleCupRemovedPending));
    delta.field("guards.cupProtection.bbwProtectionActive",
                bool(control.cycleBbwProtectionEnabled && !control.cycleBbwProtectionEnded));
    delta.field("guards.cupProtection.bbwProtectionEnded", bool(control.cycleBbwProtectionEnded));
  }
  if (!delta.ok || !statusJsonAppend(&delta.used, "}}")) return false;
  if (delta.changed) {
    if (!sendUiStreamFrame(session, delta.used, session.diagResync)) return false;
    if (session.diagResync) return true;
  }
  if (slow) {
    // Sampled telemetry: one gate, one frame, evaluation at poll cost.
    HomeStreamDelta telemetry{hashes.diagHashes, UiStreamFingerprints::kDiagFields,
                              delta.index, 0, 0, initial};
    telemetry.ok = statusJsonAppend(&telemetry.used, "{\"type\":\"diagnostic\",\"changes\":{");
    // Whole seconds: the uptime renderer shows seconds.
    telemetry.field("health.uptimeMs", static_cast<unsigned long>(control.uptimeMs / 1000 * 1000));
    telemetry.field("health.loopMaxGapMs", static_cast<unsigned long>(control.loopMaxGapMs));
    telemetry.field("health.minimumFreeHeapBytes",
                    static_cast<unsigned long>(control.minimumFreeHeapBytes));
    telemetry.field("health.largestFreeHeapBlockBytes",
                    static_cast<unsigned long>(control.largestFreeHeapBlockBytes));
    telemetry.field("health.psramSizeBytes", static_cast<unsigned long>(control.psramSizeBytes));
    telemetry.field("health.psramFreeBytes", static_cast<unsigned long>(control.psramFreeBytes));
    telemetry.field("health.psramLargestFreeBlockBytes",
                    static_cast<unsigned long>(control.psramLargestFreeBlockBytes));
    telemetry.field("safety.resetReasonCode", static_cast<unsigned long>(control.resetReasonCode));
    {
      const NetworkHeapTelemetrySnapshot networkHeap = heapTelemetrySnapshot();
      const HeapLifecycleAggregate webhookHeap = webhooks_.heapTelemetry();
      HeapLifecycleAggregate micraHeap = {};
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
      micraHeap = machineIntegrationHeapTelemetry();
#endif
      char heapSummary[224] = {};
      snprintf(heapSummary, sizeof(heapSummary),
               "blocks allocated/free %lu/%lu frag %u/1000; "
               "HTTP %ld/%ld WiFi %ld/%ld OTA %ld/%ld webhooks %ld/%ld Micra TLS %ld/%ld",
               static_cast<unsigned long>(control.internalHeapAllocatedBlocks),
               static_cast<unsigned long>(control.internalHeapFreeBlocks),
               static_cast<unsigned>(control.internalHeapFragmentationPermille),
               static_cast<long>(networkHeap.http.lastDelta.largestBlock),
               static_cast<long>(networkHeap.http.worstLargestDelta),
               static_cast<long>(networkHeap.wifi.lastDelta.largestBlock),
               static_cast<long>(networkHeap.wifi.worstLargestDelta),
               static_cast<long>(networkHeap.ota.lastDelta.largestBlock),
               static_cast<long>(networkHeap.ota.worstLargestDelta),
               static_cast<long>(webhookHeap.lastDelta.largestBlock),
               static_cast<long>(webhookHeap.worstLargestDelta),
               static_cast<long>(micraHeap.lastDelta.largestBlock),
               static_cast<long>(micraHeap.worstLargestDelta));
      telemetry.field("health.heap", "\"%s\"", heapSummary);
    }
    const auto &hwmon = control.hwmon;
    telemetry.field("health.hwmon.cpuLoad5s", "%.2f",
                    static_cast<double>(hwmon.cpuLoad5s));
    telemetry.field("health.hwmon.cpuLoad1m", "%.2f",
                    static_cast<double>(hwmon.cpuLoad1m));
    telemetry.field("health.hwmon.cpuLoad5m", "%.2f",
                    static_cast<double>(hwmon.cpuLoad5m));
    telemetry.field("health.hwmon.cpu0Busy", "%.2f",
                    static_cast<double>(hwmon.cpu0Busy));
    telemetry.field("health.hwmon.cpu1Busy", "%.2f",
                    static_cast<double>(hwmon.cpu1Busy));
    telemetry.field("health.hwmon.cpuLoadValid", bool(hwmon.cpuLoadValid));
    telemetry.field("health.hwmon.cpuMhz", static_cast<unsigned long>(hwmon.cpuMhz));
    telemetry.field("health.hwmon.tempValid", bool(hwmon.tempValid));
    telemetry.field("health.hwmon.tempC", "%.1f",
                    static_cast<double>(hwmon.tempC));
    telemetry.field("health.hwmon.tempPeakC", "%.1f",
                    static_cast<double>(hwmon.tempPeakC));
    telemetry.field("health.hwmon.ramTotalBytes", static_cast<unsigned long>(hwmon.ramTotalBytes));
    telemetry.field("health.hwmon.ramUsedBytes", static_cast<unsigned long>(hwmon.ramUsedBytes));
    telemetry.field("health.hwmon.ramFreeBytes", static_cast<unsigned long>(hwmon.ramFreeBytes));
    // Steady-state RSSI sits behind the 1 s sampler; 4 s display matches the
    // retired poll. Link edges still land immediately via the EVENT fields.
    if (network.staLinkMetricsValid)
      telemetry.field("network.rssi", "%d", static_cast<int>(network.staRssi));
    else telemetry.field("network.rssi", "null");
    if (network.staLinkMetricsValid)
      telemetry.field("network.signalQualityPct", "%u",
                      static_cast<unsigned>(network.staSignalQualityPct));
    else telemetry.field("network.signalQualityPct", "null");
    telemetry.field("network.apClients", "%u",
                    static_cast<unsigned>(network.apClients));
    {
      const NvsDiagnosticSnapshot nvs = captureNvsDiagnostics();
      size_t used = 0;
      const int n = snprintf(g_work->historyJson, NetworkWorkBuf::kHistoryJson,
                       "{\"statsValid\":%s,\"layoutExpected\":%s,"
                       "\"partitionBytes\":%lu,\"usedEntries\":%lu,"
                       "\"availableEntries\":%lu,\"freeEntries\":%lu,"
                       "\"totalEntries\":%lu,\"namespaces\":%lu,"
                       "\"failures\":%lu,\"flashIoLockTimeouts\":%lu,"
                       "\"lastFailure\":",
                       nvs.statsValid ? "true" : "false",
                       nvs.partitionBytes == EXPECTED_NVS_PARTITION_BYTES ? "true" : "false",
                       static_cast<unsigned long>(nvs.partitionBytes),
                       static_cast<unsigned long>(nvs.usedEntries),
                       static_cast<unsigned long>(nvs.availableEntries),
                       static_cast<unsigned long>(nvs.freeEntries),
                       static_cast<unsigned long>(nvs.totalEntries),
                       static_cast<unsigned long>(nvs.namespaceCount),
                       static_cast<unsigned long>(nvs.failureCount),
                       static_cast<unsigned long>(flashIoLockTimeouts()));
      if (n > 0 && static_cast<size_t>(n) < NetworkWorkBuf::kHistoryJson)
        used = static_cast<size_t>(n);
      if (nvs.lastFailure.present) {
        const int m = snprintf(g_work->historyJson + used,
                               NetworkWorkBuf::kHistoryJson - used,
                               "{\"subsystem\":\"%s\",\"operation\":\"%s\",\"errorName\":\"%s\"}}",
                               nvsSubsystemName(nvs.lastFailure.subsystem),
                               nvsOperationName(nvs.lastFailure.operation),
                               nvsErrorName(nvs.lastFailure.errorCode));
        if (m > 0 && static_cast<size_t>(m) < NetworkWorkBuf::kHistoryJson - used)
          used += static_cast<size_t>(m);
      } else {
        const int m = snprintf(g_work->historyJson + used,
                               NetworkWorkBuf::kHistoryJson - used, "null}");
        if (m > 0 && static_cast<size_t>(m) < NetworkWorkBuf::kHistoryJson - used)
          used += static_cast<size_t>(m);
      }
      telemetry.field("nvs", "%s", g_work->historyJson);
    }
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
    {
      const MicraWebSocketStatus ws = machineIntegrationWebSocketStatus();
      char safeReason[48] = {};
      sanitizeJsonEmbed(ws.reason, safeReason, sizeof(safeReason));
      // Whole seconds: the UI derives ages from nowMs locally.
      telemetry.field("lineaMicra.websocket.nowMs", static_cast<unsigned long>(now / 1000 * 1000));
      telemetry.field("lineaMicra.websocket.state", "\"%s\"", micraSocketStateName(ws.state));
      telemetry.field("lineaMicra.websocket.reason", "\"%s\"", safeReason);
      telemetry.field("lineaMicra.websocket.subscribed", bool(ws.subscribed));
      telemetry.field("lineaMicra.websocket.machineConnectedKnown", bool(ws.machineConnectedKnown));
      telemetry.field("lineaMicra.websocket.machineConnected", bool(ws.machineConnected));
      telemetry.field("lineaMicra.websocket.messageAtMs", "%u",
                      static_cast<unsigned>(ws.messageAtMs));
      telemetry.field("lineaMicra.websocket.powerAtMs", "%u",
                      static_cast<unsigned>(ws.powerAtMs));
      telemetry.field("lineaMicra.websocket.pongAtMs", "%u",
                      static_cast<unsigned>(ws.pongAtMs));
      telemetry.field("lineaMicra.websocket.retryRemainingMs", "%u",
                      static_cast<unsigned>(!ws.retryAtMs || micra_timing::deadlineReached(now, ws.retryAtMs) ? 0 : ws.retryAtMs - now));
      telemetry.field("lineaMicra.websocket.rxBytes", "%llu",
                      static_cast<unsigned long long>(ws.receiveBytes));
      telemetry.field("lineaMicra.websocket.txBytes", "%llu",
                      static_cast<unsigned long long>(ws.transmitBytes));
      telemetry.field("lineaMicra.websocket.rxBytesPerSecond", "%u",
                      static_cast<unsigned>(ws.receivePerSecond));
      telemetry.field("lineaMicra.websocket.txBytesPerSecond", "%u",
                      static_cast<unsigned>(ws.transmitPerSecond));
      telemetry.field("lineaMicra.websocket.rxBytesPerMinute", "%u",
                      static_cast<unsigned>(ws.receivePerMinute));
      telemetry.field("lineaMicra.websocket.txBytesPerMinute", "%u",
                      static_cast<unsigned>(ws.transmitPerMinute));
      telemetry.field("lineaMicra.websocket.messages", "%u",
                      static_cast<unsigned>(ws.messages));
      telemetry.field("lineaMicra.websocket.errors", "%u",
                      static_cast<unsigned>(ws.errors));
      telemetry.field("lineaMicra.websocket.plannedConnections", "%u",
                      static_cast<unsigned>(ws.plannedConnections));
      telemetry.field("lineaMicra.websocket.unexpectedConnections", "%u",
                      static_cast<unsigned>(ws.unexpectedConnections));
      telemetry.field("lineaMicra.websocket.retainedBytes", "%u",
                      static_cast<unsigned>(ws.retainedBytes));
      telemetry.field("lineaMicra.websocket.stoppedLatencyMs", "%u",
                      static_cast<unsigned>(ws.stoppedLatencyMs));
      telemetry.field("lineaMicra.websocket.maxStoppedLatencyMs", "%u",
                      static_cast<unsigned>(ws.maxStoppedLatencyMs));
      telemetry.field("lineaMicra.websocket.allocationFailures", "%u",
                      static_cast<unsigned>(ws.allocationFailures));
      telemetry.field("lineaMicra.websocket.connectFreeDelta", "%ld",
                      static_cast<long>(ws.heap.lastDelta.freeBytes));
      telemetry.field("lineaMicra.websocket.connectLargestDelta", "%ld",
                      static_cast<long>(ws.heap.lastDelta.largestBlock));
      telemetry.field("lineaMicra.websocket.stopFreeDelta", "%ld",
                      static_cast<long>(ws.stopHeap.lastDelta.freeBytes));
      telemetry.field("lineaMicra.websocket.stopLargestDelta", "%ld",
                      static_cast<long>(ws.stopHeap.lastDelta.largestBlock));
      telemetry.field("lineaMicra.websocket.cleaningAtMs", "%u",
                      static_cast<unsigned>(ws.cleaningAtMs));
    }
#endif
    if (!telemetry.ok || !statusJsonAppend(&telemetry.used, "}}")) return false;
    if (telemetry.changed) {
      if (!sendUiStreamFrame(session, telemetry.used, session.diagResync)) return false;
      if (session.diagResync) return true;
    }
    // Task profiler and scale profile: verbatim REST builders on their own
    // fingerprinted frames (the record-stream no-change contract).
    {
      if (callbacks_.copyTaskProfiler != nullptr)
        callbacks_.copyTaskProfiler(g_work->taskProfiler);
      else
        g_work->taskProfiler = TaskProfilerSnapshot{};
      size_t used = 0;
      if (!statusJsonAppend(&used, "{\"type\":\"diagnostic\",\"changes\":{") ||
          !statusJsonAppendTaskProfiler(&used, g_work->taskProfiler) ||
          !statusJsonAppend(&used, "}}"))
        return false;
      const uint32_t fingerprint = crc32Update(0xFFFFFFFFU,
          reinterpret_cast<const uint8_t *>(g_work->statusJson), used);
      if (initial || fingerprint != session.diagTasksFingerprint) {
        if (!sendUiStreamFrame(session, used, session.diagResync)) return false;
        if (session.diagResync) return true;
        session.diagTasksFingerprint = fingerprint;
      }
    }
    {
      ScaleProfilerStatus scaleProfile{};
      if (callbacks_.copyScaleProfilerStatus != nullptr)
        callbacks_.copyScaleProfilerStatus(scaleProfile);
      size_t used = 0;
      if (!statusJsonAppend(&used, "{\"type\":\"diagnostic\",\"changes\":{") ||
          !statusJsonAppendScaleProfiler(&used, scaleProfile) ||
          !statusJsonAppend(&used, "}}"))
        return false;
      const uint32_t fingerprint = crc32Update(0xFFFFFFFFU,
          reinterpret_cast<const uint8_t *>(g_work->statusJson), used);
      if (initial || fingerprint != session.diagProfileFingerprint) {
        if (!sendUiStreamFrame(session, used, session.diagResync)) return false;
        if (session.diagResync) return true;
        session.diagProfileFingerprint = fingerprint;
      }
    }
    // Audit latch + durable window: fingerprinted frame in its own fragment.
    if (!sendAuditDiagFrame(session, initial)) return false;
    session.diagSlowAtMs = now;
  }
  session.diagBoot = control.bootId;
  session.diagResync = false;
  return true;
}

// Serial-log view subscription: a cheap ring watermark gates the page copy,
// so an idle log costs one mutex-protected sequence read per dispatch. Frames
// reuse the retired REST log payload shape (minus chunked delivery); hasMore
// pages the backlog across dispatches and a cursor reset refills from the
// oldest retained event so the client rebuilds its view without a round trip.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendLogStream(UiStreamSession &session,
                                       const ControlStatusSnapshot &control) {
  uiStreamSendKind = "log";
  const uint32_t latest =
      callbacks_.debugLogLatestSequence ? callbacks_.debugLogLatestSequence() : 0;
  if (!session.logPush && latest == session.logAfter) return true;
  NetworkWorkBuf &work = *g_work;
  const size_t count = callbacks_.copyDebugEvents(
      session.logAfter, work.logBatch, LOG_BATCH_SIZE, &work.logMetadata);
  uint32_t lastSequence = session.logAfter;
  size_t used = 0;
  if (!statusJsonAppend(&used,
          "{\"v\":1,\"type\":\"log\",\"bootId\":%lu,"
          "\"historyOverwritten\":%lu,\"missedEvents\":%lu,"
          "\"serialDropped\":%lu,"
          "\"cursorInvalid\":%s,\"events\":[",
          static_cast<unsigned long>(control.bootId),
          static_cast<unsigned long>(work.logMetadata.historyOverwritten),
          static_cast<unsigned long>(work.logMetadata.missedEvents),
          static_cast<unsigned long>(work.logMetadata.serialDropped),
          work.logMetadata.cursorInvalid ? "true" : "false"))
    return false;
  int16_t eventOffset = 0;
  bool budgetFull = false;
  for (size_t index = 0; index < count && !budgetFull; ++index) {
    const DebugEvent &event = work.logBatch[index];
    eventOffset = 0;
    if (event.wallSec != 0)
      (void)timeZoneOffsetAtOrUtc(control.config.timezoneId, event.wallSec,
                                  eventOffset);
    char message[128];
    const char *text = event.text;
    if (text[0] == '\0') {
      formatDebugEventMessage(event, message, sizeof(message), FW_VERSION);
      text = message;
    }
    escapeJsonString(text, work.jsonItem, NetworkWorkBuf::kJsonItem);
    // Append-only budget: an event that no longer fits ends this frame and
    // resumes on the next dispatch (logPush stays armed).
    if (used + strlen(work.jsonItem) + 256 > kUiStreamFrameBudget) {
      budgetFull = true;
      break;
    }
    if (!statusJsonAppend(&used,
            "%s{\"sequence\":%lu,\"atMs\":%lu,\"wallSec\":%lu,"
            "\"localSec\":%lu,\"level\":\"%s\",\"category\":\"%s\","
            "\"message\":\"%s\"}",
            index == 0 ? "" : ",",
            static_cast<unsigned long>(event.sequence),
            static_cast<unsigned long>(event.atMs),
            static_cast<unsigned long>(event.wallSec),
            static_cast<unsigned long>(event.wallSec +
                                       static_cast<int32_t>(eventOffset) * 60),
            logLevelName(event.level), debugCategoryName(event.category),
            work.jsonItem)) {
      return false;
    }
    lastSequence = event.sequence;
  }
  const bool more = budgetFull || work.logMetadata.hasMore;
  if (!statusJsonAppend(&used, "],\"hasMore\":%s}", more ? "true" : "false")) return false;
  bool deferred = false;
  if (!sendUiStreamFrame(session, used, deferred)) return false;
  if (deferred) return true;
  session.logAfter = work.logMetadata.cursorInvalid ? 0 : lastSequence;
  session.logPush = more;
  return true;
}

// Same bounded, view-scoped contract as the diagnostic sender: only a session
// holding the History subscription reaches this path. A cheap store epoch
// gates the page copy, and a component fingerprint (epoch, window, paging,
// and the raw record bytes) suppresses the send while the page is unchanged —
// the HomeStreamDelta no-change contract. A pending one-shot fetch rides the
// same frame but never disturbs the standing window's bookkeeping.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendHistoryStream(UiStreamSession &session,
                                           const ControlStatusSnapshot &control) {
  uiStreamSendKind = "history";
  const uint32_t epoch = callbacks_.historyEpoch ? callbacks_.historyEpoch() : 0;
  const bool initial =
      session.historyResync || session.historyBoot != control.bootId;
  // A snapshot supersedes any pending append fetch: the client resets its
  // in-flight marker when a replace lands.
  if (initial) session.historyFetch = false;
  const bool fetch = session.historyFetch;
  if (!initial && !fetch && epoch == session.historyEpoch) return true;
  const size_t offset =
      fetch ? session.historyFetchOffset : session.historyOffset;
  // The page is copied under the store mutex by the bridge callback, so a
  // torn multi-word record is never observed. The union member is exclusive
  // to this dispatch; the shot-card tail rebuilds jsonItem only afterwards.
  HistoryPage &page = g_work->historyPage;
  if (callbacks_.copyHistoryPage) {
    callbacks_.copyHistoryPage(page, offset, session.historyLimit, session.historyDir,
                               fetch ? session.historyFetchTypes : session.historyTypes);
  } else {
    page.total = page.start = page.count = 0;
    page.hasMore = false;
  }
  // Component fingerprint (the HomeStreamDelta no-change contract): the raw
  // packed record bytes plus the page/window identity. HistoryRecord is a
  // packed 16-byte POD and time-varying snapshot fields (ui) stay outside,
  // so the value is comparable and storable on snapshot sends too. Worst
  // frame size: 120 maximal records plus the ui block measure ~20.8 KiB,
  // under the client's 24 KiB frame guard.
  uint32_t fingerprint = crc32Update(0xFFFFFFFFU,
      reinterpret_cast<const uint8_t *>(&epoch), sizeof(epoch));
  const uint32_t window[4] = {static_cast<uint32_t>(page.start),
                              static_cast<uint32_t>(session.historyLimit),
                              static_cast<uint32_t>(session.historyDir),
                              session.historyTypes};
  fingerprint = crc32Update(fingerprint,
      reinterpret_cast<const uint8_t *>(window), sizeof(window));
  const uint32_t paging[2] = {static_cast<uint32_t>(page.total),
                              page.hasMore ? 1u : 0u};
  fingerprint = crc32Update(fingerprint,
      reinterpret_cast<const uint8_t *>(paging), sizeof(paging));
  if (page.count)
    fingerprint = crc32Update(fingerprint,
        reinterpret_cast<const uint8_t *>(page.records),
        page.count * sizeof(HistoryRecord));
  if (!initial && !fetch && fingerprint == session.historyFingerprint) {
    // Content identical (e.g. a no-op rating bump): adopt the moved epoch so
    // idle ticks stop at the cheap gate instead of rebuilding the page.
    session.historyEpoch = epoch;
    return true;
  }
  // The fingerprint is the standing page's tag: a page that resubscribes
  // still holding it gets its snapshot with every record as a bare id.
  const bool kept = initial && session.historyKnownTag != 0 &&
                    fingerprint == session.historyKnownTag;
  // Page-level UI state (ownership, edit gate, clock) rides the Home stream.
  size_t used = 0;
  if (!statusJsonAppend(&used,
          "{\"v\":1,\"type\":\"history\",\"boot\":%lu,\"snapshot\":%s,"
          "\"epoch\":%lu,\"request\":%lu,\"tag\":%lu,",
          static_cast<unsigned long>(control.bootId),
          initial ? "true" : "false",
          static_cast<unsigned long>(epoch),
          static_cast<unsigned long>(fetch ? session.historyFetchRequest : session.historyRequest),
          static_cast<unsigned long>(fingerprint)) ||
      !statusJsonAppend(&used,
          "\"total\":%u,\"offset\":%u,\"limit\":%u,"
          "\"hasMore\":%s,\"records\":[",
          static_cast<unsigned>(page.total), static_cast<unsigned>(page.start),
          static_cast<unsigned>(session.historyLimit),
          page.hasMore ? "true" : "false"))
    return false;
  auto &held = workBuf_->uiFingerprints[&session - uiStreams_];
  for (size_t index = 0; index < page.count; ++index) {
    const HistoryRecord &record = page.records[index];
    // A standing refresh names the records the page already holds by id.
    if (kept || (!initial && !fetch &&
                 uiStreamHeld(held.historyHeld, held.historyHeldCount, record.id,
                              uiStreamRowHash(&record, sizeof(record))))) {
      if (!statusJsonAppend(&used, "%s%lu", index == 0 ? "" : ",",
                            static_cast<unsigned long>(record.id)))
        return false;
      continue;
    }
    if (!statusJsonAppend(&used,
            "%s{\"id\":%lu,\"type\":\"%s\",\"durationS\":%.1f,"
            "\"hasWallTime\":%s,\"endedAtUnixSec\":%lu,"
            "\"endedAtLocalSec\":%lu%s}",
            index == 0 ? "" : ",",
            static_cast<unsigned long>(record.id),
            historyTypeName(static_cast<HistoryType>(record.type)),
            static_cast<double>(record.durationDs) / 10.0,
            (record.flags & HISTORY_FLAG_WALL_TIME) != 0 ? "true" : "false",
            static_cast<unsigned long>(record.endedAtUnixSec),
            static_cast<unsigned long>(record.endedAtLocalSec),
            (record.flags & HISTORY_FLAG_NO_WEIGHT) != 0 ? ",\"noWeight\":true" : ""))
      return false;
  }
  if (!statusJsonAppend(&used, "]}")) return false;
  // A deferred send delivered nothing: the snapshot/fetch/epoch state that
  // produced this frame is still armed, so the next dispatch retries it.
  bool deferred;
  if (!sendUiStreamFrame(session, used, deferred)) return false;
  if (deferred) return true;
  session.historyBoot = control.bootId;
  session.historyResync = false;
  session.historyKnownTag = 0;
  session.historyFetch = false;
  if (!fetch) {
    session.historyEpoch = epoch;
    session.historyFingerprint = fingerprint;
    held.historyHeldCount = static_cast<uint8_t>(page.count < kHeldRows ? page.count : kHeldRows);
    for (size_t index = 0; index < held.historyHeldCount; ++index)
      held.historyHeld[index] = {page.records[index].id,
                                 uiStreamRowHash(&page.records[index], sizeof(HistoryRecord))};
  }
  return true;
}

// Same bounded, view-scoped contract as the history sender, plus frame
// budgeting and pacing: full curves make a page larger than one frame, so a
// dispatch delivers one budgeted frame and the page continues across
// dispatch ticks (rowBase/more) until a terminating frame. The cheap store
// epoch gates the record/curve copy, and the fingerprint — epoch, window,
// aggregate, and per-row id/packed-rating/curve hashes, since records are
// immutable after commit except the packed rating byte — suppresses the
// send while the page is unchanged.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendStatsStream(UiStreamSession &session,
                                         const ControlStatusSnapshot &control) {
  uiStreamSendKind = "stats";
  uint32_t epoch =
      callbacks_.shotLogEpoch ? callbacks_.shotLogEpoch() : 0;
  const bool initial =
      session.statsResync || session.statsBoot != control.bootId;
  // Finish the standing page before a later accepted fetch. Cancellation
  // belongs to subscribe/resync handling, never a delayed dispatch.
  const bool fetch = session.statsFetch && !initial &&
      (!session.statsPaging || session.statsPageFetch);
  const uint32_t request = fetch ? session.statsFetchRequest : session.statsRequest;
  // A replacement fetch or store change restarts only the affected page.
  if (session.statsPaging && (fetch != session.statsPageFetch ||
                              session.statsPageEpoch != epoch ||
                              session.statsPageRequest != request || initial)) {
    session.statsPaging = false;
  }
  if (!session.statsPaging && !initial && !fetch && epoch == session.statsEpoch &&
      control.bbwPresetId == session.statsPresetId &&
      session.statsWindow == session.statsShownWindow)
    return true;
  if (session.statsPaging && millis() - session.statsSentAtMs < kStatsStreamPaceMs) {
    uiStreamUrgent_.store(true, std::memory_order_release);
    return true;
  }
  const size_t offset = fetch ? session.statsFetchOffset : session.statsOffset,
               limit = fetch ? session.statsFetchLimit : session.statsLimit;
  const ShotLogSort sort = fetch ? session.statsFetchSort : session.statsSort;
  const ShotLogSortDir dir = fetch ? session.statsFetchDir : session.statsDir;
  const ShotListFilter &filter = fetch ? session.statsFetchFilter : session.statsFilter;
  const uint32_t filterHash = shotListFilterHash(filter);
  NetworkWorkBuf &work = *g_work;
  auto &cache = work.statsCache;
  auto &snapshot = cache.snapshot;
  // Only a record-epoch move reloads the cache: sort/dir toggles rebuild a
  // requested-order index permutation over the untouched newest-first
  // records, so the ~10.8 KB records copy (formerly ~509 KB with every
  // curve) happens only when the store actually changed.
  const bool reload = !cache.valid || snapshot.epoch != epoch;
  if (reload) {
    if (!callbacks_.copyShotStatsRecords) return false;
    callbacks_.copyShotStatsRecords(snapshot);
    epoch = snapshot.epoch;
    cache.curveHashes = {};
  }
  // The header follows the store, the selected window, and the active
  // preset of its "This preset" chart.
  const bool header = !cache.valid || cache.headerEpoch != epoch ||
      cache.headerWindow != session.statsWindow ||
      cache.headerPresetId != control.bbwPresetId;
  if (header) {
    cache.statsOk = formatStatsHeader(cache, session.statsWindow, control.bbwPresetId);
    cache.headerEpoch = epoch;
    cache.headerWindow = session.statsWindow;
    cache.headerPresetId = control.bbwPresetId;
  }
  const size_t count = snapshot.count;
  if (reload || header || cache.offset != offset || cache.limit != limit ||
      cache.sort != sort || cache.dir != dir || cache.filterHash != filterHash) {
    // Materialize the requested order as an index permutation over the
    // newest-first cache: date/desc is the store order, date/asc its
    // reverse, rating an insertion sort of indices, then compacted to the
    // filter's matches. The records themselves never move, so toggles
    // re-slice without recopying or re-sorting data.
    for (size_t index = 0; index < count; ++index) cache.order[index] = static_cast<uint16_t>(index);
    if (sort == ShotLogSort::Rating) {
      for (size_t i = 1; i < count; ++i) {
        const uint16_t key = cache.order[i];
        size_t j = i;
        while (j > 0 && shotLogCompareRatingOrder(
                            snapshot.records[cache.order[j - 1U]],
                            snapshot.records[key], dir) > 0) {
          cache.order[j] = cache.order[j - 1U];
          --j;
        }
        cache.order[j] = key;
      }
    } else if (dir == ShotLogSortDir::Asc) {
      for (size_t index = 0; index < count / 2; ++index) {
        const uint16_t swapped = cache.order[index];
        cache.order[index] = cache.order[count - 1U - index];
        cache.order[count - 1U - index] = swapped;
      }
    }
    cache.matched = 0;
    for (size_t index = 0; index < count; ++index)
      if (shotListFilterMatches(filter, snapshot.records[cache.order[index]]))
        cache.order[cache.matched++] = cache.order[index];
    cache.hasMore = shotLogPageSlice(cache.matched, offset, limit, cache.start, cache.pageCount);
    uint32_t fingerprint = crc32Update(0xFFFFFFFFU,
        reinterpret_cast<const uint8_t *>(&epoch), sizeof(epoch));
    const uint32_t window[5] = {static_cast<uint32_t>(offset), static_cast<uint32_t>(limit),
                                static_cast<uint32_t>(sort), static_cast<uint32_t>(dir),
                                filterHash};
    fingerprint = crc32Update(fingerprint,
        reinterpret_cast<const uint8_t *>(window), sizeof(window));
    // The row format too: a page kept across a firmware update must not be
    // resolved from rows of the old column order.
    fingerprint = crc32Update(fingerprint,
        reinterpret_cast<const uint8_t *>(kStatsRowColumns), strlen(kStatsRowColumns));
    if (cache.statsOk)
      fingerprint = crc32Update(fingerprint,
          reinterpret_cast<const uint8_t *>(cache.header), strlen(cache.header));
    for (size_t index = cache.start; index < cache.start + cache.pageCount; ++index) {
      const ShotLogRecord &record = snapshot.records[cache.order[index]];
      // Curves are immutable once committed: an id-keyed hash lets window
      // changes re-fingerprint rows without re-fetching their curves, and
      // a missing curve hashes over zero points exactly as before.
      const uint32_t *cachedHash = cache.curveHashes.find(record.id);
      uint32_t rowHash;
      if (cachedHash != nullptr) {
        rowHash = *cachedHash;
      } else {
        const bool haveCurve = callbacks_.copyShotCurveById != nullptr &&
            callbacks_.copyShotCurveById(record.id, cache.fetchCurve);
        rowHash = uiStreamCurveHash(cache.fetchCurve,
                                    haveCurve ? cache.fetchCurve.count : 0);
        cache.curveHashes.insert(record.id, rowHash);
      }
      const uint32_t row[2] = {record.id, record.sideFlags};
      fingerprint = crc32Update(fingerprint,
          reinterpret_cast<const uint8_t *>(row), sizeof(row));
      fingerprint = crc32Update(fingerprint,
          reinterpret_cast<const uint8_t *>(&rowHash), sizeof(rowHash));
    }
    cache.fingerprint = fingerprint;
    cache.offset = offset;
    cache.limit = limit;
    cache.sort = sort;
    cache.dir = dir;
    cache.filterHash = filterHash;
    cache.valid = true;
  }
  const uint32_t fingerprint = cache.fingerprint;
  const size_t start = cache.start, pageCount = cache.pageCount;
  // The owner may have committed after the cheap epoch read, before capture.
  if (session.statsPaging && session.statsPageEpoch != epoch) session.statsPaging = false;
  if (!session.statsPaging && !initial && !fetch && fingerprint == session.statsFingerprint) {
    // Content identical (e.g. a no-op rating bump): adopt the moved epoch so
    // idle ticks stop at the cheap gate instead of recopying the records.
    session.statsEpoch = epoch;
    session.statsPresetId = control.bbwPresetId;
    session.statsShownWindow = session.statsWindow;
    return true;
  }
  if (!session.statsPaging) {
    // Page start: identity is captured once and the resync/boot markers move
    // here from completion, so continuation dispatches are not initial.
    ++session.statsSeq;
    session.statsPaging = true;
    session.statsSent = 0;
    session.statsPageEpoch = epoch;
    session.statsPageRequest = request;
    session.statsPageSnapshot = initial;
    session.statsPageFetch = fetch;
    // As in History, the fingerprint is the standing page's tag.
    session.statsPageKept = initial && !fetch && session.statsKnownTag != 0 &&
                            fingerprint == session.statsKnownTag;
    session.statsKnownTag = 0;
    session.statsBoot = control.bootId;
    session.statsResync = false;
    // The header identity a standing page carries: a later window or
    // preset change, even mid-page, sends the header again.
    session.statsShownWindow = fetch ? session.statsShownWindow : session.statsWindow;
    session.statsPresetId = fetch ? session.statsPresetId : control.bbwPresetId;
  }
  const bool first = session.statsSent == 0;
  const size_t sentBase = session.statsSent;
  size_t used = 0;
  if (!statusJsonAppend(&used,
          "{\"v\":1,\"type\":\"stats\",\"boot\":%lu,\"snapshot\":%s,"
          "\"epoch\":%lu,\"seq\":%lu,\"request\":%lu,",
          static_cast<unsigned long>(control.bootId),
          first && session.statsPageSnapshot ? "true" : "false",
          static_cast<unsigned long>(epoch),
          static_cast<unsigned long>(session.statsSeq),
          static_cast<unsigned long>(request)))
      return false;
  if (first && cache.statsOk &&
      !statusJsonAppend(&used, "\"stats\":%s,", cache.header))
    return false;
  if (first && !statusJsonAppend(&used, "\"cols\":\"%s\",\"tag\":%lu,", kStatsRowColumns,
                                 static_cast<unsigned long>(fingerprint)))
    return false;
  if (!statusJsonAppend(&used,
          "\"total\":%u,\"offset\":%u,\"limit\":%u,"
          "\"hasMore\":%s,\"rows\":[",
          static_cast<unsigned>(cache.matched), static_cast<unsigned>(start),
          static_cast<unsigned>(limit), cache.hasMore ? "true" : "false"))
    return false;
  const size_t rowsBase = used;
  auto &held = workBuf_->uiFingerprints[&session - uiStreams_];
  while (session.statsSent < pageCount) {
    const ShotLogRecord &record = snapshot.records[cache.order[start + session.statsSent]];
    // Only the packed rating byte changes after commit; curves are immutable.
    const uint32_t *curveHash = cache.curveHashes.find(record.id);
    const uint32_t key[3] = {record.id, record.sideFlags, curveHash ? *curveHash : 0U};
    const HeldRow row = {record.id, curveHash ? uiStreamRowHash(key, sizeof(key)) : 0U};
    size_t rowBytes;
    if (session.statsPageKept ||
        (row.hash && !session.statsPageSnapshot && !session.statsPageFetch &&
         uiStreamHeld(held.statsHeld, held.statsHeldCount, row.id, row.hash))) {
      // A standing refresh names the rows the page already holds by id.
      rowBytes = static_cast<size_t>(snprintf(work.jsonItem, NetworkWorkBuf::kJsonItem, "%lu",
                                              static_cast<unsigned long>(record.id)));
    } else {
      // Fetch only the curve of the row being framed; short store-lock holds
      // paced across dispatches replace the former whole-store copy.
      const bool haveCurve = callbacks_.copyShotCurveById != nullptr &&
          callbacks_.copyShotCurveById(record.id, cache.fetchCurve);
      rowBytes = formatShotStatsRow(work, record, haveCurve ? &cache.fetchCurve : nullptr);
    }
    if (!rowBytes) return false;
    // Close the frame before this row unless the frame is empty; a single
    // oversized row still fits under the client's frame guard alone.
    if (used + (used > rowsBase ? 1u : 0u) + rowBytes + 48u >
            kStatsStreamFrameBytes &&
        used > rowsBase)
      break;
    if (!statusJsonAppend(&used, "%s%s", used > rowsBase ? "," : "",
                          work.jsonItem))
      return false;
    if (!session.statsPageFetch && session.statsSent < kHeldRows)
      held.statsNext[session.statsSent] = row;
    ++session.statsSent;
  }
  const bool more = session.statsSent < pageCount;
  if (!statusJsonAppend(&used, "],\"rowBase\":%u,\"more\":%s}",
          static_cast<unsigned>(sentBase), more ? "true" : "false"))
    return false;
  // A deferred send delivered nothing: roll the buffered rows back so the
  // next dispatch rebuilds the same frame for the same page.
  bool deferred;
  if (!sendUiStreamFrame(session, used, deferred)) return false;
  if (deferred) {
    session.statsSent = sentBase;
    return true;
  }
  if (more) {
    // Give ACKs time to free TCP credit without delaying live Home updates.
    session.statsSentAtMs = millis();
    uiStreamUrgent_.store(true, std::memory_order_release);
    return true;
  }
  session.statsPaging = false;
  if (fetch) session.statsFetch = false;
  if (!fetch) {
    session.statsEpoch = epoch;
    session.statsFingerprint = fingerprint;
    held.statsHeldCount = static_cast<uint8_t>(pageCount < kHeldRows ? pageCount : kHeldRows);
    memcpy(held.statsHeld, held.statsNext, held.statsHeldCount * sizeof(HeldRow));
  }
  return true;
}

// Settings page projection on the owned UI stream: only the paths Settings
// reads that the Home frame does not carry. Home owns config.revision, the
// shared config.* keys (effective, overrides included), presets, lastCommand,
// quickRevert.{after,overrides,remainingS}, connections, scale.preferred* and
// its lineaMicra.* keys; the page composes both models.
// The standing subscription rides the Home + shot-card batch. One source
// fingerprint gates the evaluation, so an idle dispatch formats nothing, and
// a reconnecting page whose tag still matches gets no snapshot.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendSettingsStream(UiStreamSession &session,
                                            const ControlStatusSnapshot &control) {
  uiStreamSendKind = "settings";
  const auto &config = control.config;
  const bool initial = session.settingsResync || session.settingsBoot != control.bootId;
  char drip[256];
  if (!formatDripChartJson(drip, sizeof(drip), control)) return false;
  if (callbacks_.copyBullseyeConfig) callbacks_.copyBullseyeConfig(&g_work->bullseyeMelody);
  else g_work->bullseyeMelody = BullseyeMelodyConfig{};
  uint32_t readback[5] = {control.bbwPresetId, control.bbwAlpha,
                          control.autoToManualGuardTrendMs, control.scaleLossProjectedSource};
  memcpy(&readback[4], &control.bbwEwmaOffsetG, sizeof(float));
  // The form shows the saved value each Quick Settings override hides.
  g_work->quickSaved[0] = '\0';
  for (size_t index = 0, used = 0; index < QUICK_SETTING_FIELD_COUNT; ++index) {
    if ((control.quickOverrideMask & (1U << index)) == 0) continue;
    const uint8_t value = control.quickSaved[index];
    const bool mode = index == static_cast<size_t>(QuickSettingField::NO_SCALE_BBW_MODE);
    const int n = snprintf(g_work->quickSaved + used, sizeof(g_work->quickSaved) - used,
                           mode ? "%s\"%s\":\"%s\"" : "%s\"%s\":%s", used ? "," : "",
                           QUICK_SETTING_FIELD_IDS[index],
                           mode ? noScaleBbwModeId(value) : value ? "true" : "false");
    if (n < 0 || static_cast<size_t>(n) >= sizeof(g_work->quickSaved) - used) return false;
    used += static_cast<size_t>(n);
  }
  const auto gateOver = [](uint32_t gate, const void *bytes, size_t length) {
    return crc32Update(gate, static_cast<const uint8_t *>(bytes), length);
  };
  // sendHomeStream filled g_work->scaleHistory earlier in this dispatch.
  uint32_t gate = gateOver(0xFFFFFFFFU, &config, sizeof(config));
  gate = gateOver(gate, &g_work->bullseyeMelody, sizeof(g_work->bullseyeMelody));
  gate = gateOver(gate, g_work->scaleHistory, sizeof(g_work->scaleHistory));
  gate = gateOver(gate, drip, strlen(drip));
  gate = gateOver(gate, readback, sizeof(readback));
  gate = gateOver(gate, g_work->quickSaved, strlen(g_work->quickSaved));
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  char email[LINEA_MICRA_USERNAME_CAPACITY] = {}, serial[LINEA_MICRA_SERIAL_CAPACITY] = {},
       name[LINEA_MICRA_NAME_CAPACITY] = {};
  uint8_t options, scaleOptions;
  {
    TaskLockGuard lock(dataMux_);
    options = settings_.lineaMicra.options;
    scaleOptions = settings_.lineaMicra.scaleOptions;
    sanitizeJsonEmbed(settings_.lineaMicra.username, email, sizeof(email));
    sanitizeJsonEmbed(settings_.lineaMicra.selectedSerial, serial, sizeof(serial));
    sanitizeJsonEmbed(settings_.lineaMicra.selectedName, name, sizeof(name));
  }
  const LineaMicraStatus micra = machineIntegrationStatus();
  const LineaMicraDiscoverySnapshot discovery = machineIntegrationDiscovery();
  const MicraWebSocketStatus ws = machineIntegrationWebSocketStatus(false);
  const uint8_t micraState[] = {options, scaleOptions, static_cast<uint8_t>(micra.phase),
                                static_cast<uint8_t>(micra.error), micra.staConnected,
                                micra.apActive, micra.shotPaused,
                                ws.machineConnectedKnown && !ws.machineConnected};
  gate = gateOver(gate, micraState, sizeof(micraState));
  gate = gateOver(gate, &discovery, sizeof(discovery));
  gate = gateOver(gate, email, sizeof(email));
  gate = gateOver(gate, serial, sizeof(serial));
  gate = gateOver(gate, name, sizeof(name));
#endif
  if (!initial && gate == session.settingsGate) return true;
  auto &hashes = workBuf_->uiFingerprints[&session - uiStreams_];
  HomeStreamDelta fields{hashes.settingsHashes, UiStreamFingerprints::kSettingsFields, 0, 0, 0, initial};
  fields.ok = initial
      ? statusJsonAppend(&fields.used,
            "{\"v\":1,\"type\":\"settings\",\"boot\":%lu,\"snapshot\":true,\"changes\":{",
            static_cast<unsigned long>(control.bootId))
      : statusJsonAppend(&fields.used, "{\"type\":\"settings\",\"changes\":{");
  fields.field("buzzerSupported", bool(BUZZER_SUPPORT_ENABLED));
  fields.field("quickRevert.saved", "{%s}", g_work->quickSaved);
  fields.field("config.bbwPresetId", static_cast<unsigned long>(control.bbwPresetId));
  fields.field("config.bbwEwmaOffsetG", "%.2f", static_cast<double>(control.bbwEwmaOffsetG));
  fields.field("config.bbwAlpha", "%.2f", control.bbwAlpha / 100.0);
  // Drip chart and quick start change as one block after each shot.
  fields.field("config.bbwDrip", "{%s}", drip + 1);
  fields.field("config.autoTare", bool(config.autoTare));
  fields.field("config.autoTareOutsideBrew", bool(config.autoTareOutsideBrew));
  fields.field("config.retareAccessoryOutsideBrew",
               bool(idleAccessoryRetareEnabled(config.noScaleBbwMode)));
  fields.field("config.postTareBaselineGraceMs",
               static_cast<unsigned long>(config.postTareBaselineGraceMs));
  fields.field("config.canTareStartTimer", bool(config.canTareStartTimer));
  fields.field("config.scaleTimerStopExtraDelayMs",
               static_cast<unsigned long>(config.scaleTimerStopExtraDelayMs));
  fields.field("config.dripDelayMs", static_cast<unsigned long>(config.dripDelayMs));
  fields.field("config.firstDropBeep", bool(config.firstDropBeep));
  fields.field("config.paddleReturnReminderBeep", bool(config.paddleReturnReminderBeep));
  fields.field("config.paddleReturnReminderIntervalMs",
               static_cast<unsigned long>(config.paddleReturnReminderIntervalMs));
  fields.field("config.paddleReturnReminderMaxDurationMs",
               static_cast<unsigned long>(config.paddleReturnReminderMaxDurationMs));
  fields.field("config.paddleMode", "\"%s\"", paddleModeId(config.paddleMode));
  fields.field("config.stopPulseMs", static_cast<unsigned long>(runtimeStopPulseMs(config)));
  fields.field("config.maxSinglePressMs",
               static_cast<unsigned long>(runtimeMaxSinglePressMs(config)));
  fields.field("config.momentaryStartEdge", "\"%s\"",
               momentaryStartEdgeId(config.momentaryStartOnPress));
  fields.field("config.reedConfirmTimeoutMs",
               static_cast<unsigned long>(runtimeReedConfirmTimeoutMs(config)));
  fields.field("config.assumeIdleWhenScaleConnects", bool(config.assumeIdleWhenScaleConnects));
  fields.field("config.shotReactTimeoutS",
               static_cast<unsigned long>(runtimeShotReactTimeoutS(config)));
  fields.field("config.buzzerScaleLostBeep", bool(config.buzzerScaleLostBeep));
  fields.field("config.buzzerAutoToManualGuardEndBeep", bool(config.buzzerAutoToManualGuardEndBeep));
  fields.field("config.buzzerManualNoScaleBeep", bool(config.buzzerManualNoScaleBeep));
  fields.field("config.buzzerScaleConnectedBeep", bool(config.buzzerScaleConnectedBeep));
  fields.field("config.scaleConnectedLed", bool(config.scaleConnectedLed));
  fields.field("config.buzzerExtendedPulseRate", "\"%s\"",
               extendedPulseRateId(config.buzzerExtendedPulseRate));
  fields.field("config.buzzerSlowExtendedPulseRate", "\"%s\"",
               extendedPulseRateId(config.buzzerSlowExtendedPulseRate));
  fields.field("config.alertOutputChannel", "\"%s\"", alertOutputChannelId(config.alertOutputChannel));
  fields.field("config.rinseGestureMs", static_cast<unsigned long>(config.rinseGestureMs));
  fields.field("config.rinseDurationMs", static_cast<unsigned long>(config.rinseDurationMs));
  fields.field("config.autoRetare", bool(config.autoRetare));
  fields.field("config.retareWindowMs", static_cast<unsigned long>(config.retareWindowMs));
  fields.field("config.minimumCupWeightG", static_cast<float>(config.minimumCupWeightG));
  fields.field("config.retareStabilitySamples",
               static_cast<unsigned long>(config.retareStabilitySamples));
  fields.field("config.retareStabilityToleranceG",
               static_cast<float>(config.retareStabilityToleranceG));
  fields.field("config.retareStabilityMaxGapMs",
               static_cast<unsigned long>(config.retareStabilityMaxGapMs));
  fields.field("config.retareStabilityMinDurationMs",
               static_cast<unsigned long>(config.retareStabilityMinDurationMs));
  fields.field("config.bbwProtectionMs", static_cast<unsigned long>(config.bbwProtectionMs));
  fields.field("config.scaleLossFixedS", config.scaleLossFixedMs / 1000.0f);
  fields.field("config.touchStopFallbackEnabled", bool(config.touchStopFallbackEnabled));
  fields.field("config.cupRemovedWeightG", static_cast<float>(config.cupRemovedWeightG));
  fields.field("config.bookooMuteOnBuzzerOnly", bool(config.bookooMuteOnBuzzerOnly));
  fields.field("config.bookooConnectBeepLevel",
               static_cast<unsigned long>(config.bookooConnectBeepLevel));
  fields.field("config.noScaleAllowRinseWhileArmed",
               bool(noScaleAllowRinseWhileArmed(config.noScaleBbwMode)));
  fields.field("config.lastShotCooldownMs", static_cast<unsigned long>(config.lastShotCooldownMs));
  sanitizeJsonEmbed(g_work->bullseyeMelody.rtttl, g_work->safeBullseyeRtttl,
                    sizeof(g_work->safeBullseyeRtttl));
  fields.field("config.bullseyeMelodyEnabled", bool(g_work->bullseyeMelody.enabled));
  fields.field("config.bullseyeRtttl", "\"%s\"", g_work->safeBullseyeRtttl);
  buildScaleHistoryJson(g_work->scaleHistory);
  fields.field("scale.history", "%s", g_work->historyJson);
  fields.field("scaleLoss.projectedS", control.autoToManualGuardTrendMs / 1000.0f);
  fields.field("scaleLoss.source", "%s",
               shotLogScaleLossSourceJson(static_cast<uint8_t>(control.scaleLossProjectedSource << 3)));
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  fields.field("lineaMicra.email", "\"%s\"", email);
  fields.field("lineaMicra.selectedSerial", "\"%s\"", serial);
  fields.field("lineaMicra.selectedName", "\"%s\"", name);
  fields.field("lineaMicra.applyTemperature", bool(options & LINEA_MICRA_APPLY_TEMPERATURE));
  fields.field("lineaMicra.recognizeWakeGesture", bool(options & LINEA_MICRA_RECOGNIZE_WAKE));
  fields.field("lineaMicra.powerOnWithScale", bool(options & LINEA_MICRA_POWER_ON_WITH_SCALE));
  fields.field("lineaMicra.shutdownWithScale", bool(options & LINEA_MICRA_SHUTDOWN_WITH_SCALE));
  fields.field("lineaMicra.shutdownGraceSeconds",
               static_cast<unsigned long>(lineaMicraShutdownGraceSeconds(options)));
  fields.field("lineaMicra.scaleOffWithMachine",
               bool(scaleOptions & LINEA_MICRA_SCALE_OFF_WITH_MACHINE));
  fields.field("lineaMicra.boostScaleDetectionWhenOn",
               bool(scaleOptions & LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON));
  fields.field("lineaMicra.reduceScaleScanningWhenOff",
               bool(scaleOptions & LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF));
  fields.field("lineaMicra.phase", "\"%s\"", lineaMicraPhaseName(micra.phase));
  fields.field("lineaMicra.error", "\"%s\"", lineaMicraErrorName(micra.error));
  fields.field("lineaMicra.machineOffline", bool(micraState[7]));
  fields.field("lineaMicra.staConnected", bool(micra.staConnected));
  fields.field("lineaMicra.apActive", bool(micra.apActive));
  fields.field("lineaMicra.shotPaused", bool(micra.shotPaused));
  fields.field("lineaMicra.machineListing", bool(discovery.listing));
  fields.field("lineaMicra.machineListError", "\"%s\"", lineaMicraErrorName(discovery.error));
  {
    size_t used = 0;
    g_work->historyJson[used++] = '[';
    for (uint8_t index = 0; index < discovery.count && index < LINEA_MICRA_MAX_ACCOUNT_MACHINES;
         ++index) {
      char safeSerial[LINEA_MICRA_SERIAL_CAPACITY] = {}, safeName[LINEA_MICRA_NAME_CAPACITY] = {};
      sanitizeJsonEmbed(discovery.machines[index].serial, safeSerial, sizeof(safeSerial));
      sanitizeJsonEmbed(discovery.machines[index].name, safeName, sizeof(safeName));
      const int n = snprintf(g_work->historyJson + used, NetworkWorkBuf::kHistoryJson - used,
                             "%s{\"serial\":\"%s\",\"name\":\"%s\"}", index ? "," : "",
                             safeSerial, safeName);
      if (n < 0 || static_cast<size_t>(n) + 2 > NetworkWorkBuf::kHistoryJson - used) return false;
      used += static_cast<size_t>(n);
    }
    g_work->historyJson[used++] = ']';
    g_work->historyJson[used] = '\0';
    fields.field("lineaMicra.machines", "%s", g_work->historyJson);
  }
#endif
  if (!fields.ok) return false;
  // The tag names this exact model: boot plus every slot fingerprint.
  uint32_t tag = gateOver(0xFFFFFFFFU, &control.bootId, sizeof(control.bootId));
  tag = gateOver(tag, fields.hashes, fields.index * sizeof(uint32_t));
  const bool kept = initial && tag == session.settingsKnownTag;
  session.settingsKnownTag = 0;
  if (fields.changed && !kept) {
    if (!statusJsonAppend(&fields.used, "},\"tag\":%lu}", static_cast<unsigned long>(tag)))
      return false;
    // The consumed fingerprints need a snapshot to repair a deferred frame.
    bool deferred;
    if (!sendUiStreamFrame(session, fields.used, deferred)) return false;
    if (deferred) {
      session.settingsResync = true;
      return true;
    }
  }
  session.settingsGate = gate;
  session.settingsBoot = control.bootId;
  session.settingsResync = false;
  return true;
}

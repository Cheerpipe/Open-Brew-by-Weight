const char *OpenBrewByWeightNetwork::wifiScanStateName(WifiScanState state) {
  switch (state) {
    case WifiScanState::IDLE: return "IDLE";
    case WifiScanState::QUEUED: return "QUEUED";
    case WifiScanState::RUNNING: return "RUNNING";
    case WifiScanState::READY: return "READY";
    case WifiScanState::FAILED: return "FAILED";
    case WifiScanState::CANCELED: return "CANCELED";
  }
  return "UNKNOWN";
}

esp_err_t OpenBrewByWeightNetwork::statusHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  // Settings stays claim-gated only. Admin mutations and the full
  // privileged status body require a temporary device-password unlock;
  // Diagnostic is intentionally public: /diagnostic stays reachable and the
  // visibility toggle only hides the menu entry.
  self.requestPendingNetworkConfirm();

  const StatusPage page = parseStatusPage(request->uri);
  if (page == StatusPage::Unknown) {
    return sendError(request, "404 Not Found", "STATUS_PAGE_UNKNOWN",
                     "Unknown status page; use /api/v1/status/{settings|admin|diagnostic}.");
  }

  // JSON / snapshot work lives in one PSRAM-only blob.
  if (!self.lockWorkBufForStatus()) {
    return self.workBufBusy(request);
  }

  ControlStatusSnapshot &control = self.workBuf_->control;
  self.loadControlStatus(control);
  const bool configMutable = controlAllowsConfiguration(control);
  const bool webUiOverrideActive = self.webUiOverrideAllowed(request);
  const uint32_t webUiOverrideRemainingMs =
      webUiOverrideActive ? self.webUiOverrideRemainingMs(request) : 0;
  bool adminUnlocked = false;
  if (page == StatusPage::Admin) {
    adminUnlocked = self.adminUnlockAllowed(request);
    if (adminUnlocked) {
      self.touchAdminUnlock();
    }
  }
  const NetworkStatusSnapshot network = self.snapshot();
  const TimeStatusSnapshot timeStatus = g_wallClock.snapshot(millis());
  int16_t appliedTimezoneOffset = 0;
  const bool timezoneOffsetKnown = timeStatus.utcSec != 0 &&
      timeZoneOffsetAtOrUtc(control.config.timezoneId, timeStatus.utcSec,
                              appliedTimezoneOffset);
  int16_t lastSyncTimezoneOffset = 0;
  const bool lastSyncOffsetKnown = timeStatus.lastSyncUtcSec != 0 &&
      timeZoneOffsetAtOrUtc(control.config.timezoneId, timeStatus.lastSyncUtcSec,
                              lastSyncTimezoneOffset);
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
  LineaMicraStatus micraStatus = {};
  LineaMicraDiscoverySnapshot micraDiscovery = {};
  char safeMicraUsername[LINEA_MICRA_USERNAME_CAPACITY] = {};
  char micraSerial[LINEA_MICRA_SERIAL_CAPACITY] = {};
  char safeMicraSerial[LINEA_MICRA_SERIAL_CAPACITY] = {};
  char micraName[LINEA_MICRA_NAME_CAPACITY] = {};
  char safeMicraName[LINEA_MICRA_NAME_CAPACITY] = {};
  uint8_t micraOptions = 0;
  uint8_t micraScaleOptions = 0;
  uint8_t micraConnectionType = 0;
  bool micraAccountConfigured = false;
  self.dataMux_.lock();
  micraAccountConfigured = self.settings_.lineaMicra.accountConfigured;
  micraOptions = self.settings_.lineaMicra.options;
  micraScaleOptions = self.settings_.lineaMicra.scaleOptions;
  micraConnectionType = self.settings_.lineaMicra.connectionType;
  memcpy(safeMicraUsername, self.settings_.lineaMicra.username,
         sizeof(safeMicraUsername));
  memcpy(micraSerial, self.settings_.lineaMicra.selectedSerial,
         sizeof(micraSerial));
  memcpy(micraName, self.settings_.lineaMicra.selectedName,
         sizeof(micraName));
  self.dataMux_.unlock();
  micraStatus = machineIntegrationStatus();
  micraDiscovery = machineIntegrationDiscovery();
  sanitizeJsonEmbed(safeMicraUsername, safeMicraUsername,
                    sizeof(safeMicraUsername));
  sanitizeJsonEmbed(micraSerial, safeMicraSerial, sizeof(safeMicraSerial));
  sanitizeJsonEmbed(micraName, safeMicraName, sizeof(safeMicraName));
#endif

  char staRssiJson[16] = "null";
  char staSignalQualityJson[16] = "null";
  char staChannelJson[8] = "null";
  if (network.staLinkMetricsValid) {
    snprintf(staRssiJson, sizeof(staRssiJson), "%d",
             static_cast<int>(network.staRssi));
    snprintf(staSignalQualityJson, sizeof(staSignalQualityJson), "%u",
             static_cast<unsigned>(network.staSignalQualityPct));
  }
  if (network.staState == StaState::CONNECTED && network.channel != 0) {
    snprintf(staChannelJson, sizeof(staChannelJson), "%u",
             static_cast<unsigned>(network.channel));
  }
  const int32_t apClosingIn =
      static_cast<int32_t>(network.apTeardownAtMs - millis());
  const uint32_t apClosingMs =
      network.apActive && network.apTeardownAtMs != 0 && apClosingIn > 0
          ? static_cast<uint32_t>(apClosingIn) : 0;

  char safeNtpCustom[NTP_SERVER_HOST_CAPACITY] = {};
  char safeActiveServer[NTP_SERVER_HOST_CAPACITY] = {};
  char safePreferredScaleMac[PREFERRED_SCALE_MAC_CAPACITY * 2] = {};
  char safePreferredScaleName[PREFERRED_SCALE_NAME_CAPACITY * 2] = {};
  char safeFirmwareVersion[32] = {};
  char safeStaSsid[WIFI_SSID_CAPACITY] = {};
  char safeApSsid[WIFI_SSID_CAPACITY] = {};
  char safeDeviceName[DEVICE_NAME_CAPACITY] = {};
  char safeMdnsHost[DEVICE_NAME_CAPACITY] = {};
  char safeWebhookUrl[WEBHOOK_URL_CAPACITY * 2] = {};
  sanitizeJsonEmbed(control.config.ntpServerCustom, safeNtpCustom,
                    sizeof(safeNtpCustom));
  sanitizeJsonEmbed(timeStatus.activeServer, safeActiveServer,
                    sizeof(safeActiveServer));
  sanitizeJsonEmbed(control.preferredScaleMac, safePreferredScaleMac,
                    sizeof(safePreferredScaleMac));
  sanitizeJsonEmbed(control.preferredScaleName, safePreferredScaleName,
                    sizeof(safePreferredScaleName));
  sanitizeJsonEmbed(FW_VERSION, safeFirmwareVersion,
                    sizeof(safeFirmwareVersion));
  sanitizeJsonEmbed(network.iface.staSsid, safeStaSsid, sizeof(safeStaSsid));
  sanitizeJsonEmbed(network.iface.apSsid, safeApSsid, sizeof(safeApSsid));
  sanitizeJsonEmbed(network.deviceName, safeDeviceName, sizeof(safeDeviceName));
  sanitizeJsonEmbed(network.mdnsHost, safeMdnsHost, sizeof(safeMdnsHost));
  const WebhookConfig webhookConfig = self.webhooks_.config();
  const WebhookStatus webhookStatus = self.webhooks_.status();
  sanitizeJsonEmbed(webhookConfig.url, safeWebhookUrl,
                    sizeof(safeWebhookUrl));
  if (page == StatusPage::Settings &&
      self.callbacks_.copyBullseyeConfig != nullptr) {
    self.callbacks_.copyBullseyeConfig(&g_work->bullseyeMelody);
    sanitizeJsonEmbed(g_work->bullseyeMelody.rtttl,
                      g_work->safeBullseyeRtttl,
                      sizeof(g_work->safeBullseyeRtttl));
  }

  const bool needPresets = page == StatusPage::Settings;
  const bool needHistory = page == StatusPage::Settings;
  // Every header names the connected scale; the history array stays Settings-only.
  if (needPresets) {
    if (self.callbacks_.copyPresetBank != nullptr) {
      self.callbacks_.copyPresetBank(&g_work->presetBank);
    } else {
      memset(&g_work->presetBank, 0, sizeof(g_work->presetBank));
    }
    copyPresetStatsForJson(self.callbacks_, g_work->presetBank);
    buildSlimPresetsJson(g_work->presetBank);
  } else {
    g_work->presetsJson[0] = '{';
    g_work->presetsJson[1] = '}';
    g_work->presetsJson[2] = 0;
  }
  g_work->presetsJsonHome = false;
  if (self.callbacks_.copyScaleHistory != nullptr) {
    self.callbacks_.copyScaleHistory(g_work->scaleHistory);
  } else {
    memset(&g_work->scaleHistory, 0, sizeof(g_work->scaleHistory));
  }
  if (needHistory) {
    buildScaleHistoryJson(g_work->scaleHistory);
  } else {
    g_work->historyJson[0] = '[';
    g_work->historyJson[1] = ']';
    g_work->historyJson[2] = 0;
  }
  char preferredScaleFriendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  findScaleHistoryFriendlyName(g_work->scaleHistory, control.preferredScaleMac,
                               preferredScaleFriendly,
                               sizeof(preferredScaleFriendly));
  char safePreferredScaleFriendly[PREFERRED_SCALE_NAME_CAPACITY * 2] = {};
  sanitizeJsonEmbed(preferredScaleFriendly, safePreferredScaleFriendly,
                    sizeof(safePreferredScaleFriendly));
  char connectedScaleFriendly[PREFERRED_SCALE_NAME_CAPACITY] = {};
  findScaleHistoryFriendlyName(g_work->scaleHistory, control.connectedScaleMac,
                               connectedScaleFriendly,
                               sizeof(connectedScaleFriendly));
  char safeConnectedScaleMac[PREFERRED_SCALE_MAC_CAPACITY * 2] = {};
  char safeConnectedScaleFriendly[PREFERRED_SCALE_NAME_CAPACITY * 2] = {};
  sanitizeJsonEmbed(control.connectedScaleMac, safeConnectedScaleMac,
                    sizeof(safeConnectedScaleMac));
  sanitizeJsonEmbed(connectedScaleFriendly, safeConnectedScaleFriendly,
                    sizeof(safeConnectedScaleFriendly));
  char wifiName[WIFI_SSID_CAPACITY * 6] = {};
  char bluetoothName[PREFERRED_SCALE_NAME_CAPACITY * 6] = {};
  escapeJsonString(network.staState == StaState::CONNECTED ? network.iface.staSsid : "",
                   wifiName, sizeof(wifiName));
  escapeConnectedScaleName(control, g_work->scaleHistory,
                           bluetoothName, sizeof(bluetoothName));
  size_t used = 0;
  const bool liveShot =
      control.activeCycle || control.relayClosed;
  // Shared envelope only: page-specific config (NTP, serial debug, buzzer)
  // is appended below so home/settings polls stay lean.
  bool ok = statusJsonAppend(
      &used,
      "{\"firmwareVersion\":\"%s\",\"bootId\":%lu,\"configMutable\":%s,"
      "\"snapshotStale\":%s,"
      "\"webUiOverrideActive\":%s,\"webUiOverrideRemainingMs\":%lu,"
      "\"configLockReason\":\"%s\",\"liveShot\":%s",
      safeFirmwareVersion,
      static_cast<unsigned long>(control.bootId),
      configMutable ? "true" : "false",
      control.snapshotStale ? "true" : "false",
      webUiOverrideActive ? "true" : "false",
      static_cast<unsigned long>(webUiOverrideRemainingMs),
      configLockReason(control), liveShot ? "true" : "false");
  if (ok) {
    ok = statusJsonAppend(
        &used, ",\"machineType\":\"%s\",\"machineIntegration\":\"%s\","
        "\"connections\":{\"wifiConnected\":%s,\"wifiLevel\":%s,"
        "\"bluetoothConnected\":%s,\"bluetoothLevel\":%s,"
        "\"wifiName\":\"%s\",\"bluetoothName\":\"%s\"}",
        compiledMachineTypeId(),
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
        "linea_micra_cloud",
#else
        "none",
#endif
        network.staState == StaState::CONNECTED ? "true" : "false",
        connectionSignalLevelJson(network.staSignalLevel),
        control.scaleAvailable ? "true" : "false",
        connectionSignalLevelJson(control.scaleSignalLevel),
        wifiName, bluetoothName);
  }
  if (ok && page == StatusPage::Admin) {
    const PowerProfile applied = powerAppliedProfile.load(std::memory_order_acquire);
    ok = statusJsonAppend(&used,
        ",\"power\":{\"enabled\":%s,\"requestedProfile\":\"%s\",\"profile\":\"%s\",\"cpuMhz\":%u,"
        "\"minMhz\":%d,\"maxMhz\":%d,\"clockError\":%d,\"bleError\":%d,"
        "\"bleSleep\":%s,\"webActive\":%s,\"cooldownMs\":%lu}",
        control.config.powerManagementEnabled ? "true" : "false",
        powerProfileName(powerRequestedProfile.load(std::memory_order_acquire)),
        powerProfileName(applied), static_cast<unsigned>(getCpuFrequencyMhz()),
        powerMinMhz(applied), powerMaxMhz(applied), powerClockError.load(),
        powerBleError.load(), powerBleSleeping.load() ? "true" : "false",
        powerWebActive(millis()) ? "true" : "false",
        static_cast<unsigned long>(powerCooldownRemainingMs.load()));
  }
  if (ok) {
    ok = statusJsonAppend(
        &used, ",\"quickRevert\":{\"after\":\"%s\",\"overrides\":%u,\"remainingS\":%lu}",
        quickRevertPolicyId(control.quickRevertPolicy),
        static_cast<unsigned>(control.quickOverrideMask),
        static_cast<unsigned long>((control.quickRevertRemainingMs + 999) / 1000));
  }
  if (ok) {
    ok = statusJsonAppend(&used, ",\"diagnosticPageVisible\":%s",
                          self.diagnosticPageEnabled() ? "true" : "false");
  }
  if (ok) {
    ok = statusJsonAppend(&used, ",\"compatibilityMode\":%s",
                          firmwareCompatibilityMode() ? "true" : "false");
  }
  if (ok && page == StatusPage::Settings) {
    ok = statusJsonAppend(&used, ",\"buzzerSupported\":%s",
                          BUZZER_SUPPORT_ENABLED ? "true" : "false");
  }
  if (ok && page == StatusPage::Settings) {
    ok = statusJsonAppend(
        &used, ",\"lastCommand\":{\"requestId\":%lu,\"state\":\"%s\"}",
        static_cast<unsigned long>(network.lastCommandRequestId),
        commandResultStateName(network.lastCommandState));
  }
  if (ok) {
    ok = statusJsonAppend(
        &used, ",\"timeUtcSec\":%lu,\"config\":{\"revision\":%lu,\"ringRetainLogLevel\":\"%s\",\"serialLogLevel\":\"%s\"",
        static_cast<unsigned long>(timeStatus.utcSec),
        static_cast<unsigned long>(control.config.revision),
        logLevelName(static_cast<LogLevel>(control.config.ringRetainLogLevel)),
        logLevelName(serialLogLevelFromRuntime(control.config)));
  }
  if (ok) {
    ok = statusJsonAppend(
        &used, ",\"timezoneId\":\"%s\",\"appliedTimezoneOffsetMinutes\":%d,"
        "\"timezoneOffsetKnown\":%s,\"lastSyncAppliedTimezoneOffsetMinutes\":%d,"
        "\"lastSyncOffsetKnown\":%s,\"tzdbVersion\":\"%s\","
        "\"timezoneResolution\":\"%s\"",
        control.config.timezoneId, static_cast<int>(appliedTimezoneOffset),
        timezoneOffsetKnown ? "true" : "false",
        static_cast<int>(lastSyncTimezoneOffset),
        lastSyncOffsetKnown ? "true" : "false", tzdb::kVersion,
        timeZoneResolution(control.config.timezoneId, timeStatus.utcSec));
  }
  if (ok) {
    ok = statusJsonAppend(&used,
        ",\"timezoneAutomatic\":%s,\"timezoneInitialized\":%s",
        control.config.timezoneAutomatic ? "true" : "false",
        durableTimezoneSaved().load() ? "true" : "false");
  }
  if (ok && page == StatusPage::Admin && adminUnlocked) {
    ok = statusJsonAppend(
        &used,
        ",\"timezoneSource\":%u,"
        "\"firstTimezoneAutoUtcSec\":%lu,\"firstTimezoneAutoUptimeUs\":%llu,"
        "\"firstTimezoneAutoBootId\":%lu,\"firstTimezoneAutoClockQuality\":%u,"
        "\"ntpServerPreset\":\"%s\",\"ntpSyncEnabled\":%s,"
        "\"ntpServerCustom\":\"%s\",\"showDiagnosticPage\":%s,\"powerManagementEnabled\":%s,"
        "\"persistPending\":%s,\"persistFailed\":%s,"
        "\"scheduledRestartEnabled\":%s,\"scheduledRestartMinuteOfDay\":%u,"
        "\"scheduledRestartState\":\"%s\",\"scheduledRestartCause\":\"%s\"",
        static_cast<unsigned>(control.config.timezoneSource),
        static_cast<unsigned long>(control.config.firstTimezoneAutoUtcSec),
        static_cast<unsigned long long>(control.config.firstTimezoneAutoUptimeUs),
        static_cast<unsigned long>(control.config.firstTimezoneAutoBootId),
        static_cast<unsigned>(control.config.firstTimezoneAutoClockQuality),
        ntpPresetId(control.config.ntpServerPreset),
        control.config.ntpSyncEnabled ? "true" : "false", safeNtpCustom,
        control.config.showDiagnosticPage ? "true" : "false",
        control.config.powerManagementEnabled ? "true" : "false",
        control.configPersistPending ? "true" : "false",
        control.configPersistFailed ? "true" : "false",
        control.config.scheduledRestartEnabled != 0 ? "true" : "false",
        static_cast<unsigned>(control.config.scheduledRestartMinuteOfDay),
        scheduledRestartStateName(
            static_cast<ScheduledRestartState>(control.scheduledRestartState)),
        scheduledRestartCauseName(control.scheduledRestartCause));
  }

  if (ok && page == StatusPage::Settings) {
    ok = statusJsonAppend(
        &used,
        ",\"goalWeightG\":%.1f,\"weightOffsetG\":%.2f,"
        "\"weightOffsetBaselineG\":%.2f,\"autoTare\":%s,"
        "\"autoTareOutsideBrew\":%s,\"retareAccessoryOutsideBrew\":%s,"
        "\"postTareBaselineGraceMs\":%lu,\"brewByWeight\":%s,"
        "\"canTareStartTimer\":%s,\"scaleTimerStopExtraDelayMs\":%lu,"
        "\"dripDelayMs\":%lu,"
        "\"soundAlertsEnabled\":%s,\"firstDropBeep\":%s,"
        "\"paddleReturnReminderBeep\":%s,"
        "\"paddleReturnReminderIntervalMs\":%lu,"
        "\"paddleReturnReminderMaxDurationMs\":%lu,\"paddleMode\":\"%s\","
        "\"stopPulseMs\":%lu,\"maxSinglePressMs\":%lu,"
        "\"momentaryStartEdge\":\"%s\",\"reedConfirmTimeoutMs\":%lu,"
        "\"assumeIdleWhenScaleConnects\":%s,\"shotReactTimeoutS\":%u,"
        "\"buzzerScaleLostBeep\":%s,\"buzzerAutoToManualGuardEndBeep\":%s,"
        "\"buzzerManualNoScaleBeep\":%s,\"buzzerScaleConnectedBeep\":%s,"
        "\"scaleConnectedLed\":%s,"
        "\"buzzerExtendedPulseRate\":\"%s\","
        "\"buzzerSlowExtendedPulseRate\":\"%s\","
        "\"alertOutputChannel\":\"%s\",\"rinseEnabled\":%s,\"rinseGestureMs\":%lu,"
        "\"rinseDurationMs\":%lu,\"autoRetare\":%s,\"retareWindowMs\":%lu,"
        "\"minimumCupWeightG\":%.1f,\"retareStabilitySamples\":%u,"
        "\"retareStabilityToleranceG\":%.1f,\"retareStabilityMaxGapMs\":%lu,"
        "\"retareStabilityMinDurationMs\":%lu,\"bbwProtectionMs\":%lu,"
        "\"operationalWallMs\":%lu,\"fastExtractionGuardEnabled\":%s,"
        "\"fastMarginG\":%.1f,\"fastBeforeS\":%.1f,"
        "\"slowExtractionGuardEnabled\":%s,\"slowMarginG\":%.1f,"
        "\"slowAfterS\":%.1f,\"scaleLossFixedS\":%.1f,\"doseG\":%.1f,"
        "\"grindHintsEnabled\":%s,"
        "\"autoToManualGuardEnabled\":%s,\"cupProtectionEnabled\":%s,"
        "\"stopIfCupRemoved\":%s,\"requireCupToStart\":%s,"
        "\"avoidAccidentalTouchEnabled\":%s,"
        "\"touchStopFallbackEnabled\":%s,"
        "\"cupRemovedWeightG\":%.1f,"
        "\"scaleMacCacheMode\":\"%s\","
        "\"bookooMuteOnBuzzerOnly\":%s,\"bookooConnectBeepLevel\":%u,"
        "\"noScaleBbwMode\":\"%s\","
        "\"noScaleAllowRinseWhileArmed\":%s,\"lastShotCooldownMs\":%lu",
        static_cast<double>(goalWeightDgToGrams(control.config.goalWeightDg)),
        static_cast<double>(control.config.weightOffsetG),
        static_cast<double>(control.config.weightOffsetBaselineG),
        control.config.autoTare ? "true" : "false",
        control.config.autoTareOutsideBrew ? "true" : "false",
        idleAccessoryRetareEnabled(control.config.noScaleBbwMode) ? "true" : "false",
        static_cast<unsigned long>(control.config.postTareBaselineGraceMs),
        control.config.timerOnly ? "false" : "true",
        control.config.canTareStartTimer ? "true" : "false",
        static_cast<unsigned long>(control.config.scaleTimerStopExtraDelayMs),
        static_cast<unsigned long>(control.config.dripDelayMs),
        control.config.soundAlertsMuted ? "false" : "true",
        control.config.firstDropBeep ? "true" : "false",
        control.config.paddleReturnReminderBeep ? "true" : "false",
        static_cast<unsigned long>(
            control.config.paddleReturnReminderIntervalMs),
        static_cast<unsigned long>(
            control.config.paddleReturnReminderMaxDurationMs),
        paddleModeId(control.config.paddleMode),
        static_cast<unsigned long>(runtimeStopPulseMs(control.config)),
        static_cast<unsigned long>(runtimeMaxSinglePressMs(control.config)),
        momentaryStartEdgeId(control.config.momentaryStartOnPress),
        static_cast<unsigned long>(
            runtimeReedConfirmTimeoutMs(control.config)),
        control.config.assumeIdleWhenScaleConnects ? "true" : "false",
        static_cast<unsigned>(runtimeShotReactTimeoutS(control.config)),
        control.config.buzzerScaleLostBeep ? "true" : "false",
        control.config.buzzerAutoToManualGuardEndBeep ? "true" : "false",
        control.config.buzzerManualNoScaleBeep ? "true" : "false",
        control.config.buzzerScaleConnectedBeep ? "true" : "false",
        control.config.scaleConnectedLed ? "true" : "false",
        extendedPulseRateId(control.config.buzzerExtendedPulseRate),
        extendedPulseRateId(control.config.buzzerSlowExtendedPulseRate),
        alertOutputChannelId(control.config.alertOutputChannel),
        control.config.rinseEnabled ? "true" : "false",
        static_cast<unsigned long>(control.config.rinseGestureMs),
        static_cast<unsigned long>(control.config.rinseDurationMs),
        control.config.autoRetare ? "true" : "false",
        static_cast<unsigned long>(control.config.retareWindowMs),
        static_cast<double>(control.config.minimumCupWeightG),
        static_cast<unsigned>(control.config.retareStabilitySamples),
        static_cast<double>(control.config.retareStabilityToleranceG),
        static_cast<unsigned long>(control.config.retareStabilityMaxGapMs),
        static_cast<unsigned long>(control.config.retareStabilityMinDurationMs),
        static_cast<unsigned long>(control.config.bbwProtectionMs),
        static_cast<unsigned long>(control.config.operationalWallMs),
        control.config.fastSideEnabled ? "true" : "false",
        static_cast<double>(goalWeightDgToGrams(control.config.fastMarginDg)),
        control.config.fastBeforeMs / 1000.0,
        control.config.slowSideEnabled ? "true" : "false",
        static_cast<double>(goalWeightDgToGrams(control.config.slowMarginDg)),
        control.config.slowAfterMs / 1000.0,
        control.config.scaleLossFixedMs / 1000.0,
        // Preset-only recipe fields come from the active preset.
        static_cast<double>(goalWeightDgToGrams(activeShotPreset(g_work->presetBank).doseDg)),
        activeShotPreset(g_work->presetBank).grindHintsEnabled ? "true" : "false",
        control.config.scaleLossEnabled ? "true" : "false",
        control.config.cupProtectionEnabled ? "true" : "false",
        control.config.stopIfCupRemoved ? "true" : "false",
        control.config.requireCupToStart ? "true" : "false",
        control.config.avoidAccidentalTouchEnabled ? "true" : "false",
        control.config.touchStopFallbackEnabled ? "true" : "false",
        static_cast<double>(control.config.cupRemovedWeightG),
        scaleMacCacheModeId(control.config.scaleMacCacheMode),
        control.config.bookooMuteOnBuzzerOnly ? "true" : "false",
        static_cast<unsigned>(control.config.bookooConnectBeepLevel),
        noScaleBbwModeId(control.config.noScaleBbwMode),
        noScaleAllowRinseWhileArmed(control.config.noScaleBbwMode) ? "true"
                                                                 : "false",
        static_cast<unsigned long>(control.config.lastShotCooldownMs));
  }

  if (ok && page == StatusPage::Settings) {
    ok = statusJsonAppend(
        &used, ",\"bbwAlgorithm\":\"%s\",\"bbwPresetId\":%u,"
        "\"bbwEwmaOffsetG\":%.2f,"
        "\"bbwAlpha\":%.2f,\"bbwAlphaBaseline\":%.2f,\"bbwAlphaSource\":\"%s\",\"bbwEvidenceCount\":%u",
        bbwAlgorithmName(static_cast<uint8_t>(BbwAlgorithm::LINEAR_EWMA)), control.bbwPresetId,
        static_cast<double>(control.bbwEwmaOffsetG), control.bbwAlpha / 100.0,
        control.bbwAlphaBaseline / 100.0,
        control.bbwAlphaLearned ? "learned" : "initial", control.bbwEvidenceCount);
  }
  if (ok && page == StatusPage::Settings) {
    char drip[256];
    ok = formatDripChartJson(drip, sizeof(drip), control) > 0 &&
         statusJsonAppend(&used, "%s", drip);
  }
  if (ok && page == StatusPage::Settings) {
    // Live platform weight for the cup-minimum setup guidance; presence
    // classification itself depends on the minimum, so it cannot be used.
    char cupObserved[24] = "null";
    if (control.scaleAvailable && control.currentWeightValid &&
        std::isfinite(control.currentWeightG)) {
      snprintf(cupObserved, sizeof(cupObserved), "%.1f",
               static_cast<double>(control.currentWeightG));
    }
    ok = statusJsonAppend(&used, ",\"cupSetup\":{\"observedG\":%s}",
                          cupObserved);
  }
  if (ok && page == StatusPage::Settings) {
    ok = statusJsonAppend(
        &used, ",\"bullseyeMelodyEnabled\":%s,\"bullseyeRtttl\":\"%s\"",
        g_work->bullseyeMelody.enabled ? "true" : "false",
        g_work->safeBullseyeRtttl);
  }

  if (ok) {
    ok = statusJsonAppend(&used, "}");
  }

#include "OpenBrewByWeightLineaMicraStatus.inc"

  if (ok && page == StatusPage::Settings) {
    ok = statusJsonAppend(
        &used,
        ",\"scale\":{\"preferredMac\":\"%s\",\"preferredName\":\"%s\","
        "\"preferredFriendlyName\":\"%s\","
        "\"macCachePauseRemainingMs\":%lu,\"history\":%s},"
        "\"presets\":%s,\"scaleLoss\":{\"enabled\":%s,\"projectedS\":%.1f,"
        "\"source\":%s},\"stopPulseMs\":%lu,\"maxSinglePressMs\":%lu,"
        "\"momentaryStartEdge\":\"%s\",\"reedConfirmTimeoutMs\":%lu,"
        "\"assumeIdleWhenScaleConnects\":%s,\"shotReactTimeoutS\":%u",
        safePreferredScaleMac, safePreferredScaleName,
        safePreferredScaleFriendly,
        static_cast<unsigned long>(control.scaleMacCachePauseRemainingMs),
        g_work->historyJson, g_work->presetsJson,
        control.config.scaleLossEnabled ? "true" : "false",
        control.autoToManualGuardTrendMs / 1000.0,
        shotLogScaleLossSourceJson(
            static_cast<uint8_t>(control.scaleLossProjectedSource << 3)),
        static_cast<unsigned long>(runtimeStopPulseMs(control.config)),
        static_cast<unsigned long>(runtimeMaxSinglePressMs(control.config)),
        momentaryStartEdgeId(control.config.momentaryStartOnPress),
        static_cast<unsigned long>(
            runtimeReedConfirmTimeoutMs(control.config)),
        control.config.assumeIdleWhenScaleConnects ? "true" : "false",
        static_cast<unsigned>(runtimeShotReactTimeoutS(control.config)));
  } else if (ok && page == StatusPage::Admin) {
    // Locked admin: confirm-window hint and the factory-password reminder
    // (that password is public). Unlocked: Wi-Fi/AP + BLE + OTA.
    bool factoryPassword = false;
    {
      TaskLockGuard lock(self.dataMux_);
      factoryPassword = passwordIsFactoryDefault(self.settings_);
    }
    ok = statusJsonAppend(&used,
                          ",\"adminUnlocked\":%s,\"development\":%s,\"factoryPassword\":%s",
                          adminUnlocked ? "true" : "false",
                          DEVELOPMENT_BUILD ? "true" : "false",
                          factoryPassword ? "true" : "false");
    if (ok && !adminUnlocked) {
      ok = statusJsonAppend(
          &used,
          ",\"network\":{\"configState\":\"%s\",\"confirmRemainingMs\":%lu}",
          staConfigStateName(network.staConfigState),
          static_cast<unsigned long>(network.confirmRemainingMs));
    } else if (ok) {
      // Admin page: Wi-Fi/AP status + STA address form hydration only.
      // Diagnostics metrics ride the owned UI WebSocket stream.
      ok = statusJsonAppend(
          &used,
          ",\"network\":{\"apActive\":%s,\"apSsid\":\"%s\",\"apIp\":\"%s\",\"apClients\":%u,"
          "\"wifiConfigured\":%s,\"ssid\":\"%s\",\"open\":%s,\"wifiSleep\":%s,"
          "\"staState\":\"%s\",\"channel\":%s,\"staIp\":\"%s\",\"ipMode\":\"%s\","
          "\"configState\":\"%s\",\"confirmRemainingMs\":%lu,"
          "\"setup\":\"%s\",\"setupBrew\":\"%s\",\"setupPresetId\":%u,"
          "\"apClosingMs\":%lu,"
          "\"rssi\":%s,\"signalQualityPct\":%s,"
          "\"configuredIp\":\"%s\",\"configuredNetmask\":\"%s\","
          "\"configuredGateway\":\"%s\",\"configuredDns1\":\"%s\","
          "\"configuredDns2\":\"%s\","
          "\"deviceName\":\"%s\",\"mdnsHost\":\"%s\"},"
          "\"bleScan\":{\"scanIntensity\":\"%s\",\"enabled\":%s}",
          network.apActive ? "true" : "false", safeApSsid, network.iface.apIp,
          static_cast<unsigned>(network.apClients),
          network.wifiConfigured ? "true" : "false", safeStaSsid,
          network.staOpen ? "true" : "false",
          network.staWifiSleep ? "true" : "false", staStateName(network.staState),
          staChannelJson, network.iface.staIp, staIpModeName(network.staIpMode),
          staConfigStateName(network.staConfigState),
          static_cast<unsigned long>(network.confirmRemainingMs),
          setupPhaseName(network.setupPhase), setupBrewName(network.setupBrew),
          static_cast<unsigned>(network.setupPresetId),
          static_cast<unsigned long>(apClosingMs), staRssiJson,
          staSignalQualityJson, network.iface.configuredIp, network.iface.configuredNetmask,
          network.iface.configuredGateway, network.iface.configuredDns1,
          network.iface.configuredDns2, safeDeviceName, safeMdnsHost,
          bleScanIntensityName(clampBleScanIntensity(
              control.bleScanIntensity)),
          control.bleScanEnabled ? "true" : "false");
      if (ok) ok = self.appendWebAccessStatus(request, &used);
      if (ok) {
        ok = statusJsonAppend(
            &used,
            ",\"webhooks\":{\"enabled\":%s,\"url\":\"%s\","
            "\"brewState\":%s,\"firstDrop\":%s,\"end\":%s,"
            "\"deferDuringShot\":%s,"
            "\"workerReady\":%s,\"sending\":%s,\"lastSuccess\":%s,"
            "\"lastHttpStatus\":%u,\"lastError\":%ld,"
            "\"lastEvent\":\"%s\",\"lastEndpoint\":\"%s\","
            "\"lastPhase\":\"%s\",\"lastCancellation\":\"%s\","
            "\"lastDispatched\":%s,\"lastDeliveryUnknown\":%s,"
            "\"lastAttemptAtMs\":%lu,\"sent\":%lu,\"dropped\":%lu,"
            "\"staleConfigDropped\":%lu,\"workerStarts\":%lu,"
            "\"workerStops\":%lu,\"workerStartFailures\":%lu,"
            "\"clientCreates\":%lu,\"clientReuses\":%lu,"
            "\"transportResets\":%lu,\"clientCleanups\":%lu,"
            "\"heapSamples\":%lu,\"internalFreeBefore\":%lu,"
            "\"internalFreeAfter\":%lu,\"internalLargestBefore\":%lu,"
            "\"internalLargestAfter\":%lu,\"internalLargestMinimum\":%lu,"
            "\"psramLargestBefore\":%lu,\"psramLargestAfter\":%lu},"
            "\"lastCommand\":{\"requestId\":%lu,\"state\":\"%s\"}",
            webhookConfig.enabled ? "true" : "false", safeWebhookUrl,
            webhookConfig.brewState ? "true" : "false",
            webhookConfig.firstDrop ? "true" : "false",
            webhookConfig.end ? "true" : "false",
            webhookConfig.deferDuringShot ? "true" : "false",
            webhookStatus.workerReady ? "true" : "false",
            webhookStatus.sending ? "true" : "false",
            webhookStatus.lastSuccess ? "true" : "false",
            static_cast<unsigned>(webhookStatus.lastHttpStatus),
            static_cast<long>(webhookStatus.lastError),
            webhookStatus.lastEvent, webhookStatus.lastEndpoint,
            webhookRequestPhaseName(webhookStatus.lastPhase),
            webhookCancellationReasonName(webhookStatus.lastCancellation),
            webhookStatus.lastDispatched ? "true" : "false",
            webhookStatus.lastDeliveryUnknown ? "true" : "false",
            static_cast<unsigned long>(webhookStatus.lastAttemptAtMs),
            static_cast<unsigned long>(webhookStatus.sent),
            static_cast<unsigned long>(webhookStatus.dropped),
            static_cast<unsigned long>(webhookStatus.staleConfigDropped),
            static_cast<unsigned long>(webhookStatus.workerStarts),
            static_cast<unsigned long>(webhookStatus.workerStops),
            static_cast<unsigned long>(webhookStatus.workerStartFailures),
            static_cast<unsigned long>(webhookStatus.clientCreates),
            static_cast<unsigned long>(webhookStatus.clientReuses),
            static_cast<unsigned long>(webhookStatus.transportResets),
            static_cast<unsigned long>(webhookStatus.clientCleanups),
            static_cast<unsigned long>(webhookStatus.heapSamples),
            static_cast<unsigned long>(webhookStatus.internalFreeBefore),
            static_cast<unsigned long>(webhookStatus.internalFreeAfter),
            static_cast<unsigned long>(webhookStatus.internalLargestBefore),
            static_cast<unsigned long>(webhookStatus.internalLargestAfter),
            static_cast<unsigned long>(webhookStatus.internalLargestMinimum),
            static_cast<unsigned long>(webhookStatus.psramLargestBefore),
            static_cast<unsigned long>(webhookStatus.psramLargestAfter),
            static_cast<unsigned long>(network.lastCommandRequestId),
            commandResultStateName(network.lastCommandState));
      }
      if (ok) {
        self.buildOtaJson(g_work->otaJson, NetworkWorkBuf::kOtaJson,
                          controlGateOf(control), control.bootId);
        ok = statusJsonAppend(&used, ",\"boardArch\":\"%s\",\"ota\":%s",
                              FW_BOARD_ARCH_STRING, g_work->otaJson);
      }
    }
  }

  if (ok) {
    ok = statusJsonAppend(&used, "}");
  }

  if (!ok) {
    const esp_err_t tooLarge =
        sendError(request, "500 Internal Server Error", "STATUS_TOO_LARGE",
                  "Status snapshot exceeds its size limit.");
    self.unlockWorkBuf();
    return tooLarge;
  }
  const esp_err_t sent =
      sendJson(request, STATUS_OK, g_work->statusJson);
  self.unlockWorkBuf();
  return sent;
}

namespace {

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
  // Admin and Setup only: every other view reads the owned UI stream. Admin
  // mutations and the full privileged status body require a temporary
  // device-password unlock.
  self.requestPendingNetworkConfirm();

  if (!statusPageIsAdmin(request->uri)) {
    return sendError(request, "404 Not Found", "STATUS_PAGE_UNKNOWN",
                     "Unknown status page; use /api/v1/status/admin.");
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
  const bool adminUnlocked = self.adminUnlockAllowed(request);
  if (adminUnlocked) {
    self.touchAdminUnlock();
  }
  const NetworkStatusSnapshot network = self.snapshot();
  const TimeStatusSnapshot timeStatus = g_wallClock.snapshot(millis());
  int16_t appliedTimezoneOffset = 0;
  if (timeStatus.utcSec != 0)
    timeZoneOffsetAtOrUtc(control.config.timezoneId, timeStatus.utcSec,
                          appliedTimezoneOffset);

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
  char safeFirmwareVersion[32] = {};
  char safeStaSsid[WIFI_SSID_CAPACITY] = {};
  char safeApSsid[WIFI_SSID_CAPACITY] = {};
  char safeDeviceName[DEVICE_NAME_CAPACITY] = {};
  char safeMdnsHost[DEVICE_NAME_CAPACITY] = {};
  char safeWebhookUrl[WEBHOOK_URL_CAPACITY * 2] = {};
  sanitizeJsonEmbed(control.config.ntpServerCustom, safeNtpCustom,
                    sizeof(safeNtpCustom));
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
  // The header names the connected scale from its history entry.
  if (self.callbacks_.copyScaleHistory != nullptr) {
    self.callbacks_.copyScaleHistory(g_work->scaleHistory);
  } else {
    memset(&g_work->scaleHistory, 0, sizeof(g_work->scaleHistory));
  }
  char wifiName[WIFI_SSID_CAPACITY * 6] = {};
  char bluetoothName[PREFERRED_SCALE_NAME_CAPACITY * 6] = {};
  escapeJsonString(network.staState == StaState::CONNECTED ? network.iface.staSsid : "",
                   wifiName, sizeof(wifiName));
  escapeConnectedScaleName(control, g_work->scaleHistory,
                           bluetoothName, sizeof(bluetoothName));
  size_t used = 0;
  // Fields Admin and Setup read; privileged sections follow once unlocked.
  bool ok = statusJsonAppend(
      &used,
      "{\"firmwareVersion\":\"%s\",\"bootId\":%lu,\"configMutable\":%s,"
      "\"snapshotStale\":%s,"
      "\"webUiOverrideActive\":%s,\"webUiOverrideRemainingMs\":%lu,"
      "\"configLockReason\":\"%s\"",
      safeFirmwareVersion,
      static_cast<unsigned long>(control.bootId),
      configMutable ? "true" : "false",
      control.snapshotStale ? "true" : "false",
      webUiOverrideActive ? "true" : "false",
      static_cast<unsigned long>(webUiOverrideRemainingMs),
      configLockReason(control));
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
  if (ok) {
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
    ok = statusJsonAppend(&used, ",\"diagnosticPageVisible\":%s",
                          self.diagnosticPageEnabled() ? "true" : "false");
  }
  if (ok) {
    ok = statusJsonAppend(&used, ",\"compatibilityMode\":%s",
                          firmwareCompatibilityMode() ? "true" : "false");
  }
  if (ok) {
    ok = statusJsonAppend(
        &used, ",\"timeUtcSec\":%lu,\"config\":{\"revision\":%lu,\"timezoneId\":\"%s\","
        "\"appliedTimezoneOffsetMinutes\":%d,\"timezoneAutomatic\":%s,\"timezoneInitialized\":%s",
        static_cast<unsigned long>(timeStatus.utcSec),
        static_cast<unsigned long>(control.config.revision),
        control.config.timezoneId, static_cast<int>(appliedTimezoneOffset),
        control.config.timezoneAutomatic ? "true" : "false",
        durableTimezoneSaved().load() ? "true" : "false");
  }
  if (ok && adminUnlocked) {
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

  if (ok) {
    ok = statusJsonAppend(&used, "}");
  }

  if (ok) {
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
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
      if (ok) ok = self.appendCompanionStatus(&used);
#endif
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

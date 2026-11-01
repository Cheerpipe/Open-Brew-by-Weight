esp_err_t OpenBrewByWeightNetwork::timeSyncHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  if (firmwareCompatibilityMode()) {
    return sendError(request, STATUS_UNAVAILABLE, "TIME_SYNC_DISABLED",
                     nullptr, "TIME_SYNC_DISABLED");
  }
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "CLOCK_SYNC_REQUIRES_IDLE");
  }
  self.ntpManualSyncPending_ = true;
  return self.sendAccepted(request, self.allocateRequestId());
}
esp_err_t OpenBrewByWeightNetwork::timeZoneCatalogHandler(httpd_req_t *request) {
  httpd_resp_set_type(request, JSON_CONTENT_TYPE);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  if (sendChunk(request, "[", 1) != ESP_OK) return ESP_FAIL;
  char chunk[512] = {};
  size_t used = 0;
  for (uint16_t i = 0; i < tzdb::kZoneCount; ++i) {
    const int added = snprintf(chunk + used, sizeof(chunk) - used,
                               "%s\"%s\"", i == 0 ? "" : ",",
                               tzdb::kZones[i].id);
    if (added < 0) return ESP_FAIL;
    if (static_cast<size_t>(added) >= sizeof(chunk) - used) {
      if (sendChunk(request, chunk, used) != ESP_OK) return ESP_FAIL;
      used = 0;
      const int retry = snprintf(chunk, sizeof(chunk), ",\"%s\"",
                                 tzdb::kZones[i].id);
      if (retry < 0 || static_cast<size_t>(retry) >= sizeof(chunk))
        return ESP_FAIL;
      used = static_cast<size_t>(retry);
    } else {
      used += static_cast<size_t>(added);
    }
  }
  if ((used != 0 && sendChunk(request, chunk, used) != ESP_OK) ||
      sendChunk(request, "]", 1) != ESP_OK)
    return ESP_FAIL;
  return sendChunk(request, nullptr, 0);
}

esp_err_t OpenBrewByWeightNetwork::timeZonePreviewHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) return bodyStatus;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char zoneId[TIMEZONE_ID_CAPACITY] = {};
  uint32_t browserUtcSec = 0;
  ValidationFailure error;
  static const char *const fields[] = {"timezoneId", "browserUtcSec"};
  if (root == nullptr ||
      !jsonHasOnlyUniqueFields(root, fields, 2) ||
      !jsonString(root, "timezoneId", zoneId, sizeof(zoneId), false) ||
      (jsonFieldPresent(root, "browserUtcSec") &&
       !jsonUint32(root, "browserUtcSec", browserUtcSec)) ||
      findTimeZone(zoneId) == nullptr)
    error = validationFailure("TIMEZONE_ID_CHOOSE");
  cJSON_Delete(root);
  self.unlockJsonBody();
  if (error.reason != nullptr)
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_TIMEZONE", nullptr,
                     error.reason, error.params);
  const TimeStatusSnapshot clock = g_wallClock.snapshot(millis());
  const uint32_t utcSec = clock.utcSec != 0 ? clock.utcSec : browserUtcSec;
  int16_t offset = 0;
  uint32_t nextUtc = 0;
  const bool known = utcSec != 0 &&
      timeZoneOffsetAt(zoneId, utcSec, offset, &nextUtc);
  char body[256] = {};
  snprintf(body, sizeof(body),
           "{\"timezoneId\":\"%s\",\"clockAvailable\":%s,"
           "\"clockSource\":\"%s\","
           "\"utcSec\":%lu,\"localSec\":%lu,\"offsetMinutes\":%d,"
           "\"nextTransitionUtcSec\":%lu,\"tzdbVersion\":\"%s\"}",
           zoneId, known ? "true" : "false",
           known ? (clock.utcSec != 0 ? "device" : "browser") : "unavailable",
           static_cast<unsigned long>(known ? utcSec : 0),
           static_cast<unsigned long>(known ? utcSec + offset * 60 : 0),
           static_cast<int>(known ? offset : 0),
           static_cast<unsigned long>(known ? nextUtc : 0), tzdb::kVersion);
  return sendJson(request, STATUS_OK, body);
}

esp_err_t OpenBrewByWeightNetwork::configHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "CONFIG_LOCKED_DURING_ACTIVE_CYCLE");
  }
  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  const bool dateTimePatch =
      jsonFieldPresent(root, "timezoneId") ||
      jsonFieldPresent(root, "timezoneAutoInit") ||
      jsonFieldPresent(root, "timezoneDetected") ||
      jsonFieldPresent(root, "timezoneAutomatic") ||
      jsonFieldPresent(root, "ntpSyncEnabled") ||
      jsonFieldPresent(root, "ntpServerPreset") ||
      jsonFieldPresent(root, "ntpServerCustom") ||
      jsonFieldPresent(root, "scheduledRestartEnabled") ||
      jsonFieldPresent(root, "scheduledRestartMinuteOfDay");
  // Log/debug fields stay public with the Diagnostic page; the visibility
  // toggle and the Frontend preferences always require the Admin session.
  const bool diagnosticPagePatch = jsonFieldPresent(root, "showDiagnosticPage") ||
      jsonFieldPresent(root, "powerManagementEnabled") || jsonFieldPresent(root, "heroCardMode");
  // Patch: seed live effective config; only present keys overwrite.
  RuntimeConfig candidate = {};
  if (self.callbacks_.copyRuntimeConfig != nullptr) {
    self.callbacks_.copyRuntimeConfig(&candidate);
  }
  bool baseRevisionPresent = false;
  uint32_t baseRevision = 0;
  char preferredScaleMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  const bool preferredScaleSpecified = jsonFieldPresent(root, "preferredScaleMac");
  const bool timezoneIdPresent = jsonFieldPresent(root, "timezoneId");
  bool timezoneAutoInit = false;
  bool timezoneDetected = false;
  bool timezoneAutomatic = candidate.timezoneAutomatic != 0;
  bool browserTimezonePatch = false;
  bool soundAlertsEnabled = !candidate.soundAlertsMuted;
  bool scheduledRestartEnabled = candidate.scheduledRestartEnabled != 0;
  uint32_t scheduledRestartMinute = candidate.scheduledRestartMinuteOfDay;
  BullseyeMelodyConfig candidateBullseye = {};
  if (self.callbacks_.copyBullseyeConfig != nullptr) {
    self.callbacks_.copyBullseyeConfig(&candidateBullseye);
  } else {
    self.dataMux_.lock();
    candidateBullseye = self.settings_.bullseyeMelody;
    self.dataMux_.unlock();
  }
  const bool bullseyeEnabledPresent =
      jsonFieldPresent(root, "bullseyeMelodyEnabled");
  const bool bullseyeRtttlPresent = jsonFieldPresent(root, "bullseyeRtttl");
  const bool bullseyeConfigSpecified =
      bullseyeEnabledPresent || bullseyeRtttlPresent;
  uint8_t parsedNoScaleMode = noScaleBbwModeValue(candidate.noScaleBbwMode);
  bool parsedAllowRinseWhileArmed =
      noScaleAllowRinseWhileArmed(candidate.noScaleBbwMode);
  bool parsedAccessoryRetare = idleAccessoryRetareEnabled(candidate.noScaleBbwMode);
  char customNtp[NTP_SERVER_HOST_CAPACITY] = {};
  memcpy(customNtp, candidate.ntpServerCustom, sizeof(customNtp));
  static const char *const fields[] = {
      "baseRevision", "goalWeightG", "rinseEnabled", "rinseGestureMs", "rinseDurationMs",
      "operationalWallMs", "autoTare", "autoTareOutsideBrew", "retareAccessoryOutsideBrew", "postTareBaselineGraceMs",
      "canTareStartTimer",
      "scaleTimerStopExtraDelayMs",       "dripDelayMs", "soundAlertsEnabled",
      "firstDropBeep", "scaleConnectedLed",
      "paddleReturnReminderBeep",
      "paddleReturnReminderIntervalMs", "paddleReturnReminderMaxDurationMs",
      "paddleMode", "stopPulseMs", "maxSinglePressMs", "momentaryStartEdge",
      "reedConfirmTimeoutMs", "assumeIdleWhenScaleConnects", "shotReactTimeoutS",
      "buzzerScaleLostBeep", "buzzerAutoToManualGuardEndBeep",
      "buzzerManualNoScaleBeep", "buzzerScaleConnectedBeep",
      "buzzerExtendedPulseRate", "buzzerSlowExtendedPulseRate",
      "bullseyeMelodyEnabled", "bullseyeRtttl",
      "alertOutputChannel", "autoRetare", "retareWindowMs", "minimumCupWeightG",
      "cupRemovedWeightG",
      "retareStabilitySamples", "retareStabilityToleranceG",
      "retareStabilityMaxGapMs", "retareStabilityMinDurationMs",
      "bbwProtectionMs", "fastExtractionGuardEnabled", "avoidAccidentalTouchEnabled",
      "touchStopFallbackEnabled", "cupProtectionEnabled", "stopIfCupRemoved",
      "requireCupToStart",
      "fastMarginG", "fastBeforeS", "slowExtractionGuardEnabled", "slowMarginG",
      "slowAfterS", "autoToManualGuardEnabled", "scaleLossFixedS",
      "fastRampS", "fastCurve", "fastCurveHardness", "slowRampS", "slowCurve",
      "slowCurveHardness",
      "weightOffsetBaselineG", "timezoneId", "timezoneAutoInit",
      "timezoneDetected", "timezoneAutomatic", "ntpSyncEnabled",
      "ntpServerPreset",
      "ntpServerCustom", "scheduledRestartEnabled", "scheduledRestartMinuteOfDay",
      "scaleMacCacheMode", "preferredScaleMac", "bookooMuteOnBuzzerOnly",
      "bookooConnectBeepLevel", "noScaleBbwMode",
      "noScaleAllowRinseWhileArmed", "lastShotCooldownMs",
      "serialLogLevel", "ringRetainLogLevel", "showDiagnosticPage",
      "powerManagementEnabled", "heroCardMode"};
  ValidationFailure parseError;
  size_t settingFieldCount = 0;
  if (root != nullptr) {
    for (cJSON *item = root->child; item != nullptr; item = item->next) {
      if (item->string != nullptr &&
          strcmp(item->string, "baseRevision") != 0) {
        ++settingFieldCount;
      }
    }
  }
  if (root == nullptr ||
      !jsonHasOnlyUniqueFields(root, fields,
                               sizeof(fields) / sizeof(fields[0]))) {
    parseError = jsonParseFailure(validationFailure("CONFIG_FIELDS_INVALID"));
  } else if (settingFieldCount == 0) {
    parseError = validationFailure("CONFIG_PATCH_EMPTY");
  } else if (jsonFieldPresent(root, "baseRevision") &&
             !jsonUint32(root, "baseRevision", baseRevision)) {
    parseError = validationFailure("FIELD_INTEGER", "baseRevision");
  } else if (jsonFieldPresent(root, "goalWeightG") &&
             !jsonGoalWeightDg(root, "goalWeightG", candidate.goalWeightDg)) {
    parseError = validationFailure("FIELD_RANGE_NUMBER", "goalWeightG", "10", "200");
  } else if (jsonFieldPresent(root, "rinseEnabled") &&
             !jsonBoolean(root, "rinseEnabled", candidate.rinseEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "rinseEnabled");
  } else if (jsonFieldPresent(root, "rinseGestureMs") &&
             !jsonUint32(root, "rinseGestureMs", candidate.rinseGestureMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "rinseGestureMs");
  } else if (jsonFieldPresent(root, "rinseDurationMs") &&
             !jsonUint32(root, "rinseDurationMs", candidate.rinseDurationMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "rinseDurationMs");
  } else if (jsonFieldPresent(root, "operationalWallMs") &&
             !jsonUint32(root, "operationalWallMs",
                         candidate.operationalWallMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "operationalWallMs");
  } else if (jsonFieldPresent(root, "autoTare") &&
             !jsonBoolean(root, "autoTare", candidate.autoTare)) {
    parseError = validationFailure("FIELD_BOOLEAN", "autoTare");
  } else if (jsonFieldPresent(root, "autoTareOutsideBrew") &&
             !jsonBoolean(root, "autoTareOutsideBrew", candidate.autoTareOutsideBrew)) {
    parseError = validationFailure("FIELD_BOOLEAN", "autoTareOutsideBrew");
  } else if (jsonFieldPresent(root, "retareAccessoryOutsideBrew") &&
             !jsonBoolean(root, "retareAccessoryOutsideBrew", parsedAccessoryRetare)) {
    parseError = validationFailure("FIELD_BOOLEAN", "retareAccessoryOutsideBrew");
  } else if (jsonFieldPresent(root, "postTareBaselineGraceMs") &&
             !jsonUint32(root, "postTareBaselineGraceMs",
                         candidate.postTareBaselineGraceMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "postTareBaselineGraceMs");
  } else if (jsonFieldPresent(root, "canTareStartTimer") &&
             !jsonBoolean(root, "canTareStartTimer",
                          candidate.canTareStartTimer)) {
    parseError = validationFailure("FIELD_BOOLEAN", "canTareStartTimer");
  } else if (jsonFieldPresent(root, "scaleTimerStopExtraDelayMs") &&
             !jsonUint32(root, "scaleTimerStopExtraDelayMs",
                         candidate.scaleTimerStopExtraDelayMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "scaleTimerStopExtraDelayMs");
  } else if (jsonFieldPresent(root, "dripDelayMs") &&
             !jsonUint32(root, "dripDelayMs", candidate.dripDelayMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "dripDelayMs");
  } else if (jsonFieldPresent(root, "soundAlertsEnabled") &&
             !jsonBoolean(root, "soundAlertsEnabled", soundAlertsEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "soundAlertsEnabled");
  } else if (jsonFieldPresent(root, "firstDropBeep") &&
             !jsonBoolean(root, "firstDropBeep", candidate.firstDropBeep)) {
    parseError = validationFailure("FIELD_BOOLEAN", "firstDropBeep");
  } else if (bullseyeEnabledPresent &&
             !jsonBoolean(root, "bullseyeMelodyEnabled",
                          candidateBullseye.enabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "bullseyeMelodyEnabled");
  } else if (bullseyeRtttlPresent &&
             !jsonString(root, "bullseyeRtttl", candidateBullseye.rtttl,
                         sizeof(candidateBullseye.rtttl), true)) {
    parseError = validationFailure("FIELD_STRING_MAX", "bullseyeRtttl", "500");
  } else if (bullseyeConfigSpecified &&
             !validBullseyeMelodyConfig(candidateBullseye)) {
    parseError = candidateBullseye.enabled && candidateBullseye.rtttl[0] == '\0'
                         ? validationFailure("BULLSEYE_MELODY_TUNE_REQUIRED")
                         : validationFailure("BULLSEYE_RTTTL_INVALID");
  } else if (jsonFieldPresent(root, "scaleConnectedLed") &&
             !jsonBoolean(root, "scaleConnectedLed",
                          candidate.scaleConnectedLed)) {
    parseError = validationFailure("FIELD_BOOLEAN", "scaleConnectedLed");
  } else if (jsonFieldPresent(root, "paddleReturnReminderBeep") &&
             !jsonBoolean(root, "paddleReturnReminderBeep",
                          candidate.paddleReturnReminderBeep)) {
    parseError = validationFailure("FIELD_BOOLEAN", "paddleReturnReminderBeep");
  } else if (jsonFieldPresent(root, "paddleReturnReminderIntervalMs") &&
             !jsonUint32(root, "paddleReturnReminderIntervalMs",
                         candidate.paddleReturnReminderIntervalMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "paddleReturnReminderIntervalMs");
  } else if (jsonFieldPresent(root, "paddleReturnReminderMaxDurationMs") &&
             !jsonUint32(root, "paddleReturnReminderMaxDurationMs",
                         candidate.paddleReturnReminderMaxDurationMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "paddleReturnReminderMaxDurationMs");
  } else if (jsonFieldPresent(root, "paddleMode") &&
             !jsonPaddleMode(root, "paddleMode", candidate.paddleMode)) {
    parseError = validationFailure("FIELD_ENUM", "paddleMode", "auto, natural or original");
  } else if (jsonFieldPresent(root, "stopPulseMs") &&
             !jsonStopPulseMs(root, candidate)) {
    parseError = validationFailure("FIELD_RANGE_INT", "stopPulseMs", "50", "1000");
  } else if (jsonFieldPresent(root, "maxSinglePressMs") &&
             !jsonMaxSinglePressMs(root, candidate)) {
    parseError = validationFailure("FIELD_RANGE_INT", "maxSinglePressMs", "100", "5000");
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE != 0
  } else if (jsonFieldPresent(root, "momentaryStartEdge") &&
             !jsonMomentaryStartEdge(root, candidate)) {
    parseError = validationFailure("FIELD_ENUM", "momentaryStartEdge", "press or release");
#endif
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
  } else if (jsonFieldPresent(root, "reedConfirmTimeoutMs") &&
             !jsonReedConfirmTimeoutMs(root, candidate)) {
    parseError = validationFailure("FIELD_RANGE_INT", "reedConfirmTimeoutMs", "200", "5000");
#endif
  } else if (jsonFieldPresent(root, "assumeIdleWhenScaleConnects") &&
             !jsonAssumeIdleWhenScaleConnects(root, candidate)) {
    parseError = validationFailure("FIELD_BOOLEAN", "assumeIdleWhenScaleConnects");
  } else if (jsonFieldPresent(root, "shotReactTimeoutS") &&
             !jsonShotReactTimeoutS(root, candidate)) {
    parseError = validationFailure("SHOT_REACT_TIMEOUT_FIELD_INVALID");
  } else if (jsonFieldPresent(root, "buzzerScaleLostBeep") &&
             !jsonBoolean(root, "buzzerScaleLostBeep",
                          candidate.buzzerScaleLostBeep)) {
    parseError = validationFailure("FIELD_BOOLEAN", "buzzerScaleLostBeep");
  } else if (jsonFieldPresent(root, "buzzerAutoToManualGuardEndBeep") &&
             !jsonBoolean(root, "buzzerAutoToManualGuardEndBeep",
                          candidate.buzzerAutoToManualGuardEndBeep)) {
    parseError = validationFailure("FIELD_BOOLEAN", "buzzerAutoToManualGuardEndBeep");
  } else if (jsonFieldPresent(root, "buzzerManualNoScaleBeep") &&
             !jsonBoolean(root, "buzzerManualNoScaleBeep",
                          candidate.buzzerManualNoScaleBeep)) {
    parseError = validationFailure("FIELD_BOOLEAN", "buzzerManualNoScaleBeep");
  } else if (jsonFieldPresent(root, "buzzerScaleConnectedBeep") &&
             !jsonBoolean(root, "buzzerScaleConnectedBeep",
                          candidate.buzzerScaleConnectedBeep)) {
    parseError = validationFailure("FIELD_BOOLEAN", "buzzerScaleConnectedBeep");
  } else if (jsonFieldPresent(root, "buzzerExtendedPulseRate") &&
             !jsonExtendedPulseRate(root, "buzzerExtendedPulseRate",
                                    candidate.buzzerExtendedPulseRate)) {
    parseError = validationFailure("FIELD_ENUM", "buzzerExtendedPulseRate",
                                   "disabled, slow, medium, fast, or rapid");
  } else if (jsonFieldPresent(root, "buzzerSlowExtendedPulseRate") &&
             !jsonExtendedPulseRate(root, "buzzerSlowExtendedPulseRate",
                                    candidate.buzzerSlowExtendedPulseRate)) {
    parseError = validationFailure("FIELD_ENUM", "buzzerSlowExtendedPulseRate",
                                   "disabled, slow, medium, fast, or rapid");
  } else if (jsonFieldPresent(root, "alertOutputChannel") &&
             !jsonAlertOutputChannel(root, "alertOutputChannel",
                                     candidate.alertOutputChannel,
                                     /*optional=*/false)) {
    parseError = validationFailure("FIELD_ENUM", "alertOutputChannel",
                                   "scale_only, buzzer_only, or scale_priority");
  } else if (jsonFieldPresent(root, "autoRetare") &&
             !jsonBoolean(root, "autoRetare", candidate.autoRetare)) {
    parseError = validationFailure("FIELD_BOOLEAN", "autoRetare");
  } else if (jsonFieldPresent(root, "retareWindowMs") &&
             !jsonUint32(root, "retareWindowMs", candidate.retareWindowMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "retareWindowMs");
  } else if (jsonFieldPresent(root, "minimumCupWeightG") &&
             !jsonFloat(root, "minimumCupWeightG",
                        candidate.minimumCupWeightG)) {
    parseError = validationFailure("FIELD_NUMBER", "minimumCupWeightG");
  } else if (jsonFieldPresent(root, "cupRemovedWeightG") &&
             !jsonFloat(root, "cupRemovedWeightG",
                        candidate.cupRemovedWeightG)) {
    parseError = validationFailure("FIELD_NUMBER", "cupRemovedWeightG");
  } else if (jsonFieldPresent(root, "retareStabilitySamples") &&
             !jsonUint8(root, "retareStabilitySamples",
                        candidate.retareStabilitySamples)) {
    parseError = validationFailure("FIELD_RANGE_INT", "retareStabilitySamples", "2", "10");
  } else if (jsonFieldPresent(root, "retareStabilityToleranceG") &&
             !jsonFloat(root, "retareStabilityToleranceG",
                        candidate.retareStabilityToleranceG)) {
    parseError = validationFailure("FIELD_NUMBER", "retareStabilityToleranceG");
  } else if (jsonFieldPresent(root, "retareStabilityMaxGapMs") &&
             !jsonUint32(root, "retareStabilityMaxGapMs",
                         candidate.retareStabilityMaxGapMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "retareStabilityMaxGapMs");
  } else if (jsonFieldPresent(root, "retareStabilityMinDurationMs") &&
             !jsonUint32(root, "retareStabilityMinDurationMs",
                         candidate.retareStabilityMinDurationMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "retareStabilityMinDurationMs");
  } else if (jsonFieldPresent(root, "bbwProtectionMs") &&
             !jsonUint32(root, "bbwProtectionMs", candidate.bbwProtectionMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "bbwProtectionMs");
  } else if (jsonFieldPresent(root, "fastExtractionGuardEnabled") &&
             !jsonBoolean(root, "fastExtractionGuardEnabled",
                          candidate.fastSideEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "fastExtractionGuardEnabled");
  } else if (jsonFieldPresent(root, "avoidAccidentalTouchEnabled") &&
             !jsonBoolean(root, "avoidAccidentalTouchEnabled",
                          candidate.avoidAccidentalTouchEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "avoidAccidentalTouchEnabled");
  } else if (jsonFieldPresent(root, "touchStopFallbackEnabled") &&
             !jsonBoolean(root, "touchStopFallbackEnabled",
                          candidate.touchStopFallbackEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "touchStopFallbackEnabled");
  } else if (jsonFieldPresent(root, "cupProtectionEnabled") &&
             !jsonBoolean(root, "cupProtectionEnabled",
                          candidate.cupProtectionEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "cupProtectionEnabled");
  } else if (jsonFieldPresent(root, "stopIfCupRemoved") &&
             !jsonBoolean(root, "stopIfCupRemoved", candidate.stopIfCupRemoved)) {
    parseError = validationFailure("FIELD_BOOLEAN", "stopIfCupRemoved");
  } else if (jsonFieldPresent(root, "requireCupToStart") &&
             !jsonBoolean(root, "requireCupToStart", candidate.requireCupToStart)) {
    parseError = validationFailure("FIELD_BOOLEAN", "requireCupToStart");
  } else if (jsonFieldPresent(root, "fastBeforeS") &&
             !jsonSecondsMs(root, "fastBeforeS", candidate.fastBeforeMs)) {
    parseError = validationFailure("FIELD_SECONDS", "fastBeforeS");
  } else if (jsonFieldPresent(root, "fastMarginG") &&
             !jsonGramsDg(root, "fastMarginG", candidate.fastMarginDg)) {
    parseError = validationFailure("FIELD_GRAMS", "fastMarginG");
  } else if (jsonFieldPresent(root, "slowExtractionGuardEnabled") &&
             !jsonBoolean(root, "slowExtractionGuardEnabled",
                          candidate.slowSideEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "slowExtractionGuardEnabled");
  } else if (jsonFieldPresent(root, "slowAfterS") &&
             !jsonSecondsMs(root, "slowAfterS", candidate.slowAfterMs)) {
    parseError = validationFailure("FIELD_SECONDS", "slowAfterS");
  } else if (jsonFieldPresent(root, "slowMarginG") &&
             !jsonGramsDg(root, "slowMarginG", candidate.slowMarginDg)) {
    parseError = validationFailure("FIELD_GRAMS", "slowMarginG");
  } else if (jsonFieldPresent(root, "fastRampS") &&
             !jsonRampMs(root, "fastRampS", candidate.fastRampMs)) {
    parseError = validationFailure("FIELD_SECONDS", "fastRampS");
  } else if (jsonFieldPresent(root, "fastCurve") &&
             !jsonRampChoice(root, "fastCurve", rampCurveName, RAMP_CURVE_TYPES,
                             candidate.fastCurve)) {
    parseError = validationFailure("FIELD_ENUM", "fastCurve", "early, even or late");
  } else if (jsonFieldPresent(root, "fastCurveHardness") &&
             !jsonRampHardness(root, "fastCurveHardness", candidate.fastCurveHardness,
                               candidate.fastCurveHardnessPct)) {
    parseError = validationFailure("FIELD_RANGE_NUMBER", "fastCurveHardness", "0", "100");
  } else if (jsonFieldPresent(root, "slowRampS") &&
             !jsonRampMs(root, "slowRampS", candidate.slowRampMs)) {
    parseError = validationFailure("FIELD_SECONDS", "slowRampS");
  } else if (jsonFieldPresent(root, "slowCurve") &&
             !jsonRampChoice(root, "slowCurve", rampCurveName, RAMP_CURVE_TYPES,
                             candidate.slowCurve)) {
    parseError = validationFailure("FIELD_ENUM", "slowCurve", "early, even or late");
  } else if (jsonFieldPresent(root, "slowCurveHardness") &&
             !jsonRampHardness(root, "slowCurveHardness", candidate.slowCurveHardness,
                               candidate.slowCurveHardnessPct)) {
    parseError = validationFailure("FIELD_RANGE_NUMBER", "slowCurveHardness", "0", "100");
  } else if (jsonFieldPresent(root, "autoToManualGuardEnabled") &&
             !jsonBoolean(root, "autoToManualGuardEnabled",
                          candidate.scaleLossEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "autoToManualGuardEnabled");
  } else if (jsonFieldPresent(root, "scaleLossFixedS") &&
             !jsonSecondsMs(root, "scaleLossFixedS", candidate.scaleLossFixedMs)) {
    parseError = validationFailure("FIELD_SECONDS", "scaleLossFixedS");
  } else if (jsonFieldPresent(root, "weightOffsetBaselineG") &&
             !jsonFloat(root, "weightOffsetBaselineG",
                        candidate.weightOffsetBaselineG)) {
    parseError = validationFailure("FIELD_NUMBER", "weightOffsetBaselineG");
  } else if (timezoneIdPresent &&
             (!jsonString(root, "timezoneId", candidate.timezoneId,
                          sizeof(candidate.timezoneId), false) ||
              findTimeZone(candidate.timezoneId) == nullptr)) {
    parseError = validationFailure("TIMEZONE_ID_INVALID");
  } else if (jsonFieldPresent(root, "timezoneAutoInit") &&
             !jsonBoolean(root, "timezoneAutoInit", timezoneAutoInit)) {
    parseError = validationFailure("FIELD_BOOLEAN", "timezoneAutoInit");
  } else if (jsonFieldPresent(root, "timezoneDetected") &&
             !jsonBoolean(root, "timezoneDetected", timezoneDetected)) {
    parseError = validationFailure("FIELD_BOOLEAN", "timezoneDetected");
  } else if (jsonFieldPresent(root, "timezoneAutomatic") &&
             !jsonBoolean(root, "timezoneAutomatic", timezoneAutomatic)) {
    parseError = validationFailure("FIELD_BOOLEAN", "timezoneAutomatic");
  } else if (jsonFieldPresent(root, "ntpSyncEnabled") &&
             !jsonBoolean(root, "ntpSyncEnabled", candidate.ntpSyncEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "ntpSyncEnabled");
  } else if (jsonFieldPresent(root, "scheduledRestartEnabled") &&
             !jsonBoolean(root, "scheduledRestartEnabled",
                          scheduledRestartEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "scheduledRestartEnabled");
  } else if (jsonFieldPresent(root, "scheduledRestartMinuteOfDay") &&
             (!jsonUint32(root, "scheduledRestartMinuteOfDay",
                          scheduledRestartMinute) ||
              scheduledRestartMinute >= 24 * 60)) {
    parseError = validationFailure("FIELD_RANGE_INT", "scheduledRestartMinuteOfDay", "0", "1439");
  } else if (jsonFieldPresent(root, "ntpServerPreset") &&
             !jsonNtpPreset(root, "ntpServerPreset",
                            candidate.ntpServerPreset)) {
    parseError = validationFailure("FIELD_ENUM", "ntpServerPreset",
                                   "pool, google, cloudflare, or nist");
  } else if (jsonFieldPresent(root, "ntpServerCustom") &&
             !jsonString(root, "ntpServerCustom", customNtp, sizeof(customNtp),
                         true)) {
    parseError = validationFailure("FIELD_STRING_MAX", "ntpServerCustom", "63");
  } else if (jsonFieldPresent(root, "scaleMacCacheMode") &&
             !jsonScaleMacCacheMode(root, "scaleMacCacheMode",
                                    candidate.scaleMacCacheMode)) {
    parseError = validationFailure("FIELD_ENUM", "scaleMacCacheMode", "first, prefer, or only");
  } else if (preferredScaleSpecified &&
             (!jsonString(root, "preferredScaleMac", preferredScaleMac,
                          sizeof(preferredScaleMac), true) ||
              (preferredScaleMac[0] != '\0' &&
               !validPreferredScaleMac(preferredScaleMac)))) {
    parseError = validationFailure("PREFERRED_SCALE_MAC_INVALID");
  } else if (jsonFieldPresent(root, "bookooMuteOnBuzzerOnly") &&
             !jsonBoolean(root, "bookooMuteOnBuzzerOnly",
                          candidate.bookooMuteOnBuzzerOnly)) {
    parseError = validationFailure("FIELD_BOOLEAN", "bookooMuteOnBuzzerOnly");
  } else if (jsonFieldPresent(root, "bookooConnectBeepLevel") &&
             (!jsonUint8(root, "bookooConnectBeepLevel",
                         candidate.bookooConnectBeepLevel) ||
              candidate.bookooConnectBeepLevel > BOOKOO_BEEP_LEVEL_MAX)) {
    parseError = validationFailure("FIELD_RANGE_INT", "bookooConnectBeepLevel", "0", "5");
  } else if (jsonFieldPresent(root, "noScaleBbwMode") &&
             !jsonNoScaleBbwMode(root, "noScaleBbwMode", parsedNoScaleMode)) {
    parseError = validationFailure("FIELD_ENUM", "noScaleBbwMode",
                                   "off, warn_once, or require_scale");
  } else if (jsonFieldPresent(root, "noScaleAllowRinseWhileArmed") &&
             !jsonBoolean(root, "noScaleAllowRinseWhileArmed",
                          parsedAllowRinseWhileArmed)) {
    parseError = validationFailure("FIELD_BOOLEAN", "noScaleAllowRinseWhileArmed");
  } else if (jsonFieldPresent(root, "lastShotCooldownMs") &&
             !jsonUint32(root, "lastShotCooldownMs",
                         candidate.lastShotCooldownMs)) {
    parseError = validationFailure("FIELD_INTEGER_MS", "lastShotCooldownMs");
  } else if (jsonFieldPresent(root, "serialLogLevel") &&
             !jsonLogLevel(root, "serialLogLevel", candidate.serialLogLevel)) {
    parseError = validationFailure("FIELD_ENUM", "serialLogLevel",
                                   "none, critical, error, warning, info, or debug");
  } else if (jsonFieldPresent(root, "showDiagnosticPage") &&
             !jsonBoolean(root, "showDiagnosticPage", candidate.showDiagnosticPage)) {
    parseError = validationFailure("FIELD_BOOLEAN", "showDiagnosticPage");
  } else if (jsonFieldPresent(root, "heroCardMode") &&
             !jsonHeroCardMode(root, "heroCardMode", candidate.heroCardMode)) {
    parseError = validationFailure("FIELD_ENUM", "heroCardMode", "technical or colorful");
  } else if (jsonFieldPresent(root, "ringRetainLogLevel") &&
             !jsonLogLevel(root, "ringRetainLogLevel",
                           candidate.ringRetainLogLevel)) {
    parseError = validationFailure("FIELD_ENUM", "ringRetainLogLevel",
                                   "none, critical, error, warning, info, or debug");
  }
  if (parseError.reason == nullptr && jsonFieldPresent(root, "powerManagementEnabled") &&
      !jsonBoolean(root, "powerManagementEnabled", candidate.powerManagementEnabled)) {
    parseError = validationFailure("FIELD_BOOLEAN", "powerManagementEnabled");
  }
  if (parseError.reason == nullptr &&
      ((timezoneAutoInit || timezoneDetected) && !timezoneIdPresent)) {
    parseError = validationFailure("ZONE_DETECTION_REQUIRES_TIMEZONE_ID");
  }
  if (root != nullptr) {
    static const char *const browserFields[] = {
        "baseRevision", "timezoneId", "timezoneAutoInit", "timezoneDetected"};
    browserTimezonePatch = timezoneIdPresent &&
        ((timezoneAutoInit && !durableTimezoneSaved().load()) ||
         (timezoneDetected && candidate.timezoneAutomatic)) &&
        jsonHasOnlyUniqueFields(root, browserFields, 4);
    baseRevisionPresent = jsonFieldPresent(root, "baseRevision");
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (parseError.reason != nullptr) {
    memset(customNtp, 0, sizeof(customNtp));
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD", nullptr,
                   parseError.reason, parseError.params);
  }
  if (dateTimePatch && !browserTimezonePatch && !self.requireAdminUnlock(request)) {
    memset(customNtp, 0, sizeof(customNtp));
    return ESP_OK;
  }
  // Compatibility mode keeps date/time settings frozen: the browser-driven
  // zone patch must not persist while every clock feature is stopped.
  if (dateTimePatch && firmwareCompatibilityMode()) {
    memset(customNtp, 0, sizeof(customNtp));
    return sendError(request, STATUS_UNAVAILABLE, "DATE_TIME_DISABLED",
                     nullptr, "DATE_TIME_DISABLED");
  }
  if (diagnosticPagePatch && !self.requireAdminUnlock(request)) {
    memset(customNtp, 0, sizeof(customNtp));
    return ESP_OK;
  }
  if (baseRevisionPresent && baseRevision != candidate.revision) {
    memset(customNtp, 0, sizeof(customNtp));
    return sendError(request, STATUS_CONFLICT, "CONFIG_REVISION_STALE",
                     nullptr, "CONFIG_REVISION_STALE");
  }
  if (timezoneAutoInit && durableTimezoneSaved().load()) {
    return sendError(request, STATUS_CONFLICT, "TIMEZONE_ALREADY_SET",
                     nullptr, "TIMEZONE_ALREADY_SET");
  }
  candidate.timezoneAutomatic = timezoneAutomatic;
  candidate.scheduledRestartEnabled = scheduledRestartEnabled ? 1 : 0;
  candidate.scheduledRestartMinuteOfDay =
      static_cast<uint16_t>(scheduledRestartMinute);
  if (timezoneIdPresent) {
    candidate.timezoneSource = timezoneAutoInit ? 3 :
        timezoneDetected ? (browserTimezonePatch ? 4 : 1) : 2;
  }
  candidate.noScaleBbwMode =
      packNoScaleBbwMode(parsedNoScaleMode, parsedAllowRinseWhileArmed) |
      (parsedAccessoryRetare ? IDLE_ACCESSORY_RETARE : 0U);
  candidate.soundAlertsMuted = !soundAlertsEnabled;
  memcpy(candidate.ntpServerCustom, customNtp, sizeof(candidate.ntpServerCustom));
  memset(customNtp, 0, sizeof(customNtp));
  // ControlGateSnapshot is gate-only (no recipe bank). Validate the patched
  // machine config against the live preset overlay, same as APPLY_CONFIG.
  ShotPresetBank livePresets = {};
  if (self.callbacks_.copyPresetBank != nullptr) {
    self.callbacks_.copyPresetBank(&livePresets);
  } else {
    self.dataMux_.lock();
    livePresets = self.settings_.presets;
    self.dataMux_.unlock();
  }
  const RuntimeConfig effective =
      composeEffectiveConfig(candidate, livePresets);
  const ConfigValidationError error = validateRuntimeConfig(effective);
  if (error != ConfigValidationError::NONE) {
    self.log(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED,
             static_cast<int32_t>(error));
    char errorBody[320] = {};
    configValidationErrorBody(error, errorBody, sizeof(errorBody));
    return sendJson(request, STATUS_UNPROCESSABLE, errorBody);
  }
  WebCommand command;
  command.type = WebCommandType::APPLY_CONFIG;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  command.config = candidate;
  command.preferredScaleSpecified = preferredScaleSpecified;
  copyCString(command.preferredScaleMac, sizeof(command.preferredScaleMac),
              preferredScaleMac);
  command.bullseyeConfigSpecified = bullseyeConfigSpecified;
  if (bullseyeConfigSpecified) {
    if (self.callbacks_.stageBullseyeConfig == nullptr ||
        !self.callbacks_.stageBullseyeConfig(candidateBullseye,
                                              command.requestId)) {
      return sendError(request, STATUS_UNAVAILABLE, "CONTROL_BUSY",
                       nullptr, "BULLSEYE_STAGE_BUSY");
    }
    command.bullseyeStageRequestId = command.requestId;
  }
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "SAVE_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::webhookHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const ControlGateSnapshot gate = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, gate)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "WEBHOOK_REQUIRES_READY");
  }
  if (!self.requireAdminUnlock(request)) return ESP_OK;
  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "WEBHOOK_BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) return bodyStatus;

  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {
      "action", "enabled", "url", "brewState", "firstDrop", "end",
      "deferDuringShot", "baseRevision"};
  char action[8] = {};
  WebhookConfig candidate = self.webhooks_.config();
  bool stale = false;
  const bool known = root != nullptr &&
      jsonHasOnlyUniqueFields(root, fields, sizeof(fields) / sizeof(fields[0]));
  const bool actionOk = known &&
      jsonString(root, "action", action, sizeof(action), false);
  bool parsed = actionOk;
  if (parsed && strcmp(action, "save") == 0) {
    // A form filled before another screen saved is refused (Admin's revision).
    stale = !baseRevisionCurrent(root, webhookRevision(candidate));
    parsed = jsonBoolean(root, "enabled", candidate.enabled) &&
             jsonString(root, "url", candidate.url, sizeof(candidate.url), true) &&
             jsonBoolean(root, "brewState", candidate.brewState) &&
             jsonBoolean(root, "firstDrop", candidate.firstDrop) &&
             jsonBoolean(root, "end", candidate.end) &&
             jsonBoolean(root, "deferDuringShot", candidate.deferDuringShot) &&
             validWebhookConfig(candidate);
  } else if (parsed && strcmp(action, "test") == 0) {
    parsed = validWebhookUrl(candidate.url);
  } else {
    parsed = false;
  }
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_WEBHOOK",
                     nullptr, "INVALID_WEBHOOK");
  }
  if (stale)
    return sendError(request, STATUS_CONFLICT, "STALE_REVISION",
                     nullptr, "WEBHOOK_SETTINGS_CHANGED_RETRY");

  if (strcmp(action, "test") == 0) {
    WebhookEvent event;
    event.type = WebhookEventType::TEST;
    event.uptimeMs = millis();
    event.unixSec = g_wallClock.synced() ? g_wallClock.nowUtcSec(millis()) : 0;
    if (!self.webhooks_.enqueue(event)) {
      return sendError(request, STATUS_UNAVAILABLE, "WEBHOOK_UNAVAILABLE",
                       nullptr, "WEBHOOK_UNAVAILABLE");
    }
    return sendJson(request, STATUS_ACCEPTED, "{\"queued\":true}");
  }

  WebCommand command;
  command.type = WebCommandType::SAVE_WEBHOOK;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  command.stagedConfigRequestId = command.requestId;
  self.dataMux_.lock();
  const bool stageAvailable = self.stagedWebhookRequestId_ == 0;
  if (stageAvailable) {
    self.stagedWebhook_ = candidate;
    self.stagedWebhookRequestId_ = command.requestId;
  }
  self.dataMux_.unlock();
  if (!stageAvailable) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_BUSY",
                     nullptr, "WEBHOOK_CONFIG_SAVE_PENDING");
  }
  if (!self.callbacks_.enqueueWebCommand(command)) {
    self.clearStagedWebhook(command.requestId);
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "WEBHOOK_SAVE_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::bullseyeTestHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "BULLSEYE_TEST_REQUIRES_READY");
  }
  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "BULLSEYE_TEST_REQUEST_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  // This handler owns NetworkWorkBuf until unlockJsonBody(). Keep the 502-byte
  // candidate in its PSRAM scratch instead of consuming HTTP task stack.
  BullseyeMelodyConfig &testConfig = self.workBuf_->bullseyeMelody;
  testConfig = BullseyeMelodyConfig{};
  static const char *const fields[] = {"bullseyeRtttl"};
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, fields, 1) &&
                      jsonString(root, "bullseyeRtttl", testConfig.rtttl,
                                 sizeof(testConfig.rtttl), true) &&
                      validBullseyeRtttl(testConfig.rtttl);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  if (!parsed) {
    self.unlockJsonBody();
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_BULLSEYE_RTTTL",
                     nullptr, "BULLSEYE_RTTTL_INVALID");
  }

  WebCommand command;
  command.type = WebCommandType::BUZZER_TEST;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  command.bullseyeConfigSpecified = true;
  command.bullseyeStageRequestId = command.requestId;
  const bool staged = self.callbacks_.stageBullseyeConfig != nullptr &&
                      self.callbacks_.stageBullseyeConfig(testConfig,
                                                          command.requestId);
  self.unlockJsonBody();
  if (!staged) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_BUSY",
                     nullptr, "BULLSEYE_STAGE_BUSY");
  }
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "BULLSEYE_TEST_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::preferredScaleClearHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "SCALE_FORGET_LOCKED_ACTIVE_CYCLE");
  }
  return self.enqueueAcceptedCommand(request,
                                WebCommandType::CLEAR_PREFERRED_SCALE,
                                self.webUiOverrideAllowed(request),
                                nullptr, nullptr, "PREFERRED_SCALE_CLEAR_BUSY");
}

esp_err_t OpenBrewByWeightNetwork::preferredScaleSelectHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "PREFERRED_SCALE_LOCKED_ACTIVE_CYCLE");
  }

  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char mac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char name[PREFERRED_SCALE_NAME_CAPACITY] = {};
  ValidationFailure parseError;
  if (root == nullptr) {
    parseError = jsonParseFailure(validationFailure("SCALE_JSON_BODY_REQUIRED"));
  } else if (!jsonString(root, "mac", mac, sizeof(mac), true)) {
    parseError = validationFailure("PREFERRED_SCALE_MAC_STRING");
  } else if (mac[0] != '\0' && !validPreferredScaleMac(mac)) {
    parseError = validationFailure("SCALE_MAC_EMPTY_OR_BLE");
  } else {
    cJSON *nameItem = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (nameItem != nullptr) {
      if (!jsonString(root, "name", name, sizeof(name), true) ||
          !validPreferredScaleName(name)) {
        parseError = validationFailure("PREFERRED_SCALE_NAME_INVALID");
      }
    }
  }
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (parseError.reason != nullptr) {
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", nullptr,
                 parseError.reason, parseError.params);
  }

  WebCommand command;
  command.setNetworkType(WebCommandType::SELECT_PREFERRED_SCALE);
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  copyCString(command.network.scaleSelectMac, sizeof(command.network.scaleSelectMac), mac);
  copyCString(command.network.scaleSelectName, sizeof(command.network.scaleSelectName), name);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "SCALE_SELECT_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::scaleFriendlyNameHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "SCALE_NAME_LOCKED_ACTIVE_CYCLE");
  }

  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char mac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char name[PREFERRED_SCALE_NAME_CAPACITY] = {};
  ValidationFailure parseError;
  if (root == nullptr) {
    parseError = jsonParseFailure(validationFailure("SCALE_JSON_BODY_REQUIRED"));
  } else if (!jsonString(root, "mac", mac, sizeof(mac), true)) {
    parseError = validationFailure("SCALE_MAC_STRING_REQUIRED");
  } else if (!validPreferredScaleMac(mac) || mac[0] == '\0') {
    parseError = validationFailure("SCALE_MAC_INVALID");
  } else {
    cJSON *nameItem = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (nameItem != nullptr &&
        (!jsonString(root, "name", name, sizeof(name), true) ||
         !validScaleFriendlyName(name))) {
      parseError = validationFailure("SCALE_NAME_RANGE");
    }
  }
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  if (parseError.reason != nullptr) {
    self.unlockJsonBody();
    return sendError(request, STATUS_BAD_REQUEST, "INVALID_REQUEST", nullptr,
                 parseError.reason, parseError.params);
  }

  if (self.callbacks_.copyScaleHistory != nullptr) {
    self.callbacks_.copyScaleHistory(self.workBuf_->scaleHistory);
    char known[PREFERRED_SCALE_NAME_CAPACITY] = {};
    if (!findScaleHistoryName(self.workBuf_->scaleHistory, mac, known,
                              sizeof(known))) {
      self.unlockJsonBody();
      return sendError(request, STATUS_UNPROCESSABLE, "UNKNOWN_SCALE",
                       nullptr, "UNKNOWN_SCALE");
    }
  }
  self.unlockJsonBody();

  WebCommand command;
  command.setNetworkType(WebCommandType::SET_SCALE_FRIENDLY_NAME);
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  copyCString(command.network.scaleSelectMac, sizeof(command.network.scaleSelectMac), mac);
  copyCString(command.network.scaleSelectName, sizeof(command.network.scaleSelectName), name);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "SCALE_NAME_SAVE_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::presetsHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "PRESETS_LOCKED_ACTIVE_CYCLE");
  }

  const esp_err_t bodyStatus = self.lockJsonBody(
      request, nullptr, "BOUNDED_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char action[24] = {};
  uint8_t presetId = 0;
  char presetName[SHOT_PRESET_NAME_CAPACITY] = {};
  ValidationFailure parseError;
  PresetAction presetAction = PresetAction::APPLY;
  WebCommand command;
  command.type = WebCommandType::PRESET_OP;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  if (self.callbacks_.copyRuntimeConfig != nullptr) {
    self.callbacks_.copyRuntimeConfig(&command.config);
  }

  if (root == nullptr || !jsonString(root, "action", action, sizeof(action),
                                     false)) {
    parseError = validationFailure("FIELD_REQUIRED", "action");
  } else if (strcmp(action, "apply") == 0 || strcmp(action, "load") == 0) {
    presetAction = PresetAction::APPLY;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    }
  } else if (strcmp(action, "create") == 0 || strcmp(action, "new") == 0) {
    presetAction = PresetAction::CREATE;
  } else if (strcmp(action, "delete") == 0) {
    presetAction = PresetAction::DELETE;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    }
  } else if (strcmp(action, "duplicate") == 0) {
    presetAction = PresetAction::DUPLICATE;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    }
  } else if (strcmp(action, "rename") == 0) {
    presetAction = PresetAction::RENAME;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    } else if (!jsonString(root, "name", presetName, sizeof(presetName),
                           false)) {
      parseError = validationFailure("FIELD_REQUIRED", "name");
    }
  } else if (strcmp(action, "restore_factory_values") == 0) {
    presetAction = PresetAction::RESTORE_FACTORY_VALUES;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    }
  } else if (strcmp(action, "new_coffee") == 0) {
    presetAction = PresetAction::NEW_COFFEE;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    }
  } else if (strcmp(action, "save") == 0 || strcmp(action, "update") == 0) {
    presetAction = PresetAction::SAVE;
    if (!jsonUint8(root, "id", presetId) || presetId == 0) {
      parseError = validationFailure("FIELD_NONZERO_INTEGER", "id");
    } else {
      // A save is a patch: absent fields keep the stored preset value. Keys follow PresetRecipeField.
      static const char *const recipeKeys[] = {
          "brewByWeight", "goalWeightG", "operationalWallMs", "bbwProtectionMs",
          "fastExtractionGuardEnabled", "fastMarginG", "fastBeforeS",
          "slowExtractionGuardEnabled", "slowMarginG", "slowAfterS", "scaleLossFixedS",
          "fastRampS", "fastCurve", "fastCurveHardness", "slowRampS", "slowCurve",
          "slowCurveHardness", "weightOffsetBaselineG", "doseG", "grindHintsEnabled",
          "bbwAlphaBaseline", "lineaMicraBrewTargetC"};
      uint32_t present = 0;
      for (size_t i = 0; i < sizeof(recipeKeys) / sizeof(recipeKeys[0]); ++i)
        if (jsonFieldPresent(root, recipeKeys[i])) present |= 1UL << i;
      command.presetRecipeFields = present & PRESET_RECIPE_ALL;
      const auto absent = [&](const char *key) { return !jsonFieldPresent(root, key); };
      bool brewByWeight = true, grindHints = true;
      RuntimeConfig &c = command.config;
      if (present == 0) {
        parseError = validationFailure("PRESET_SAVE_FIELDS_REQUIRED");
      } else if (!((absent("brewByWeight") || jsonBoolean(root, "brewByWeight", brewByWeight)) &&
                   (absent("goalWeightG") || jsonGoalWeightDg(root, "goalWeightG", c.goalWeightDg)) &&
                   (absent("operationalWallMs") || jsonUint32(root, "operationalWallMs", c.operationalWallMs)) &&
                   (absent("bbwProtectionMs") || jsonUint32(root, "bbwProtectionMs", c.bbwProtectionMs)) &&
                   (absent("weightOffsetBaselineG") ||
                    jsonFloat(root, "weightOffsetBaselineG", c.weightOffsetBaselineG)) &&
                   (absent("fastExtractionGuardEnabled") ||
                    jsonBoolean(root, "fastExtractionGuardEnabled", c.fastSideEnabled)) &&
                   (absent("fastBeforeS") || jsonSecondsMs(root, "fastBeforeS", c.fastBeforeMs)) &&
                   (absent("fastMarginG") || jsonGramsDg(root, "fastMarginG", c.fastMarginDg)) &&
                   (absent("slowExtractionGuardEnabled") ||
                    jsonBoolean(root, "slowExtractionGuardEnabled", c.slowSideEnabled)) &&
                   (absent("slowAfterS") || jsonSecondsMs(root, "slowAfterS", c.slowAfterMs)) &&
                   (absent("slowMarginG") || jsonGramsDg(root, "slowMarginG", c.slowMarginDg)) &&
                   (absent("scaleLossFixedS") ||
                    jsonSecondsMs(root, "scaleLossFixedS", c.scaleLossFixedMs)) &&
                   (absent("fastRampS") || jsonRampMs(root, "fastRampS", c.fastRampMs)) &&
                   (absent("fastCurve") ||
                    jsonRampChoice(root, "fastCurve", rampCurveName, RAMP_CURVE_TYPES,
                                   c.fastCurve)) &&
                   (absent("fastCurveHardness") ||
                    jsonRampHardness(root, "fastCurveHardness", c.fastCurveHardness,
                                     c.fastCurveHardnessPct)) &&
                   (absent("slowRampS") || jsonRampMs(root, "slowRampS", c.slowRampMs)) &&
                   (absent("slowCurve") ||
                    jsonRampChoice(root, "slowCurve", rampCurveName, RAMP_CURVE_TYPES,
                                   c.slowCurve)) &&
                   (absent("slowCurveHardness") ||
                    jsonRampHardness(root, "slowCurveHardness", c.slowCurveHardness,
                                     c.slowCurveHardnessPct)) &&
                   (absent("doseG") || jsonGramsDg(root, "doseG", command.presetDoseDg)) &&
                   (absent("grindHintsEnabled") || jsonBoolean(root, "grindHintsEnabled", grindHints)))) {
        parseError = validationFailure("PRESET_SAVE_FIELD_INVALID");
      } else {
        if (!absent("brewByWeight")) c.timerOnly = !brewByWeight;
        if (jsonFieldPresent(root, "grindHintsEnabled"))
          command.presetGrindHints = grindHints ? 1 : 0;
        command.bbwBaselineSpecified = jsonFieldPresent(root, "weightOffsetBaselineG");
        const cJSON *alphaBase = cJSON_GetObjectItemCaseSensitive(root, "bbwAlphaBaseline");
        if (alphaBase && (!cJSON_IsNumber(alphaBase) ||
            !parseBbwAlphaBaseline(alphaBase->valuedouble, command.bbwAlphaBaseline)))
          parseError = validationFailure("BBW_ALPHA_BASELINE_INVALID");
        uint8_t algorithm = 0;
        char algorithmName[20] = {};
        if (jsonFieldPresent(root, "bbwAlgorithm") &&
            (!jsonString(root, "bbwAlgorithm", algorithmName, sizeof(algorithmName), false) ||
             !parseBbwAlgorithm(algorithmName, algorithm)))
          parseError = validationFailure("BBW_ALGORITHM_INVALID");
        if (jsonFieldPresent(root, "bbwAlpha") ||
            jsonFieldPresent(root, "bbwEwmaAlpha") ||
            jsonFieldPresent(root, "bbwAlphaLearned") ||
            jsonFieldPresent(root, "bbwEwmaOffsetG"))
          parseError = validationFailure("BBW_LEARNING_READ_ONLY");
        const cJSON *micraTarget =
            cJSON_GetObjectItemCaseSensitive(root, "lineaMicraBrewTargetC");
        if (parseError.reason == nullptr && micraTarget != nullptr) {
#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
          const double scaled = cJSON_IsNumber(micraTarget)
                                    ? micraTarget->valuedouble * 10.0
                                    : NAN;
          const long rounded = lround(scaled);
          if (!std::isfinite(scaled) || fabs(scaled - rounded) > 0.0001 ||
              rounded < LINEA_MICRA_BREW_TARGET_MIN_DECI_C ||
              rounded > LINEA_MICRA_BREW_TARGET_MAX_DECI_C) {
            parseError = validationFailure("LINEA_MICRA_PRESET_TARGET_RANGE");
          } else {
            command.lineaMicraBrewTargetSpecified = true;
            command.lineaMicraBrewTargetDeciC =
                static_cast<uint16_t>(rounded);
          }
#else
          parseError = validationFailure("LINEA_MICRA_PRESET_TARGET_UNSUPPORTED");
#endif
        }
      }
    }
  } else {
    parseError = validationFailure("PRESET_ACTION_UNKNOWN");
  }

  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (parseError.reason != nullptr) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD", nullptr,
                   parseError.reason, parseError.params);
  }

  command.presetAction = static_cast<uint8_t>(presetAction);
  command.presetId = presetId;
  copyCString(command.presetName, sizeof(command.presetName), presetName);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "SAVE_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::resetCalibrationHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "CALIBRATION_LOCKED_ACTIVE_CYCLE");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "EMPTY_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const resetFields[] = {"fullEwma", "baseRevision", "dripG", "alpha"};
  bool fullEwma = false;
  uint32_t baseRevision = 0;
  uint8_t alpha = 0;
  const bool revisionSpecified = root && jsonFieldPresent(root, "baseRevision");
  const cJSON *drip = root ? cJSON_GetObjectItemCaseSensitive(root, "dripG") : nullptr;
  const cJSON *speed = root ? cJSON_GetObjectItemCaseSensitive(root, "alpha") : nullptr;
  const double dripG = drip != nullptr && cJSON_IsNumber(drip) ? drip->valuedouble : -1.0;
  const bool parsed = root != nullptr &&
      jsonHasOnlyUniqueFields(root, resetFields, 4) &&
      (!revisionSpecified || jsonUint32(root, "baseRevision", baseRevision)) &&
      (!jsonFieldPresent(root, "fullEwma") || jsonBoolean(root, "fullEwma", fullEwma)) &&
      (drip == nullptr || (dripG >= 0.0 && dripG <= MAX_OFFSET_G)) &&
      (speed == nullptr ||
       (cJSON_IsNumber(speed) && parseBbwAlphaBaseline(speed->valuedouble, alpha)));
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST",
                     nullptr, "STOP_PREDICTION_FIELDS_INVALID");
  }
  WebCommand command;
  command.type = WebCommandType::RESET_WEIGHT_OFFSET;
  command.bbwFullReset = fullEwma;
  command.bbwSetDrip = dripG >= 0.0;
  command.config.weightOffsetBaselineG = static_cast<float>(dripG);
  command.bbwAlphaBaseline = alpha;
  command.bbwResetRevisionSpecified = revisionSpecified;
  command.config.revision = baseRevision;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "CALIBRATION_RESET_BUSY");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::resetGuardSamplesHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "GUARD_SAMPLES_LOCKED_ACTIVE_CYCLE");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "EMPTY_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const noFields[] = {nullptr};
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, noFields, 0);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST",
                     nullptr, "GUARD_SAMPLES_RESET_REQUIRES_EMPTY_OBJECT");
  }
  return self.enqueueAcceptedCommand(
      request, WebCommandType::RESET_AUTO_TO_MANUAL_GUARD_SAMPLES,
      self.webUiOverrideAllowed(request),
      nullptr, nullptr, "GUARD_SAMPLES_RESET_BUSY");
}

esp_err_t OpenBrewByWeightNetwork::paddleHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "JSON_BODY_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  bool on = false;
  static const char *const fields[] = {"on"};
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, fields, 1) &&
                      jsonBoolean(root, "on", on);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD",
                     nullptr, "FIELD_ON_MUST_BE_BOOLEAN");
  }
  ControlGateSnapshot status = self.controlGate();
  if (on && !REMOTE_MACHINE_CONTROL_ENABLED) {
    return sendError(request, "403 Forbidden", "REMOTE_CONTROL_DISABLED",
                     nullptr, "REMOTE_CONTROL_DISABLED");
  }
  const bool allowed = on ? self.webUiConfigurationAllowed(request, status)
                          : (status.activeCycle &&
                             status.source == ControlSource::WEB);
  if (!allowed) {
    return sendError(request, STATUS_CONFLICT, "CONTROL_STATE_CONFLICT",
                     nullptr, "CONTROL_STATE_CONFLICT");
  }
  WebCommand command;
  command.type = on ? WebCommandType::REMOTE_ON : WebCommandType::REMOTE_OFF;
  command.requestId = self.allocateRequestId();
  command.unsafeWebUiOverride = self.webUiOverrideAllowed(request);
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "CONTROL_QUEUE_FULL");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::rinseHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  ControlGateSnapshot status = self.controlGate();
  if (!REMOTE_MACHINE_CONTROL_ENABLED) {
    return sendError(request, "403 Forbidden", "REMOTE_CONTROL_DISABLED",
                     nullptr, "REMOTE_CONTROL_DISABLED");
  }
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT, "CONTROL_STATE_CONFLICT",
                     nullptr, "RINSE_REQUIRES_READY");
  }
  return self.enqueueAcceptedCommand(request, WebCommandType::RINSE,
                                self.webUiOverrideAllowed(request),
                                nullptr, nullptr, "CONTROL_QUEUE_FULL");
}

esp_err_t OpenBrewByWeightNetwork::stopHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  ControlGateSnapshot status = self.controlGate();
  if (!status.activeCycle || !status.machineRunning) {
    return sendError(request, STATUS_CONFLICT, "CONTROL_STATE_CONFLICT",
                     nullptr, "NO_ACTIVE_SHOT");
  }
  return self.enqueueAcceptedCommand(request, WebCommandType::STOP, false,
                                nullptr, nullptr, "CONTROL_QUEUE_FULL");
}

esp_err_t OpenBrewByWeightNetwork::forcePulseHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  if (!REMOTE_MACHINE_CONTROL_ENABLED) {
    return sendError(request, "403 Forbidden", "REMOTE_CONTROL_DISABLED",
                     nullptr, "REMOTE_CONTROL_DISABLED");
  }
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0
  if (OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0) {
    return sendError(request, STATUS_CONFLICT, "MACHINE_TYPE_UNSUPPORTED",
                     nullptr, "MACHINE_TYPE_UNSUPPORTED");
  }
#endif
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "EMPTY_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const noFields[] = {nullptr};
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, noFields, 0);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST",
                     nullptr, "FORCED_PULSE_REQUIRES_EMPTY_OBJECT");
  }
  return self.enqueueAcceptedCommand(request, WebCommandType::FORCE_SWITCH_PULSE,
                                false, nullptr, nullptr, "CONTROL_QUEUE_FULL");
}

esp_err_t OpenBrewByWeightNetwork::stateOverrideHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (OPEN_BREW_BY_WEIGHT_MACHINE_TYPE != 1) {
    return sendError(request, STATUS_CONFLICT, "MACHINE_TYPE_UNSUPPORTED",
                     nullptr, "STATE_OVERRIDE_SWITCH_ONLY");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "JSON_BODY_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  char state[8] = {};
  static const char *const fields[] = {"state"};
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, fields, 1) &&
                      jsonString(root, "state", state, sizeof(state), false);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed || (strcmp(state, "off") != 0 && strcmp(state, "on") != 0)) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIELD",
                     nullptr, "STATE_FIELD_INVALID");
  }
  WebCommand command;
  command.type = strcmp(state, "off") == 0 ? WebCommandType::STATE_OVERRIDE_OFF
                                           : WebCommandType::STATE_OVERRIDE_ON;
  command.requestId = self.allocateRequestId();
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "CONTROL_QUEUE_FULL");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::restartHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  // Shot always wins. Queue the restart even mid-cycle; the control task
  // holds it until idle instead of opening K1 to make way for the reset.
  // Override must not cut a pour.
  return self.enqueueAcceptedCommand(request, WebCommandType::RESTART, false,
                                nullptr, nullptr, "CONTROL_QUEUE_FULL");
}

esp_err_t OpenBrewByWeightNetwork::firmwareModeHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  if (!self.networkFlashWriteAllowed()) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "FIRMWARE_MODE_REQUIRES_IDLE");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "OTA_JSON_REQUEST_REQUIRED");
  if (bodyStatus != ESP_OK) {
    return bodyStatus;
  }
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const fields[] = {"enabled"};
  bool enabled = false;
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, fields, 1) &&
                      jsonBoolean(root, "enabled", enabled);
  if (root != nullptr) {
    cJSON_Delete(root);
  }
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_FIRMWARE_MODE",
                     nullptr, "FIELD_ENABLED_MUST_BE_BOOLEAN");
  }
  // The mode is boot-scoped: persist under the durable-write gate checked
  // above, then queue the same idle-safe restart the Restart button uses.
  if (!openbrewbyweight::saveFirmwareMode(
          enabled ? openbrewbyweight::FirmwareMode::FULL
                  : openbrewbyweight::FirmwareMode::COMPATIBILITY)) {
    return sendError(request, STATUS_UNAVAILABLE, "FIRMWARE_MODE_SAVE_FAILED",
                     nullptr, "FIRMWARE_MODE_SAVE_FAILED");
  }
  return self.enqueueAcceptedCommand(request, WebCommandType::RESTART, false,
                                nullptr, nullptr, "CONTROL_QUEUE_FULL");
}

esp_err_t OpenBrewByWeightNetwork::scaleTareHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!controlAllowsConfiguration(self.controlGate())) {
    return sendError(request, STATUS_CONFLICT, "CONTROL_STATE_CONFLICT",
                     nullptr, "TARE_REQUIRES_READY");
  }
  const esp_err_t bodyStatus =
      self.lockJsonBody(request, nullptr, "EMPTY_JSON_REQUIRED");
  if (bodyStatus != ESP_OK) return bodyStatus;
  cJSON *root = parseJsonDocument(self.workBuf_->requestBody);
  static const char *const noFields[] = {nullptr};
  const bool parsed = root != nullptr &&
                      jsonHasOnlyUniqueFields(root, noFields, 0);
  if (root != nullptr) cJSON_Delete(root);
  self.unlockJsonBody();
  if (!parsed) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_REQUEST",
                     nullptr, "TARE_REQUIRES_EMPTY_OBJECT");
  }
  WebCommand command;
  command.type = WebCommandType::BOOKOO_DEBUG;
  command.bookooDebugAction = BookooDebugAction::TARE;
  command.requestId = self.allocateRequestId();
  if (!self.callbacks_.enqueueWebCommand(command)) {
    return sendError(request, STATUS_UNAVAILABLE, "CONTROL_QUEUE_FULL",
                     nullptr, "CONTROL_QUEUE_FULL");
  }
  return self.sendAccepted(request, command.requestId);
}

esp_err_t OpenBrewByWeightNetwork::clearResetHistoryHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.lockConfirmedBody(
          request, nullptr,
          "CLEAR_RESET_HISTORY", "RESET_HISTORY_CONFIRMATION_REQUIRED")) {
    return sendError(request, STATUS_UNPROCESSABLE, "INVALID_CONFIRMATION",
                     nullptr, "RESET_HISTORY_NOT_CONFIRMED");
  }
  return self.enqueueAcceptedCommand(request, WebCommandType::CLEAR_RESET_HISTORY,
                                false,
                                nullptr, nullptr, "RESET_HISTORY_CLEAR_BUSY");
}

esp_err_t OpenBrewByWeightNetwork::factoryResetHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  if (!self.requireAdminUnlock(request)) {
    return ESP_OK;
  }
  ControlGateSnapshot status = self.controlGate();
  if (!self.webUiConfigurationAllowed(request, status)) {
    return sendError(request, STATUS_CONFLICT,
                     "CONFIG_LOCKED_DURING_ACTIVE_CYCLE",
                     nullptr, "FACTORY_RESET_REQUIRES_IDLE");
  }

  if (!self.lockConfirmedBody(
          request, nullptr,
          "ERASE_ALL_SETTINGS", "FACTORY_RESET_CONFIRMATION_REQUIRED")) {
    return sendError(request, STATUS_UNPROCESSABLE,
                     "FACTORY_RESET_NOT_CONFIRMED",
                     nullptr, "FACTORY_RESET_NOT_CONFIRMED");
  }

  return self.enqueueAcceptedCommand(request, WebCommandType::FACTORY_RESET,
                                self.webUiOverrideAllowed(request),
                                nullptr,
                                "\"state\":\"QUEUED\"", "FACTORY_RESET_BUSY");
}

// Onboarding brewing choice (the setup "brew" action), accepted only while the
// firmware is off: brewByWeight, plus presetId exactly when it is true.
static ValidationFailure parseSetupBrew(cJSON *root, SetupBrew &brew,
                                     uint8_t &presetId) {
  brew = SetupBrew::NONE;
  const bool hasPreset = jsonFieldPresent(root, "presetId");
  bool byWeight = false;
  if (!firmwareCompatibilityMode()) {
    return validationFailure("SETUP_BREW_SETUP_ONLY");
  }
  if (!jsonBoolean(root, "brewByWeight", byWeight) || byWeight != hasPreset ||
      (hasPreset && (!jsonUint8(root, "presetId", presetId) || presetId == 0))) {
    return validationFailure("SETUP_BREW_FIELDS_INVALID");
  }
  brew = byWeight ? SetupBrew::BY_WEIGHT : SetupBrew::USUAL;
  return ValidationFailure{};
}

// Saves the onboarding brewing choice on its own, unbound to any Wi-Fi: boot
// applies "by weight" at the next start, which Finish or the setup network
// closing triggers; "usual" keeps compatibility mode and drops an earlier one.
bool OpenBrewByWeightNetwork::saveSetupBrew(SetupBrew brew, uint8_t presetId) {
  SetupIntent intent;
  intent.presetId = presetId;
  if (!networkFlashWriteAllowed() ||
      !(brew == SetupBrew::USUAL ? clearSetupIntent() : saveSetupIntent(intent))) {
    return false;
  }
  dataMux_.lock();
  status_.setupBrew = brew;
  status_.setupPresetId = presetId;
  dataMux_.unlock();
  setupRestartPending_.store(brew == SetupBrew::BY_WEIGHT, std::memory_order_release);
  return true;
}

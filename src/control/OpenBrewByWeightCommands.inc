void rejectWebCommand(const WebCommand &command) {
  addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_REJECTED,
                static_cast<int32_t>(command.type));
  reportControlCommandResult(command, CommandResultState::FAILED);
}
void persistBleScanIntensity(BleScanIntensity intensity);
void persistBleScanEnabled(bool enabled);

// Compatibility mode blocks commands that actuate the machine or execute
// disabled functionality (brew intelligence, scale, alerts). Persistence,
// network, restart, OTA, and factory-reset commands keep flowing so the
// reduced surface stays manageable.
bool webCommandBlockedInCompatibilityMode(WebCommandType type) {
  switch (type) {
    case WebCommandType::STOP:
    case WebCommandType::STOP_HEARTBEAT:
    case WebCommandType::FORCE_SWITCH_PULSE:
    case WebCommandType::STATE_OVERRIDE_OFF:
    case WebCommandType::STATE_OVERRIDE_ON:
    case WebCommandType::BLE_SCAN_INTENSITY:
    case WebCommandType::RESET_WEIGHT_OFFSET:
    case WebCommandType::RESET_AUTO_TO_MANUAL_GUARD_SAMPLES:
    case WebCommandType::BUZZER_TEST:
    case WebCommandType::BOOKOO_DEBUG:
      return true;
    default:
      // Remote web start/off and web rinse belong to the same actuation
      // family; compared here so the remote-control guards in the main
      // switch below stay the first `case WebCommandType::REMOTE_*` text.
      return type == WebCommandType::REMOTE_ON ||
             type == WebCommandType::REMOTE_OFF ||
             type == WebCommandType::RINSE;
  }
}

// A preset's statistics epoch: the next shot-log record id (plan §4.8).
uint32_t shotStatisticsEpochNow() {
  TaskLockGuard lock(shotStoreMutex);
  return shotLog.nextRecordId();
}

// Quick starts the preset from its current drip; a drip the owner set marks
// the shot history from the next record on.
void armBbwQuickStart(const ShotPreset &preset, bool userSet) {
  armQuickStart(bbwQuickStart, presetBank, preset.id, preset.bbwEwmaOffsetG,
                userSet ? shotStatisticsEpochNow() : 0);
  stageBbwQuickStart();
}

// Commits a session-only Quick Settings change: live and announced, never saved.
void commitSessionQuickSettings(RuntimeConfig candidate) {
  candidate.revision = runtimeConfig.revision + 1;
  if (candidate.revision == 0) candidate.revision = 1;
  commitLiveRuntimeConfig(candidate, 0);
  queueWebhookQuickSettingsChanged();
}

// Drops every session override and restores the saved values.
void revertSessionQuickSettings() {
  if (sessionQuickSettings.mask == 0) return;
  const RuntimeConfig saved = overlaySessionQuickSettings(sessionQuickSettings, runtimeConfig, true);
  sessionQuickSettings.mask = 0;
  commitSessionQuickSettings(saved);
}

// Runs the Duration revert here so it happens with no Web UI open. A due
// revert waits until no cycle is active and the relay is open.
void serviceQuickSettingsRevert() {
  if (!session.active && !relayCircuitClosed() && quickRevertDue(sessionQuickSettings, millis()))
    revertSessionQuickSettings();
}

void processWebCommand(const WebCommand &command) {
  if (firmwareCompatibilityMode() &&
      webCommandBlockedInCompatibilityMode(command.type)) {
    rejectWebCommand(command);
    return;
  }
  switch (command.type) {
    case WebCommandType::STOP:
    case WebCommandType::STOP_HEARTBEAT:
    case WebCommandType::REMOTE_OFF:
      if (!machineIsRunning() ||
          (command.type == WebCommandType::REMOTE_OFF &&
           (!session.active || session.source != ControlSource::WEB))) {
        rejectWebCommand(command);
        return;
      }
      machineNoteFirmwareStop();
      addDebugEvent(DebugCategory::WEB,
                    command.type == WebCommandType::REMOTE_OFF
                        ? DebugCode::WEB_REMOTE_OFF
                        : DebugCode::WEB_STOP,
                    static_cast<int32_t>(command.type),
                    static_cast<int32_t>(session.id));
      if (!session.active) {
        const bool applied = machineRequestWebStop();
        reportControlCommandResult(
            command, applied ? CommandResultState::APPLIED
                             : CommandResultState::FAILED);
        return;
      }
      {
        const bool applied = finalizeCycle(
          command.type == WebCommandType::STOP_HEARTBEAT
              ? EndReason::WEB_HEARTBEAT_TIMEOUT
              : EndReason::WEB_STOP,
          nextStateForUserHold(machineLastIntention()), true);
        reportControlCommandResult(
            command, applied ? CommandResultState::APPLIED
                             : CommandResultState::FAILED);
      }
      return;

    case WebCommandType::FORCE_SWITCH_PULSE:
      if (!REMOTE_MACHINE_CONTROL_ENABLED || OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 0) {
        rejectWebCommand(command);
        return;
      }
      if (machineRequestForcedPulse()) {
        addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_ACCEPTED,
                      static_cast<int32_t>(command.type));
        reportControlCommandResult(command, CommandResultState::APPLIED);
      } else {
        rejectWebCommand(command);
      }
      return;

    case WebCommandType::STATE_OVERRIDE_OFF:
      machineOverrideInferredOff();
      if (session.active) {
        finalizeCycle(EndReason::WEB_STOP,
                      nextStateForUserHold(machineLastIntention()));
      }
      addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_ACCEPTED,
                    static_cast<int32_t>(command.type));
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::STATE_OVERRIDE_ON:
      if (!machineOverrideInferredOn()) {
        rejectWebCommand(command);
        return;
      }
      addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_ACCEPTED,
                    static_cast<int32_t>(command.type));
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::REMOTE_ON:
      if (!REMOTE_MACHINE_CONTROL_ENABLED || !webCommandAllowsUnsafeConfiguration(command)) {
        rejectWebCommand(command);
        return;
      }
      virtualHoldOn = true;
      beginCycle(ControlSource::WEB);
      if (!session.active) {
        virtualHoldOn = false;
        reportControlCommandResult(command, CommandResultState::FAILED);
        return;
      }
      addDebugEvent(DebugCategory::WEB, DebugCode::WEB_REMOTE_ON,
                    static_cast<int32_t>(command.type));
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::RINSE:
      if (!machineSupportsRinse() || !REMOTE_MACHINE_CONTROL_ENABLED ||
          !webCommandAllowsUnsafeConfiguration(command)) {
        rejectWebCommand(command);
        return;
      }
      beginWebRinse();
      if (!session.active) {
        reportControlCommandResult(command, CommandResultState::FAILED);
        return;
      }
      addDebugEvent(DebugCategory::WEB, DebugCode::WEB_RINSE,
                    static_cast<int32_t>(command.type));
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::APPLY_CONFIG: {
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      if (command.config.revision != runtimeConfig.revision ||
          command.config.timezoneAutomatic > 1 ||
          (command.preferredScaleSpecified &&
           !validPreferredScaleMac(command.preferredScaleMac)) ||
          (command.config.timezoneSource == 3 &&
           durableTimezoneSaved().load()) ||
          (command.config.timezoneSource == 4 &&
           !runtimeConfig.timezoneAutomatic) ||
          strnlen(command.config.timezoneId, TIMEZONE_ID_CAPACITY) >=
              TIMEZONE_ID_CAPACITY ||
          (strncmp(command.config.timezoneId, runtimeConfig.timezoneId,
                   TIMEZONE_ID_CAPACITY) != 0 &&
           findTimeZone(command.config.timezoneId) == nullptr)) {
        rejectWebCommand(command);
        return;
      }
      RecipeSnapshot quickBefore;
      quickBefore.presets = presetBank;
      quickBefore.runtime = runtimeConfig;
      BullseyeMelodyConfig stagedBullseye = {};
      if (command.bullseyeConfigSpecified &&
          (!takeStagedBullseyeConfig(command.bullseyeStageRequestId,
                                     stagedBullseye) ||
           !validBullseyeMelodyConfig(stagedBullseye))) {
        rejectWebCommand(command);
        return;
      }
      // Machine/workflow fields only. Recipe authority stays on the active preset.
      RuntimeConfig candidate = runtimeConfig;
      candidate.autoTare = command.config.autoTare;
      candidate.autoTareOutsideBrew = command.config.autoTareOutsideBrew;
      candidate.postTareBaselineGraceMs = command.config.postTareBaselineGraceMs;
      candidate.canTareStartTimer = command.config.canTareStartTimer;
      candidate.scaleTimerStopExtraDelayMs =
          command.config.scaleTimerStopExtraDelayMs;
      candidate.dripDelayMs = command.config.dripDelayMs;
      candidate.soundAlertsMuted = command.config.soundAlertsMuted;
      candidate.firstDropBeep = command.config.firstDropBeep;
      copyDeviceProtectionFromConfig(command.config, candidate);
      machineApplyWorkflowConfig(candidate, command.config);
      candidate.rinseDurationMs = command.config.rinseDurationMs;
      candidate.rinseEnabled = command.config.rinseEnabled;
      candidate.buzzerScaleLostBeep = command.config.buzzerScaleLostBeep;
      candidate.buzzerAutoToManualGuardEndBeep =
          command.config.buzzerAutoToManualGuardEndBeep;
      candidate.buzzerManualNoScaleBeep = command.config.buzzerManualNoScaleBeep;
      candidate.buzzerScaleConnectedBeep =
          command.config.buzzerScaleConnectedBeep;
      candidate.scaleConnectedLed = command.config.scaleConnectedLed;
      candidate.buzzerExtendedPulseRate = command.config.buzzerExtendedPulseRate;
      candidate.buzzerSlowExtendedPulseRate =
          command.config.buzzerSlowExtendedPulseRate;
      candidate.alertOutputChannel = command.config.alertOutputChannel;
      candidate.bookooMuteOnBuzzerOnly = command.config.bookooMuteOnBuzzerOnly;
      candidate.bookooConnectBeepLevel = command.config.bookooConnectBeepLevel;
      candidate.autoRetare = command.config.autoRetare;
      candidate.retareWindowMs = command.config.retareWindowMs;
      candidate.minimumCupWeightG = command.config.minimumCupWeightG;
      candidate.cupRemovedWeightG = command.config.cupRemovedWeightG;
      candidate.retareStabilitySamples = command.config.retareStabilitySamples;
      candidate.retareStabilityToleranceG =
          command.config.retareStabilityToleranceG;
      candidate.retareStabilityMaxGapMs = command.config.retareStabilityMaxGapMs;
      candidate.retareStabilityMinDurationMs =
          command.config.retareStabilityMinDurationMs;
      memcpy(candidate.timezoneId, command.config.timezoneId,
             sizeof(candidate.timezoneId));
      candidate.timezoneAutomatic = command.config.timezoneAutomatic;
      candidate.timezoneSource = command.config.timezoneSource;
      if (candidate.timezoneSource == 3 &&
          candidate.firstTimezoneAutoClockQuality == 0) {
        candidate.firstTimezoneAutoClockQuality = 1;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
        candidate.firstTimezoneAutoUptimeUs =
            static_cast<uint64_t>(esp_timer_get_time());
#else
        candidate.firstTimezoneAutoUptimeUs =
            static_cast<uint64_t>(millis()) * 1000U;
#endif
        candidate.firstTimezoneAutoBootId = currentBootId();
        candidate.firstTimezoneAutoUtcSec = g_wallClock.nowUtcSec(millis());
        if (candidate.firstTimezoneAutoUtcSec != 0) {
          candidate.firstTimezoneAutoClockQuality = 2;
        }
      }
      if (candidate.timezoneSource >= 3) candidate.timezoneSource = 1;
      candidate.ntpSyncEnabled = command.config.ntpSyncEnabled;
      candidate.ntpServerPreset = command.config.ntpServerPreset;
      memcpy(candidate.ntpServerCustom, command.config.ntpServerCustom,
             sizeof(candidate.ntpServerCustom));
      candidate.scheduledRestartEnabled = command.config.scheduledRestartEnabled;
      candidate.scheduledRestartMinuteOfDay =
          command.config.scheduledRestartMinuteOfDay;
      if (candidate.scaleMacCacheMode != command.config.scaleMacCacheMode) {
        serialTraceCategoryf(LogLevel::INFO, DebugCategory::SCALE,
                             "Scale preference: %s",
                             scaleMacCacheModeId(command.config.scaleMacCacheMode));
      }
      candidate.scaleMacCacheMode = command.config.scaleMacCacheMode;
      candidate.noScaleBbwMode = command.config.noScaleBbwMode;
      candidate.lastShotCooldownMs = command.config.lastShotCooldownMs;
      candidate.serialLogLevel = command.config.serialLogLevel;
      candidate.ringRetainLogLevel = command.config.ringRetainLogLevel;
      candidate.showDiagnosticPage = command.config.showDiagnosticPage;
      candidate.powerManagementEnabled = command.config.powerManagementEnabled;
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
      // Host APPLY_CONFIG still carries brew fields; update the active recipe.
      {
        ensureShotPresetBank(presetBank, candidate.retareWindowMs,
                             candidate.autoRetare);
        ShotPreset &preset = mutableActiveShotPreset(presetBank);
        copyUserRecipeFromConfig(command.config, preset);
      }
#endif
      RuntimeConfig composed = composeEffectiveConfig(candidate, presetBank);
      if (validateRuntimeConfig(composed) != ConfigValidationError::NONE) {
        addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED);
        rejectWebCommand(command);
        return;
      }
      composed.revision = runtimeConfig.revision + 1;
      if (composed.revision == 0) {
        composed.revision = 1;
      }
      ensureShotPresetBank(presetBank, composed.retareWindowMs,
                           composed.autoRetare);
      if (candidate.dripDelayMs != runtimeConfig.dripDelayMs ||
          candidate.autoTare != runtimeConfig.autoTare ||
          candidate.autoRetare != runtimeConfig.autoRetare ||
          candidate.retareWindowMs != runtimeConfig.retareWindowMs ||
          candidate.postTareBaselineGraceMs != runtimeConfig.postTareBaselineGraceMs) {
        for (uint8_t i = 0; i < presetBank.count; ++i)
          bbwLearningBank.invalidate(presetBank.presets[i].id, presetBank);
      }
      commitLiveRuntimeConfig(composed, RUNTIME_PERSIST_REASON_USER, &command.config);
      if (command.preferredScaleSpecified) {
        selectPreferredScale(command.preferredScaleMac, nullptr);
        servicePreferredScaleMacPersistence();
      }
      RecipeSnapshot quickAfter;
      quickAfter.presets = presetBank;
      quickAfter.runtime = runtimeConfig;
      quickSettingsPersistPending |= !quickSettingsEqual(
          quickSettingsSnapshot(quickBefore), quickSettingsSnapshot(quickAfter));
      if (command.bullseyeConfigSpecified) {
        commitLiveBullseyeConfig(stagedBullseye);
      }
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }

    case WebCommandType::QUICK_SETTING: {
      // Home Quick Settings are session-only overrides: applied live, never
      // written to flash or the preset (see SessionQuickSettings).
      if (pendingPresetPersistence.requestId != 0 ||
          !persistentConfigurationMutationAllowed(command) ||
          command.config.revision != runtimeConfig.revision) {
        rejectWebCommand(command);
        return;
      }
      const uint8_t action = command.presetAction;
      if (action == QUICK_REVERT_NOW_ACTION) {
        revertSessionQuickSettings();
      } else if (action == QUICK_REVERT_POLICY_ACTION &&
                 command.presetId < QUICK_REVERT_POLICY_COUNT) {
        sessionQuickSettings.revertPolicy = static_cast<QuickRevertPolicy>(command.presetId);
        sessionQuickSettings.changedAtMs = millis();
        sessionQuickSettings.shotEnded = false;
      } else if (action == 0 || action >= (1U << QUICK_SETTING_FIELD_COUNT)) {
        rejectWebCommand(command);
        return;
      } else {
        // `action` is the patch's field mask and command.config holds its
        // values: the patch goes live as one change (one revision, webhook and
        // revert-timer restart) or, if invalid, not at all.
        RuntimeConfig candidate = runtimeConfig;
        SessionQuickSettings session = sessionQuickSettings;
        bool changed = false;
        for (uint8_t index = 0; index < QUICK_SETTING_FIELD_COUNT; ++index) {
          const auto field = static_cast<QuickSettingField>(index);
          const uint8_t value = quickSettingValue(command.config, field);
          if ((action & (1U << index)) == 0 || quickSettingValue(candidate, field) == value)
            continue;
          setSessionQuickSetting(session, presetBank.activeId, candidate, field, value);
          changed = true;
        }
        if (changed && validateRuntimeConfig(candidate) != ConfigValidationError::NONE) {
          rejectWebCommand(command);
          return;
        }
        if (changed) {
          session.changedAtMs = millis();
          session.shotEnded = false;
          sessionQuickSettings = session;
          commitSessionQuickSettings(candidate);
        }
      }
      // Nothing to save: the change is complete once it is live.
      reportControlCommandResult(command, CommandResultState::PERSISTED);
      return;
    }

    case WebCommandType::RESET_WEIGHT_OFFSET: {
      if (!persistentConfigurationMutationAllowed(command) ||
          (command.bbwResetRevisionSpecified &&
           command.config.revision != runtimeConfig.revision)) {
        rejectWebCommand(command);
        return;
      }
      // Set drip and the plain reset (re)start the drip and quick start from
      // it; Set learning speed alone keeps the drip.
      ShotPreset &preset = mutableActiveShotPreset(presetBank);
      bbwLearningBank.invalidate(preset.id, presetBank);
      if (command.bbwSetDrip)
        preset.weightOffsetBaselineG = command.config.weightOffsetBaselineG;
      if (command.bbwSetDrip || command.bbwAlphaBaseline == 0) {
        preset.bbwEwmaOffsetG = preset.weightOffsetBaselineG;
        armBbwQuickStart(preset, true);
      }
      if (command.bbwAlphaBaseline != 0)
        preset.bbwAlphaBaseline = command.bbwAlphaBaseline;
      if (command.bbwFullReset || command.bbwAlphaBaseline != 0) {
        preset.bbwEwmaAlpha = preset.bbwAlphaBaseline;
        preset.bbwAlphaLearned = 0;
      }
      RuntimeConfig candidate = runtimeConfig;
      applyShotPresetToConfig(preset, candidate);
      ++candidate.revision;
      if (candidate.revision == 0) {
        candidate.revision = 1;
      }
      commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }

    case WebCommandType::RESET_AUTO_TO_MANUAL_GUARD_SAMPLES: {
      // Alias of "I changed beans or grind" for the active preset.
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      // Store the pending shot before the new epoch, so it never counts for
      // the new coffee; a cancelled analysis does not learn.
      cancelPendingFinalize("Drip analysis ended by a new coffee", 2);
      mutableActiveShotPreset(presetBank).learningEpoch = shotStatisticsEpochNow();
      RuntimeConfig candidate = runtimeConfig;
      applyShotPresetToConfig(activeShotPreset(presetBank), candidate);
      ++candidate.revision;
      if (candidate.revision == 0) {
        candidate.revision = 1;
      }
      addDebugEvent(DebugCategory::CONFIG,
                    DebugCode::AUTO_TO_MANUAL_GUARD_SAMPLES_RESET);
      commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }

    case WebCommandType::PRESET_OP: {
      if (pendingPresetPersistence.requestId != 0) {
        rejectWebCommand(command);
        return;
      }
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      RecipeSnapshot quickBefore;
      quickBefore.presets = presetBank;
      quickBefore.runtime = runtimeConfig;
      bool ok = false;
      uint8_t newId = 0;
      const PresetAction action = static_cast<PresetAction>(command.presetAction);
      switch (action) {
        case PresetAction::APPLY:
          ok = setActiveShotPreset(presetBank, command.presetId);
          break;
        case PresetAction::SAVE: {
          ShotPreset *preset = mutableShotPreset(presetBank, command.presetId);
          if (preset == nullptr) {
            ok = false;
            break;
          }
          ShotPreset candidateRecipe = *preset;
          copyUserRecipeFromConfig(command.config, candidateRecipe,
                                   command.presetRecipeFields);
          if (command.lineaMicraBrewTargetSpecified)
            candidateRecipe.lineaMicraBrewTargetDeciC =
                command.lineaMicraBrewTargetDeciC;
          if (!command.bbwBaselineSpecified)
            candidateRecipe.weightOffsetBaselineG = preset->weightOffsetBaselineG;
          if (command.bbwAlphaBaseline != 0)
            candidateRecipe.bbwAlphaBaseline = command.bbwAlphaBaseline;
          if (command.presetDoseDg != 0) candidateRecipe.doseDg = command.presetDoseDg;
          if (command.presetGrindHints >= 0)
            candidateRecipe.grindHintsEnabled = command.presetGrindHints != 0;
          ok = validateShotPresetRecipe(candidateRecipe,
                                        runtimeConfig.retareWindowMs,
                                        runtimeConfig.autoRetare);
          if (ok) {
            // The epoch stays: arrival statistics count only shots judged
            // against the current window and target (plan §4.8), and the
            // first drop does not depend on either.
            ShotPreset previous = *preset;
            previous.weightOffsetBaselineG = candidateRecipe.weightOffsetBaselineG;
            previous.bbwAlphaBaseline = candidateRecipe.bbwAlphaBaseline;
            previous.doseDg = candidateRecipe.doseDg;
            previous.grindHintsEnabled = candidateRecipe.grindHintsEnabled;
            if (memcmp(&previous, &candidateRecipe, sizeof(previous)) != 0)
              bbwLearningBank.invalidate(preset->id, presetBank);
            *preset = candidateRecipe;
            presetBank.activeId = preset->id;
          }
          break;
        }
        case PresetAction::CREATE:
          ok = createUntitledShotPreset(presetBank, newId);
          if (ok) mutableShotPreset(presetBank, newId)->learningEpoch = shotStatisticsEpochNow();
          if (ok) armBbwQuickStart(*mutableShotPreset(presetBank, newId), false);
          break;
        case PresetAction::DELETE:
          ok = deleteShotPreset(presetBank, command.presetId);
          if (ok) {
            for (auto &state : bbwLearningBank.states)
              if (state.id == command.presetId) state = BbwLearningBank::State{};
          }
          break;
        case PresetAction::DUPLICATE:
          ok = duplicateShotPreset(presetBank, command.presetId, newId);
          if (ok) mutableShotPreset(presetBank, newId)->learningEpoch = shotStatisticsEpochNow();
          if (ok) armBbwQuickStart(*mutableShotPreset(presetBank, newId), false);
          break;
        case PresetAction::RENAME:
          ok = renameShotPreset(presetBank, command.presetId, command.presetName);
          break;
        case PresetAction::RESTORE_FACTORY_VALUES:
          ok = restoreFactoryShotPresetValues(presetBank, command.presetId);
          if (ok) {
            bbwLearningBank.invalidate(command.presetId, presetBank);
            mutableShotPreset(presetBank, command.presetId)->learningEpoch =
                shotStatisticsEpochNow();
            armBbwQuickStart(*mutableShotPreset(presetBank, command.presetId), true);
          }
          break;
        case PresetAction::NEW_COFFEE: {
          ShotPreset *preset = mutableShotPreset(presetBank, command.presetId);
          ok = preset != nullptr;
          if (ok) preset->learningEpoch = shotStatisticsEpochNow();
          break;
        }
      }
      if (!ok) {
        addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_REJECTED,
                      static_cast<int32_t>(command.presetAction));
        rejectWebCommand(command);
        return;
      }
      ensureShotPresetBank(presetBank, runtimeConfig.retareWindowMs,
                           runtimeConfig.autoRetare);
      const bool presetChanged =
          memcmp(&quickBefore.presets, &presetBank, sizeof(presetBank)) != 0;
      RuntimeConfig candidate = runtimeConfig;
      // The v1 preset form still carries the device-level switches.
      if (action == PresetAction::SAVE)
        copyDeviceProtectionFromConfig(command.config, candidate);
      applyShotPresetToConfig(activeShotPreset(presetBank), candidate);
      ++candidate.revision;
      if (candidate.revision == 0) {
        candidate.revision = 1;
      }
      commitLiveRuntimeConfig(candidate, RUNTIME_PERSIST_REASON_USER);
      const ShotPreset &beforeActive = activeShotPreset(quickBefore.presets);
      const ShotPreset &afterActive = activeShotPreset(presetBank);
      const bool applyMachineTemperature =
          beforeActive.id != afterActive.id ||
          beforeActive.lineaMicraBrewTargetDeciC !=
              afterActive.lineaMicraBrewTargetDeciC;
      RecipeSnapshot quickAfter;
      quickAfter.presets = presetBank;
      quickAfter.runtime = runtimeConfig;
      quickSettingsPersistPending |= !quickSettingsEqual(
          quickSettingsSnapshot(quickBefore), quickSettingsSnapshot(quickAfter));
      pendingPresetPersistence = {command.requestId, candidate.revision,
                                  presetChanged,
                                  applyMachineTemperature};
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }

    case WebCommandType::RESTART:
      holdOrBeginPlannedRestart(command);
      return;

    // Micra settings touch neither the network nor the machine: they persist
    // without a maintenance lease, so configuration and the cloud stay live.
    case WebCommandType::SAVE_LINEA_MICRA:
      if (!webCommandAllowsUnsafeConfiguration(command) ||
          !forwardAcceptedNetworkCommand(command)) {
        rejectWebCommand(command);
      }
      return;

    case WebCommandType::SAVE_NETWORK:
    case WebCommandType::SAVE_WEBHOOK:
    case WebCommandType::FORGET_NETWORK:
    case WebCommandType::CHANGE_DEVICE_PASSWORD:
    case WebCommandType::RESET_DEVICE_PASSWORD:
    case WebCommandType::RESET_NETWORK_AP:
    case WebCommandType::FACTORY_RESET:
      if (!webCommandAllowsUnsafeConfiguration(command)) {
        rejectWebCommand(command);
        return;
      }
      if (!beginMaintenanceLease(command, false)) {
        rejectWebCommand(command);
      }
      return;

    case WebCommandType::PERSIST_RUNTIME:
      rejectWebCommand(command);
      return;

    case WebCommandType::CLEAR_SHOT_LOG:
      rejectWebCommand(command);
      return;

    case WebCommandType::CLEAR_RESET_HISTORY:
      holdOrBeginResetHistoryClear(command);
      return;

    case WebCommandType::CLEAR_PREFERRED_SCALE:
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      clearPreferredScaleCache();
      servicePreferredScaleMacPersistence();
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::SELECT_PREFERRED_SCALE:
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      if (command.network.scaleSelectMac[0] == '\0') {
        selectPreferredScale("", "");
      } else if (!validPreferredScaleMac(command.network.scaleSelectMac)) {
        rejectWebCommand(command);
        return;
      } else {
        selectPreferredScale(command.network.scaleSelectMac, command.network.scaleSelectName);
      }
      servicePreferredScaleMacPersistence();
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::SET_SCALE_FRIENDLY_NAME:
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      if (!setScaleFriendlyName(command.network.scaleSelectMac,
                                command.network.scaleSelectName)) {
        rejectWebCommand(command);
        return;
      }
      servicePreferredScaleMacPersistence();
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::START_WIFI_SCAN:
      if (!beginMaintenanceLease(command, false)) {
        rejectWebCommand(command);
      }
      return;

    case WebCommandType::BUZZER_TEST: {
      if (!webCommandAllowsUnsafeConfiguration(command)) {
        rejectWebCommand(command);
        return;
      }
      bool accepted = false;
      if (command.bullseyeConfigSpecified) {
        BullseyeMelodyConfig testBullseye = {};
        if (!takeStagedBullseyeConfig(command.bullseyeStageRequestId,
                                      testBullseye) ||
            !validBullseyeRtttl(testBullseye.rtttl)) {
          rejectWebCommand(command);
          return;
        }
        // A busy finite cue rejects the test before the tune is swapped: only
        // this control task queues Bullseye, so an idle buzzer has none queued
        // that the phase timer could start with the test notes. An accepted
        // test has already copied its notes to the active playback buffer, and
        // the live config has a single writer here, so no stack copy is needed.
        if (localBuzzer.canStartNow()) {
          accepted = localBuzzer.configureBullseyeRtttl(testBullseye.rtttl) &&
                     localBuzzer.requestBullseye(/*allowQueue=*/false);
          (void)localBuzzer.configureBullseyeRtttl(
              bullseyeMelodyConfig.rtttl);
        }
      } else {
        accepted = buzzerPatternIsPulseTrain(command.buzzerPattern)
                       ? startPulseTrain(command.buzzerPattern,
                                         BUZZER_PULSE_TRAIN_DEBUG_MS)
                       : localBuzzer.request(command.buzzerPattern);
      }
      if (!accepted) {
        rejectWebCommand(command);
        return;
      }
      addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_ACCEPTED,
                    static_cast<int32_t>(command.type));
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }

    case WebCommandType::BOOKOO_DEBUG: {
      if (!controlAllowsConfigurationNow()) {
        rejectWebCommand(command);
        return;
      }
      if (!enqueueScaleDebugCommand(command.bookooDebugAction,
                                    command.bookooBeepLevel)) {
        rejectWebCommand(command);
        return;
      }
      addDebugEvent(DebugCategory::WEB, DebugCode::WEB_COMMAND_ACCEPTED,
                    static_cast<int32_t>(command.type));
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }

    case WebCommandType::MAINTENANCE_COMPLETE:
      completeMaintenanceLease(command);
      return;

    case WebCommandType::WIFI_CONNECT:
    case WebCommandType::WIFI_DISCONNECT:
    case WebCommandType::WIFI_RESTART:
    case WebCommandType::AP_START:
    case WebCommandType::AP_STOP:
    case WebCommandType::WEBUI_START:
    case WebCommandType::WEBUI_STOP:
    case WebCommandType::WEBUI_RESTART:
      if (!forwardAcceptedNetworkCommand(command)) {
        rejectWebCommand(command);
      }
      return;

    case WebCommandType::BLE_SCAN_INTENSITY: {
      if (!persistentConfigurationMutationAllowed(command)) {
        rejectWebCommand(command);
        return;
      }
      bool applied = false;
      if (command.bleScan.specified & BleScanCommandPayload::INTENSITY) {
        applied = true;
        persistBleScanIntensity(
            clampBleScanIntensity(command.bleScan.intensity));
      }
      if (command.bleScan.specified & BleScanCommandPayload::ENABLED) {
        applied = true;
        persistBleScanEnabled(command.bleScan.enabled != 0);
      }
      if (!applied) {
        rejectWebCommand(command);
        return;
      }
      // Hold the result: serviceBleScanPersistResult reports PERSISTED after
      // the settings_persist worker's deferred NVS flush succeeds.
      bleScanPersistMux.lock();
      if (pendingBleScanRequestIdCount ==
          PENDING_BLE_SCAN_REQUEST_CAPACITY) {
        for (uint8_t i = 1; i < PENDING_BLE_SCAN_REQUEST_CAPACITY; ++i) {
          pendingBleScanRequestIds[i - 1] = pendingBleScanRequestIds[i];
        }
        --pendingBleScanRequestIdCount;
      }
      pendingBleScanRequestIds[pendingBleScanRequestIdCount++] =
          command.requestId;
      bleScanPersistMux.unlock();
      addDebugEvent(DebugCategory::CONFIG, DebugCode::CONFIG_ACCEPTED,
                    static_cast<int32_t>(command.bleScan.intensity));
      return;
    }

    case WebCommandType::TASK_PROFILER_START:
      healthProfilerRequest.store(HealthProfilerRequest::START,
                                  std::memory_order_release);
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::TASK_PROFILER_STOP:
      healthProfilerRequest.store(HealthProfilerRequest::STOP,
                                  std::memory_order_release);
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;

    case WebCommandType::SCALE_PROFILE_START:
    case WebCommandType::SCALE_PROFILE_STOP:
    case WebCommandType::SCALE_PROFILE_DELETE: {
      // Refuse while a download pins the frozen generation; the profiler
      // repeats this guard when the health worker services the request.
      const bool mutating =
          command.type != WebCommandType::SCALE_PROFILE_STOP;
      if (mutating && scaleProfiler().downloadLeaseGeneration() != 0) {
        reportControlCommandResult(command, CommandResultState::FAILED);
        return;
      }
      scaleProfileRequest.store(
          command.type == WebCommandType::SCALE_PROFILE_START
              ? ScaleProfilerRequest::START
              : command.type == WebCommandType::SCALE_PROFILE_STOP
                    ? ScaleProfilerRequest::STOP
                    : ScaleProfilerRequest::DELETE,
          std::memory_order_release);
      reportControlCommandResult(command, CommandResultState::APPLIED);
      return;
    }
  }
}

void processWebCommands() {
  // WebCommand construction runs ~440 B of member initializers: skip it on
  // the common empty-queue tick.
  if (webCommandQueue == nullptr || controlResultPending ||
      maintenanceCancellationPending ||
      uxQueueMessagesWaiting(webCommandQueue) == 0) {
    return;
  }
  WebCommand command;
  if (xQueueReceive(webCommandQueue, &command, 0) == pdTRUE) {
    processWebCommand(command);
  }
}

void persistBleScanIntensity(BleScanIntensity intensity) {
  const BleScanIntensity clamped =
      clampBleScanIntensity(static_cast<uint8_t>(intensity));
  applyLiveBleScanIntensity(clamped);
  // Stage only: the settings_persist worker owns the durable NVS write, so
  // the Arduino control loop never performs flash I/O for this command.
  bleScanPersistMux.lock();
  bleScanPersistIntensity = static_cast<uint8_t>(clamped);
  bleScanPersistPending = true;
  bleScanPersistMux.unlock();
}

void persistBleScanEnabled(bool enabled) {
  if (liveBleEnabled() != enabled)
    applyLiveBleEnabled(enabled);
  // Stage only: the settings_persist worker owns the durable NVS write, so
  // the Arduino control loop never performs flash I/O for this command.
  bleScanPersistMux.lock();
  bleScanPersistEnabled = enabled ? 1 : 0;
  bleScanEnabledPersistPending = true;
  bleScanPersistMux.unlock();
}

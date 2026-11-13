// Fixed registrations run on the control task after the new runtime snapshot is
// published. Subscribers own both their relevant-field filter and the effect.
static bool cupSettingsChanged(const RuntimeConfig &a, const RuntimeConfig &b) {
  return a.minimumCupWeightG != b.minimumCupWeightG ||
         a.cupRemovedWeightG != b.cupRemovedWeightG ||
         a.retareStabilitySamples != b.retareStabilitySamples ||
         a.retareStabilityToleranceG != b.retareStabilityToleranceG ||
         a.retareStabilityMaxGapMs != b.retareStabilityMaxGapMs ||
         a.retareStabilityMinDurationMs != b.retareStabilityMinDurationMs ||
         a.postTareBaselineGraceMs != b.postTareBaselineGraceMs ||
         a.autoRetare != b.autoRetare || a.retareWindowMs != b.retareWindowMs;
}

const RuntimeSettingsSubscription runtimeSettingsSubscribers[] = {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    {OpenBrewByWeightNetwork::ntpSettingsChanged,
     [](const RuntimeConfig &, const RuntimeConfig &after) {
       networkManager.syncLiveRuntime(
           overlaySessionQuickSettings(sessionQuickSettings, after, true), &presetBank, true);
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return !OpenBrewByWeightNetwork::ntpSettingsChanged(a, b);
     }, [](const RuntimeConfig &, const RuntimeConfig &after) {
       networkManager.syncLiveRuntime(
           overlaySessionQuickSettings(sessionQuickSettings, after, true), &presetBank, false);
     }},
#endif
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return a.soundAlertsMuted != b.soundAlertsMuted ||
              a.alertOutputChannel != b.alertOutputChannel ||
              a.bookooMuteOnBuzzerOnly != b.bookooMuteOnBuzzerOnly ||
              a.bookooConnectBeepLevel != b.bookooConnectBeepLevel ||
              a.scaleMacCacheMode != b.scaleMacCacheMode;
     }, [](const RuntimeConfig &, const RuntimeConfig &after) {
       publishScaleWorkerPolicy(after, firmwareInitializationComplete);
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return a.scaleMacCacheMode != b.scaleMacCacheMode;
     }, [](const RuntimeConfig &, const RuntimeConfig &) {
       requestScalePreferenceModeReset();
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return noScaleBbwModeValue(a.noScaleBbwMode) !=
              noScaleBbwModeValue(b.noScaleBbwMode);
     }, [](const RuntimeConfig &, const RuntimeConfig &after) {
       noScaleShotGuardActivityAtMs = 0;
       noScaleShotGuardArmed = noScaleBbwEnabled(after.noScaleBbwMode);
       resetNoScaleRequireBypassGesture();
       // A held activator must not become a fresh relay-close request.
       if (!noScaleShotGuardArmed)
         noScaleShotGuardHold = machineLastIntention().holdActive;
     }},
    {cupSettingsChanged, [](const RuntimeConfig &, const RuntimeConfig &) {
       if (++cupSettingsGeneration == 0) ++cupSettingsGeneration;
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return cupSettingsChanged(a, b) ||
              a.autoTareOutsideBrew != b.autoTareOutsideBrew ||
              a.noScaleBbwMode != b.noScaleBbwMode;
     }, [](const RuntimeConfig &, const RuntimeConfig &) {
       if (++idleTareSettingsGeneration == 0) ++idleTareSettingsGeneration;
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return a.machinePowerControlEnabled != b.machinePowerControlEnabled;
     }, [](const RuntimeConfig &, const RuntimeConfig &after) {
       publishMachineIntegrationManualPower(after.machinePowerControlEnabled);
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return !a.soundAlertsMuted && b.soundAlertsMuted;
     }, [](const RuntimeConfig &, const RuntimeConfig &) {
       cancelOperationalAlerts();
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return a.serialLogLevel != b.serialLogLevel ||
              a.ringRetainLogLevel != b.ringRetainLogLevel;
     }, [](const RuntimeConfig &, const RuntimeConfig &after) {
       publishLogLevels(after);
       configureEspLogRuntime();
     }},
    {[](const RuntimeConfig &a, const RuntimeConfig &b) {
       return bookooDesiredVolume(!a.soundAlertsMuted,
                 effectiveAlertOutputChannel(a.alertOutputChannel),
                 a.bookooMuteOnBuzzerOnly, a.bookooConnectBeepLevel) !=
              bookooDesiredVolume(!b.soundAlertsMuted,
                 effectiveAlertOutputChannel(b.alertOutputChannel),
                 b.bookooMuteOnBuzzerOnly, b.bookooConnectBeepLevel);
     }, [](const RuntimeConfig &, const RuntimeConfig &after) {
       const int8_t volume = bookooDesiredVolume(
           !after.soundAlertsMuted,
           effectiveAlertOutputChannel(after.alertOutputChannel),
           after.bookooMuteOnBuzzerOnly, after.bookooConnectBeepLevel);
       if (volume == 0) requestBookooSilenceIfConfigured();
       else if (volume > 0) requestBookooAlertVolumeRestore();
     }},
};

// Included inside namespace openbrewbyweight by OpenBrewByWeightDomain.h.
// Publication storage and synchronization are owned by openBrewByWeight.cpp.
struct ControlStatusFields : ScaleLinkMetrics {
  CupTareDiagnostics cupTare;
  // Coherent publication identity. uptimeMs is the publication timestamp;
  // snapshotVersion advances exactly once for every committed snapshot.
  uint32_t snapshotVersion = 0;
  StopperState state = StopperState::REQUIRES_OFF;
  bool activeCycle = false;
  // A shot trace capture is open: cycle start to drip finalize. Nothing is
  // exported and no stored trace or old curve is read meanwhile.
  bool traceCapturing = false;
  // True only on a reader copy when its requested control refresh timed out.
  // The remaining fields still come from one complete committed snapshot.
  bool snapshotStale = false;
  bool relayClosed = false;
  bool machineRunning = false;
  bool reedOn = false;
  bool physicalActivatorOn = false;
  bool rawActivatorOn = false;
  bool virtualHoldOn = false;
  bool remoteControlEnabled = REMOTE_MACHINE_CONTROL_ENABLED;
  ControlSource source = ControlSource::NONE;
  uint32_t cycleId = 0;
  uint32_t bootId = 0;
  bool maintenanceLeaseActive = false;
  uint32_t maintenanceLeaseId = 0;
  uint32_t maintenanceStartedAtMs = 0;
  uint8_t scheduledRestartState = 0;  // ScheduledRestartState
  uint8_t scheduledRestartCause = 0;  // ScheduledRestartCause
  // Quick Settings Duration: policy, live overrides, time left.
  uint8_t quickRevertPolicy = 0;
  uint8_t quickOverrideMask = 0;  // one bit per QuickSettingField
  uint8_t quickSaved[7] = {};     // saved value each override hides
  uint32_t quickRevertRemainingMs = 0;
  uint32_t circuitElapsedMs = 0;
  RelaySafetyState safetyState = RelaySafetyState::BOOT_SAFE;
  RelaySafetyFault safetyFault = RelaySafetyFault::NONE;
  uint32_t safetyGeneration = 0;
  uint32_t safetyHardLimitMs = HARD_MAX_CIRCUIT_CLOSED_MS;
  uint32_t safetyRemainingMs = 0;
  bool backflushWaiting = false, backflushReady = false, backflushUnresolved = false;
  char backflushStopReason[25] = "none";
  bool safetyTimersReady = false;
  bool taskWatchdogReady = false;
  bool externalSafetyPresent = false;
  bool circuitFeedbackClosed = false;
  uint32_t resetReasonCode = 0;
  uint32_t unsafeResetCount = 0;
  bool bootLoopDetected = false;
  uint8_t resetHistoryCount = 0;
  ResetHistoryEntry resetHistory[RESET_HISTORY_CAPACITY] = {};
  bool scaleAvailable = false;
  WeightStreamState weightStreamState = WeightStreamState::NO_SAMPLE;
  WeightControlState weightControlState = WeightControlState::INACTIVE;
  bool currentWeightValid = false;
  float currentWeightG = 0.0f;
  uint32_t currentWeightAgeMs = 0;
  bool observedWeightValid = false;
  float observedWeightG = 0.0f;
  uint32_t observedWeightAgeMs = 0;
  bool currentTimerValid = false;
  // Scale-reported flow, fresh with weight. bool + int16 fill the padding
  // before currentTimerMs, so the status size budget holds.
  bool currentFlowRateValid = false;
  int16_t currentFlowRateCgS = 0;
  uint32_t currentTimerMs = 0;
  uint32_t currentTimerAgeMs = 0;
  uint32_t uptimeMs = 0;
  // Recent loop gap: max over the last completed ~5 s health window (and any
  // larger gap already seen in the in-progress window). Lifetime max is
  // loopMaxGapMs and never decreases until reboot.
  uint32_t loopIntervalGapMs = 0, loopMaxGapMs = 0, loopDeadlineMisses = 0,
           loopMaxExecutionUs = 0;
  uint32_t scaleWorkerMaxGapMs = 0, scaleWorkerDeadlineMisses = 0,
           scaleWorkerMaxExecutionUs = 0;
  uint32_t healthSnapshotVersion = 0, healthSnapshotAgeMs = UINT32_MAX;
  bool healthSnapshotValid = false;
  uint32_t loopStackMinBytes = UINT32_MAX;
  uint32_t scaleStackMinBytes = UINT32_MAX;
  uint32_t freeHeapBytes = 0;
  uint32_t minimumFreeHeapBytes = 0;
  uint32_t largestFreeHeapBlockBytes = 0;
  uint32_t internalHeapAllocatedBlocks = 0, internalHeapFreeBlocks = 0;
  uint16_t internalHeapFragmentationPermille = 0;
  uint32_t psramSizeBytes = 0;
  uint32_t psramFreeBytes = 0;
  uint32_t psramMinimumFreeBytes = 0;
  uint32_t psramLargestFreeBlockBytes = 0;
  // Backend-neutral host lifecycle metrics.
  uint8_t bleBackend = 2;  // Native NimBLE; retained in the diagnostic ABI.
  uint8_t bleRuntimeState = 0;
  int32_t bleRuntimeLastError = 0;
  int32_t bleRuntimeLastResetReason = 0;
  uint32_t bleRuntimeSyncGeneration = 0;
  uint32_t bleRuntimeResetCount = 0;
  uint32_t bleRuntimeHostStackMinBytes = UINT32_MAX;
  bool workBufExternal = false;
  bool jsonArenaExternal = false;
  uint32_t allocExternalFallbackCount = 0;
  uint32_t scaleEventsDropped = 0;
  RuntimeConfig config = {};
  uint8_t bbwPresetId = 0;
  uint8_t bbwAlpha = DEFAULT_BBW_EWMA_ALPHA;
  float bbwEwmaOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  // Drip chart: the active preset's newest shots (cg, newest first), how many
  // came after the drip was set and after quick start decided (0xFF = no
  // marker), and the quick start (QuickStartEntry fields).
  uint8_t bbwRecentCount = 0;
  uint16_t bbwRecentSkipped = 0;
  uint8_t bbwRecentAfterSet = 0xFF;
  uint8_t bbwRecentAfterQuick = 0xFF;
  int16_t bbwRecentCg[10] = {};
  uint8_t bbwQuickState = 0;
  uint8_t bbwQuickCount = 0;
  int16_t bbwQuickSetCg = 0;
  int16_t bbwQuickCg = 0;
  LastCycleSummary lastCycle = {};
  // Read-only Home lifecycle; never persisted or used to authorize actuation.
  PersistedLastShot lastShot = {}, lastGoodShot = {}, homeCycle = {};
  uint32_t homeResolvedAtMs = 0;
  bool homePending = false;
  bool lastGoodShotHistoryLinked = false;
  // Mirror of the newest activation-history record (WebUI History card).
  uint32_t lastActivationId = 0;
  uint32_t lastActivationEndedAtUnixSec = 0;
  uint32_t lastActivationEndedAtLocalSec = 0;
  uint32_t lastActivationDurationDs = 0;
  uint8_t lastActivationType = 2;  // openbrewbyweight::HistoryType::OTHER (history header not included)
  uint8_t lastActivationFlags = 0;
  HwmonSnapshot hwmon = {};
  uint32_t debugEventsDropped = 0;
  bool cycleFlowDuringRetare = false;
  bool cycleRetarePerformed = false;
  bool cycleStartedWithScale = false;
  bool cycleAutomaticBrew = false;
  bool cycleTimerOnly = false;
  uint32_t cycleFirstDropMs = 0;
  uint32_t cycleRetareAtMs = 0;
  char cycleScaleName[SCALE_FRIENDLY_NAME_MAX_LEN + 1] = {};
  uint32_t cycleRetareFlowFirstDetectedAtMs = 0;
  uint32_t cycleStartedAtMs = 0;
  uint32_t cycleElapsedMs = 0;
  bool cycleExtractionExtended = false;
  bool cycleSlowExtractionExtended = false;
  bool cycleTargetReachedEarly = false;
  float cycleActiveStopWeightG = 0.0f;
  uint32_t cycleExpectedEndMs = 0;  // Forecast cut on the shot clock; 0 = none.
  // SAP display forecast (acceleration-aware) on the shot clock; 0 = none.
  uint32_t cycleForecastEndMs = 0;
  // Live hero tint from the shot predictor (0 neutral, 1 fast, 2 on_recipe,
  // 3 slow), blend percent 0..100, and note badge (ShotTintNote).
  uint8_t cycleTintClass = 0;
  uint8_t cycleTintPct = 0;
  uint8_t cycleTintNote = 0;
  uint32_t cycleMinBbwBrewTimeRemainingMs = 0;
  bool cycleAutoToManualGuardArmed = false;
  bool cycleAutoToManualGuardEnforced = false;
  uint32_t cycleAutoToManualGuardRemainingMs = 0;
  bool cycleAccidentalTouchHolding = false;
  // New fields: consider debug export (OpenBrewByWeightDebugExport.h).
  uint8_t cycleAccidentalTouchPhase = 0;
  uint8_t cycleAccidentalTouchClass = 0;
  uint8_t cycleAccidentalTouchPendingCount = 0;
  bool cycleCupRemovedPending = false;
  bool cycleBbwProtectionEnabled = false;
  bool cycleBbwProtectionEnded = false;
  uint32_t autoToManualGuardTrendMs = 0;  // Idle scale-loss stop time.
  uint8_t scaleLossProjectedSource = 2;   // corridor::LossSource (window).
  char scaleProtocol[20] = "none";
  char connectedScaleMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char preferredScaleMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char preferredScaleName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  ScaleModel scaleModel = ScaleModel::Unknown;
  uint32_t scaleMacCachePauseRemainingMs = 0;
  bool noScaleShotGuardEnabled = true;
  bool noScaleShotGuardArmed = true;
  bool noScaleShotGuardHold = false;
  bool noScaleShotGuardScaleWasAvailable = false;
  uint32_t noScaleShotGuardCooldownRemainingMs = 0;
  bool cupStartGuardHold = false;
  MachineRunState machineRunState = MachineRunState::IDLE;
  bool machineStartAckPending = false;
  bool machineStopAckPending = false;
  bool machineOrphanRun = false;
  CupPresenceState cupPresenceState = CupPresenceState::ABSENT;
  bool cupPresent = false;
  bool configPersistPending = false;
  bool configPersistFailed = false;
  BootState bootState = BootState::BOOTING;
  BootCapabilities bootCapabilities = {};
  bool bootComplete = false;
  bool bootDegraded = false;
  bool scaleWorkerReady = false;
  bool usbConsoleIo4Closed = false;
  UsbSerialEnableSource usbSerialEnableSource = UsbSerialEnableSource::OFF;
  uint8_t bleScanIntensity = 0;
  bool bleScanEnabled = true;
};

// The live or just-ended activity's curve (empty otherwise), published in
// PSRAM under the same mutex/version. Readers receive one coherent copy.
struct ControlStatusCurve {
  ShotCurveRecord shotCurve{};
};
struct ControlStatusSnapshot : ControlStatusFields, ControlStatusCurve {};
static_assert(sizeof(ControlStatusFields) <= 2048,
              "Control status exceeds its internal publication budget");
// 6920: +8 bytes for the shot predictor's live hero tint (class, blend
// percent, note); 6944: +24 for the drip chart (newest five shots, markers
// and quick start); 6952: +8 for the drip chart's newest ten shots; 7432: the
// curve as one delta-stream record (its scalars leave the internal fields) —
// external work-area growth, internal budget unchanged.
static_assert(sizeof(ControlStatusSnapshot) <= 7432,
              "ControlStatusSnapshot exceeds its external workspace budget");

// Gate checks use this compact internal view. The full scalar/curve snapshot
// is copied only into the reader's owned workspace.
struct ControlGateSnapshot {
  BootState bootState = BootState::BOOTING;
  StopperState state = StopperState::REQUIRES_OFF;
  bool activeCycle = false;
  bool relayClosed = false;
  bool machineRunning = false;
  bool physicalActivatorOn = false;
  bool maintenanceLeaseActive = false;
  uint32_t maintenanceLeaseId = 0;
  ControlSource source = ControlSource::NONE;
};

static_assert(sizeof(ControlGateSnapshot) <= 32,
              "ControlGateSnapshot must stay a stack-safe gate copy");

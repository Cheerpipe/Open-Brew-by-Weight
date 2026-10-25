#pragma once

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightAuditStore.h"
#include "OpenBrewByWeightDebugExport.h"
#include "OpenBrewByWeightHistoryTypes.h"
#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightShotCurveTypes.h"
#include "OpenBrewByWeightShotLogTypes.h"
#include "OpenBrewByWeightTime.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "OpenBrewByWeightBootHeap.h"
#include "OpenBrewByWeightResourceOwner.h"
#include "OpenBrewByWeightNetworkSetup.h"
#include "OpenBrewByWeightWebAccess.h"

#include <WiFi.h>
#include <esp_http_server.h>
#include <DNSServer.h>
#include "OpenBrewByWeightRfCoex.h"

#include "OpenBrewByWeightTaskProfiler.h"
#include "OpenBrewByWeightScaleProfiler.h"
#include "OpenBrewByWeightWebhook.h"

#include <atomic>

struct timeval;

namespace openbrewbyweight {

struct FreeRtosQueueDeleter {
  void operator()(QueueHandle_t handle) const { vQueueDelete(handle); }
};

struct FreeRtosSemaphoreDeleter {
  void operator()(SemaphoreHandle_t handle) const { vSemaphoreDelete(handle); }
};

enum class StaState : uint8_t {
  NOT_CONFIGURED,
  CONNECTING,
  CONNECTED,
  FAILED,
  DISCONNECTED
};

enum class WifiScanState : uint8_t {
  IDLE,
  QUEUED,
  RUNNING,
  READY,
  FAILED,
  CANCELED
};

constexpr size_t MAX_WIFI_SCAN_RESULTS = 12;
constexpr size_t kNetworkLogBatchSize = 32;

struct WifiScanNetwork {
  char ssid[WIFI_SSID_CAPACITY] = {};
  int32_t rssi = 0;
  uint8_t channel = 0;
  bool open = false;
};

struct WifiScanSnapshot {
  WifiScanState state = WifiScanState::IDLE;
  uint32_t updatedAtMs = 0;
  uint8_t count = 0;
  WifiScanNetwork networks[MAX_WIFI_SCAN_RESULTS] = {};
};

// Fields httpd needs from persisted STA config. Do not return PersistedSettings
// by value on the 8 KiB httpd stack.
struct StaJoinHints {
  bool staConfigured = false;
  bool staOpen = false;
  uint8_t staConfigState = 0;
  char staSsid[WIFI_SSID_CAPACITY] = {};
};

// Coherent radio-only snapshot; excludes recipes, logs and NVS metadata.
struct NetworkSettingsSnapshot {
  bool staConfigured = false;
  bool staOpen = false;
  bool staWifiSleep = true;
  uint8_t staConfigState = 0;
  uint8_t staIpMode = 0;
  char staSsid[WIFI_SSID_CAPACITY] = {};
  char staPassword[WIFI_PASSWORD_CAPACITY] = {};
  char devicePassword[WIFI_PASSWORD_CAPACITY] = {};
  char deviceName[DEVICE_NAME_CAPACITY] = {};
  uint8_t staIp[4] = {};
  uint8_t staNetmask[4] = {};
  uint8_t staGateway[4] = {};
  uint8_t staDns1[4] = {};
  uint8_t staDns2[4] = {};
};
static_assert(sizeof(NetworkSettingsSnapshot) <= 224,
              "Radio snapshot exceeds its stack budget");

struct NetworkStatusSnapshot {
  bool networkActive = false;
  bool apActive = false;
  bool wifiConfigured = false;
  bool staOpen = false;
  bool staWifiSleep = true;
  WifiPsLive wifiPs = WifiPsLive::UNKNOWN;
  RfCoexPreference wifiCoex = RfCoexPreference::UNKNOWN;
  bool staLinkMetricsValid = false;
  uint8_t apClients = 0;
  StaState staState = StaState::NOT_CONFIGURED;
  uint8_t staIpMode = static_cast<uint8_t>(StaIpMode::DHCP);
  uint8_t staConfigState = static_cast<uint8_t>(StaConfigState::CONFIRMED);
  int8_t staRssi = 0;
  uint8_t staSignalLevel = 0;
  uint8_t staSignalQualityPct = 0;
  uint32_t windowRemainingMs = 0;
  uint32_t confirmRemainingMs = 0;
  // SoftAP teardown deadline once the station is up (0 = none yet; see
  // softApTeardownStep). Confirm and a recovery AP with phones associated set
  // the grace window; the "finish" action shortens it to a probe linger.
  uint32_t apTeardownAtMs = 0;
  uint32_t taskAgeMs = 0;
  uint32_t taskStackMinBytes = UINT32_MAX;
  uint32_t startupFailures = 0;
  // Onboarding phase; the network task is the only writer.
  SetupPhase setupPhase = SetupPhase::NONE;
  // Uncommitted onboarding brewing choice; cleared when the SoftAP stops.
  SetupBrew setupBrew = SetupBrew::NONE;
  uint8_t setupPresetId = 0;
  uint32_t lastCommandRequestId = 0;
  CommandResultState lastCommandState = CommandResultState::NONE;
  bool httpActive = false;
  bool staReconnectHeld = false;
  bool apStartHeld = false;
  bool httpStartHeld = false;
  bool devicePasswordFactory = false;
  bool ntpMayArm = false;
  uint8_t wifiMode = 0;
  uint8_t channel = 0;
  uint8_t scanState = 0;
  uint8_t ntpState = 0;
  int32_t wifiStatus = 6;
  uint32_t staConnectAgeMs = 0;
  uint32_t staReconnectAgeMs = 0;
  uint32_t staReconnectDelayMs = 0;
  NetworkInterfaceSnapshot iface = {};
  char deviceName[DEVICE_NAME_CAPACITY] = {};
  char mdnsHost[DEVICE_NAME_CAPACITY] = {};
};

struct NetworkHeapTelemetrySnapshot {
  HeapLifecycleAggregate http = {};
  HeapLifecycleAggregate wifi = {};
  HeapLifecycleAggregate ota = {};
};

__attribute__((noinline))
inline const char *connectionSignalLevelJson(uint8_t level) {
  static const char *const kLevels[] = {"null", "1", "2", "3"};
  return kLevels[level < 4 ? level : 0];
}

inline uint8_t wifiRssiToSignalQualityPct(int32_t rssi) {
  if (rssi <= -100) {
    return 0;
  }
  if (rssi >= -50) {
    return 100;
  }
  return static_cast<uint8_t>(2 * (rssi + 100));
}

inline int8_t clampWifiRssi(int32_t rssi) {
  if (rssi < -128) {
    return -128;
  }
  if (rssi > 127) {
    return 127;
  }
  return static_cast<int8_t>(rssi);
}

struct NetworkWorkBuf;

struct UiStreamFingerprints {
  static constexpr size_t kHomeFields = 128, kDiagFields = 224, kCardFields = 32,
                          kSettingsFields = 80;
  // The shot card's ten curve scalars (forEachShotCurveScalar) follow its
  // card fields in cardHashes.
  static constexpr size_t kCurveFields = 10;
  uint32_t homeHashes[kHomeFields] = {};
  uint32_t diagHashes[kDiagFields] = {};
  uint32_t cardHashes[kCardFields + kCurveFields] = {};
  uint32_t settingsHashes[kSettingsFields] = {};
  // Rows (id, content hash) of the last standing Stats and History pages the
  // page holds: a later standing page names an unchanged row by its id.
  // statsNext collects a paced page until its last frame lands.
  static constexpr size_t kHeldRows = 32;
  struct HeldRow { uint32_t id, hash; };
  HeldRow statsHeld[kHeldRows] = {}, statsNext[kHeldRows] = {}, historyHeld[kHeldRows] = {};
  uint8_t statsHeldCount = 0, historyHeldCount = 0;
};

struct NetworkBridgeCallbacks {
  void (*copyControlStatus)(ControlStatusSnapshot &output) = nullptr;
  void (*copyControlGate)(ControlGateSnapshot &output) = nullptr;
  void (*refreshControlStatus)() = nullptr;
  bool (*enqueueWebCommand)(const WebCommand &command) = nullptr;
  size_t (*copyDebugEvents)(uint32_t afterSequence, DebugEvent *output,
                            size_t capacity,
                            DebugLogReadMetadata *metadata) = nullptr;
  size_t (*copyAuditEvents)(uint32_t afterSequence, AuditRecord *output,
                            size_t capacity, uint32_t *dropped,
                            uint32_t *rateLimited) = nullptr;
  uint32_t (*auditLogLatestSequence)() = nullptr;
  void (*copyAuditStoreStatus)(AuditStoreStatus &output) = nullptr;
  bool (*setAuditVerbose)(bool on) = nullptr;
  uint32_t (*auditVerboseDeadline)() = nullptr;
  uint32_t (*debugLogLatestSequence)() = nullptr;
  void (*addDebugEvent)(DebugCategory category, DebugCode code,
                        int32_t argument1, int32_t argument2) = nullptr;
  void (*reportTaskWatchdogFault)() = nullptr;
  void (*requestSafeRestart)() = nullptr;
  size_t (*copyShotRecords)(ShotLogRecord *output, size_t capacity) = nullptr;
  void (*copyShotStatsRecords)(ShotStatsRecordsSnapshot &output) = nullptr;
  bool (*copyShotCurveById)(uint32_t shotId, ShotCurveRecord &curve) = nullptr;
  bool (*copyHomeShot)(ShotLogRecord &record, ShotCurveRecord *curve) = nullptr;
  bool (*shotLogSavePending)() = nullptr;
  void (*copyHistoryPage)(HistoryPage &page, size_t offset, size_t limit,
                          ShotLogSortDir dir, uint8_t types) = nullptr;
  // Cheap change epoch (store header digest) so idle dispatches skip the
  // page copy entirely.
  uint32_t (*historyEpoch)() = nullptr;
  // Same cheap gate for the shot log (record store header digest; curve
  // stores only mutate alongside records).
  uint32_t (*shotLogEpoch)() = nullptr;
  ShotStatsView (*copyShotStats)() = nullptr;
  bool (*deleteHistoryRecord)(uint32_t id) = nullptr;
  bool (*clearHistoryLog)() = nullptr;
  bool (*deleteShotRecord)(uint32_t id) = nullptr;
  bool (*rateShotRecord)(uint32_t id, uint8_t rating) = nullptr;
  bool (*rateLastShot)(uint8_t rating) = nullptr;
  bool (*clearShotLog)() = nullptr;
  bool (*clearLastShot)() = nullptr;
  bool (*clearResetHistory)(uint32_t unsafeResetCount) = nullptr;
  bool (*releaseNvsSpaceForFactoryReset)() = nullptr;
  bool (*resetAllDurableStores)(PersistedSettings &settings) = nullptr;
  // Keeps NVS Wi-Fi/runtime saves from overwriting a newer preferred scale MAC.
  void (*copyPreferredScaleMac)(char *out, size_t capacity) = nullptr;
  void (*copyPreferredScaleName)(char *out, size_t capacity) = nullptr;
  void (*copyScaleHistory)(ScaleHistoryEntry *out) = nullptr;
  void (*copyRecipeSnapshot)(RecipeSnapshot *out) = nullptr;
  void (*copyPresetBank)(ShotPresetBank *out) = nullptr;
  void (*copyPresetShotStats)(const ShotPresetBank &bank, PresetShotStats *out) = nullptr;
  void (*copyRuntimeConfig)(RuntimeConfig *out) = nullptr;
  void (*copyBullseyeConfig)(BullseyeMelodyConfig *out) = nullptr;
  bool (*stageBullseyeConfig)(const BullseyeMelodyConfig &config,
                              uint32_t requestId) = nullptr;
  void (*copyDebugExportExtras)(DebugExportExtras &out,
                                const ControlStatusSnapshot &control) = nullptr;
  void (*copyTaskProfiler)(TaskProfilerSnapshot &out) = nullptr;
  void (*copyScaleProfilerStatus)(ScaleProfilerStatus &out) = nullptr;
  void (*requestLoopMaxReset)() = nullptr;
  void (*requestScaleGapMaxReset)() = nullptr;
  // Notify the controller that the STA address was assigned or changed.
  // Called from the network task with the dotted IP; implementations must be
  // non-blocking and enqueue-only.
  void (*reportStationIpChange)(const char *ip) = nullptr;
};

// Pre-computed stats JSON fragment (WebUI/HA Stats mirror); defined in the
// integration API include with external linkage so the shots-page handler
// (inlined earlier in the same translation unit) can share it.
bool buildIntegrationStats(const ShotStatsView &stats, char *output,
                           size_t capacity, size_t window = SHOT_LOG_STATS_WINDOW,
                           bool durations = true);

class OpenBrewByWeightNetwork {
 public:
  OpenBrewByWeightNetwork();
  OpenBrewByWeightNetwork(const OpenBrewByWeightNetwork &) = delete;
  OpenBrewByWeightNetwork &operator=(const OpenBrewByWeightNetwork &) = delete;

  bool begin(const PersistedSettings &settings,
             const NetworkBridgeCallbacks &callbacks);
  // Stops the manager and webhook workers, then releases every begin()
  // resource. Safe after a partial begin() and when called repeatedly.
  bool stop();
  bool enqueueAcceptedCommand(const WebCommand &command);
  NetworkStatusSnapshot snapshot();
  NetworkHeapTelemetrySnapshot heapTelemetrySnapshot();
  void requestNtpSyncIfNeeded();
  void syncPreferredScale(const char *mac, const char *name);
  void syncScaleLinkRf(bool connectingOrUp);
  void syncScaleConnectingRf(bool connecting);
  void syncControlCriticalRf(bool active, bool publishTransition = false);
  void syncScaleHuntRf(bool huntActive);
  // Scale worker, per weight sample: wakes the stream dispatch when it is due.
  void noteScaleSample();
  void syncLiveRuntime(const RuntimeConfig &runtime,
                       const ShotPresetBank *presets, bool ntpSettingsChanged);
  static bool ntpSettingsChanged(const RuntimeConfig &before,
                                 const RuntimeConfig &after);
  void syncLiveBullseye(const BullseyeMelodyConfig &config);
  void syncDurableStorageRevision(uint32_t storageRevision);
  PersistedSettings settingsCopy();
  void copySettings(PersistedSettings &output);
  StaJoinHints staJoinHints();
  bool enqueueWebhook(const WebhookEvent &event);
  WebhookConfig webhookConfig() const;
  // Gate shared by every registered handler: answers the request and returns
  // true when its Host does not name this controller (DNS rebinding).
  static bool rejectForeignHost(httpd_req_t *request);

  private:
  void mergePreferredScaleMac(PersistedSettings &settings);
  void clearStagedWebhook(uint32_t requestId);
  void clearStagedLineaMicra(uint32_t requestId);
  void overlayLiveShotSettings(PersistedSettings &settings);
  static constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 25000;
  static constexpr uint32_t STA_RECOVERY_ATTEMPT_MS = 60000;
  // Onboarding (/setup live apply) attempts age out sooner: a wrong
  // password can leave the driver mid-status (AUTH_EXPIRE keeps the last
  // status), which the terminal-fail set cannot distinguish from healthy
  // association progress.
  static constexpr uint32_t LIVE_APPLY_ATTEMPT_MS = 20000;
  static constexpr uint32_t STA_CONFIRM_TIMEOUT_MS = 180000;
  // Automatic STA retries back off from 10 s to 120 s while the network
  // stays unreachable; every retry costs an all-channel scan.
  static constexpr uint32_t STA_RECONNECT_INTERVAL_MS = 10000;
  static constexpr uint32_t STA_RECONNECT_MAX_INTERVAL_MS = 120000;
  // Auto SoftAP shuts down after this long with zero SoftAP stations.
  // Resets when the last client leaves; skipped while AP_START keep is set.
  static constexpr uint32_t SOFTAP_IDLE_TIMEOUT_MS = 180000;
  // After a /setup confirm, keep the SoftAP up this long so the success
  // screen (with its .local/IP links) stays readable inside the OS captive
  // window; the "finish" action ends it early after a probe linger.
  static constexpr uint32_t SOFTAP_TEARDOWN_GRACE_MS = 60000;
  static constexpr uint32_t SOFTAP_FINISH_LINGER_MS = 3000;
  static constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 20000;
  static constexpr uint32_t RESTART_DELAY_MS = 750;
  // A freshly committed OTA image proves itself by serving its Web UI. Confirm
  // no earlier than this so a crash during startup still rolls back...
  static constexpr uint32_t OTA_CONFIRM_MIN_UPTIME_MS = 15000;
  // ...and give up if HTTP never comes up. A bootable previous slot is then
  // marked invalid so the bootloader can roll back; if no previous slot is
  // bootable the running image is confirmed so the machine still has firmware.
  static constexpr uint32_t OTA_CONFIRM_DEADLINE_MS = 180000;
  // Each HTTP socket read waits five seconds. Wi-Fi/BLE coexistence can stall
  // longer than the previous 15 s total without meaning that the peer died.
  // This remains bounded so a vanished client cannot occupy a socket forever.
  static constexpr uint8_t OTA_RECEIVE_ATTEMPTS = 6;
  static constexpr uint32_t OTA_RESTART_GIVE_UP_MS = 300000;
  static constexpr uint32_t NETWORK_RETRY_MIN_MS = 1000;
  static constexpr uint32_t NETWORK_RETRY_MAX_MS = 30000;
  static constexpr uint32_t COMMAND_RETRY_MIN_MS = 250;
  static constexpr uint32_t MAINTENANCE_PUBLICATION_TIMEOUT_MS = 2000;
  static constexpr uint32_t HTTP_RETRY_MS = 1000;
  static constexpr uint32_t WIFI_PS_RETRY_MS = 1000;
  // Display-only status fields (extended status, steady-state RSSI) refresh at
  // 1 s from the 20 Hz service pass; connect/disconnect edges still update
  // link metrics immediately.
  static constexpr uint32_t NETWORK_EXTENDED_STATUS_INTERVAL_MS = 1000;
  static constexpr uint32_t STA_RSSI_SAMPLE_INTERVAL_MS = 1000;
  static constexpr uint32_t HEALTH_TELEMETRY_INTERVAL_MS = 5000;
  static constexpr uint32_t NETWORK_STOP_TIMEOUT_MS = 5000;
  static constexpr uint8_t COMMAND_MAX_ATTEMPTS = 5;
  // Admin unlock is RAM-only and tied to the exclusive WebUI claim. Idle
  // slides on Admin page polls and privileged APIs, not on Home polls.
  static constexpr uint32_t ADMIN_UNLOCK_IDLE_MS = 15 * 60 * 1000;
  static constexpr uint32_t WEB_UI_OVERRIDE_MS = 60 * 1000;
  // Machine config JSON is ~1 KiB and grows with NTP custom / bool false
  // literals; keep headroom above the wire payload.
  static constexpr size_t REQUEST_BODY_CAPACITY = 2048;
  static constexpr size_t LOG_BATCH_SIZE = kNetworkLogBatchSize;
  static constexpr size_t WEB_UI_CLIENT_ID_CAPACITY = 25;

  static OpenBrewByWeightNetwork *instance_;

  PersistedSettings &settings_;
  NetworkBridgeCallbacks callbacks_ = {};
  UniqueResource<QueueHandle_t, FreeRtosQueueDeleter> acceptedCommandQueue_;
  TaskHandle_t taskHandle_ = nullptr;
  UniqueResource<SemaphoreHandle_t, FreeRtosSemaphoreDeleter> taskStopped_;
  UniqueResource<SemaphoreHandle_t, FreeRtosSemaphoreDeleter>
      statusResponseMux_;
  NetworkWorkBuf *workBuf_ = nullptr;
  // Network manager is a boot-lifetime owner. stop() is repeatable, while a
  // successful begin-stop-begin cycle is rejected instead of reusing stale
  // protocol/timer state.
  bool beginCompleted_ = false;
  httpd_handle_t server_ = nullptr;
  struct UiStreamSession {
    int fd = -1;
    uint32_t openedAtMs = 0, boundAtMs = 0, heardAtMs = 0, observedCycle = 0, observedBoot = 0;
    uint32_t sequence = 0, cycle = 0, shot = 0, boot = 0;
    uint32_t fingerprint = 0, prefixHash = 0;
    // presetsTag the page reported at bind (0: none); consumed by its first
    // Home snapshot.
    uint32_t knownPresetsTag = 0;
    uint16_t cursor = 0;
    uint32_t homeBoot = 0, homeClockSync = 0, homeClockUtc = 0;
    // Diagnostic page subscription: state dies with the session struct reset,
    // so an unbound or superseded socket never keeps streaming diagnostics.
    // Slots cover the live projection plus every migrated REST section;
    // sampled sections (health, NVS, profilers, network measurements) share
    // one 4 s evaluation gate so their producers run at most per interval.
    uint32_t diagBoot = 0, diagSlowAtMs = 0;
    uint32_t diagTasksFingerprint = 0, diagProfileFingerprint = 0,
             diagAuditFingerprint = 0;
    bool diagnostic = false, diagResync = true;
    // Settings subscription: standing for the page session once subscribed.
    // The source gate keeps idle dispatches from formatting the projection;
    // the tag names the model a reconnecting page still holds.
    bool settingsOn = false, settingsResync = true;
    uint32_t settingsBoot = 0, settingsGate = 0, settingsKnownTag = 0;
    // Serial-log subscription, same view-scoped contract: a cheap ring
    // watermark gates the copy, hasMore pages across dispatches, and a
    // cursor reset refills from the oldest retained event.
    bool logOn = false, logPush = false;
    uint32_t logAfter = 0;
    // History page subscription, same view-scoped contract: the window dies
    // with the session, one-shot fetches append without disturbing it, and
    // epoch/fingerprint suppress frames while the page is unchanged. Type
    // masks (HistoryType bits, 0 = all) filter the standing and fetch pages.
    bool historyOn = false, historyResync = true, historyFetch = false;
    size_t historyOffset = 0, historyLimit = HISTORY_PAGE_DEFAULT,
           historyFetchOffset = 0;
    uint8_t historyTypes = 0, historyFetchTypes = 0;
    ShotLogSortDir historyDir = ShotLogSortDir::Desc;
    uint32_t historyEpoch = 0, historyFingerprint = 0, historyBoot = 0;
    uint32_t historyRequest = 0, historyFetchRequest = 0;
    // Stats page subscription, same layout plus the sort field and the
    // fetch's own window and filter (sentinel appends page by the view's
    // order and filter; the CSV export asks for date/desc, unfiltered), and
    // the number of newest shots the aggregate and charts cover.
    bool statsOn = false, statsResync = true, statsFetch = false;
    size_t statsOffset = 0, statsLimit = SHOT_LOG_PAGE_DEFAULT,
           statsFetchOffset = 0, statsFetchLimit = SHOT_LOG_PAGE_DEFAULT;
    size_t statsWindow = SHOT_LOG_STATS_WINDOW;
    ShotLogSort statsSort = ShotLogSort::Date, statsFetchSort = ShotLogSort::Date;
    ShotLogSortDir statsDir = ShotLogSortDir::Desc, statsFetchDir = ShotLogSortDir::Desc;
    ShotListFilter statsFilter, statsFetchFilter;
    uint32_t statsEpoch = 0, statsFingerprint = 0, statsBoot = 0, statsSeq = 0;
    // Window and active preset of the last standing page's header.
    size_t statsShownWindow = 0;
    uint8_t statsPresetId = 0;
    // Paced delivery: one budgeted frame per dispatch resumes the in-flight
    // page (snapshot/fetch identity and store epoch captured at page start)
    // until its terminating frame; a deferred send keeps this state intact.
    bool statsPaging = false, statsPageSnapshot = false, statsPageFetch = false;
    size_t statsSent = 0;
    uint32_t statsPageEpoch = 0, statsPageRequest = 0, statsSentAtMs = 0;
    uint32_t statsRequest = 0, statsFetchRequest = 0;
    char clientId[WEB_UI_CLIENT_ID_CAPACITY] = {};
    bool bound = false, resync = true;
    bool homeResync = true;
    // Hidden page: frames wait (fingerprints kept) outside a live shot.
    bool paused = false;
    // Captured at handshake: the socket is served by the SoftAP interface, so
    // the session reports admin unlocked for its lifetime (the WPA2
    // passphrase is the device password).
    bool fromSoftAp = false;
  };
  UiStreamSession uiStreams_[2];
  std::atomic<bool> uiStreamWorkPending_{false};
  std::atomic<bool> uiStreamUrgent_{false};
  uint32_t uiStreamDispatchAtMs_ = 0;
  // Periodic dispatches follow the scale's weight samples (counted by the
  // scale worker); the worker only wakes this task while a stream is open.
  std::atomic<uint32_t> scaleSamples_{0}, uiStreamDispatchSamples_{0};
  std::atomic<bool> uiStreamOpen_{false};
  mutable TaskMutex dataMux_;
  char activeWebUiClientId_[WEB_UI_CLIENT_ID_CAPACITY] = {};
  bool webUiOverrideActive_ = false;
  uint32_t webUiOverrideUntilMs_ = 0;
  char adminUnlockClientId_[WEB_UI_CLIENT_ID_CAPACITY] = {};
  bool adminUnlocked_ = false;
  uint32_t adminUnlockUntilMs_ = 0;
  WebAccessPasswordPeer passwordPeers_[WEB_ACCESS_PASSWORD_PEERS] = {};
  // Last Integration API request; the Access panel warns before the login
  // starts asking Home Assistant for a token.
  uint32_t integrationSeenMs_ = 0;
  bool integrationSeen_ = false;
  // Web UI session that asked for that password-change request; it stays
  // signed in. Another request (USB, a later one) keeps nobody.
  uint8_t passwordKeepToken_[WEB_ACCESS_TOKEN_BYTES] = {};
  uint32_t passwordKeepRequestId_ = 0;
  bool passwordKeepValid_ = false;
  uint32_t webAccessRetryAtMs_ = 0;  // 0 = no failed save waiting
  NetworkStatusSnapshot status_ = {};
  HeapLifecycleTracker httpHeap_ = {};
  HeapLifecycleTracker wifiHeap_ = {};
  HeapLifecycleTracker otaHeap_ = {};
  bool startupComplete_ = false;
  // Latched for process lifetime after first STA CONNECTED. SoftAP auto-raise
  // is boot/bootstrap only; never cleared by startStation / pending revert.
  bool staEverConnected_ = false;
  bool scanRequested_ = false;
  bool restartPending_ = false;
  bool apRestartPending_ = false;
  // SetupPhase::QUEUED retry: the live-apply connect waits out deferred RF
  // gates like restartRequestedAtMs_ does.
  uint32_t liveApplyRetryAtMs_ = 0;
  bool acceptedCommandPending_ = false;
  bool completionPending_ = false;
  bool staConfirmArmed_ = false;
  bool pendingConfirmRequest_ = false;
  bool staReconnectHeld_ = false;
  bool apStartHeld_ = false;
  // Latched when SoftAP idle timeout fires. Blocks further auto SoftAP this
  // boot; USB AP_START (force + keep) still works. Cleared only by reboot.
  bool apAutoRaiseExhausted_ = false;
  bool httpStartHeld_ = false;
  std::atomic<bool> scaleConnectingOrUp_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<bool> scaleConnecting_{false};
  std::atomic<bool> controlCriticalRfActive_{false};
  std::atomic<uint32_t> rfGateGeneration_{0};
  std::atomic<bool> scaleHuntRfActive_{false};
  wifi_ps_type_t lastAppliedWifiPs_{WIFI_PS_NONE};
  // Inputs of the last verified power-save write; the driver is re-read only
  // when they change or once per WIFI_PS_RETRY_MS.
  uint8_t lastWifiPsInputs_ = 0xFF;
  bool lastAppliedWifiPsValid_{false};
  uint32_t wifiPsNextWriteAtMs_{0};
  bool mdnsActive_ = false;
  bool mdnsServiceActive_ = false;
  char activeMdnsHost_[DEVICE_NAME_CAPACITY] = {};
  std::atomic<bool> otaRestartPending_{false};
  bool otaRollbackRestartPending_ = false;
  const char *otaBootReason_ = "";
  int32_t otaBootLastError_ = 0;
  std::atomic<uint32_t> otaRestartRequestedAtMs_{0};
  WebCommand acceptedCommand_ = {};
  WebCommand completionCommand_ = {};
  uint8_t acceptedCommandAttempts_ = 0;
  uint8_t startupFailures_ = 0;
  uint32_t acceptedCommandReceivedAtMs_ = 0;
  uint32_t acceptedCommandRetryAtMs_ = 0;
  uint32_t networkRetryAtMs_ = 0;
  uint32_t httpRetryAtMs_ = 0;
  std::atomic<uint32_t> lastTaskProgressAtMs_{0};
  std::atomic<uint32_t> taskStackMinBytes_{UINT32_MAX};
  uint32_t nextRequestId_ = 1;
  uint32_t scanMaintenanceLeaseId_ = 0;
  uint32_t scanRequestId_ = 0;
  uint32_t networkStartedAtMs_ = 0;
  uint32_t apIdleDeadlineMs_ = 0;
  uint8_t lastApClients_ = 0;
  uint32_t staConnectStartedAtMs_ = 0;
  uint32_t staReconnectAttemptAtMs_ = 0;
  uint32_t staReconnectIntervalMs_ = STA_RECONNECT_INTERVAL_MS;
  uint32_t extendedStatusAtMs_ = 0;
  uint32_t staRssiSampledAtMs_ = 0;
  uint32_t staConfirmDeadlineMs_ = 0;
  uint32_t restartRequestedAtMs_ = 0;
  bool ntpStarted_ = false;
  bool ntpRearmPending_ = false;
  bool ntpManualSyncPending_ = false;
  // Network-task marker: the in-flight attempt was armed by Sync now, so it
  // must survive the ntpSyncEnabled=false gate and RF-gate aborts.
  bool ntpManualAttempt_ = false;
  bool ntpActivitySyncPending_ = false;
  uint8_t ntpFailoverIndex_ = 0;
  uint32_t ntpSyncStartedAtMs_ = 0;
  uint32_t staNtpEligibleAtMs_ = 0;
  uint32_t ntpSettingsGeneration_ = 1;
  uint32_t ntpAppliedSettingsGeneration_ = 0;
  char ntpServerBuffer_[NTP_SERVER_HOST_CAPACITY] = {};
  std::atomic<bool> ntpCallbackAccepting_{false};
  std::atomic<bool> ntpAbortRequested_{false};
  WebhookDispatcher webhooks_;
  WebhookConfig stagedWebhook_ = {};
  uint32_t stagedWebhookRequestId_ = 0;
  LineaMicraPersistedSettings stagedLineaMicra_ = {};
  uint32_t stagedLineaMicraRequestId_ = 0;
  uint32_t stagedPresetRequestId_ = 0;

  static void taskEntry(void *parameter);
  void taskLoop();
  void service();
  void serviceNtp(uint32_t now, bool staConnected);
  bool ntpMayArm(uint32_t now, bool staConnected) const;
  void abortNtpForRfGate();
  // Abort an in-flight attempt without counting a failure; an explicit Sync
  // now is requeued, auto attempts rearm when the gate clears.
  void abortInFlightNtp();
  void stopNtp();
  bool armNtp(uint32_t now, bool staConnected,
              uint32_t expectedGateGeneration);
  void handleNtpFailure(uint32_t now);
  static void ntpSyncNotificationCallback(struct timeval *tv);
  bool startNetwork();
  NetworkSettingsSnapshot networkSettingsSnapshot();
  void startStation(const NetworkSettingsSnapshot &settings, uint32_t now);
  void applyStationAddressConfig(const NetworkSettingsSnapshot &settings);
  bool beginStationConnect(const NetworkSettingsSnapshot &settings, uint32_t now);
  void applyWifiPowerSave(bool apStarting = false);
  // mDNS responder (network-task-only ownership). ensureMdns() starts the
  // component once Wi-Fi comes up; applyMdnsName() publishes the persisted
  // device name (no-op when unchanged); stopMdns() releases everything.
  bool ensureMdns();
  void applyMdnsName();
  void stopMdns();
  void clearStaLinkMetrics();
  bool brewRfActive() const;
  bool ensureAccessPoint(uint32_t now, bool force = false);
  void stopSoftApKeepStation();
  void stopSoftApLeaveHttp();
  void stopSoftAp(bool stopHttp);
  // Onboarding phase (OpenBrewByWeightNetworkSetup.h): network-task writer, read
  // under dataMux_; the teardown applies softApTeardownStep once STA is up.
  SetupPhase advanceSetupPhase(SetupEvent event);
  SetupPhase setupPhase();
  void serviceSoftApTeardown(const NetworkStatusSnapshot &status, uint32_t now);
  // Captive-portal DNS (wildcard A -> SoftAP IP) on UDP/53 while the SoftAP
  // is up. Network task owns the lifecycle; replies come from the AsyncUDP
  // callback inside the vendored component (processNextRequest is a stub).
  void startCaptivePortalDns();
  void stopCaptivePortalDns();
  DNSServer captiveDns_;
  bool captiveDnsActive_ = false;
  // Captive-portal exit ("skip"/"finish"): while set, OS connectivity probes
  // get their platform success response instead of the 302 to /setup, so the
  // OS closes its captive window as "logged in" and keeps the SoftAP link.
  // Set from the httpd task; a fresh captive DNS session resets it.
  std::atomic<bool> portalExitRequested_{false};
  // A committed onboarding switch to full mode restarts the controller once
  // the setup SoftAP stops.
  std::atomic<bool> setupRestartPending_{false};
  bool persistSetupIntent(bool bound, uint32_t staCrc);
  bool keepSetupBrew(SetupBrew brew, uint8_t presetId, bool commit);
  void requestSetupRestart();
  void clearSoftApIdleState();
  void armSoftApIdleDeadline(uint32_t now);
  void serviceSoftApIdle(uint32_t now, bool apActive, uint8_t clients,
                         bool activatorOn);
  bool wifiScanInProgress();
  void abortWifiScan(uint32_t now, bool logTimeout);
  void stopNetwork();
  bool startHttpServer();
  void stopHttpServer();
  void serviceStaState(uint32_t now);
  void serviceWifiScan(uint32_t now);
  void finishWifiScan(int16_t resultCount, uint32_t now);
  void processAcceptedCommands();
  void processAcceptedMaintenanceCommand(uint32_t now);
  bool processAcceptedCommand(const WebCommand &command);
  bool processPersistedCommand(const WebCommand &command);
  void publishConfiguredAddressStatus();
  void armPendingConfirmWindow(uint32_t now);
  void clearPendingConfirmWindow();
  void requestPendingNetworkConfirm();
  bool confirmPendingNetwork(const char *reason);
  bool revertPendingNetwork(uint32_t now, const char *reason);
  bool controlAllowsNetworkMutation();
  bool networkFlashWriteAllowed() const;
  ControlGateSnapshot controlGate() const;
  bool lockWorkBufForStatus();
  void loadControlStatus(ControlStatusSnapshot &control);
  void beginOwnedHeapLifecycle(HeapLifecycleTracker &tracker,
                               HeapLifecycleEvent event);
  void finishOwnedHeapLifecycle(HeapLifecycleTracker &tracker,
                                HeapLifecycleResult result);
  void log(DebugCategory category, DebugCode code, int32_t argument1 = 0,
           int32_t argument2 = 0);
  void actionLog(const char *message);
  void actionLogf(const char *fmt, ...);
  void lifecycleLog(const char *message);
  void lifecycleLogf(const char *fmt, ...);
  void refreshExtendedStatus(uint32_t now);
  bool handleCliNetworkAction(const WebCommand &command, uint32_t now);
  bool handleCliWifiAction(const WebCommand &command, uint32_t now);
  bool handleCliApAction(const WebCommand &command, uint32_t now);
  bool handleCliWebUiAction(const WebCommand &command, uint32_t now);
  void printActionSnapshot(const char *command, bool ok);
  void noteCliNetworkProgress();

  bool enqueueMaintenanceCompletion(const WebCommand &command,
                                    bool succeeded,
                                    CommandResultState failureState =
                                        CommandResultState::FAILED);
  void recordCommandResult(uint32_t requestId, CommandResultState state);
  esp_err_t sendAccepted(httpd_req_t *request, uint32_t requestId,
                         const char *extraJson = nullptr);
  uint32_t allocateRequestId();

  static esp_err_t rootHandler(httpd_req_t *request);
  static esp_err_t crashArchiveDownloadHandler(httpd_req_t *request);
  static esp_err_t crashArchiveClearHandler(httpd_req_t *request);
  static esp_err_t auditLogClearHandler(httpd_req_t *request);
  static esp_err_t auditLogExportHandler(httpd_req_t *request);
  static esp_err_t heapMapDownloadHandler(httpd_req_t *request);
  static esp_err_t auditVerboseHandler(httpd_req_t *request);
  static esp_err_t assetHandler(httpd_req_t *request);
  static esp_err_t notFoundHandler(httpd_req_t *request, httpd_err_code_t error);
  static esp_err_t claimHandler(httpd_req_t *request);
  static esp_err_t uiStreamHandshake(httpd_req_t *request);
  static esp_err_t uiStreamHandler(httpd_req_t *request);
  static void uiStreamFree(void *context);
  static void uiStreamDispatch(void *context);
  void serviceUiStream(uint32_t now);
  void sendUiStream(UiStreamSession &session, ControlStatusSnapshot &control,
                    const NetworkStatusSnapshot &network,
                    const ShotLogRecord &latest, bool eligible);
  bool sendShotCard(UiStreamSession &session, const ControlStatusSnapshot &control,
                    const ShotLogRecord &latest, bool eligible);
  bool sendHomeStream(UiStreamSession &session, const ControlStatusSnapshot &control,
                      const NetworkStatusSnapshot &network,
                      const ShotLogRecord *latest);
  bool sendDiagnosticStream(UiStreamSession &session,
                            const ControlStatusSnapshot &control,
                            const NetworkStatusSnapshot &network);
  // Audit fingerprinted frame inside the diagnostic stream: newest latch and
  // durable-window records plus the hunt-session state (own .inc fragment).
  bool sendAuditDiagFrame(UiStreamSession &session, bool initial);
  bool sendUiStreamFrame(UiStreamSession &session, size_t used, bool &deferArm);
  // Settings-only paths (own .inc fragment); rides the Home + shot-card batch.
  bool sendSettingsStream(UiStreamSession &session,
                          const ControlStatusSnapshot &control);
  bool sendLogStream(UiStreamSession &session,
                     const ControlStatusSnapshot &control);
  bool sendHistoryStream(UiStreamSession &session,
                         const ControlStatusSnapshot &control);
  bool sendStatsStream(UiStreamSession &session,
                       const ControlStatusSnapshot &control);
  // Shared stats row projection (scalars plus embedded curve fields) used by
  // the WS sender; writes into work.jsonItem and returns its length, 0 on
  // overflow.
  size_t formatShotStatsRow(NetworkWorkBuf &work, const ShotLogRecord &record,
                            const ShotCurveRecord *curve);
#if OPEN_BREW_BY_WEIGHT_DEVELOPMENT == 1
  // Unlock handlers are release-only: development builds serve public
  // administration and never compile the unlock endpoints.
#else
  static esp_err_t unlockHandler(httpd_req_t *request);
  static esp_err_t adminUnlockHandler(httpd_req_t *request);
  static esp_err_t adminLockHandler(httpd_req_t *request);
#endif
  static esp_err_t ownedApiHandler(httpd_req_t *request);
  static esp_err_t statusHandler(httpd_req_t *request);
  static esp_err_t debugExportHandler(httpd_req_t *request);
  static esp_err_t shotsClearHandler(httpd_req_t *request);
  static esp_err_t shotsDeleteHandler(httpd_req_t *request);
  static esp_err_t shotsRateHandler(httpd_req_t *request);
  static esp_err_t historyClearHandler(httpd_req_t *request);
  static esp_err_t historyDeleteHandler(httpd_req_t *request);
  static esp_err_t lastShotClearHandler(httpd_req_t *request);
  static esp_err_t timeSyncHandler(httpd_req_t *request);
  static esp_err_t timeZoneCatalogHandler(httpd_req_t *request);
  static esp_err_t timeZonePreviewHandler(httpd_req_t *request);
  static esp_err_t configHandler(httpd_req_t *request);
  static esp_err_t webhookHandler(httpd_req_t *request);
  static esp_err_t lineaMicraHandler(httpd_req_t *request);
  static esp_err_t integrationHandler(httpd_req_t *request);
  static esp_err_t bullseyeTestHandler(httpd_req_t *request);
  static esp_err_t preferredScaleClearHandler(httpd_req_t *request);
  static esp_err_t preferredScaleSelectHandler(httpd_req_t *request);
  static esp_err_t scaleFriendlyNameHandler(httpd_req_t *request);
  static esp_err_t presetsHandler(httpd_req_t *request);
  static esp_err_t resetCalibrationHandler(httpd_req_t *request);
  static esp_err_t resetGuardSamplesHandler(httpd_req_t *request);
  static esp_err_t paddleHandler(httpd_req_t *request);
  static esp_err_t rinseHandler(httpd_req_t *request);
  static esp_err_t stopHandler(httpd_req_t *request);
  static esp_err_t forcePulseHandler(httpd_req_t *request);
  static esp_err_t stateOverrideHandler(httpd_req_t *request);
  static esp_err_t restartHandler(httpd_req_t *request);
  static esp_err_t firmwareModeHandler(httpd_req_t *request);
  static esp_err_t clearResetHistoryHandler(httpd_req_t *request);
  static esp_err_t factoryResetHandler(httpd_req_t *request);
  static esp_err_t networkHandler(httpd_req_t *request);
  static esp_err_t wifiScanStartHandler(httpd_req_t *request);
  static esp_err_t wifiScanStatusHandler(httpd_req_t *request);
  static esp_err_t devicePasswordHandler(httpd_req_t *request);
  static esp_err_t bleScanHandler(httpd_req_t *request);
  static esp_err_t taskProfilerHandler(httpd_req_t *request);
  static esp_err_t loopMaxResetHandler(httpd_req_t *request);
  static esp_err_t scaleGapMaxResetHandler(httpd_req_t *request);
  static esp_err_t scaleProfileHandler(httpd_req_t *request);
  static esp_err_t scaleProfileDownloadHandler(httpd_req_t *request);
  static esp_err_t scaleTareHandler(httpd_req_t *request);
  // OTA routes authenticate with the device password instead of the
  // exclusive WebUI claim, so the command line client works without stealing
  // control from an open browser window.
  static esp_err_t otaStatusHandler(httpd_req_t *request);
  static esp_err_t otaSessionHandler(httpd_req_t *request);
  static esp_err_t otaUploadHandler(httpd_req_t *request);
  static esp_err_t otaPatchHandler(httpd_req_t *request);
  static esp_err_t otaFlashHandler(httpd_req_t *request);
  static esp_err_t otaAbortHandler(httpd_req_t *request);
  static esp_err_t otaSessionWork(httpd_req_t *request);
  static esp_err_t otaPatchWork(httpd_req_t *request);
  static esp_err_t otaFlashWork(httpd_req_t *request);
  static esp_err_t otaAbortWork(httpd_req_t *request);

  // OTA erase, range writes and commit run in FIFO order on a transient
  // "ota_worker" task with an httpd-sized internal stack, through ESP-IDF
  // async requests, so httpd keeps serving the Web UI stream and API reads.
  using OtaWork = esp_err_t (*)(httpd_req_t *);
  struct OtaJob {
    httpd_req_t *request = nullptr;
    OtaWork work = nullptr;
  };
  static constexpr UBaseType_t OTA_JOB_QUEUE_LENGTH = 4;
  esp_err_t runOnOtaWorker(httpd_req_t *request, OtaWork work);
  void runOtaJob(const OtaJob &job);
  void drainOtaJobs();
  static void otaWorkerTask(void *context);
  QueueHandle_t otaJobs_ = nullptr;
  std::atomic<bool> otaWorkerRunning_{false};
  std::atomic<bool> otaWorkerStopping_{false};
  // Accepted OTA requests not yet answered; httpd_stop waits for zero.
  std::atomic<uint8_t> otaJobsPending_{0};

  bool authorizeOtaRequest(httpd_req_t *request);
  esp_err_t sendOtaSnapshot(httpd_req_t *request, const char *httpStatus);
  void buildOtaJson(char *buffer, size_t capacity,
                    const ControlGateSnapshot &control, uint32_t bootId);
  void serviceOtaRollback(uint32_t now);
  static int otaReadChunk(void *context, uint8_t *buffer, size_t capacity);
  static bool otaTransferStillSafe(void *context);
  static void otaTransferProgress(void *context, uint32_t received,
                                  uint32_t expected);

  static esp_err_t sendJson(httpd_req_t *request, const char *status,
                            const char *json);
  static esp_err_t sendError(httpd_req_t *request, const char *status,
                             const char *error, const char *message);
  static bool readJsonBody(httpd_req_t *request,
                           char body[REQUEST_BODY_CAPACITY]);
  static bool requireJsonContentType(httpd_req_t *request);
  bool lockWorkBuf();
  void unlockWorkBuf();
  void unlockJsonBody();
  esp_err_t lockJsonBody(httpd_req_t *request, const char *invalidMessage);
  bool lockConfirmedBody(httpd_req_t *request, const char *invalidMessage,
                         const char *expectedConfirmation);
  // Shared destructive-action handler bodies: gate → confirm/parse → callback
  // → reply. Per-handler texts stay at the call sites; the response bytes are
  // identical to the previous inline implementations.
  esp_err_t runConfirmedClear(httpd_req_t *request, const char *lockedDetail,
                              const char *expectedConfirmation,
                              const char *notConfirmedCode,
                              const char *notConfirmedDetail,
                              bool (*clearFn)(), const char *failedCode,
                              const char *failedDetail);
  esp_err_t runDeleteById(httpd_req_t *request, const char *lockedDetail,
                          bool (*deleteFn)(uint32_t),
                          const char *invalidDetail, const char *notFoundCode,
                          const char *notFoundDetail);
  esp_err_t enqueueAcceptedCommand(httpd_req_t *request, WebCommandType type,
                                   bool unsafeWebUiOverride,
                                   const char *queueFullDetail,
                                   const char *extraJson = nullptr);
  // WiFi failure bookkeeping shared by the STA failure paths: bump variant
  // records a failure, schedules the retry backoff and (optionally) logs it;
  // the clear variant resets the counter and optionally publishes
  // DISCONNECTED. Log payloads and timing values are unchanged.
  void noteNetworkRetryBackoff(bool logRetry);
  void clearNetworkFailureStatus(bool staDisconnected);
  esp_err_t workBufBusy(httpd_req_t *request);
  bool requireActiveWebUiClient(httpd_req_t *request);
  void clearAdminUnlock();
  void grantAdminUnlock(const char *clientId, uint32_t now);
  void touchAdminUnlock();
  // True when the socket is served by the SoftAP interface address with a
  // peer on the SoftAP subnet (local-address match keeps a LAN that happens
  // to use 192.168.4.0/24 from inheriting the bypass).
  static bool socketServedBySoftAp(int socketFd);
  bool adminUnlockAllowed(httpd_req_t *request);
  bool requireAdminUnlock(httpd_req_t *request);
  // Device-password check shared by Admin unlock, Web UI sign-in and the
  // Home Assistant exchange and the OTA header: one cooldown per peer.
  enum class PasswordCheck : uint8_t { OK, WRONG, COOLDOWN };
  PasswordCheck checkDevicePassword(httpd_req_t *request, const char *password);
  static esp_err_t sendPasswordCheckError(httpd_req_t *request,
                                         PasswordCheck check);
  // Optional Web UI login (OpenBrewByWeightWebAccess.h). Effective only while
  // the device password is not the factory default.
  bool webAccessActive();
  // Joining the SoftAP already proved the device password (WPA2 passphrase).
  bool softApPeer(httpd_req_t *request);
  bool webAccessSessionAllowed(httpd_req_t *request);
  // everything: login off (sessions, Home Assistant, pairing). Otherwise
  // revoke Web UI sessions except `keep`; with no `keep` the claim drops too.
  void webAccessSignOut(const uint8_t *keep, bool everything);
  void keepPasswordChangeSession(httpd_req_t *request, uint32_t requestId);
  void serviceWebAccess();
  // Union of the login, access, pairing and integration request bodies.
  struct WebAccessBody {
    char password[WIFI_PASSWORD_CAPACITY] = {};
    char action[24] = {};
    char device[WEB_ACCESS_LABEL_BYTES * 2] = {};  // label, sanitized on store
    uint32_t id = 0;
    bool remember = false, allow = false, hasPassword = false;
    ~WebAccessBody() { memset(password, 0, sizeof(password)); }
  };
  bool readWebAccessBody(httpd_req_t *request, WebAccessBody &body,
                         esp_err_t &sent);
  static esp_err_t authStatusHandler(httpd_req_t *request);
  static esp_err_t authLoginHandler(httpd_req_t *request);
  static esp_err_t authLogoutHandler(httpd_req_t *request);
  static esp_err_t authAccessHandler(httpd_req_t *request);
  static esp_err_t authNoticeHandler(httpd_req_t *request);
  static esp_err_t authPairingHandler(httpd_req_t *request);
  bool integrationAccessRoute(httpd_req_t *request, esp_err_t &result);
  bool appendWebAccessStatus(httpd_req_t *request, size_t *used);
  bool diagnosticPageEnabled();
  bool webUiOverrideAllowed(httpd_req_t *request);
  uint32_t webUiOverrideRemainingMs(httpd_req_t *request);
  bool webUiConfigurationAllowed(httpd_req_t *request,
                                 const ControlGateSnapshot &status);
  bool historyMutationAllowed(httpd_req_t *request,
                              const ControlGateSnapshot &status);
  static const char *stateLabel(StopperState state);
  static const char *controlSourceName(ControlSource source);
  static const char *endReasonName(EndReason reason);
  static const char *staStateName(StaState state);
  static const char *wifiScanStateName(WifiScanState state);
};

}  // namespace openbrewbyweight

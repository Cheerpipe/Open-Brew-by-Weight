#include "OpenBrewByWeightNetwork.h"
#include "OpenBrewByWeightOutboundAdmission.h"
#include "OpenBrewByWeightBbwQuickStart.h"
#include "OpenBrewByWeightDebugExport.h"
#include "OpenBrewByWeightFirmwareMode.h"
#include "OpenBrewByWeightMachineMomentaryConfig.h"
#include "OpenBrewByWeightMachinePaddleConfig.h"
#include "OpenBrewByWeightBuildProfile.h"
#include "machine/OpenBrewByWeightLineaMicraIntegration.h"
#include "machine/OpenBrewByWeightMachineIntegration.h"
#include "machine/OpenBrewByWeightMicraTiming.h"
#include "OpenBrewByWeightJsonArena.h"
#include "OpenBrewByWeightShotPredictor.h"
#include "OpenBrewByWeightOta.h"
#include "OpenBrewByWeightPreferences.h"
#include "OpenBrewByWeightPowerManagement.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightRecovery.h"
#include "OpenBrewByWeightResetGuard.h"
#include "OpenBrewByWeightScheduledRestart.h"
#include "OpenBrewByWeightCrashArchive.h"
#include "OpenBrewByWeightSerialCli.h"
#include "OpenBrewByWeightScaleWorker.h"
#include "OpenBrewByWeightShotCurveTypes.h"
#include <OpenBrewByWeightVersion.h>
#include "OpenBrewByWeightWatchdog.h"
#include "OpenBrewByWeightWebAccessStore.h"

#include "OpenBrewByWeightWebAssetsGzip.h"

#include <Arduino.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <mdns.h>
#include "OpenBrewByWeightRfCoex.h"
#include <cmath>
#include <float.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "OpenBrewByWeightIntegrationRequest.h"

// The application logger is owned by openBrewByWeight.cpp. Network diagnostics use
// it so they reach both the ESP-IDF serial backend and the optional RAM ring.
void serialTraceCategory(openbrewbyweight::LogLevel level,
                         openbrewbyweight::DebugCategory category,
                         const char *message);
void serialTraceCategoryf(openbrewbyweight::LogLevel level,
                          openbrewbyweight::DebugCategory category,
                          const char *fmt, ...);

namespace openbrewbyweight {

WallClock g_wallClock;

// Id-keyed cache of per-row curve hashes. Curves are immutable once
// committed, so a hash stays valid until the record epoch moves; window
// changes (sort/dir/paging) re-fingerprint rows without re-fetching or
// re-hashing their curves.
struct StatsCurveHashCache {
  static constexpr size_t kCapacity = 512;  // power of two, > SHOT_LOG_CAPACITY
  uint32_t shotId[kCapacity] = {};
  uint32_t hash[kCapacity] = {};

  const uint32_t *find(uint32_t id) const {
    if (id == 0) return nullptr;
    const size_t seed = id & (kCapacity - 1);
    for (size_t probe = 0; probe < kCapacity; ++probe) {
      const size_t index = (seed + probe) & (kCapacity - 1);
      if (shotId[index] == id) return &hash[index];
      if (shotId[index] == 0) return nullptr;
    }
    return nullptr;
  }

  void insert(uint32_t id, uint32_t value) {
    if (id == 0) return;
    const size_t seed = id & (kCapacity - 1);
    for (size_t probe = 0; probe < kCapacity; ++probe) {
      const size_t index = (seed + probe) & (kCapacity - 1);
      if (shotId[index] == id || shotId[index] == 0) {
        shotId[index] = id;
        hash[index] = value;
        return;
      }
    }
    // Full table: drop the entry; that row re-fetches its curve next time.
  }
};

struct StatsStreamCache {
  ShotStatsRecordsSnapshot snapshot{};
  // Fetch scratch for the row curve currently being hashed or formatted, and
  // formatShotStatsRow's empty curve once a fetch found none.
  ShotCurveRecord fetchCurve{};
  StatsCurveHashCache curveHashes{};
  // Requested-order permutation over the newest-first records, compacted to
  // the filter's matches (`matched` of them): rows point into the untouched
  // cache, so sort/dir/filter toggles are pure index math and a reload
  // always restores the store order.
  uint16_t order[SHOT_LOG_CAPACITY] = {};
  bool valid = false, statsOk = false, hasMore = false;
  ShotLogSort sort = ShotLogSort::Date;
  ShotLogSortDir dir = ShotLogSortDir::Desc;
  size_t offset = 0, limit = 0, start = 0, pageCount = 0, matched = 0;
  uint32_t fingerprint = 0, filterHash = 0;
  // Header identity: store epoch, selected window, and the active preset of
  // its "This preset" chart.
  uint32_t headerEpoch = 0;
  size_t headerWindow = 0;
  uint8_t headerPresetId = 0;
  // The selected window's timed shots, newest first, and the density
  // chart's working values, off the dispatch stack.
  const ShotLogRecord *timed[SHOT_LOG_STATS_WINDOW_MAX] = {};
  const ShotLogRecord *scope[SHOT_LOG_STATS_WINDOW_MAX] = {};
  float mapped[SHOT_LOG_STATS_WINDOW_MAX] = {}, heights[121] = {};
  // Aggregate (~0.3 KB), two density charts (~1.2 KB), the consistency
  // series (~0.6 KB at 100 shots), and the filter facets (~0.6 KB typical).
  char header[5120]{};
};

struct NetworkWorkBuf {
  // Fits the diagnostic status with up to 20 task rows and 21 loop-phase
  // rows, plus the record-page UI section and debug-export chunks that
  // share this buffer.
  static constexpr size_t kStatusJson = 40960;
  // Worst case: 8 presets with fully escaped 23-char names plus every
  // per-preset rule field and shot statistic the Web UI renders.
  // Eight realistic items with the grind hint object: ~6.2 KB.
  static constexpr size_t kPresetsJson = 7168;
  static constexpr size_t kHistoryJson = 1400;
  // One full-capacity curve plus scalar fields per row; the 4 KiB headroom
  // (external memory) keeps a maximal row well under the client's frame
  // guard with room for future row fields and worst-case escaping.
  static constexpr size_t kJsonItem = SHOT_CURVE_JSON_CAPACITY + 4096;
  // Includes resumable-session identity (transfer id + SHA-256) as well as
  // two image tags. This buffer is in the shared external work area, never
  // used by the flash-writing path.
  static constexpr size_t kOtaJson = 1664;
  char statusJson[kStatusJson]{};
  char presetsJson[kPresetsJson]{};
  PresetShotStats presetStats[MAX_SHOT_PRESETS]{};
  char historyJson[kHistoryJson]{};
  // One-curve JSON scratch for the status page and the shots-list rows, kept
  // off the 8 KiB internal httpd stack like every other large JSON body.
  char curveJson[SHOT_CURVE_JSON_CAPACITY]{};
  // Exclusive handlers under the work-buffer mutex: record/CSV items, the
  // activation-history page, and OTA JSON are never live together.
  // Status/presets/history remain separate.
  union {
    char jsonItem[kJsonItem]{};
    char otaJson[kOtaJson];
    HistoryPage historyPage;
  };
  DebugEvent logBatch[kNetworkLogBatchSize]{};
  // Audit latch export batches: full-capacity copies stay bounded by the
  // latch ring itself (64 records), so one array serves every page.
  static constexpr size_t kAuditBatchSize = AUDIT_RECORD_CAPACITY;
  AuditRecord auditBatch[kAuditBatchSize]{};
  DebugLogReadMetadata logMetadata{};
  UiStreamFingerprints uiFingerprints[2]{};
  // Rollback copy of one session's Home, shot-card and Settings fingerprints
  // for the stream's batch (httpd only, under the work buffer).
  uint32_t batchHomeHashes[UiStreamFingerprints::kHomeFields]{};
  uint32_t batchCardHashes[UiStreamFingerprints::kCardFields +
                           UiStreamFingerprints::kCurveFields]{};
  uint32_t batchSettingsHashes[UiStreamFingerprints::kSettingsFields]{};
  StatsStreamCache statsCache{};
  ShotCurveRecord homeCurve{};
  ControlStatusSnapshot control{};
  TaskProfilerSnapshot taskProfiler{};
  DebugExportExtras debugExport{};
  ShotPresetBank presetBank{};
  // Home stream's last slim-presets render, compared to skip rebuilds.
  ShotPresetBank renderedPresets{};
  PresetShotStats renderedStats[MAX_SHOT_PRESETS]{};
  uint32_t renderedPresetsHash = 0;
  uint32_t renderedShotLogEpoch = 0;
  // presetsJson still holds the Home render; other handlers clear it.
  bool presetsJsonHome = false;
  // Integration snapshot pieces, kept off the internal httpd stack.
  RecipeSnapshot recipe{};
  char integrationLastShot[768]{};
  char integrationQuickSettings[384]{};
  char integrationStats[384]{};
  char integrationLastActivation[192]{};
  ScaleHistoryEntry scaleHistory[SCALE_HISTORY_CAPACITY]{};
  BullseyeMelodyConfig bullseyeMelody{};
  char safeBullseyeRtttl[BULLSEYE_RTTTL_CAPACITY]{};
  // Must match OpenBrewByWeightNetwork::REQUEST_BODY_CAPACITY (asserted in begin()).
  char requestBody[2048]{};
  WifiScanSnapshot wifiScan{};
};
static_assert(sizeof(NetworkWorkBuf) <= 655360,
              "Network workspace exceeds its external-memory budget");

// Wi-Fi scan snapshots. Network task / httpd only; not BLE.
// AP records come from Arduino's SCAN_DONE cache (getScanInfoByIndex).
static OPEN_BREW_BY_WEIGHT_PSRAM_BSS WifiScanSnapshot g_wifiScan;
static OPEN_BREW_BY_WEIGHT_PSRAM_BSS WifiScanSnapshot g_wifiScanWorking;
static OPEN_BREW_BY_WEIGHT_PSRAM_BSS PersistedSettings g_networkSettings;
// Web UI login state; dataMux_ guards it and the network task persists it.
static OPEN_BREW_BY_WEIGHT_PSRAM_BSS WebAccessState g_webAccess;

namespace {

NetworkWorkBuf *g_work = nullptr;

void quietIdfWifiDriverWarnings() {
  // Home APs often advertise 802.11r (FT-PSK). STA leaves FT off on purpose:
  // the machine does not roam, and enabling 11r has broken IoT joins on some
  // mesh/UniFi setups. The closed-source driver then WARNs on every matching
  // BSS and falls back to WPA2-PSK. Association still succeeds; Open Brew by
  // Weight logs join/fail itself. Drop the wifi tag to ERROR so that expected
  // fallback does not flood USB serial. Other IDF tags stay at WARN.
  esp_log_level_set("wifi", ESP_LOG_ERROR);
}

LogLevel networkTextLogLevel(const char *message) {
  if (message == nullptr) {
    return LogLevel::INFO;
  }
  if (strncmp(message, "ERR ", 4) == 0) {
    return LogLevel::ERROR;
  }
  if (strstr(message, " failed") != nullptr ||
      strstr(message, " mismatch") != nullptr ||
      strstr(message, "timed out") != nullptr ||
      strstr(message, "rollback impossible") != nullptr ||
      strstr(message, "rolling back") != nullptr) {
    return LogLevel::WARNING;
  }
  return LogLevel::INFO;
}

void formatWifiMac(const uint8_t mac[6], char *output, size_t outputCapacity) {
  if (mac == nullptr || output == nullptr || outputCapacity < 18) {
    return;
  }
  snprintf(output, outputCapacity, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void fillSoftApSsid(char *out, size_t cap) {
  uint8_t mac[6] = {};
  if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK &&
      formatSoftApSsid(out, cap, mac)) {
    return;
  }
  if (out == nullptr || cap == 0) {
    return;
  }
  snprintf(out, cap, "%s", SOFT_AP_SSID_PREFIX);
}

const char *jsonParseFailureMessage(const char *fallback) {
  if (jsonArenaExhaustedRecently()) {
    return "JSON too large for device buffer";
  }
  return fallback;
}

constexpr const char *AP_IP = "192.168.4.1";
constexpr const char CAPTIVE_PORTAL_URL[] = "http://192.168.4.1/setup";
constexpr const char *JSON_CONTENT_TYPE = "application/json";
constexpr const char *STATUS_OK = "200 OK";
constexpr const char *STATUS_NOT_MODIFIED = "304 Not Modified";
constexpr const char *STATUS_ACCEPTED = "202 Accepted";
constexpr const char *STATUS_ALREADY_REPORTED = "208 Already Reported";
constexpr size_t IF_NONE_MATCH_CAPACITY = 80;
constexpr const char *STATUS_BAD_REQUEST = "400 Bad Request";
constexpr const char *STATUS_UNAUTHORIZED = "401 Unauthorized";
constexpr const char *STATUS_FORBIDDEN = "403 Forbidden";
constexpr const char *STATUS_TOO_LARGE = "413 Content Too Large";
constexpr const char *STATUS_SERVER_ERROR = "500 Internal Server Error";
constexpr const char *DEVICE_PASSWORD_HEADER = "X-Device-Password";
constexpr const char *OTA_ALLOW_DOWNGRADE_HEADER = "X-OTA-Allow-Downgrade";
constexpr const char *OTA_TRANSFER_HEADER = "X-OTA-Transfer";
constexpr const char *OTA_OFFSET_HEADER = "X-OTA-Offset";
constexpr const char *OTA_LENGTH_HEADER = "X-OTA-Length";
constexpr const char *STATUS_TOO_MANY = "429 Too Many Requests";
constexpr const char *STATUS_CONFLICT = "409 Conflict";
constexpr const char *STATUS_NOT_FOUND = "404 Not Found";
constexpr const char *STATUS_UNPROCESSABLE = "422 Unprocessable Entity";
constexpr const char *STATUS_UNAVAILABLE = "503 Service Unavailable";
// PersistedSettings is ≤ PERSISTED_SETTINGS_NVS_BUDGET (3328 B).
// processPersistedCommand() keeps one copy on the stack via settingsCopy();
// NVS dual-slot scratch is shared off-stack. 7 168 was too small (canary on
// FACTORY_RESET). 10 240 keeps headroom without the old 12 KiB margin.
// Keep this stack internal: flash writes disable both caches (flash and
// PSRAM mappings) while they run, so a PSRAM stack could not perform them.
constexpr uint32_t NETWORK_MANAGER_TASK_STACK_SIZE = 10240;
// POST JSON bodies live in NetworkWorkBuf (PSRAM), so the httpd worker no
// longer needs a 2 KiB request-body frame on top of headers and send buffers.
// 11 264 after status polling left only 1 540 B free on 10 240 (2026-09-26
// target capture), 4 B above the 1 536 B release gate.
// Stack stays internal: OTA flash writes run on this task.
constexpr uint32_t HTTP_SERVER_TASK_STACK_SIZE = 11264;

bool jsonFieldPresent(cJSON *object, const char *name) {
  return object != nullptr && name != nullptr &&
         cJSON_GetObjectItemCaseSensitive(object, name) != nullptr;
}

bool jsonBoolean(cJSON *object, const char *name, bool &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsBool(item)) {
    return false;
  }
  output = cJSON_IsTrue(item);
  return true;
}

bool jsonUint32(cJSON *object, const char *name, uint32_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      item->valuedouble < 0.0 || item->valuedouble > UINT32_MAX ||
      floor(item->valuedouble) != item->valuedouble) {
    return false;
  }
  output = static_cast<uint32_t>(item->valuedouble);
  return true;
}

bool jsonUint8(cJSON *object, const char *name, uint8_t &output) {
  uint32_t value = 0;
  if (!jsonUint32(object, name, value) || value > UINT8_MAX) {
    return false;
  }
  output = static_cast<uint8_t>(value);
  return true;
}

bool jsonFloat(cJSON *object, const char *name, float &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      item->valuedouble > static_cast<double>(FLT_MAX) ||
      item->valuedouble < -static_cast<double>(FLT_MAX)) {
    return false;
  }
  output = static_cast<float>(item->valuedouble);
  return true;
}

// Target weight arrives in grams with tenths (36.5); stored as decigrams.
bool jsonGoalWeightDg(cJSON *object, const char *name, uint16_t &output) {
  float grams = 0.0f;
  if (!jsonFloat(object, name, grams)) {
    return false;
  }
  const float clipped = fmaxf(0.1f * MIN_GOAL_WEIGHT_DG,
                              fminf(0.1f * MAX_GOAL_WEIGHT_DG, grams));
  output = static_cast<uint16_t>(clipped * 10.0f + 0.5f);
  return true;
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION == OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD
bool jsonInt16(cJSON *object, const char *name, int16_t &output) {
  if (object == nullptr || name == nullptr) {
    return false;
  }
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsNumber(item) || item->valuedouble != item->valueint) {
    return false;
  }
  const long value = item->valueint;
  if (value < INT16_MIN || value > INT16_MAX) {
    return false;
  }
  output = static_cast<int16_t>(value);
  return true;
}
#endif

// Compared in constant time so a wrong secret cannot be recovered one byte at a
// time by measuring how long the rejection takes.
bool secretsMatch(const char *candidate, const char *expected) {
  const size_t candidateLength = strnlen(candidate, WIFI_PASSWORD_CAPACITY);
  const size_t expectedLength = strnlen(expected, WIFI_PASSWORD_CAPACITY);
  uint32_t difference =
      static_cast<uint32_t>(candidateLength ^ expectedLength);
  const size_t span =
      candidateLength < expectedLength ? candidateLength : expectedLength;
  for (size_t index = 0; index < span; ++index) {
    difference |= static_cast<uint32_t>(
        static_cast<uint8_t>(candidate[index]) ^
        static_cast<uint8_t>(expected[index]));
  }
  return difference == 0 && expectedLength > 0;
}

bool jsonString(cJSON *object, const char *name, char *output,
                size_t outputCapacity, bool allowEmpty) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  const size_t length = strlen(item->valuestring);
  if ((!allowEmpty && length == 0) || length >= outputCapacity) {
    return false;
  }
  memset(output, 0, outputCapacity);
  memcpy(output, item->valuestring, length);
  return true;
}

bool jsonHasOnlyUniqueFields(cJSON *object, const char *const *allowed,
                             size_t allowedCount) {
  // Two fixed words cover the broad config patch without heap allocation.
  // Keep the ceiling explicit so callers fail loudly if they outgrow it.
  constexpr size_t kSeenWords = 2;
  if (!cJSON_IsObject(object) || allowed == nullptr ||
      allowedCount > kSeenWords * 64U) {
    return false;
  }
  uint64_t seen[kSeenWords] = {};
  for (cJSON *item = object->child; item != nullptr; item = item->next) {
    if (item->string == nullptr) {
      return false;
    }
    size_t index = 0;
    while (index < allowedCount && strcmp(item->string, allowed[index]) != 0) {
      ++index;
    }
    if (index == allowedCount) {
      return false;
    }
    const size_t word = index / 64U;
    const uint64_t bit = uint64_t{1} << (index % 64U);
    if ((seen[word] & bit) != 0) {
      return false;
    }
    seen[word] |= bit;
  }
  return true;
}

const char *configValidationMessage(ConfigValidationError error) {
  switch (error) {
    case ConfigValidationError::NONE:
      return "Configuration is valid.";
    case ConfigValidationError::GOAL_WEIGHT:
      return "Target must be an integer from 10 to 200 g.";
    case ConfigValidationError::WEIGHT_OFFSET:
      return "Internal offset is invalid; it cannot be edited from the Web UI.";
    case ConfigValidationError::RINSE_GESTURE:
      return "Rinse gesture must be from 0.1 to 5 s.";
    case ConfigValidationError::RINSE_DURATION:
      return "Rinse duration must be from 0.5 to 10 s.";
    case ConfigValidationError::RETARE_WINDOW:
      return "Retare window must be from 0.5 to 10 s.";
    case ConfigValidationError::MINIMUM_CUP_WEIGHT:
      return "Minimum cup weight must be from 1 to 500 g.";
    case ConfigValidationError::RETARE_STABILITY_SAMPLES:
      return "Retare stable samples must be from 2 to 10.";
    case ConfigValidationError::RETARE_STABILITY_TOLERANCE:
      return "Retare stability tolerance must be from 0.1 to 20 g.";
    case ConfigValidationError::RETARE_STABILITY_MAX_GAP:
      return "Retare sample gap must be from 0.1 to 5 s.";
    case ConfigValidationError::RETARE_STABILITY_MIN_DURATION:
      return "Retare min stable time must be from 0 to 2 s.";
    case ConfigValidationError::RETARE_STABILITY_RELATION:
      return "Retare min stable time must be ≤ retare window and ≤ "
             "samples × sample gap.";
    case ConfigValidationError::BBW_PROTECTION_TIMEOUT:
      return "Earliest weight stop must be from 0.5 to 30 s.";
    case ConfigValidationError::BBW_PROTECTION_RETARE_RELATION:
      return "Earliest weight stop must be at least the effective retare "
             "window + 3 s.";
    case ConfigValidationError::OPERATIONAL_WALL:
      return "Max shot time must be from 5 to 60 s.";
    case ConfigValidationError::PADDLE_REMINDER_INTERVAL:
      return "Paddle reminder interval must be from 5 to 60 s.";
    case ConfigValidationError::PADDLE_REMINDER_MAX_DURATION:
      return "Paddle reminder limit must be from 1 to 60 min and at least "
             "the reminder interval.";
    case ConfigValidationError::TIMING_RELATION:
      return "Required: rinse gesture < max shot time; rinse duration, retare "
             "window, and earliest weight stop each ≤ max shot time.";
    case ConfigValidationError::COMBINED_TARE_REQUIRES_AUTOTARE:
      return "The Bookoo combined command requires automatic tare.";
    case ConfigValidationError::POST_TARE_BASELINE_GRACE:
      return "Post-tare grace must be from 0.5 to 10 s.";
    case ConfigValidationError::SCALE_TIMER_STOP_EXTRA_DELAY:
      return "Scale timer stop extra delay must be from 0 to 1000 ms.";
    case ConfigValidationError::TIMEZONE_ID:
      return "Choose a supported IANA time zone.";
    case ConfigValidationError::NTP_SERVER_PRESET:
      return "NTP server preset must be pool, google, cloudflare, or nist.";
    case ConfigValidationError::NTP_SERVER_CUSTOM:
      return "Custom NTP hostname is invalid (letters, digits, '.', '-'; "
             "max 63 chars).";
    case ConfigValidationError::FAST_MARGIN:
      return "Extra yield for fast shots must be 0.5 to 10 g, at most a quarter "
             "of the target, and keep the target plus extra within 200 g.";
    case ConfigValidationError::FAST_BEFORE:
      return "The shot time window must start at least 2 s after the earliest "
             "weight stop, end at least 1 s later, and use 0.5 s steps.";
    case ConfigValidationError::SLOW_MARGIN:
      return "Less yield for slow shots must be 0.5 to 10 g and leave at least 5 g.";
    case ConfigValidationError::SLOW_AFTER:
      return "The shot time window must end at least 1 s before the max shot "
             "time, in 0.5 s steps.";
    case ConfigValidationError::SCALE_LOSS_FIXED:
      return "The scale-loss stop time must be empty or between the earliest "
             "weight stop and the max shot time.";
    case ConfigValidationError::WEIGHT_OFFSET_BASELINE:
      return "Offset baseline must be from 0 to 5.0 g.";
    case ConfigValidationError::SCALE_MAC_CACHE_MODE:
      return "Scale preference must be first, prefer, or only.";
    case ConfigValidationError::ALERT_OUTPUT_CHANNEL:
      return "Alert output channel must be scale_only, buzzer_only, or "
             "scale_priority.";
    case ConfigValidationError::EXTENDED_PULSE_RATE:
      return "Fast-shot pulse must be disabled, slow, medium, fast, or "
             "rapid.";
    case ConfigValidationError::SLOW_EXTENDED_PULSE_RATE:
      return "Slow-shot pulse must be disabled, slow, medium, fast, or "
             "rapid.";
    case ConfigValidationError::BOOKOO_CONNECT_BEEP_LEVEL:
      return "Bookoo scale volume must be disabled (0) or 1 to 5.";
    case ConfigValidationError::NO_SCALE_BBW_MODE:
      return "No-scale BBW mode must be off, warn_once, or require_scale.";
    case ConfigValidationError::LAST_SHOT_COOLDOWN:
      return "Last shot cooldown must be from 5 to 240 min.";
    case ConfigValidationError::DRIP_DELAY:
      return "Drip delay must be from 0 to 10 s.";
    case ConfigValidationError::SERIAL_LOG_LEVEL:
      return "Serial log level must be none, critical, error, warning, info, "
             "or debug.";
    case ConfigValidationError::RING_RETAIN_LOG_LEVEL:
      return "Ring log level must be none, critical, error, warning, info, or "
             "debug.";
    case ConfigValidationError::PADDLE_MODE:
      return "Paddle mode must be auto, natural or original.";
    case ConfigValidationError::CUP_REMOVED_WEIGHT:
      return "Cup-removed threshold must be from -50 to -0.1 g.";
    case ConfigValidationError::STOP_PULSE:
      return "Auto-stop pulse must be from 50 to 1000 ms.";
    case ConfigValidationError::MAX_SINGLE_PRESS:
      return "Single-press limit must be from 100 to 5000 ms.";
    case ConfigValidationError::REED_CONFIRM_TIMEOUT:
      return "Reed confirm timeout must be from 0.2 to 5 s.";
    case ConfigValidationError::SHOT_REACT_TIMEOUT:
      return "Shot reaction timeout must be 0 (compiled default) or from 3 to 30 s.";
    case ConfigValidationError::SCHEDULED_RESTART:
      return "Scheduled restart time must be from 00:00 to 23:59.";
  }
  return "Invalid configuration.";
}

// Recipe fields arrive in grams and seconds with tenths and are stored as
// decigrams and milliseconds; range checks stay in the recipe validator.
bool jsonTenths(cJSON *object, const char *name, uint32_t limitTenths,
                uint32_t &tenths) {
  float value = 0.0f;
  if (!jsonFloat(object, name, value) || value < 0.0f ||
      value * 10.0f > static_cast<float>(limitTenths)) {
    return false;
  }
  tenths = static_cast<uint32_t>(lroundf(value * 10.0f));
  return true;
}

bool jsonGramsDg(cJSON *object, const char *name, uint16_t &output) {
  uint32_t tenths = 0;
  if (!jsonTenths(object, name, UINT16_MAX, tenths)) return false;
  output = static_cast<uint16_t>(tenths);
  return true;
}

bool jsonSecondsMs(cJSON *object, const char *name, uint32_t &output) {
  uint32_t tenths = 0;
  if (!jsonTenths(object, name, UINT32_MAX / 100, tenths)) return false;
  output = tenths * 100;
  return true;
}

bool jsonNoScaleBbwMode(cJSON *object, const char *name, uint8_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  if (strcmp(item->valuestring, "off") == 0) {
    output = static_cast<uint8_t>(NoScaleBbwMode::OFF);
    return true;
  }
  if (strcmp(item->valuestring, "warn_once") == 0) {
    output = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
    return true;
  }
  if (strcmp(item->valuestring, "require_scale") == 0) {
    output = static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
    return true;
  }
  return false;
}

bool jsonPaddleMode(cJSON *object, const char *name, uint8_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  return parsePaddleMode(item->valuestring, output);
}

bool jsonStopPulseMs(cJSON *object, RuntimeConfig &config) {
  uint32_t ms = 0;
  if (!jsonUint32(object, "stopPulseMs", ms) || ms < 50 || ms > 1000) {
    return false;
  }
  setRuntimeStopPulseMs(config, ms);
  return true;
}

bool jsonMaxSinglePressMs(cJSON *object, RuntimeConfig &config) {
  uint32_t ms = 0;
  if (!jsonUint32(object, "maxSinglePressMs", ms) || ms < 100 || ms > 5000) {
    return false;
  }
  setRuntimeMaxSinglePressMs(config, ms);
  return true;
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
bool jsonReedConfirmTimeoutMs(cJSON *object, RuntimeConfig &config) {
  uint32_t ms = 0;
  if (!jsonUint32(object, "reedConfirmTimeoutMs", ms) || ms < 200 ||
      ms > 5000) {
    return false;
  }
  setRuntimeReedConfirmTimeoutMs(config, ms);
  return true;
}
#endif

bool jsonAssumeIdleWhenScaleConnects(cJSON *object, RuntimeConfig &config) {
  return jsonBoolean(object, "assumeIdleWhenScaleConnects",
                     config.assumeIdleWhenScaleConnects);
}

bool jsonShotReactTimeoutS(cJSON *object, RuntimeConfig &config) {
  uint8_t seconds = 0;
  if (!jsonUint8(object, "shotReactTimeoutS", seconds)) {
    return false;
  }
  if (seconds != 0 && (seconds < MIN_SHOT_REACT_TIMEOUT_S ||
                       seconds > MAX_SHOT_REACT_TIMEOUT_S)) {
    return false;
  }
  config.shotReactTimeoutS = seconds;
  return true;
}

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE != 0
bool jsonMomentaryStartEdge(cJSON *object, RuntimeConfig &config) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, "momentaryStartEdge");
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  return parseMomentaryStartEdge(item->valuestring, config.momentaryStartOnPress);
}
#endif

bool jsonNtpPreset(cJSON *object, const char *name, uint8_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  if (strcmp(item->valuestring, "pool") == 0) {
    output = static_cast<uint8_t>(NtpServerPreset::POOL);
    return true;
  }
  if (strcmp(item->valuestring, "google") == 0) {
    output = static_cast<uint8_t>(NtpServerPreset::GOOGLE);
    return true;
  }
  if (strcmp(item->valuestring, "cloudflare") == 0) {
    output = static_cast<uint8_t>(NtpServerPreset::CLOUDFLARE);
    return true;
  }
  if (strcmp(item->valuestring, "nist") == 0) {
    output = static_cast<uint8_t>(NtpServerPreset::NIST);
    return true;
  }
  return false;
}

bool jsonScaleMacCacheMode(cJSON *object, const char *name, uint8_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  return parseScaleMacCacheMode(item->valuestring, output);
}

bool jsonAlertOutputChannel(cJSON *object, const char *name, uint8_t &output,
                            bool optional) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (item == nullptr) {
    return optional;
  }
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  return parseAlertOutputChannel(item->valuestring, output);
}

bool jsonLogLevel(cJSON *object, const char *name, uint8_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  return parseLogLevel(item->valuestring, output);
}

bool jsonExtendedPulseRate(cJSON *object, const char *name, uint8_t &output) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  return parseExtendedPulseRate(item->valuestring, output);
}

void formatIp(const IPAddress &ip, char output[16]) {
  const uint8_t octets[4] = {ip[0], ip[1], ip[2], ip[3]};
  formatIpv4(octets, output);
}

// Every handler shares the one httpd task, so a slow one delays the Web UI
// stream, Home Assistant and OTA polls. Report those in the Web UI log, with
// the path only: query strings are never logged. CPU time (esp_timer run-time
// stats, accrued at context switches) separates compute from waits on the
// network, locks, or preemption.
constexpr uint32_t SLOW_HTTP_HANDLER_MS = 500;

uint32_t httpdCpuUs() {
  TaskStatus_t status;
  vTaskGetInfo(nullptr, &status, pdFALSE, eRunning);
  return static_cast<uint32_t>(status.ulRunTimeCounter);
}

esp_err_t timedHandler(httpd_req_t *request) {
  const uint32_t startedAtMs = millis();
  const uint32_t startedCpuUs = httpdCpuUs();
  const esp_err_t result =
      OpenBrewByWeightNetwork::rejectForeignHost(request)
          ? ESP_OK
          : reinterpret_cast<esp_err_t (*)(httpd_req_t *)>(request->user_ctx)(request);
  const uint32_t elapsedMs = millis() - startedAtMs;
  if (elapsedMs >= SLOW_HTTP_HANDLER_MS) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "http: %s %.*s held the web server %lu ms (%lu ms CPU)",
                         http_method_str(static_cast<http_method>(request->method)),
                         static_cast<int>(strcspn(request->uri, "?")), request->uri,
                         static_cast<unsigned long>(elapsedMs),
                         static_cast<unsigned long>((httpdCpuUs() - startedCpuUs) / 1000U));
  }
  return result;
}

bool registerHandler(httpd_handle_t server, const char *uri,
                     httpd_method_t method, esp_err_t (*handler)(httpd_req_t *),
                     bool websocket = false,
                     esp_err_t (*handshake)(httpd_req_t *) = nullptr) {
  httpd_uri_t descriptor = {};
  descriptor.uri = uri;
  descriptor.method = method;
  descriptor.handler = timedHandler;
  descriptor.user_ctx = reinterpret_cast<void *>(handler);
  descriptor.is_websocket = websocket;
  descriptor.ws_pre_handshake_cb = handshake;
  return httpd_register_uri_handler(server, &descriptor) == ESP_OK;
}

// HTTPD's socket send copies source bytes into lwIP; callers retain ownership
// of PSRAM and embedded-asset data until this synchronous call returns.
esp_err_t sendChunk(httpd_req_t *request, const void *data, size_t length) {
  return httpd_resp_send_chunk(request, static_cast<const char *>(data), length);
}

esp_err_t sendBody(httpd_req_t *request, const void *data, size_t length) {
  if (length == 0) {
    return httpd_resp_send(request, nullptr, 0);
  }
  return httpd_resp_send(request, static_cast<const char *>(data), length);
}

esp_err_t sendJsonStringChunk(httpd_req_t *request, const char *value) {
  if (value == nullptr) {
    value = "";
  }
  if (httpd_resp_send_chunk(request, "\"", 1) != ESP_OK) {
    return ESP_FAIL;
  }
  const unsigned char *cursor =
      reinterpret_cast<const unsigned char *>(value);
  const unsigned char *segment = cursor;
  while (*cursor != '\0') {
    const char *escape = nullptr;
    char unicodeEscape[7] = {};
    switch (*cursor) {
      case '\"': escape = "\\\""; break;
      case '\\': escape = "\\\\"; break;
      case '\b': escape = "\\b"; break;
      case '\f': escape = "\\f"; break;
      case '\n': escape = "\\n"; break;
      case '\r': escape = "\\r"; break;
      case '\t': escape = "\\t"; break;
      default:
        if (*cursor < 0x20) {
          snprintf(unicodeEscape, sizeof(unicodeEscape), "\\u%04x", *cursor);
          escape = unicodeEscape;
        }
        break;
    }
    if (escape != nullptr) {
      if (cursor > segment &&
          sendChunk(request, segment,
                          static_cast<size_t>(cursor - segment)) != ESP_OK) {
        return ESP_FAIL;
      }
      if (sendChunk(request, escape, strlen(escape)) != ESP_OK) {
        return ESP_FAIL;
      }
      segment = cursor + 1;
    }
    ++cursor;
  }
  if (cursor > segment &&
      sendChunk(request, segment,
                      static_cast<size_t>(cursor - segment)) != ESP_OK) {
    return ESP_FAIL;
  }
  return httpd_resp_send_chunk(request, "\"", 1);
}

void sanitizeJsonEmbed(const char *input, char *output, size_t capacity) {
  if (capacity == 0) {
    return;
  }
  size_t written = 0;
  for (size_t index = 0; input != nullptr && input[index] != '\0' &&
                          written + 1 < capacity;
       ++index) {
    const unsigned char byte = static_cast<unsigned char>(input[index]);
    if (byte == '\"' || byte == '\\' || byte < 0x20) {
      continue;
    }
    output[written++] = static_cast<char>(byte);
  }
  output[written] = '\0';
}

// The one REST status page left: Admin, which the Setup portal also reads.
bool statusPageIsAdmin(const char *uri) {
  static constexpr char kPath[] = "/api/v1/status/admin";
  if (uri == nullptr) return false;
  const char *query = strchr(uri, '?');
  const size_t length = query != nullptr ? static_cast<size_t>(query - uri) : strlen(uri);
  return length == sizeof(kPath) - 1 && strncmp(uri, kPath, length) == 0;
}

bool statusJsonAppend(size_t *used, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

bool __attribute__((format(printf, 2, 3)))
statusJsonAppend(size_t *used, const char *fmt, ...) {
  if (g_work == nullptr || used == nullptr ||
      *used >= NetworkWorkBuf::kStatusJson) {
    return false;
  }
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(g_work->statusJson + *used,
                          NetworkWorkBuf::kStatusJson - *used, fmt, args);
  va_end(args);
  if (n < 0 ||
      static_cast<size_t>(n) >= NetworkWorkBuf::kStatusJson - *used) {
    return false;
  }
  *used += static_cast<size_t>(n);
  return true;
}


bool jsonScratchAppend(char *buf, size_t cap, size_t *used, const char *fmt,
                       ...) {
  if (buf == nullptr || used == nullptr || *used >= cap) {
    return false;
  }
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf + *used, cap - *used, fmt, args);
  va_end(args);
  if (n < 0 || static_cast<size_t>(n) >= cap - *used) {
    return false;
  }
  *used += static_cast<size_t>(n);
  return true;
}

bool formatTaskProfilerObject(char *buf, size_t cap, size_t *used,
                              const TaskProfilerSnapshot &tasks, bool full = true) {
  if (!jsonScratchAppend(
          buf, cap, used,
          "{\"state\":\"%s\",\"stopReason\":\"%s\",\"elapsedMs\":%lu,"
          "\"currentTotalCpuPct\":%.1f,"
          "\"unreportedCurrentCpuPct\":%.1f,\"unreportedAverageCpuPct\":%.1f,"
          "\"truncated\":%s,"
          "\"peakGapMs\":%lu,\"recentGapMs\":%lu,"
          "\"peakGapUs\":%lu,\"recentGapUs\":%lu,"
          "\"peakDelayUs\":%lu,\"recentDelayUs\":%lu,"
          "\"peakDispatchUs\":%lu,\"recentDispatchUs\":%lu",
          taskProfilerStateName(tasks.state),
          taskProfilerStopReasonName(tasks.stopReason),
          static_cast<unsigned long>(tasks.elapsedMs),
          static_cast<double>(tasks.currentTotalCpuPct),
          static_cast<double>(tasks.unreportedCurrentCpuPct),
          static_cast<double>(tasks.unreportedAverageCpuPct),
          tasks.truncated ? "true" : "false",
          static_cast<unsigned long>(tasks.loopPhases.peakGapMs),
          static_cast<unsigned long>(tasks.loopPhases.recentGapMs),
          static_cast<unsigned long>(tasks.loopPhases.peakGapUs),
          static_cast<unsigned long>(tasks.loopPhases.recentGapUs),
          static_cast<unsigned long>(tasks.loopPhases.peakDelayUs),
          static_cast<unsigned long>(tasks.loopPhases.recentDelayUs),
          static_cast<unsigned long>(tasks.loopPhases.peakDispatchUs),
          static_cast<unsigned long>(tasks.loopPhases.recentDispatchUs))) {
    return false;
  }
  // Detailed measurements remain available in the full debug export.
  if (full && !jsonScratchAppend(buf, cap, used,
          ",\"peakGapCpuUs\":%lu,\"peakGapIdleUs\":%lu,\"peakGapAtMs\":%lu,"
          "\"stackUnit\":\"bytes\",\"remainingMs\":%lu,\"sampleCount\":%lu,"
          "\"averageTotalCpuPct\":%.1f,\"lastCaptureUs\":%lu,\"maxCaptureUs\":%lu,"
          "\"lockWaits\":{\"micraBackflush\":{\"lastUs\":%lu,\"maxUs\":%lu,"
          "\"holdLastUs\":%lu,\"holdMaxUs\":%lu},"
          "\"scaleEvents\":{\"lastUs\":%lu,\"maxUs\":%lu},"
          "\"shotStore\":{\"lastUs\":%lu,\"maxUs\":%lu},"
          "\"controlStatus\":{\"lastUs\":%lu,\"maxUs\":%lu}}",
          static_cast<unsigned long>(tasks.loopPhases.peakGapCpuUs),
          static_cast<unsigned long>(tasks.loopPhases.peakGapIdleUs),
          static_cast<unsigned long>(tasks.loopPhases.peakGapAtMs),
          static_cast<unsigned long>(tasks.remainingMs),
          static_cast<unsigned long>(tasks.sampleCount),
          static_cast<double>(tasks.averageTotalCpuPct),
          static_cast<unsigned long>(tasks.lastCaptureUs),
          static_cast<unsigned long>(tasks.maxCaptureUs),
          static_cast<unsigned long>(tasks.micraBackflushWaitLastUs),
          static_cast<unsigned long>(tasks.micraBackflushWaitMaxUs),
          static_cast<unsigned long>(tasks.micraBackflushHoldLastUs),
          static_cast<unsigned long>(tasks.micraBackflushHoldMaxUs),
          static_cast<unsigned long>(tasks.scaleEventsWaitLastUs),
          static_cast<unsigned long>(tasks.scaleEventsWaitMaxUs),
          static_cast<unsigned long>(tasks.shotStoreWaitLastUs),
          static_cast<unsigned long>(tasks.shotStoreWaitMaxUs),
          static_cast<unsigned long>(tasks.controlStatusWaitLastUs),
          static_cast<unsigned long>(tasks.controlStatusWaitMaxUs))) return false;
  if (!jsonScratchAppend(buf, cap, used, ",\"rows\":[")) return false;
  for (uint8_t i = 0; i < tasks.rowCount; ++i) {
    char safeName[TASK_PROFILER_NAME_CAPACITY * 2] = {};
    sanitizeJsonEmbed(tasks.rows[i].name, safeName, sizeof(safeName));
    if (!jsonScratchAppend(
            buf, cap, used,
            "%s{\"name\":\"%s\",\"core\":%d,\"currentCpuPct\":%.1f,"
            "\"averageCpuPct\":%.1f,\"stackMinWords\":%lu}",
            i == 0 ? "" : ",", safeName, static_cast<int>(tasks.rows[i].core),
            static_cast<double>(tasks.rows[i].currentCpuPct),
            static_cast<double>(tasks.rows[i].averageCpuPct),
            static_cast<unsigned long>(tasks.rows[i].stackMinBytes))) {
      return false;
    }
  }
  for (uint8_t i = 0; i < tasks.loopPhases.rowCount; ++i) {
    const LoopPhaseProfilerRow &row = tasks.loopPhases.rows[i];
    if (!jsonScratchAppend(
            buf, cap, used,
            "%s{\"name\":\"loopTask/%s\",\"core\":1,\"stackMinWords\":4294967295,"
            "\"sampleCount\":%lu,"
            "\"currentCpuPct\":%.1f,\"averageCpuPct\":%.1f,"
            "\"peakGapExecutionUs\":%lu,\"recentGapExecutionUs\":%lu",
            tasks.rowCount == 0 && i == 0 ? "" : ",",
            row.name != nullptr ? row.name : "unknown",
            static_cast<unsigned long>(row.sampleCount),
            static_cast<double>(row.currentCpuPct),
            static_cast<double>(row.averageCpuPct),
            static_cast<unsigned long>(row.peakGapExecutionUs),
            static_cast<unsigned long>(row.recentGapExecutionUs))) {
      return false;
    }
    if (full && !jsonScratchAppend(buf, cap, used,
            ",\"averageExecutionUs\":%lu,\"maxExecutionUs\":%lu,\"maxCpuUs\":%lu,"
            "\"maxAtMs\":%lu,\"lastExecutionUs\":%lu",
            static_cast<unsigned long>(row.averageExecutionUs),
            static_cast<unsigned long>(row.maxExecutionUs),
            static_cast<unsigned long>(row.maxCpuUs),
            static_cast<unsigned long>(row.maxAtMs),
            static_cast<unsigned long>(row.lastExecutionUs))) return false;
    if (!jsonScratchAppend(buf, cap, used, "}")) return false;
  }
  for (uint8_t i = 0; i < tasks.scaleWorkerPhases.rowCount; ++i) {
    const LoopPhaseProfilerRow &row = tasks.scaleWorkerPhases.rows[i];
    if (!jsonScratchAppend(
            buf, cap, used,
            "%s{\"name\":\"scaleWorker/%s\",\"core\":1,"
            "\"stackMinWords\":4294967295,\"sampleCount\":%lu,"
            "\"currentCpuPct\":%.1f,\"averageCpuPct\":%.1f",
            tasks.rowCount == 0 && tasks.loopPhases.rowCount == 0 && i == 0
                ? ""
                : ",",
            row.name != nullptr ? row.name : "unknown",
            static_cast<unsigned long>(row.sampleCount),
            static_cast<double>(row.currentCpuPct),
            static_cast<double>(row.averageCpuPct))) {
      return false;
    }
    if (full && !jsonScratchAppend(buf, cap, used,
            ",\"averageExecutionUs\":%lu,\"maxExecutionUs\":%lu,\"maxCpuUs\":%lu,"
            "\"maxAtMs\":%lu,\"lastExecutionUs\":%lu",
            static_cast<unsigned long>(row.averageExecutionUs),
            static_cast<unsigned long>(row.maxExecutionUs),
            static_cast<unsigned long>(row.maxCpuUs),
            static_cast<unsigned long>(row.maxAtMs),
            static_cast<unsigned long>(row.lastExecutionUs))) return false;
    if (!jsonScratchAppend(buf, cap, used, "}")) return false;
  }
  return jsonScratchAppend(buf, cap, used, "]}");
}

bool statusJsonAppendTaskProfiler(size_t *used,
                                  const TaskProfilerSnapshot &tasks) {
  if (!statusJsonAppend(used, "\"tasks\":")) {
    return false;
  }
  return formatTaskProfilerObject(g_work->statusJson, NetworkWorkBuf::kStatusJson,
                                  used, tasks, false);
}

bool statusJsonAppendScaleProfiler(size_t *used,
                                   const ScaleProfilerStatus &profile) {
  char remaining[16] = "null";
  if (profile.estimatedRemainingMs != UINT32_MAX) {
    snprintf(remaining, sizeof(remaining), "%lu",
             static_cast<unsigned long>(profile.estimatedRemainingMs));
  }
  // The can* flags mirror the profiler's own guards so the browser and the
  // server can never disagree about availability.
  const bool busy = profile.downloading ||
                    profile.persistence == ScaleProfilerPersistence::SAVING ||
                    profile.persistence == ScaleProfilerPersistence::INVALIDATING;
  const bool canStart =
      profile.partitionAvailable && !busy &&
      (profile.state == ScaleProfilerState::EMPTY ||
       profile.state == ScaleProfilerState::STOPPED ||
       profile.state == ScaleProfilerState::SAVED);
  const bool canStop = profile.state == ScaleProfilerState::RECORDING;
  // Stopped traces stay deletable once saving is neither pending nor active;
  // a failed save keeps the RAM evidence downloadable and deletable.
  const bool canDelete =
      !busy && (profile.state == ScaleProfilerState::SAVED ||
                (profile.state == ScaleProfilerState::STOPPED &&
                 (profile.persistence == ScaleProfilerPersistence::FAILED ||
                  profile.persistence == ScaleProfilerPersistence::NONE)));
  const bool canDownload =
      !profile.downloading && profile.state != ScaleProfilerState::RECORDING &&
      profile.state != ScaleProfilerState::PREPARING &&
      (profile.state == ScaleProfilerState::SAVED ||
       profile.state == ScaleProfilerState::STOPPED);
  return statusJsonAppend(
      used,
      "\"scaleProfile\":{\"state\":\"%s\",\"persistence\":\"%s\","
      "\"elapsedMs\":%llu,\"recordCount\":%lu,\"recordCapacity\":%lu,"
      "\"reservedRecords\":%u,\"estimatedRemainingMs\":%s,"
      "\"weightCount\":%lu,\"eventCount\":%lu,\"lostCount\":%lu,"
      "\"stopReason\":\"%s\","
      "\"partitionAvailable\":%s,\"lastError\":\"%s\","
      "\"canStart\":%s,\"canStop\":%s,\"canDelete\":%s,\"canDownload\":%s}",
      scaleProfilerStateName(profile.state),
      scaleProfilerPersistenceName(profile.persistence),
      static_cast<unsigned long long>(profile.elapsedMs),
      static_cast<unsigned long>(profile.state == ScaleProfilerState::SAVED
                                     ? profile.savedRecordCount
                                     : profile.recordCount),
      static_cast<unsigned long>(SCALE_PROFILE_RECORD_CAPACITY),
      profile.state == ScaleProfilerState::RECORDING ? 1U : 0U,
      remaining,
      static_cast<unsigned long>(profile.weightCount),
      static_cast<unsigned long>(profile.eventCount),
      static_cast<unsigned long>(profile.lostCount),
      scaleProfilerStopReasonName(profile.stopReason),
      profile.partitionAvailable ? "true" : "false",
      scaleProfilerErrorName(profile.lastError),
      canStart ? "true" : "false", canStop ? "true" : "false",
      canDelete ? "true" : "false", canDownload ? "true" : "false");
}

void copyPresetStatsForJson(const NetworkBridgeCallbacks &callbacks,
                            const ShotPresetBank &bank) {
  memset(g_work->presetStats, 0, sizeof(g_work->presetStats));
  if (callbacks.copyPresetShotStats) callbacks.copyPresetShotStats(bank, g_work->presetStats);
}

// Grind hint JSON (null when there is none): enum names by value.
void formatGrindHint(char *out, size_t cap, const corridor::GrindHintV2 &h) {
  static const char *const kStates[] = {"none", "dial_in", "maintenance", "checking", "vary"};
  static const char *const kDirections[] = {"null", "\"finer\"", "\"coarser\""};
  static const char *const kTiers[] = {"null", "\"tentative\"", "\"likely\"", "\"confident\""};
  static const char *const kBins[] = {"null", "\"small\"", "\"medium\"", "\"large\""};
  if (h.state == corridor::HintState::NONE) {
    snprintf(out, cap, "null");
    return;
  }
  snprintf(out, cap,
           "{\"state\":\"%s\",\"direction\":%s,\"tier\":%s,\"bin\":%s,\"shots\":%u,"
           "\"offsetS\":%.1f,\"bandS\":%.1f,\"atLeast\":%s,\"segmentStartId\":%lu}",
           kStates[static_cast<uint8_t>(h.state)], kDirections[static_cast<uint8_t>(h.direction)],
           kTiers[static_cast<uint8_t>(h.tier)], kBins[static_cast<uint8_t>(h.bin)],
           static_cast<unsigned>(h.n), h.offsetMs / 1000.0, h.bandMs / 1000.0,
           h.atLeast ? "true" : "false",
           static_cast<unsigned long>(h.segmentStartId));
}

// Shot statistics of one preset (plan §4.10): the last <= 10 arrivals, the
// median first drop and the grind hint.
int formatPresetShotStats(char *out, size_t cap, const PresetShotStats &s) {
  const corridor::ArrivalStats &st = s.stats;
  // Worst case per arrival: "," + "-2147483.6" (any int32 ms), so it never truncates.
  char arrivals[corridor::STATS_MAX_SHOTS * 11 + 2] = "[";
  for (uint8_t i = 0; i < st.n; ++i)
    snprintf(arrivals + strlen(arrivals), sizeof(arrivals) - strlen(arrivals), "%s%.1f",
             i ? "," : "", s.arrivalsMs[i] / 1000.0);
  strcat(arrivals, "]");
  char hintJson[192];
  formatGrindHint(hintJson, sizeof(hintJson), s.grindHint);
  return snprintf(out, cap,
      ",\"shotStats\":{\"n\":%u,\"total\":%u,\"minS\":%.1f,\"maxS\":%.1f,"
      "\"medianS\":%.1f},\"firstDropS\":%.1f,"
      "\"firstDropN\":%u,\"arrivalsS\":%s,\"grindHint\":%s}",
      static_cast<unsigned>(st.n), static_cast<unsigned>(st.total), st.minMs / 1000.0,
      st.maxMs / 1000.0, st.medianMs / 1000.0,
      s.firstDrop.medianMs / 1000.0, static_cast<unsigned>(s.firstDrop.n), arrivals,
      hintJson);
}

void buildSlimPresetsJson(const ShotPresetBank &presets) {
  if (g_work == nullptr) {
    return;
  }
  char *buf = g_work->presetsJson;
  const size_t cap = NetworkWorkBuf::kPresetsJson;
  buf[0] = '{';
  buf[1] = '}';
  buf[2] = 0;
  size_t used = 0;
  int n = snprintf(buf, cap, "{\"activeId\":%u,\"cutRampS\":%.1f,\"items\":[",
                   static_cast<unsigned>(presets.activeId), corridor::RAMP_MS / 1000.0);
  if (n > 0) {
    used = static_cast<size_t>(n);
  }
  for (uint8_t i = 0; i < presets.count && i < MAX_SHOT_PRESETS; ++i) {
    const ShotPreset &p = presets.presets[i];
    char safeName[SHOT_PRESET_NAME_CAPACITY * 6] = {};
    escapeJsonString(p.name, safeName, sizeof(safeName));
    const size_t itemStart = used;
    n = snprintf(
        buf + used, cap - used,
        "%s{\"id\":%u,\"name\":\"%s\",\"isFactory\":%s,\"brewByWeight\":%s,"
        "\"doseG\":%.1f,\"goalWeightG\":%.1f,\"fastBeforeS\":%.1f,"
        "\"slowAfterS\":%.1f,\"fastMarginG\":%.1f,\"slowMarginG\":%.1f,"
        "\"operationalWallMs\":%lu,\"bbwProtectionMs\":%lu,"
        "\"fastExtractionGuardEnabled\":%s,\"slowExtractionGuardEnabled\":%s,"
        "\"grindHintsEnabled\":%s,\"lineaMicraBrewTargetC\":%.1f",
        i == 0 ? "" : ",", static_cast<unsigned>(p.id), safeName,
        p.isFactory ? "true" : "false", p.brewByWeight ? "true" : "false",
        static_cast<double>(goalWeightDgToGrams(p.doseDg)),
        static_cast<double>(goalWeightDgToGrams(p.goalWeightDg)),
        p.fastBeforeMs / 1000.0, p.slowAfterMs / 1000.0,
        static_cast<double>(goalWeightDgToGrams(p.fastMarginDg)),
        static_cast<double>(goalWeightDgToGrams(p.slowMarginDg)),
        static_cast<unsigned long>(p.operationalWallMs),
        static_cast<unsigned long>(p.bbwProtectionMs),
        p.fastSideEnabled ? "true" : "false",
        p.slowSideEnabled ? "true" : "false",
        p.grindHintsEnabled ? "true" : "false",
        static_cast<double>(p.lineaMicraBrewTargetDeciC) / 10.0);
    if (n < 0 || static_cast<size_t>(n) >= cap - used) {
      break;
    }
    used += static_cast<size_t>(n);
    n = formatPresetShotStats(buf + used, cap - used, g_work->presetStats[i]);
    if (n < 0 || static_cast<size_t>(n) >= cap - used) {
      used = itemStart;  // never leave an item unterminated
      break;
    }
    used += static_cast<size_t>(n);
  }
  if (used + 2 < cap) {
    buf[used++] = ']';
    buf[used++] = '}';
    buf[used] = 0;
  }
}

void buildScaleHistoryJson(const ScaleHistoryEntry *entries) {
  if (g_work == nullptr) {
    return;
  }
  char *buf = g_work->historyJson;
  const size_t cap = NetworkWorkBuf::kHistoryJson;
  buf[0] = '[';
  buf[1] = ']';
  buf[2] = 0;
  size_t used = 0;
  int n = snprintf(buf, cap, "[");
  if (n > 0) {
    used = static_cast<size_t>(n);
  }
  bool first = true;
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    const ScaleHistoryEntry &entry = entries[i];
    if (entry.mac[0] == '\0') {
      continue;
    }
    char safeMac[PREFERRED_SCALE_MAC_CAPACITY * 2] = {};
    char safeName[PREFERRED_SCALE_NAME_CAPACITY * 2] = {};
    char safeFriendly[PREFERRED_SCALE_NAME_CAPACITY * 2] = {};
    sanitizeJsonEmbed(entry.mac, safeMac, sizeof(safeMac));
    sanitizeJsonEmbed(entry.name, safeName, sizeof(safeName));
    sanitizeJsonEmbed(entry.friendlyName[0] != '\0'
                          ? entry.friendlyName
                          : scaleDefaultFriendlyName(entry.name),
                      safeFriendly, sizeof(safeFriendly));
    n = snprintf(buf + used, cap - used,
                 "%s{\"mac\":\"%s\",\"name\":\"%s\",\"friendlyName\":\"%s\"}",
                 first ? "" : ",", safeMac, safeName, safeFriendly);
    if (n <= 0 || static_cast<size_t>(n) >= cap - used) {
      break;
    }
    used += static_cast<size_t>(n);
    first = false;
  }
  if (used + 1 < cap) {
    buf[used++] = ']';
    buf[used] = 0;
  }
}


void escapeConnectedScaleName(const ControlStatusSnapshot &control,
                              const ScaleHistoryEntry *entries, char *output,
                              size_t capacity) {
  output[0] = '\0';
  if (!control.scaleAvailable) return;
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    const auto &entry = entries[i];
    if (preferredScaleMacEqual(entry.mac, control.connectedScaleMac)) {
      escapeJsonString(entry.friendlyName[0] ? entry.friendlyName
                                             : scaleDefaultFriendlyName(entry.name),
                       output, capacity);
      return;
    }
  }
}

}  // namespace

OpenBrewByWeightNetwork *OpenBrewByWeightNetwork::instance_ = nullptr;


// Behavior-preserving service fragments; kept in this translation unit.
#include "network/OpenBrewByWeightNetworkService.inc"
#include "network/OpenBrewByWeightWifi.inc"
#include "network/OpenBrewByWeightHttpLifecycle.inc"
#include "network/OpenBrewByWeightHttpAuthAssets.inc"
#include "network/OpenBrewByWeightWebAccessHttp.inc"
#include "network/OpenBrewByWeightUiStream.inc"
#include "network/OpenBrewByWeightStatus.inc"
#include "diagnostics/OpenBrewByWeightNetworkDiagnostics.inc"
#include "diagnostics/OpenBrewByWeightCrashRoutes.inc"
#include "network/OpenBrewByWeightConfiguration.inc"
#include "network/OpenBrewByWeightLineaMicraWeb.inc"
#include "network/OpenBrewByWeightIntegrationApi.inc"
#include "network/OpenBrewByWeightNetworkOta.inc"

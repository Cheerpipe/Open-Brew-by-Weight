#pragma once

// Mahlkönig Grind-by-Sync (docs/MAHLKONIG_GBS.md): the E64 WS pull state
// machine, the E65/E80 push client's decisions, the reporting and
// attribution rules every route shares, and the exact wire bodies. Pure and
// allocation-free except the push response parser (cJSON, network task
// only); pinned by docs/schemas/mahlkonig-gbs-v1.vectors.json, which the
// companion reproduces. Grinder input is a signal only: nothing here reaches
// the relay or any machine command.

#include "OpenBrewByWeightShotLogTypes.h"
#include "OpenBrewByWeightTimingTypes.h"

#include <atomic>
#include <cJSON.h>
#include <cmath>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace openbrewbyweight {

// --- Models and settings (the model ids live in OpenBrewByWeightDomain.h) ----

constexpr size_t GBS_UUID_TEXT = 41;      // a grinder job id, 40 characters at most
constexpr size_t GBS_BODY_CAPACITY = 320;

inline const char *gbsModelName(uint8_t model) {
  static const char *const names[] = {"", "E64 WS", "E65W", "E65T", "E80W", "E80T"};
  return model < GBS_MODEL_COUNT ? names[model] : names[0];
}
// E65 and E80 share the push protocol; the E64 WS polls the machine itself.
inline bool gbsModelPush(uint8_t model) { return model >= GBS_MODEL_E65W && model < GBS_MODEL_COUNT; }

// A grinder address as typed: letters, digits, dots, hyphens (a name or an
// IPv4 literal). Anything else could not be a host and could break a URL.
inline bool gbsHostValid(const char *host) {
  const size_t length = host == nullptr ? 0 : strnlen(host, GBS_HOST_CAPACITY);
  if (length == 0 || length >= GBS_HOST_CAPACITY || host[0] == '.' || host[0] == '-') return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = host[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '-'))
      return false;
  }
  return true;
}

// --- Timing (docs/MAHLKONIG_GBS.md, tolerances) ------------------------------

constexpr uint32_t GBS_PULL_WINDOW_MS = 180000;     // execute to shot start
constexpr uint32_t GBS_RESULT_HOLD_MS = 6000;       // status 4 at least this long...
constexpr uint32_t GBS_RESULT_CAP_MS = 60000;       // ...and one poll, at most this long
constexpr uint32_t GBS_SEEN_MS = 30000;             // an E64 WS polls every 2 s while on
constexpr uint32_t GBS_BACKOFF_MS[] = {60000, 120000};  // after the cadence's first retry
constexpr uint32_t GBS_PAIR_STATUS_MS = 5000;
constexpr uint32_t GBS_PAIR_LIMIT_MS = 180000;      // the grinder may restart after accepting
constexpr uint32_t GBS_UNPAIRED_MS = 60000;         // 403s for this long mean unpaired
constexpr uint32_t GBS_LATE_JOB_MS = 60000;         // a job seen after its shot
constexpr uint32_t GBS_JOB_WINDOW_MS = 180000;      // job seen to shot start
constexpr uint32_t GBS_RESULT_RETRY_MS = 120000;
constexpr uint32_t GBS_CANCEL_RETRY_MS = 20000;
constexpr uint32_t GBS_RETRY_MS = 5000;
constexpr uint32_t GBS_REQUEST_TIMEOUT_MS = 8000;

// --- Reporting rules (A4) -------------------------------------------------------

// What the rules read of a stored shot; the companion reads the same from an
// archived Stats row.
struct GbsShotFacts {
  ShotLogType type = ShotLogType::AUTO;
  ShotLogStopDetail stop = ShotLogStopDetail::OTHER;
  bool channeling = false;
  ActualWeightSource source = ActualWeightSource::NONE;
  bool bad = true;  // shotLogBadReason
  uint16_t durationDs = 0;
  int16_t yieldCg = SHOT_LOG_WEIGHT_MISSING;
};

inline GbsShotFacts gbsShotFactsOf(const ShotLogRecord &record) {
  GbsShotFacts facts;
  facts.type = shotLogType(record);
  facts.stop = static_cast<ShotLogStopDetail>(record.stopDetail);
  facts.channeling = corridor::channelingOf(record.verdict);
  facts.source = shotLogWeightSource(record);
  facts.bad = shotLogBadReason(record) != nullptr;
  facts.durationDs = record.durationDs;
  facts.yieldCg = record.actualWeightCg;
  return facts;
}

// A cycle too short to be a shot (a group flush after grinding) settles no
// grind: the shot that follows does (A4). The grinder learns nothing under 10 s.
inline bool gbsIsShot(const GbsShotFacts &facts) { return facts.durationDs > MIN_SHOT_LOG_DURATION_MS / 100U; }

struct GbsShotReport {
  bool success = false;
  uint32_t timeMs = 0;  // pump time: start to the relay cut, drip excluded
  uint32_t massMg = 0;  // final yield after the drip; 0 for a cancelled shot
};

// Success only for a shot cut by weight or at the max shot time, with the
// scale connected throughout and its yield settled after the drip. Every
// other shot is reported cancelled, so the grinder learns nothing from it.
inline GbsShotReport gbsReport(const GbsShotFacts &facts) {
  GbsShotReport report;
  report.timeMs = facts.durationDs * 100U;
  const ShotLogStopDetail s = facts.stop;
  const bool cut = s == ShotLogStopDetail::NORMAL_TARGET || s == ShotLogStopDetail::EXTENDED_MAX_WEIGHT ||
                   s == ShotLogStopDetail::EXTENDED_MIN_TIME || s == ShotLogStopDetail::SLOW_MAX_TIME ||
                   s == ShotLogStopDetail::SLOW_MIN_WEIGHT || s == ShotLogStopDetail::WALL_LIMIT;
  report.success = cut && facts.type != ShotLogType::MANUAL && !facts.channeling && !facts.bad &&
                   facts.source == ActualWeightSource::POST_DRIP &&
                   !shotLogWeightIsMissing(facts.yieldCg) && facts.yieldCg > 0;
  report.massMg = report.success ? static_cast<uint32_t>(facts.yieldCg) * 10U : 0U;
  return report;
}

// A shot belongs to a grind (`seenAtMs`: execute received, or the job first
// seen) when it started after the poll before it (`priorAtMs`, equal to
// seenAtMs for the E64 WS) and either after the grind was seen or, for a job
// seen late, ended less than GBS_LATE_JOB_MS before it was seen.
inline bool gbsAttributable(uint32_t priorAtMs, uint32_t seenAtMs, uint32_t startMs, uint32_t endMs) {
  if (!tickAfter(startMs, priorAtMs)) return false;
  return !tickAfter(seenAtMs, startMs) || ticksSince(seenAtMs, endMs) < static_cast<int32_t>(GBS_LATE_JOB_MS);
}

// --- Pull mode (E64 WS) ----------------------------------------------------------

enum class GbsPullState : uint8_t { READY, ARMED, BREWING, FINISHING, ABORTED };

inline const char *gbsPullStateName(GbsPullState state) {
  switch (state) {
    case GbsPullState::ARMED: return "armed";
    case GbsPullState::BREWING: return "brewing";
    case GbsPullState::FINISHING: return "finishing";
    case GbsPullState::ABORTED: return "aborted";
    default: return "ready";
  }
}

struct GbsPull {
  GbsPullState state = GbsPullState::READY;
  uint32_t armedAtMs = 0, enteredAtMs = 0, pollsAtEnter = 0, idleSinceMs = 0;
  uint32_t timeMs = 0, volumeDg = 0;  // status 4's values
  uint32_t recipeDg = 0;              // the last SYNC_BEVERAGE_WEIGHT
};

inline void gbsPullEnter(GbsPull &pull, GbsPullState state, uint32_t now, uint32_t polls) {
  pull.state = state;
  pull.enteredAtMs = now;
  pull.pollsAtEnter = polls;
}

// `execute {"ID":9}`: the start-brew request after a GbS grind, a signal
// only. A repeat while armed or brewing is the grinder's own retry.
inline bool gbsPullExecute(GbsPull &pull, uint32_t now, uint32_t polls) {
  if (pull.state == GbsPullState::ARMED || pull.state == GbsPullState::BREWING) return false;
  pull.armedAtMs = pull.idleSinceMs = now;
  pull.timeMs = pull.volumeDg = 0;
  gbsPullEnter(pull, GbsPullState::ARMED, now, polls);
  return true;
}

inline void gbsPullAbort(GbsPull &pull, uint32_t timeMs, uint32_t now, uint32_t polls) {
  pull.timeMs = timeMs;
  pull.volumeDg = 0;
  gbsPullEnter(pull, GbsPullState::ABORTED, now, polls);
}

// Release from Settings: the grinder stops waiting and may grind again.
inline bool gbsPullRelease(GbsPull &pull, uint32_t now, uint32_t polls) {
  if (pull.state != GbsPullState::ARMED && pull.state != GbsPullState::BREWING) return false;
  gbsPullAbort(pull, now - pull.armedAtMs, now, polls);
  return true;
}

// A stored shot that started after the arm settles it; earlier shots (a
// flush the machine ran on its own) leave it armed.
inline bool gbsPullShot(GbsPull &pull, const GbsShotReport &report, uint32_t startMs, uint32_t now,
                        uint32_t polls) {
  if ((pull.state != GbsPullState::ARMED && pull.state != GbsPullState::BREWING) ||
      !gbsAttributable(pull.armedAtMs, pull.armedAtMs, startMs, startMs))
    return false;
  // No weight falls back to the recipe weight the grinder sent; neither refuses.
  const uint32_t volumeDg = report.massMg ? (report.massMg + 50U) / 100U : pull.recipeDg;
  if (!report.success || volumeDg == 0) {
    gbsPullAbort(pull, report.timeMs, now, polls);
    return true;
  }
  pull.timeMs = report.timeMs;
  pull.volumeDg = volumeDg;
  gbsPullEnter(pull, GbsPullState::FINISHING, now, polls);
  return true;
}

// Every network pass: a running cycle is BREWING; a cycle that left no shot
// (a rinse) returns to ARMED; the window and the result hold expire. True
// when the answer changed.
inline bool gbsPullTick(GbsPull &pull, uint32_t now, uint32_t polls, bool cycleActive) {
  const GbsPullState before = pull.state;
  switch (pull.state) {
    case GbsPullState::ARMED:
      if (cycleActive) {
        pull.idleSinceMs = now;
        gbsPullEnter(pull, GbsPullState::BREWING, now, polls);
      } else if (ticksSince(now, pull.armedAtMs) >= static_cast<int32_t>(GBS_PULL_WINDOW_MS))
        gbsPullAbort(pull, GBS_PULL_WINDOW_MS, now, polls);
      break;
    case GbsPullState::BREWING:
      if (cycleActive) pull.idleSinceMs = now;
      // The record lands after the drip; a cycle quiet this long left none.
      else if (ticksSince(now, pull.idleSinceMs) >= static_cast<int32_t>(GBS_RESULT_CAP_MS))
        gbsPullEnter(pull, GbsPullState::ARMED, now, polls);
      break;
    case GbsPullState::FINISHING:
    case GbsPullState::ABORTED:
      if ((ticksSince(now, pull.enteredAtMs) >= static_cast<int32_t>(GBS_RESULT_HOLD_MS) &&
           polls != pull.pollsAtEnter) ||
          ticksSince(now, pull.enteredAtMs) >= static_cast<int32_t>(GBS_RESULT_CAP_MS))
        gbsPullEnter(pull, GbsPullState::READY, now, polls);
      break;
    default: break;
  }
  return pull.state != before;
}

// The answer to GET /api/v2/mako. Always ready to grind: set and sensed
// temperatures equal, tank full (no machine state gates a grind in v1).
inline size_t gbsMakoBody(char *out, size_t capacity, const GbsPull &pull, uint32_t now,
                          uint32_t unixS) {
  uint32_t status = 1, extraction = 0, timeMs = 0, volumeDg = 0;
  switch (pull.state) {
    case GbsPullState::ARMED:
    case GbsPullState::BREWING:
      status = 3, extraction = 1, timeMs = now - pull.armedAtMs;
      break;
    case GbsPullState::FINISHING: status = 4, timeMs = pull.timeMs, volumeDg = pull.volumeDg; break;
    case GbsPullState::ABORTED: status = 4, extraction = 2, timeMs = pull.timeMs; break;
    default: break;
  }
  const int n = snprintf(out, capacity,
                         "{\"MA_STATUS\":%lu,\"MA_EXTRACTION_STATUS\":%lu,"
                         "\"REAL_EXTRACTION_TIME_MS\":%lu,\"PU_SENS_FLOW_METER_VOLUME\":%lu.%lu,"
                         "\"BG_SET_TEMP\":93.0,\"BG_SENS_TEMP\":93.0,\"TANK_LEVEL\":1,\"script\":0,"
                         "\"TIMESTAMP\":%lu}",
                         static_cast<unsigned long>(status), static_cast<unsigned long>(extraction),
                         static_cast<unsigned long>(timeMs), static_cast<unsigned long>(volumeDg / 10U),
                         static_cast<unsigned long>(volumeDg % 10U), static_cast<unsigned long>(unixS));
  return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
}

// A pull answer httpd serves (S4): the network task alone publishes, httpd
// copies it out without a lock or an allocation. Word-sized relaxed atomics
// under a sequence count; a copy that raced a publish is taken again.
struct GbsSnapshot {
  static constexpr size_t kWords = GBS_BODY_CAPACITY / 4;
  std::atomic<uint32_t> sequence{0}, length{0};
  std::atomic<uint32_t> words[kWords] = {};

  void publish(const char *body, size_t n) {
    if (n > GBS_BODY_CAPACITY) return;
    const uint32_t start = sequence.load(std::memory_order_relaxed);
    sequence.store(start + 1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i * 4 < n; ++i) {
      uint32_t word = 0;
      memcpy(&word, body + i * 4, n - i * 4 < 4 ? n - i * 4 : 4);
      words[i].store(word, std::memory_order_relaxed);
    }
    length.store(static_cast<uint32_t>(n), std::memory_order_relaxed);
    sequence.store(start + 2U, std::memory_order_release);
  }

  // The body's length, 0 when none was published or every attempt raced.
  size_t read(char *out, size_t capacity) const {
    for (int attempt = 0; attempt < 8; ++attempt) {
      const uint32_t before = sequence.load(std::memory_order_acquire);
      if (before & 1U) continue;
      const size_t n = length.load(std::memory_order_relaxed);
      if (n > capacity) return 0;
      for (size_t i = 0; i * 4 < n; ++i) {
        const uint32_t word = words[i].load(std::memory_order_relaxed);
        memcpy(out + i * 4, &word, n - i * 4 < 4 ? n - i * 4 : 4);
      }
      std::atomic_thread_fence(std::memory_order_acquire);
      if (sequence.load(std::memory_order_relaxed) == before) return n;
    }
    return 0;
  }
};

// GET /api/v2/machine: generic Xenia identity; the serial comes from the
// device id (its MAC text), so it never changes.
inline size_t gbsMachineBody(char *out, size_t capacity, const char *deviceId) {
  char serial[20] = "OBBW";
  size_t used = 4;
  for (const char *c = deviceId; c != nullptr && *c && used + 1 < sizeof(serial); ++c)
    if (*c != ':') serial[used++] = *c;
  serial[used] = '\0';
  const int n = snprintf(out, capacity,
                         "{\"MA_TYPE\":1,\"MA_SUBTYPE\":1,\"MA_SN\":\"%s\",\"FW_VERSION_MAJOR\":1,"
                         "\"FW_VERSION_MINOR\":0,\"ESP_FW_MAJOR\":1,\"ESP_FW_MINOR\":0,"
                         "\"MA_MAX_AMPERE\":16,\"MA_FIX_WATER_SUPPLY\":0,\"BLE\":0,\"MA_DELAY\":0}",
                         serial);
  return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
}

// One number member of a flat JSON object (the grinder's POST bodies), with
// no allocation: other members, nested values and strings are skipped. False
// when the body is malformed or the member is absent or not a number.
inline bool gbsJsonNumber(const char *body, size_t length, const char *key, double &out) {
  size_t i = 0;
  const auto ws = [&] {
    while (i < length && (body[i] == ' ' || body[i] == '\t' || body[i] == '\r' || body[i] == '\n')) ++i;
  };
  // A string from its opening quote; `match` compares it with `key`.
  const auto string = [&](const char *match) {
    if (i >= length || body[i] != '"') return false;
    const size_t start = ++i;
    while (i < length && body[i] != '"') i += body[i] == '\\' ? 2 : 1;
    if (i >= length) return false;
    const bool same = match != nullptr && strlen(match) == i - start && memcmp(body + start, match, i - start) == 0;
    ++i;
    return match == nullptr || same;
  };
  const auto skip = [&] {
    size_t depth = 0;
    do {
      ws();
      if (i >= length) return false;
      const char c = body[i];
      if (c == '"') {
        if (!string(nullptr)) return false;
      } else if (c == '{' || c == '[') {
        ++depth, ++i;
      } else if (c == '}' || c == ']') {
        if (depth == 0) return false;
        --depth, ++i;
      } else if (c == ',' || c == ':') {
        if (depth == 0) return false;
        ++i;
      } else {
        const size_t start = i;
        while (i < length && strchr(",:}] \t\r\n\"{[", body[i]) == nullptr) ++i;
        if (i == start) return false;
      }
    } while (depth > 0);
    return true;
  };
  bool found = false;
  ws();
  if (i >= length || body[i++] != '{') return false;
  ws();
  if (i < length && body[i] == '}') return false;
  for (;;) {
    ws();
    const size_t keyAt = i;
    if (!string(nullptr)) return false;
    i = keyAt;
    const bool wanted = string(key) && !found;
    ws();
    if (i >= length || body[i++] != ':') return false;
    ws();
    if (wanted && i < length && (body[i] == '-' || (body[i] >= '0' && body[i] <= '9'))) {
      char number[32];
      size_t n = 0;
      while (i < length && n + 1 < sizeof(number) && strchr("+-.eE0123456789", body[i]) != nullptr)
        number[n++] = body[i++];
      number[n] = '\0';
      char *end = nullptr;
      out = strtod(number, &end);
      found = end == number + n && std::isfinite(out);
      if (!found) return false;
    } else if (!skip()) {
      return false;
    }
    ws();
    if (i < length && body[i] == ',') {
      ++i;
      continue;
    }
    ws();
    if (i >= length || body[i] != '}') return false;
    ++i;
    ws();
    return found && i == length;
  }
}

// --- Push mode (E65W, E65T, E80W, E80T) --------------------------------------------

enum class GbsRequest : uint8_t { NONE, PAIR_APPLY, PAIR_STATUS, BREW_JOBS, BREW_ACCEPT, BREW_DONE };
enum class GbsPairPhase : uint8_t { IDLE, APPLYING, ACCEPTING, NEED_ID, FAILED };
// The connection as Settings shows it (push), and the companion's `st`.
enum class GbsLink : uint8_t { UNKNOWN, OK, UNREACHABLE, UNPAIRED, IDENTITY };

inline const char *gbsRequestPath(GbsRequest request) {
  switch (request) {
    case GbsRequest::PAIR_APPLY: return "/grinderApi/pairingApplication";
    case GbsRequest::PAIR_STATUS: return "/grinderApi/pairingStatus";
    case GbsRequest::BREW_JOBS: return "/grinderApi/brewJobs";
    case GbsRequest::BREW_ACCEPT: return "/grinderApi/brewAccept";
    case GbsRequest::BREW_DONE: return "/grinderApi/brewDone";
    default: return "";
  }
}
inline const char *gbsRequestName(GbsRequest request) {
  static const char *const names[] = {"none", "pair_apply", "pair_status", "brew_jobs", "brew_accept", "brew_done"};
  return names[static_cast<uint8_t>(request)];
}
inline const char *gbsLinkName(GbsLink link) {
  static const char *const names[] = {"unknown", "ok", "unreach", "unpaired", "identity"};
  return names[static_cast<uint8_t>(link)];
}
inline const char *gbsPairPhaseName(GbsPairPhase phase) {
  static const char *const names[] = {"idle", "applying", "accepting", "need_id", "failed"};
  return names[static_cast<uint8_t>(phase)];
}

// The grinder's job ids and our UUIDs are echoed into bodies: only letters,
// digits and hyphens pass.
// `capacity`: the buffer holding it, when shorter than GBS_UUID_TEXT.
inline bool gbsUuidValid(const char *text, size_t capacity = GBS_UUID_TEXT) {
  const size_t bound = capacity < GBS_UUID_TEXT ? capacity : GBS_UUID_TEXT;
  const size_t length = text == nullptr ? 0 : strnlen(text, bound);
  if (length == 0 || length >= bound) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = text[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  return true;
}

// A version 4 UUID in lowercase text from 16 random bytes.
inline void gbsUuidV4(const uint8_t random[16], char (&out)[37]) {
  uint8_t b[16];
  memcpy(b, random, sizeof(b));
  b[6] = static_cast<uint8_t>((b[6] & 0x0F) | 0x40);
  b[8] = static_cast<uint8_t>((b[8] & 0x3F) | 0x80);
  size_t used = 0;
  for (size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) out[used++] = '-';
    used += static_cast<size_t>(snprintf(out + used, sizeof(out) - used, "%02x", b[i]));
  }
  out[36] = '\0';
}

// The pairing identity (NVS gbs_ident; never in a settings backup) with the
// grinder's address it belongs to. No padding: the stored checksum covers
// every byte.
struct GbsIdentity {
  uint32_t pairingId = 0;
  uint8_t paired = 0, pinned = 0;
  uint8_t pin[32] = {};  // SHA-256 of the grinder's certificate (DER)
  char brewDeviceId[37] = {};
  uint8_t reserved = 0;
  char host[GBS_HOST_CAPACITY] = {};  // as the grinder's Wi-Fi screen shows it
};
static_assert(sizeof(GbsIdentity) == 124, "GbsIdentity must have no padding");

struct GbsJob {
  char uuid[GBS_UUID_TEXT] = {};
  uint32_t doseType = 0, durationMs = 0, weightMg = 0;
};

struct GbsResponse {
  enum Status : uint8_t { OK, ALREADY_PAIRED, APP_NOT_FOUND, JOB_NOT_EXISTING, REFUSED, FAILED, MALFORMED };
  Status status = MALFORMED;
  uint32_t applicationId = 0, pairingCode = 0, pairingId = 0;
  bool accepted = false, hasJob = false;
  GbsJob job;
};

inline const char *gbsResponseStatusName(GbsResponse::Status status) {
  static const char *const names[] = {"ok", "already_paired", "app_not_found", "job_not_existing",
                                      "refused", "failed", "malformed"};
  return names[status];
}

namespace detail {
inline bool gbsUint(const cJSON *object, const char *key, uint32_t &out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
      item->valuedouble > 4294967295.0)
    return false;
  out = static_cast<uint32_t>(lround(item->valuedouble));
  return true;
}
}  // namespace detail

// A reply to `request` (HTTP status and body). 403 is the grinder refusing
// the pairing; any other non-2xx, a body that is not JSON or a reply without
// the fields its request needs is FAILED or MALFORMED.
inline GbsResponse gbsParseResponse(GbsRequest request, int httpStatus, const char *body, size_t length) {
  GbsResponse out;
  if (httpStatus == 403) {
    out.status = GbsResponse::REFUSED;
    return out;
  }
  if (httpStatus < 200 || httpStatus > 299) {
    out.status = GbsResponse::FAILED;
    return out;
  }
  cJSON *root = body == nullptr ? nullptr : cJSON_ParseWithLength(body, length);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return out;
  }
  const cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
  const char *text = cJSON_IsString(status) ? status->valuestring : nullptr;
  if (text != nullptr && strcmp(text, "ClientErrorDevAlreadyPaired") == 0) out.status = GbsResponse::ALREADY_PAIRED;
  else if (text != nullptr && strcmp(text, "ClientErrorPairingAppIdNotFound") == 0)
    out.status = GbsResponse::APP_NOT_FOUND;
  else if (text != nullptr && strcmp(text, "ClientErrorOngoingJobNotExisting") == 0)
    out.status = GbsResponse::JOB_NOT_EXISTING;
  else if (text != nullptr && strcmp(text, "Ok") != 0) out.status = GbsResponse::FAILED;
  else if (request == GbsRequest::PAIR_APPLY) {
    out.status = detail::gbsUint(root, "applicationId", out.applicationId) &&
                         detail::gbsUint(root, "pairingCode", out.pairingCode)
                     ? GbsResponse::OK
                     : GbsResponse::MALFORMED;
  } else if (request == GbsRequest::PAIR_STATUS) {
    const cJSON *accepted = cJSON_GetObjectItemCaseSensitive(root, "accepted");
    out.accepted = cJSON_IsTrue(accepted);
    out.status = cJSON_IsBool(accepted) && (!out.accepted || detail::gbsUint(root, "pairingId", out.pairingId))
                     ? GbsResponse::OK
                     : GbsResponse::MALFORMED;
  } else if (request == GbsRequest::BREW_JOBS) {
    const cJSON *jobs = cJSON_GetObjectItemCaseSensitive(root, "brewJobs");
    out.status = cJSON_IsArray(jobs) ? GbsResponse::OK : GbsResponse::MALFORMED;
    const cJSON *job = nullptr;
    cJSON_ArrayForEach(job, jobs) {
      uint32_t head = 1;
      const cJSON *uuid = cJSON_GetObjectItemCaseSensitive(job, "grindUuid");
      // Grouphead 1 only; a job for another group head is not this machine's.
      if (out.hasJob || !cJSON_IsObject(job) || !cJSON_IsString(uuid) || !gbsUuidValid(uuid->valuestring) ||
          (cJSON_GetObjectItemCaseSensitive(job, "groupHeadId") != nullptr &&
           (!detail::gbsUint(job, "groupHeadId", head) || head != 1)))
        continue;
      snprintf(out.job.uuid, sizeof(out.job.uuid), "%s", uuid->valuestring);
      (void)detail::gbsUint(job, "doseTypeExpected", out.job.doseType);
      (void)detail::gbsUint(job, "durationExpectedMs", out.job.durationMs);
      (void)detail::gbsUint(job, "brewWeightExpectedMg", out.job.weightMg);
      out.hasJob = true;
    }
  } else {
    out.status = GbsResponse::OK;
  }
  cJSON_Delete(root);
  return out;
}

struct GbsResult {
  bool due = false, retake = false;
  char uuid[GBS_UUID_TEXT] = {}, eventUuid[37] = {};
  uint32_t doseType = 0, sinceMs = 0, retryAtMs = 0;
  GbsShotReport report;
};

struct GbsPush {
  uint32_t pollMs = GBS_CADENCE_DEFAULT_S * 1000U;  // the direct cadence (D12)
  GbsPairPhase phase = GbsPairPhase::IDLE;
  uint32_t applicationId = 0, pairingCode = 0, pairSinceMs = 0;
  GbsLink link = GbsLink::UNKNOWN;
  uint8_t failures = 0;
  uint32_t dueAtMs = 0, refusedSinceMs = 0, lastOkAtMs = 0, lastPollAtMs = 0;
  bool hasJob = false, jobTaken = false;
  GbsJob job;
  uint32_t jobSeenAtMs = 0, jobPriorAtMs = 0;
  GbsResult result;
  // Set once a delivery or a cancellation settled, for Settings and Home.
  bool pollAfterResult = false;
};

// The request the client may start now, or NONE. Nothing starts during a
// sync hold; a result goes first after one, then one jobs poll.
inline GbsRequest gbsPushNext(const GbsPush &push, const GbsIdentity &identity, bool hold, bool direct,
                              uint32_t now) {
  if (hold) return GbsRequest::NONE;
  if (push.phase == GbsPairPhase::APPLYING) return tickReached(now, push.dueAtMs) ? GbsRequest::PAIR_APPLY : GbsRequest::NONE;
  if (push.phase == GbsPairPhase::ACCEPTING)
    return tickReached(now, push.dueAtMs) ? GbsRequest::PAIR_STATUS : GbsRequest::NONE;
  if (!identity.paired || !direct || push.link == GbsLink::IDENTITY || push.link == GbsLink::UNPAIRED)
    return GbsRequest::NONE;
  if (push.result.due && tickReached(now, push.result.retryAtMs))
    return push.result.retake ? GbsRequest::BREW_ACCEPT : GbsRequest::BREW_DONE;
  // After a failure the job waits for the next poll (its back-off) to show it again.
  if (push.hasJob && !push.jobTaken && push.failures == 0) return GbsRequest::BREW_ACCEPT;
  return push.pollAfterResult || tickReached(now, push.dueAtMs) ? GbsRequest::BREW_JOBS : GbsRequest::NONE;
}

// Pairing from Settings (person present, outside shots).
inline void gbsPushPair(GbsPush &push, uint32_t now) {
  const uint32_t pollMs = push.pollMs;
  push = GbsPush{};
  push.pollMs = pollMs;
  push.phase = GbsPairPhase::APPLYING;
  push.pairSinceMs = push.dueAtMs = now;
}

// The body of `request`; 0 when it does not fit or an id is unusable.
inline size_t gbsRequestBody(char *out, size_t capacity, GbsRequest request, const GbsPush &push,
                             const GbsIdentity &identity) {
  int n = -1;
  const unsigned long pairing = identity.pairingId;
  switch (request) {
    case GbsRequest::PAIR_APPLY:
      if (gbsUuidValid(identity.brewDeviceId, sizeof(identity.brewDeviceId)))
        n = snprintf(out, capacity,
                     "{\"version\":1,\"name\":\"Open Brew by Weight\",\"model\":\"Open Brew by Weight\","
                     "\"manufacturer\":\"Open Brew by Weight\",\"brewDeviceId\":\"%s\","
                     "\"numOfGroupHeads\":1,\"progressSupported\":false}",
                     identity.brewDeviceId);
      break;
    case GbsRequest::PAIR_STATUS:
      n = snprintf(out, capacity, "{\"version\":1,\"applicationId\":%lu}",
                   static_cast<unsigned long>(push.applicationId));
      break;
    case GbsRequest::BREW_JOBS: n = snprintf(out, capacity, "{\"version\":1,\"pairingId\":%lu}", pairing); break;
    case GbsRequest::BREW_ACCEPT: {
      const bool retake = push.result.due && push.result.retake;
      const char *uuid = retake ? push.result.uuid : push.job.uuid;
      if (gbsUuidValid(uuid))
        n = snprintf(out, capacity,
                     "{\"version\":1,\"pairingId\":%lu,\"grindUuid\":\"%s\",\"groupHeadId\":1,\"doseType\":%lu}",
                     pairing, uuid,
                     static_cast<unsigned long>(retake ? push.result.doseType : push.job.doseType));
      break;
    }
    case GbsRequest::BREW_DONE:
      if (gbsUuidValid(push.result.uuid) && gbsUuidValid(push.result.eventUuid, sizeof(push.result.eventUuid)))
        n = snprintf(out, capacity,
                     "{\"version\":1,\"pairingId\":%lu,\"grindUuid\":\"%s\",\"durationFinalMs\":%lu,"
                     "\"massFinalMg\":%lu,\"brewEventUuid\":\"%s\",\"result\":\"%s\"}",
                     pairing, push.result.uuid, static_cast<unsigned long>(push.result.report.timeMs),
                     static_cast<unsigned long>(push.result.report.massMg), push.result.eventUuid,
                     push.result.report.success ? "Success" : "Canceled");
      break;
    default: break;
  }
  return n > 0 && static_cast<size_t>(n) < capacity ? static_cast<size_t>(n) : 0;
}

// The shot settling the job in hand: a result to deliver, its event id from
// the caller's random bytes. False when the shot is not this job's.
inline bool gbsPushShot(GbsPush &push, const GbsShotReport &report, uint32_t startMs, uint32_t endMs,
                        const uint8_t random[16], uint32_t now) {
  if (!push.hasJob || !gbsAttributable(push.jobPriorAtMs, push.jobSeenAtMs, startMs, endMs)) return false;
  push.result = GbsResult{};
  push.result.due = true;
  push.result.retake = !push.jobTaken;
  memcpy(push.result.uuid, push.job.uuid, sizeof(push.result.uuid));
  push.result.doseType = push.job.doseType;
  push.result.report = report;
  push.result.sinceMs = push.result.retryAtMs = now;
  gbsUuidV4(random, push.result.eventUuid);
  push.hasJob = push.jobTaken = false;
  return true;
}

// A shot stored after its hold (S3): the job in hand takes it, its result
// going first; without one, a jobs poll goes at once (not at the next 25 s
// tick), in case the grind came just before the shot and its job is first
// seen there. The 25 s cadence restarts from that poll.
inline bool gbsPushShotEnded(GbsPush &push, const GbsShotReport &report, uint32_t startMs, uint32_t endMs,
                             const uint8_t random[16], uint32_t now) {
  if (gbsPushShot(push, report, startMs, endMs, random, now)) return true;
  push.pollAfterResult = true;
  return false;
}

// A taken job with no shot in its window is cancelled, so the grinder stops
// handing it out. True when that happened.
inline bool gbsPushExpire(GbsPush &push, const uint8_t random[16], uint32_t now) {
  if (!push.hasJob || push.result.due ||
      ticksSince(now, push.jobSeenAtMs) < static_cast<int32_t>(GBS_JOB_WINDOW_MS))
    return false;
  GbsShotReport cancelled;
  cancelled.timeMs = 0;
  push.jobPriorAtMs = push.jobSeenAtMs;
  return gbsPushShot(push, cancelled, push.jobSeenAtMs + 1U, push.jobSeenAtMs + 1U, random, now);
}

// The wait after `failures` failures in a row: the cadence, then 60, 120 s.
inline uint32_t gbsBackoffMs(uint8_t failures, uint32_t pollMs) {
  return failures <= 1 ? pollMs : GBS_BACKOFF_MS[failures >= 3 ? 1 : 0];
}

// What a finished request changes. `transportOk` false is a connect error, a
// timeout or a TLS failure; `pinMismatch` a certificate other than the pinned
// one. Returns true when the identity changed (save it).
inline bool gbsPushReply(GbsPush &push, GbsIdentity &identity, GbsRequest request, bool transportOk,
                         bool pinMismatch, const GbsResponse &reply, uint32_t now) {
  bool identityChanged = false;
  const bool pairing = request == GbsRequest::PAIR_APPLY || request == GbsRequest::PAIR_STATUS;
  if (pinMismatch) {
    push.link = GbsLink::IDENTITY;
    if (pairing) push.phase = GbsPairPhase::FAILED;
    return false;
  }
  if (!transportOk || reply.status == GbsResponse::FAILED || reply.status == GbsResponse::MALFORMED) {
    if (pairing) {
      // The grinder may freeze and restart after accepting: keep asking.
      const bool waiting = push.phase == GbsPairPhase::ACCEPTING &&
                           ticksSince(now, push.pairSinceMs) < static_cast<int32_t>(GBS_PAIR_LIMIT_MS);
      push.phase = waiting ? GbsPairPhase::ACCEPTING : GbsPairPhase::FAILED;
      push.dueAtMs = now + GBS_PAIR_STATUS_MS;
      return false;
    }
    if (push.failures < UINT8_MAX) ++push.failures;
    push.link = GbsLink::UNREACHABLE;
    push.dueAtMs = now + gbsBackoffMs(push.failures, push.pollMs);
    if (push.result.due) {
      const uint32_t limit = push.result.report.success ? GBS_RESULT_RETRY_MS : GBS_CANCEL_RETRY_MS;
      push.result.retryAtMs = now + GBS_RETRY_MS;
      if (ticksSince(now, push.result.sinceMs) >= static_cast<int32_t>(limit)) push.result.due = false;
    }
    return false;
  }
  push.failures = 0;
  push.lastOkAtMs = now;
  if (request != GbsRequest::BREW_JOBS || reply.status != GbsResponse::REFUSED) {
    push.refusedSinceMs = 0;
    if (push.link != GbsLink::UNPAIRED) push.link = GbsLink::OK;
  }
  switch (request) {
    case GbsRequest::PAIR_APPLY:
      if (reply.status == GbsResponse::OK) {
        push.phase = GbsPairPhase::ACCEPTING;
        push.applicationId = reply.applicationId;
        push.pairingCode = reply.pairingCode;
        push.pairSinceMs = now;
        push.dueAtMs = now + GBS_PAIR_STATUS_MS;
      } else {
        push.phase = reply.status == GbsResponse::ALREADY_PAIRED ? GbsPairPhase::NEED_ID : GbsPairPhase::FAILED;
      }
      break;
    case GbsRequest::PAIR_STATUS:
      if (reply.status == GbsResponse::APP_NOT_FOUND) {
        push.phase = GbsPairPhase::APPLYING;
        push.dueAtMs = now;
      } else if (reply.status == GbsResponse::OK && reply.accepted) {
        push.phase = GbsPairPhase::IDLE;
        push.link = GbsLink::OK;
        identity.pairingId = reply.pairingId;
        identity.paired = 1;
        identityChanged = true;
        push.dueAtMs = now;
      } else if (ticksSince(now, push.pairSinceMs) >= static_cast<int32_t>(GBS_PAIR_LIMIT_MS)) {
        push.phase = GbsPairPhase::FAILED;
      } else {
        push.dueAtMs = now + GBS_PAIR_STATUS_MS;
      }
      break;
    case GbsRequest::BREW_JOBS:
      push.pollAfterResult = false;
      push.dueAtMs = now + push.pollMs;
      if (reply.status == GbsResponse::REFUSED) {
        // The grinder also refuses briefly while it boots.
        if (push.refusedSinceMs == 0) push.refusedSinceMs = now ? now : 1U;
        else if (ticksSince(now, push.refusedSinceMs) >= static_cast<int32_t>(GBS_UNPAIRED_MS))
          push.link = GbsLink::UNPAIRED;
        break;
      }
      if (reply.hasJob && (!push.hasJob || strcmp(push.job.uuid, reply.job.uuid) != 0) &&
          !(push.result.due && strcmp(push.result.uuid, reply.job.uuid) == 0)) {
        // A new job, or a newer one superseding the one in hand.
        push.hasJob = true;
        push.jobTaken = false;
        push.job = reply.job;
        push.jobSeenAtMs = now;
        push.jobPriorAtMs = push.lastPollAtMs ? push.lastPollAtMs : now - push.pollMs;
      } else if (reply.hasJob && push.hasJob) {
        push.jobTaken = false;  // handed out again: take it again
      }
      push.lastPollAtMs = now;
      break;
    case GbsRequest::BREW_ACCEPT:
      if (push.result.due && push.result.retake) {
        push.result.retake = false;
        // The grinder no longer has the job: its result cannot be delivered.
        if (reply.status != GbsResponse::OK) push.result.due = false, push.pollAfterResult = true;
      } else {
        push.jobTaken = reply.status == GbsResponse::OK;
        if (!push.jobTaken) push.hasJob = false;
      }
      break;
    case GbsRequest::BREW_DONE: {
      const uint32_t limit = push.result.report.success ? GBS_RESULT_RETRY_MS : GBS_CANCEL_RETRY_MS;
      if (reply.status == GbsResponse::JOB_NOT_EXISTING &&
          ticksSince(now, push.result.sinceMs) < static_cast<int32_t>(limit)) {
        push.result.retake = true;  // take it again, then send again, paced like a retry
        push.result.retryAtMs = now + GBS_RETRY_MS;
      } else {
        push.result.due = false;
        push.pollAfterResult = true;
      }
      break;
    }
    default: break;
  }
  return identityChanged;
}

// The pin verdict for one TLS handshake: capture while unpinned, else the
// certificate must be the pinned one.
inline bool gbsPinAccepts(const GbsIdentity &identity, const uint8_t seen[32]) {
  return !identity.pinned || memcmp(identity.pin, seen, sizeof(identity.pin)) == 0;
}

inline void gbsPinHex(const uint8_t pin[32], char (&out)[65]) {
  for (size_t i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", pin[i]);
}

// The companion's `gbs` status values (docs/COMPANION_LINK.md).
enum class GbsCompanionState : uint8_t { NONE, OK, UNREACH, UNPAIRED, IDENTITY, SEEN, LOST };

inline GbsCompanionState gbsCompanionStateOf(const char *text) {
  static const char *const names[] = {"ok", "unreach", "unpaired", "identity", "seen", "lost"};
  for (uint8_t i = 0; i < 6; ++i)
    if (text != nullptr && strcmp(text, names[i]) == 0) return static_cast<GbsCompanionState>(i + 1);
  return GbsCompanionState::NONE;
}

inline const char *gbsCompanionStateName(GbsCompanionState state) {
  static const char *const names[] = {"", "ok", "unreach", "unpaired", "identity", "seen", "lost"};
  return names[static_cast<uint8_t>(state)];
}

// `cfg.gbs` (minimal data: each member has a companion consumer). The E64 WS
// needs only the model; a push model goes once paired and pinned. Empty when
// the companion has nothing to do for GbS.
inline size_t gbsCompanionConfig(char *out, size_t capacity, uint8_t model, const char *host,
                                 const GbsIdentity &identity) {
  int n = -1;
  if (model == GBS_MODEL_E64WS) {
    n = snprintf(out, capacity, "{\"m\":\"e64ws\"}");
  } else if (gbsModelPush(model) && identity.paired && identity.pinned && gbsHostValid(host) &&
             gbsUuidValid(identity.brewDeviceId, sizeof(identity.brewDeviceId))) {
    char pin[65];
    gbsPinHex(identity.pin, pin);
    n = snprintf(out, capacity, "{\"m\":\"%s\",\"h\":\"%s\",\"d\":\"%s\",\"p\":%lu,\"fp\":\"%s\"}",
                 gbsModelId(model), host, identity.brewDeviceId,
                 static_cast<unsigned long>(identity.pairingId), pin);
  }
  if (n > 0 && static_cast<size_t>(n) < capacity) return static_cast<size_t>(n);
  if (capacity) out[0] = '\0';
  return 0;
}

// The companion's last `gbs` frame, under the network's lock.
struct GbsCompanionReport {
  bool own = false;
  GbsCompanionState state = GbsCompanionState::NONE;
  uint32_t delivered = 0;  // the last shot it delivered as Success (0: none)
};

// What Settings, Home and the Diagnostic export read of the integration,
// copied under the network's lock (times are millis()).
struct GbsView {
  bool enabled = false, companionRoute = false, paired = false, adjusts = false;
  uint8_t model = GBS_MODEL_NONE;
  char host[GBS_HOST_CAPACITY] = {};
  GbsPullState pull = GbsPullState::READY;
  GbsPairPhase phase = GbsPairPhase::IDLE;
  GbsLink link = GbsLink::UNKNOWN;
  GbsCompanionState companion = GbsCompanionState::NONE;
  uint32_t pairingCode = 0, armedAtMs = 0, seenAtMs = 0, okAtMs = 0;
  uint32_t lastId = 0, lastAtMs = 0, lastTimeMs = 0, lastMassMg = 0;
  bool lastSuccess = false, lastDelivered = false;
  uint32_t polls = 0, requests = 0, failures = 0;
};

}  // namespace openbrewbyweight

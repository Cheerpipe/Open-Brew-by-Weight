// Stats header golden vectors (docs/schemas/stats-header-v1.vectors.json):
// the firmware's own formatStatsHeader over shared fixtures, which the
// companion's port of it must reproduce. Each fixture row is a Stats row in
// the units the companion stores (the columns the header reads), converted
// back to a ShotLogRecord with the exact inverse of formatShotStatsRow's
// formatting. STATS_HEADER_REGENERATE=1 rewrites the file from the cases
// below; otherwise every stored header must match, field for field.

#include "OpenBrewByWeightShotCurveTypes.h"
#include "OpenBrewByWeightShotLogTypes.h"
#include "OpenBrewByWeightWebhook.h"  // escapeJsonString

#include <cJSON.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace openbrewbyweight {
#include "network/OpenBrewByWeightStatsJson.inc"
#include "network/OpenBrewByWeightStatsHeader.inc"
}  // namespace openbrewbyweight

using namespace openbrewbyweight;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
      std::exit(1);                                                             \
    }                                                                           \
  } while (0)

namespace {

StatsHeaderWork work;

const char *const kVerdicts[] = {"on_recipe", "fast", "slow"};
const char *const kTypes[] = {"auto", "timer_only", "manual"};
const ShotLogStopDetail kDetails[] = {
    ShotLogStopDetail::NORMAL_TARGET,  ShotLogStopDetail::EXTENDED_MAX_WEIGHT,
    ShotLogStopDetail::SLOW_MIN_WEIGHT, ShotLogStopDetail::EXTENDED_MIN_TIME,
    ShotLogStopDetail::AUTO_TO_MANUAL, ShotLogStopDetail::WEB_STOP,
    ShotLogStopDetail::OTHER};

const cJSON *field(const cJSON *row, const char *name) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(row, name);
  CHECK(item != nullptr);
  return item;
}
long scaled(const cJSON *row, const char *name, double scale) {
  const cJSON *item = field(row, name);
  CHECK(cJSON_IsNumber(item));
  return std::lround(item->valuedouble * scale);
}
const char *text(const cJSON *row, const char *name) {
  const cJSON *item = field(row, name);
  CHECK(cJSON_IsString(item));
  return item->valuestring;
}
uint8_t named(const char *value, const char *const *names, size_t count) {
  for (size_t index = 0; index < count; ++index)
    if (std::strcmp(value, names[index]) == 0) return static_cast<uint8_t>(index);
  CHECK(false);
  return 0;
}

// The inverse of formatShotStatsRow for the columns the header reads.
ShotLogRecord recordOf(const cJSON *row) {
  ShotLogRecord record{};
  record.id = static_cast<uint32_t>(scaled(row, "id", 1));
  record.durationDs = static_cast<uint16_t>(scaled(row, "durationS", 10));
  record.actualWeightCg = cJSON_IsNull(field(row, "actualG"))
      ? SHOT_LOG_WEIGHT_MISSING : static_cast<int16_t>(scaled(row, "actualG", 100));
  record.avgFlowCgS = cJSON_IsNull(field(row, "avgFlowGS"))
      ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(scaled(row, "avgFlowGS", 100));
  record.arrivalDs = cJSON_IsNull(field(row, "arrivalS"))
      ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(scaled(row, "arrivalS", 10));
  record.cutLineCg = cJSON_IsNull(field(row, "cutLineG"))
      ? SHOT_LOG_WEIGHT_MISSING : static_cast<int16_t>(scaled(row, "cutLineG", 100));
  record.goalWeightDg = static_cast<uint16_t>(scaled(row, "goalG", 10));
  record.fastBeforeDs = static_cast<uint16_t>(scaled(row, "fastBeforeS", 10));
  record.slowAfterDs = static_cast<uint16_t>(scaled(row, "slowAfterS", 10));
  record.hasWallTime = cJSON_IsTrue(field(row, "hasWallTime")) ? 1 : 0;
  record.endedAtLocalSec = static_cast<uint32_t>(scaled(row, "endedAtLocalSec", 1));
  record.verdict = cJSON_IsNull(field(row, "verdict"))
      ? 3 : named(text(row, "verdict"), kVerdicts, 3);
  record.shotType = named(text(row, "shotType"), kTypes, 3);
  shotLogSetPresetId(record, static_cast<uint8_t>(scaled(row, "presetId", 1)));
  bool detail = false;
  for (const ShotLogStopDetail candidate : kDetails)
    if (std::strcmp(text(row, "stopDetail"), shotLogStopDetailName(candidate)) == 0) {
      record.stopDetail = static_cast<uint8_t>(candidate);
      detail = true;
    }
  CHECK(detail);
  CHECK(std::strlen(text(row, "presetName")) < sizeof(record.presetName));
  CHECK(std::strlen(text(row, "scaleName")) < sizeof(record.scaleName));
  std::snprintf(record.presetName, sizeof(record.presetName), "%s", text(row, "presetName"));
  std::snprintf(record.scaleName, sizeof(record.scaleName), "%s", text(row, "scaleName"));
  // The stored reason must be the one the record derives.
  const char *reason = shotLogBadReason(record);
  const cJSON *bad = field(row, "badReason");
  CHECK(reason == nullptr ? cJSON_IsNull(bad) : std::strcmp(text(row, "badReason"), reason) == 0);
  return record;
}

std::string headerOf(const cJSON *rows, size_t window, uint8_t presetId) {
  work.snapshot.count = 0;
  for (const cJSON *row = rows->child; row != nullptr; row = row->next)
    work.snapshot.records[work.snapshot.count++] = recordOf(row);
  CHECK(formatStatsHeader(work, window, presetId));
  return work.header;
}

// --- The cases (regeneration only) -------------------------------------------

uint32_t seed = 12345;
uint32_t next(uint32_t range) {
  seed = seed * 1103515245U + 12345U;
  return (seed >> 16) % range;
}

void number(cJSON *row, const char *name, long value, double scale, bool missing = false) {
  if (missing) cJSON_AddNullToObject(row, name);
  else cJSON_AddNumberToObject(row, name, value / scale);
}

struct Shape {
  uint8_t presets = 3, scales = 2, badEvery = 0, untimedEvery = 0;
  bool clockless = false, sharedWindow = true, arrivalTies = false;
};

const char *const kPresetNames[] = {"House", "Decaf \"Swiss\"", "Ethiopia", "Kenya", "Back\\slash",
                                    "Tab\there", "Colombia", "Brazil", "Guji", "Sidamo",
                                    "Yirga", "Huila"};
const char *const kScaleNames[] = {"Lunar", "Pearl S", "Bookoo \"Themis\"", "Decent\\Scale",
                                   "Felicita Arc", "Timemore", "Eureka", "Skale",
                                   "Varia", "Brewista", "Hiroia", "Acaia\tPyxis"};

cJSON *rowsOf(size_t count, const Shape &shape) {
  cJSON *rows = cJSON_CreateArray();
  const uint32_t day = 20000;
  for (size_t index = 0; index < count; ++index) {
    cJSON *row = cJSON_CreateObject();
    ShotLogRecord probe{};
    const uint32_t id = static_cast<uint32_t>(count - index + 40);
    const uint8_t preset = static_cast<uint8_t>(1 + next(shape.presets));
    const bool bad = shape.badEvery && index % shape.badEvery == shape.badEvery - 1;
    const uint8_t badKind = static_cast<uint8_t>(index % 3);
    probe.durationDs = static_cast<uint16_t>(bad && badKind == 0 ? 80 + next(41) : 200 + next(150));
    probe.actualWeightCg = bad && badKind == 1 ? SHOT_LOG_WEIGHT_MISSING
                           : bad && badKind == 2 ? static_cast<int16_t>(50 + next(151))
                                                 : static_cast<int16_t>(3200 + next(900));
    const bool untimed = shape.untimedEvery && index % shape.untimedEvery == shape.untimedEvery / 2u;
    const uint16_t fast = shape.sharedWindow ? 250 : static_cast<uint16_t>(220 + 10 * next(5));
    const uint16_t slow = shape.sharedWindow ? 320 : static_cast<uint16_t>(300 + 10 * next(5));
    const uint16_t arrival = shape.arrivalTies ? static_cast<uint16_t>(270 + 5 * next(4))
                                               : static_cast<uint16_t>(200 + next(180));
    number(row, "id", id, 1);
    number(row, "durationS", probe.durationDs, 10);
    number(row, "actualG", probe.actualWeightCg, 100,
           shotLogWeightIsMissing(probe.actualWeightCg));
    number(row, "avgFlowGS", 100 + next(200), 100, next(7) == 0);
    number(row, "arrivalS", untimed && index % 2 ? 0 : arrival, 10, untimed && !(index % 2));
    const bool cutLine = next(3) != 0;
    number(row, "cutLineG", 3400 + next(300), 100, !cutLine);
    number(row, "goalG", next(9) == 0 ? 0 : 340 + 10 * next(5), 10);
    number(row, "fastBeforeS", fast, 10);
    number(row, "slowAfterS", slow, 10);
    const bool wall = !shape.clockless && next(5) != 0;
    cJSON_AddBoolToObject(row, "hasWallTime", wall);
    number(row, "endedAtLocalSec", wall ? (day + index / 4) * 86400L + next(80000) : 0, 1);
    const uint8_t verdict = static_cast<uint8_t>(next(3));
    cJSON_AddStringToObject(row, "verdict", kVerdicts[verdict]);
    cJSON_AddNumberToObject(row, "presetId", preset);
    cJSON_AddStringToObject(row, "presetName", kPresetNames[(preset - 1) % 12]);
    cJSON_AddStringToObject(row, "scaleName",
                            next(6) == 0 ? "" : kScaleNames[next(shape.scales)]);
    cJSON_AddStringToObject(row, "shotType", kTypes[next(4) == 0 ? 1 + next(2) : 0]);
    cJSON_AddStringToObject(row, "stopDetail", shotLogStopDetailName(kDetails[next(7)]));
    const char *reason = shotLogBadReason(probe);
    if (reason == nullptr) cJSON_AddNullToObject(row, "badReason");
    else cJSON_AddStringToObject(row, "badReason", reason);
    cJSON_AddItemToArray(rows, row);
  }
  return rows;
}

cJSON *caseOf(const char *name, size_t window, uint8_t presetId, cJSON *rows) {
  cJSON *item = cJSON_CreateObject();
  cJSON_AddStringToObject(item, "name", name);
  cJSON_AddNumberToObject(item, "window", static_cast<double>(window));
  cJSON_AddNumberToObject(item, "presetId", presetId);
  cJSON_AddItemToObject(item, "rows", rows);
  cJSON *header = cJSON_Parse(headerOf(rows, window, presetId).c_str());
  CHECK(header != nullptr);
  cJSON_AddItemToObject(item, "header", header);
  return item;
}

void regenerate(const char *path) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "version", 1);
  cJSON *cases = cJSON_AddArrayToObject(root, "cases");
  Shape mixed;
  cJSON_AddItemToArray(cases, caseOf("window10_mixed_presets", 10, 2, rowsOf(30, mixed)));
  Shape bad;
  bad.badEvery = 4;
  bad.untimedEvery = 5;
  bad.arrivalTies = true;
  bad.sharedWindow = false;
  cJSON_AddItemToArray(cases, caseOf("window50_bad_untimed_ties_unshared", 50, 9, rowsOf(70, bad)));
  Shape one;
  one.presets = 1;
  cJSON_AddItemToArray(cases, caseOf("window100_one_preset_130_rows", 100, 1, rowsOf(130, one)));
  Shape facets;
  facets.presets = 12;
  facets.scales = 12;
  cJSON_AddItemToArray(cases, caseOf("window10_facet_cap_escaping", 10, 5, rowsOf(60, facets)));
  Shape clockless;
  clockless.clockless = true;
  clockless.badEvery = 3;
  cJSON_AddItemToArray(cases, caseOf("window50_clockless", 50, 1, rowsOf(40, clockless)));
  Shape untimed;
  untimed.untimedEvery = 1;
  cJSON_AddItemToArray(cases, caseOf("window10_no_timed_shots", 10, 1, rowsOf(15, untimed)));
  cJSON_AddItemToArray(cases, caseOf("window10_empty", 10, 1, cJSON_CreateArray()));
  char *printed = cJSON_Print(root);
  FILE *file = std::fopen(path, "wb");
  CHECK(file != nullptr && printed != nullptr);
  CHECK(std::fputs(printed, file) >= 0 && std::fputc('\n', file) == '\n' && std::fclose(file) == 0);
  std::free(printed);
  cJSON_Delete(root);
}

void verify(const char *path) {
  FILE *file = std::fopen(path, "rb");
  CHECK(file != nullptr);
  std::string data;
  char chunk[4096];
  for (size_t read; (read = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) data.append(chunk, read);
  std::fclose(file);
  cJSON *root = cJSON_Parse(data.c_str());
  CHECK(root != nullptr && cJSON_GetObjectItemCaseSensitive(root, "version")->valueint == 1);
  size_t cases = 0;
  for (const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "cases")->child; item;
       item = item->next, ++cases) {
    const std::string header = headerOf(field(item, "rows"),
                                         static_cast<size_t>(scaled(item, "window", 1)),
                                         static_cast<uint8_t>(scaled(item, "presetId", 1)));
    cJSON *computed = cJSON_Parse(header.c_str());
    if (computed == nullptr || !cJSON_Compare(computed, field(item, "header"), true)) {
      std::fprintf(stderr, "case %s: header differs\n%s\n", text(item, "name"), header.c_str());
      std::exit(1);
    }
    cJSON_Delete(computed);
  }
  CHECK(cases >= 7);
  cJSON_Delete(root);
}

}  // namespace

// The text frames' escaper keeps valid UTF-8 and never passes an invalid byte.
void escaperReplacesInvalidUtf8() {
  char out[64];
  CHECK(escapeJsonString("caf\xC3\xA9 \xF0\x9F\x8D\xB5", out, sizeof(out)));
  CHECK(std::strcmp(out, "caf\xC3\xA9 \xF0\x9F\x8D\xB5") == 0);
  CHECK(escapeJsonString("BOOKO\xFF", out, sizeof(out)));
  CHECK(std::strcmp(out, "BOOKO\\ufffd") == 0);
  CHECK(escapeJsonString("cut\xE2\x82", out, sizeof(out)));
  CHECK(std::strcmp(out, "cut\\ufffd\\ufffd") == 0);
  CHECK(!escapeJsonString("\xF0\x9F\x8D\xB5", out, 4));
}

int main() {
  escaperReplacesInvalidUtf8();
  const char *regenerateFlag = std::getenv("STATS_HEADER_REGENERATE");
  if (regenerateFlag != nullptr && std::strcmp(regenerateFlag, "1") == 0) regenerate(STATS_HEADER_VECTORS);
  verify(STATS_HEADER_VECTORS);
  std::puts("stats_header_host_test: ok");
  return 0;
}

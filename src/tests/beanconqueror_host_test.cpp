// Beanconqueror machine endpoints: each vector case's Stats row, rebuilt as
// the stored record and curve, must give the exact Gaggiuino and GaggiMate
// answers in docs/schemas/beanconqueror-v1.vectors.json.
#include "../OpenBrewByWeightBeanconqueror.h"

#include <cJSON.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace openbrewbyweight;

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition);    \
      std::exit(1);                                                                         \
    }                                                                                       \
  } while (0)

namespace {

std::string readFile(const char *path) {
  FILE *file = std::fopen(path, "rb");
  CHECK(file != nullptr);
  std::string text;
  char chunk[4096];
  for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) text.append(chunk, n);
  std::fclose(file);
  return text;
}

std::vector<uint8_t> base64(const char *text) {
  static const std::string abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<uint8_t> out;
  uint32_t bits = 0;
  int count = 0;
  for (const char *c = text; *c && *c != '='; ++c) {
    bits = bits << 6 | static_cast<uint32_t>(abc.find(*c));
    if ((count += 6) >= 8) out.push_back(static_cast<uint8_t>(bits >> (count -= 8)));
  }
  return out;
}

double number(const cJSON *object, const char *key) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}
uint16_t ms(const cJSON *object, const char *key) {
  const double s = number(object, key);
  return std::isnan(s) ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(std::lround(s * 1000));
}
bool listed(const cJSON *list, int index) {
  for (const cJSON *item = list ? list->child : nullptr; item; item = item->next)
    if (item->valueint == index) return true;
  return false;
}

// The stored record and curve behind a Stats row.
void rebuild(const cJSON *row, ShotLogRecord &r, ShotCurveRecord &curve) {
  r = {};
  r.id = static_cast<uint32_t>(number(row, "id"));
  r.hasWallTime = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "hasWallTime"));
  r.endedAtUnixSec = static_cast<uint32_t>(number(row, "endedAtUnixSec"));
  r.durationDs = static_cast<uint16_t>(std::lround(number(row, "durationS") * 10));
  r.actualWeightCg = static_cast<int16_t>(std::lround(number(row, "actualG") * 100));
  r.doseDg = static_cast<uint16_t>(std::lround(number(row, "doseG") * 10));
  r.targetTempDeciC = static_cast<uint16_t>(number(row, "targetTempDeciC"));
  const double flow = number(row, "avgFlowGS"), drop = number(row, "firstDropS");
  r.avgFlowCgS = std::isnan(flow) ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(std::lround(flow * 100));
  r.firstDropDs = std::isnan(drop) ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(std::lround(drop * 10));
  r.sideFlags = shotLogPackRating(0, static_cast<uint8_t>(number(row, "rating")));
  shotLogSetPresetId(r, static_cast<uint8_t>(number(row, "presetId")));
  snprintf(r.presetName, sizeof(r.presetName), "%s",
           cJSON_GetObjectItemCaseSensitive(row, "presetName")->valuestring);
  resetShotCurveRecord(curve);
  const cJSON *at = cJSON_GetObjectItemCaseSensitive(row, "wAtMs")->child;
  const cJSON *cg = cJSON_GetObjectItemCaseSensitive(row, "wCg")->child;
  const cJSON *breaks = cJSON_GetObjectItemCaseSensitive(row, "wBreakBefore");
  const cJSON *held = cJSON_GetObjectItemCaseSensitive(row, "wHeld");
  for (int i = 0; at && cg; ++i, at = at->next, cg = cg->next)
    CHECK(appendShotCurvePoint(curve, static_cast<int16_t>(cg->valueint),
                               static_cast<uint32_t>(at->valueint), listed(breaks, i), listed(held, i)));
  curve.firstDrop.atMs = ms(row, "dropS");
  curve.ended.atMs = ms(row, "endS");
  curve.atm.atMs = ms(row, "atmS");
  curve.atmClearedMs = ms(row, "atmClearedS");
}

void checkJson(const BeanconquerorOut &out, const cJSON *expected) {
  CHECK(out.ok);
  cJSON *got = cJSON_ParseWithLength(reinterpret_cast<const char *>(out.data), out.used);
  CHECK(got != nullptr && cJSON_Compare(got, expected, true));
  cJSON_Delete(got);
}
void checkBytes(const BeanconquerorOut &out, const char *expected) {
  CHECK(out.ok && std::vector<uint8_t>(out.data, out.data + out.used) == base64(expected));
}

std::vector<uint8_t> buffer(40960);  // the network work buffer's statusJson
BeanconquerorFlowWork flowWork;

}  // namespace

int main() {
  const std::string text = readFile(BEANCONQUEROR_VECTORS);
  cJSON *doc = cJSON_Parse(text.c_str());
  CHECK(doc != nullptr && number(doc, "version") == 1);
  std::vector<BeanconquerorShot> good;
  for (const cJSON *c = cJSON_GetObjectItemCaseSensitive(doc, "cases")->child; c; c = c->next) {
    ShotLogRecord record;
    ShotCurveRecord curve;
    rebuild(cJSON_GetObjectItemCaseSensitive(c, "shot"), record, curve);
    BeanconquerorShot shot;
    const bool served = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "served"));
    CHECK(beanconquerorShotOf(record, shot) == served);
    if (!served) continue;
    good.push_back(shot);
    beanconquerorFlow(curve, shot, flowWork);
    int index = 0;
    for (const cJSON *f = cJSON_GetObjectItemCaseSensitive(c, "flowGS")->child; f; f = f->next, ++index) {
      const double v = cJSON_IsNull(f) ? 0 : f->valuedouble;
      CHECK(flowWork.tenths[index] == static_cast<int16_t>(std::floor(v * 10 + 0.5)));
      CHECK(flowWork.hundredths[index] == std::min(32767.0, std::floor(v * 100 + 0.5)));
    }
    const cJSON *gaggiuino = cJSON_GetObjectItemCaseSensitive(c, "gaggiuino");
    const cJSON *gaggimate = cJSON_GetObjectItemCaseSensitive(c, "gaggimate");
    BeanconquerorOut json{buffer.data(), buffer.size()};
    beanconquerorGaggiuinoShot(shot, curve, flowWork, json);
    checkJson(json, cJSON_GetObjectItemCaseSensitive(gaggiuino, "shot"));
    BeanconquerorOut entry{buffer.data(), buffer.size()};
    beanconquerorIndexEntry(entry, shot);
    CHECK(entry.used == BEANCONQUEROR_INDEX_ENTRY);
    checkBytes(entry, cJSON_GetObjectItemCaseSensitive(gaggimate, "indexEntry")->valuestring);
    BeanconquerorOut slog{buffer.data(), buffer.size()};
    beanconquerorSlog(slog, shot, curve, flowWork);
    checkBytes(slog, cJSON_GetObjectItemCaseSensitive(gaggimate, "slog")->valuestring);
  }
  // Indexes are newest first: the whole store, then a `limit` cut.
  std::sort(good.begin(), good.end(), [](const auto &a, const auto &b) { return a.id > b.id; });
  const cJSON *gm = cJSON_GetObjectItemCaseSensitive(doc, "gaggimate");
  BeanconquerorOut index{buffer.data(), buffer.size()};
  beanconquerorIndexHeader(index, static_cast<uint32_t>(good.size()), good[0].id + 1);
  for (const auto &shot : good) beanconquerorIndexEntry(index, shot);
  checkBytes(index, cJSON_GetObjectItemCaseSensitive(gm, "index")->valuestring);
  BeanconquerorOut recent{buffer.data(), buffer.size()};
  beanconquerorIndexHeader(recent, 2, 0);
  for (size_t i = 0; i < 2; ++i) beanconquerorIndexEntry(recent, good[i]);
  checkBytes(recent, cJSON_GetObjectItemCaseSensitive(gm, "recentLimit2")->valuestring);

  // The largest curve (1201 readings, 50 ms apart, yields past 200 g): its
  // .slog fits statusJson, and the JSON streams in chunks unchanged.
  ShotLogRecord record{};
  record.id = 9;
  record.durationDs = 600;
  record.actualWeightCg = 9999;
  record.targetTempDeciC = 1000;
  memset(record.presetName, 'x', sizeof(record.presetName) - 1);
  ShotCurveRecord curve;
  resetShotCurveRecord(curve);
  for (uint32_t i = 0; i < SHOT_CURVE_MAX_POINTS; ++i)
    CHECK(appendShotCurvePoint(curve, static_cast<int16_t>(i * 17), i * 50, false));
  BeanconquerorShot shot;
  CHECK(beanconquerorShotOf(record, shot));
  beanconquerorFlow(curve, shot, flowWork);
  BeanconquerorOut big{buffer.data(), buffer.size()};
  beanconquerorGaggiuinoShot(shot, curve, flowWork, big);
  CHECK(big.ok);
  // Streamed through a small buffer (HTTP chunks), the answer is the same.
  std::string streamed;
  uint8_t window[512];
  BeanconquerorOut chunks{window, sizeof(window)};
  chunks.context = &streamed;
  chunks.flush = [](void *to, const uint8_t *bytes, size_t length) {
    static_cast<std::string *>(to)->append(reinterpret_cast<const char *>(bytes), length);
    return true;
  };
  beanconquerorGaggiuinoShot(shot, curve, flowWork, chunks);
  CHECK(chunks.finish() && streamed == std::string(reinterpret_cast<const char *>(buffer.data()), big.used));
  BeanconquerorOut bigSlog{buffer.data(), buffer.size()};
  beanconquerorSlog(bigSlog, shot, curve, flowWork);
  CHECK(bigSlog.ok && bigSlog.used == BEANCONQUEROR_SLOG_HEADER + SHOT_CURVE_MAX_POINTS * BEANCONQUEROR_SLOG_SAMPLE);
  std::printf("beanconqueror: %zu served shots match; largest Gaggiuino shot %zu bytes\n",
              good.size(), big.used);
  cJSON_Delete(doc);
  return 0;
}

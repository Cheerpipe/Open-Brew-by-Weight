// The companion archive's link frames (plan 2A, X1), built by the firmware's
// own code: the record index, History and Stats senders (the fetch-only link
// session), the Stats row formatter, the trace frame, the in-place sealer and
// the format strings of the `rs` notice and the ping's hold answer. The
// `page_*` cases are the same senders on a browser's session: the Stats and
// History pages (sorts, filters, windows, paging, export and Home's fetch)
// the companion's replica must serve frame for frame over the same records
// (every member but its own tag, seq and request). They are pinned in
// docs/schemas/companion-records-v1.vectors.json, which the companion's sync
// and serving tests replay; COMPANION_RECORDS_REGENERATE=1 rewrites it.
{
  const assert = require('assert').strict, {spawnSync} = require('child_process'), zlib = require('zlib');
  const read = (file) => fs.readFileSync(path.join(sketchDir, file), 'utf8');
  const records = read('network/OpenBrewByWeightRecordStreams.inc');
  const home = read('network/OpenBrewByWeightHomeStream.inc');
  const statsPage = read('network/OpenBrewByWeightStatsPage.inc');
  const statsHeader = read('network/OpenBrewByWeightStatsHeader.inc');
  const recordsWork = read('network/OpenBrewByWeightCompanionRecords.inc');
  const companion = read('network/OpenBrewByWeightCompanion.inc');
  const link = read('network/OpenBrewByWeightCompanionLink.h');
  const uiStream = read('network/OpenBrewByWeightUiStream.inc');
  const transport = read('network/OpenBrewByWeightUiStreamTransport.inc');
  const networkSource = read('OpenBrewByWeightNetwork.cpp');
  const header = read('OpenBrewByWeightNetwork.h');
  const webhook = read('OpenBrewByWeightWebhook.h');
  const block = (text, marker) => {
    const out = blockAt(text, marker);
    assert(out, marker);
    return out;
  };
  const nested = (text, marker) => {
    const at = text.indexOf(marker);
    assert(at >= 0, marker);
    return text.slice(at, text.indexOf('\n  };', at) + 5);
  };
  const line = (text, pattern) => {
    const match = pattern.exec(text);
    assert(match, String(pattern));
    return match[0];
  };
  // The bodies the controller sends outside a record frame, as its code
  // spells them (a changed format string changes the vectors).
  const rsFormat = line(recordsWork, /"\\"t\\":\\"rs\\",[^\n]*?}"/);
  const holdFormat = line(companion,
      /"\\"t\\":\\"hold\\",\\"on\\":%s,\\"sync\\":false,\\"sh\\":%lu,\\"hi\\":%lu}"/);
  const native = String.raw`
#include <cassert>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightHistoryTypes.h"
#include "OpenBrewByWeightShotCurveTypes.h"
#include "OpenBrewByWeightBrewTypes.h"
namespace openbrewbyweight {
#include "network/OpenBrewByWeightStatsJson.inc"
}
using namespace openbrewbyweight;
constexpr int ESP_OK = 0, HTTPD_WS_TYPE_TEXT = 1;
${line(link, /constexpr size_t COMPANION_FRAME_BYTES = \d+;/)}
${line(link, /constexpr size_t COMPANION_MAC_HEX = \d+;/)}
${line(link, /constexpr size_t COMPANION_RECORD_FRAME_BYTES = \d+;/)}
${line(link, /constexpr size_t COMPANION_RECORD_ORIGIN = \d+;/)}
${line(uiStream, /constexpr size_t kStatsStreamFrameBytes = \d+;/)}
${line(transport, /constexpr uint32_t kStatsStreamPaceMs = \d+;/)}
constexpr uint32_t kUiStreamDispatchBudgetMs = 800;
uint32_t uiStreamDeadlineStartMs = 0, uiStreamDeadlineBudgetMs = UINT32_MAX;
const char *uiStreamSendKind = "";
bool uiStreamSendCleanAbort = false;
uint32_t now = 1;
uint32_t millis() { return now; }
struct TaskLockGuard { explicit TaskLockGuard(int) {} };
struct httpd_ws_frame_t { int type; bool final; uint8_t *payload; size_t len; };
uint8_t shotTraceDownloadChunk[4096];
// The MAC is stripped from the vectors; the companion checks its own.
bool companionFrameMacHex(const uint8_t *, char, const char *, size_t, char hex[COMPANION_MAC_HEX]) {
  memset(hex, '0', COMPANION_MAC_HEX);
  return true;
}
${block(link, 'inline size_t companionSealFrame(')}
${block(link, 'inline size_t companionSealRecord(')}
${link.slice(link.indexOf('struct CompanionBase64Stream {'), link.indexOf('\n};', link.indexOf('struct CompanionBase64Stream {')) + 3)}
${block(webhook, 'inline bool escapeJsonString(')}
${block(networkSource, 'struct StatsCurveHashCache {')};
${block(statsHeader, 'struct StatsHeaderWork {')};
${block(networkSource, 'struct StatsStreamCache : StatsHeaderWork {')};
${block(header, 'struct UiStreamFingerprints {')};
using HeldRow = UiStreamFingerprints::HeldRow;
constexpr size_t kHeldRows = UiStreamFingerprints::kHeldRows;
${block(uiStream, 'uint32_t uiStreamRowHash(')}
${block(uiStream, 'bool uiStreamHeld(')}
${block(uiStream, 'uint32_t uiStreamCurveHash(')}
struct NetworkWorkBuf {
  static constexpr size_t kStatusJson = 40960, kJsonItem = SHOT_CURVE_JSON_CAPACITY + 4096;
  char statusJson[kStatusJson]{}, jsonItem[kJsonItem]{}, curveJson[SHOT_CURVE_JSON_CAPACITY]{};
  StatsStreamCache statsCache;
  UiStreamFingerprints uiFingerprints[2]{};
  HistoryPage historyPage{};
} work;
auto *g_work = &work;
${block(networkSource, 'bool __attribute__((format(printf, 2, 3)))\nstatusJsonAppend(')}
${block(statsHeader, 'bool jsonScratchAppend(')}
${line(statsPage, /constexpr char kStatsRowColumns\[\] =[^;]*;/)}
// Fixture store: fixed epochs and counters, shots newest first (one bad,
// short), each with a small curve, and five activations.
// moved: the delta case's store, a new good shot 13, shot 9 rated
// again and shot 6 deleted.
constexpr uint32_t kShotsEpoch = 7001, kMovedEpoch = 7003, kHistoryEpoch = 7002, kBoot = 43;
constexpr size_t kShots = 12, kActivations = 5;
bool moved = false;
ShotLogRecord shotAt(uint32_t id) {
  ShotLogRecord r = {};
  r.id = id;
  r.bootId = kBoot;
  r.endedAtMs = 600000 + r.id * 1000;
  r.hasWallTime = r.id % 4 != 0;
  r.endedAtUnixSec = r.hasWallTime ? 1793000000 + r.id * 600 : 0;
  r.endedAtLocalSec = r.hasWallTime ? r.endedAtUnixSec - 10800 : 0;
  r.timezoneOffsetMinutesAtCommit = r.hasWallTime ? -180 : 0;
  r.durationDs = r.id == kShots ? 80 : 280 + r.id;
  r.goalWeightDg = 360;
  r.actualWeightCg = static_cast<int16_t>(3550 + r.id * 7);
  r.avgFlowCgS = r.id % 3 == 0 ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(120 + r.id);
  r.arrivalDs = r.id % 5 == 0 ? SHOT_LOG_METRIC_MISSING : static_cast<uint16_t>(270 + r.id);
  r.verdict = static_cast<uint8_t>(r.id % 3);
  r.fastBeforeDs = 250;
  r.slowAfterDs = 320;
  r.cutLineCg = SHOT_LOG_METRIC_MISSING;
  r.firstDropDs = SHOT_LOG_METRIC_MISSING;
  r.tareAtDs = SHOT_LOG_METRIC_MISSING;
  r.sideFlags = shotLogPackRating(0, static_cast<uint8_t>(moved && id == 9 ? 5 : r.id % 6));
  shotLogSetPresetId(r, r.id % 2 ? 1 : 2);
  snprintf(r.presetName, sizeof(r.presetName), "%s", r.id % 2 ? "House" : "Decaf \"Night\"");
  snprintf(r.scaleName, sizeof(r.scaleName), "%s", r.id % 4 ? "Lunar" : "");
  return r;
}
void capture(ShotStatsRecordsSnapshot &out) {
  out.epoch = moved ? kMovedEpoch : kShotsEpoch;
  out.count = 0;
  for (uint32_t id = moved ? kShots + 1 : kShots; id > 0; --id)
    if (!moved || id != 6) out.records[out.count++] = shotAt(id);
}
ShotCurveRead curveById(uint32_t id, ShotCurveRecord &curve, bool) {
  resetShotCurveRecord(curve);
  if (id == 0 || id > kShots) return ShotCurveRead::NONE;
  for (uint32_t k = 0; k < 160; ++k)
    appendShotCurvePoint(curve, static_cast<int16_t>(k * 120 + id), 900 + k * 250, k == 3);
  return ShotCurveRead::FOUND;
}
HistoryRecord activationAt(uint32_t id) {
  HistoryRecord r = {};
  r.id = id;
  r.type = static_cast<uint8_t>(id % 3);
  r.durationDs = static_cast<uint16_t>(40 + id * 11);
  r.flags = static_cast<uint8_t>((id == 2 ? 0 : HISTORY_FLAG_WALL_TIME) |
                                 (id == 4 ? HISTORY_FLAG_NO_WEIGHT : 0));
  r.endedAtUnixSec = r.flags & HISTORY_FLAG_WALL_TIME ? 1793000000 + id * 900 : 0;
  r.endedAtLocalSec = r.endedAtUnixSec ? r.endedAtUnixSec - 10800 : 0;
  return r;
}
// HistoryLog::copyPage over the fixture's activations (ids 1 oldest .. 5).
void historyPage(HistoryPage &page, size_t offset, size_t limit, ShotLogSortDir dir, uint8_t types) {
  page.total = page.count = 0;
  for (size_t step = 0; step < kActivations; ++step) {
    const HistoryRecord r = activationAt(static_cast<uint32_t>(
        dir == ShotLogSortDir::Desc ? kActivations - step : step + 1));
    if (types && !((types >> r.type) & 1U)) continue;
    if (page.total >= offset && page.count < limit) page.records[page.count++] = r;
    ++page.total;
  }
  page.start = offset < page.total ? offset : page.total;
  page.hasMore = page.start + page.count < page.total;
}
// Trace 1 is a deterministic 5,000-byte record (two chunks); 2 has none, 3 is busy.
ShotTraceRead streamTrace(uint32_t id, uint32_t, uint32_t, uint32_t, uint8_t *chunk, size_t capacity,
                          ShotTraceSink sink, void *context) {
  if (id == 2) return ShotTraceRead::NONE;
  if (id == 3) return ShotTraceRead::BUSY;
  for (size_t at = 0; at < 5000;) {
    const size_t n = std::min(capacity, size_t(5000) - at);
    for (size_t k = 0; k < n; ++k) chunk[k] = static_cast<uint8_t>((at + k) * 31 + 7);
    if (!sink(context, chunk, n, 0)) return ShotTraceRead::UNAVAILABLE;
    at += n;
  }
  return ShotTraceRead::FOUND;
}
std::vector<std::string> frames;
uint32_t sequence = 0;
const uint8_t key[32] = {};
struct OpenBrewByWeightNetwork {
  static constexpr size_t WEB_UI_CLIENT_ID_CAPACITY = 25;
${nested(header, '  struct UiStreamSession {')}
${nested(header, '  struct CompanionRecords {')}
${header.slice(header.indexOf('  struct RecordIndexBaseline {'), header.indexOf('  RecordIndexWork *recordIndexWork_'))}
  RecordIndexWork *recordIndexWork_ = nullptr;
  RecordIndexWork *recordIndexWork() { return recordIndexWork_ ? recordIndexWork_ : recordIndexWork_ = new RecordIndexWork{}; }
  NetworkWorkBuf *workBuf_ = &work;
  UiStreamSession uiStreams_[2];
  CompanionRecords records;
  CompanionRecords *companionRecords_ = &records;
  int dataMux_ = 0, server_ = 1;
  char activeWebUiClientId_[WEB_UI_CLIENT_ID_CAPACITY] = "owner";
  std::atomic<bool> uiStreamUrgent_{false};
  struct {
    uint32_t (*shotLogEpoch)() = +[] { return moved ? kMovedEpoch : kShotsEpoch; };
    void (*copyShotStatsRecords)(ShotStatsRecordsSnapshot &) = capture;
    ShotCurveRead (*copyShotCurveById)(uint32_t, ShotCurveRecord &, bool) = curveById;
    uint32_t (*historyEpoch)() = +[] { return kHistoryEpoch; };
    void (*copyHistoryPage)(HistoryPage &, size_t, size_t, ShotLogSortDir, uint8_t) = historyPage;
    void (*recordStoreCounters)(bool, uint32_t &, uint32_t &, RecordStoreMarks &) =
        +[](bool shots, uint32_t &generation, uint32_t &next, RecordStoreMarks &marks) {
          generation = shots ? 2400 : 1300;
          next = shots ? kShots + 1 + moved : kActivations + 1;
          marks.clears = shots ? 1 : 0;
          marks.goneCount = shots ? 1 : 0;
          marks.gone[0] = shots ? 999 : 0;
        };
    ShotTraceRead (*streamShotTrace)(uint32_t, uint32_t, uint32_t, uint32_t, uint8_t *, size_t,
                                     ShotTraceSink, void *) = streamTrace;
  } callbacks_;
  bool companionRecordsSession(const UiStreamSession &s) const { return &s == &records.session; }
  // The real in-place sealer; the vector keeps each frame after its MAC and dot.
  bool companionRecordsSend(size_t used, bool wrap, bool &deferArm) {
    deferArm = false;
    size_t length = 0;
    const size_t start = companionSealRecord(key, ++sequence, wrap, g_work->statusJson, used,
                                             NetworkWorkBuf::kStatusJson, length);
    assert(start != SIZE_MAX);
    frames.emplace_back(g_work->statusJson + start + COMPANION_MAC_HEX + 1, length - COMPANION_MAC_HEX - 1);
    return true;
  }
  size_t formatShotStatsRow(NetworkWorkBuf &work, const ShotLogRecord &record,
                            const ShotCurveRecord *curve);
  bool sendUiStreamFrame(UiStreamSession &, size_t, bool &);
  bool sendHistoryStream(UiStreamSession &, const ControlStatusSnapshot &);
  bool sendStatsStream(UiStreamSession &, const ControlStatusSnapshot &);
  bool sendRecordsIndex(UiStreamSession &, const ControlStatusSnapshot &);
  bool companionTraceFrame();
};
// sendUiStreamFrame's browser path, which no link case reaches.
int httpd_ws_send_frame_async(int, int, httpd_ws_frame_t *frame) {
  frames.emplace_back(reinterpret_cast<const char *>(frame->payload), frame->len);
  return ESP_OK;
}
${block(statsHeader, 'bool appendStatsDensity(')}
${block(statsHeader, 'bool formatStatsHeader(')}
${block(statsPage, 'size_t OpenBrewByWeightNetwork::formatShotStatsRow(')}
${block(home, 'bool OpenBrewByWeightNetwork::sendUiStreamFrame(')}
${block(records, 'bool OpenBrewByWeightNetwork::sendHistoryStream(')}
${block(records, 'bool OpenBrewByWeightNetwork::sendStatsStream(')}
${block(records, 'bool OpenBrewByWeightNetwork::sendRecordsIndex(')}
${block(recordsWork, 'bool companionTraceSink(')}
${block(recordsWork, 'bool OpenBrewByWeightNetwork::companionTraceFrame(')}
// A plain link body (rs, the ping's answer) through the real 4 KiB sealer.
void plain(const char *body) {
  char out[COMPANION_FRAME_BYTES + 1];
  const size_t length = companionSealFrame(key, ++sequence, body, out, sizeof(out));
  assert(length != 0);
  frames.emplace_back(out + COMPANION_MAC_HEX + 1, length - COMPANION_MAC_HEX - 1);
}
int main() {
  static OpenBrewByWeightNetwork network;
  ControlStatusSnapshot control = {};
  control.bootId = kBoot;
  // Each request sets what companionLinkRecordRequest records for it (its
  // parsing is host-tested in companion_link_host_test).
  auto &session = network.records.session;
  session.recordsKey = 1;  // a link session keys its baselines
  auto drain = [&](const char *name, auto send) {
    frames.clear();
    for (int i = 0; i < 20 && send(); ++i) now += 100;
    std::printf("CASE %s\n", name);
    for (const auto &frame : frames) std::printf("%s\n", frame.c_str());
  };
  session.recordsShots = true;
  session.recordsShotsEpoch = 0;
  drain("index_shots", [&] { return session.recordsShots && network.sendRecordsIndex(session, control); });
  session.recordsShots = true;
  session.recordsShotsEpoch = kShotsEpoch;
  drain("index_shots_unchanged", [&] { return session.recordsShots && network.sendRecordsIndex(session, control); });
  session.recordsHistory = true;
  session.recordsHistoryEpoch = 0;
  drain("index_history", [&] { return session.recordsHistory && network.sendRecordsIndex(session, control); });
  session.statsFetch = true;
  session.statsFetchOffset = 0;
  session.statsFetchLimit = kShots;
  session.statsFetchSort = ShotLogSort::Date;
  session.statsFetchDir = ShotLogSortDir::Desc;
  session.statsFetchFilter = ShotListFilter{};
  session.statsFetchFilter.includeBad = true;
  session.statsFetchRequest = 7;
  drain("fetch_shots", [&] { return session.statsFetch && network.sendStatsStream(session, control); });
  session.historyFetch = true;
  session.historyFetchOffset = 0;
  session.historyFetchLimit = kActivations;
  session.historyFetchDir = ShotLogSortDir::Desc;
  session.historyFetchTypes = 0;
  session.historyFetchRequest = 8;
  drain("fetch_history", [&] { return session.historyFetch && network.sendHistoryStream(session, control); });
  const uint32_t traces[3][2] = {{1, 0}, {2, 0}, {3, 0}};
  const char *names[3] = {"trace_ok", "trace_none", "trace_busy"};
  for (size_t i = 0; i < 3; ++i) {
    network.records.trace = true;
    network.records.traceId = traces[i][0];
    network.records.traceBoot = kBoot;
    network.records.traceEnd = 600000 + traces[i][0] * 1000;
    drain(names[i], [&] { return network.records.trace && network.companionTraceFrame(); });
  }
  char body[96];
  snprintf(body, sizeof(body), ${rsFormat}, static_cast<unsigned long>(kShotsEpoch),
           static_cast<unsigned long>(kHistoryEpoch));
  drain("notice", [&] { static bool done; return !done && (plain(body), done = true); });
  snprintf(body, sizeof(body), ${holdFormat}, "false", static_cast<unsigned long>(kShotsEpoch),
           static_cast<unsigned long>(kHistoryEpoch));
  drain("ping_answer", [&] { static bool done; return !done && (plain(body), done = true); });
  // Browser pages (a screen's own session): what the companion's replica
  // serves from the same records. Each case starts a fresh session.
  auto &screen = network.uiStreams_[0];
  auto fresh = [&] {
    screen = OpenBrewByWeightNetwork::UiStreamSession{};
    snprintf(screen.clientId, sizeof(screen.clientId), "owner");
  };
  auto stats = [&](const char *name, size_t offset, size_t limit, ShotLogSort sort, ShotLogSortDir dir,
                   ShotListFilter filter, size_t window, uint8_t active) {
    fresh();
    control.bbwPresetId = active;
    screen.statsOn = true;
    screen.statsOffset = offset;
    screen.statsLimit = limit;
    screen.statsSort = sort;
    screen.statsDir = dir;
    screen.statsFilter = filter;
    screen.statsWindow = window;
    screen.statsRequest = 1;
    drain(name, [&] { network.sendStatsStream(screen, control); return screen.statsPaging; });
  };
  ShotListFilter none, fast, presetBad, lunar, bad;
  fast.results = 1U << 1;
  presetBad.presetCount = 1;
  presetBad.presets[0] = 2;
  presetBad.includeBad = true;
  lunar.scaleCount = 1;
  lunar.scales[0] = shotLogScaleKey(shotAt(1));
  bad.includeBad = true;
  stats("page_stats", 0, 10, ShotLogSort::Date, ShotLogSortDir::Desc, none, 10, 1);
  stats("page_stats_rating_asc", 2, 5, ShotLogSort::Rating, ShotLogSortDir::Asc, none, 10, 1);
  stats("page_stats_fast_window_50", 0, 10, ShotLogSort::Date, ShotLogSortDir::Desc, fast, 50, 1);
  stats("page_stats_preset_bad", 0, 10, ShotLogSort::Rating, ShotLogSortDir::Desc, presetBad, 10, 1);
  stats("page_stats_scale", 0, 10, ShotLogSort::Date, ShotLogSortDir::Asc, lunar, 100, 1);
  stats("page_stats_active_2", 0, 10, ShotLogSort::Date, ShotLogSortDir::Desc, none, 10, 2);
  stats("page_stats_scroll", 10, 10, ShotLogSort::Date, ShotLogSortDir::Desc, none, 10, 1);
  // Fetches: a scroll or export window on a screen holding Stats (its header
  // follows the fetch's window) and Home's rows-only hero fetch.
  auto statsFetch = [&](const char *name, bool view, size_t offset, size_t limit, ShotListFilter filter,
                        size_t window) {
    fresh();
    control.bbwPresetId = 1;
    screen.statsOn = view;
    screen.statsResync = false;
    screen.statsBoot = kBoot;
    screen.statsFetch = true;
    screen.statsFetchOffset = offset;
    screen.statsFetchLimit = limit;
    screen.statsFetchFilter = filter;
    screen.statsFetchWindow = window;
    screen.statsFetchRequest = 3;
    drain(name, [&] { network.sendStatsStream(screen, control); return screen.statsFetch; });
  };
  statsFetch("page_stats_export", true, 0, 50, bad, 50);
  statsFetch("page_stats_home_fetch", false, 0, 2, none, 10);
  auto history = [&](const char *name, bool fetch, size_t offset, size_t limit, ShotLogSortDir dir,
                     uint8_t types) {
    fresh();
    screen.historyOn = !fetch;
    screen.historyResync = !fetch;
    screen.historyBoot = fetch ? kBoot : 0;
    screen.historyFetch = fetch;
    (fetch ? screen.historyFetchOffset : screen.historyOffset) = offset;
    (fetch ? screen.historyFetchLimit : screen.historyLimit) = limit;
    (fetch ? screen.historyFetchDir : screen.historyDir) = dir;
    (fetch ? screen.historyFetchTypes : screen.historyTypes) = types;
    (fetch ? screen.historyFetchRequest : screen.historyRequest) = 4;
    drain(name, [&] { network.sendHistoryStream(screen, control); return false; });
  };
  history("page_history", false, 0, 20, ShotLogSortDir::Desc, 0);
  history("page_history_asc_types", false, 1, 2, ShotLogSortDir::Asc, (1U << 1) | (1U << 0));
  history("page_history_fetch", true, 2, 2, ShotLogSortDir::Desc, 0);
  // The store moved after index_shots: an opted-in request from its epoch
  // gets only the difference.
  moved = true;
  session.recordsShots = true;
  session.recordsShotsEpoch = kShotsEpoch;
  session.recordsShotsDelta = true;
  drain("index_shots_delta", [&] { return session.recordsShots && network.sendRecordsIndex(session, control); });
  // The two windows the companion's archive then fetches: shot 9 at position 4, new shot 13 at 0.
  for (const uint32_t offset : {4u, 0u}) {
    session.statsFetch = true;
    session.statsFetchOffset = offset;
    session.statsFetchLimit = 1;
    session.statsFetchRequest = 9 + !offset;
    drain(offset ? "fetch_shot_9_moved" : "fetch_shot_13_moved",
          [&] { return session.statsFetch && network.sendStatsStream(session, control); });
  }
}
`;
  const directory = path.resolve(sketchDir, '..', 'temp', 'ai_temp_companion_records');
  fs.mkdirSync(directory, {recursive: true});
  const binary = path.join(directory, 'vectors-' + process.pid);
  const compiled = buildHostProgram(binary, native,
    ['-DOPEN_BREW_BY_WEIGHT_HOST_TEST', '-Wno-unknown-attributes', '-I' + sketchDir]);
  assert.equal(compiled.status, 0, compiled.error?.message || compiled.stderr);
  const run = spawnSync(binary, [], {encoding: 'utf8'});
  fs.unlinkSync(binary);
  assert.equal(run.status, 0, run.stderr);
  const produced = {};
  let current = null;
  for (const text of run.stdout.split('\n').filter(Boolean)) {
    if (text.startsWith('CASE ')) produced[current = text.slice(5)] = [];
    else produced[current].push(text);
  }
  const requests = {
    index_shots: {t: 'rx', s: 'shots', ep: 0},
    index_shots_unchanged: {t: 'rx', s: 'shots', ep: 7001},
    index_history: {t: 'rx', s: 'history', ep: 0},
    fetch_shots: {t: 'rf', s: 'shots', o: 0, n: 12, r: 7},
    fetch_history: {t: 'rf', s: 'history', o: 0, n: 5, r: 8},
    trace_ok: {t: 'rt', id: 1, b: 43, e: 601000},
    trace_none: {t: 'rt', id: 2, b: 43, e: 602000},
    trace_busy: {t: 'rt', id: 3, b: 43, e: 603000},
    notice: null,
    ping_answer: {t: 'ping', up: false},
    page_stats: {op: 'stats', on: true, request: 1, offset: 0, limit: 10, sort: 'date', dir: 'desc', active: 1},
    page_stats_rating_asc: {op: 'stats', on: true, request: 1, offset: 2, limit: 5, sort: 'rating', dir: 'asc',
      active: 1},
    page_stats_fast_window_50: {op: 'stats', on: true, request: 1, offset: 0, limit: 10, sort: 'date',
      dir: 'desc', filter: {result: ['fast']}, window: 50, active: 1},
    page_stats_preset_bad: {op: 'stats', on: true, request: 1, offset: 0, limit: 10, sort: 'rating',
      dir: 'desc', filter: {preset: [2], bad: true}, active: 1},
    page_stats_scale: {op: 'stats', on: true, request: 1, offset: 0, limit: 10, sort: 'date', dir: 'asc',
      filter: {scale: [(zlib.crc32(Buffer.from('Lunar')) ^ 0xFFFFFFFF) >>> 0]}, window: 100, active: 1},
    page_stats_active_2: {op: 'stats', on: true, request: 1, offset: 0, limit: 10, sort: 'date', dir: 'desc',
      active: 2},
    page_stats_scroll: {op: 'stats', on: true, request: 1, offset: 10, limit: 10, sort: 'date', dir: 'desc',
      active: 1},
    page_stats_export: {op: 'stats', on: true, fetch: true, request: 3, offset: 0, limit: 50, sort: 'date',
      dir: 'desc', filter: {bad: true}, window: 50, view: true, active: 1},
    page_stats_home_fetch: {op: 'stats', on: true, fetch: true, request: 3, offset: 0, limit: 2, sort: 'date',
      dir: 'desc', view: false, active: 1},
    page_history: {op: 'history', on: true, request: 4, offset: 0, limit: 20, dir: 'desc'},
    page_history_asc_types: {op: 'history', on: true, request: 4, offset: 1, limit: 2, dir: 'asc',
      types: ['shot', 'rinse']},
    page_history_fetch: {op: 'history', on: true, fetch: true, request: 4, offset: 2, limit: 2, dir: 'desc',
      types: []},
    index_shots_delta: {t: 'rx', s: 'shots', ep: 7001, d: 1},
    fetch_shot_9_moved: {t: 'rf', s: 'shots', o: 4, n: 1, r: 9},
    fetch_shot_13_moved: {t: 'rf', s: 'shots', o: 0, n: 1, r: 10},
  };
  assert.deepEqual(Object.keys(produced), Object.keys(requests));
  const vectors = {
    version: 1,
    note: 'Controller frames as the firmware builds them (q kept, MAC and dot removed), ' +
        'each a JSON text exactly as sent; request is the companion frame that asked (without q), ' +
        'null for a notice. page_* cases are a screen\'s ui-stream frames for the ui-stream op in ' +
        'request (active: the active preset; view: the screen holds Stats), over the records of ' +
        'fetch_shots and fetch_history. Regenerate with COMPANION_RECORDS_REGENERATE=1.',
    cases: Object.entries(requests).map(([name, request]) => ({name, request, frames: produced[name]})),
  };
  for (const item of vectors.cases) {
    assert(item.frames.length > 0, item.name);
    for (const frame of item.frames) JSON.parse(frame);
  }
  const frameOf = (name, i = 0) => JSON.parse(vectors.cases.find((c) => c.name === name).frames[i]);
  // The pieces the companion relies on.
  assert.deepEqual([frameOf('index_shots').f.gen, frameOf('index_shots').f.next], [2400, 13]);
  assert.deepEqual([frameOf('index_shots').f.clr, frameOf('index_shots').f.gone], [1, [999]],
      'D8: the index names the Clear count and the ids the user deleted');
  assert.equal(frameOf('index_shots').f.rows.length, 24, 'every shot, the bad one included');
  assert(!('rows' in frameOf('index_shots_unchanged').f) && !('sum' in frameOf('index_shots_unchanged').f));
  // `sum` is zlib's CRC-32 of the whole index as little-endian id/revision
  // pairs; the delta rebuilds the moved store's index from index_shots.
  const sum = (rows) => {
    const bytes = Buffer.alloc(rows.length * 4);
    rows.forEach((value, i) => bytes.writeUInt32LE(value, i * 4));
    return zlib.crc32(bytes);
  };
  const full = frameOf('index_shots').f, delta = frameOf('index_shots_delta').f;
  assert.equal(full.sum, sum(full.rows));
  assert.equal(frameOf('index_history').f.sum, sum(frameOf('index_history').f.rows));
  assert(!('rows' in delta) && delta.base === 7001 && delta.epoch === 7003 && delta.next === 14);
  assert.deepEqual([delta.up.filter((_, i) => i % 2 === 0), delta.del], [[13, 9], [6]]);
  const pairs = new Map();
  for (let i = 0; i < full.rows.length; i += 2) pairs.set(full.rows[i], full.rows[i + 1]);
  assert.notEqual(delta.up[3], pairs.get(9), 'the new rating moves the revision');
  for (const id of delta.del) pairs.delete(id);
  for (let i = 0; i < delta.up.length; i += 2) pairs.set(delta.up[i], delta.up[i + 1]);
  assert.equal(delta.sum, sum([...pairs].sort((a, b) => b[0] - a[0]).flat()));
  for (const [name, id, rating] of [['fetch_shot_9_moved', 9, 5], ['fetch_shot_13_moved', 13, 1]]) {
    const page = frameOf(name).f, cols = page.cols.split(',');
    assert.deepEqual([page.epoch, page.rows.length, page.rows[0][cols.indexOf('id')],
      page.rows[0][cols.indexOf('rating')]], [7003, 1, id, rating], name);
  }
  assert(vectors.cases.find((c) => c.name === 'fetch_shots').frames.length > 1, 'the page spans frames');
  assert(frameOf('fetch_shots').f.cols && Number.isInteger(frameOf('fetch_shots').f.good));
  assert(!('stats' in frameOf('fetch_shots').f), 'the link never gets the Stats header');
  assert.deepEqual(['ok', 'none', 'busy'].map((r, i) => frameOf(['trace_ok', 'trace_none', 'trace_busy'][i]).r),
      ['ok', 'none', 'busy']);
  assert.equal(Buffer.from(frameOf('trace_ok').d, 'base64').length, 5000);
  assert.deepEqual([frameOf('ping_answer').sh, frameOf('ping_answer').hi], [7001, 7002],
      'the ping answer carries both store epochs (the replica heartbeat)');
  const page = (name) => vectors.cases.find((c) => c.name === name).frames.map((f) => JSON.parse(f));
  assert(page('page_stats')[0].stats && page('page_stats')[0].snapshot, 'a standing Stats page has its header');
  assert(!('stats' in page('page_stats_home_fetch')[0]), 'Home\'s rows-only fetch has no header');
  assert.notDeepEqual(page('page_stats')[0].stats.dens, page('page_stats_active_2')[0].stats.dens,
      'the header\'s second density follows the active preset');
  assert(page('page_stats_export').length > 1 && page('page_stats_export').at(-1).more === false);
  assert.deepEqual(page('page_history_asc_types')[0].records.map((r) => r.id), [3, 4]);
  assert(page('page_history')[0].records.some((r) => r.noWeight === true));
  const file = path.join(sketchDir, '..', 'docs', 'schemas', 'companion-records-v1.vectors.json');
  const text = JSON.stringify(vectors, null, 2) + '\n';
  if (process.env.COMPANION_RECORDS_REGENERATE === '1') fs.writeFileSync(file, text);
  assert.equal(fs.readFileSync(file, 'utf8'), text,
      'docs/schemas/companion-records-v1.vectors.json matches the firmware (COMPANION_RECORDS_REGENERATE=1)');
}

// HTTP mapping of the shot trace routes (OpenBrewByWeightShotTraceRoutes.inc)
// over shimmed httpd calls: status codes, cache headers, chunking, a stream
// cut short, the vocabulary and the Shot storage JSON. The persistence side
// (busy gates, pending records, identity, recycling) is TPW17-TPW19.

#include "../OpenBrewByWeightShotCurveTypes.h"
#include "../OpenBrewByWeightShotTrace.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NOT_FOUND = 0x105,
              ESP_ERR_HTTPD_RESULT_TRUNC = 0xb004;
constexpr const char *STATUS_OK = "200", *STATUS_NOT_FOUND = "404", *STATUS_CONFLICT = "409",
                     *STATUS_UNPROCESSABLE = "422", *STATUS_UNAVAILABLE = "503",
                     *STATUS_NOT_MODIFIED = "304", *JSON_CONTENT_TYPE = "application/json";

struct httpd_req_t {
  std::string query;  // Without '?'; "-" for none.
  std::map<std::string, std::string> requestHeaders, headers;
  std::string status = "200", body;
  int chunks = 0, failChunk = 0;
  bool ended = false;
};

int httpd_req_get_url_query_str(httpd_req_t *request, char *out, size_t capacity) {
  if (request->query == "-") return ESP_ERR_NOT_FOUND;
  snprintf(out, capacity, "%s", request->query.c_str());
  return request->query.size() < capacity ? ESP_OK : ESP_ERR_HTTPD_RESULT_TRUNC;
}
int httpd_query_key_value(const char *query, const char *key, char *out, size_t capacity) {
  const std::string q = std::string("&") + query, k = std::string("&") + key + "=";
  const size_t at = q.find(k);
  if (at == std::string::npos) return ESP_ERR_NOT_FOUND;
  const std::string value = q.substr(at + k.size(), q.find('&', at + 1) - at - k.size());
  if (value.size() >= capacity) return ESP_ERR_HTTPD_RESULT_TRUNC;
  snprintf(out, capacity, "%s", value.c_str());
  return ESP_OK;
}
int httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *out,
                                size_t capacity) {
  const auto it = request->requestHeaders.find(name);
  if (it == request->requestHeaders.end()) return ESP_ERR_NOT_FOUND;
  snprintf(out, capacity, "%s", it->second.c_str());
  return ESP_OK;
}
int httpd_resp_set_type(httpd_req_t *request, const char *type) {
  request->headers["Content-Type"] = type;
  return ESP_OK;
}
int httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value) {
  request->headers[name] = value;
  return ESP_OK;
}
int httpd_resp_set_status(httpd_req_t *request, const char *status) {
  request->status = status;
  return ESP_OK;
}
int httpd_resp_send(httpd_req_t *request, const char *data, size_t length) {
  if (data) request->body.append(data, length);
  request->ended = true;
  return ESP_OK;
}
int httpd_resp_send_chunk(httpd_req_t *request, const char *data, size_t length) {
  if (++request->chunks == request->failChunk) return ESP_FAIL;
  if (data == nullptr) request->ended = true;
  else request->body.append(data, length);
  return ESP_OK;
}

namespace openbrewbyweight {
void shotTraceWriteBack(const void *, size_t) {}
void shotTraceLogRecoveryDropped(uint32_t) {}
void shotTraceNoteProfileEvent(ScaleProfileEvent, uint32_t, float, uint32_t, uint32_t) {}
void shotTraceNoteDroppedFrame(bool) {}

// The fake pool record: a 9000-byte trace in chunks of the caller's size.
ShotTraceRead fakeResult = ShotTraceRead::FOUND;
uint32_t fakeCrc = 0xA1B2C3D4U, seenKnown = 0, seenIdentity[3] = {};
size_t fakeCutAfter = SIZE_MAX;
ShotTraceRead fakeStream(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs, uint32_t knownCrc,
                         uint8_t *chunk, size_t capacity, ShotTraceSink sink, void *context) {
  seenIdentity[0] = shotId;
  seenIdentity[1] = bootId;
  seenIdentity[2] = endedAtMs;
  seenKnown = knownCrc;
  if (fakeResult != ShotTraceRead::FOUND) return fakeResult;
  if (knownCrc == fakeCrc) return ShotTraceRead::NOT_MODIFIED;
  for (size_t at = 0, n = 0; at < 9000; at += capacity, ++n) {
    if (n == fakeCutAfter) return ShotTraceRead::UNAVAILABLE;
    const size_t piece = 9000 - at < capacity ? 9000 - at : capacity;
    memset(chunk, static_cast<int>(n), piece);
    if (!sink(context, chunk, piece, fakeCrc)) return ShotTraceRead::UNAVAILABLE;
  }
  return ShotTraceRead::FOUND;
}
ShotStorageStats fakeStorage = {};
ShotTraceRead fakeStorageResult = ShotTraceRead::FOUND;
ShotTraceRead fakeCopyStorage(ShotStorageStats &out, uint32_t *tags, size_t capacity) {
  if (tags == nullptr || capacity < 1000) return ShotTraceRead::UNAVAILABLE;
  out = fakeStorage;
  return fakeStorageResult;
}

class OpenBrewByWeightNetwork {
 public:
  struct Callbacks {
    ShotTraceRead (*streamShotTrace)(uint32_t, uint32_t, uint32_t, uint32_t, uint8_t *, size_t,
                                     ShotTraceSink, void *) = fakeStream;
    ShotTraceRead (*copyShotStorage)(ShotStorageStats &, uint32_t *, size_t) = fakeCopyStorage;
  } callbacks_;
  static OpenBrewByWeightNetwork *instance_;
  static esp_err_t shotTraceDownloadHandler(httpd_req_t *request);
  static esp_err_t shotTraceVocabularyHandler(httpd_req_t *request);
  static esp_err_t shotStorageHandler(httpd_req_t *request);
  static esp_err_t sendJson(httpd_req_t *request, const char *status, const char *json) {
    request->status = status;
    request->headers["Cache-Control"] = "no-store";
    request->body = json;
    request->ended = true;
    return ESP_OK;
  }
  static esp_err_t sendError(httpd_req_t *request, const char *status, const char *error,
                             const char *) {
    return sendJson(request, status, (std::string("{\"error\":\"") + error + "\"}").c_str());
  }
};
OpenBrewByWeightNetwork *OpenBrewByWeightNetwork::instance_ = nullptr;

#include "../diagnostics/OpenBrewByWeightShotTraceRoutes.inc"
}  // namespace openbrewbyweight

using namespace openbrewbyweight;

int failures = 0;
#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      std::cerr << __func__ << ":" << __LINE__ << ": check failed: " << #condition << "\n"; \
      ++failures;                                                                             \
    }                                                                                         \
  } while (false)

httpd_req_t traceRequest(const std::string &query) {
  httpd_req_t request;
  request.query = query;
  (void)OpenBrewByWeightNetwork::shotTraceDownloadHandler(&request);
  return request;
}

void aPoolRecordStreamsImmutableInChunks() {
  fakeResult = ShotTraceRead::FOUND;
  fakeCutAfter = SIZE_MAX;
  const httpd_req_t r = traceRequest("id=12&boot=7&end=345678");
  CHECK(seenIdentity[0] == 12 && seenIdentity[1] == 7 && seenIdentity[2] == 345678);
  CHECK(r.status == "200" && r.ended && r.body.size() == 9000 && r.chunks == 4);
  CHECK(r.body[0] == 0 && r.body[8999] == 2);  // 4096-byte chunks, then the end.
  CHECK(r.headers.at("ETag") == "\"a1b2c3d4\"");
  CHECK(r.headers.at("Cache-Control") == "private, max-age=31536000, immutable");
  CHECK(r.headers.at("Content-Type") == "application/octet-stream");
  // The browser's copy is current: 304 with the same validators, no body.
  httpd_req_t again;
  again.query = "id=12&boot=7&end=345678";
  again.requestHeaders["If-None-Match"] = "\"a1b2c3d4\"";
  (void)OpenBrewByWeightNetwork::shotTraceDownloadHandler(&again);
  CHECK(seenKnown == 0xA1B2C3D4U && again.status == "304" && again.body.empty() &&
        again.headers.at("ETag") == "\"a1b2c3d4\"");
  // A malformed validator is ignored.
  httpd_req_t odd;
  odd.query = "id=12&boot=7&end=345678";
  odd.requestHeaders["If-None-Match"] = "W/x";
  (void)OpenBrewByWeightNetwork::shotTraceDownloadHandler(&odd);
  CHECK(seenKnown == 0 && odd.status == "200");
}

void aStreamCutShortIsABrokenDownload() {
  fakeResult = ShotTraceRead::FOUND;
  fakeCutAfter = 1;  // The ring recycled it, or a shot started.
  const httpd_req_t r = traceRequest("id=12&boot=7&end=345678");
  CHECK(r.body.size() == 4096 && !r.ended);
  fakeCutAfter = SIZE_MAX;
  httpd_req_t lost;
  lost.query = "id=12&boot=7&end=345678";
  lost.failChunk = 2;  // The client went away.
  CHECK(OpenBrewByWeightNetwork::shotTraceDownloadHandler(&lost) == ESP_FAIL && !lost.ended);
}

void refusalsMapToTheirStatus() {
  fakeCutAfter = SIZE_MAX;
  const std::pair<ShotTraceRead, const char *> cases[] = {
      {ShotTraceRead::NONE, "404"}, {ShotTraceRead::BUSY, "409"},
      {ShotTraceRead::PENDING, "409"}, {ShotTraceRead::UNAVAILABLE, "503"}};
  for (const auto &c : cases) {
    fakeResult = c.first;
    const httpd_req_t r = traceRequest("id=0&boot=41&end=0");
    CHECK(r.status == c.second && r.headers.at("Cache-Control") == "no-store");
  }
  fakeResult = ShotTraceRead::BUSY;
  CHECK(traceRequest("id=0&boot=41&end=0").body.find("SHOT_TRACE_BUSY") != std::string::npos);
  fakeResult = ShotTraceRead::PENDING;
  CHECK(traceRequest("id=3&boot=41&end=9").body.find("SHOT_TRACE_SAVING") != std::string::npos);
  fakeResult = ShotTraceRead::FOUND;
  CHECK(traceRequest("id=1&boot=2&end=4294967295").status == "200" && seenIdentity[2] == UINT32_MAX);
  for (const char *bad : {"id=1&boot=2", "id=-1&boot=2&end=3", "id=1&boot=x&end=3",
                          "id=1&boot=2&end=99999999999", "id=1&boot=2&end=4294967296",
                          "id=1&boot=2&end=3&pad=" \
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})
    CHECK(traceRequest(bad).status == "422");
}

void withoutAQueryTheNewestTraceComesFromRam() {
  const httpd_req_t empty = traceRequest("-");
  CHECK(empty.status == "404" && empty.body.find("NO_SHOT_TRACE") != std::string::npos);
  shotTrace().begin(1000);
  shotTrace().attachStart(ShotTraceSnapshot{}, 5);
  const httpd_req_t busy = traceRequest("-");
  CHECK(busy.status == "409" && busy.body.find("SHOT_TRACE_BUSY") != std::string::npos);
  shotTrace().complete(9, 5, 4000, 1);
  const httpd_req_t r = traceRequest("-");
  ShotTraceHeader header;
  CHECK(r.status == "200" && r.ended && r.body.size() >= sizeof(header));
  memcpy(&header, r.body.data(), sizeof(header));
  CHECK(header.shotLogId == 9 && header.totalBytes == r.body.size());
  CHECK(r.headers.at("Cache-Control") == "no-store" && r.headers.count("ETag") == 0 &&
        r.headers.at("Content-Disposition").find("attachment") == 0);
}

// Every list holds exactly the numbers in use (each *_COUNT is
// static_asserted next to its name table).
void theVocabularyNamesEveryNumber() {
  httpd_req_t r;
  r.query = "fw=1.1.0%2Babc1234";
  CHECK(OpenBrewByWeightNetwork::shotTraceVocabularyHandler(&r) == ESP_OK && r.ended);
  // Kept for good only under an identity no other build shares.
  const std::string id(FW_VERSION);
  const bool unique = id.find("-dirty") == id.npos && id.find("+unknown") == id.npos;
  CHECK(r.headers.at("Cache-Control") ==
        (unique ? "private, max-age=31536000, immutable" : "no-cache"));
  const auto list = [&](const std::string &key) {
    const size_t at = r.body.find("\"" + key + "\":[");
    const size_t end = r.body.find(']', at);
    size_t items = 0;
    for (size_t i = at; at != std::string::npos && i < end; ++i) items += r.body[i] == ',';
    return at == std::string::npos ? 0 : items + 1;
  };
  CHECK(list("profile") == SCALE_PROFILE_EVENT_COUNT && list("debug") == DEBUG_CODE_COUNT &&
        list("signals") == static_cast<size_t>(ScaleProfileSignal::COUNT) &&
        list("endReasons") == END_REASON_COUNT && list("weightSources") == ACTUAL_WEIGHT_SOURCE_COUNT);
  CHECK(r.body.rfind("{\"version\":1,\"profile\":[\"WEIGHT\",", 0) == 0 && r.body.back() == '}');
  CHECK(r.body.find("\"machine circuit closed\"") != std::string::npos);
  CHECK(r.body.find("\"endReasons\":[\"none\",\"activator\",\"\",") != std::string::npos);
  CHECK(r.body.find("\"weightSources\":[\"none\",\"post_drip\",\"last_known\"]}") !=
        std::string::npos);
  CHECK(r.chunks >= 3);  // More than one 4 KiB chunk, then the end.
}

void shotStorageIsOneJsonObject() {
  fakeStorage = {};
  fakeStorage.ready = true;
  fakeStorage.lastError = 3;
  fakeStorage.poolBytes = 5476352;
  fakeStorage.good = 7;
  fakeStorage.interrupted = 5;
  fakeStorage.orphansListed = 2;
  fakeStorage.orphans[0] = {41, 0};
  fakeStorage.orphans[1] = {40, 52000};
  fakeStorage.sinceLocalSec = 1792840364;
  httpd_req_t r;
  CHECK(OpenBrewByWeightNetwork::shotStorageHandler(&r) == ESP_OK && r.status == "200");
  CHECK(r.body.rfind("{\"ready\":true,\"lastError\":\"program\",\"poolBytes\":5476352,", 0) == 0);
  CHECK(r.body.find("\"good\":7,") != std::string::npos &&
        r.body.find("\"sinceLocalSec\":1792840364,") != std::string::npos &&
        r.body.find("\"interrupted\":5,") != std::string::npos);
  CHECK(r.body.find("\"shotCapacity\":1000,\"orphanTraces\":[{\"boot\":41,\"end\":0},"
                    "{\"boot\":40,\"end\":52000}]}") != std::string::npos);
  fakeStorageResult = ShotTraceRead::BUSY;
  httpd_req_t busy;
  (void)OpenBrewByWeightNetwork::shotStorageHandler(&busy);
  CHECK(busy.status == "409" && busy.body.find("SHOT_STORAGE_BUSY") != std::string::npos);
  fakeStorageResult = ShotTraceRead::FOUND;
}

int main() {
  OpenBrewByWeightNetwork network;
  OpenBrewByWeightNetwork::instance_ = &network;
  aPoolRecordStreamsImmutableInChunks();
  aStreamCutShortIsABrokenDownload();
  refusalsMapToTheirStatus();
  withoutAQueryTheNewestTraceComesFromRam();
  theVocabularyNamesEveryNumber();
  shotStorageIsOneJsonObject();
  if (failures == 0) std::cout << "shot trace routes host test passed\n";
  return failures == 0 ? 0 : 1;
}

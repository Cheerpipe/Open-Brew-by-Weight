// Shot trace downloads and the Diagnostic Shot storage section (plan B8,
// docs/SHOT_TRACE.md). Neither reads flash during a shot (cycle start to
// drip finalize): they answer 409 and the Web UI retries after the shot.

// Download chunk and scratch (JSON, Shot storage's summary tags): PSRAM, off
// the httpd stack; 4 KiB keeps a 16 KiB trace to four sends. httpd runs one
// request at a time.
alignas(4) OPEN_BREW_BY_WEIGHT_PSRAM_BSS uint8_t shotTraceDownloadChunk[4096];
static_assert(sizeof(shotTraceDownloadChunk) >= SHOT_LOG_CAPACITY * sizeof(uint32_t),
              "Shot storage sorts every summary's tag in the chunk");

namespace {

struct ShotTraceResponse {
  httpd_req_t *request;
  bool started;
  char etag[12];  // Lives until the headers go out with the first chunk.
};

// The first chunk sends the headers: a pool record is immutable (its URL
// names the shot's full identity), so its CRC is a strong ETag and the
// browser may keep it; the newest trace from RAM (crc 0) is never cached.
bool sendShotTraceChunk(void *context, const uint8_t *data, size_t bytes, uint32_t crc) {
  ShotTraceResponse &response = *static_cast<ShotTraceResponse *>(context);
  if (!response.started) {
    response.started = true;
    httpd_resp_set_type(response.request, "application/octet-stream");
    httpd_resp_set_hdr(response.request, "X-Content-Type-Options", "nosniff");
    if (crc == 0) {
      httpd_resp_set_hdr(response.request, "Content-Disposition",
                         "attachment; filename=\"openbrewbyweight-shot-trace.bin\"");
      httpd_resp_set_hdr(response.request, "Cache-Control", "no-store");
    } else {
      snprintf(response.etag, sizeof(response.etag), "\"%08lx\"", static_cast<unsigned long>(crc));
      httpd_resp_set_hdr(response.request, "ETag", response.etag);
      httpd_resp_set_hdr(response.request, "Cache-Control", "private, max-age=31536000, immutable");
    }
  }
  return httpd_resp_send_chunk(response.request, reinterpret_cast<const char *>(data), bytes) ==
         ESP_OK;
}

// id, boot and end: the shot's summary identity (id 0, end 0 for an
// interrupted trace of that boot). All three, whole decimal numbers that fit
// 32 bits (parsed in 64, so a larger one is refused, not saturated).
bool parseShotTraceIdentity(const char *query, uint32_t (&identity)[3]) {
  static const char *const keys[] = {"id", "boot", "end"};
  for (size_t i = 0; i < 3; ++i) {
    char value[12] = {};
    char *end = nullptr;
    if (httpd_query_key_value(query, keys[i], value, sizeof(value)) != ESP_OK || value[0] < '0' ||
        value[0] > '9')
      return false;
    const unsigned long long parsed = strtoull(value, &end, 10);
    if (*end != '\0' || parsed > UINT32_MAX) return false;
    identity[i] = static_cast<uint32_t>(parsed);
  }
  return true;
}

// JSON written through the PSRAM chunk, flushed when it fills.
struct ShotTraceJson {
  httpd_req_t *request;
  size_t used;
  bool ok;
  void flush() {
    ok = ok && httpd_resp_send_chunk(request, reinterpret_cast<const char *>(shotTraceDownloadChunk),
                                     used) == ESP_OK;
    used = 0;
  }
  void add(const char *text) {
    for (size_t n = strlen(text); ok && n != 0;) {
      if (used == sizeof(shotTraceDownloadChunk)) flush();
      const size_t piece = n < sizeof(shotTraceDownloadChunk) - used
                               ? n : sizeof(shotTraceDownloadChunk) - used;
      memcpy(shotTraceDownloadChunk + used, text, piece);
      used += piece;
      text += piece;
      n -= piece;
    }
  }
  template <typename Name>
  void list(const char *key, size_t count, Name &&name) {
    add(key);
    for (size_t i = 0; i < count; ++i) {
      add(i == 0 ? "[\"" : ",\"");
      add(name(i));
      add("\"");
    }
    add(count == 0 ? "[]" : "]");
  }
};

}  // namespace

// GET /api/v1/diagnostic/shot-trace: without a query, the newest completed
// trace from RAM; with ?id&boot&end, that shot's trace from the pool. Busy
// covers a shot and its drip, and whatever else closes the durable write gate
// (a rinse, a closed relay, a scale connecting, a maintenance lease).
esp_err_t OpenBrewByWeightNetwork::shotTraceDownloadHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ShotTraceResponse response = {request, false, {}};
  unsigned long known = 0;
  char query[64] = {};
  const esp_err_t queried = httpd_req_get_url_query_str(request, query, sizeof(query));
  ShotTraceRead result = ShotTraceRead::UNAVAILABLE;
  if (queried == ESP_ERR_NOT_FOUND) {
    const ShotTraceExport exported = shotTrace().exportCompleted(
        shotTraceDownloadChunk, sizeof(shotTraceDownloadChunk),
        [&](const uint8_t *data, size_t bytes) {
          return sendShotTraceChunk(&response, data, bytes, 0);
        });
    if (exported == ShotTraceExport::OK) return httpd_resp_send_chunk(request, nullptr, 0);
    if (response.started) return ESP_FAIL;
    if (exported == ShotTraceExport::EMPTY)
      return sendError(request, STATUS_NOT_FOUND, "NO_SHOT_TRACE",
                       "No shot trace is available since the last restart.");
    result = ShotTraceRead::BUSY;
  } else {
    uint32_t identity[3] = {};
    if (queried != ESP_OK || !parseShotTraceIdentity(query, identity))
      return sendError(request, STATUS_UNPROCESSABLE, "INVALID_SHOT_TRACE",
                       "id, boot and end must name a shot.");
    // If-None-Match: the ETag this route sent, "xxxxxxxx" (else ignored).
    char tag[16] = {};
    char *end = tag;
    if (httpd_req_get_hdr_value_str(request, "If-None-Match", tag, sizeof(tag)) == ESP_OK &&
        tag[0] == '"')
      known = strtoul(tag + 1, &end, 16);
    if (end != tag + 9 || strcmp(end, "\"") != 0) known = 0;
    if (self.callbacks_.streamShotTrace != nullptr)
      result = self.callbacks_.streamShotTrace(identity[0], identity[1], identity[2],
                                               static_cast<uint32_t>(known),
                                               shotTraceDownloadChunk,
                                               sizeof(shotTraceDownloadChunk),
                                               sendShotTraceChunk, &response);
    if (result == ShotTraceRead::FOUND) return httpd_resp_send_chunk(request, nullptr, 0);
    if (response.started) return ESP_FAIL;  // Cut short: the client sees a broken download.
  }
  switch (result) {
    case ShotTraceRead::NOT_MODIFIED:
      httpd_resp_set_status(request, STATUS_NOT_MODIFIED);
      snprintf(response.etag, sizeof(response.etag), "\"%08lx\"", known);
      httpd_resp_set_hdr(request, "ETag", response.etag);
      httpd_resp_set_hdr(request, "Cache-Control", "private, max-age=31536000, immutable");
      return httpd_resp_send(request, nullptr, 0);
    case ShotTraceRead::NONE:
      return sendError(request, STATUS_NOT_FOUND, "NO_SHOT_TRACE",
                       "This shot has no stored trace.");
    case ShotTraceRead::BUSY:
      return sendError(request, STATUS_CONFLICT, "SHOT_TRACE_BUSY",
                       "The controller is busy (a shot, a rinse or the scale); try again shortly.");
    case ShotTraceRead::PENDING:
      return sendError(request, STATUS_CONFLICT, "SHOT_TRACE_SAVING",
                       "This shot's trace is still being saved; try again shortly.");
    default:
      return sendError(request, STATUS_UNAVAILABLE, "SHOT_TRACE_UNAVAILABLE",
                       "The shot trace could not be read.");
  }
}

// The names the Web UI shows for the trace's kinds, codes, signals and end
// reasons, by number. They change only with the firmware, so the Web UI asks
// with the firmware identity in the query and the answer is kept; a build
// whose identity is not unique (uncommitted changes, no Git) is revalidated.
esp_err_t OpenBrewByWeightNetwork::shotTraceVocabularyHandler(httpd_req_t *request) {
  httpd_resp_set_type(request, JSON_CONTENT_TYPE);
  const std::string_view id(FW_VERSION);
  const bool unique = id.find("-dirty") == id.npos && id.find("+unknown") == id.npos;
  httpd_resp_set_hdr(request, "Cache-Control",
                     unique ? "private, max-age=31536000, immutable" : "no-cache");
  httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
  ShotTraceJson json = {request, 0, true};
  char version[48];
  snprintf(version, sizeof(version), "{\"version\":%u,", SHOT_TRACE_VOCABULARY_VERSION);
  json.add(version);
  json.list("\"profile\":", SCALE_PROFILE_EVENT_COUNT,
            [](size_t i) { return scaleProfileEventName(static_cast<ScaleProfileEvent>(i)); });
  json.list(",\"debug\":", DEBUG_CODE_COUNT,
            [](size_t i) { return debugCodeName(static_cast<DebugCode>(i)); });
  json.list(",\"signals\":", static_cast<size_t>(ScaleProfileSignal::COUNT),
            [](size_t i) { return scaleProfileSignalName(static_cast<uint16_t>(i)); });
  json.list(",\"endReasons\":", END_REASON_COUNT,
            [](size_t i) {
              const EndReasonLabel *label = findEndReasonLabel(static_cast<EndReason>(i));
              return label ? label->debugName : "";
            });
  json.list(",\"weightSources\":", ACTUAL_WEIGHT_SOURCE_COUNT, [](size_t i) {
    return actualWeightSourceName(static_cast<ActualWeightSource>(i));
  });
  json.add("}");
  json.flush();
  return json.ok ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
}

// GET /api/v1/diagnostic/shot-storage: the Diagnostic Shot storage section,
// read on demand when the section opens (never in the polled status).
esp_err_t OpenBrewByWeightNetwork::shotStorageHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  ShotStorageStats s = {};
  const ShotTraceRead result = self.callbacks_.copyShotStorage != nullptr
                                   ? self.callbacks_.copyShotStorage(
                                         s, reinterpret_cast<uint32_t *>(shotTraceDownloadChunk),
                                         sizeof(shotTraceDownloadChunk) / sizeof(uint32_t))
                                   : ShotTraceRead::UNAVAILABLE;
  if (result == ShotTraceRead::BUSY)
    return sendError(request, STATUS_CONFLICT, "SHOT_STORAGE_BUSY",
                     "The controller is busy (a shot, a rinse or the scale); try again shortly.");
  if (result != ShotTraceRead::FOUND)
    return sendError(request, STATUS_UNAVAILABLE, "SHOT_STORAGE_UNAVAILABLE",
                     "Shot storage could not be read.");
  static const char *const errors[] = {"none", "read", "erase", "program", "lock", "too_large",
                                       "source"};
  char *json = reinterpret_cast<char *>(shotTraceDownloadChunk);
  int used = snprintf(json, sizeof(shotTraceDownloadChunk),
           "{\"ready\":%s,\"lastError\":\"%s\",\"poolBytes\":%lu,\"liveBytes\":%lu,"
           "\"reclaimableBytes\":%lu,\"unindexedBytes\":%lu,\"unusedBytes\":%lu,"
           "\"records\":%lu,\"meanRecordBytes\":%lu,\"droppedRecords\":%lu,\"good\":%lu,"
           "\"bad\":%lu,\"traceOnly\":%lu,\"interrupted\":%lu,\"traces\":%lu,"
           "\"truncated\":%lu,\"sinceShotId\":%lu,\"sinceLocalSec\":%lu,"
           "\"pendingCurves\":%lu,\"pendingTraces\":%lu,\"tracesDropped\":%lu,"
           "\"shotCapacity\":%u,\"orphanTraces\":[",
           s.ready ? "true" : "false",
           errors[s.lastError < sizeof(errors) / sizeof(errors[0]) ? s.lastError : 0],
           static_cast<unsigned long>(s.poolBytes), static_cast<unsigned long>(s.liveBytes),
           static_cast<unsigned long>(s.reclaimableBytes),
           static_cast<unsigned long>(s.unindexedBytes), static_cast<unsigned long>(s.unusedBytes),
           static_cast<unsigned long>(s.records), static_cast<unsigned long>(s.meanRecordBytes),
           static_cast<unsigned long>(s.droppedRecords), static_cast<unsigned long>(s.good),
           static_cast<unsigned long>(s.bad), static_cast<unsigned long>(s.traceOnly),
           static_cast<unsigned long>(s.interrupted), static_cast<unsigned long>(s.traces),
           static_cast<unsigned long>(s.truncated), static_cast<unsigned long>(s.sinceShotId),
           static_cast<unsigned long>(s.sinceLocalSec), static_cast<unsigned long>(s.pendingCurves),
           static_cast<unsigned long>(s.pendingTraces), static_cast<unsigned long>(s.tracesDropped),
           static_cast<unsigned>(SHOT_LOG_CAPACITY));
  // At most 16 entries of two 10-digit numbers: well inside the chunk.
  for (uint8_t i = 0; i < s.orphansListed; ++i)
    used += snprintf(json + used, sizeof(shotTraceDownloadChunk) - used,
                     "%s{\"boot\":%lu,\"end\":%lu}", i ? "," : "",
                     static_cast<unsigned long>(s.orphans[i].bootId),
                     static_cast<unsigned long>(s.orphans[i].endedAtMs));
  snprintf(json + used, sizeof(shotTraceDownloadChunk) - used, "]}");
  return sendJson(request, STATUS_OK, json);
}

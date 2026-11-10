// Beanconqueror machine endpoints (docs/schemas/README.md): with Settings →
// Coffee apps on, /beanconqueror answers as the chosen Gaggiuino or GaggiMate.
// Public and read-only by design, like the machines it stands in for: no
// token, the Host check of every route, no CORS headers. Answers stream from
// the work buffer; nothing is sent while the radio must stay quiet.

namespace {

bool beanconquerorChunk(void *request, const uint8_t *bytes, size_t length) {
  return httpd_resp_send_chunk(static_cast<httpd_req_t *>(request),
                               reinterpret_cast<const char *>(bytes),
                               static_cast<ssize_t>(length)) == ESP_OK;
}

// `<id><suffix>` with the id's digits padded or not; 0 when it is not one.
uint32_t beanconquerorId(const char *text, const char *suffix) {
  char *end = nullptr;
  const unsigned long id = strtoul(text, &end, 10);
  return end != text && isdigit(static_cast<unsigned char>(*text)) && strcmp(end, suffix) == 0 &&
                 id <= UINT32_MAX
             ? static_cast<uint32_t>(id)
             : 0;
}

}  // namespace

uint8_t OpenBrewByWeightNetwork::beanconquerorEndpoint() const {
  RuntimeConfig config = {};
  if (callbacks_.copyRuntimeConfig) callbacks_.copyRuntimeConfig(&config);
  return config.beanconquerorEndpoint < BEANCONQUEROR_ENDPOINT_COUNT ? config.beanconquerorEndpoint
                                                                     : BEANCONQUEROR_OFF;
}

// The newest-first records outside the Stats stream (the companion's record
// sync, Beanconqueror): the Stats cache's copy, reloaded when the store moved.
// The Stats stream keeps its own epochs for its header and order.
bool OpenBrewByWeightNetwork::freshShotSnapshot(uint32_t epoch) {
  auto &cache = g_work->statsCache;
  if (cache.valid && cache.snapshot.epoch == epoch) return true;
  if (!callbacks_.copyShotStatsRecords) return false;
  callbacks_.copyShotStatsRecords(cache.snapshot);
  cache.curveHashes = {};
  return true;
}

esp_err_t OpenBrewByWeightNetwork::beanconquerorHandler(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  const uint8_t format = self.beanconquerorEndpoint();
  char path[64] = {};
  const char *uri = request->uri + strlen("/beanconqueror");
  snprintf(path, sizeof(path), "%.*s", static_cast<int>(strcspn(uri, "?")), uri);
  const auto notFound = [&] {
    return sendError(request, STATUS_NOT_FOUND, "NOT_FOUND", nullptr, "NOT_FOUND");
  };
  if (format == BEANCONQUEROR_OFF) return notFound();
  if (format == BEANCONQUEROR_GAGGIUINO && strcmp(path, "/api/system/status") == 0)
    return sendJson(request, STATUS_OK, "{}");
  if (format == BEANCONQUEROR_GAGGIMATE && strcmp(path, "/api/status") == 0)
    return sendJson(request, STATUS_OK, "{\"mode\":0}");
  const bool gaggiuino = format == BEANCONQUEROR_GAGGIUINO;
  const char *const shots = gaggiuino ? "/api/shots/" : "/api/history/";
  if (strncmp(path, shots, strlen(shots)) != 0) return notFound();
  const char *name = path + strlen(shots);
  const bool latest = gaggiuino && strcmp(name, "latest") == 0;
  const bool index = !gaggiuino && strcmp(name, "index.bin") == 0;
  const bool recent = !gaggiuino && strcmp(name, "recent.bin") == 0;
  const uint32_t shotId = gaggiuino ? beanconquerorId(name, "") : beanconquerorId(name, ".slog");
  const uint32_t notesId = gaggiuino ? 0 : beanconquerorId(name, ".json");
  if (!latest && !index && !recent && !shotId && !notesId) return notFound();
  // A shot until its drip ends, a rinse, a scale connecting: the companion
  // link's sync hold, so no answer wakes the radio then.
  if (self.companionSyncHold())
    return sendError(request, STATUS_UNAVAILABLE, "SHOT_STORAGE_BUSY", nullptr,
                     "SHOT_STORAGE_BUSY");
  if (!self.lockWorkBuf()) return self.workBufBusy(request);
  NetworkWorkBuf &work = *self.workBuf_;
  const uint32_t epoch = self.callbacks_.shotLogEpoch ? self.callbacks_.shotLogEpoch() : 0;
  if (!self.freshShotSnapshot(epoch)) {
    self.unlockWorkBuf();
    return sendError(request, STATUS_UNAVAILABLE, "SHOT_STORAGE_BUSY", nullptr, "SHOT_STORAGE_BUSY");
  }
  const ShotStatsRecordsSnapshot &records = work.statsCache.snapshot;
  BeanconquerorOut out{reinterpret_cast<uint8_t *>(work.statusJson), NetworkWorkBuf::kStatusJson};
  out.flush = beanconquerorChunk;
  out.context = request;
  BeanconquerorShot shot;
  // A streamed answer closes its socket like every JSON answer (sendJson), so
  // an app's keep-alive pool never holds one of the few sessions.
  const auto begin = [&](const char *type) {
    httpd_resp_set_type(request, type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Connection", "close");
  };
  const auto finish = [&](uint32_t id) {
    const esp_err_t result = out.finish() ? httpd_resp_send_chunk(request, nullptr, 0) : ESP_FAIL;
    (void)httpd_sess_trigger_close(request->handle, httpd_req_to_sockfd(request));
    if (result != ESP_OK)
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "Beanconqueror: %s %lu could not be sent", id ? "shot" : "index",
                           static_cast<unsigned long>(id));
    self.unlockWorkBuf();
    return result;
  };
  if (latest || index || recent) {
    size_t limit = SIZE_MAX, good = 0, newest = SIZE_MAX;
    if (recent) {
      char query[32] = {}, value[8] = {};
      limit = 8;
      if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
          httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK)
        limit = static_cast<size_t>(atoi(value) < 1 ? 1 : atoi(value) > 50 ? 50 : atoi(value));
    }
    for (size_t i = 0; i < records.count; ++i)
      if (shotLogRecordEligible(records.records[i]) && good++ == 0) newest = i;
    if (latest) {
      char body[48] = "[]";
      if (newest != SIZE_MAX)
        snprintf(body, sizeof(body), "[{\"lastShotId\":%lu}]",
                 static_cast<unsigned long>(records.records[newest].id));
      self.unlockWorkBuf();
      return sendJson(request, STATUS_OK, body);
    }
    begin("application/octet-stream");
    const uint32_t count = static_cast<uint32_t>(good < limit ? good : limit);
    beanconquerorIndexHeader(out, count, index && count ? records.records[newest].id + 1U : 0U);
    for (size_t i = 0, sent = 0; i < records.count && sent < count; ++i)
      if (beanconquerorShotOf(records.records[i], shot)) beanconquerorIndexEntry(out, shot), ++sent;
    return finish(0);
  }
  const uint32_t id = shotId ? shotId : notesId;
  const ShotLogRecord *record = nullptr;
  for (size_t i = 0; i < records.count && record == nullptr; ++i)
    if (records.records[i].id == id) record = &records.records[i];
  if (record == nullptr || !beanconquerorShotOf(*record, shot)) {
    self.unlockWorkBuf();
    return notFound();
  }
  if (notesId) {
    char body[64];
    snprintf(body, sizeof(body), "{\"id\":\"%lu\",\"doseIn\":\"%u.%u\"}",
             static_cast<unsigned long>(shot.id), shot.doseDg / 10U, shot.doseDg % 10U);
    self.unlockWorkBuf();
    return shot.doseDg ? sendJson(request, STATUS_OK, body) : notFound();
  }
  ShotCurveRecord &curve = work.statsCache.fetchCurve;
  const ShotCurveRead read = self.callbacks_.copyShotCurveById
                                 ? self.callbacks_.copyShotCurveById(id, curve, true)
                                 : ShotCurveRead::NONE;
  if (read == ShotCurveRead::BUSY) {
    self.unlockWorkBuf();
    return sendError(request, STATUS_UNAVAILABLE, "SHOT_CURVE_BUSY", nullptr, "SHOT_CURVE_BUSY");
  }
  if (read != ShotCurveRead::FOUND) resetShotCurveRecord(curve);  // a shot without a curve
  beanconquerorFlow(curve, shot, work.beanconquerorFlow);
  begin(gaggiuino ? "application/json" : "application/octet-stream");
  if (gaggiuino) beanconquerorGaggiuinoShot(shot, curve, work.beanconquerorFlow, out);
  else beanconquerorSlog(out, shot, curve, work.beanconquerorFlow);
  return finish(id);
}

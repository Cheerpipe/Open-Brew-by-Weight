// httpd owns framing, session contexts and serialization. The existing priority-1
// network task queues at most one dispatch; it never sends or borrows JSON scratch.
namespace {
constexpr uint32_t kUiStreamBindMs = 5000, kUiStreamRetainMs = 30000;
// A page pings every 19-20 s only while its power window is active or a shot
// is live, otherwise every 120 s of silence; a bound session silent past the
// page's own 5-minute hidden close plus one ping belongs to a page the phone
// froze hidden.
constexpr uint32_t kUiStreamSilentMs = 330000;
// Wi-Fi and the BLE scale share one radio: a periodic frame leaves right after
// every second weight sample, in the gap before the next notification. The
// timer only covers a missing or silent scale.
constexpr uint32_t kUiStreamSampleStride = 2, kUiStreamSampleFloorMs = 150;
constexpr uint32_t kUiStreamFallbackMs = 300;
bool uiStreamDispatchDue(bool urgent, uint32_t elapsedMs, uint32_t newSamples) {
  return urgent || elapsedMs >= kUiStreamFallbackMs ||
         (newSamples >= kUiStreamSampleStride && elapsedMs >= kUiStreamSampleFloorMs);
}
// Fits a bind carrying the page's usual subscriptions (Settings, History,
// Stats, and Diagnostic with its log), or one Stats subscribe or fetch with
// the largest filter (~300 bytes); the page sends whatever does not fit as
// separate ops. Lives on the httpd task stack for one request.
constexpr size_t kUiStreamRequestBytes = 640;
// bind > ops[] > op > filter > list: the parser recurses once per level on the
// httpd stack, so stream requests get their own nesting limit.
constexpr size_t kUiStreamRequestDepth = 6;
// Leave TCP send-buffer headroom for live state. A larger individual curve
// stays intact, alone under the client's 28 KiB message guard.
constexpr size_t kStatsStreamFrameBytes = 8192;

// Shared op validation for the view-scoped record subscriptions: an exact
// field whitelist and bounded optional integers.
bool uiStreamKnownFields(const cJSON *message, const char *const *names,
                           size_t count) {
  for (const cJSON *field = message->child; field; field = field->next) {
    bool ok = false;
    for (size_t index = 0; index < count && !ok; ++index)
      ok = field->string != nullptr &&
           strcmp(field->string, names[index]) == 0;
    if (!ok) return false;
  }
  return true;
}

bool uiStreamIntInRange(const cJSON *node, double low, double high) {
  return node == nullptr ||
         (cJSON_IsNumber(node) && node->valuedouble >= low &&
          node->valuedouble <= high && floor(node->valuedouble) == node->valuedouble);
}

// List filters: an absent one leaves `out` untouched, a malformed one
// rejects the op. History types are HistoryType names, one bit each.
__attribute__((optimize("Os")))
bool uiStreamParseHistoryTypes(const cJSON *types, uint8_t &out) {
  constexpr uint8_t kLast = static_cast<uint8_t>(HistoryType::BACKFLUSH);
  if (!types) return true;
  if (!cJSON_IsArray(types) || cJSON_GetArraySize(types) > kLast + 1) return false;
  uint8_t mask = 0;
  for (const cJSON *item = types->child; item; item = item->next) {
    if (!cJSON_IsString(item)) return false;
    uint8_t type = 0;
    while (type <= kLast &&
           strcmp(item->valuestring, historyTypeName(static_cast<HistoryType>(type))))
      ++type;
    if (type > kLast) return false;
    mask |= static_cast<uint8_t>(1U << type);
  }
  out = mask;
  return true;
}

// A Shots-list filter: result names, preset ids, and scale keys from the
// Stats facets, at most SHOT_LIST_FILTER_MAX of each, and `bad`, which also
// lists the bad shots.
__attribute__((optimize("Os")))
bool uiStreamParseShotFilter(const cJSON *filter, ShotListFilter &out) {
  static const char *const kFields[] = {"result", "preset", "scale", "bad"};
  static const char *const kResults[] = {"on_recipe", "fast", "slow"};
  if (!filter) return true;
  if (!cJSON_IsObject(filter) || !uiStreamKnownFields(filter, kFields, 4)) return false;
  const cJSON *bad = cJSON_GetObjectItemCaseSensitive(filter, "bad");
  if (bad && !cJSON_IsBool(bad)) return false;
  const cJSON *lists[3];
  for (size_t index = 0; index < 3; ++index) {
    lists[index] = cJSON_GetObjectItemCaseSensitive(filter, kFields[index]);
    if (lists[index] && (!cJSON_IsArray(lists[index]) ||
                         cJSON_GetArraySize(lists[index]) > static_cast<int>(SHOT_LIST_FILTER_MAX)))
      return false;
  }
  ShotListFilter parsed;
  parsed.includeBad = cJSON_IsTrue(bad);
  for (const cJSON *item = lists[0] ? lists[0]->child : nullptr; item; item = item->next) {
    uint8_t result = 0;
    while (result < 3 && !(cJSON_IsString(item) && !strcmp(item->valuestring, kResults[result])))
      ++result;
    if (result == 3) return false;
    parsed.results |= static_cast<uint8_t>(1U << result);
  }
  for (const cJSON *item = lists[1] ? lists[1]->child : nullptr; item; item = item->next) {
    if (!cJSON_IsNumber(item) || !uiStreamIntInRange(item, 0, UINT8_MAX)) return false;
    parsed.presets[parsed.presetCount++] = static_cast<uint8_t>(item->valueint);
  }
  for (const cJSON *item = lists[2] ? lists[2]->child : nullptr; item; item = item->next) {
    if (!cJSON_IsNumber(item) || !uiStreamIntInRange(item, 0, UINT32_MAX)) return false;
    parsed.scales[parsed.scaleCount++] = static_cast<uint32_t>(item->valuedouble);
  }
  out = parsed;
  return true;
}

// Router DNS names and reverse-proxy hosts are longer than the device name.
constexpr size_t UI_STREAM_ORIGIN_CAPACITY = 128;

bool uiStreamOriginMatches(const char *origin, const char *host) {
  if (!host || !host[0]) return false;
  char accepted[UI_STREAM_ORIGIN_CAPACITY + 12];
  snprintf(accepted, sizeof(accepted), "http://%s", host);
  if (strcmp(origin, accepted) == 0) return true;
  snprintf(accepted, sizeof(accepted), "http://%s:80", host);
  return strcmp(origin, accepted) == 0;
}

// Identity of the first `count` points: their stream bytes and flags.
uint32_t uiStreamCurveHash(const ShotCurveRecord &curve, uint16_t count) {
  ShotCurveReader prefix;
  (void)shotCurveSeek(curve, count, prefix);
  uint32_t hash = crc32Update(0xFFFFFFFFU, curve.stream, prefix.offset);
  // Same byte sequence as one flag byte per point, hashed in blocks.
  uint8_t flags[64] = {};
  for (uint16_t i = 0; i < count;) {
    size_t n = 0;
    for (; n < sizeof(flags) && i < count; ++n, ++i)
      flags[n] = (shotCurveBreakBefore(curve, i) ? 1 : 0) | (shotCurveHeld(curve, i) ? 2 : 0);
    hash = crc32Update(hash, flags, n);
  }
  return hash;
}

using HeldRow = UiStreamFingerprints::HeldRow;
constexpr size_t kHeldRows = UiStreamFingerprints::kHeldRows;
uint32_t uiStreamRowHash(const void *bytes, size_t length) {
  return crc32Update(0xFFFFFFFFU, static_cast<const uint8_t *>(bytes), length);
}
// True when the page holds this row unchanged in its standing page.
bool uiStreamHeld(const HeldRow *rows, size_t count, uint32_t id, uint32_t hash) {
  for (size_t index = 0; index < count; ++index)
    if (rows[index].id == id) return rows[index].hash == hash;
  return false;
}

// Close notice before httpd drops the socket. 4001 (ownership taken over,
// from the dispatch path or a rejected bind) makes the page deactivate and
// offer Reactivate instead of retrying forever; 4002 (page silent) makes it
// reconnect quietly once visible, without the out-of-reach overlay; 4003 (an
// owner op failed validation) makes it drop its list filters and Stats window
// before reconnecting, so a bad subscription is not replayed forever.
constexpr uint16_t kUiStreamCloseTakeover = 4001, kUiStreamCloseSilent = 4002,
                   kUiStreamCloseRejected = 4003;
void sendUiStreamClose(httpd_handle_t server, int fd, uint16_t reason) {
  uint8_t code[] = {static_cast<uint8_t>(reason >> 8), static_cast<uint8_t>(reason)};
  httpd_ws_frame_t close = {};
  close.type = HTTPD_WS_TYPE_CLOSE;
  close.payload = code;
  close.len = sizeof(code);
  (void)httpd_ws_send_frame_async(server, fd, &close);
}

#include "OpenBrewByWeightUiStreamTransport.inc"
}  // namespace

#include "OpenBrewByWeightStatsPage.inc"
#include "OpenBrewByWeightHomeStream.inc"
#include "OpenBrewByWeightAuditStream.inc"
#include "OpenBrewByWeightSettingsStream.inc"

esp_err_t OpenBrewByWeightNetwork::uiStreamHandshake(httpd_req_t *request) {
  OpenBrewByWeightNetwork &self = *instance_;
  char origin[UI_STREAM_ORIGIN_CAPACITY] = {}, host[UI_STREAM_ORIGIN_CAPACITY] = {};
  const size_t length = httpd_req_get_hdr_value_len(request, "Origin");
  if (length == 0 || length >= sizeof(origin) ||
      httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK) {
    // A refused upgrade otherwise leaves only ESP-IDF's bare
    // ws_pre_handshake_cb failed; each reason names itself, rate-limited.
    static RefusalLog log;
    if (const uint32_t count = log.due())
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "ui-stream: refused %lu upgrade(s) without a usable Origin header",
                           static_cast<unsigned long>(count));
    return ESP_FAIL;
  }
  // Same origin, and that host must name this controller (the HTTP gate's
  // allowlist), so a rebound page cannot open the stream.
  if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK)
    host[0] = '\0';
  const NetworkStatusSnapshot network = self.snapshot();
  if (!uiStreamOriginMatches(origin, host) ||
      !webAccessHostAllowed(host, network.mdnsHost)) {
    logRefusedHost(origin, network.mdnsHost);
    return ESP_FAIL;
  }
  // The companion's upstream session arrives signed, like its REST calls.
  if (!relayAdmit(request)) return ESP_FAIL;
  char screen[COMPANION_RELAY_SCREEN_TEXT], address[COMPANION_RELAY_ADDR_TEXT];
  const bool relayed = self.relayIdentity(request, screen, address);
  TaskLockGuard lock(self.dataMux_);
  for (auto &session : self.uiStreams_) {
    if (session.fd != -1) continue;
    session = UiStreamSession{};
    new (&self.workBuf_->uiFingerprints[&session - self.uiStreams_]) UiStreamFingerprints{};
    session.fd = httpd_req_to_sockfd(request);
    session.fromSoftAp = socketServedBySoftAp(session.fd);
    session.relayed = relayed;
    session.openedAtMs = millis();
    self.uiStreamOpen_.store(true, std::memory_order_release);
    request->sess_ctx = &session;
    request->free_ctx = uiStreamFree;
    // Bound incomplete incoming frames independently of ordinary HTTP/OTA.
    const timeval timeout = {0, 100000};
    setsockopt(session.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    // Frames leave back to back: without NODELAY, Nagle holds each write
    // until the previous one's ACK, which modem sleep delays.
    const int noDelay = 1;
    setsockopt(session.fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    return ESP_OK;
  }
  static RefusalLog log;
  if (const uint32_t count = log.due())
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                         "ui-stream: refused %lu upgrade(s): both sessions are busy",
                         static_cast<unsigned long>(count));
  return ESP_FAIL;
}

void OpenBrewByWeightNetwork::uiStreamFree(void *context) {
  if (!instance_ || !context) return;
  TaskLockGuard lock(instance_->dataMux_);
  new (&instance_->workBuf_->uiFingerprints[
      static_cast<UiStreamSession *>(context) - instance_->uiStreams_]) UiStreamFingerprints{};
  *static_cast<UiStreamSession *>(context) = UiStreamSession{};
  bool open = false;
  for (const auto &session : instance_->uiStreams_) open |= session.fd >= 0;
  instance_->uiStreamOpen_.store(open, std::memory_order_release);
}

esp_err_t OpenBrewByWeightNetwork::uiStreamHandler(httpd_req_t *request) {
  uiStreamSendKind = "control";
  OpenBrewByWeightNetwork &self = *instance_;
  auto *session = static_cast<UiStreamSession *>(request->sess_ctx);
  if (!session) return ESP_FAIL;
  httpd_ws_frame_t frame = {};
  char body[kUiStreamRequestBytes + 1] = {};
  if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK ||
      frame.type != HTTPD_WS_TYPE_TEXT || !frame.final ||
      frame.len == 0 || frame.len > kUiStreamRequestBytes) return ESP_FAIL;
  frame.payload = reinterpret_cast<uint8_t *>(body);
  if (httpd_ws_recv_frame(request, &frame, frame.len) != ESP_OK ||
      memchr(body, '\0', frame.len)) return ESP_FAIL;
  cJSON *document = parseJsonDocumentWithinLimits(
      body, kUiStreamRequestBytes, JSON_DOCUMENT_MAX_VALUES, kUiStreamRequestDepth);
  if (!document) return ESP_FAIL;
  const cJSON *op = cJSON_GetObjectItemCaseSensitive(document, "op");
  const cJSON *client = cJSON_GetObjectItemCaseSensitive(document, "client");
  // A bind may carry the page's subscriptions as `ops`: once the bind is
  // accepted each runs through the same chain, and the first one rejected
  // rejects the frame like a separate op would.
  const cJSON *ops = cJSON_GetObjectItemCaseSensitive(document, "ops");
  const bool bindOp = cJSON_IsString(op) && strcmp(op->valuestring, "bind") == 0 &&
                      cJSON_IsString(client);
  bool accepted = false, owner = false, bound = false;
  bool activity = false, resumed = false;
  int superseded = -1;
  {
    TaskLockGuard lock(self.dataMux_);
    for (const cJSON *message = document; message != nullptr;
         message = !accepted ? nullptr
                   : message != document ? message->next
                   : bound && ops ? ops->child : nullptr) {
      op = cJSON_GetObjectItemCaseSensitive(message, "op");
      accepted = false;
      if (cJSON_IsString(op) && session->bound && message == document &&
          strcmp(op->valuestring, "activity") == 0 &&
          strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        const cJSON *seconds = cJSON_GetObjectItemCaseSensitive(message, "seconds");
        const cJSON *hidden = cJSON_GetObjectItemCaseSensitive(message, "hidden");
        if (cJSON_IsNumber(seconds) && seconds->valuedouble >= 0 &&
            seconds->valuedouble <= 30 && seconds->valuedouble == seconds->valueint &&
            (!hidden || cJSON_IsBool(hidden))) {
          notePowerWebActivity(millis(), static_cast<uint32_t>(seconds->valueint));
          // A visible page resumes with one delta of everything it missed.
          resumed = session->paused && cJSON_IsFalse(hidden);
          if (hidden) session->paused = cJSON_IsTrue(hidden);
          accepted = activity = true;
        }
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "resync") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        session->resync = true;
        session->homeResync = true;
        session->diagResync = session->diagnostic;
        session->settingsResync = true;
        session->settingsKnownTag = session->historyKnownTag = session->statsKnownTag = 0;
        session->historyResync = true;
        session->statsResync = true;
        session->statsFetch = false;
        accepted = true;
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "settings") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        // Standing Settings subscription: the page subscribes once per socket
        // and never unsubscribes; `tag` names the model it still holds.
        // Compatibility mode has no Settings view: the op is accepted inert,
        // since a reconnecting page replays it before Home names the mode.
        static const char *const kFields[] = {"op", "on", "tag"};
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(message, "tag");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(message, "on")) &&
            uiStreamKnownFields(message, kFields, 3) && uiStreamIntInRange(tag, 0, UINT32_MAX)) {
          session->settingsOn = !firmwareCompatibilityMode();
          session->settingsResync = true;
          session->settingsKnownTag = tag ? static_cast<uint32_t>(tag->valuedouble) : 0;
          self.uiStreamUrgent_.store(true, std::memory_order_release);
          accepted = true;
        }
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "diagnostic") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        // View-scoped telemetry toggle: the client subscribes only while the
        // Diagnostic page is shown and unsubscribes on every navigation.
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(message, "on");
        if (cJSON_IsBool(on)) {
          session->diagnostic = cJSON_IsTrue(on);
          session->diagResync = session->diagnostic;
          accepted = true;
        }
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "log") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        // View-scoped serial-log toggle: the cursor comes from the client's
        // rendered tail; unsubscribe carries no window fields.
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(message, "on");
        const cJSON *after = cJSON_GetObjectItemCaseSensitive(message, "after");
        bool shapeOk = cJSON_IsBool(on);
        for (const cJSON *field = message->child; shapeOk && field;
             field = field->next) {
          const char *name = field->string;
          shapeOk = name && (!strcmp(name, "op") || !strcmp(name, "on") ||
                             !strcmp(name, "after"));
        }
        const bool numbersOk = uiStreamIntInRange(after, 0, UINT32_MAX);
        if (shapeOk && numbersOk) {
          if (!cJSON_IsTrue(on)) {
            accepted = !after;
            if (accepted) session->logOn = false;
          } else {
            session->logOn = true;
            session->logAfter = after ? static_cast<uint32_t>(after->valuedouble) : 0;
            session->logPush = true;
            accepted = true;
          }
          if (accepted)
            self.uiStreamUrgent_.store(true, std::memory_order_release);
        }
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "history") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        // View-scoped History page subscription: the standing window tracks the
        // page; a one-shot fetch serves sentinel appends without disturbing it.
        // `types`, `limit` and `dir` shape both; a fetch keeps the standing
        // value of each one it omits.
        // A subscribe's `tag` names the standing page the browser still holds.
        static const char *const kFields[] = {"op", "on", "offset", "limit", "dir",
                                              "fetch", "request", "types", "tag"};
        const cJSON *requestId = cJSON_GetObjectItemCaseSensitive(message, "request");
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(message, "on");
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(message, "offset");
        const cJSON *limit = cJSON_GetObjectItemCaseSensitive(message, "limit");
        const cJSON *dir = cJSON_GetObjectItemCaseSensitive(message, "dir");
        const cJSON *fetch = cJSON_GetObjectItemCaseSensitive(message, "fetch");
        const cJSON *types = cJSON_GetObjectItemCaseSensitive(message, "types");
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(message, "tag");
        uint8_t typeMask = 0;
        const bool shapeOk = cJSON_IsBool(on) && uiStreamKnownFields(message, kFields, 9) &&
                             uiStreamParseHistoryTypes(types, typeMask);
        const bool numbersOk =
            uiStreamIntInRange(requestId, 1, UINT32_MAX) &&
            uiStreamIntInRange(tag, 1, UINT32_MAX) &&
            uiStreamIntInRange(offset, 0, HISTORY_CAPACITY) &&
            uiStreamIntInRange(limit, 1, HISTORY_PAGE_MAX) &&
            (!fetch || cJSON_IsBool(fetch)) &&
            (!dir || (cJSON_IsString(dir) &&
                      (!strcmp(dir->valuestring, "asc") ||
                       !strcmp(dir->valuestring, "desc"))));
        if (shapeOk && numbersOk) {
          if (!cJSON_IsTrue(on)) {
            // Unsubscribe carries no window fields.
            accepted = !offset && !limit && !dir && !fetch && !requestId && !types && !tag;
            if (accepted) {
              session->historyOn = false;
              session->historyFetch = false;
            }
          } else if (fetch && cJSON_IsTrue(fetch)) {
            accepted = session->historyOn && offset != nullptr && !tag;
            if (accepted) {
              session->historyFetchOffset =
                  static_cast<size_t>(offset->valueint);
              session->historyFetchTypes = types ? typeMask : session->historyTypes;
              session->historyFetchLimit =
                  limit ? static_cast<size_t>(limit->valueint) : session->historyLimit;
              session->historyFetchDir = !dir ? session->historyDir
                                         : !strcmp(dir->valuestring, "asc") ? ShotLogSortDir::Asc
                                                                            : ShotLogSortDir::Desc;
              session->historyFetch = true;
              session->historyFetchRequest = requestId ? static_cast<uint32_t>(requestId->valuedouble) : 0;
            }
          } else {
            session->historyOn = true;
            session->historyResync = true;
            session->historyRequest = requestId ? static_cast<uint32_t>(requestId->valuedouble) : 0;
            session->historyTypes = typeMask;
            session->historyKnownTag = tag ? static_cast<uint32_t>(tag->valuedouble) : 0;
            if (offset)
              session->historyOffset = static_cast<size_t>(offset->valueint);
            if (limit)
              session->historyLimit = static_cast<size_t>(limit->valueint);
            if (dir)
              session->historyDir = !strcmp(dir->valuestring, "asc")
                                         ? ShotLogSortDir::Asc
                                         : ShotLogSortDir::Desc;
            accepted = true;
          }
          if (accepted)
            self.uiStreamUrgent_.store(true, std::memory_order_release);
        }
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "stats") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        // View-scoped Stats page subscription on the same contract; one-shot
        // fetches also serve the CSV export window (date/desc, good shots).
        // `filter` works like History's `types`; `window` picks the newest
        // shots the aggregate and charts cover (a fetch's own header too).
        static const char *const kFields[] = {"op",    "on",      "offset",
                                              "limit", "sort",    "dir",
                                              "fetch", "request", "filter",
                                              "window", "tag"};
        const cJSON *requestId = cJSON_GetObjectItemCaseSensitive(message, "request");
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(message, "on");
        const cJSON *offset = cJSON_GetObjectItemCaseSensitive(message, "offset");
        const cJSON *limit = cJSON_GetObjectItemCaseSensitive(message, "limit");
        const cJSON *sort = cJSON_GetObjectItemCaseSensitive(message, "sort");
        const cJSON *dir = cJSON_GetObjectItemCaseSensitive(message, "dir");
        const cJSON *fetch = cJSON_GetObjectItemCaseSensitive(message, "fetch");
        const cJSON *filter = cJSON_GetObjectItemCaseSensitive(message, "filter");
        const cJSON *window = cJSON_GetObjectItemCaseSensitive(message, "window");
        const cJSON *tag = cJSON_GetObjectItemCaseSensitive(message, "tag");
        ShotListFilter shotFilter;
        bool windowOk = !window;
        for (const size_t size : SHOT_LOG_STATS_WINDOWS)
          windowOk = windowOk || (cJSON_IsNumber(window) && window->valuedouble == size);
        const bool shapeOk = cJSON_IsBool(on) && uiStreamKnownFields(message, kFields, 11) &&
                             uiStreamParseShotFilter(filter, shotFilter) && windowOk;
        const bool numbersOk =
            uiStreamIntInRange(requestId, 1, UINT32_MAX) &&
            uiStreamIntInRange(tag, 1, UINT32_MAX) &&
            uiStreamIntInRange(offset, 0, SHOT_LOG_CAPACITY) &&
            uiStreamIntInRange(limit, 1, SHOT_LOG_CAPACITY) &&
            (!fetch || cJSON_IsBool(fetch)) &&
            (!sort || (cJSON_IsString(sort) &&
                       (!strcmp(sort->valuestring, "date") ||
                        !strcmp(sort->valuestring, "rating")))) &&
            (!dir || (cJSON_IsString(dir) &&
                      (!strcmp(dir->valuestring, "asc") ||
                       !strcmp(dir->valuestring, "desc"))));
        if (shapeOk && numbersOk) {
          if (!cJSON_IsTrue(on)) {
            accepted = !offset && !limit && !sort && !dir && !fetch && !requestId &&
                       !filter && !window && !tag;
            if (accepted) {
              session->statsOn = false;
              session->statsFetch = false;
              session->statsPaging = false;
            }
          } else if (fetch && cJSON_IsTrue(fetch)) {
            accepted = session->statsOn && offset != nullptr && !tag;
            if (accepted) {
              session->statsFetchFilter = filter ? shotFilter : session->statsFilter;
              session->statsFetchWindow =
                  window ? static_cast<size_t>(window->valueint) : session->statsWindow;
              session->statsFetchOffset =
                  static_cast<size_t>(offset->valueint);
              session->statsFetchLimit = limit
                  ? static_cast<size_t>(limit->valueint)
                  : session->statsLimit;
              session->statsFetchSort = sort
                  ? shotLogSortFromName(sort->valuestring)
                  : session->statsSort;
              session->statsFetchDir = dir
                  ? shotLogSortDirFromName(dir->valuestring)
                  : session->statsDir;
              session->statsFetch = true;
              session->statsFetchRequest = requestId ? static_cast<uint32_t>(requestId->valuedouble) : 0;
            }
          } else if (window && session->statsOn && !offset && !limit && !sort && !dir &&
                     !filter && !requestId && !tag) {
            // A window-only change refreshes the header: rows stay held.
            session->statsWindow = static_cast<size_t>(window->valueint);
            accepted = true;
          } else {
            session->statsOn = true;
            session->statsResync = true;
            session->statsFetch = false;
            session->statsPaging = false;
            session->statsRequest = requestId ? static_cast<uint32_t>(requestId->valuedouble) : 0;
            session->statsFilter = shotFilter;
            session->statsKnownTag = tag ? static_cast<uint32_t>(tag->valuedouble) : 0;
            session->statsWindow = window ? static_cast<size_t>(window->valueint)
                                          : SHOT_LOG_STATS_WINDOW;
            if (offset)
              session->statsOffset = static_cast<size_t>(offset->valueint);
            if (limit)
              session->statsLimit = static_cast<size_t>(limit->valueint);
            if (sort)
              session->statsSort = shotLogSortFromName(sort->valuestring);
            if (dir)
              session->statsDir = shotLogSortDirFromName(dir->valuestring);
            accepted = true;
          }
          if (accepted)
            self.uiStreamUrgent_.store(true, std::memory_order_release);
        }
      } else if (cJSON_IsString(op) && session->bound &&
                 strcmp(op->valuestring, "records") == 0 &&
                 strcmp(session->clientId, self.activeWebUiClientId_) == 0) {
        // Record index for the browser's offline copy: each named store
        // carries the epoch that copy holds (0: none).
        static const char *const kFields[] = {"op", "shots", "history"};
        const cJSON *shots = cJSON_GetObjectItemCaseSensitive(message, "shots");
        const cJSON *history = cJSON_GetObjectItemCaseSensitive(message, "history");
        accepted = (shots || history) && uiStreamKnownFields(message, kFields, 3) &&
                   uiStreamIntInRange(shots, 0, UINT32_MAX) &&
                   uiStreamIntInRange(history, 0, UINT32_MAX);
        if (accepted) {
          if (shots) session->recordsShotsEpoch = static_cast<uint32_t>(shots->valuedouble);
          if (history) session->recordsHistoryEpoch = static_cast<uint32_t>(history->valuedouble);
          session->recordsShots = session->recordsShots || shots;
          session->recordsHistory = session->recordsHistory || history;
          self.uiStreamUrgent_.store(true, std::memory_order_release);
        }
      } else if (cJSON_IsString(op) && !session->bound &&
                 strcmp(op->valuestring, "bind") == 0 && cJSON_IsString(client) &&
                 self.activeWebUiClientId_[0] &&
                 strcmp(client->valuestring, self.activeWebUiClientId_) == 0 &&
                 millis() - session->openedAtMs < kUiStreamBindMs) {
        copyCString(session->clientId, sizeof(session->clientId), client->valuestring);
        const cJSON *cycle = cJSON_GetObjectItemCaseSensitive(message, "cycle");
        const cJSON *boot = cJSON_GetObjectItemCaseSensitive(message, "boot");
        if (cJSON_IsNumber(cycle) && cycle->valuedouble > 0 &&
            cycle->valuedouble <= UINT32_MAX && floor(cycle->valuedouble) == cycle->valuedouble &&
            cJSON_IsNumber(boot) && boot->valuedouble >= 0 &&
            boot->valuedouble <= UINT32_MAX && floor(boot->valuedouble) == boot->valuedouble) {
          session->observedCycle = static_cast<uint32_t>(cycle->valuedouble);
          session->observedBoot = static_cast<uint32_t>(boot->valuedouble);
        }
        const cJSON *presetsTag = cJSON_GetObjectItemCaseSensitive(message, "presetsTag");
        if (presetsTag && uiStreamIntInRange(presetsTag, 1, UINT32_MAX))
          session->knownPresetsTag = static_cast<uint32_t>(presetsTag->valuedouble);
        for (auto &other : self.uiStreams_) {
          if (&other != session && other.bound) {
            superseded = other.fd;
            other.bound = false;
            other.openedAtMs = millis() - kUiStreamBindMs;
          }
        }
        session->bound = true;
        session->boundAtMs = millis();
        bound = true;
        // A malformed ops list fails like a rejected op, not like a takeover.
        accepted = !ops || cJSON_IsArray(ops);
      }
    }
    if (accepted) session->heardAtMs = millis();
    owner = session->bound && strcmp(session->clientId, self.activeWebUiClientId_) == 0;
  }
  const bool bindRejected = bindOp && !bound;
  if (!accepted && !bindRejected && owner)
    self.lifecycleLogf("UI stream op %.12s failed validation; the page resets its list views",
                       cJSON_IsString(op) ? op->valuestring : "?");
  cJSON_Delete(document);
  memset(body, 0, sizeof(body));
  if (superseded >= 0) httpd_sess_trigger_close(request->handle, superseded);
  // An accepted bind confirms the network even when a folded op then fails.
  if (bound) self.requestPendingNetworkConfirm();
  if (!accepted) {
    if (bindRejected || owner)
      sendUiStreamClose(request->handle, session->fd,
                        bindRejected ? kUiStreamCloseTakeover : kUiStreamCloseRejected);
    return ESP_FAIL;
  }
  if (activity) {
    if (resumed) self.uiStreamUrgent_.store(true, std::memory_order_release);
    uint8_t alive[] = "{\"v\":1,\"type\":\"alive\"}";
    httpd_ws_frame_t reply = {};
    reply.type = HTTPD_WS_TYPE_TEXT;
    reply.payload = alive;
    reply.len = sizeof(alive) - 1;
    return httpd_ws_send_frame_async(request->handle, session->fd, &reply);
  }
  httpd_sess_set_send_override(request->handle, session->fd, uiStreamSend);
  if (!bound) self.requestPendingNetworkConfirm();
  return ESP_OK;
}

void OpenBrewByWeightNetwork::noteScaleSample() {
  const uint32_t pending = scaleSamples_.fetch_add(1, std::memory_order_relaxed) + 1 -
                          uiStreamDispatchSamples_.load(std::memory_order_acquire);
  // Wake only when a dispatch is due and none is in flight; stream sessions
  // close before the task handle is retired.
  if (pending >= kUiStreamSampleStride && taskHandle_ != nullptr &&
      uiStreamOpen_.load(std::memory_order_acquire) &&
      !uiStreamWorkPending_.load(std::memory_order_acquire)) {
    xTaskNotifyGive(taskHandle_);
  }
}

void OpenBrewByWeightNetwork::serviceUiStream(uint32_t now) {
  if (!server_) return;
  const uint32_t samples = scaleSamples_.load(std::memory_order_acquire);
  if (!uiStreamDispatchDue(uiStreamUrgent_.load(std::memory_order_acquire),
                           now - uiStreamDispatchAtMs_,
                           samples - uiStreamDispatchSamples_.load(std::memory_order_acquire)))
    return;
  bool occupied = false;
  {
    TaskLockGuard lock(dataMux_);
    for (const auto &session : uiStreams_) occupied |= session.fd >= 0;
  }
  if (!occupied || uiStreamWorkPending_.exchange(true, std::memory_order_acq_rel)) return;
  uiStreamUrgent_.exchange(false, std::memory_order_acq_rel);
  uiStreamDispatchAtMs_ = now;
  uiStreamDispatchSamples_.store(samples, std::memory_order_release);
  if (httpd_queue_work(server_, uiStreamDispatch, this) != ESP_OK) {
    uiStreamUrgent_.store(true, std::memory_order_release);
    uiStreamWorkPending_.store(false, std::memory_order_release);
  }
}

void OpenBrewByWeightNetwork::uiStreamDispatch(void *context) {
  auto &self = *static_cast<OpenBrewByWeightNetwork *>(context);
  // One fetch per dispatch, shared by every session below: the refresh
  // handshake, the 7.4 KB control snapshot, and the home-shot copy are
  // dispatch-constant, so a takeover must not redo them per session. A live
  // card draws the status curve, so the stored one is not copied then.
  // httpd serialization stays exclusive: one take(0) for the whole
  // dispatch; if settings work holds it, the urgent flag re-arms the next
  // dispatch with a complete view.
  if (xSemaphoreTake(self.statusResponseMux_.get(), 0) != pdTRUE) {
    self.uiStreamUrgent_.store(true, std::memory_order_release);
    self.uiStreamWorkPending_.store(false, std::memory_order_release);
    return;
  }
  self.callbacks_.refreshControlStatus();
  ControlStatusSnapshot &control = self.workBuf_->control;
  self.loadControlStatus(control);
  // The RF notification can precede the committed presentation snapshot.
  if ((control.activeCycle || control.relayClosed) !=
      self.controlCriticalRfActive_.load(std::memory_order_acquire))
    self.uiStreamUrgent_.store(true, std::memory_order_release);
  ShotLogRecord latest = {};
  const bool eligible = self.callbacks_.copyHomeShot &&
      self.callbacks_.copyHomeShot(latest, control.activeCycle || control.homePending
                                               ? nullptr : &self.workBuf_->homeCurve);
  // Tick-constant network state is fetched once per dispatch too and
  // shared by the home and diagnostic projections.
  const NetworkStatusSnapshot network = self.snapshot();
  const bool live = control.activeCycle || control.homePending || control.relayClosed;
  // A bound stream may keep flowing only while the session behind its claim
  // is still valid: revoking or aging out that session closes the stream on
  // this dispatch instead of waiting for silence or a takeover.
  const uint32_t claimNowMs = millis();
  bool claimSessionLive = true;
  {
    TaskLockGuard lock(self.dataMux_);
    claimSessionLive = webAccessClaimSessionLive(
        g_webAccess, self.activeWebUiClaimToken_,
        self.activeWebUiClaimTokenValid_, claimNowMs,
        webAccessNowUtc(claimNowMs));
  }
  uiStreamDeadlineBudgetMs = kUiStreamDispatchBudgetMs;
  for (auto &session : self.uiStreams_) {
    if (session.fd < 0) continue;
    // Each session gets the budget; its close, takeover and stream writes share it.
    uiStreamDeadlineStartMs = millis();
    bool allowed, silent;
    {
      TaskLockGuard lock(self.dataMux_);
      allowed = session.bound
          ? claimSessionLive &&
                strcmp(session.clientId, self.activeWebUiClientId_) == 0
          : millis() - session.openedAtMs < kUiStreamBindMs;
      silent = allowed && session.bound && !live &&
          millis() - session.heardAtMs >= kUiStreamSilentMs;
      // Unbound until httpd frees it: later dispatches neither log nor send.
      if (silent) session.bound = false;
    }
    if (silent) {
      self.lifecycleLog("UI stream closed: page silent for 5 minutes");
      uiStreamSendKind = "close";
      sendUiStreamClose(self.server_, session.fd, kUiStreamCloseSilent);
      httpd_sess_trigger_close(self.server_, session.fd);
    } else if (!allowed) {
      if (session.bound) {
        uiStreamSendKind = "close";
        sendUiStreamClose(self.server_, session.fd, kUiStreamCloseTakeover);
      }
      httpd_sess_trigger_close(self.server_, session.fd);
    }
    else if (session.bound) {
      httpd_sess_update_lru_counter(self.server_, session.fd);
      // A hidden page still gets owed snapshots: its setup window waits for them.
      if (!session.paused || live || session.homeResync || session.resync)
        self.sendUiStream(session, control, network, latest, eligible);
    }
  }
  self.unlockWorkBuf();
  uiStreamDeadlineBudgetMs = UINT32_MAX;
  uiStreamSendKind = "control";
  self.uiStreamWorkPending_.store(false, std::memory_order_release);
}

void OpenBrewByWeightNetwork::sendUiStream(UiStreamSession &session,
                                      ControlStatusSnapshot &control,
                                      const NetworkStatusSnapshot &network,
                                      const ShotLogRecord &latest, bool eligible) {
  // Home first, then the shot card: the client's setup window waits for both
  // and must not queue behind record pages, which can pace across dispatches.
  // The dispatch deadline caps every write's wait on the single httpd task;
  // once it is spent, close the session instead of serializing more.
  const auto within = [] { return uiStreamDeadlineLeft() > 0; };
  // Home, shot card and Settings leave in one write after consuming their
  // fingerprints, so a save's revision and its Settings deltas arrive together
  // and in order: a cleanly dropped batch rolls session and fingerprints
  // back, so the next dispatch resends the same deltas; a partial one closes.
  auto &hashes = workBuf_->uiFingerprints[&session - uiStreams_];
  const UiStreamSession saved = session;
  memcpy(workBuf_->batchHomeHashes, hashes.homeHashes, sizeof(hashes.homeHashes));
  memcpy(workBuf_->batchCardHashes, hashes.cardHashes, sizeof(hashes.cardHashes));
  memcpy(workBuf_->batchSettingsHashes, hashes.settingsHashes, sizeof(hashes.settingsHashes));
  uiStreamBatchBegin();
  bool open = sendHomeStream(session, control, network) &&
      within() && sendShotCard(session, control, latest, eligible) &&
      (!session.settingsOn || (within() && sendSettingsStream(session, control)));
  uiStreamSendKind = "batch";
  if (!uiStreamBatchEnd(open) && open) {
    if (uiStreamSendCleanAbort) {
      shotTrace().noteUiDeferral();
      session = saved;
      memcpy(hashes.homeHashes, workBuf_->batchHomeHashes, sizeof(hashes.homeHashes));
      memcpy(hashes.cardHashes, workBuf_->batchCardHashes, sizeof(hashes.cardHashes));
      memcpy(hashes.settingsHashes, workBuf_->batchSettingsHashes, sizeof(hashes.settingsHashes));
      // Record pages take their page state from Home: none may overtake it.
      return;
    } else open = false;
  }
  if (open && session.diagnostic)
    open = within() && sendDiagnosticStream(session, control, network);
  if (open && session.historyOn) open = within() && sendHistoryStream(session, control);
  if (open && session.statsOn) open = within() && sendStatsStream(session, control);
  if (open && (session.recordsShots || session.recordsHistory))
    open = within() && sendRecordsIndex(session, control);
  if (open && session.logOn) open = within() && sendLogStream(session, control);
  if (!open) httpd_sess_trigger_close(server_, session.fd);
}

bool OpenBrewByWeightNetwork::sendShotCard(UiStreamSession &session,
                                      const ControlStatusSnapshot &control,
                                      const ShotLogRecord &latest, bool eligible) {
  uiStreamSendKind = "shot";
  const ShotCurveRecord *source = &workBuf_->homeCurve;
  const PersistedLastShot &cycle = control.homeCycle;
  const bool live = control.activeCycle || control.homePending;
  if (live) {
    session.observedCycle = cycle.cycleId;
    session.observedBoot = control.bootId;
  }
  const uint32_t age = millis() - control.homeResolvedAtMs;
  const bool retained = !live && cycle.valid && cycle.shotLogId == 0 &&
      session.observedBoot == control.bootId && session.observedCycle == cycle.cycleId &&
      age < kUiStreamRetainMs;
  PersistedLastShot card = {};
  const char *phase = "idle";
  if (live || retained) {
    card = cycle;
    phase = control.activeCycle ? "active" : control.homePending ? "pending" : "transient";
    source = &control.shotCurve;
  } else if (eligible) {
    card = shotLogProjectLastShot(latest, control.lastShot, control.lastGoodShot);
  } else resetShotCurveRecord(workBuf_->homeCurve);
  const ShotCurveRecord &curve = *source;
  char preset[sizeof(card.presetName) * 6];
  escapeJsonString(card.presetName, preset, sizeof(preset));
  char weight[32] = "null", flow[32] = "null", drop[16] = "null", tare[16] = "null";
  if (card.weightValid && std::isfinite(card.currentWeightG))
    snprintf(weight, sizeof(weight), "%.2f", static_cast<double>(card.currentWeightG));
  if (card.averageFlowValid && std::isfinite(card.averageFlowGps))
    snprintf(flow, sizeof(flow), "%.2f", static_cast<double>(card.averageFlowGps));
  const bool canonical = !live && !retained && eligible;
  if (canonical ? latest.firstDropDs != SHOT_LOG_METRIC_MISSING
                : card.firstDropElapsedMs != 0 || shotCurveEventPresent(curve.firstDrop))
    snprintf(drop, sizeof(drop), "%lu", static_cast<unsigned long>(card.firstDropElapsedMs));
  if (canonical ? latest.tareAtDs != SHOT_LOG_METRIC_MISSING
                : card.tareElapsedMs != 0)
    snprintf(tare, sizeof(tare), "%lu", static_cast<unsigned long>(card.tareElapsedMs));
  // Smooth-cut result: the stored verdict and cut line of a finished shot,
  // or the scale-loss backup state of the live one. A scale-loss yield is
  // estimated only when the flow projection chose the stop time.
  // A stored shot also carries its preset snapshot, so the hero draws the cut
  // line on first paint without waiting for the Stats records.
  char cutG[16] = "null", scaleLoss[80] = "null", rule[392] = "null";
  if (canonical) {
    snprintf(rule, sizeof(rule),
             "{\"fastBeforeS\":%s,\"slowAfterS\":%s,\"fastMarginG\":%s,"
             "\"slowMarginG\":%s,\"delayS\":%s,\"wallS\":%s,\"fastRampS\":%s,"
             "\"slowRampS\":%s,\"fastCurve\":\"%s\",\"slowCurve\":\"%s\","
             "\"fastCurveHardness\":\"%s\",\"slowCurveHardness\":\"%s\","
             "\"extractionGuardEnabled\":%s,\"slowExtractionGuardEnabled\":%s}",
             FixedText(latest.fastBeforeDs, 1).text, FixedText(latest.slowAfterDs, 1).text,
             FixedText(latest.fastMarginDg, 1).text, FixedText(latest.slowMarginDg, 1).text,
             FixedText(latest.delayDs, 1).text, FixedText(latest.wallDs, 1).text,
             FixedText(latest.cutRampDs, 1).text, FixedText(shotLogSlowRampDs(latest), 1).text,
             rampCurveName(corridor::rampCurveTypeOf(latest.fastCurve)),
             rampCurveName(corridor::rampCurveTypeOf(latest.slowCurve)),
             rampHardnessName(corridor::rampCurveHardnessOf(latest.fastCurve)),
             rampHardnessName(corridor::rampCurveHardnessOf(latest.slowCurve)),
             shotLogFastGuardEnabled(latest.sideFlags) ? "true" : "false",
             shotLogSlowGuardEnabled(latest.sideFlags) ? "true" : "false");
    if (latest.cutLineCg > 0 &&
        corridor::weightCutReason(shotLogArrivalRecord(latest).endReason))
      strcpy(cutG, FixedText(latest.cutLineCg, 2).text);
    if (latest.stopDetail == static_cast<uint8_t>(ShotLogStopDetail::AUTO_TO_MANUAL)) {
      char estimate[16] = "null";
      if (corridor::lossSourceOf(latest.verdict) == corridor::LossSource::PROJECTION)
        strcpy(estimate, FixedText(latest.cutLineCg, 2).text);
      snprintf(scaleLoss, sizeof(scaleLoss),
               "{\"state\":\"finished\",\"stopAtS\":%s,\"estimatedG\":%s}",
               FixedText(latest.durationDs, 1).text, estimate);
    }
  } else if (live && control.cycleAutoToManualGuardEnforced) {
    snprintf(scaleLoss, sizeof(scaleLoss), "{\"state\":\"lost\",\"stopAtS\":%.1f}",
             (card.durationMs + control.cycleAutoToManualGuardRemainingMs) / 1000.0);
  } else if (live && curve.atmClearedMs != SHOT_LOG_METRIC_MISSING) {
    strcpy(scaleLoss, "{\"state\":\"recovered\"}");
  }
  // Staleness and phase ride the envelope only when they change; markers
  // and truncation are curve scalar deltas. Only identity or sent-point
  // changes replace.
  const uint8_t stale = control.snapshotStale ? 1U : 0U;
  const uint32_t fingerprint = crc32Update(crc32Update(0xFFFFFFFFU, &stale, 1),
      reinterpret_cast<const uint8_t *>(phase), strlen(phase));
  const uint32_t prefix = uiStreamCurveHash(curve, curve.count);
  const bool replace = session.resync || session.boot != control.bootId ||
      session.cycle != card.cycleId || session.shot != card.shotLogId ||
      session.cursor > curve.count ||
      // Steady state has cursor == count; reuse the full-curve hash already
      // computed above instead of CRC-ing the same bytes a second time.
      session.prefixHash != (session.cursor == curve.count
                                 ? prefix
                                 : uiStreamCurveHash(curve, session.cursor));
  const uint16_t base = replace ? 0 : session.cursor;
  // A replacement names the whole identity; an increment carries only its
  // sequence and cursor, and continues the client's last replacement.
  size_t used = 0;
  const bool envelope = replace || session.fingerprint != fingerprint;
  if (!(replace
            ? statusJsonAppend(&used,
                  "{\"v\":1,\"boot\":%lu,\"seq\":%lu,\"snapshot\":true,\"cycle\":%lu,"
                  "\"cursor\":%u",
                  static_cast<unsigned long>(control.bootId),
                  static_cast<unsigned long>(session.sequence + 1),
                  static_cast<unsigned long>(card.cycleId), curve.count)
            : statusJsonAppend(&used, "{\"seq\":%lu,\"cursor\":%u",
                  static_cast<unsigned long>(session.sequence + 1), curve.count)) ||
      (envelope && !statusJsonAppend(&used, ",\"stale\":%s,\"phase\":\"%s\"",
                                     stale ? "true" : "false", phase)) ||
      !statusJsonAppend(&used, ",\"card\":{"))
    return false;
  // Card fields are deltas over the previous frame; a replacement sends all.
  HomeStreamDelta delta;
  delta.hashes = workBuf_->uiFingerprints[&session - uiStreams_].cardHashes;
  delta.capacity = UiStreamFingerprints::kCardFields;
  delta.used = used;
  delta.snapshot = replace;
  delta.field("valid", bool(card.valid));
  delta.field("live", bool(control.activeCycle));
  delta.field("weight", "%s", weight);
  delta.field("goal", "%s", FixedText(card.goalWeightDg, 1).text);
  delta.field("elapsedMs", "%lu", static_cast<unsigned long>(card.durationMs));
  delta.field("firstDropMs", "%s", drop);
  delta.field("tareMs", "%s", tare);
  delta.field("presetName", "\"%s\"", preset);
  delta.field("scaleAvailable", bool(card.scaleAvailable));
  delta.field("shotType", "\"%s\"", lastShotTypeName(static_cast<LastShotType>(card.shotType)));
  delta.field("endReason", "\"%s\"",
      canonical ? shotLogStopDetailName(static_cast<ShotLogStopDetail>(latest.stopDetail))
                : shotLogStopDetailName(shotLogStopDetailFromEndReason(
                      card.endReason, card.fastExtractionGuardEnabled, card.extractionExtended)));
  delta.field("averageFlowGps", "%s", flow);
  delta.field("extractionExtended", bool(card.extractionExtended));
  delta.field("slowExtractionExtended", bool(card.slowExtractionExtended));
  delta.field("touchHold", bool(control.activeCycle && control.cycleAccidentalTouchHolding));
  delta.field("verdict", "%s", canonical ? shotLogVerdictJson(latest.verdict) : "null");
  delta.field("channeling", bool(canonical && corridor::channelingOf(latest.verdict)));
  delta.field("cutG", "%s", cutG);
  delta.field("scaleLoss", "%s", scaleLoss);
  // The cut forecast on the shot clock while brewing; 0 = none.
  delta.field("expectedEndMs", "%lu",
              static_cast<unsigned long>(control.activeCycle ? control.cycleExpectedEndMs : 0U));
  // The SAP's acceleration-aware display forecast; 0 = none. The timed law
  // above stays on expectedEndMs; this only paints the Live curve.
  delta.field("fcEndMs", "%lu",
              static_cast<unsigned long>(control.activeCycle ? control.cycleForecastEndMs : 0U));
  delta.field("rule", "%s", rule);
  // Live hero tint from the shot predictor (blend + note badge).
  delta.field("tintClass", "\"%s\"", control.activeCycle
      ? shotTintJson(static_cast<ShotTint>(control.cycleTintClass)) : "neutral");
  delta.field("tintPct", "%u", control.activeCycle ? static_cast<unsigned>(control.cycleTintPct) : 0U);
  delta.field("tintNote", "\"%s\"", control.activeCycle
      ? shotTintNoteJson(static_cast<ShotTintNote>(control.cycleTintNote)) : "none");
  if (!delta.ok || !statusJsonAppend(&delta.used, "},\"curve\":{")) return false;
  HomeStreamDelta scalars;
  scalars.hashes = delta.hashes + UiStreamFingerprints::kCardFields;
  scalars.capacity = UiStreamFingerprints::kCurveFields;
  scalars.used = delta.used;
  scalars.snapshot = replace;
  (void)forEachShotCurveScalar(curve, [&](const char *key, const char *value) {
    scalars.field(key, "%s", value);
    return scalars.ok;
  });
  if (!scalars.ok) return false;
  if (!replace && !delta.changed && !scalars.changed && base == curve.count && !envelope)
    return true;
  if (!formatShotCurveJsonBody(workBuf_->curveJson, SHOT_CURVE_JSON_CAPACITY, curve, base, false) ||
      !statusJsonAppend(&scalars.used, "%s%s}}", scalars.changed ? "," : "", workBuf_->curveJson))
    return false;
  // Recheck ownership immediately before delivery; telemetry never renews it.
  bool owner;
  { TaskLockGuard lock(dataMux_); owner = strcmp(session.clientId, activeWebUiClientId_) == 0; }
  if (!owner) return false;
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t *>(workBuf_->statusJson);
  frame.len = scalars.used;
  // A clean abort defers: the dispatch batch rolls the consumed card
  // fingerprints back, and bookkeeping below only advances on delivery.
  if (httpd_ws_send_frame_async(server_, session.fd, &frame) != ESP_OK)
    return uiStreamSendCleanAbort;
  ++session.sequence;
  session.boot = control.bootId;
  session.cycle = card.cycleId;
  session.shot = card.shotLogId;
  session.cursor = curve.count;
  session.prefixHash = prefix;
  session.fingerprint = fingerprint;
  session.resync = false;
  return true;
}

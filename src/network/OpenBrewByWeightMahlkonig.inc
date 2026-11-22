// Mahlkönig Grind-by-Sync on the network task (docs/MAHLKONIG_GBS.md): the
// E64 WS state machine and its precomputed answers, the direct E65/E80 push
// client (async, one TLS connection per request, the grinder's certificate
// pinned at pairing), the route through the companion and the diagnostics.
// With the integration off nothing here runs or holds memory. The grinder's
// requests are signals only: nothing here reaches the machine.

namespace openbrewbyweight {
namespace {

constexpr size_t GBS_JOURNAL = 32, GBS_RESPONSE_BYTES = 1024;
constexpr uint32_t GBS_PUBLISH_MS = 1000;
// Above SPIRAM_MALLOC_ALWAYSINTERNAL (2048): the client's buffers land in PSRAM.
constexpr int GBS_CLIENT_BUFFER = 4096;

// httpd and the network task: counters read-modified-written by both (in
// internal RAM, where atomics are native), and the answers httpd copies out.
struct GbsCounters {
  std::atomic<bool> serving{false};
  std::atomic<uint32_t> polls{0}, seenAtMs{0}, executes{0}, recipeDg{0}, posts{0}, refused{0};
};
GbsCounters g_gbsCounters;
OPEN_BREW_BY_WEIGHT_PSRAM_BSS GbsSnapshot g_gbsMako, g_gbsMachine;

// One exchange for the Diagnostic export: no bodies, no ids.
struct GbsJournalEntry {
  uint32_t atMs = 0;
  uint8_t request = 0;  // GbsRequest; GBS_JOURNAL_* for the grinder's own requests
  uint8_t outcome = 0;  // GbsResponse::Status, or GBS_OUTCOME_*
  int16_t http = 0;     // the HTTP status, 0 without an answer
  int32_t error = 0;    // esp_err_t of a failed transport
  uint16_t bytes = 0;
};
constexpr uint8_t GBS_JOURNAL_EXECUTE = 0x80, GBS_JOURNAL_RATIO = 0x81;
constexpr uint8_t GBS_OUTCOME_TRANSPORT = 0xF0, GBS_OUTCOME_CANCELED = 0xF1, GBS_OUTCOME_PIN = 0xF2;

// The network task's state, in PSRAM while the integration is on.
struct GbsWork {
  GbsIdentity identity;
  bool identityLoaded = false, identityDirty = false, identityErase = false;
  uint8_t model = GBS_MODEL_NONE;
  GbsPull pull;
  GbsPush push;
  uint32_t executesSeen = 0, recipeSeen = 0, publishedAtMs = 0;
  uint32_t shotEpoch = 0, recordId = 0;
  bool recordKnown = false;
  // The newest shot (Settings, Home's grind advice) and, until a job claims
  // it, the facts a job seen late is attributed with.
  uint32_t lastId = 0, lastAtMs = 0, lastStartMs = 0, lastEndMs = 0;
  uint8_t lastPreset = 0;
  bool lastUnclaimed = false, lastDelivered = false;
  GbsShotReport lastReport;
  bool companionRoute = false;
  // The request in flight.
  esp_http_client_handle_t client = nullptr;
  GbsRequest request = GbsRequest::NONE;
  uint32_t startedAtMs = 0;
  int httpStatus = 0;
  size_t used = 0;
  bool overflow = false;
  char response[GBS_RESPONSE_BYTES + 1] = {};
  char body[GBS_BODY_CAPACITY] = {};
  char url[96] = {};
  GbsJournalEntry journal[GBS_JOURNAL];
  uint8_t journalNext = 0;
  uint32_t requests = 0, failures = 0;
  // What the Web UI log last said, so it records transitions only.
  GbsLink loggedLink = GbsLink::UNKNOWN;
  GbsPairPhase loggedPhase = GbsPairPhase::IDLE;
  GbsPullState loggedPull = GbsPullState::READY;
  bool loggedSeen = false, loggedRoute = false;
};
GbsWork *g_gbs = nullptr;
// Settings actions waiting for the network task (dataMux_).
enum class GbsAction : uint8_t { NONE, PAIR, CANCEL, PAIRING_ID, RELEASE, TRUST, FORGET };
GbsAction g_gbsAction = GbsAction::NONE;
uint32_t g_gbsActionPairingId = 0;
char g_gbsActionHost[GBS_HOST_CAPACITY] = {};

// The certificate the current handshake presented (network task: the verify
// callback runs inside esp_http_client_perform).
struct GbsTlsPin {
  GbsIdentity identity;
  uint8_t seen[32] = {};
  bool captured = false, mismatch = false;
};
GbsTlsPin g_gbsPin;

// The leaf certificate decides: pinned, it must be the pinned one; unpinned,
// it is captured (trust on first use, docs/MAHLKONIG_GBS.md). The grinder's
// certificate is self-signed and names no host, so nothing else is checked.
int gbsVerifyCertificate(void *, mbedtls_x509_crt *certificate, int depth, uint32_t *flags) {
  if (depth > 0) {
    *flags = 0;
    return 0;
  }
  size_t length = 0;
  if (psa_hash_compute(PSA_ALG_SHA_256, certificate->raw.p, certificate->raw.len, g_gbsPin.seen,
                       sizeof(g_gbsPin.seen), &length) != PSA_SUCCESS ||
      length != sizeof(g_gbsPin.seen))
    return 0;  // the flags stand: the handshake fails
  g_gbsPin.captured = true;
  if (gbsPinAccepts(g_gbsPin.identity, g_gbsPin.seen)) *flags = 0;
  else g_gbsPin.mismatch = true;
  return 0;
}

// esp-tls requires a CA chain before it calls the verify callback; an empty
// certificate satisfies that and verifies nothing (as esp_crt_bundle does).
esp_err_t gbsAttachPin(void *configuration) {
  static mbedtls_x509_crt empty;
  static bool ready = false;
  if (!ready) mbedtls_x509_crt_init(&empty), ready = true;
  auto *ssl = static_cast<mbedtls_ssl_config *>(configuration);
  mbedtls_ssl_conf_ca_chain(ssl, &empty, nullptr);
  mbedtls_ssl_conf_verify(ssl, gbsVerifyCertificate, nullptr);
  return ESP_OK;
}

esp_err_t gbsHttpEvent(esp_http_client_event_t *event) {
  auto *work = static_cast<GbsWork *>(event->user_data);
  if (work == nullptr) return ESP_FAIL;
  if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE)
    work->httpStatus = esp_http_client_get_status_code(event->client);
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data == nullptr || event->data_len <= 0) return ESP_OK;
  const size_t length = static_cast<size_t>(event->data_len);
  if (length > GBS_RESPONSE_BYTES - work->used) {
    work->overflow = true;
    return ESP_FAIL;
  }
  memcpy(work->response + work->used, event->data, length);
  work->used += length;
  return ESP_OK;
}

// A grinder that is off makes esp-tls and HTTP_CLIENT report every attempt;
// while this client works its lines are DEBUG traces and the client logs one
// transition with the same error codes instead (serial log hook).
struct GbsQuietLog {
  GbsQuietLog() { espLogQuietTask.store(xTaskGetCurrentTaskHandle(), std::memory_order_release); }
  ~GbsQuietLog() { espLogQuietTask.store(nullptr, std::memory_order_release); }
};

// Written under the lock the Diagnostic export reads it with.
void gbsJournal(TaskMutex &mux, GbsWork &work, uint32_t now, uint8_t request, uint8_t outcome, int http,
                int32_t error, size_t bytes) {
  TaskLockGuard lock(mux);
  GbsJournalEntry &entry = work.journal[work.journalNext];
  work.journalNext = static_cast<uint8_t>((work.journalNext + 1) % GBS_JOURNAL);
  entry = {now | 1U, request, outcome, static_cast<int16_t>(http), error,
           static_cast<uint16_t>(bytes > UINT16_MAX ? UINT16_MAX : bytes)};
}

const char *gbsJournalRequestName(uint8_t request) {
  return request == GBS_JOURNAL_EXECUTE ? "execute"
         : request == GBS_JOURNAL_RATIO ? "brewratio"
         : request <= static_cast<uint8_t>(GbsRequest::BREW_DONE)
             ? gbsRequestName(static_cast<GbsRequest>(request))
             : "unknown";
}

const char *gbsJournalOutcomeName(uint8_t outcome) {
  return outcome == GBS_OUTCOME_TRANSPORT ? "transport"
         : outcome == GBS_OUTCOME_CANCELED ? "canceled"
         : outcome == GBS_OUTCOME_PIN      ? "identity"
         : outcome <= GbsResponse::MALFORMED
             ? gbsResponseStatusName(static_cast<GbsResponse::Status>(outcome))
             : "unknown";
}

void gbsReleaseClient(GbsWork &work) {
  if (work.client == nullptr) return;
  GbsQuietLog quiet;
  (void)esp_http_client_cleanup(work.client);
  work.client = nullptr;
}

}  // namespace

// httpd (the companion link): the companion's Mahlkönig state.
void OpenBrewByWeightNetwork::mahlkonigCompanionFrame(bool own, GbsCompanionState state,
                                                     uint32_t delivered) {
  TaskLockGuard lock(dataMux_);
  g_gbsLink.own = own;
  g_gbsLink.state = state;
  g_gbsLink.delivered = delivered;
}

// cfg.gbs as the controller would send it now; empty when the companion has
// nothing to do for the grinder (network task).
size_t OpenBrewByWeightNetwork::mahlkonigCompanionConfig(char *out, size_t capacity) const {
  if (capacity) out[0] = '\0';
  if (g_gbs == nullptr || !g_gbs->identityLoaded) return 0;
  return gbsCompanionConfig(out, capacity, g_gbs->model, g_gbs->identity.host, g_gbs->identity);
}

void OpenBrewByWeightNetwork::mahlkonigView(GbsView &out) const {
  TaskLockGuard lock(dataMux_);
  out = g_gbsView;
}

// Network task: a new stored shot settles a grind (the E64 WS's execute or a
// push job in hand) or waits for a job seen late. One Web UI log line per shot.
void OpenBrewByWeightNetwork::mahlkonigShots(uint32_t now) {
  GbsWork &work = *g_gbs;
  if (callbacks_.shotLogEpoch == nullptr || callbacks_.copyShotRecords == nullptr ||
      callbacks_.bootId == nullptr)
    return;
  const uint32_t epoch = callbacks_.shotLogEpoch();
  if (epoch == work.shotEpoch ||
      (callbacks_.recordStoresSavePending != nullptr && callbacks_.recordStoresSavePending()))
    return;
  work.shotEpoch = epoch;
  ShotLogRecord shot;
  if (callbacks_.copyShotRecords(&shot, 1) == 0 || shot.id == work.recordId) return;
  const bool known = work.recordKnown;
  work.recordId = shot.id;
  work.recordKnown = true;
  // A shot from before the integration was on, or from another boot, is no grind's.
  if (!known || shot.bootId != callbacks_.bootId()) return;
  const GbsShotReport report = gbsReport(gbsShotFactsOf(shot));
  const uint32_t end = shot.endedAtMs, start = end - shot.durationDs * 100U;
  const uint32_t polls = g_gbsCounters.polls.load(std::memory_order_acquire);
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  bool claimed = false;
  if (work.model == GBS_MODEL_E64WS) claimed = gbsPullShot(work.pull, report, start, now, polls);
  else if (!work.companionRoute) claimed = gbsPushShotEnded(work.push, report, start, end, random, now);
  work.lastId = shot.id;
  work.lastAtMs = now;
  work.lastStartMs = start;
  work.lastEndMs = end;
  work.lastReport = report;
  work.lastPreset = shotLogPresetId(shot);
  work.lastDelivered = false;
  // A push job may still show up for this shot (the 25 s cadence): ask now.
  work.lastUnclaimed = !claimed && gbsModelPush(work.model) && !work.companionRoute;
  lifecycleLogf("Mahlkönig: shot %lu %s", static_cast<unsigned long>(shot.id),
                !claimed && work.lastUnclaimed ? "waits for a grinder job"
                : !claimed && work.companionRoute ? "goes to the grinder through the companion"
                : !claimed                         ? "was not ground for (not synced)"
                : report.success                   ? "will be sent with its time and yield"
                                                   : "will be sent as cancelled");
}

// Network task: one async step of the direct push client. A sync hold tears
// a request down at once (and starts none); the grinder drops kept-open
// connections, so every request opens its own.
void OpenBrewByWeightNetwork::mahlkonigClientStep(uint32_t now, bool hold, bool direct) {
  GbsWork &work = *g_gbs;
  if (work.request != GbsRequest::NONE) {
    if (hold) {
      gbsJournal(dataMux_, work, now, static_cast<uint8_t>(work.request), GBS_OUTCOME_CANCELED, 0, 0, 0);
      gbsReleaseClient(work);
      work.request = GbsRequest::NONE;
      completeOutboundAcquisitionPause(OutboundClient::MAHLKONIG_HTTP, now);
      return;
    }
    esp_err_t performed;
    {
      GbsQuietLog quiet;
      performed = esp_http_client_perform(work.client);
    }
    if (performed == ESP_ERR_HTTP_EAGAIN) {
      if (ticksSince(now, work.startedAtMs) < static_cast<int32_t>(GBS_REQUEST_TIMEOUT_MS)) return;
      performed = ESP_ERR_HTTP_READ_TIMEOUT;
    }
    mahlkonigFinishRequest(now, performed);
    return;
  }
  if (hold && outboundScaleInhibited()) completeOutboundAcquisitionPause(OutboundClient::MAHLKONIG_HTTP, now);
  const GbsRequest next = gbsPushNext(work.push, work.identity, hold, direct, now);
  if (next == GbsRequest::NONE || !gbsHostValid(work.identity.host)) return;
  const size_t length = gbsRequestBody(work.body, sizeof(work.body), next, work.push, work.identity);
  const int url = snprintf(work.url, sizeof(work.url), "https://%s:30001%s", work.identity.host, gbsRequestPath(next));
  if (length == 0 || url <= 0 || static_cast<size_t>(url) >= sizeof(work.url)) return;
  esp_http_client_config_t config{};
  config.url = work.url;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = static_cast<int>(GBS_REQUEST_TIMEOUT_MS);
  config.disable_auto_redirect = true;
  config.crt_bundle_attach = gbsAttachPin;
  config.event_handler = gbsHttpEvent;
  config.user_data = &work;
  config.buffer_size = GBS_CLIENT_BUFFER;
  config.buffer_size_tx = GBS_CLIENT_BUFFER;
  config.is_async = true;
  {
    GbsQuietLog quiet;
    work.client = esp_http_client_init(&config);
  }
  if (work.client == nullptr ||
      esp_http_client_set_header(work.client, "Content-Type", "application/json") != ESP_OK ||
      esp_http_client_set_header(work.client, "Connection", "close") != ESP_OK ||
      esp_http_client_set_post_field(work.client, work.body, static_cast<int>(length)) != ESP_OK) {
    gbsReleaseClient(work);
    return;
  }
  g_gbsPin = GbsTlsPin{};
  g_gbsPin.identity = work.identity;
  work.request = next;
  work.startedAtMs = now;
  work.httpStatus = 0;
  work.used = 0;
  work.overflow = false;
  ++work.requests;
}

// Network task: a request ended (an answer, a transport failure or the
// deadline). The answer drives the push state; a good exchange with an
// unpinned grinder pins its certificate.
void OpenBrewByWeightNetwork::mahlkonigFinishRequest(uint32_t now, esp_err_t performed) {
  GbsWork &work = *g_gbs;
  const GbsRequest request = work.request;
  work.request = GbsRequest::NONE;
  const bool transportOk = performed == ESP_OK && !work.overflow;
  const bool mismatch = g_gbsPin.mismatch;
  int tlsError = 0, tlsFlags = 0;
  const int socketErrno = work.client != nullptr ? esp_http_client_get_errno(work.client) : 0;
  if (work.client != nullptr)
    (void)esp_http_client_get_and_clear_last_tls_error(work.client, &tlsError, &tlsFlags);
  gbsReleaseClient(work);
  work.response[work.used] = '\0';
  const GbsResponse reply = transportOk ? gbsParseResponse(request, work.httpStatus, work.response, work.used)
                                        : GbsResponse{};
  if (transportOk && !mismatch && g_gbsPin.captured && !work.identity.pinned && work.httpStatus >= 200 &&
      work.httpStatus < 300) {
    memcpy(work.identity.pin, g_gbsPin.seen, sizeof(work.identity.pin));
    work.identity.pinned = 1;
    work.identityDirty = true;
    lifecycleLog("Mahlkönig: the grinder's certificate is now trusted");
  }
  const bool delivered = request == GbsRequest::BREW_DONE && transportOk && reply.status == GbsResponse::OK &&
                         work.push.result.due && work.push.result.report.success;
  if (gbsPushReply(work.push, work.identity, request, transportOk, mismatch, reply, now)) work.identityDirty = true;
  if (delivered) work.lastDelivered = work.lastId != 0;
  // A job seen after its shot: the shot that just ended may be its.
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  if (request == GbsRequest::BREW_JOBS && work.lastUnclaimed &&
      gbsPushShot(work.push, work.lastReport, work.lastStartMs, work.lastEndMs, random, now)) {
    work.lastUnclaimed = false;
    lifecycleLogf("Mahlkönig: shot %lu matched a grinder job seen after it",
                  static_cast<unsigned long>(work.lastId));
  }
  if (!transportOk || mismatch) ++work.failures;
  gbsJournal(dataMux_, work, now, static_cast<uint8_t>(request),
             mismatch ? GBS_OUTCOME_PIN : transportOk ? static_cast<uint8_t>(reply.status) : GBS_OUTCOME_TRANSPORT,
             work.httpStatus, transportOk ? 0 : performed, work.used);
  // The first failure of a run names its codes; the transition log does the rest.
  if ((!transportOk || mismatch) && work.push.failures <= 1)
    lifecycleLogf("Mahlkönig: %s to %s failed (%s, err=0x%lx errno=%d tls=0x%x/0x%x)", gbsRequestName(request),
                  work.identity.host, mismatch ? "another certificate" : "no answer",
                  static_cast<unsigned long>(performed), socketErrno, static_cast<unsigned>(-tlsError),
                  static_cast<unsigned>(tlsFlags));
}

// Network task: what the Web UI log says about the grinder, transitions only
// (never one line per poll or per request).
void OpenBrewByWeightNetwork::mahlkonigLogTransitions(uint32_t now) {
  GbsWork &work = *g_gbs;
  if (work.model == GBS_MODEL_E64WS) {
    const uint32_t seenAt = g_gbsCounters.seenAtMs.load(std::memory_order_acquire);
    const bool seen = seenAt != 0 && ticksSince(now, seenAt) < static_cast<int32_t>(GBS_SEEN_MS);
    if (seen != work.loggedSeen)
      lifecycleLog(seen ? "Mahlkönig: the E64 WS checked in"
                        : "Mahlkönig: the E64 WS has not checked in for 30 s");
    work.loggedSeen = seen;
    if (work.pull.state != work.loggedPull) {
      const GbsPull &p = work.pull;
      if (p.state == GbsPullState::ARMED && work.loggedPull != GbsPullState::BREWING)
        lifecycleLog("Mahlkönig: the grinder ground for a shot; waiting up to 3 min for it");
      else if (p.state == GbsPullState::FINISHING)
        lifecycleLogf("Mahlkönig: shot reported to the grinder (%lu.%lu s, %lu.%lu g)",
                      static_cast<unsigned long>(p.timeMs / 1000U), static_cast<unsigned long>(p.timeMs % 1000U / 100U),
                      static_cast<unsigned long>(p.volumeDg / 10U), static_cast<unsigned long>(p.volumeDg % 10U));
      else if (p.state == GbsPullState::ABORTED)
        lifecycleLog("Mahlkönig: reported to the grinder as cancelled");
      work.loggedPull = p.state;
    }
    return;
  }
  const GbsPush &push = work.push;
  if (push.phase != work.loggedPhase) {
    static const char *const phases[] = {"pairing ended", "asking the grinder to pair",
                                         "waiting for the code to be accepted on the grinder",
                                         "the grinder is already paired; its Pairing ID is needed",
                                         "pairing did not complete"};
    lifecycleLogf("Mahlkönig: %s", phases[static_cast<uint8_t>(push.phase)]);
    work.loggedPhase = push.phase;
  }
  if (push.link != work.loggedLink && push.link != GbsLink::UNKNOWN) {
    static const char *const links[] = {"", "the grinder answers", "the grinder does not answer",
                                        "the grinder no longer accepts this controller",
                                        "the grinder presented another certificate"};
    lifecycleLogf("Mahlkönig: %s", links[static_cast<uint8_t>(push.link)]);
  }
  work.loggedLink = push.link;
}

// Network task, every pass. Integration off: tear everything down (answers
// off, client closed, memory freed) and stop. The stored setup and identity
// stay for when it is turned on again.
void OpenBrewByWeightNetwork::serviceMahlkonig(uint32_t now) {
  bool enabled;
  uint8_t model, activePreset, cadenceS;
  {
    TaskLockGuard lock(dataMux_);
    enabled = settings_.runtime.integrationMahlkonig && settings_.runtime.mahlkonigModel != GBS_MODEL_NONE &&
              settings_.runtime.mahlkonigModel < GBS_MODEL_COUNT;
    model = settings_.runtime.mahlkonigModel;
    activePreset = settings_.presets.activeId;
    cadenceS = gbsCadenceS(settings_.runtime.mahlkonigPushCadenceS);
  }
  if (!enabled || model != (g_gbs != nullptr ? g_gbs->model : model)) {
    g_gbsCounters.serving.store(false, std::memory_order_release);
    if (g_gbs != nullptr) {
      if (g_gbs->request != GbsRequest::NONE) completeOutboundAcquisitionPause(OutboundClient::MAHLKONIG_HTTP, now);
      gbsReleaseClient(*g_gbs);
      g_gbs->request = GbsRequest::NONE;
      // An unsaved identity is saved (or erased) first, once flash may be written.
      if (g_gbs->identityErase || g_gbs->identityDirty) {
        if (!networkFlashWriteAllowed()) return;
        const GbsIdentity identity = g_gbs->identity;
        if (!(g_gbs->identityErase ? eraseGbsIdentity() : saveGbsIdentity(identity))) return;
      }
      lifecycleLog(enabled ? "Mahlkönig: grinder model changed" : "Mahlkönig: integration stopped");
      TaskLockGuard lock(dataMux_);
      g_gbs->~GbsWork();
      heapCapsFree(g_gbs);
      g_gbs = nullptr;
    }
    TaskLockGuard lock(dataMux_);
    g_gbsView = GbsView{};
    g_gbsAction = GbsAction::NONE;
    if (!enabled) return;
  }
  if (g_gbs == nullptr) {
    void *memory = allocExternal(sizeof(GbsWork));
    if (memory == nullptr) return;
    g_gbs = new (memory) GbsWork{};
    g_gbs->model = model;
    g_gbs->executesSeen = g_gbsCounters.executes.load(std::memory_order_acquire);
    lifecycleLogf("Mahlkönig: integration started (%s)", gbsModelName(model));
  }
  GbsWork &work = *g_gbs;
  // A new cadence counts from the next poll.
  work.push.pollMs = cadenceS * 1000U;
  // Flash is read and written only while the write gate is open.
  const bool flash = networkFlashWriteAllowed();
  if (!work.identityLoaded && flash) {
    GbsIdentity identity;
    if (loadGbsIdentity(identity) == SettingsBlobLoad::INVALID)
      lifecycleLog("Mahlkönig: the stored pairing could not be read; pair again");
    work.identity = identity;
    work.identityLoaded = true;
  }
  if (!work.identityLoaded) return;
  if (model == GBS_MODEL_E64WS && !g_gbsCounters.serving.load(std::memory_order_acquire)) {
    char deviceIdText[24] = {};
    deviceId(deviceIdText, sizeof(deviceIdText));
    char body[GBS_BODY_CAPACITY];
    g_gbsMachine.publish(body, gbsMachineBody(body, sizeof(body), deviceIdText));
    work.publishedAtMs = now - GBS_PUBLISH_MS;
  }
  // The companion's report; its route ends with the link, its switch or cfg.gbs.
  GbsCompanionReport companion;
  bool companionWanted;
  {
    TaskLockGuard lock(dataMux_);
    if (!companionLinkUp_.load(std::memory_order_acquire)) g_gbsLink = GbsCompanionReport{};
    companion = g_gbsLink;
    const CompanionStored &stored = g_companion.stored;
    companionWanted = stored.paired && stored.archiveSwitch && stored.gbsSwitch;
  }
  char config[COMPANION_GBS_CONFIG_TEXT];
  work.companionRoute = companion.own && companionWanted && mahlkonigCompanionConfig(config, sizeof(config)) != 0;
  if (work.companionRoute != work.loggedRoute)
    lifecycleLog(work.companionRoute ? "Mahlkönig: the companion now talks to the grinder"
                                     : "Mahlkönig: the controller talks to the grinder itself");
  work.loggedRoute = work.companionRoute;
  // Settings actions.
  GbsAction action;
  uint32_t pairingId;
  char pairHost[GBS_HOST_CAPACITY];
  {
    TaskLockGuard lock(dataMux_);
    action = g_gbsAction;
    pairingId = g_gbsActionPairingId;
    memcpy(pairHost, g_gbsActionHost, sizeof(pairHost));
    g_gbsAction = GbsAction::NONE;
  }
  const uint32_t polls = g_gbsCounters.polls.load(std::memory_order_acquire);
  switch (action) {
    case GbsAction::PAIR:
      if (!gbsUuidValid(work.identity.brewDeviceId, sizeof(work.identity.brewDeviceId))) {
        uint8_t random[16];
        esp_fill_random(random, sizeof(random));
        char uuid[37];
        gbsUuidV4(random, uuid);
        memcpy(work.identity.brewDeviceId, uuid, sizeof(uuid));
      }
      work.identity.paired = work.identity.pinned = 0;
      work.identity.pairingId = 0;
      memcpy(work.identity.host, pairHost, sizeof(work.identity.host));
      work.identityDirty = true;
      gbsPushPair(work.push, now);
      break;
    case GbsAction::CANCEL: work.push.phase = GbsPairPhase::IDLE; break;
    case GbsAction::PAIRING_ID:
      if (!gbsHostValid(work.identity.host)) break;
      work.identity.pairingId = pairingId;
      work.identity.paired = 1;
      work.identityDirty = true;
      work.push.phase = GbsPairPhase::IDLE;
      work.push.link = GbsLink::UNKNOWN;
      work.push.dueAtMs = now;
      break;
    case GbsAction::TRUST:
      work.identity.pinned = 0;
      work.identityDirty = true;
      work.push.link = GbsLink::UNKNOWN;
      work.push.dueAtMs = now;
      break;
    case GbsAction::FORGET:
      work.identity = GbsIdentity{};
      work.push = GbsPush{};
      work.push.pollMs = cadenceS * 1000U;
      work.identityErase = true;
      work.identityDirty = false;
      lifecycleLog("Mahlkönig: grinder pairing forgotten");
      break;
    case GbsAction::RELEASE:
      if (gbsPullRelease(work.pull, now, polls)) lifecycleLog("Mahlkönig: grinder released from its shot");
      break;
    default: break;
  }
  if (flash && work.identityErase) {
    work.identityErase = !eraseGbsIdentity();
  } else if (flash && work.identityDirty) {
    const GbsIdentity identity = work.identity;
    work.identityDirty = !saveGbsIdentity(identity);
  }
  mahlkonigShots(now);
  if (model == GBS_MODEL_E64WS) {
    // The grinder's requests, counted by httpd.
    const uint32_t executes = g_gbsCounters.executes.load(std::memory_order_acquire);
    if (executes != work.executesSeen) {
      work.executesSeen = executes;
      gbsJournal(dataMux_, work, now, GBS_JOURNAL_EXECUTE, GbsResponse::OK, 200, 0, 0);
      (void)gbsPullExecute(work.pull, now, polls);
    }
    const uint32_t recipe = g_gbsCounters.recipeDg.load(std::memory_order_acquire);
    if (recipe != work.recipeSeen) {
      work.recipeSeen = work.pull.recipeDg = recipe;
      gbsJournal(dataMux_, work, now, GBS_JOURNAL_RATIO, GbsResponse::OK, 200, 0, 0);
    }
    const GbsPullState before = work.pull.state;
    const bool changed = gbsPullTick(work.pull, now, polls, outboundShotActive.load(std::memory_order_acquire));
    // A result the grinder polled before it cleared was delivered.
    if (changed && before == GbsPullState::FINISHING && polls != work.pull.pollsAtEnter) work.lastDelivered = true;
    if (changed || before != work.pull.state || ticksSince(now, work.publishedAtMs) >= static_cast<int32_t>(GBS_PUBLISH_MS)) {
      const TimeStatusSnapshot clock = g_wallClock.snapshot(now);
      char body[GBS_BODY_CAPACITY];
      g_gbsMako.publish(body, gbsMakoBody(body, sizeof(body), work.pull, now, clock.utcSec));
      work.publishedAtMs = now;
    }
    g_gbsCounters.serving.store(true, std::memory_order_release);
  } else {
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    if (gbsPushExpire(work.push, random, now))
      lifecycleLog("Mahlkönig: no shot followed the grind; it is reported as cancelled");
    mahlkonigClientStep(now, companionSyncHold(), !work.companionRoute);
  }
  mahlkonigLogTransitions(now);
  if (work.companionRoute && companion.delivered == work.lastId && work.lastId != 0) work.lastDelivered = true;
  TaskLockGuard lock(dataMux_);
  GbsView &view = g_gbsView;
  view.enabled = true;
  view.model = model;
  view.companionRoute = work.companionRoute;
  view.paired = work.identity.paired != 0;
  memcpy(view.host, work.identity.host, sizeof(view.host));
  view.pull = work.pull.state;
  view.armedAtMs = work.pull.armedAtMs;
  view.phase = work.push.phase;
  view.pairingCode = work.push.pairingCode;
  view.link = work.push.link;
  view.companion = companion.state;
  view.seenAtMs = g_gbsCounters.seenAtMs.load(std::memory_order_acquire);
  view.okAtMs = work.push.lastOkAtMs;
  view.lastId = work.lastId;
  view.lastAtMs = work.lastAtMs;
  view.lastTimeMs = work.lastReport.timeMs;
  view.lastMassMg = work.lastReport.massMg;
  view.lastSuccess = work.lastReport.success;
  view.lastDelivered = work.lastDelivered;
  // D9: Home pauses its grind advice while the grinder corrects the grind.
  view.adjusts = work.lastDelivered && work.lastReport.success && work.lastPreset == activePreset;
  view.polls = polls;
  view.requests = work.requests;
  view.failures = work.failures;
}

// The Diagnostic export's section: counters and the exchange journal.
bool OpenBrewByWeightNetwork::mahlkonigDebugExport(httpd_req_t *request, char *buf, size_t cap) const {
  GbsView view;
  mahlkonigView(view);
  const uint32_t now = millis();
  bool ok = debugExportChunkf(
      request, buf, cap,
      "\"mahlkonig\":{\"enabled\":%s,\"model\":\"%s\",\"route\":\"%s\",\"pull\":\"%s\",\"pairing\":\"%s\","
      "\"link\":\"%s\",\"paired\":%s,\"polls\":%lu,\"posts\":%lu,\"refused\":%lu,\"requests\":%lu,"
      "\"failures\":%lu,\"seenAgeS\":%ld,\"journal\":[",
      view.enabled ? "true" : "false", gbsModelId(view.model), view.companionRoute ? "companion" : "direct",
      gbsPullStateName(view.pull), gbsPairPhaseName(view.phase), gbsLinkName(view.link),
      view.paired ? "true" : "false", static_cast<unsigned long>(view.polls),
      static_cast<unsigned long>(g_gbsCounters.posts.load()), static_cast<unsigned long>(g_gbsCounters.refused.load()),
      static_cast<unsigned long>(view.requests), static_cast<unsigned long>(view.failures),
      view.seenAtMs ? static_cast<long>((now - view.seenAtMs) / 1000U) : -1L);
  // The journal belongs to the network task; a copy under the lock that
  // guards its teardown is enough for a diagnostic read.
  GbsJournalEntry journal[GBS_JOURNAL] = {};
  uint8_t next = 0;
  {
    TaskLockGuard lock(dataMux_);
    if (g_gbs != nullptr) {
      memcpy(journal, g_gbs->journal, sizeof(journal));
      next = g_gbs->journalNext;
    }
  }
  const char *separator = "";
  for (size_t i = 0; ok && i < GBS_JOURNAL; ++i) {
    const GbsJournalEntry &e = journal[(next + i) % GBS_JOURNAL];
    if (e.atMs == 0) continue;
    ok = debugExportChunkf(request, buf, cap, "%s{\"ageS\":%lu,\"r\":\"%s\",\"o\":\"%s\",\"h\":%d,\"e\":%ld,\"b\":%u}",
                           separator, static_cast<unsigned long>((now - e.atMs) / 1000U),
                           gbsJournalRequestName(e.request), gbsJournalOutcomeName(e.outcome), e.http,
                           static_cast<long>(e.error), static_cast<unsigned>(e.bytes));
    separator = ",";
  }
  return ok && debugExportChunk(request, "]},");
}

}  // namespace openbrewbyweight

// Companion link's archive record sync (docs/COMPANION_LINK.md, plan 2A): the
// companion's `rx`/`rf`/`rt` requests, the `rs` epoch notice and the `rec`/
// `trc` answers. httpd records each request and serves it through a fetch-only
// ui-stream session, so the index, the History and Stats fetch windows and the
// trace reader are the Web UI's own; the network task decides when (never
// during a hold, a shot or a closed flash-write gate) and nothing is persisted.

namespace openbrewbyweight {

// The trace chunk is the trace route's own PSRAM buffer (httpd runs one job at
// a time), so the frame in statusJson never shares memory with its source.
static_assert(COMPANION_RECORD_ORIGIN + 128 + 4 * ((SHOT_TRACE_BUFFER_BYTES + 2) / 3) <=
                  COMPANION_RECORD_FRAME_BYTES,
              "a whole trace fits one trc frame");
static_assert(COMPANION_RECORD_FRAME_BYTES + COMPANION_RECORD_ORIGIN <= NetworkWorkBuf::kStatusJson,
              "a record frame and its reserved prefix fit the HTTP workspace");

// httpd: one request, replacing the pending one of its kind. False (and the
// link closes) on a malformed frame. A new link session starts clean.
bool OpenBrewByWeightNetwork::companionLinkRecordRequest(const CompanionLinkSlot &slot,
                                                         const cJSON *frame, const char *type) {
  CompanionRecordRequest request;
  if (!companionParseRecordRequest(frame, type, request)) return false;
  {
    TaskLockGuard lock(dataMux_);
    CompanionRecords *records = companionRecords_;
    if (records == nullptr) return true;
    if (records->generation != slot.generation) {
      const uint32_t sentAtMs = records->sentAtMs;
      *records = CompanionRecords{};
      records->generation = slot.generation;
      records->session.recordsKey = nextRecordsKey();
      records->sentAtMs = sentAtMs;
    }
    UiStreamSession &session = records->session;
    if (request.kind == CompanionRecordKind::TRACE) {
      records->trace = true;
      records->traceId = request.id;
      records->traceBoot = request.boot;
      records->traceEnd = request.end;
    } else if (request.kind == CompanionRecordKind::INDEX) {
      records->subscribed = true;
      (request.shots ? session.recordsShots : session.recordsHistory) = true;
      (request.shots ? session.recordsShotsEpoch : session.recordsHistoryEpoch) = request.epoch;
      (request.shots ? session.recordsShotsDelta : session.recordsHistoryDelta) = request.delta;
    } else if (request.shots) {
      // Newest first by date over every shot, bad ones included.
      session.statsFetch = true;
      session.statsFetchOffset = request.offset;
      session.statsFetchLimit = request.count;
      session.statsFetchSort = ShotLogSort::Date;
      session.statsFetchDir = ShotLogSortDir::Desc;
      session.statsFetchFilter = ShotListFilter{};
      session.statsFetchFilter.includeBad = true;
      session.statsFetchRequest = request.request;
    } else {
      session.historyFetch = true;
      session.historyFetchOffset = request.offset;
      session.historyFetchLimit = request.count;
      session.historyFetchDir = ShotLogSortDir::Desc;
      session.historyFetchTypes = 0;
      session.historyFetchRequest = request.request;
    }
  }
  companionRecordsDue_.store(true, std::memory_order_release);
  return true;
}

// Network task, every pass with a link: the notice (through the shared send
// job) once a change is committed, and the records work, both behind the gate.
void OpenBrewByWeightNetwork::companionRecordsService(uint32_t now, bool syncHold) {
  const uint32_t shots = callbacks_.shotLogEpoch ? callbacks_.shotLogEpoch() : 0,
                 history = callbacks_.historyEpoch ? callbacks_.historyEpoch() : 0;
  const bool gate = !syncHold;
  bool moved = false, notice = false;
  {
    TaskLockGuard lock(dataMux_);
    if (companionRecords_ == nullptr) return;  // no link has opened yet
    CompanionRecords &records = *companionRecords_;
    if (shots != records.seenShots || history != records.seenHistory) {
      records.seenShots = shots;
      records.seenHistory = history;
      records.movedAtMs = now;
    }
    records.heldSinceTold |= syncHold;
    const CompanionLinkSlot *link = companionActiveLink();
    moved = link != nullptr && !link->closing && link->generation == records.generation &&
            g_companion.stored.archiveSwitch != 0 && records.subscribed && gate &&
            (shots != records.toldShots || history != records.toldHistory);
  }
  // Committed: the store lock only while a notice could be due.
  const bool committed = moved && (callbacks_.recordStoresSavePending == nullptr ||
                                   !callbacks_.recordStoresSavePending());
  {
    TaskLockGuard lock(dataMux_);
    CompanionRecords &records = *companionRecords_;
    notice = records.noticeDue = companionNoticeDue(true, moved, committed, gate,
                                                    records.heldSinceTold, now - records.movedAtMs);
  }
  if (notice) companionNotified_.store(true, std::memory_order_release);
  // A device wants events and the shot store moved: look once it is saved.
  bool eventCheck = false;
  if (gate) {
    TaskLockGuard lock(dataMux_);
    const CompanionLinkSlot *link = companionActiveLink();
    eventCheck = link != nullptr && !link->closing && link->eventsWanted &&
                 shots != g_companion.shotEvents.epoch;
  }
  if (eventCheck && (callbacks_.recordStoresSavePending == nullptr ||
                     !callbacks_.recordStoresSavePending()))
    companionNotified_.store(true, std::memory_order_release);
  if (syncHold || now - companionRecordsAtMs_ < kStatsStreamPaceMs ||
      !companionRecordsDue_.load(std::memory_order_acquire) ||
      companionRecordsPending_.exchange(true, std::memory_order_acq_rel))
    return;
  companionRecordsDue_.store(false, std::memory_order_release);
  companionRecordsAtMs_ = now;
  if (httpd_queue_work(server_, companionRecordsWork, this) != ESP_OK) {
    companionRecordsDue_.store(true, std::memory_order_release);
    companionRecordsPending_.store(false, std::memory_order_release);
  }
}

// Network task, from companionPrepareSends: the `rs` body when
// companionRecordsService found it due and no hold began since. The caller
// marks the epochs told once the job is queued.
bool OpenBrewByWeightNetwork::companionRecordsNoticeBody(uint8_t &count, bool syncHold,
                                                         uint32_t (&told)[2]) const {
  uint32_t &shots = told[0], &history = told[1];
  bool due = false;
  {
    TaskLockGuard lock(dataMux_);
    const CompanionRecords *records = companionRecords_;
    const CompanionLinkSlot *link = companionActiveLink();
    due = !syncHold && records != nullptr && records->noticeDue && link != nullptr &&
          link->generation == records->generation && g_companion.stored.archiveSwitch != 0;
    if (due) {
      shots = records->seenShots;
      history = records->seenHistory;
    }
  }
  return due && companionAddBody(count, "\"t\":\"rs\",\"sh\":%lu,\"hi\":%lu}",
                                 static_cast<unsigned long>(shots),
                                 static_cast<unsigned long>(history));
}

// Network task, from companionPrepareSends: the `ev` body when the newest saved
// shot qualifies (companionShotEventVerdict). The caller records it once the
// job is queued; a shot that does not qualify is recorded here, with its reason
// in the Web UI log, the only diagnostic a field unit has.
bool OpenBrewByWeightNetwork::companionShotEventBody(uint8_t &count, bool syncHold,
                                                     ShotLogRecord &shot, uint32_t &epoch) {
  if (syncHold || callbacks_.shotLogEpoch == nullptr || callbacks_.copyShotRecords == nullptr ||
      callbacks_.bootId == nullptr)
    return false;
  uint32_t seen = 0, after = 0;
  {
    TaskLockGuard lock(dataMux_);
    const CompanionLinkSlot *link = companionActiveLink();
    if (link == nullptr || !link->eventsWanted) return false;
    seen = g_companion.shotEvents.epoch;
    after = g_companion.shotEvents.afterMs;
  }
  epoch = callbacks_.shotLogEpoch();
  if (epoch == seen || (callbacks_.recordStoresSavePending != nullptr &&
                        callbacks_.recordStoresSavePending()))
    return false;
  const uint32_t now = millis();
  const CompanionShotEventVerdict verdict =
      callbacks_.copyShotRecords(&shot, 1) == 0
          ? CompanionShotEventVerdict::NONE
          : companionShotEventVerdict(shot, callbacks_.bootId(), after, now);
  if (verdict == CompanionShotEventVerdict::SEND) {
    char text[256];
    if (companionShotEventText(shot, text, sizeof(text)) && companionAddBody(count, "%s", text)) {
      lifecycleLogf("Notifications: shot %lu sent to the companion",
                    static_cast<unsigned long>(shot.id));
      return true;
    }
    companionNotified_.store(true, std::memory_order_release);
    return false;
  }
  const bool skipped = verdict != CompanionShotEventVerdict::NONE;
  const char *const reason = verdict == CompanionShotEventVerdict::BAD ? "bad" : "stale";
  {
    TaskLockGuard lock(dataMux_);
    CompanionShotEvents &events = g_companion.shotEvents;
    events.epoch = epoch;
    if (skipped) {
      events.afterMs = shot.endedAtMs;
      ++events.skipped;
      events.skip = reason;
    }
  }
  if (skipped)
    lifecycleLogf("Notifications: shot %lu not sent (%s)", static_cast<unsigned long>(shot.id),
                  reason);
  return false;
}

// httpd: a record frame built in statusJson after COMPANION_RECORD_ORIGIN,
// sealed in place and sent on the link. A clean abort defers it like a
// ui-stream frame (deferArm); any other failure closes the link.
bool OpenBrewByWeightNetwork::companionRecordsSend(size_t used, bool wrap, bool &deferArm) {
  int fd = -1;
  uint8_t key[32] = {};
  uint32_t sequence = 0;
  {
    TaskLockGuard lock(dataMux_);
    const CompanionLinkSlot *link = companionActiveLink();
    if (link != nullptr && !link->closing && companionRecords_ != nullptr &&
        link->generation == companionRecords_->generation) {
      fd = link->fd;
      memcpy(key, link->key, sizeof(key));
      sequence = link->outSequence + 1;
    }
  }
  // A hold that began while the frame was built: nothing leaves, the
  // serializer keeps its state (a clean deferral) and the work re-arms.
  if (fd >= 0 && companionSyncHold()) {
    memset(key, 0, sizeof(key));
    deferArm = true;
    return true;
  }
  size_t length = 0;
  const size_t start = fd < 0 ? SIZE_MAX
      : companionSealRecord(key, sequence, wrap, g_work->statusJson, used,
                            NetworkWorkBuf::kStatusJson, length);
  memset(key, 0, sizeof(key));
  if (start == SIZE_MAX) return false;
  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.final = true;
  frame.payload = reinterpret_cast<uint8_t *>(g_work->statusJson + start);
  frame.len = length;
  deferArm = false;
  uiStreamSendKind = "companion";
  const bool sent = httpd_ws_send_frame_async(server_, fd, &frame) == ESP_OK;
  deferArm = !sent && uiStreamSendCleanAbort;
  {
    TaskLockGuard lock(dataMux_);
    for (auto &slot : g_companion.links) {
      if (slot.fd != fd) continue;
      if (sent) slot.outSequence = sequence;
      else if (!deferArm) {
        slot.closing = true;
        slot.closeReason = "a frame could not be sent";
      }
    }
    if (sent) companionRecords_->sentAtMs = millis() | 1U;
  }
  if (sent) httpd_sess_update_lru_counter(server_, fd);
  else if (!deferArm) (void)httpd_sess_trigger_close(server_, fd);
  return sent || deferArm;
}

namespace {
// The trace reader's sink: each chunk straight into the frame as base64.
bool companionTraceSink(void *context, const uint8_t *data, size_t bytes, uint32_t) {
  auto &stream = *static_cast<CompanionBase64Stream *>(context);
  stream.add(data, bytes);
  return stream.ok;
}
}  // namespace

// httpd: one `trc` frame, the trace section the trace route would send.
bool OpenBrewByWeightNetwork::companionTraceFrame() {
  uint32_t id, boot, end;
  {
    TaskLockGuard lock(dataMux_);
    id = companionRecords_->traceId;
    boot = companionRecords_->traceBoot;
    end = companionRecords_->traceEnd;
  }
  char *out = g_work->statusJson;
  size_t used = COMPANION_RECORD_ORIGIN;
  used += static_cast<size_t>(snprintf(out + used, 96, "\"t\":\"trc\",\"id\":%lu,\"b\":%lu,\"e\":%lu,",
                                       static_cast<unsigned long>(id),
                                       static_cast<unsigned long>(boot),
                                       static_cast<unsigned long>(end)));
  const size_t head = used;
  CompanionBase64Stream stream;
  stream.out = out;
  stream.capacity = NetworkWorkBuf::kStatusJson - 2;
  stream.used = used + strlen("\"r\":\"ok\",\"d\":\"");
  const ShotTraceRead read = callbacks_.streamShotTrace == nullptr ? ShotTraceRead::UNAVAILABLE
      : callbacks_.streamShotTrace(id, boot, end, 0, shotTraceDownloadChunk,
                                   sizeof(shotTraceDownloadChunk), companionTraceSink, &stream);
  const bool ok = read == ShotTraceRead::FOUND && stream.finish();
  const char *result = ok ? "ok"
                       : read == ShotTraceRead::NONE    ? "none"
                       : read == ShotTraceRead::BUSY    ? "busy"
                       : read == ShotTraceRead::PENDING ? "saving"
                                                        : "error";
  if (ok) {
    memcpy(out + head, "\"r\":\"ok\",\"d\":\"", strlen("\"r\":\"ok\",\"d\":\""));
    used = stream.used;
    out[used++] = '"';
  } else {
    used = head + static_cast<size_t>(snprintf(out + head, 24, "\"r\":\"%s\"", result));
  }
  bool deferred = false;
  if (!companionRecordsSend(used, false, deferred)) return false;
  if (!deferred) {
    TaskLockGuard lock(dataMux_);
    companionRecords_->trace = false;
  }
  return true;
}

// httpd: one frame of the oldest pending request, at most one per job; the
// network task paces the next. Requests survive a busy workspace, a closed
// gate and a deferred send; a frame that cannot be built drops them.
void OpenBrewByWeightNetwork::companionRecordsWork(void *context) {
  auto &self = *static_cast<OpenBrewByWeightNetwork *>(context);
  if (xSemaphoreTake(self.statusResponseMux_.get(), 0) != pdTRUE) {
    self.companionRecordsDue_.store(true, std::memory_order_release);
    self.companionRecordsPending_.store(false, std::memory_order_release);
    return;
  }
  bool armed;
  {
    TaskLockGuard lock(self.dataMux_);
    const CompanionLinkSlot *link = companionActiveLink();
    armed = link != nullptr && !link->closing && self.companionRecords_ != nullptr &&
            link->generation == self.companionRecords_->generation &&
            g_companion.stored.archiveSwitch != 0;
  }
  // S17 on httpd too: a hold that began after the network task queued this
  // job sends nothing; the request waits for the hold's end.
  const bool held = armed && self.companionSyncHold();
  bool more = held;
  if (armed && !held) {
    self.callbacks_.refreshControlStatus();
    ControlStatusSnapshot &control = self.workBuf_->control;
    self.loadControlStatus(control);
    CompanionRecords &records = *self.companionRecords_;
    UiStreamSession &session = records.session;
    uiStreamDeadlineStartMs = millis();
    uiStreamDeadlineBudgetMs = kUiStreamDispatchBudgetMs;
    bool ok = true;
    if (records.trace) {
      ok = self.companionTraceFrame();
    } else if (session.recordsShots || session.recordsHistory) {
      const bool shots = session.recordsShots;
      const auto read = shots ? self.callbacks_.shotLogEpoch : self.callbacks_.historyEpoch;
      const uint32_t epoch = read != nullptr ? read() : 0;
      ok = self.sendRecordsIndex(session, control);
      if (ok && !(shots ? session.recordsShots : session.recordsHistory)) {
        TaskLockGuard lock(self.dataMux_);
        (shots ? records.toldShots : records.toldHistory) = epoch;
      }
    } else if (session.historyFetch) {
      ok = self.sendHistoryStream(session, control);
    } else if (session.statsFetch) {
      ok = self.sendStatsStream(session, control);
    }
    if (!ok) {
      {
        TaskLockGuard lock(self.dataMux_);
        records.trace = session.recordsShots = session.recordsHistory = false;
        session.historyFetch = session.statsFetch = session.statsPaging = false;
      }
      static RefusalLog failed;
      if (const uint32_t count = failed.due())
        self.lifecycleLogf("Companion archive: %lu record request(s) could not be answered",
                           static_cast<unsigned long>(count));
    }
    more = records.trace || session.recordsShots || session.recordsHistory ||
           session.historyFetch || session.statsFetch;
    uiStreamDeadlineBudgetMs = UINT32_MAX;
    uiStreamSendKind = "control";
  }
  self.unlockWorkBuf();
  if (more) self.companionRecordsDue_.store(true, std::memory_order_release);
  self.companionRecordsPending_.store(false, std::memory_order_release);
}

}  // namespace openbrewbyweight

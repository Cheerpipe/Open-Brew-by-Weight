// History and Stats record senders and the record index (split out of
// OpenBrewByWeightHomeStream.inc at its size cap, behaviour unchanged): the
// browser's standing windows and fetches, and the companion archive's
// fetch-only link session (plan 2A). Included by the UI stream after it.

// Same bounded, view-scoped contract as the diagnostic sender: only a session
// holding the History subscription reaches this path. A cheap store epoch
// gates the page copy, and a component fingerprint (epoch, window, paging,
// and the raw record bytes) suppresses the send while the page is unchanged —
// the HomeStreamDelta no-change contract. A pending one-shot fetch rides the
// same frame but never disturbs the standing window's bookkeeping.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendHistoryStream(UiStreamSession &session,
                                           const ControlStatusSnapshot &control) {
  uiStreamSendKind = "history";
  const uint32_t epoch = callbacks_.historyEpoch ? callbacks_.historyEpoch() : 0;
  // The companion archive's link session only fetches: no snapshot, no standing page.
  const bool link = companionRecordsSession(session);
  if (link && !session.historyFetch) return true;
  const bool initial =
      !link && (session.historyResync || session.historyBoot != control.bootId);
  // A snapshot supersedes any pending append fetch: the client resets its
  // in-flight marker when a replace lands.
  if (initial) session.historyFetch = false;
  const bool fetch = session.historyFetch;
  if (!initial && !fetch && epoch == session.historyEpoch) return true;
  const size_t offset = fetch ? session.historyFetchOffset : session.historyOffset,
               limit = fetch ? session.historyFetchLimit : session.historyLimit;
  const ShotLogSortDir dir = fetch ? session.historyFetchDir : session.historyDir;
  // The page is copied under the store mutex by the bridge callback, so a
  // torn multi-word record is never observed. The union member is exclusive
  // to this dispatch; the shot-card tail rebuilds jsonItem only afterwards.
  HistoryPage &page = g_work->historyPage;
  if (callbacks_.copyHistoryPage) {
    callbacks_.copyHistoryPage(page, offset, limit, dir,
                               fetch ? session.historyFetchTypes : session.historyTypes);
  } else {
    page.total = page.start = page.count = 0;
    page.hasMore = false;
  }
  // Component fingerprint (the HomeStreamDelta no-change contract): the raw
  // packed record bytes plus the page/window identity. HistoryRecord is a
  // packed 16-byte POD and time-varying snapshot fields (ui) stay outside,
  // so the value is comparable and storable on snapshot sends too. Worst
  // frame size: 120 maximal records plus the ui block measure ~20.8 KiB,
  // under the client's 24 KiB frame guard.
  uint32_t fingerprint = crc32Update(0xFFFFFFFFU,
      reinterpret_cast<const uint8_t *>(&epoch), sizeof(epoch));
  const uint32_t window[4] = {static_cast<uint32_t>(page.start),
                              static_cast<uint32_t>(limit),
                              static_cast<uint32_t>(dir),
                              session.historyTypes};
  fingerprint = crc32Update(fingerprint,
      reinterpret_cast<const uint8_t *>(window), sizeof(window));
  const uint32_t paging[2] = {static_cast<uint32_t>(page.total),
                              page.hasMore ? 1u : 0u};
  fingerprint = crc32Update(fingerprint,
      reinterpret_cast<const uint8_t *>(paging), sizeof(paging));
  if (page.count)
    fingerprint = crc32Update(fingerprint,
        reinterpret_cast<const uint8_t *>(page.records),
        page.count * sizeof(HistoryRecord));
  if (!initial && !fetch && fingerprint == session.historyFingerprint) {
    // Content identical (e.g. a no-op rating bump): adopt the moved epoch so
    // idle ticks stop at the cheap gate instead of rebuilding the page.
    session.historyEpoch = epoch;
    return true;
  }
  // The fingerprint is the standing page's tag: a page that resubscribes
  // still holding it gets its snapshot with every record as a bare id.
  const bool kept = initial && session.historyKnownTag != 0 &&
                    fingerprint == session.historyKnownTag;
  // Page-level UI state (ownership, edit gate, clock) rides the Home stream.
  size_t used = link ? COMPANION_RECORD_ORIGIN : 0;
  if (!statusJsonAppend(&used,
          "{\"v\":1,\"type\":\"history\",\"boot\":%lu,\"snapshot\":%s,"
          "\"epoch\":%lu,\"request\":%lu,\"tag\":%lu,",
          static_cast<unsigned long>(control.bootId),
          initial ? "true" : "false",
          static_cast<unsigned long>(epoch),
          static_cast<unsigned long>(fetch ? session.historyFetchRequest : session.historyRequest),
          static_cast<unsigned long>(fingerprint)) ||
      !statusJsonAppend(&used,
          "\"total\":%u,\"offset\":%u,\"limit\":%u,"
          "\"hasMore\":%s,\"records\":[",
          static_cast<unsigned>(page.total), static_cast<unsigned>(page.start),
          static_cast<unsigned>(limit),
          page.hasMore ? "true" : "false"))
    return false;
  auto &held = workBuf_->uiFingerprints[link ? 0 : &session - uiStreams_];
  for (size_t index = 0; index < page.count; ++index) {
    const HistoryRecord &record = page.records[index];
    // A standing refresh names the records the page already holds by id.
    if (kept || (!initial && !fetch &&
                 uiStreamHeld(held.historyHeld, held.historyHeldCount, record.id,
                              uiStreamRowHash(&record, sizeof(record))))) {
      if (!statusJsonAppend(&used, "%s%lu", index == 0 ? "" : ",",
                            static_cast<unsigned long>(record.id)))
        return false;
      continue;
    }
    if (!statusJsonAppend(&used,
            "%s{\"id\":%lu,\"type\":\"%s\",\"durationS\":%.1f,"
            "\"hasWallTime\":%s,\"endedAtUnixSec\":%lu,"
            "\"endedAtLocalSec\":%lu%s}",
            index == 0 ? "" : ",",
            static_cast<unsigned long>(record.id),
            historyTypeName(static_cast<HistoryType>(record.type)),
            static_cast<double>(record.durationDs) / 10.0,
            (record.flags & HISTORY_FLAG_WALL_TIME) != 0 ? "true" : "false",
            static_cast<unsigned long>(record.endedAtUnixSec),
            static_cast<unsigned long>(record.endedAtLocalSec),
            (record.flags & HISTORY_FLAG_NO_WEIGHT) != 0 ? ",\"noWeight\":true" : ""))
      return false;
  }
  if (!statusJsonAppend(&used, "]}")) return false;
  // A deferred send delivered nothing: the snapshot/fetch/epoch state that
  // produced this frame is still armed, so the next dispatch retries it.
  bool deferred;
  if (!sendUiStreamFrame(session, used, deferred)) return false;
  if (deferred) return true;
  session.historyBoot = control.bootId;
  session.historyResync = false;
  session.historyKnownTag = 0;
  session.historyFetch = false;
  if (!fetch) {
    session.historyEpoch = epoch;
    session.historyFingerprint = fingerprint;
    held.historyHeldCount = static_cast<uint8_t>(page.count < kHeldRows ? page.count : kHeldRows);
    for (size_t index = 0; index < held.historyHeldCount; ++index)
      held.historyHeld[index] = {page.records[index].id,
                                 uiStreamRowHash(&page.records[index], sizeof(HistoryRecord))};
  }
  return true;
}

// Same bounded, view-scoped contract as the history sender, plus frame
// budgeting and pacing: full curves make a page larger than one frame, so a
// dispatch delivers one budgeted frame and the page continues across
// dispatch ticks (rowBase/more) until a terminating frame. The cheap store
// epoch gates the record/curve copy, and the fingerprint — epoch, window,
// aggregate, and per-row id/packed-rating/curve hashes, since records are
// immutable after commit except the packed rating byte — suppresses the
// send while the page is unchanged.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendStatsStream(UiStreamSession &session,
                                         const ControlStatusSnapshot &control) {
  uiStreamSendKind = "stats";
  uint32_t epoch =
      callbacks_.shotLogEpoch ? callbacks_.shotLogEpoch() : 0;
  // As in History, the archive's link session only fetches (and needs no
  // header); so does Home's fetch of its hero shots, with no standing page.
  const bool link = companionRecordsSession(session), rowsOnly = link || !session.statsOn;
  if (rowsOnly && !session.statsFetch) return true;
  const bool initial =
      !rowsOnly && (session.statsResync || session.statsBoot != control.bootId);
  // Finish the standing page before a later accepted fetch. Cancellation
  // belongs to subscribe/resync handling, never a delayed dispatch.
  const bool fetch = session.statsFetch && !initial &&
      (!session.statsPaging || session.statsPageFetch);
  const uint32_t request = fetch ? session.statsFetchRequest : session.statsRequest;
  // Nothing is exported during a shot, drip included: a fetch (an export
  // window or a scroll page) waits, sending nothing, until the capture ends.
  if (fetch && control.traceCapturing) return true;
  // A replacement fetch or store change restarts only the affected page.
  if (session.statsPaging && (fetch != session.statsPageFetch ||
                              session.statsPageEpoch != epoch ||
                              session.statsPageRequest != request || initial)) {
    session.statsPaging = false;
  }
  if (!session.statsPaging && !initial && !fetch && epoch == session.statsEpoch &&
      control.bbwPresetId == session.statsPresetId &&
      session.statsWindow == session.statsShownWindow)
    return true;
  // The link's records work keeps its own pace (companionRecordsService).
  if (!link && session.statsPaging && millis() - session.statsSentAtMs < kStatsStreamPaceMs) {
    uiStreamUrgent_.store(true, std::memory_order_release);
    return true;
  }
  const size_t offset = fetch ? session.statsFetchOffset : session.statsOffset,
               limit = fetch ? session.statsFetchLimit : session.statsLimit;
  const ShotLogSort sort = fetch ? session.statsFetchSort : session.statsSort;
  const ShotLogSortDir dir = fetch ? session.statsFetchDir : session.statsDir;
  const ShotListFilter &filter = fetch ? session.statsFetchFilter : session.statsFilter;
  // A fetch's header covers its own window; the standing gate above keeps
  // following the subscription's.
  const size_t statsWindow = fetch ? session.statsFetchWindow : session.statsWindow;
  const uint32_t filterHash = shotListFilterHash(filter);
  NetworkWorkBuf &work = *g_work;
  auto &cache = work.statsCache;
  auto &snapshot = cache.snapshot;
  // Only a record-epoch move reloads the cache: sort/dir toggles rebuild a
  // requested-order index permutation over the untouched newest-first
  // records, so the records copy (128 KB at 1000 summaries; curves are
  // fetched per row) happens only when the store actually changed.
  const bool reload = !cache.valid || snapshot.epoch != epoch;
  if (reload) {
    if (!callbacks_.copyShotStatsRecords) return false;
    callbacks_.copyShotStatsRecords(snapshot);
    epoch = snapshot.epoch;
    cache.curveHashes = {};
  }
  // The header follows the store, the selected window, and the active
  // preset of its "This preset" chart.
  const bool header = !rowsOnly && (!cache.valid || cache.headerEpoch != epoch ||
      cache.headerWindow != statsWindow ||
      cache.headerPresetId != control.bbwPresetId);
  if (header) {
    cache.statsOk = formatStatsHeader(cache, statsWindow, control.bbwPresetId);
    cache.headerEpoch = epoch;
    cache.headerWindow = statsWindow;
    cache.headerPresetId = control.bbwPresetId;
  }
  const size_t count = snapshot.count;
  // The order follows the store copy too: another session (or the record
  // index) may have reloaded it since, and the link never rebuilds a header.
  if (reload || header || cache.orderEpoch != epoch || cache.offset != offset || cache.limit != limit ||
      cache.sort != sort || cache.dir != dir || cache.filterHash != filterHash) {
    // Materialize the requested order as an index permutation over the
    // newest-first cache, then compact it to the filter's matches. The
    // records themselves never move, so toggles re-slice without recopying
    // or re-sorting data.
    shotLogSortOrder(snapshot.records, cache.order, count, sort, dir);
    cache.orderEpoch = epoch;
    cache.matched = cache.good = 0;
    for (size_t index = 0; index < count; ++index)
      if (shotListFilterMatches(filter, snapshot.records[cache.order[index]])) {
        cache.good += shotLogRecordEligible(snapshot.records[cache.order[index]]);
        cache.order[cache.matched++] = cache.order[index];
      }
    cache.hasMore = shotLogPageSlice(cache.matched, offset, limit, cache.start, cache.pageCount);
    uint32_t fingerprint = crc32Update(0xFFFFFFFFU,
        reinterpret_cast<const uint8_t *>(&epoch), sizeof(epoch));
    const uint32_t window[5] = {static_cast<uint32_t>(offset), static_cast<uint32_t>(limit),
                                static_cast<uint32_t>(sort), static_cast<uint32_t>(dir),
                                filterHash};
    fingerprint = crc32Update(fingerprint,
        reinterpret_cast<const uint8_t *>(window), sizeof(window));
    // The row format too: a page kept across a firmware update must not be
    // resolved from rows of the old column order.
    fingerprint = crc32Update(fingerprint,
        reinterpret_cast<const uint8_t *>(kStatsRowColumns), strlen(kStatsRowColumns));
    if (cache.statsOk)
      fingerprint = crc32Update(fingerprint,
          reinterpret_cast<const uint8_t *>(cache.header), strlen(cache.header));
    for (size_t index = cache.start; index < cache.start + cache.pageCount; ++index) {
      const ShotLogRecord &record = snapshot.records[cache.order[index]];
      // Curves are immutable once committed: an id-keyed hash lets window
      // changes re-fingerprint rows without re-fetching their curves, and
      // a missing curve hashes over zero points exactly as before.
      const uint32_t *cachedHash = cache.curveHashes.find(record.id);
      uint32_t rowHash;
      if (cachedHash != nullptr) {
        rowHash = *cachedHash;
      } else {
        // A curve past the PSRAM cache is never read for its hash: an
        // immutable stored curve hashes by its id (the row frame reads it).
        const ShotCurveRead read = callbacks_.copyShotCurveById == nullptr
            ? ShotCurveRead::NONE : callbacks_.copyShotCurveById(record.id, cache.fetchCurve, false);
        rowHash = read == ShotCurveRead::STORED ? record.id ^ 0x53544F52U  // "STOR"
            : uiStreamCurveHash(cache.fetchCurve,
                                read == ShotCurveRead::FOUND ? cache.fetchCurve.count : 0);
        cache.curveHashes.insert(record.id, rowHash);
      }
      const uint32_t row[2] = {record.id, record.sideFlags};
      fingerprint = crc32Update(fingerprint,
          reinterpret_cast<const uint8_t *>(row), sizeof(row));
      fingerprint = crc32Update(fingerprint,
          reinterpret_cast<const uint8_t *>(&rowHash), sizeof(rowHash));
    }
    cache.fingerprint = fingerprint;
    cache.offset = offset;
    cache.limit = limit;
    cache.sort = sort;
    cache.dir = dir;
    cache.filterHash = filterHash;
    cache.valid = true;
  }
  const uint32_t fingerprint = cache.fingerprint;
  const size_t start = cache.start, pageCount = cache.pageCount;
  // The owner may have committed after the cheap epoch read, before capture.
  if (session.statsPaging && session.statsPageEpoch != epoch) session.statsPaging = false;
  if (!session.statsPaging && !initial && !fetch && fingerprint == session.statsFingerprint) {
    // Content identical (e.g. a no-op rating bump): adopt the moved epoch so
    // idle ticks stop at the cheap gate instead of recopying the records.
    session.statsEpoch = epoch;
    session.statsPresetId = control.bbwPresetId;
    session.statsShownWindow = session.statsWindow;
    return true;
  }
  if (!session.statsPaging) {
    // Page start: identity is captured once and the resync/boot markers move
    // here from completion, so continuation dispatches are not initial.
    ++session.statsSeq;
    session.statsPaging = true;
    session.statsSent = 0;
    session.statsPageEpoch = epoch;
    session.statsPageRequest = request;
    session.statsPageSnapshot = initial;
    session.statsPageFetch = fetch;
    // As in History, the fingerprint is the standing page's tag.
    session.statsPageKept = initial && !fetch && session.statsKnownTag != 0 &&
                            fingerprint == session.statsKnownTag;
    session.statsKnownTag = 0;
    session.statsBoot = control.bootId;
    session.statsResync = false;
    // The header identity a standing page carries: a later window or
    // preset change, even mid-page, sends the header again.
    session.statsShownWindow = fetch ? session.statsShownWindow : session.statsWindow;
    session.statsPresetId = fetch ? session.statsPresetId : control.bbwPresetId;
  }
  const bool first = session.statsSent == 0;
  const size_t sentBase = session.statsSent;
  size_t used = link ? COMPANION_RECORD_ORIGIN : 0;
  if (!statusJsonAppend(&used,
          "{\"v\":1,\"type\":\"stats\",\"boot\":%lu,\"snapshot\":%s,"
          "\"epoch\":%lu,\"seq\":%lu,\"request\":%lu,",
          static_cast<unsigned long>(control.bootId),
          first && session.statsPageSnapshot ? "true" : "false",
          static_cast<unsigned long>(epoch),
          static_cast<unsigned long>(session.statsSeq),
          static_cast<unsigned long>(request)))
      return false;
  if (first && cache.statsOk && !rowsOnly &&
      !statusJsonAppend(&used, "\"stats\":%s,", cache.header))
    return false;
  if (first && !statusJsonAppend(&used, "\"cols\":\"%s\",\"tag\":%lu,\"good\":%u,", kStatsRowColumns,
                                 static_cast<unsigned long>(fingerprint),
                                 static_cast<unsigned>(cache.good)))
    return false;
  if (!statusJsonAppend(&used,
          "\"total\":%u,\"offset\":%u,\"limit\":%u,"
          "\"hasMore\":%s,\"rows\":[",
          static_cast<unsigned>(cache.matched), static_cast<unsigned>(start),
          static_cast<unsigned>(limit), cache.hasMore ? "true" : "false"))
    return false;
  const size_t rowsBase = used;
  auto &held = workBuf_->uiFingerprints[link ? 0 : &session - uiStreams_];
  bool busy = false;
  while (session.statsSent < pageCount) {
    const ShotLogRecord &record = snapshot.records[cache.order[start + session.statsSent]];
    // Only the packed rating byte changes after commit; curves are immutable.
    const uint32_t *curveHash = cache.curveHashes.find(record.id);
    const uint32_t key[3] = {record.id, record.sideFlags, curveHash ? *curveHash : 0U};
    const HeldRow row = {record.id, curveHash ? uiStreamRowHash(key, sizeof(key)) : 0U};
    size_t rowBytes;
    if (session.statsPageKept ||
        (row.hash && !session.statsPageSnapshot && !session.statsPageFetch &&
         uiStreamHeld(held.statsHeld, held.statsHeldCount, row.id, row.hash))) {
      // A standing refresh names the rows the page already holds by id.
      rowBytes = static_cast<size_t>(snprintf(work.jsonItem, NetworkWorkBuf::kJsonItem, "%lu",
                                              static_cast<unsigned long>(record.id)));
    } else {
      // Fetch only the curve of the row being framed; short store-lock holds
      // paced across dispatches replace the former whole-store copy. A curve
      // flash may not give now (a shot runs) holds the page at this row.
      const ShotCurveRead read = callbacks_.copyShotCurveById != nullptr
          ? callbacks_.copyShotCurveById(record.id, cache.fetchCurve, true) : ShotCurveRead::NONE;
      if ((busy = read == ShotCurveRead::BUSY)) break;
      rowBytes = formatShotStatsRow(work, record,
                                    read == ShotCurveRead::FOUND ? &cache.fetchCurve : nullptr);
    }
    if (!rowBytes) return false;
    // Close the frame before this row unless the frame is empty; a single
    // oversized row still fits under the client's frame guard alone.
    if (used + (used > rowsBase ? 1u : 0u) + rowBytes + 48u >
            kStatsStreamFrameBytes &&
        used > rowsBase)
      break;
    if (!statusJsonAppend(&used, "%s%s", used > rowsBase ? "," : "",
                          work.jsonItem))
      return false;
    if (!session.statsPageFetch && session.statsSent < kHeldRows)
      held.statsNext[session.statsSent] = row;
    ++session.statsSent;
  }
  // Nothing to send yet: the next dispatch tries the row again.
  if (busy && used == rowsBase) return true;
  const bool more = session.statsSent < pageCount;
  if (!statusJsonAppend(&used, "],\"rowBase\":%u,\"more\":%s}",
          static_cast<unsigned>(sentBase), more ? "true" : "false"))
    return false;
  // A deferred send delivered nothing: roll the buffered rows back so the
  // next dispatch rebuilds the same frame for the same page.
  bool deferred;
  if (!sendUiStreamFrame(session, used, deferred)) return false;
  if (deferred) {
    session.statsSent = sentBase;
    return true;
  }
  if (more) {
    // Give ACKs time to free TCP credit without delaying live Home updates.
    session.statsSentAtMs = millis();
    if (!link) uiStreamUrgent_.store(true, std::memory_order_release);
    return true;
  }
  session.statsPaging = false;
  if (fetch) session.statsFetch = false;
  if (!fetch) {
    session.statsEpoch = epoch;
    session.statsFingerprint = fingerprint;
    held.statsHeldCount = static_cast<uint8_t>(pageCount < kHeldRows ? pageCount : kHeldRows);
    memcpy(held.statsHeld, held.statsNext, held.statsHeldCount * sizeof(HeldRow));
  }
  return true;
}

// Made by the first companion index; without the memory every answer is a
// whole index (said in the Web UI log, rate-limited) and a later answer
// retries.
OpenBrewByWeightNetwork::RecordIndexWork *OpenBrewByWeightNetwork::recordIndexWork() {
  if (recordIndexWork_ != nullptr) return recordIndexWork_;
  if (void *memory = allocExternal(sizeof(RecordIndexWork)))
    return recordIndexWork_ = new (memory) RecordIndexWork{};
  static RefusalLog noMemory;
  if (noMemory.due() != 0)
    lifecycleLog("Record index: no memory for deltas; the companion gets whole indexes");
  return nullptr;
}

// The companion link's record index (`rx`), one store per dispatch: its
// epoch and, when that moved from the archive's, every record (bad shots
// included) newest first as an id and a 24-bit revision (a CRC of the packed
// record, so a rating change or an id reused after a clear reads as
// changed). 1000 history pairs of at most 10+8 digits stay under the frame
// budget. An epoch read ahead of a torn or newer copy only makes the next
// request resend the index. Shots reuse the Stats cache's store copy,
// reloaded as sendStatsStream does, whose header and order then rebuild.
// A listed index carries `sum`, the CRC-32 of its id/revision pairs. It also
// becomes the link session's baseline once sent, and an opted-in request
// holding the baseline's epoch gets only the difference (`base`, `up`,
// `del`) when that is smaller.
__attribute__((optimize("Os")))
bool OpenBrewByWeightNetwork::sendRecordsIndex(UiStreamSession &session,
                                          const ControlStatusSnapshot &control) {
  uiStreamSendKind = "records";
  const bool shots = session.recordsShots;
  uint32_t epoch = shots ? (callbacks_.shotLogEpoch ? callbacks_.shotLogEpoch() : 0)
                         : (callbacks_.historyEpoch ? callbacks_.historyEpoch() : 0);
  const uint32_t held = shots ? session.recordsShotsEpoch : session.recordsHistoryEpoch;
  const bool rows = epoch != held;
  auto &cache = g_work->statsCache;
  if (shots && rows && (!cache.valid || cache.snapshot.epoch != epoch)) {
    if (!callbacks_.copyShotStatsRecords) return false;
    callbacks_.copyShotStatsRecords(cache.snapshot);
    epoch = cache.snapshot.epoch;
    cache.curveHashes = {};
  }
  size_t used = COMPANION_RECORD_ORIGIN;
  // The header counters (S19), read before the rows: only a Clear or a
  // factory reset makes nextRecordId go back; the user's Clear count and
  // newest deleted ids tell the companion which removals were deletions.
  uint32_t generation = 0, next = 0;
  RecordStoreMarks marks = {};
  if (callbacks_.recordStoreCounters)
    callbacks_.recordStoreCounters(shots, generation, next, marks);
  if (!statusJsonAppend(&used, "{\"v\":1,\"type\":\"records\",\"boot\":%lu,"
                               "\"store\":\"%s\",\"epoch\":%lu,\"gen\":%lu,\"next\":%lu,"
                               "\"clr\":%lu,\"gone\":[",
                        static_cast<unsigned long>(control.bootId), shots ? "shots" : "history",
                        static_cast<unsigned long>(epoch), static_cast<unsigned long>(generation),
                        static_cast<unsigned long>(next), static_cast<unsigned long>(marks.clears)))
    return false;
  for (uint32_t n = 0; n < marks.goneCount; ++n)
    if (!statusJsonAppend(&used, "%s%lu", n ? "," : "", static_cast<unsigned long>(marks.gone[n])))
      return false;
  if (!statusJsonAppend(&used, "]")) return false;
  RecordIndexWork *const work = rows && session.recordsKey ? recordIndexWork() : nullptr;
  RecordIndexBaseline *const base = work ? &work->baselines[shots ? 0 : 1] : nullptr;
  RecordIndexBaseline *const list = work ? &work->scratch : nullptr;
  if (list != nullptr) list->count = 0;
  // Pairs leave newest first, which is id-descending in both stores; a list
  // out of that order (or a scratch overflow) only forgoes the baseline.
  bool ordered = true;
  uint32_t crc = 0xFFFFFFFFU;
  auto pair = [&](size_t n, uint32_t id, const void *record, size_t size) {
    const uint32_t words[2] = {id, uiStreamRowHash(record, size) & 0xFFFFFFU};
    // Both builds are little-endian: the bytes are id then revision, LE.
    crc = crc32Update(crc, reinterpret_cast<const uint8_t *>(words), sizeof(words));
    if (list != nullptr) {
      ordered = ordered && list->count < SHOT_LOG_CAPACITY &&
                (list->count == 0 || id < list->id[list->count - 1]);
      if (ordered) {
        list->id[list->count] = id;
        list->rev[list->count++] = words[1];
      }
    }
    return statusJsonAppend(&used, "%s%lu,%lu", n ? "," : "", static_cast<unsigned long>(id),
                            static_cast<unsigned long>(words[1]));
  };
  const size_t head = used;
  if (rows && !statusJsonAppend(&used, ",\"rows\":[")) return false;
  if (rows && shots) {
    for (size_t n = 0; n < cache.snapshot.count; ++n)
      if (!pair(n, cache.snapshot.records[n].id, &cache.snapshot.records[n],
                sizeof(ShotLogRecord)))
        return false;
  } else if (rows && callbacks_.copyHistoryPage) {
    HistoryPage &page = g_work->historyPage;
    for (size_t offset = 0;; offset += page.count) {
      callbacks_.copyHistoryPage(page, offset, HISTORY_PAGE_MAX, ShotLogSortDir::Desc, 0);
      for (size_t index = 0; index < page.count; ++index)
        if (!pair(offset + index, page.records[index].id, &page.records[index],
                  sizeof(HistoryRecord)))
          return false;
      if (!page.hasMore || !page.count) break;
    }
  }
  const unsigned long sum = ~crc;
  if (rows && !statusJsonAppend(&used, "],\"sum\":%lu", sum)) return false;
  if (rows && base != nullptr && ordered &&
      (shots ? session.recordsShotsDelta : session.recordsHistoryDelta) &&
      base->key == session.recordsKey && base->epoch == held) {
    // Merge walk of the two id-descending lists: `emit(true, i)` for a new
    // or re-revised scratch entry, `emit(false, j)` for a gone baseline one.
    auto walk = [&](auto emit) {
      for (size_t i = 0, j = 0; i < list->count || j < base->count;) {
        if (j == base->count || (i < list->count && list->id[i] > base->id[j])) {
          if (!emit(true, i++)) return false;
        } else if (i == list->count || base->id[j] > list->id[i]) {
          if (!emit(false, j++)) return false;
        } else {
          if (list->rev[i] != base->rev[j] && !emit(true, i)) return false;
          ++i, ++j;
        }
      }
      return true;
    };
    size_t ups = 0, dels = 0;
    walk([&](bool up, size_t) { return ++(up ? ups : dels); });
    if (2 * ups + dels < 2 * list->count) {
      used = head;
      size_t upCount = 0, goneCount = 0;
      const auto upPair = [&](bool up, size_t i) {
        return !up || statusJsonAppend(&used, "%s%lu,%lu", upCount++ ? "," : "",
                                       static_cast<unsigned long>(list->id[i]),
                                       static_cast<unsigned long>(list->rev[i]));
      };
      const auto gone = [&](bool up, size_t j) {
        return up || statusJsonAppend(&used, "%s%lu", goneCount++ ? "," : "",
                                      static_cast<unsigned long>(base->id[j]));
      };
      if (!statusJsonAppend(&used, ",\"base\":%lu,\"up\":[", static_cast<unsigned long>(held)) ||
          !walk(upPair) || !statusJsonAppend(&used, "],\"del\":[") || !walk(gone) ||
          !statusJsonAppend(&used, "],\"sum\":%lu", sum))
        return false;
    }
  }
  if (!statusJsonAppend(&used, "}")) return false;
  bool deferred;
  if (!sendUiStreamFrame(session, used, deferred)) return false;
  if (!deferred) {
    (shots ? session.recordsShots : session.recordsHistory) = false;
    // What the session now holds is the listed index, whichever form left.
    if (rows && base != nullptr && ordered) {
      *base = *list;
      base->key = session.recordsKey;
      base->epoch = epoch;
    }
  }
  return true;
}

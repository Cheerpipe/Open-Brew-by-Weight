// =============================================================================
// Shot trace flash pool: the settings_persist worker owns it (plan B4b, B6).
// =============================================================================
// The worker's first idle pass with the write gate open recovers the pool;
// until then it reports not ready and nothing reads or writes it. Each idle
// pass then lands a requested clear, loads the curve cache back after boot,
// and appends one record per stored shot: its curve and its trace while that
// waits in one of the two RAM buffers (a trace without a stored shot goes
// alone, after the pending curves). Appends run one flash operation per step
// and return as soon as a shot or a maintenance lease closes the gate; the
// next pass resumes them. The trace is copied by generation, so a capture
// opened over its buffer abandons the append instead of mixing two shots. A
// record being written when power is lost is lost. Factory reset clears the
// pool under the same writer mutex; control takes neither lock. Format:
// docs/SHOT_TRACE.md.

OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotTracePoolIndex shotTracePoolIndex;
// The legacy curve store is retired: the ring owns all of shotcurve but its
// two epoch sectors, and the legacy blocks read as holes it overwrites.
ShotTracePool &shotTracePool() {
  static ShotTracePool pool(shotTracePoolIndex, SHOT_TRACE_POOL_SECTORS);
  return pool;
}
// One pool writer at a time: this worker, or a factory reset (maintenance).
TaskMutex shotTracePoolWriterMutex;
// Clear all: lands in the pool before the shot log clear is persisted, so a
// power loss leaves both cleared, neither, or only the pool.
std::atomic<bool> shotTracePoolClearRequested{false};

constexpr uint8_t SHOT_TRACE_PERSIST_ATTEMPTS = 3;
// Curves the cache load reads back per idle pass, so the worker's queue is
// served between batches.
constexpr size_t SHOT_CURVE_LOAD_BATCH = 8;
// A failed recovery or clear is retried after 1 s, doubling up to 10 min.
constexpr uint32_t SHOT_TRACE_POOL_RETRY_MAX_MS = 600000;
static_assert(sizeof(ShotTracePoolRecordHeader) + 2U * sizeof(ShotTracePoolSectionHeader) +
                      ((SHOT_CURVE_SECTION_MAX_BYTES + 3U) & ~size_t(3U)) +
                      SHOT_TRACE_BUFFER_BYTES <=
                  SHOT_TRACE_POOL_MAX_RECORD_BYTES,
              "A worst-case curve and a full trace fit one record");

// Worker-owned staging: the curve section of the record being appended, or
// a curve read back while the cache loads (loading waits for no append).
union ShotCurveStaging {
  uint8_t section[SHOT_CURVE_SECTION_MAX_BYTES];
  ShotCurveRecord curve;
};
OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotCurveStaging shotCurveStaging;
// The pending trace's CRC pass, off the worker's 5,120 B stack.
OPEN_BREW_BY_WEIGHT_PSRAM_BSS uint8_t shotTraceCrcChunk[256];

// Worker-owned; the trace source reads `pending` and the curve source the
// staging until the append completes.
struct ShotTracePersistState {
  ShotTracePending pending = {};
  ShotTracePoolSource sources[2] = {};
  size_t sourceCount = 0;
  ShotTracePoolIdentity record = {};  // The record staged; attempts count per record.
  uint32_t curveId = 0, curveGeneration = 0;  // Its curve (0: none) and cache generation.
  bool hasTrace = false;
  uint8_t attempts = 0;
  // Cache load after boot: summaries from loadCursor down are left.
  uint32_t loadCursor = UINT32_MAX, loadGeneration = 0;
  bool cacheLoaded = false;
  uint32_t retryAtMs = 0, backoffMs = 0;  // After a failed recovery or clear.
  bool recoveryFailureLogged = false, writeFailureLogged = false, clearFailureLogged = false;
};
ShotTracePersistState shotTracePersist;

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
bool shotStoreWorkerWriteAllowed(bool &maintenance);
#endif

// The durable write gate, and no shot trace capture open: the capture spans
// cycle start to drip finalize, so the pool's flash I/O never meets a shot.
bool shotTracePoolGateOpen() {
  if (shotTrace().capturing()) return false;
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  bool maintenance = false;
  return shotStoreWorkerWriteAllowed(maintenance);
#else
  return !session.active && !maintenanceLease.active;
#endif
}

bool readPendingShotTrace(void *context, size_t offset, uint8_t *out, size_t bytes) {
  const ShotTracePending &pending = static_cast<ShotTracePersistState *>(context)->pending;
  return shotTrace().copyExport(pending.generation, offset, out, bytes, pending.crc);
}

// Backs the next recovery or clear attempt off after a failure.
bool shotTracePoolBackingOff() {
  const ShotTracePersistState &state = shotTracePersist;
  return state.backoffMs != 0 && static_cast<int32_t>(millis() - state.retryAtMs) < 0;
}
void shotTracePoolAttempted(bool ok) {
  ShotTracePersistState &state = shotTracePersist;
  state.backoffMs = ok ? 0
      : state.backoffMs == 0 ? 1000
      : state.backoffMs >= SHOT_TRACE_POOL_RETRY_MAX_MS / 2 ? SHOT_TRACE_POOL_RETRY_MAX_MS
      : 2U * state.backoffMs;
  state.retryAtMs = millis() + state.backoffMs;
}

bool recoverShotTracePool() {
  ShotTracePool &pool = shotTracePool();
  if (shotTracePoolBackingOff()) return false;
  const uint32_t startedAtMs = millis();
  const bool recovered = pool.recover();
  shotTracePoolAttempted(recovered);
  if (!recovered) {
    if (!shotTracePersist.recoveryFailureLogged)
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                           "Shot trace pool unavailable: recovery failed (error %u); retrying",
                           static_cast<unsigned>(pool.lastError()));
    shotTracePersist.recoveryFailureLogged = true;
    return false;
  }
  const ShotTracePoolStats stats = pool.stats(nullptr, nullptr);
  serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM,
                       "Shot trace pool ready: %lu records, %lu of %lu sectors unused, "
                       "%lu not indexed, %lu ms",
                       static_cast<unsigned long>(stats.records),
                       static_cast<unsigned long>(stats.unusedBytes / SHOT_TRACE_POOL_SECTOR_BYTES),
                       static_cast<unsigned long>(stats.poolBytes / SHOT_TRACE_POOL_SECTOR_BYTES),
                       static_cast<unsigned long>(stats.droppedRecords),
                       static_cast<unsigned long>(millis() - startedAtMs));
  return true;
}

// Writer mutex held. Returns whether no clear is pending. A pool that cannot
// be recovered or cleared, or a capture open (no pool flash I/O during a
// shot or its drip), never holds the shot log back: the request stays
// pending (and appends wait for it), and the record identity keeps old
// records apart from reused shot ids meanwhile.
bool landShotTracePoolClear() {
  if (!shotTracePoolClearRequested.load(std::memory_order_acquire)) return true;
  if (shotTrace().capturing()) return false;
  ShotTracePool &pool = shotTracePool();
  if ((!pool.ready() && !recoverShotTracePool()) || shotTracePoolBackingOff()) return false;
  const bool cleared = pool.clear();
  shotTracePoolAttempted(cleared);
  if (!cleared) {
    if (!shotTracePersist.clearFailureLogged)
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                           "Shot trace pool not cleared (error %u); retrying",
                           static_cast<unsigned>(pool.lastError()));
    shotTracePersist.clearFailureLogged = true;
    return false;
  }
  shotTracePoolClearRequested.store(false, std::memory_order_release);
  shotTracePersist.clearFailureLogged = false;
  serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM, "Shot trace pool cleared");
  return true;
}

void landShotTracePoolClearForShotStores() {
  TaskLockGuard writer(shotTracePoolWriterMutex);
  (void)landShotTracePoolClear();
}

// Factory reset, with the store lock and the writer mutex held. The cache
// and the unsaved trace go whatever else the reset did, since the shot log
// may already restart its ids; the pool is cleared and verified by a fresh
// recovery, or the clear stays requested for the worker. An unusable pool
// never fails the reset.
void resetShotRecordsForFactoryReset() {
  shotCurves.clear();
  shotTrace().consumeCompleted();
  ShotTracePool &pool = shotTracePool();
  const bool cleared = pool.clear() && pool.recover() && pool.count() == 0;
  shotTracePoolClearRequested.store(!cleared, std::memory_order_release);
  if (!cleared)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                         "Shot trace pool not cleared by the factory reset (error %u); retrying",
                         static_cast<unsigned>(pool.lastError()));
}

// Curves the pool can still take (caller holds the store lock): a pool that
// failed to recover holds nothing back, such as a planned restart.
bool shotCurvesSavePending() {
  ShotTracePool &pool = shotTracePool();
  return shotCurves.pending() != 0 &&
         (pool.ready() || pool.lastError() == ShotTracePoolError::NONE);
}

// The record is on flash, or given up: its curve stops pending (unless a
// clear came in between) and its trace is consumed.
void retireShotRecord() {
  ShotTracePersistState &state = shotTracePersist;
  if (state.curveId != 0) {
    TaskLockGuard lock(shotStoreMutex);
    if (shotCurves.generation() == state.curveGeneration) shotCurves.markSaved(state.curveId);
  }
  if (state.hasTrace) shotTrace().consume(state.pending.generation);
}

// Stages the next record: the oldest curve not in the pool yet, joined by
// its shot's trace while that is unsaved in RAM; else the oldest unsaved
// trace alone. A curve is stored at finalize while its shot's capture is
// open, and the trace completes right after: with no capture open under the
// store lock, the trace of every curve seen here is already completed (or
// never will be), so a curve never leaves without a trace that could join it.
bool stageShotRecord() {
  ShotTracePersistState &state = shotTracePersist;
  ShotTracePoolIdentity id = {};
  uint16_t curveBytes = 0;
  {
    TaskLockGuard lock(shotStoreMutex);
    if (shotTrace().capturing()) return false;
    for (const ShotCurveRecord *curve; curveBytes == 0 &&
                                       (curve = shotCurves.oldestPending()) != nullptr;) {
      const ShotLogRecord *summary = shotLog.findNewestById(curve->shotId);
      if (summary == nullptr) {  // Deleted: nothing links to it any more.
        shotCurves.markSaved(curve->shotId);
        continue;
      }
      id = {curve->shotId, summary->bootId, summary->endedAtMs, FW_VERSION_PACKED, 0};
      curveBytes = static_cast<uint16_t>(encodeShotCurveSection(*curve, shotCurveStaging.section));
      state.curveGeneration = shotCurves.generation();
    }
  }
  ShotTracePending &pending = state.pending;
  pending.shotLogId = id.shotId;
  pending.bootId = id.bootId;
  pending.endedAtMs = id.endedAtMs;
  state.hasTrace = shotTrace().pendingPersist(pending, shotTraceCrcChunk,
                                              sizeof(shotTraceCrcChunk), curveBytes != 0);
  if (curveBytes == 0 && !state.hasTrace) return false;
  // A shot that ended after the cache was read: its curve goes first, with
  // this trace, on the next pass (a trace-only record would make contains()
  // skip that curve).
  if (curveBytes == 0 && pending.shotLogId != 0) {
    TaskLockGuard lock(shotStoreMutex);
    if (shotCurves.oldestPending() != nullptr) return false;
  }
  if (state.hasTrace)
    id = {pending.shotLogId, pending.bootId, pending.endedAtMs, pending.firmwarePacked,
          pending.endReason};
  state.curveId = curveBytes != 0 ? id.shotId : 0;
  if (id.shotId != state.record.shotId || id.bootId != state.record.bootId ||
      id.endedAtMs != state.record.endedAtMs) {
    state.record = id;
    state.attempts = 0;
  }
  // A commit reported as failed may have landed: never write a shot twice.
  if (shotTracePool().contains(id.shotId, id.bootId, id.endedAtMs)) {
    retireShotRecord();
    return false;
  }
  if (state.attempts >= SHOT_TRACE_PERSIST_ATTEMPTS) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                         "Shot record of shot %lu not saved after %u attempts (error %u)",
                         static_cast<unsigned long>(id.shotId),
                         static_cast<unsigned>(SHOT_TRACE_PERSIST_ATTEMPTS),
                         static_cast<unsigned>(shotTracePool().lastError()));
    retireShotRecord();
    return false;
  }
  state.sourceCount = 0;
  if (curveBytes != 0)
    state.sources[state.sourceCount++] = {ShotTracePoolSection::CURVE, 0, curveBytes,
                                          shotCurveStaging.section, nullptr, nullptr};
  if (state.hasTrace)
    state.sources[state.sourceCount++] = {ShotTracePoolSection::TRACE, 0, pending.totalBytes,
                                          nullptr, readPendingShotTrace, &state};
  return true;
}

void finishShotRecordAppend(bool committed) {
  ShotTracePersistState &state = shotTracePersist;
  if (committed) {
    state.writeFailureLogged = false;
    retireShotRecord();
    return;
  }
  // A capture opened over the trace's buffer (begin() counted it dropped):
  // the next record is staged afresh. A busy flash lock is retried later
  // without costing an attempt.
  const ShotTracePoolError error = shotTracePool().lastError();
  if (error == ShotTracePoolError::SOURCE || error == ShotTracePoolError::LOCK) return;
  ++state.attempts;
  if (!state.writeFailureLogged)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                         "Shot record save failed (error %u); retrying",
                         static_cast<unsigned>(shotTracePool().lastError()));
  state.writeFailureLogged = true;
}

// A stored shot's curve from its pool record. Flash: hold no store lock.
// Busy while the gate is closed (a shot runs), before the first recovery and
// on a read or lock failure; a pool that failed to recover, a record without
// a curve, and one gone or torn with its recycled sector all read as no
// curve, never as damage.
ShotCurveRead readShotCurve(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs,
                            ShotCurveRecord &curve) {
  if (!shotTracePoolGateOpen()) return ShotCurveRead::BUSY;
  ShotTracePool &pool = shotTracePool();
  ShotTracePoolRef ref;
  size_t offset = 0, bytes = 0;
  // The record's CRC is checked through the stream bytes the decode refills.
  ShotTracePoolStatus status =
      pool.open(shotId, bootId, endedAtMs, ref, curve.stream, sizeof(curve.stream));
  if (status == ShotTracePoolStatus::OK)
    status = pool.findSection(ref, ShotTracePoolSection::CURVE, offset, bytes);
  if (status == ShotTracePoolStatus::OK &&
      decodeShotCurveSection(bytes, shotId, curve, [&](size_t at, void *out, size_t n) {
        return (status = pool.read(ref, offset + at, out, n)) == ShotTracePoolStatus::OK;
      }))
    return ShotCurveRead::FOUND;
  resetShotCurveRecord(curve);
  return status == ShotTracePoolStatus::IO_ERROR ||
                 (status == ShotTracePoolStatus::NOT_READY &&
                  pool.lastError() == ShotTracePoolError::NONE)
             ? ShotCurveRead::BUSY
             : ShotCurveRead::NONE;
}

// After boot: fills the cache's oldest end from the pool, newest summary
// first, until it is full or every summary was tried; at most
// SHOT_CURVE_LOAD_BATCH summaries per pass. Each curve is read with no store
// lock and joins only if no clear came in between; the cursor passes a
// summary only once its read was answered (found or none), so a busy flash
// retries it on a later pass.
void loadShotCurveCache() {
  ShotTracePersistState &state = shotTracePersist;
  for (size_t n = 0; !state.cacheLoaded && n < SHOT_CURVE_LOAD_BATCH; ++n) {
    ShotLogRecord summary = {};
    bool cached;
    size_t count;
    {
      TaskLockGuard lock(shotStoreMutex);
      if (state.loadCursor == UINT32_MAX) state.loadGeneration = shotCurves.generation();
      state.cacheLoaded = shotCurves.generation() != state.loadGeneration || shotCurves.full() ||
                          !shotLog.copyNewestBefore(state.loadCursor, summary);
      count = shotCurves.count();
      cached = !state.cacheLoaded && shotCurves.containsShotId(summary.id);
    }
    if (state.cacheLoaded) {
      serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM, "Shot curves loaded: %u in RAM",
                           static_cast<unsigned>(count));
      return;
    }
    const ShotCurveRead read = cached ? ShotCurveRead::NONE
        : readShotCurve(summary.id, summary.bootId, summary.endedAtMs, shotCurveStaging.curve);
    if (read == ShotCurveRead::BUSY) return;
    if (read == ShotCurveRead::FOUND) {
      TaskLockGuard lock(shotStoreMutex);
      if (shotCurves.generation() == state.loadGeneration)
        shotCurves.prependSaved(shotCurveStaging.curve);
    }
    state.loadCursor = summary.id;
    feedFlashIoWatchdog();
  }
}

// settings_persist idle pass; no flash I/O unless the write gate is open.
// shotStoreMutex is taken only outside the writer mutex (lock order): the
// cache load and the staging read the pool as any reader does.
void serviceShotTracePool() {
  if (!shotTracePoolGateOpen()) return;
  ShotTracePool &pool = shotTracePool();
  bool resume;
  {
    TaskLockGuard writer(shotTracePoolWriterMutex);
    if (!pool.ready() && !recoverShotTracePool()) return;
    if (!landShotTracePoolClear()) return;
    resume = pool.appending();
  }
  // The load only fills the cache's older end; new records need not wait.
  if (!resume) loadShotCurveCache();
  if (!resume && !stageShotRecord()) return;
  FlashStoreStepResult result = FlashStoreStepResult::FAILED;
  {
    TaskLockGuard writer(shotTracePoolWriterMutex);
    ShotTracePersistState &state = shotTracePersist;
    // A factory reset in between dropped the paused append: stage again.
    if (!pool.appending() && (resume || !shotTracePoolGateOpen())) return;
    // A refused begin (other than a newer trace) costs an attempt, so one
    // record can never stall the queue.
    if (pool.appending() || pool.beginAppend(state.record, state.sources, state.sourceCount)) {
      do {
        if (!shotTracePoolGateOpen()) return;  // Paused: the next pass resumes.
        result = pool.appendStep();
        if (result == FlashStoreStepResult::MORE) yieldFlashIo();
      } while (result == FlashStoreStepResult::MORE);
    }
  }
  finishShotRecordAppend(result == FlashStoreStepResult::COMPLETE);
}

// Read path of the per-shot trace endpoint (B8). Flash: hold no store lock,
// and never during a shot (the caller's gate). An interrupted trace is the
// record (shot 0, its crashed boot, end time 0). A record that fails its CRC
// sits in a sector being recycled after an interrupted erase: it reads as
// gone, never as damaged data.
ShotTracePoolStatus openShotTraceRecord(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs,
                                        ShotTracePoolRef &ref, uint8_t *buffer, size_t bytes) {
  const ShotTracePoolStatus status = shotTracePool().open(shotId, bootId, endedAtMs, ref, buffer, bytes);
  if (status != ShotTracePoolStatus::CORRUPT) return status;
  serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM,
                       "Shot trace of shot %lu is being recycled; not available",
                       static_cast<unsigned long>(shotId));
  return ShotTracePoolStatus::GONE;
}

// A stored shot whose record the worker has not written yet: its curve is
// in the cache's pending (newest) suffix, or its trace is the unsaved one.
bool shotRecordPending(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs) {
  if (shotTrace().unsaved(shotId, bootId, endedAtMs)) return true;
  TaskLockGuard lock(shotStoreMutex);
  const ShotCurveRecord *oldest = shotCurves.oldestPending();
  return shotId != 0 && oldest != nullptr && shotId >= oldest->shotId;
}

ShotTraceRead shotTraceReadOf(ShotTracePoolStatus status) {
  switch (status) {
    case ShotTracePoolStatus::OK: return ShotTraceRead::FOUND;
    case ShotTracePoolStatus::NOT_FOUND:
    case ShotTracePoolStatus::GONE: return ShotTraceRead::NONE;
    case ShotTracePoolStatus::NOT_READY:
      return shotTracePool().lastError() == ShotTracePoolError::NONE ? ShotTraceRead::PENDING
                                                                     : ShotTraceRead::UNAVAILABLE;
    default: return ShotTraceRead::UNAVAILABLE;
  }
}

// GET /api/v1/diagnostic/shot-trace?id&boot&end (B8): the trace section of a
// shot's pool record, through the caller's chunk. A stored shot is reached
// only through its summary (same identity); shot 0 is a trace without one:
// interrupted (its crashed boot, end 0) or refused by the shot log. Never
// during a shot, and PENDING until the worker wrote the record. The sink gets
// the record CRC with each chunk (the ETag; a copy with `knownCrc` is
// NOT_MODIFIED); a shot starting or the ring recycling the record mid-stream
// ends it UNAVAILABLE.
ShotTraceRead streamShotTrace(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs,
                              uint32_t knownCrc, uint8_t *chunk, size_t capacity,
                              ShotTraceSink sink, void *context) {
  if (!shotTracePoolGateOpen()) return ShotTraceRead::BUSY;
  if (shotId != 0) {
    TaskLockGuard lock(shotStoreMutex);
    const ShotLogRecord *summary = shotLog.findNewestById(shotId);
    if (summary == nullptr || summary->bootId != bootId || summary->endedAtMs != endedAtMs)
      return ShotTraceRead::NONE;
  }
  ShotTracePool &pool = shotTracePool();
  ShotTracePoolRef ref;
  size_t offset = 0, bytes = 0;
  // Pending first: a commit landing before the lookup then reads as found,
  // never as a spurious 404. A current copy costs one header read.
  const bool pending = shotRecordPending(shotId, bootId, endedAtMs);
  ShotTracePoolStatus status = pool.identify(shotId, bootId, endedAtMs, ref);
  if (status == ShotTracePoolStatus::NOT_FOUND && pending) return ShotTraceRead::PENDING;
  if (!shotTracePoolGateOpen()) return ShotTraceRead::BUSY;
  if (status == ShotTracePoolStatus::OK && knownCrc != 0 && ref.header.crc == knownCrc)
    return ShotTraceRead::NOT_MODIFIED;
  if (status == ShotTracePoolStatus::OK)
    status = openShotTraceRecord(shotId, bootId, endedAtMs, ref, chunk, capacity);
  if (status == ShotTracePoolStatus::OK)
    status = pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes);
  if (status != ShotTracePoolStatus::OK) return shotTraceReadOf(status);
  for (size_t at = 0; at < bytes; at += capacity) {
    const size_t piece = bytes - at < capacity ? bytes - at : capacity;
    if (!shotTracePoolGateOpen() || pool.read(ref, offset + at, chunk, piece) != ShotTracePoolStatus::OK ||
        !sink(context, chunk, piece, ref.header.crc))
      return ShotTraceRead::UNAVAILABLE;
  }
  return ShotTraceRead::FOUND;
}

// Shot storage's live predicate: the summaries' index tags, good ones from
// the front of the caller's scratch and bad ones from its back, each part
// sorted so every pool entry costs a binary search, not a scan of the log.
struct ShotStorageLive {
  const uint32_t *good, *bad;
  size_t goodCount, badCount;
  uint32_t goodRecords, badRecords, oldestTag;
};
bool shotStorageTagIn(const uint32_t *tags, size_t count, uint32_t tag) {
  return std::binary_search(tags, tags + count, tag);
}
// Live: a summary of its identity exists, or it is a trace without one
// (shot 0, reachable by its identity); only the former count as shots.
bool shotStorageLive(void *context, const ShotTracePoolEntry &entry) {
  ShotStorageLive &live = *static_cast<ShotStorageLive *>(context);
  if ((entry.idTag & 0xFFFFFFU) == 0) return true;
  const bool good = shotStorageTagIn(live.good, live.goodCount, entry.idTag);
  if (!good && !shotStorageTagIn(live.bad, live.badCount, entry.idTag)) return false;
  if (live.goodRecords + live.badRecords == 0) live.oldestTag = entry.idTag;
  ++(good ? live.goodRecords : live.badRecords);
  return true;
}

// GET /api/v1/diagnostic/shot-storage (B8), never during a shot. The store
// lock covers one walk of the summaries only (their tags into `tags`, the
// caller's scratch of `capacity` words, httpd's chunk); the sort and the RAM
// pool statistics run without it, and a second short hold finds the oldest
// stored shot. Then, without any store lock, one header and trace-flag read
// per live record counts the traces. Records appended or recycled during the
// walk can shift the counts by one: diagnostic figures, not accounting.
ShotTraceRead copyShotStorage(ShotStorageStats &out, uint32_t *tags, size_t capacity) {
  if (capacity < SHOT_LOG_CAPACITY) return ShotTraceRead::UNAVAILABLE;
  if (!shotTracePoolGateOpen()) return ShotTraceRead::BUSY;
  ShotTracePool &pool = shotTracePool();
  out = {};
  size_t good = 0, bad = 0;
  {
    TaskLockGuard lock(shotStoreMutex);
    (void)shotLog.findNewest([&](const ShotLogRecord &r) {
      tags[shotLogRecordEligible(r) ? good++ : capacity - ++bad] =
          shotTracePoolIdTag(r.id, r.bootId, r.endedAtMs);
      return false;
    });
    out.pendingCurves = static_cast<uint32_t>(shotCurves.pending());
  }
  uint32_t *badTags = tags + capacity - bad;
  std::make_heap(tags, tags + good);
  std::sort_heap(tags, tags + good);
  std::make_heap(badTags, badTags + bad);
  std::sort_heap(badTags, badTags + bad);
  ShotStorageLive live = {tags, badTags, good, bad, 0, 0, 0};
  const ShotTracePoolStats stats = pool.stats(shotStorageLive, &live);
  out.good = live.goodRecords;
  out.bad = live.badRecords;
  if (out.good + out.bad != 0) {
    const uint32_t oldestTag = live.oldestTag;
    TaskLockGuard lock(shotStoreMutex);
    const ShotLogRecord *oldest = shotLog.findNewest([oldestTag](const ShotLogRecord &r) {
      return shotTracePoolIdTag(r.id, r.bootId, r.endedAtMs) == oldestTag;
    });
    if (oldest != nullptr) {
      out.sinceShotId = oldest->id;
      out.sinceLocalSec = oldest->hasWallTime ? oldest->endedAtLocalSec : 0;
    }
  }
  out.ready = pool.ready();
  out.lastError = static_cast<uint8_t>(stats.lastError);
  out.poolBytes = stats.poolBytes;
  out.liveBytes = stats.liveBytes;
  out.reclaimableBytes = stats.reclaimableBytes;
  out.unindexedBytes = stats.unindexedBytes;
  out.unusedBytes = stats.unusedBytes;
  out.records = stats.records;
  out.meanRecordBytes = stats.meanRecordBytes;
  out.droppedRecords = stats.droppedRecords;
  out.pendingTraces = shotTrace().unsavedCount();
  out.tracesDropped = shotTrace().tracesDropped();
  for (size_t n = out.records; n-- != 0;) {  // Newest first, for the orphan list.
    if (!shotTracePoolGateOpen()) return ShotTraceRead::BUSY;
    if (n % 64U == 0) yieldFlashIo();
    ShotTracePoolRef ref;
    size_t offset = 0, bytes = 0;
    uint8_t flags = 0;
    if (pool.headerAt(n, ref) != ShotTracePoolStatus::OK) continue;
    const ShotTracePoolRecordHeader &h = ref.header;
    const uint32_t tag = shotTracePoolIdTag(h.shotId, h.bootId, h.endedAtMs);
    if (h.shotId != 0 && !shotStorageTagIn(tags, good, tag) &&
        !shotStorageTagIn(badTags, bad, tag))
      continue;  // A deleted or evicted shot's record.
    if (pool.findSection(ref, ShotTracePoolSection::TRACE, offset, bytes) ==
            ShotTracePoolStatus::OK &&
        pool.read(ref, offset + offsetof(ShotTraceHeader, flags), &flags, 1) ==
            ShotTracePoolStatus::OK) {
      ++out.traces;
      if (flags & (SHOT_TRACE_VERBOSE_TRUNCATED | SHOT_TRACE_DECISIONS_TRUNCATED)) ++out.truncated;
    }
    if (h.shotId != 0) continue;
    ++(h.endedAtMs != 0 ? out.traceOnly : out.interrupted);
    if (out.orphansListed < sizeof(out.orphans) / sizeof(out.orphans[0]))
      out.orphans[out.orphansListed++] = {h.bootId, h.endedAtMs};
  }
  return ShotTraceRead::FOUND;
}

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
// A reboot for the pool's RAM state and the curve cache; the host flash
// keeps its bytes.
void hostRebootShotTracePool() {
  ShotTracePool &pool = shotTracePool();
  const size_t sectors = pool.logicalEnd();  // As the firmware constructs it.
  pool.~ShotTracePool();
  new (&pool) ShotTracePool(shotTracePoolIndex, sectors);
  shotTracePersist = ShotTracePersistState();
  shotTracePoolClearRequested.store(false);
  TaskLockGuard lock(shotStoreMutex);
  shotCurves.clear();
}
#endif

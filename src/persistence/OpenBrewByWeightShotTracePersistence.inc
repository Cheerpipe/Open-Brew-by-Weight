// =============================================================================
// Shot trace flash pool: the settings_persist worker owns it (plan B4b, B6).
// =============================================================================
// The worker's first idle pass with the write gate open recovers the pool;
// until then it reports not ready and nothing reads or writes it. Each idle
// pass then lands a requested clear, loads the curve cache back after boot,
// and appends one record per stored shot: its curve and, when it is the
// newest completed one, its trace (a trace without a stored shot goes
// alone). Appends run one flash operation per step and return as soon as a
// shot or a maintenance lease closes the gate; the next pass resumes them.
// The trace is copied by generation, so a newer completion abandons the
// append (newest wins) instead of mixing two shots. A record being written
// when power is lost is lost. Factory reset clears the pool under the same
// writer mutex; control takes neither lock. Format: docs/SHOT_TRACE.md.

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

// Worker-owned staging: the curve section of the record being appended, or
// a curve read back while the cache loads (loading waits for no append).
union ShotCurveStaging {
  uint8_t section[SHOT_CURVE_SECTION_MAX_BYTES];
  ShotCurveRecord curve;
};
OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotCurveStaging shotCurveStaging;

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

bool recoverShotTracePool() {
  ShotTracePool &pool = shotTracePool();
  const uint32_t startedAtMs = millis();
  if (!pool.recover()) {
    if (!shotTracePersist.recoveryFailureLogged)
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                           "Shot trace pool unavailable: recovery failed (error %u)",
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

// Writer mutex held. A pool that cannot be recovered never blocks the shot
// log: the request stays pending and the linkage keeps old records apart.
bool landShotTracePoolClear() {
  if (!shotTracePoolClearRequested.load(std::memory_order_acquire)) return true;
  ShotTracePool &pool = shotTracePool();
  if (!pool.ready() && !recoverShotTracePool()) return true;
  if (!pool.clear()) {
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

bool landShotTracePoolClearForShotStores() {
  TaskLockGuard writer(shotTracePoolWriterMutex);
  return landShotTracePoolClear();
}

void noteShotTraceConsumed(uint32_t generation) {
  const uint32_t dropped = shotTrace().consume(generation);
  if (dropped != 0)
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM,
                         "%lu shot trace(s) not saved: a newer shot ended first",
                         static_cast<unsigned long>(dropped));
}

// The record is on flash, or given up: its curve stops pending (unless a
// clear came in between) and its trace is consumed.
void retireShotRecord() {
  ShotTracePersistState &state = shotTracePersist;
  if (state.curveId != 0) {
    TaskLockGuard lock(shotStoreMutex);
    if (shotCurves.generation() == state.curveGeneration) shotCurves.markSaved(state.curveId);
  }
  if (state.hasTrace) noteShotTraceConsumed(state.pending.generation);
}

// Stages the next record: the oldest curve not in the pool yet, joined by
// its shot's trace when that is the newest completed one; else that trace
// alone. A curve is stored at finalize while its shot's capture is open, and
// the trace completes right after: with no capture open under the store
// lock, the trace of every curve seen here is already completed (or never
// will be), so a curve never leaves without a trace that could join it.
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
  uint8_t chunk[256];
  const ShotTracePending &pending = state.pending;
  const bool trace = shotTrace().pendingPersist(state.pending, chunk, sizeof(chunk));
  state.hasTrace = trace && (curveBytes == 0 || (pending.shotLogId == id.shotId &&
                                                 pending.bootId == id.bootId &&
                                                 pending.endedAtMs == id.endedAtMs));
  if (curveBytes == 0 && !trace) return false;
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
  // A newer shot ended meanwhile: it is next, and this trace counts as
  // dropped (its curve goes alone).
  if (shotTracePool().lastError() == ShotTracePoolError::SOURCE) return;
  ++state.attempts;
  if (!state.writeFailureLogged)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                         "Shot record save failed (error %u); retrying",
                         static_cast<unsigned>(shotTracePool().lastError()));
  state.writeFailureLogged = true;
}

// A stored shot's curve from its pool record. Flash: hold no store lock.
// Busy while the gate is closed (a shot runs) or before the first recovery;
// a pool that failed to recover, a record without a curve, and one gone or
// torn with its recycled sector all read as no curve, never as damage.
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
  if (status == ShotTracePoolStatus::NOT_READY && pool.lastError() == ShotTracePoolError::NONE)
    return ShotCurveRead::BUSY;
  if (status == ShotTracePoolStatus::OK &&
      decodeShotCurveSection(bytes, shotId, curve, [&](size_t at, void *out, size_t n) {
        return pool.read(ref, offset + at, out, n) == ShotTracePoolStatus::OK;
      }))
    return ShotCurveRead::FOUND;
  resetShotCurveRecord(curve);
  return ShotCurveRead::NONE;
}

// After boot: fills the cache's oldest end from the pool, newest summary
// first, until it is full or every summary was tried. Each curve is read
// with no store lock and joins only if no clear came in between. Returns
// false while the gate pauses it.
bool loadShotCurveCache() {
  ShotTracePersistState &state = shotTracePersist;
  while (!state.cacheLoaded) {
    if (!shotTracePoolGateOpen()) return false;
    ShotLogRecord summary;
    {
      TaskLockGuard lock(shotStoreMutex);
      if (state.loadCursor == UINT32_MAX) state.loadGeneration = shotCurves.generation();
      state.cacheLoaded = shotCurves.generation() != state.loadGeneration || shotCurves.full() ||
                          !shotLog.copyNewestBefore(state.loadCursor, summary);
      if (state.cacheLoaded) {
        serialTraceCategoryf(LogLevel::INFO, DebugCategory::SYSTEM,
                             "Shot curves loaded: %u in RAM",
                             static_cast<unsigned>(shotCurves.count()));
        break;
      }
      state.loadCursor = summary.id;
      if (shotCurves.containsShotId(summary.id)) continue;
    }
    if (readShotCurve(summary.id, summary.bootId, summary.endedAtMs, shotCurveStaging.curve) ==
        ShotCurveRead::FOUND) {
      TaskLockGuard lock(shotStoreMutex);
      if (shotCurves.generation() == state.loadGeneration)
        shotCurves.prependSaved(shotCurveStaging.curve);
    }
    feedFlashIoWatchdog();
    yieldFlashIo();
  }
  return true;
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
  if (!resume && (!loadShotCurveCache() || !stageShotRecord())) return;
  FlashStoreStepResult result;
  {
    TaskLockGuard writer(shotTracePoolWriterMutex);
    ShotTracePersistState &state = shotTracePersist;
    // A factory reset in between dropped the paused append: stage again.
    if (!pool.appending() &&
        (resume || !shotTracePoolGateOpen() ||
         !pool.beginAppend(state.record, state.sources, state.sourceCount)))
      return;
    do {
      if (!shotTracePoolGateOpen()) return;  // Paused: the next pass resumes.
      result = pool.appendStep();
      if (result == FlashStoreStepResult::MORE) yieldFlashIo();
    } while (result == FlashStoreStepResult::MORE);
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

// =============================================================================
// Shot trace flash pool: the settings_persist worker owns it (plan B4b).
// =============================================================================
// The worker's first idle pass with the write gate open recovers the pool;
// until then it reports not ready and nothing reads or writes it. Each idle
// pass then lands a requested clear and appends the newest completed shot
// trace, one flash operation per step, returning as soon as a shot or a
// maintenance lease closes the gate; the next pass resumes the append. The
// trace is copied by generation, so a newer completion abandons the append
// (newest wins) instead of mixing two shots. A record being written when
// power is lost is lost. Factory reset clears the pool under the same writer
// mutex; control takes neither lock. Format: docs/SHOT_TRACE.md.

OPEN_BREW_BY_WEIGHT_PSRAM_BSS ShotTracePoolIndex shotTracePoolIndex;
ShotTracePool &shotTracePool() {
  static ShotTracePool pool(shotTracePoolIndex);
  return pool;
}
// One pool writer at a time: this worker, or a factory reset (maintenance).
TaskMutex shotTracePoolWriterMutex;
// Clear all: lands in the pool before the shot log clear is persisted, so a
// power loss leaves both cleared, neither, or only the pool.
std::atomic<bool> shotTracePoolClearRequested{false};

constexpr uint8_t SHOT_TRACE_PERSIST_ATTEMPTS = 3;

// Worker-owned; `source` reads `pending` until the append completes.
struct ShotTracePersistState {
  ShotTracePending pending = {};
  ShotTracePoolSource source = {};
  uint32_t attemptsGeneration = 0;
  uint8_t attempts = 0;
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

bool beginShotTraceAppend() {
  ShotTracePersistState &state = shotTracePersist;
  uint8_t chunk[256];
  if (!shotTrace().pendingPersist(state.pending, chunk, sizeof(chunk))) return false;
  const ShotTracePending &pending = state.pending;
  if (state.attemptsGeneration != pending.generation) {
    state.attemptsGeneration = pending.generation;
    state.attempts = 0;
  }
  // A commit reported as failed may have landed: never write a shot twice.
  if (shotTracePool().contains(pending.shotLogId, pending.bootId, pending.endedAtMs)) {
    noteShotTraceConsumed(pending.generation);
    return false;
  }
  if (state.attempts >= SHOT_TRACE_PERSIST_ATTEMPTS) {
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                         "Shot trace of shot %lu not saved after %u attempts (error %u)",
                         static_cast<unsigned long>(pending.shotLogId),
                         static_cast<unsigned>(SHOT_TRACE_PERSIST_ATTEMPTS),
                         static_cast<unsigned>(shotTracePool().lastError()));
    noteShotTraceConsumed(pending.generation);
    return false;
  }
  state.source = {ShotTracePoolSection::TRACE, 0, pending.totalBytes, nullptr,
                  readPendingShotTrace, &state};
  return shotTracePool().beginAppend({pending.shotLogId, pending.bootId, pending.endedAtMs,
                                      pending.firmwarePacked, pending.endReason},
                                     &state.source, 1);
}

void finishShotTraceAppend(bool committed) {
  ShotTracePersistState &state = shotTracePersist;
  if (committed) {
    state.writeFailureLogged = false;
    noteShotTraceConsumed(state.pending.generation);
    return;
  }
  // A newer shot ended meanwhile: it is next, and this one counts as dropped.
  if (shotTracePool().lastError() == ShotTracePoolError::SOURCE) return;
  ++state.attempts;
  if (!state.writeFailureLogged)
    serialTraceCategoryf(LogLevel::WARNING, DebugCategory::SYSTEM,
                         "Shot trace save failed (error %u); retrying",
                         static_cast<unsigned>(shotTracePool().lastError()));
  state.writeFailureLogged = true;
}

// settings_persist idle pass; no flash I/O unless the write gate is open.
void serviceShotTracePool() {
  if (!shotTracePoolGateOpen()) return;
  TaskLockGuard writer(shotTracePoolWriterMutex);
  ShotTracePool &pool = shotTracePool();
  if (!pool.ready() && !recoverShotTracePool()) return;
  if (!landShotTracePoolClear()) return;
  if (!pool.appending() && !beginShotTraceAppend()) return;
  FlashStoreStepResult result;
  do {
    if (!shotTracePoolGateOpen()) return;  // Paused: the next pass resumes.
    result = pool.appendStep();
    if (result == FlashStoreStepResult::MORE) yieldFlashIo();
  } while (result == FlashStoreStepResult::MORE);
  finishShotTraceAppend(result == FlashStoreStepResult::COMPLETE);
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
// A reboot for the pool's RAM state; the host flash keeps its bytes.
void hostRebootShotTracePool() {
  ShotTracePool &pool = shotTracePool();
  pool.~ShotTracePool();
  new (&pool) ShotTracePool(shotTracePoolIndex);
  shotTracePersist = ShotTracePersistState();
  shotTracePoolClearRequested.store(false);
}
#endif

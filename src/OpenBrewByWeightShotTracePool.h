#pragma once

// =============================================================================
// LAYER: Shot trace pool (log-structured FIFO flash ring of shot records)
// =============================================================================
// WHAT: One immutable, variable-size record per shot (tagged sections: curve,
//       trace) written back to back over one logical space: the `reserved`
//       partition, then `shotcurve` without its two legacy epoch sectors. No
//       filesystem, garbage collection or copy-forward: the writer erases the
//       next 4 KiB sector before using it, and the records that start there
//       are gone. On-flash format and recovery: docs/SHOT_TRACE.md.
//
// OWNERSHIP: the settings_persist worker recovers, appends and clears, one
//       flash operation per step, and only outside a shot (its caller's
//       gate). Readers look the RAM index up under the pool's leaf mutex,
//       release it, read flash under the flash I/O lock and revalidate the
//       record after every chunk, so a recycled sector is reported as GONE.
//       The control loop takes neither lock.

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightShotLogTypes.h"
#include "OpenBrewByWeightTaskMutex.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <mutex>
#else
#include <esp_partition.h>
#endif

namespace openbrewbyweight {

constexpr size_t SHOT_TRACE_POOL_SECTOR_BYTES = FLASH_IO_SECTOR_BYTES;
// Partition sizes from idf/partitions-n16r8.csv, frozen: the table cannot
// change by OTA. The first two shotcurve sectors keep the legacy curve epoch.
constexpr size_t SHOT_TRACE_POOL_RESERVED_BYTES = 0x14F000;
constexpr size_t SHOT_TRACE_POOL_SHOTCURVE_BYTES = 0x3EC000;
constexpr size_t SHOT_TRACE_POOL_LEGACY_SECTORS = 2;
constexpr size_t SHOT_TRACE_POOL_RESERVED_SECTORS =
    SHOT_TRACE_POOL_RESERVED_BYTES / SHOT_TRACE_POOL_SECTOR_BYTES;  // 335
constexpr size_t SHOT_TRACE_POOL_SECTORS = SHOT_TRACE_POOL_RESERVED_SECTORS +
    SHOT_TRACE_POOL_SHOTCURVE_BYTES / SHOT_TRACE_POOL_SECTOR_BYTES -
    SHOT_TRACE_POOL_LEGACY_SECTORS;  // 1337
constexpr uint16_t SHOT_TRACE_POOL_FORMAT_VERSION = 1;
// A 16 KiB trace plus a worst-case curve and section headers fit.
constexpr size_t SHOT_TRACE_POOL_MAX_RECORD_BYTES = 24576;
// Records are zero-padded to 16 bytes, the index's length unit.
constexpr size_t SHOT_TRACE_POOL_RECORD_ALIGN = 16;
constexpr size_t SHOT_TRACE_POOL_MAX_SECTIONS = 4;
// Records indexed in RAM; the oldest is dropped when full. A record is only
// reachable through a shot log summary, so twice the summary capacity is
// ample; checked against SHOT_LOG_CAPACITY itself so that raising it
// re-checks the bound. A power of two keeps serial % capacity continuous
// across the 32-bit serial wrap.
constexpr size_t SHOT_TRACE_POOL_INDEX_CAPACITY = 2048;
static_assert(SHOT_TRACE_POOL_INDEX_CAPACITY >= 2U * SHOT_LOG_CAPACITY &&
                  (SHOT_TRACE_POOL_INDEX_CAPACITY & (SHOT_TRACE_POOL_INDEX_CAPACITY - 1U)) == 0,
              "The pool index keeps twice the summaries, a power of two");

struct ShotTracePoolSectorHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;
  uint32_t sequence;     // +1 per sector the ring starts, never reused
  uint32_t floor;        // sectors with a lower sequence are cleared
  uint32_t crc;          // CRC-32 of the 16 bytes above
  uint32_t firstRecord;  // programmed later: offset | ~offset << 16
};

struct ShotTracePoolRecordHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t endReason;  // raw EndReason
  uint16_t totalBytes;  // header and sections, padded to a multiple of 16
  uint32_t shotId;
  uint32_t bootId;
  uint32_t endedAtMs;
  uint32_t firmwarePacked;
  uint32_t crc;     // CRC-32 of the header up to here and of the sections
  uint32_t commit;  // programmed last
};

struct ShotTracePoolSectionHeader {
  uint8_t tag;  // ShotTracePoolSection; 0 ends the list (zero padding follows)
  uint8_t flags;
  uint16_t bytes;  // payload, followed by zero padding to 4 bytes
};
static_assert(sizeof(ShotTracePoolSectorHeader) == 24 &&
                  sizeof(ShotTracePoolRecordHeader) == 32 &&
                  sizeof(ShotTracePoolSectionHeader) == 4,
              "Shot trace pool wire layout");
static_assert(SHOT_TRACE_POOL_MAX_RECORD_BYTES <= UINT16_MAX,
              "Record lengths fit the header");

enum class ShotTracePoolSection : uint8_t { END = 0, CURVE = 1, TRACE = 2 };

// One section to append: `data`, or `read` when the bytes may be replaced
// meanwhile (it copies a stable range and fails once they changed, which
// abandons the append). Both stay the caller's until the append completes.
struct ShotTracePoolSource {
  ShotTracePoolSection tag;
  uint8_t flags;
  uint16_t bytes;
  const void *data;
  bool (*read)(void *context, size_t offset, uint8_t *out, size_t bytes);
  void *context;
};

struct ShotTracePoolIdentity {
  uint32_t shotId, bootId, endedAtMs, firmwarePacked;
  uint8_t endReason;
};

// 8-byte index entry: bits 0-23 of the shot id and an 8-bit tag of its boot
// and end time, the record's logical address in words (21 bits) and its
// length in 16-byte units (11 bits). Flash keeps the full identity.
struct ShotTracePoolEntry {
  uint32_t idTag;
  uint32_t place;
  uint32_t address() const { return (place & 0x1FFFFFU) * 4U; }
  uint32_t bytes() const { return (place >> 21) * 16U; }
};
static_assert(SHOT_TRACE_POOL_SECTORS * SHOT_TRACE_POOL_SECTOR_BYTES / 4U <= 0x200000U &&
                  SHOT_TRACE_POOL_MAX_RECORD_BYTES / 16U < 0x800U,
              "Index entries address the whole space");

inline uint32_t shotTracePoolIdTag(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs) {
  uint32_t mix = bootId * 0x9E3779B1U ^ endedAtMs;
  mix ^= mix >> 16;
  mix ^= mix >> 8;
  return (shotId & 0xFFFFFFU) | (mix & 0xFFU) << 24;
}

// Summary linkage: id reuse after a clear or factory reset never matches a
// record of another boot or end time (exactly checked again on open).
inline bool shotTracePoolMatches(const ShotTracePoolEntry &entry, uint32_t shotId,
                                 uint32_t bootId, uint32_t endedAtMs) {
  return entry.idTag == shotTracePoolIdTag(shotId, bootId, endedAtMs);
}

// Static PSRAM storage of one pool: the FIFO index of the newest records
// (oldest first) and one bit per logical sector holding no pool data (never
// written, cleared or a hole).
struct ShotTracePoolIndex {
  ShotTracePoolEntry entries[SHOT_TRACE_POOL_INDEX_CAPACITY];
  uint8_t unused[(SHOT_TRACE_POOL_SECTORS + 7U) / 8U];
};

enum class ShotTracePoolStatus : uint8_t { OK, NOT_READY, NOT_FOUND, GONE, CORRUPT, IO_ERROR };
// SOURCE: a section's bytes changed while it was being written.
enum class ShotTracePoolError : uint8_t { NONE, READ, ERASE, PROGRAM, LOCK, TOO_LARGE, SOURCE };

// An opened, CRC-verified record; reads revalidate it against recycling.
struct ShotTracePoolRef {
  uint32_t serial, address, bytes;
  ShotTracePoolRecordHeader header;
};

// poolBytes = liveBytes + reclaimableBytes + unindexedBytes + unusedBytes.
// Indexed records are live or reclaimable (deleted or orphaned summaries);
// unindexed space is the rest of every used sector: records dropped from a
// full index, sector headers, torn data and the head's unwritten tail.
struct ShotTracePoolStats {
  uint32_t poolBytes, liveBytes, reclaimableBytes, unindexedBytes, unusedBytes;
  uint32_t records, meanRecordBytes;  // Indexed records.
  uint32_t droppedRecords;  // Dropped from the full index since the last recovery or clear.
  ShotTracePoolError lastError;
};

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
// Both partitions back to back by physical sector: reserved 0-334, then
// shotcurve 335-1338 (its legacy epoch sectors are 335 and 336). After
// setFailAfter(n), n operations succeed and the next one loses power: a
// program keeps its first `torn` bytes plus the high nibble of the next, an
// erase blanks only its last `torn` bytes; nothing else lands until reset.
// With kRandomTear (seeded) a program instead leaves each target byte
// unprogrammed, programmed or with a random subset of its bits cleared, and
// an erase leaves each byte unerased, erased or with a random subset of its
// bits set. A transient failure tears that one operation and goes on.
// A read hook runs once, before the next read, as a concurrent writer would;
// an erase spy sees every erase before it happens.
class ShotTracePoolHostFlash {
 public:
  static constexpr size_t kSectors = SHOT_TRACE_POOL_RESERVED_SECTORS +
                                     SHOT_TRACE_POOL_SHOTCURVE_BYTES / SHOT_TRACE_POOL_SECTOR_BYTES;
  static void reset() {
    memset(flash_, 0xFF, sizeof(flash_));
    memset(touched_, 0, sizeof(touched_));
    memset(erases_, 0, sizeof(erases_));
    failAfter_ = -1;
    dead_ = false;
    readsFail_ = false;
    operations_ = 0;
  }
  static constexpr size_t kRandomTear = SIZE_MAX;
  static void setFailAfter(int operations, size_t torn = 0, bool transient = false,
                           uint32_t seed = 1) {
    failAfter_ = operations;
    torn_ = torn;
    transient_ = transient;
    dead_ = false;
    random_ = seed | 1U;
  }
  static size_t operations() { return operations_; }
  static uint32_t touched(size_t physical) { return touched_[physical]; }  // Any operation.
  static uint32_t erases(size_t physical) { return erases_[physical]; }    // Interrupted too.
  static uint8_t *sector(size_t physical) { return flash_ + physical * SHOT_TRACE_POOL_SECTOR_BYTES; }
  static uint8_t *image() { return flash_; }
  static constexpr size_t imageBytes() { return sizeof(flash_); }

  static void setReadHook(void (*hook)()) { readHook_ = hook; }
  static void setEraseSpy(void (*spy)(size_t physical)) { eraseSpy_ = spy; }
  static void setReadsFail(bool fail) { readsFail_ = fail; }  // Every read errors.
  static bool read(size_t physical, size_t offset, void *out, size_t bytes) {
    if (void (*hook)() = readHook_) {
      readHook_ = nullptr;
      hook();
    }
    if (readsFail_) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    memcpy(out, sector(physical) + offset, bytes);
    return true;
  }
  static bool program(size_t physical, size_t offset, const uint8_t *in, size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t *at = sector(physical) + offset;
    size_t landed = bytes;
    const bool ok = operate(physical);
    if (!ok && dead_) return false;
    if (!ok) {
      landed = torn_ < bytes ? torn_ : bytes;
      if (torn_ == kRandomTear) {
        landed = 0;
        for (size_t i = 0; i < bytes; ++i) at[i] &= static_cast<uint8_t>(in[i] | randomMask());
      } else if (landed < bytes) {
        at[landed] &= static_cast<uint8_t>(in[landed] | 0x0FU);
      }
      failed();
    }
    for (size_t i = 0; i < landed; ++i) at[i] &= in[i];
    return ok;
  }
  static bool erase(size_t physical) {
    if (eraseSpy_ != nullptr) eraseSpy_(physical);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t blanked = SHOT_TRACE_POOL_SECTOR_BYTES;
    const bool ok = operate(physical);
    if (!ok && dead_) return false;
    ++erases_[physical];
    if (!ok) {
      blanked = torn_ < blanked ? torn_ : blanked;
      if (torn_ == kRandomTear) {
        blanked = 0;
        for (size_t i = 0; i < SHOT_TRACE_POOL_SECTOR_BYTES; ++i)
          sector(physical)[i] |= static_cast<uint8_t>(~randomMask());
      }
      failed();
    }
    memset(sector(physical) + SHOT_TRACE_POOL_SECTOR_BYTES - blanked, 0xFF, blanked);
    return ok;
  }

 private:
  static bool operate(size_t physical) {
    ++operations_;
    ++touched_[physical];
    if (failAfter_ == 0) return false;
    if (failAfter_ > 0) --failAfter_;
    return true;
  }
  static void failed() {
    dead_ = !transient_;
    if (transient_) failAfter_ = -1;
  }
  // 0xFF (bits kept), 0x00 (all changed) or random bits, a third each.
  static uint8_t randomMask() {
    random_ ^= random_ << 13;
    random_ ^= random_ >> 17;
    random_ ^= random_ << 5;
    const uint32_t mode = random_ % 3U;
    return mode == 0 ? 0xFF : mode == 1 ? 0x00 : static_cast<uint8_t>(random_ >> 8);
  }
  inline static uint8_t flash_[kSectors * SHOT_TRACE_POOL_SECTOR_BYTES] = {};
  inline static uint32_t touched_[kSectors] = {}, erases_[kSectors] = {};
  inline static int failAfter_ = -1;
  inline static size_t torn_ = 0, operations_ = 0;
  inline static bool dead_ = false, transient_ = false, readsFail_ = false;
  inline static std::mutex mutex_;
  inline static void (*readHook_)() = nullptr;
  inline static void (*eraseSpy_)(size_t) = nullptr;
  inline static uint32_t random_ = 1;
};
#endif

class ShotTracePool {
 public:
  using LiveFn = bool (*)(void *context, const ShotTracePoolEntry &entry);

  // `sectors` of the logical space form the ring: the firmware's is the
  // whole space (the legacy curve store is retired).
  ShotTracePool(ShotTracePoolIndex &index, size_t sectors)
      : index_(index), end_(sectors <= SHOT_TRACE_POOL_SECTORS ? sectors : SHOT_TRACE_POOL_SECTORS) {}
  ShotTracePool(const ShotTracePool &) = delete;
  ShotTracePool &operator=(const ShotTracePool &) = delete;

  size_t logicalEnd() const { return end(); }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  // Tests only: grows the ring (a recover() must follow; a record that
  // wrapped from the old last sector to sector 0 is dropped by that walk).
  bool setLogicalEnd(size_t sectors) {
    if (op_ != Op::IDLE || sectors < end() || sectors > SHOT_TRACE_POOL_SECTORS) return false;
    TaskLockGuard lock(mux_);
    ready_ = false;
    end_.store(sectors, std::memory_order_relaxed);
    return true;
  }
#endif

  // Boot scan, persistence worker only: every sector header of the ring, the
  // record headers of the valid sectors, and one blank check of the head.
  bool recover() {
    findPartitions();
    dropIndex(false);
    op_ = Op::IDLE;
    lastError_.store(ShotTracePoolError::NONE, std::memory_order_relaxed);
    // Pass 1: the head is the valid sector with the highest sequence.
    ShotTracePoolSectorHeader h;
    uint32_t headSeq = 0;
    size_t head = 0;
    floor_ = 1;
    for (size_t s = 0; s < end(); ++s) {
      if (!read(s, 0, &h, sizeof(h))) return fail(ShotTracePoolError::READ);
      setUnused(s, !validSector(h));
      if (validSector(h) && h.sequence > headSeq) {
        headSeq = h.sequence;
        head = s;
        floor_ = h.floor;
      }
      pace(s);
    }
    nextSequence_ = headSeq + 1U;
    sector_ = headSeq == 0 ? end() - 1U : head;
    fresh_ = true;
    if (headSeq != 0 && !walk(head, headSeq)) return fail(ShotTracePoolError::READ);
    TaskLockGuard lock(mux_);
    ready_ = true;
    return true;
  }

  // Stages one record; appendStep() then performs one flash operation per
  // call: erase, sector header, first-record pointer, a chunk of at most
  // FLASH_IO_CHUNK_BYTES, and the commit word last.
  bool beginAppend(const ShotTracePoolIdentity &id, const ShotTracePoolSource *sources,
                   size_t count) {
    if (!ready() || op_ != Op::IDLE || count > SHOT_TRACE_POOL_MAX_SECTIONS) return false;
    size_t total = sizeof(ShotTracePoolRecordHeader);
    for (size_t i = 0; i < count; ++i) {
      if (sources[i].tag == ShotTracePoolSection::END ||
          (sources[i].data == nullptr && sources[i].read == nullptr && sources[i].bytes != 0))
        return false;
      total += sizeof(ShotTracePoolSectionHeader) + align4(sources[i].bytes);
    }
    if (total > SHOT_TRACE_POOL_MAX_RECORD_BYTES) {
      lastError_.store(ShotTracePoolError::TOO_LARGE, std::memory_order_relaxed);
      return false;
    }
    total = (total + SHOT_TRACE_POOL_RECORD_ALIGN - 1U) & ~(SHOT_TRACE_POOL_RECORD_ALIGN - 1U);
    memcpy(sources_, sources, count * sizeof(*sources));
    sourceCount_ = count;
    header_ = {kRecordMagic, static_cast<uint8_t>(SHOT_TRACE_POOL_FORMAT_VERSION),
               id.endReason, static_cast<uint16_t>(total), id.shotId, id.bootId,
               id.endedAtMs, id.firmwarePacked, 0, UINT32_MAX};
    uint8_t chunk[256];
    uint32_t crc = crc32Update(0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&header_),
                               offsetof(ShotTracePoolRecordHeader, crc));
    for (size_t at = sizeof(header_); at < total; at += sizeof(chunk)) {
      const size_t piece = total - at < sizeof(chunk) ? total - at : sizeof(chunk);
      if (!gather(at, chunk, piece)) return fail(ShotTracePoolError::SOURCE);
      crc = crc32Update(crc, chunk, piece);
    }
    header_.crc = ~crc;
    // A record header never crosses a sector; a fresh cursor (after a torn
    // append or a failure) always starts the next sector.
    needErase_ = fresh_ || offset_ > kLastStart;
    firstPending_ = !needErase_ && !cursorFirstSet_;
    needHeader_ = false;
    start_ = static_cast<uint32_t>(needErase_ ? next(sector_) * kSectorBytes + kDataStart
                                              : sector_ * kSectorBytes + offset_);
    written_ = 0;
    fresh_ = true;  // Until the commit lands, the cursor is not reusable.
    op_ = Op::APPEND;
    return true;
  }

  FlashStoreStepResult appendStep(uint32_t timeoutMs = FLASH_IO_LOCK_TIMEOUT_MS) {
    if (op_ != Op::APPEND) return FlashStoreStepResult::FAILED;
    if (!tryLockFlashIo(timeoutMs)) {
      op_ = Op::IDLE;
      return fail(ShotTracePoolError::LOCK), FlashStoreStepResult::FAILED;
    }
    bool ok;
    ShotTracePoolError error = ShotTracePoolError::PROGRAM;
    if (needErase_) {
      // Only the start sector of the record carries its first-record pointer.
      const size_t target = next(sector_);
      error = ShotTracePoolError::ERASE;
      ok = startSector(target, start_ == target * kSectorBytes + kDataStart ? kDataStart : 0);
    } else if (needHeader_) {
      ok = programHeader();
    } else if (firstPending_) {
      const uint32_t first = encodeFirst(start_ % kSectorBytes);
      ok = program(start_ / kSectorBytes, offsetof(ShotTracePoolSectorHeader, firstRecord),
                   &first, sizeof(first));
      firstPending_ = false;
    } else if (written_ < header_.totalBytes) {
      size_t piece = header_.totalBytes - written_;
      if (piece > kSectorBytes - offset_) piece = kSectorBytes - offset_;
      if (piece > FLASH_IO_CHUNK_BYTES) piece = FLASH_IO_CHUNK_BYTES;
      if (!gather(written_, flashIoScratchBytes(), piece)) error = ShotTracePoolError::SOURCE;
      ok = error != ShotTracePoolError::SOURCE &&
           program(sector_, offset_, flashIoScratchBytes(), piece);
      written_ += piece;
      offset_ += piece;
      needErase_ = written_ < header_.totalBytes && offset_ == kSectorBytes;
    } else {
      const uint32_t commit = kCommitted;
      const size_t at = start_ % kSectorBytes + offsetof(ShotTracePoolRecordHeader, commit);
      ok = program(start_ / kSectorBytes, at, &commit, sizeof(commit));
      // A commit reported as failed may have landed: then it is committed,
      // and indexing it keeps a retry from writing the shot twice.
      uint32_t landed = 0;
      if (!ok) ok = readLocked(start_ / kSectorBytes, at, &landed, sizeof(landed)) && landed == kCommitted;
      if (ok) {
        push(header_, start_);
        op_ = Op::IDLE;
        fresh_ = false;
        // A sector the record only continued into has no first record yet.
        cursorFirstSet_ = sector_ == start_ / kSectorBytes;
      }
    }
    unlockFlashIo();
    feedFlashIoWatchdog();
    if (!ok) {
      op_ = Op::IDLE;
      return fail(error), FlashStoreStepResult::FAILED;
    }
    return op_ == Op::IDLE ? FlashStoreStepResult::COMPLETE : FlashStoreStepResult::MORE;
  }

  bool appending() const { return op_ != Op::IDLE; }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  // Tests only: a whole append, and abandoning one (an unfinished append
  // leaves its bytes behind; the next starts a sector).
  bool append(const ShotTracePoolIdentity &id, const ShotTracePoolSource *sources, size_t count) {
    if (!beginAppend(id, sources, count)) return false;
    FlashStoreStepResult result;
    do { result = appendStep(); } while (result == FlashStoreStepResult::MORE);
    return result == FlashStoreStepResult::COMPLETE;
  }
  void cancelAppend() { op_ = Op::IDLE; }
#endif

  // Clear all and factory reset: one new sector whose floor is its own
  // sequence hides every older sector (two flash operations, no full erase).
  bool clear() {
    if (!ready() && !recover()) return false;
    op_ = Op::IDLE;
    dropIndex(true);
    for (size_t s = 0; s < end(); ++s) setUnused(s, true);
    floor_ = nextSequence_;
    fresh_ = true;
    if (!tryLockFlashIo()) return fail(ShotTracePoolError::LOCK);
    const bool erased = startSector(next(sector_), 0);
    const bool ok = erased && programHeader();
    unlockFlashIo();
    if (!ok) return fail(erased ? ShotTracePoolError::PROGRAM : ShotTracePoolError::ERASE);
    fresh_ = false;
    cursorFirstSet_ = false;
    return true;
  }

  bool ready() const {
    TaskLockGuard lock(mux_);
    return ready_;
  }
  size_t count() const {
    TaskLockGuard lock(mux_);
    return count_;
  }
  // n-th oldest entry, for diagnostics and tests.
  bool entryAt(size_t n, ShotTracePoolEntry &out) const {
    TaskLockGuard lock(mux_);
    if (n >= count_) return false;
    out = slot(firstSerial_ + static_cast<uint32_t>(n));
    return true;
  }
  // The newest record of the summary, by its full identity on flash (one
  // header read per candidate entry). Reads flash: hold no store lock.
  ShotTracePoolStatus identify(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs,
                               ShotTracePoolRef &ref) {
    if (!ready()) return ShotTracePoolStatus::NOT_READY;
    const uint32_t idTag = shotTracePoolIdTag(shotId, bootId, endedAtMs);
    ShotTracePoolEntry entry;
    // Older entries are tried too: a tag collision after an id reuse without
    // a pool clear (a downgrade) must not hide the matching record.
    for (bool newest = true; find(idTag, newest, entry, ref.serial); newest = false) {
      ref.address = entry.address();
      ref.bytes = sizeof(ref.header);
      const ShotTracePoolStatus status = read(ref, 0, &ref.header, sizeof(ref.header));
      if (status == ShotTracePoolStatus::IO_ERROR) return status;
      const ShotTracePoolRecordHeader &h = ref.header;
      if (status == ShotTracePoolStatus::OK && validRecord(h) && h.commit == kCommitted &&
          h.shotId == shotId && h.bootId == bootId && h.endedAtMs == endedAtMs) {
        ref.bytes = h.totalBytes;
        return ShotTracePoolStatus::OK;
      }
    }
    return ShotTracePoolStatus::NOT_FOUND;
  }
  bool contains(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs) {
    ShotTracePoolRef ref;
    return identify(shotId, bootId, endedAtMs, ref) == ShotTracePoolStatus::OK;
  }

  // identify(), then the CRC through the caller's buffer (any size >= 4).
  ShotTracePoolStatus open(uint32_t shotId, uint32_t bootId, uint32_t endedAtMs,
                           ShotTracePoolRef &ref, uint8_t *buffer, size_t bufferBytes) {
    if (buffer == nullptr || bufferBytes < 4) return ShotTracePoolStatus::IO_ERROR;
    ShotTracePoolStatus status = identify(shotId, bootId, endedAtMs, ref);
    if (status != ShotTracePoolStatus::OK) return status;
    const ShotTracePoolRecordHeader &h = ref.header;
    uint32_t crc = crc32Update(0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&h),
                               offsetof(ShotTracePoolRecordHeader, crc));
    for (size_t at = sizeof(h); at < ref.bytes; at += bufferBytes) {
      const size_t piece = ref.bytes - at < bufferBytes ? ref.bytes - at : bufferBytes;
      if ((status = read(ref, at, buffer, piece)) != ShotTracePoolStatus::OK) return status;
      crc = crc32Update(crc, buffer, piece);
    }
    if (~crc == h.crc) return ShotTracePoolStatus::OK;
    return alive(ref.serial) ? ShotTracePoolStatus::CORRUPT : ShotTracePoolStatus::GONE;
  }

  // Record bytes [offset, offset + bytes) of an opened record, in any chunk
  // size (an HTTP stream reads 1-4 KiB at a time).
  ShotTracePoolStatus read(const ShotTracePoolRef &ref, size_t offset, void *out, size_t bytes) {
    if (offset > ref.bytes || bytes > ref.bytes - offset) return ShotTracePoolStatus::IO_ERROR;
    if (!alive(ref.serial)) return ShotTracePoolStatus::GONE;
    size_t sector, at;
    locate(ref.address, offset, sector, at);
    if (!readSpan(sector, at, static_cast<uint8_t *>(out), bytes)) return ShotTracePoolStatus::IO_ERROR;
    // Eviction precedes every erase: alive after the read means intact.
    return alive(ref.serial) ? ShotTracePoolStatus::OK : ShotTracePoolStatus::GONE;
  }

  ShotTracePoolStatus findSection(const ShotTracePoolRef &ref, ShotTracePoolSection tag,
                                  size_t &offset, size_t &bytes) {
    for (size_t at = sizeof(ShotTracePoolRecordHeader);
         at + sizeof(ShotTracePoolSectionHeader) <= ref.bytes;) {
      ShotTracePoolSectionHeader section;
      const ShotTracePoolStatus status = read(ref, at, &section, sizeof(section));
      if (status != ShotTracePoolStatus::OK) return status;
      if (section.tag == static_cast<uint8_t>(ShotTracePoolSection::END)) break;
      at += sizeof(section);
      if (section.tag == static_cast<uint8_t>(tag)) {
        offset = at;
        bytes = section.bytes;
        return ShotTracePoolStatus::OK;
      }
      at += align4(section.bytes);
    }
    return ShotTracePoolStatus::NOT_FOUND;
  }

  // RAM only. `live` runs under the pool's leaf mutex and takes no lock of
  // its own; a caller may hold shotStoreMutex around the call.
  ShotTracePoolStats stats(LiveFn live, void *context) const {
    ShotTracePoolStats out = {};
    out.poolBytes = static_cast<uint32_t>(end() * kSectorBytes);
    TaskLockGuard lock(mux_);
    for (size_t s = 0; s < end(); ++s)
      if ((index_.unused[s / 8U] >> (s % 8U)) & 1U) out.unusedBytes += kSectorBytes;
    for (size_t n = 0; n < count_; ++n) {
      const ShotTracePoolEntry &entry = slot(firstSerial_ + static_cast<uint32_t>(n));
      (live != nullptr && live(context, entry) ? out.liveBytes : out.reclaimableBytes) += entry.bytes();
    }
    out.unindexedBytes = out.poolBytes - out.unusedBytes - out.liveBytes - out.reclaimableBytes;
    out.records = static_cast<uint32_t>(count_);
    out.droppedRecords = dropped_;
    if (count_ != 0) out.meanRecordBytes = (out.liveBytes + out.reclaimableBytes) / out.records;
    out.lastError = lastError();
    return out;
  }
  // Header of the oldest reachable record (its boot and end time: "history
  // since"). Reads flash: hold no store lock.
  ShotTracePoolStatus oldest(ShotTracePoolRecordHeader &out) {
    ShotTracePoolRef ref = {};
    {
      TaskLockGuard lock(mux_);
      if (!ready_) return ShotTracePoolStatus::NOT_READY;
      if (count_ == 0) return ShotTracePoolStatus::NOT_FOUND;
      ref.serial = firstSerial_;
      ref.address = slot(firstSerial_).address();
    }
    ref.bytes = sizeof(out);
    return read(ref, 0, &out, sizeof(out));
  }
  ShotTracePoolError lastError() const { return lastError_.load(std::memory_order_relaxed); }

 private:
  enum class Op : uint8_t { IDLE, APPEND };
  static constexpr uint32_t kSectorMagic = 0x53505453U;  // "STPS"
  static constexpr uint32_t kRecordMagic = 0x52505453U;  // "STPR"
  static constexpr uint32_t kCommitted = 0x54494D43U;    // "CMIT"
  static constexpr size_t kSectorBytes = SHOT_TRACE_POOL_SECTOR_BYTES;
  static constexpr size_t kDataStart = sizeof(ShotTracePoolSectorHeader);
  static constexpr size_t kLastStart = kSectorBytes - sizeof(ShotTracePoolRecordHeader);

  static size_t align4(size_t bytes) { return (bytes + 3U) & ~size_t(3U); }
  static uint32_t encodeFirst(size_t offset) {
    return static_cast<uint32_t>(offset) | (~static_cast<uint32_t>(offset) & 0xFFFFU) << 16;
  }
  // The first-record field is blank, valid, or torn (neither).
  static bool validFirst(uint32_t value) {
    const uint32_t offset = value & 0xFFFFU;
    return value == encodeFirst(offset) && offset >= kDataStart && offset <= kLastStart;
  }
  static uint32_t sectorCrc(const ShotTracePoolSectorHeader &h) {
    return ~crc32Update(0xFFFFFFFFU, reinterpret_cast<const uint8_t *>(&h),
                        offsetof(ShotTracePoolSectorHeader, crc));
  }
  static bool validSector(const ShotTracePoolSectorHeader &h) {
    return h.magic == kSectorMagic && h.version == SHOT_TRACE_POOL_FORMAT_VERSION &&
           h.headerBytes == kDataStart && h.sequence != 0 && h.floor != 0 &&
           h.floor <= h.sequence && h.crc == sectorCrc(h);
  }
  static bool validRecord(const ShotTracePoolRecordHeader &h) {
    return h.magic == kRecordMagic && h.version == SHOT_TRACE_POOL_FORMAT_VERSION &&
           h.totalBytes >= sizeof(h) && h.totalBytes <= SHOT_TRACE_POOL_MAX_RECORD_BYTES &&
           h.totalBytes % SHOT_TRACE_POOL_RECORD_ALIGN == 0;
  }
  static bool blank(const void *bytes, size_t count) {
    const uint8_t *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < count; ++i)
      if (p[i] != 0xFFU) return false;
    return true;
  }

  size_t end() const { return end_.load(std::memory_order_relaxed); }
  size_t next(size_t sector) const { return sector + 1U >= end() ? 0 : sector + 1U; }
  // Recovery yields and feeds the watchdog every 64 sectors (~0.2 s in all).
  static void pace(size_t sector) {
    if (sector % 64U != 63U) return;
    yieldFlashIo();
    feedFlashIoWatchdog();
  }
  // Sector and offset of record byte `pos`; an end position stays in the
  // sector it fills.
  void locate(uint32_t address, size_t pos, size_t &sector, size_t &offset) const {
    sector = address / kSectorBytes;
    offset = address % kSectorBytes;
    while (pos > kSectorBytes - offset) {
      pos -= kSectorBytes - offset;
      sector = next(sector);
      offset = kDataStart;
    }
    offset += pos;
  }
  void setUnused(size_t s, bool value) {
    const uint8_t bit = static_cast<uint8_t>(1U << (s % 8U));
    TaskLockGuard lock(mux_);
    index_.unused[s / 8U] = static_cast<uint8_t>(value ? index_.unused[s / 8U] | bit
                                                       : index_.unused[s / 8U] & ~bit);
  }

  // The virtual record: header (commit blank), then each section header,
  // payload and zero padding, then zero padding to the record length.
  bool gather(size_t pos, uint8_t *out, size_t bytes) const {
    memset(out, 0, bytes);
    size_t at = 0;
    copySpan(out, pos, bytes, at, &header_, sizeof(header_), nullptr);
    at += sizeof(header_);
    for (size_t i = 0; i < sourceCount_; ++i) {
      const ShotTracePoolSource &source = sources_[i];
      const ShotTracePoolSectionHeader section = {static_cast<uint8_t>(source.tag),
                                                  source.flags, source.bytes};
      copySpan(out, pos, bytes, at, &section, sizeof(section), nullptr);
      at += sizeof(section);
      if (!copySpan(out, pos, bytes, at, source.data, source.bytes, &source)) return false;
      at += align4(source.bytes);
    }
    return true;
  }
  // The part of [at, at + length) inside [pos, pos + bytes), from memory or
  // through the source's reader.
  static bool copySpan(uint8_t *out, size_t pos, size_t bytes, size_t at, const void *data,
                       size_t length, const ShotTracePoolSource *source) {
    const size_t from = pos > at ? pos : at;
    const size_t to = pos + bytes < at + length ? pos + bytes : at + length;
    if (from >= to) return true;
    if (source != nullptr && source->read != nullptr)
      return source->read(source->context, from - at, out + (from - pos), to - from);
    memcpy(out + (from - pos), static_cast<const uint8_t *>(data) + (from - at), to - from);
    return true;
  }

  // Index ring addressed by serial; live serials [firstSerial_, +count_).
  // Dropping advances the first serial, so every open reference goes GONE.
  const ShotTracePoolEntry &slot(uint32_t serial) const {
    return index_.entries[serial % SHOT_TRACE_POOL_INDEX_CAPACITY];
  }
  void dropIndex(bool keepReady) {
    TaskLockGuard lock(mux_);
    ready_ = ready_ && keepReady;
    firstSerial_ += static_cast<uint32_t>(count_);
    count_ = 0;
    dropped_ = 0;
  }
  bool alive(uint32_t serial) const {
    TaskLockGuard lock(mux_);
    return ready_ && serial - firstSerial_ < count_;
  }
  void push(const ShotTracePoolRecordHeader &r, uint32_t address) {
    TaskLockGuard lock(mux_);
    if (count_ == SHOT_TRACE_POOL_INDEX_CAPACITY) {  // Full: drop the oldest.
      ++firstSerial_;
      --count_;
      ++dropped_;
    }
    index_.entries[(firstSerial_ + count_) % SHOT_TRACE_POOL_INDEX_CAPACITY] = {
        shotTracePoolIdTag(r.shotId, r.bootId, r.endedAtMs),
        address / 4U | static_cast<uint32_t>(r.totalBytes) / 16U << 21};
    ++count_;
  }
  // Records start in ring order, so those of the sector about to be erased
  // are the oldest entries (older ones may already have left a full index).
  void evict(size_t sector) {
    TaskLockGuard lock(mux_);
    while (count_ != 0 && slot(firstSerial_).address() / kSectorBytes == sector) {
      ++firstSerial_;
      --count_;
    }
  }
  // Newest entry with the tag, or (newest == false) the next one older than
  // `serial`; the entry is copied before the mutex is released.
  bool find(uint32_t idTag, bool newest, ShotTracePoolEntry &entry, uint32_t &serial) const {
    TaskLockGuard lock(mux_);
    size_t n = newest ? count_ : serial - firstSerial_;  // Entries below it.
    for (n = ready_ && n <= count_ ? n : 0; n != 0;) {
      serial = firstSerial_ + static_cast<uint32_t>(--n);
      entry = slot(serial);
      if (entry.idTag == idTag) return true;
    }
    return false;
  }

  // Evicts the records that start in `target`, erases it and makes it the
  // cursor sector; the cursor stays put until the erase succeeded, so a
  // retry erases (and evicts) the same sector. Caller holds the flash lock.
  bool startSector(size_t target, size_t first) {
    evict(target);
    needErase_ = false;
    if (!erase(target)) return false;
    sector_ = target;
    offset_ = kDataStart;
    firstOffset_ = first;
    needHeader_ = true;
    return true;
  }
  // The sequence is consumed even when the program fails: a header that
  // reported an error but landed must not share its sequence with the next.
  bool programHeader() {
    ShotTracePoolSectorHeader h = {kSectorMagic, SHOT_TRACE_POOL_FORMAT_VERSION,
                                   static_cast<uint16_t>(kDataStart), nextSequence_++, floor_, 0,
                                   firstOffset_ != 0 ? encodeFirst(firstOffset_) : UINT32_MAX};
    h.crc = sectorCrc(h);
    needHeader_ = false;
    const bool ok = program(sector_, 0, &h, sizeof(h));
    if (ok) setUnused(sector_, false);
    return ok;
  }
  bool fail(ShotTracePoolError error) {
    lastError_.store(error, std::memory_order_relaxed);
    return false;
  }

  // Pass 2 of recover(): walks the ring oldest first from head + 1. A sector
  // joins the run only with a valid header, a sequence above every earlier
  // one and at least the floor; anything else is a hole. Records chain by
  // length through consecutive sequences and resynchronise at a sector's
  // first-record pointer after a hole or an uncommitted record. A record
  // that spans sectors is indexed only once its last sector joined.
  bool walk(size_t head, uint32_t headSeq) {
    ShotTracePoolSectorHeader h;
    ShotTracePoolRecordHeader r, spanning = {};
    uint32_t prevSeq = 0, spanningAddress = 0;
    bool chain = false, unconfirmed = false;
    size_t chainSector = 0, chainOffset = 0, endSector = 0, appendAt = 0;
    for (size_t n = 1; n <= end(); ++n) {
      pace(n);
      const size_t s = (head + n) % end();
      if (!read(s, 0, &h, sizeof(h))) return false;
      const bool valid = validSector(h) && h.sequence >= floor_ && h.sequence > prevSeq &&
                         h.sequence <= headSeq;
      if (!valid || prevSeq == 0 || h.sequence != prevSeq + 1U) unconfirmed = chain = false;
      if (!valid) {
        setUnused(s, true);
        continue;
      }
      prevSeq = h.sequence;
      // The first-record pointer outranks a length read from an unverified
      // header (a stale record in a torn-erased sector): on disagreement the
      // spanning record is dropped and the walk resynchronises.
      const bool first = validFirst(h.firstRecord);
      if (chain && first && (chainSector != s || chainOffset != (h.firstRecord & 0xFFFFU)))
        unconfirmed = chain = false;
      if (unconfirmed && s == endSector) {
        push(spanning, spanningAddress);
        unconfirmed = false;
      }
      if (!chain) {
        if (!first) continue;
        chainOffset = h.firstRecord & 0xFFFFU;
      } else if (chainSector != s) {
        continue;  // Covered by a record that spans it.
      }
      chain = true;
      while (chainOffset <= kLastStart) {
        if (!read(s, chainOffset, &r, sizeof(r))) return false;
        // Appending may follow a committed record only, and only where the
        // first-record pointer is blank or valid (programmable).
        if (blank(&r, sizeof(r)) && s == head && chainOffset > kDataStart &&
            (h.firstRecord == UINT32_MAX || first)) appendAt = chainOffset;
        if (!validRecord(r) || r.commit != kCommitted) {
          chain = false;  // Blank, torn or uncommitted: nothing follows here.
          break;
        }
        const uint32_t address = static_cast<uint32_t>(s * kSectorBytes + chainOffset);
        locate(address, r.totalBytes, endSector, chainOffset);
        if (endSector != s) {
          spanning = r;
          spanningAddress = address;
          unconfirmed = true;
          break;
        }
        push(r, address);
      }
      chainSector = endSector;
      if (chainOffset > kLastStart) {
        chainSector = next(endSector);
        chainOffset = kDataStart;
      }
    }
    // A record spanning past the head never joined: it is not indexed.
    // Append after the newest record only when the rest of the head sector
    // is blank; otherwise (a torn append, an interrupted erase, a head with
    // no committed record) start a new sector. `h` is the head's header.
    if (appendAt != 0 && headTailBlank(head, appendAt)) {
      offset_ = appendAt;
      fresh_ = false;
      cursorFirstSet_ = h.firstRecord != UINT32_MAX;
    }
    return true;
  }
  // A small chunk and its own frame: the walk runs deep on the 4 KiB
  // settings_persist stack (stack-depth gate).
  [[gnu::noinline]] bool headTailBlank(size_t head, size_t from) {
    uint8_t chunk[64];
    for (size_t at = from; at < kSectorBytes; at += sizeof(chunk)) {
      const size_t piece = kSectorBytes - at < sizeof(chunk) ? kSectorBytes - at : sizeof(chunk);
      if (!read(head, at, chunk, piece) || !blank(chunk, piece)) return false;
    }
    return true;
  }

  bool readSpan(size_t sector, size_t offset, uint8_t *out, size_t bytes) const {
    while (bytes != 0) {
      if (offset == kSectorBytes) {
        sector = next(sector);
        offset = kDataStart;
      }
      const size_t piece = bytes < kSectorBytes - offset ? bytes : kSectorBytes - offset;
      if (!read(sector, offset, out, piece)) return false;
      out += piece;
      bytes -= piece;
      offset += piece;
    }
    return true;
  }

  // Logical sector -> physical location: `reserved` first, then shotcurve
  // after its two legacy sectors. Every call stays inside one sector, so no
  // flash call spans the partition boundary.
  static size_t physical(size_t sector) {
    return sector < SHOT_TRACE_POOL_RESERVED_SECTORS ? sector
                                                     : sector + SHOT_TRACE_POOL_LEGACY_SECTORS;
  }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  bool read(size_t sector, size_t offset, void *out, size_t bytes) const {
    return ShotTracePoolHostFlash::read(physical(sector), offset, out, bytes);
  }
  bool readLocked(size_t sector, size_t offset, void *out, size_t bytes) const {
    return read(sector, offset, out, bytes);
  }
  bool program(size_t sector, size_t offset, const void *in, size_t bytes) {
    return ShotTracePoolHostFlash::program(physical(sector), offset,
                                           static_cast<const uint8_t *>(in), bytes);
  }
  bool erase(size_t sector) { return ShotTracePoolHostFlash::erase(physical(sector)); }
  void findPartitions() {}
#else
  // Looked up once, by recover() at the top of its caller's stack: the first
  // lookup loads the partition table, a deep path (stack-depth gate) that
  // must not run under the walk. Written before ready, never changed after.
  const esp_partition_t *parts_[2] = {};
  void findPartitions() {
    if (parts_[0] == nullptr) parts_[0] = find("reserved", SHOT_TRACE_POOL_RESERVED_BYTES);
    if (parts_[1] == nullptr) parts_[1] = find("shotcurve", SHOT_TRACE_POOL_SHOTCURVE_BYTES);
  }
  const esp_partition_t *partition(size_t sector, size_t &offset) const {
    const bool curve = sector >= SHOT_TRACE_POOL_RESERVED_SECTORS;
    offset = (curve ? physical(sector) - SHOT_TRACE_POOL_RESERVED_SECTORS : sector) * kSectorBytes;
    return parts_[curve];
  }
  static const esp_partition_t *find(const char *label, size_t bytes) {
    const auto *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40), label);
    // Encrypted flash cannot be re-programmed bit by bit: refuse it.
    return part != nullptr && !part->encrypted && part->size == bytes ? part : nullptr;
  }
  // Staged through the internal scratch like flashIoReadChunked, without its
  // per-chunk yield: recovery issues thousands of small reads and paces itself.
  bool read(size_t sector, size_t offset, void *out, size_t bytes) const {
    if (!tryLockFlashIo()) return false;
    const bool ok = readLocked(sector, offset, out, bytes);
    unlockFlashIo();
    return ok;
  }
  // Caller holds the flash I/O lock (the commit read-back): no nested lock,
  // so the lock-hold diagnostics see one hold.
  bool readLocked(size_t sector, size_t offset, void *out, size_t bytes) const {
    size_t base;
    const esp_partition_t *part = partition(sector, base);
    uint8_t *scratch = flashIoScratchBytes();
    bool ok = part != nullptr;
    for (size_t done = 0; ok && done < bytes;) {
      const size_t piece = bytes - done < FLASH_IO_CHUNK_BYTES ? bytes - done : FLASH_IO_CHUNK_BYTES;
      ok = esp_partition_read(part, base + offset + done, scratch, piece) == ESP_OK;
      if (ok) memcpy(static_cast<uint8_t *>(out) + done, scratch, piece);
      done += piece;
    }
    return ok;
  }
  // Caller holds the flash I/O lock; the source is staged in the internal
  // scratch unless it already is the scratch.
  bool program(size_t sector, size_t offset, const void *in, size_t bytes) {
    size_t base;
    const esp_partition_t *part = partition(sector, base);
    const void *staged = in == flashIoScratchBytes() ? in : copyToFlashIoScratch(in, bytes);
    return part != nullptr && staged != nullptr &&
           esp_partition_write(part, base + offset, staged, bytes) == ESP_OK;
  }
  bool erase(size_t sector) {
    size_t base;
    const esp_partition_t *part = partition(sector, base);
    return part != nullptr &&
           esp_partition_erase_range(part, base, kSectorBytes) == ESP_OK;
  }
#endif

  ShotTracePoolIndex &index_;
  mutable TaskMutex mux_;  // Leaf: index ring, dropped_, ready_.
  uint32_t firstSerial_ = 0;
  size_t count_ = 0;
  uint32_t dropped_ = 0;
  bool ready_ = false;
  std::atomic<size_t> end_;
  // Writer state, settings_persist worker only.
  uint32_t nextSequence_ = 1, floor_ = 1;
  size_t sector_ = 0, offset_ = kDataStart, firstOffset_ = 0;
  bool fresh_ = true, cursorFirstSet_ = false;
  Op op_ = Op::IDLE;
  ShotTracePoolRecordHeader header_ = {};
  ShotTracePoolSource sources_[SHOT_TRACE_POOL_MAX_SECTIONS] = {};
  size_t sourceCount_ = 0, written_ = 0;
  uint32_t start_ = 0;
  bool needErase_ = false, needHeader_ = false, firstPending_ = false;
  std::atomic<ShotTracePoolError> lastError_{ShotTracePoolError::NONE};
};

}  // namespace openbrewbyweight

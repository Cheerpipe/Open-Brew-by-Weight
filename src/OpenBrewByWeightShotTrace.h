#pragma once

// =============================================================================
// LAYER: Shot trace (per-shot firmware decision record, RAM capture)
// =============================================================================
// WHAT: For every brew cycle started with a scale, one compact binary record
//       that explains the shot: a start snapshot of what the shot log summary
//       lacks, the deduplicated pre-shot events, every decision from cycle
//       start to drip finalize or cancellation, and exact end counters.
//       Layout and decoding: docs/SHOT_TRACE.md.
//
// OWNERSHIP: the control loop opens, discards and completes the capture and
//       alone owns the state-change cache and the reading statistics.
//       Producers (scale worker, control loop, health worker, httpd, Linea
//       Micra WebSocket task and any addDebugEvent caller) append through one
//       leaf mutex that nests nothing, or through relaxed atomics; no path
//       allocates, formats or touches flash. Both buffers and the pre-shot
//       ring are static PSRAM BSS with a fixed, pointer-free layout. The
//       completed buffer is immutable until the next completion replaces it.
//
// BOUNDARY: Observes only: no control decisions, no machine access.

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightScaleProfiler.h"
#include "OpenBrewByWeightTaskMutex.h"
#include <OpenBrewByWeightVersion.h>

namespace openbrewbyweight {

constexpr uint32_t SHOT_TRACE_MAGIC = 0x43525453UL;  // "STRC": a completed record
// An in-flight capture, recoverable after a panic, watchdog or software reset.
constexpr uint32_t SHOT_TRACE_CAPTURING_MAGIC = 0x49525453UL;  // "STRI"
constexpr uint16_t SHOT_TRACE_FORMAT_VERSION = 1;
// Bumped when ScaleProfileEvent, DebugCode or ShotTraceKind meanings change.
constexpr uint16_t SHOT_TRACE_VOCABULARY_VERSION = 1;
// D2: one buffer holds the whole record, header to last event.
constexpr size_t SHOT_TRACE_BUFFER_BYTES = 16384;
constexpr size_t SHOT_TRACE_PRESHOT_CAPACITY = 32;
// Record head: bits 0-22 ms from the anchor (saturating), bit 23 namespace
// (set: DebugCode), bits 24-31 kind. DebugCode needs all eight kind bits.
constexpr uint32_t SHOT_TRACE_MS_MAX = 0x7FFFFFU;
constexpr uint32_t SHOT_TRACE_DEBUG_NAMESPACE = 0x800000U;
constexpr int16_t SHOT_TRACE_NO_WEIGHT = INT16_MIN;
constexpr uint16_t SHOT_TRACE_STATE_INITIAL = 0x8000;

// Namespace-0 kinds above the scale profile vocabulary (ScaleProfileEvent).
enum class ShotTraceKind : uint8_t {
  STOP_REQUESTED = 0xE0,  // a EndReason, b STOP_* bits; after the stop command
  STOP_BLOCKED,           // a 1 blocked / 0 released, b cause | BLOCKED_PENDING
  MACHINE_RUNNING,        // a 1 running / 0 stopped
  SLOW_WINDOW_END,        // a weight dg, b cut line dg; +1: a offset dg, b flags
  OVER_LINE,              // a weight dg, b cut line dg; +1: a offset dg, b confirmations
  FINAL_WEIGHT,           // a weight dg (NO_WEIGHT), b ActualWeightSource
  TRACE_DISCARDED,        // pre-shot only: a EndReason, b ms from its anchor
  CONTINUATION = 0xFF,    // extends the previous record (see SHOT_TRACE.md)
};
static_assert(static_cast<uint32_t>(ScaleProfileEvent::CUP_ANCHOR_REJECTED) <
                  static_cast<uint32_t>(ShotTraceKind::STOP_REQUESTED),
              "Scale profile events must stay below the trace-only kinds");
constexpr uint16_t SHOT_TRACE_STOP_WEB = 0x1;
constexpr uint16_t SHOT_TRACE_STOP_REFUSED = 0x2;
constexpr uint16_t SHOT_TRACE_BLOCKED_PADDLE = 1;
constexpr uint16_t SHOT_TRACE_BLOCKED_PROTECTION = 2;
constexpr uint16_t SHOT_TRACE_BLOCKED_PENDING = 0x100;  // A weight stop was pending.
// SLOW_WINDOW_END continuation flags.
constexpr uint16_t SHOT_TRACE_SLOW_REACHED = 0x1;
constexpr uint16_t SHOT_TRACE_SLOW_EXTENDED = 0x2;
constexpr uint16_t SHOT_TRACE_SLOW_FAST_EXTENDED = 0x4;
constexpr uint16_t SHOT_TRACE_SLOW_OVER_LINE = 0x8;

// Header flags.
constexpr uint8_t SHOT_TRACE_VERBOSE_TRUNCATED = 0x1;
constexpr uint8_t SHOT_TRACE_DECISIONS_TRUNCATED = 0x2;
constexpr uint8_t SHOT_TRACE_INTERRUPTED = 0x4;   // Recovered after a reset.
constexpr uint8_t SHOT_TRACE_TAIL_DROPPED = 0x8;  // Its torn last append was dropped.

// Snapshot flags.
enum : uint16_t {
  SHOT_TRACE_SNAP_AUTO_TARE = 1U << 0,
  SHOT_TRACE_SNAP_AUTO_RETARE = 1U << 1,
  SHOT_TRACE_SNAP_IDLE_TARE = 1U << 2,
  SHOT_TRACE_SNAP_ACCESSORY_RETARE = 1U << 3,
  SHOT_TRACE_SNAP_TOUCH_AVOIDANCE = 1U << 4,
  SHOT_TRACE_SNAP_REQUIRE_CUP = 1U << 5,
  SHOT_TRACE_SNAP_STOP_IF_REMOVED = 1U << 6,
  SHOT_TRACE_SNAP_CUP_PROTECTION = 1U << 7,
  SHOT_TRACE_SNAP_CUP_PRESENT = 1U << 8,
  SHOT_TRACE_SNAP_CUP_TARED = 1U << 9,
  SHOT_TRACE_SNAP_EMPTY_VALID = 1U << 10,
  SHOT_TRACE_SNAP_IDLE_ELIGIBLE = 1U << 11,
  SHOT_TRACE_SNAP_SCALE_TARE = 1U << 12,
  SHOT_TRACE_SNAP_MICRA_CONNECTED = 1U << 13,
  SHOT_TRACE_SNAP_RSSI_VALID = 1U << 14,
};

// Rejected readings by reason: the five SCALE_SAMPLE_REJECTED_* codes, other.
constexpr size_t SHOT_TRACE_REJECT_REASONS = 6;

struct ShotTraceRecord {
  uint32_t head;
  int16_t a;
  uint16_t b;
};

struct ShotTracePreShot {
  ShotTraceRecord event;  // head time: ms before the anchor
  uint32_t arg2;          // the profile event's second argument
  uint16_t arg1High;      // high half of its first argument (b holds the low)
  uint16_t repeats;       // earlier occurrences folded into this one, saturating
};

struct ShotTraceHeader {
  uint32_t magic;
  uint16_t formatVersion;
  uint16_t vocabularyVersion;
  uint32_t totalBytes;  // header through the last event
  uint32_t crc32;       // CRC-32 of totalBytes with this field zero; set on export
  uint32_t shotLogId;   // 0: the summary was not stored
  uint32_t bootId;      // boot that captured the shot
  uint32_t anchorMs;    // millis() at cycle start; event times count from it
  uint32_t endedAtMs;   // 0 when interrupted
  uint32_t firmwarePacked;
  uint32_t startCheck;  // FNV-1a of the snapshot and the valid pre-shot entries
  char firmware[24];
  // One aligned 16-byte group, published together after each append.
  uint32_t eventsCheck;  // FNV-1a of events[0, eventCount)
  uint32_t prevCheck;    // ... and of events[0, prevCount), before the last append
  uint16_t eventCount;
  uint16_t prevCount;
  uint8_t flags;
  uint8_t endReason;
  uint16_t preShotCount;
  uint16_t snapshotBytes;
  uint16_t countersBytes;
  uint16_t preShotCapacity;
  uint16_t reserved;
};

struct ShotTraceSnapshot {
  uint32_t freeHeapBytes;
  uint32_t freePsramBytes;
  uint32_t postTareGraceMs;
  uint32_t retareWindowMs;
  uint32_t stabilityDurationMs;
  uint32_t stabilityGapMs;
  int32_t minimumCupCg;
  int32_t cupRemovedCg;
  int32_t stabilityToleranceCg;
  int32_t emptyReferenceCg;     // INT32_MIN: unknown
  int32_t occupiedReferenceCg;  // INT32_MIN: unknown
  int32_t preTareWeightCg;      // INT32_MIN: unknown
  uint16_t flags;
  uint8_t stabilitySamples;
  uint8_t paddleMode;
  uint8_t noScaleBbwMode;
  uint8_t idleEligibility;  // IdleTareReason
  uint8_t idleLastReason;   // IdleTareReason
  int8_t wifiRssi;
  char scaleName[32];
  char scaleProtocol[20];
};

struct ShotTraceCounters {
  uint32_t readingsReceived;
  uint32_t readingsAccepted;
  uint32_t readingsHeld;
  uint32_t readingsRejected[SHOT_TRACE_REJECT_REASONS];
  uint32_t readingGapMinMs;
  uint32_t readingGapMeanMs;
  uint32_t readingGapMaxMs;
  uint32_t packetGaps;
  uint32_t staleFrames;        // decoded too late or from an old connection
  uint32_t undecodableFrames;
  uint32_t confirmationResets;  // over-line chains that broke before a stop
  uint32_t loopGapMaxMs;
  uint32_t micraRxBytes;
  uint32_t uiStreamDeferrals;
  uint32_t verboseDropped;
  uint32_t decisionsDropped;
};

constexpr size_t SHOT_TRACE_FIXED_BYTES =
    sizeof(ShotTraceHeader) + sizeof(ShotTraceSnapshot) + sizeof(ShotTraceCounters) +
    SHOT_TRACE_PRESHOT_CAPACITY * sizeof(ShotTracePreShot);
constexpr size_t SHOT_TRACE_EVENT_CAPACITY =
    (SHOT_TRACE_BUFFER_BYTES - SHOT_TRACE_FIXED_BYTES) / sizeof(ShotTraceRecord);
// Verbose events (each rejected reading, touch, packet gap) stop at 75 %.
constexpr size_t SHOT_TRACE_VERBOSE_LIMIT = SHOT_TRACE_EVENT_CAPACITY * 3 / 4;

struct ShotTraceBuffer {
  ShotTraceHeader header;
  ShotTraceSnapshot snapshot;
  ShotTraceCounters counters;
  ShotTracePreShot preShot[SHOT_TRACE_PRESHOT_CAPACITY];
  ShotTraceRecord events[SHOT_TRACE_EVENT_CAPACITY];
};
static_assert(sizeof(ShotTraceRecord) == 8 && sizeof(ShotTracePreShot) == 16 &&
                  sizeof(ShotTraceHeader) == 88 && offsetof(ShotTraceHeader, eventsCheck) == 64 &&
                  sizeof(ShotTraceSnapshot) == 108 &&
                  sizeof(ShotTraceCounters) == 84,
              "Shot trace wire layout");
static_assert(sizeof(ShotTraceBuffer) == SHOT_TRACE_BUFFER_BYTES &&
                  offsetof(ShotTraceBuffer, events) == SHOT_TRACE_FIXED_BYTES,
              "A shot trace buffer is exactly D2 with no padding");

// Two capture buffers plus the pre-shot ring in PSRAM that no boot clears:
// nothing in it is trusted before recoverAtBoot() validated it, and DRAM
// state (generation 0, empty ring) hides everything else. Line alignment
// keeps each header's publish group in one cache line.
struct alignas(64) ShotTraceStore {
  ShotTraceBuffer buffers[2];
  ShotTracePreShot ring[SHOT_TRACE_PRESHOT_CAPACITY];
  uint32_t ringAtMs[SHOT_TRACE_PRESHOT_CAPACITY];
};
inline OPEN_BREW_BY_WEIGHT_PSRAM_NOINIT ShotTraceStore shotTraceStore;

// PSRAM is write-back cached and a reset invalidates the cache. The panic
// handler writes the whole store back with the raw ROM call (target only);
// tasks use shotTraceWriteBack() (esp_cache_msync) for the shutdown hook and
// for each header whose magic changed, so a reset that skips both (another
// watchdog, a direct hardware watchdog) still finds the latest magic.
void shotTraceWriteBackForPanic();
void shotTraceWriteBack(const void *address, size_t bytes);
// Web UI log line when a recovered trace fails its first-use verification.
void shotTraceLogRecoveryDropped(uint32_t crashedBootId);

// Resets after which PSRAM kept its contents: software restart (3), panic
// (4), interrupt (5), task (6) and other watchdogs (7). Power-on, brownout
// and every other reason leave it undefined.
inline bool shotTraceResetKeepsPsram(uint32_t resetReason) {
  return resetReason >= 3 && resetReason <= 7;
}

// FNV-1a, chained across calls; a few cycles per byte on the append path.
inline uint32_t shotTraceCheck(uint32_t check, const void *data, size_t bytes) {
  const uint8_t *in = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < bytes; ++i) check = (check ^ in[i]) * 16777619U;
  return check;
}
constexpr uint32_t SHOT_TRACE_CHECK_SEED = 2166136261U;

enum class ShotTraceExport : uint8_t { OK, BUSY, EMPTY, CHANGED, SINK_FAILED };

// The newest completed trace, taken for the flash pool: its generation, the
// export CRC and the identity of the summary it explains.
struct ShotTracePending {
  uint32_t generation, crc, shotLogId, bootId, endedAtMs, firmwarePacked;
  uint16_t totalBytes;
  uint8_t endReason;
};

inline int16_t shotTraceDg(float weightG) {
  if (!std::isfinite(weightG)) return SHOT_TRACE_NO_WEIGHT;
  const float dg = std::round(weightG * 10.0f);
  return static_cast<int16_t>(dg > 32767.0f ? 32767.0f : dg < -32767.0f ? -32767.0f : dg);
}

// Order of the values noteState() takes; the first three are pre-shot evidence.
enum ShotTraceState : uint8_t {
  SHOT_TRACE_STATE_CUP_PRESENT,
  SHOT_TRACE_STATE_EMPTY_READY,
  SHOT_TRACE_STATE_IDLE_ELIGIBILITY,
  SHOT_TRACE_STATE_WEIGHT_CONTROL,
  SHOT_TRACE_STATE_BASELINE_WAITING,
  SHOT_TRACE_STATE_BBW_PROTECTION,
  SHOT_TRACE_STATE_SCALE_LOSS_GUARD,
  SHOT_TRACE_STATE_COUNT,
};

inline int16_t shotTraceSaturate16(int32_t value) {
  return static_cast<int16_t>(value > INT16_MAX ? INT16_MAX
                              : value < -INT16_MAX ? -INT16_MAX : value);
}

class ShotTrace {
 public:
  bool capturing() const { return capturing_.load(std::memory_order_acquire); }

  // ---------------------------------------------------------------------
  // Capture window: control loop only.
  // ---------------------------------------------------------------------

  // Cycle start with a scale, before the relay acts so the window catches
  // the close: only the header and counters are reset here.
  void begin(uint32_t anchorMs) {
    const TaskLockGuard lock(mux_);
    ShotTraceBuffer &buffer = *capture_;
    memset(&buffer.header, 0, sizeof(buffer.header));  // Not recoverable yet.
    memset(&buffer.counters, 0, sizeof(buffer.counters));
    buffer.header.anchorMs = anchorMs;
    buffer.header.eventsCheck = buffer.header.prevCheck = SHOT_TRACE_CHECK_SEED;
    received_.store(0, std::memory_order_relaxed);
    accepted_.store(0, std::memory_order_relaxed);
    held_.store(0, std::memory_order_relaxed);
    gapSum_.store(0, std::memory_order_relaxed);
    gapMax_.store(0, std::memory_order_relaxed);
    gapMin_.store(UINT32_MAX, std::memory_order_relaxed);
    loopGapMax_.store(0, std::memory_order_relaxed);
    confirmationResets_.store(0, std::memory_order_relaxed);
    staleFrames_.store(0, std::memory_order_relaxed);
    undecodableFrames_.store(0, std::memory_order_relaxed);
    micraRx_.store(0, std::memory_order_relaxed);
    uiDeferrals_.store(0, std::memory_order_relaxed);
    for (size_t i = SHOT_TRACE_STATE_IDLE_ELIGIBILITY; i < SHOT_TRACE_STATE_COUNT; ++i)
      stateKnown_[i] = false;  // Recorded as initial.
    capturing_.store(true, std::memory_order_release);
  }

  // After the machine started: identity, the snapshot and the frozen pre-shot
  // ring; the capture becomes recoverable last. The ring is not written while
  // the window is open.
  void attachStart(const ShotTraceSnapshot &snapshot, uint32_t bootId) {
    ShotTraceHeader *changed = nullptr;
    {
      const TaskLockGuard lock(mux_);
      if (!capturing_.load(std::memory_order_relaxed)) return;
      changed = &attachLocked_(snapshot, bootId);
    }
    shotTraceWriteBack(changed, sizeof(*changed));
  }

  // Rinse, abandoned start or refused start: the record is dropped and the
  // next trace's pre-shot section says so.
  void discard(uint8_t endReason, uint32_t nowMs) {
    ShotTraceHeader *changed = nullptr;
    {
      const TaskLockGuard lock(mux_);
      if (!capturing_.load(std::memory_order_relaxed)) return;
      const uint32_t ranMs = timeOfLocked_(nowMs);
      changed = &capture_->header;
      changed->magic = 0;  // Never recovered.
      capturing_.store(false, std::memory_order_release);
      notePreShotLocked_(static_cast<uint32_t>(ShotTraceKind::TRACE_DISCARDED) << 24,
                         endReason,
                         static_cast<uint16_t>(ranMs > UINT16_MAX ? UINT16_MAX : ranMs), 0,
                         nowMs);
    }
    shotTraceWriteBack(changed, sizeof(*changed));
  }

  // Drip finalize or cancellation. The newest completed trace always wins.
  void complete(uint32_t shotLogId, uint32_t bootId, uint32_t endedAtMs,
                uint8_t endReason) {
    ShotTraceHeader *changed = nullptr;
    {
      const TaskLockGuard lock(mux_);
      if (!capturing_.load(std::memory_order_relaxed)) return;
      changed = &completeLocked_(shotLogId, bootId, endedAtMs, endReason);
    }
    shotTraceWriteBack(changed, sizeof(*changed));
  }

 private:
  ShotTraceHeader &attachLocked_(const ShotTraceSnapshot &snapshot, uint32_t bootId) {
    ShotTraceBuffer &buffer = *capture_;
    ShotTraceHeader &header = buffer.header;
    header.formatVersion = SHOT_TRACE_FORMAT_VERSION;
    header.vocabularyVersion = SHOT_TRACE_VOCABULARY_VERSION;
    header.firmwarePacked = FW_VERSION_PACKED;
    copyCString(header.firmware, sizeof(header.firmware), FW_VERSION);
    header.snapshotBytes = sizeof(ShotTraceSnapshot);
    header.countersBytes = sizeof(ShotTraceCounters);
    header.preShotCapacity = SHOT_TRACE_PRESHOT_CAPACITY;
    header.bootId = bootId;
    buffer.snapshot = snapshot;
    if (micraSubscribed_.load(std::memory_order_relaxed))
      buffer.snapshot.flags |= SHOT_TRACE_SNAP_MICRA_CONNECTED;
    memset(buffer.preShot, 0, sizeof(buffer.preShot));
    for (size_t i = 0; i < ringCount_; ++i) {
      buffer.preShot[i] = shotTraceStore.ring[i];
      const int32_t age =
          static_cast<int32_t>(buffer.header.anchorMs - shotTraceStore.ringAtMs[i]);
      buffer.preShot[i].event.head |= age <= 0 ? 0
          : static_cast<uint32_t>(age) > SHOT_TRACE_MS_MAX ? SHOT_TRACE_MS_MAX
                                                           : static_cast<uint32_t>(age);
    }
    header.preShotCount = static_cast<uint16_t>(ringCount_);
    ringCount_ = 0;
    header.startCheck = startCheck_(buffer);
    std::atomic_signal_fence(std::memory_order_seq_cst);  // Everything before the marker.
    header.magic = SHOT_TRACE_CAPTURING_MAGIC;
    return header;
  }

  ShotTraceHeader &completeLocked_(uint32_t shotLogId, uint32_t bootId, uint32_t endedAtMs,
                                   uint8_t endReason) {
    ShotTraceBuffer &buffer = *capture_;
    ShotTraceHeader &header = buffer.header;
    header.magic = SHOT_TRACE_MAGIC;  // Completed: never recovered.
    header.shotLogId = shotLogId;
    header.bootId = bootId;
    header.endedAtMs = endedAtMs;
    header.endReason = endReason;
    header.totalBytes = static_cast<uint32_t>(
        SHOT_TRACE_FIXED_BYTES + header.eventCount * sizeof(ShotTraceRecord));
    mirrorCountersLocked_();
    ShotTraceBuffer *const completed = capture_;
    capture_ = completed_;
    completed_ = completed;
    if (++generation_ == 0) generation_ = 1;
    recoveredUnverified_ = false;  // A recovered trace was replaced.
    capturing_.store(false, std::memory_order_release);
    return header;
  }

 public:
  // Boot, before any producer runs: constant time, header fields only. After
  // a reset that kept PSRAM, a valid in-flight capture becomes the completed
  // trace, flagged interrupted; its events are verified on first use. Every
  // other in-flight marker is cleared so it is never trusted later.
  bool recoverAtBoot(uint32_t resetReason, uint32_t &crashedBootId) {
    bool changed[2] = {};
    {
      const TaskLockGuard lock(mux_);
      recoverLocked_(resetReason, crashedBootId, changed);
    }
    for (size_t i = 0; i < 2; ++i)  // Without this a crash loop would recover it again.
      if (changed[i])
        shotTraceWriteBack(&shotTraceStore.buffers[i].header, sizeof(ShotTraceHeader));
    return generation_ != 0;
  }

 private:
  void recoverLocked_(uint32_t resetReason, uint32_t &crashedBootId, bool (&changed)[2]) {
    const bool kept = shotTraceResetKeepsPsram(resetReason);
    for (size_t i = 0; i < 2; ++i) {
      ShotTraceBuffer &buffer = shotTraceStore.buffers[i];
      ShotTraceHeader &header = buffer.header;
      if (header.magic != SHOT_TRACE_CAPTURING_MAGIC) continue;
      changed[i] = true;
      header.magic = 0;
      if (!kept || generation_ != 0 || !recoverableHeader_(header)) continue;
      header.magic = SHOT_TRACE_MAGIC;  // Recovered at most once.
      header.flags |= SHOT_TRACE_INTERRUPTED;
      header.shotLogId = header.endedAtMs = 0;
      header.endReason = 0;
      completed_ = &buffer;
      capture_ = &buffer == &shotTraceStore.buffers[0] ? &shotTraceStore.buffers[1]
                                                     : &shotTraceStore.buffers[0];
      generation_ = 1;
      recoveredUnverified_ = true;
      crashedBootId = header.bootId;
    }
  }

 public:

  // ---------------------------------------------------------------------
  // Producers. Task context only, never under a portMUX; bounded and
  // non-blocking apart from the leaf mutex. A closed window costs one
  // atomic load, except the pre-shot events listed in preShotKind().
  // ---------------------------------------------------------------------

  void noteProfile(ScaleProfileEvent kind, uint32_t nowMs, float weightG,
                   uint32_t arg1, uint32_t arg2) {
    if (kind == ScaleProfileEvent::WEIGHT_ACCEPTED) {
      // Fires on every reading: counted exactly, never recorded.
      if (capturing()) accepted_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (!capturing() && !preShotKind(kind)) return;
    const uint32_t code = static_cast<uint32_t>(kind) << 24;
    const int16_t a = shotTraceDg(weightG);
    const uint16_t b = static_cast<uint16_t>(arg1 & 0xFFFFU);
    const uint16_t high1 = static_cast<uint16_t>(arg1 >> 16);
    const TaskLockGuard lock(mux_);
    if (!capturing_.load(std::memory_order_relaxed)) {
      if (preShotKind(kind)) notePreShotLocked_(code, a, b, arg2, nowMs, high1);
      return;
    }
    ShotTraceCounters &counters = capture_->counters;
    bool verbose = false;
    switch (kind) {
      case ScaleProfileEvent::WEIGHT_REJECTED: {
        // arg1 is the DebugCode; the reference weight (arg2) is not kept.
        const uint32_t reason =
            arg1 - static_cast<uint32_t>(DebugCode::SCALE_SAMPLE_REJECTED_INVALID);
        ++counters.readingsRejected[reason < SHOT_TRACE_REJECT_REASONS - 1
                                        ? reason : SHOT_TRACE_REJECT_REASONS - 1];
        arg2 = 0;
        verbose = true;
        break;
      }
      case ScaleProfileEvent::PACKET_GAP:
        ++counters.packetGaps;
        verbose = true;
        break;
      case ScaleProfileEvent::TOUCH_HOLD:
      case ScaleProfileEvent::TOUCH_RELEASE:
      case ScaleProfileEvent::TOUCH_SUSTAINED:
        verbose = true;
        break;
      default:
        break;
    }
    // The continuation keeps arg1's high half in its head and arg2 in a/b.
    appendLocked_(code | timeOfLocked_(nowMs), a, b, high1 != 0 || arg2 != 0,
                  static_cast<int16_t>(arg2 & 0xFFFFU),
                  static_cast<uint16_t>(arg2 >> 16), verbose, high1);
  }

  // Every addDebugEvent during the window; the two arguments are split in
  // 16-bit halves, the high halves in a continuation when they do not fit.
  void noteDebug(DebugCode code, int32_t argument1, int32_t argument2,
                 uint32_t nowMs) {
    if (!capturing()) return;
    bool verbose = false;
    switch (code) {
      // Twins of profile events the trace already records.
      case DebugCode::SCALE_SAMPLE_REJECTED_INVALID:
      case DebugCode::SCALE_SAMPLE_REJECTED_RANGE:
      case DebugCode::SCALE_SAMPLE_REJECTED_SLEW:
      case DebugCode::SCALE_SAMPLE_REJECTED_RECOVERY:
      case DebugCode::SCALE_SAMPLE_REJECTED_PRE_CYCLE:
      case DebugCode::ACCIDENTAL_TOUCH_HOLD:
      case DebugCode::ACCIDENTAL_TOUCH_RELEASE:
      case DebugCode::ACCIDENTAL_TOUCH_SUSTAINED:
      case DebugCode::SCALE_PACKET_GAP:
      case DebugCode::SCALE_EVENT_DROPPED:
        return;
      case DebugCode::ACCIDENTAL_TOUCH_HANDOFF:
        verbose = true;
        break;
      default:
        break;
    }
    const uint32_t high2 = static_cast<uint32_t>(argument2) >> 16;
    const bool wide = argument1 != static_cast<int16_t>(argument1) || high2 != 0;
    const TaskLockGuard lock(mux_);
    if (!capturing_.load(std::memory_order_relaxed)) return;
    appendLocked_(SHOT_TRACE_DEBUG_NAMESPACE |
                      static_cast<uint32_t>(code) << 24 | timeOfLocked_(nowMs),
                  static_cast<int16_t>(argument1 & 0xFFFF),
                  static_cast<uint16_t>(argument2 & 0xFFFF), wide,
                  static_cast<int16_t>(argument1 >> 16),
                  static_cast<uint16_t>(high2), verbose);
  }

  // Decisions without a profile or debug event; at most one continuation.
  void note(ShotTraceKind kind, uint32_t nowMs, int16_t a, uint16_t b,
            bool more = false, int16_t moreA = 0, uint16_t moreB = 0) {
    if (!capturing()) return;
    const TaskLockGuard lock(mux_);
    if (!capturing_.load(std::memory_order_relaxed)) return;
    appendLocked_(static_cast<uint32_t>(kind) << 24 | timeOfLocked_(nowMs), a, b,
                  more, moreA, moreB, false);
  }

  // Control loop only: change detection for the traced signals, given in
  // ShotTraceState order (shot-only values are ignored outside a shot). The
  // cache has one owner, so only a change takes the mutex.
  void noteState(const uint32_t (&values)[SHOT_TRACE_STATE_COUNT], uint32_t nowMs) {
    const bool window = capturing();
    const size_t first = window ? SHOT_TRACE_STATE_IDLE_ELIGIBILITY : 0;
    const size_t last = window ? SHOT_TRACE_STATE_COUNT : SHOT_TRACE_STATE_WEIGHT_CONTROL;
    for (size_t i = first; i < last; ++i) {
      const bool initial = !stateKnown_[i];
      if (!initial && stateValue_[i] == values[i]) continue;
      stateKnown_[i] = true;
      stateValue_[i] = values[i];
      const uint32_t code = static_cast<uint32_t>(ScaleProfileEvent::STATE_CHANGED) << 24;
      const int16_t a = shotTraceSaturate16(static_cast<int32_t>(values[i]));
      const uint16_t b = static_cast<uint16_t>(kStateSignals[i]) |
                         (initial ? SHOT_TRACE_STATE_INITIAL : 0);
      const TaskLockGuard lock(mux_);
      if (capturing_.load(std::memory_order_relaxed)) {
        appendLocked_(code | timeOfLocked_(nowMs), a, b, false, 0, 0, false);
      } else {
        notePreShotLocked_(code, a, b, 0, nowMs);
      }
    }
  }

  // Control loop only: every weight reading control processes.
  void noteReading(uint32_t receivedAtMs) {
    if (!capturing()) return;
    const uint32_t received = received_.load(std::memory_order_relaxed);
    if (received != 0) {
      const uint32_t gap = receivedAtMs - lastReadingAtMs_;
      gapSum_.store(gapSum_.load(std::memory_order_relaxed) + gap, std::memory_order_relaxed);
      if (gap > gapMax_.load(std::memory_order_relaxed))
        gapMax_.store(gap, std::memory_order_relaxed);
      if (gap < gapMin_.load(std::memory_order_relaxed))
        gapMin_.store(gap, std::memory_order_relaxed);
    }
    lastReadingAtMs_ = receivedAtMs;
    received_.store(received + 1, std::memory_order_relaxed);
  }

  // A reading control consumed without accepting or rejecting it (touch
  // hold, recovery confirmation, pending retare effect).
  void noteHeld() {
    if (capturing()) held_.fetch_add(1, std::memory_order_relaxed);
  }

  // Control loop only: an over-line chain broke before it confirmed a stop.
  void noteConfirmationReset() {
    if (capturing()) confirmationResets_.fetch_add(1, std::memory_order_relaxed);
  }

  // Scale worker: frames the scale profile sees but scaleProfileNoteEvent does not.
  void noteDroppedFrame(bool stale) {
    if (capturing())
      (stale ? staleFrames_ : undecodableFrames_).fetch_add(1, std::memory_order_relaxed);
  }

  // Control loop only.
  void noteLoopGap(uint32_t gapMs) {
    if (capturing() && gapMs > loopGapMax_.load(std::memory_order_relaxed))
      loopGapMax_.store(gapMs, std::memory_order_relaxed);
  }

  void noteMicraRx(uint32_t bytes) {
    if (capturing()) micraRx_.fetch_add(bytes, std::memory_order_relaxed);
  }

  void noteMicraSubscribed(bool subscribed) {
    micraSubscribed_.store(subscribed, std::memory_order_relaxed);
  }

  void noteUiDeferral() {
    if (capturing()) uiDeferrals_.fetch_add(1, std::memory_order_relaxed);
  }

  // ---------------------------------------------------------------------
  // Readers (httpd). Nothing is exported while a capture is open.
  // ---------------------------------------------------------------------

  // Streams the newest completed trace in `capacity`-byte chunks after a
  // first pass that computes its CRC, which is patched into the header.
  // The mutex is held per chunk copy only, never across the sink.
  template <typename Sink>
  ShotTraceExport exportCompleted(uint8_t *chunk, size_t capacity, Sink &&sink) {
    uint32_t generation = 0, crc = 0;
    size_t total = 0;
    const ShotTraceExport state = completedTrace_(generation, total);
    if (state != ShotTraceExport::OK) return state;
    if (!crcOf_(generation, total, chunk, capacity, crc)) return ShotTraceExport::CHANGED;
    for (size_t offset = 0; offset < total; offset += capacity) {
      const size_t bytes = total - offset < capacity ? total - offset : capacity;
      if (!copyExport(generation, offset, chunk, bytes, crc)) return ShotTraceExport::CHANGED;
      if (!sink(chunk, bytes)) return ShotTraceExport::SINK_FAILED;
    }
    return ShotTraceExport::OK;
  }

  // Export bytes [offset, offset + bytes) of `generation`, its CRC set;
  // false once a newer completion (or a capture) replaced it.
  bool copyExport(uint32_t generation, size_t offset, uint8_t *out, size_t bytes,
                  uint32_t crc) const {
    if (!copyCompleted_(generation, offset, out, bytes)) return false;
    for (size_t i = 0; i < sizeof(crc); ++i) {
      const size_t at = offsetof(ShotTraceHeader, crc32) + i;
      if (at >= offset && at < offset + bytes) out[at - offset] = static_cast<uint8_t>(crc >> (8U * i));
    }
    return true;
  }

  // ---------------------------------------------------------------------
  // Flash pool (settings_persist worker). The newest completed trace waits
  // until consumed; a newer completion replaces an unsaved one (newest
  // wins), and the next consume() counts it as dropped.
  // ---------------------------------------------------------------------

  bool pendingPersist(ShotTracePending &out, uint8_t *chunk, size_t capacity) {
    size_t total = 0;
    if (completedTrace_(out.generation, total) != ShotTraceExport::OK) return false;
    {
      const TaskLockGuard lock(mux_);
      if (out.generation != generation_ || out.generation == consumed_) return false;
      const ShotTraceHeader &header = completed_->header;
      out.shotLogId = header.shotLogId;
      out.bootId = header.bootId;
      out.endedAtMs = header.endedAtMs;
      out.firmwarePacked = header.firmwarePacked;
      out.endReason = header.endReason;
    }
    out.totalBytes = static_cast<uint16_t>(total);
    return crcOf_(out.generation, total, chunk, capacity, out.crc);
  }

  // Saved, already on flash or given up; returns how many completions
  // before it were never consumed (dropped).
  uint32_t consume(uint32_t generation) {
    const TaskLockGuard lock(mux_);
    const uint32_t dropped = generation - consumed_ - 1U;
    consumed_ = generation;
    tracesDropped_ += dropped;
    return dropped;
  }

  // Clear all and factory reset: the completed trace's summary is gone.
  void consumeCompleted() {
    const TaskLockGuard lock(mux_);
    consumed_ = generation_;
  }

  uint32_t tracesDropped() const {
    const TaskLockGuard lock(mux_);
    return tracesDropped_;
  }

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // A reboot for DRAM state; the PSRAM store keeps its bytes.
  void hostReset() {
    const TaskLockGuard lock(mux_);
    capturing_.store(false);
    generation_ = 0;
    recoveredUnverified_ = false;
    capture_ = &shotTraceStore.buffers[0];
    completed_ = &shotTraceStore.buffers[1];
    consumed_ = tracesDropped_ = 0;
    ringCount_ = 0;
    for (bool &known : stateKnown_) known = false;
    micraSubscribed_.store(false);
  }
#endif

 private:
  static constexpr ScaleProfileSignal kStateSignals[SHOT_TRACE_STATE_COUNT] = {
      ScaleProfileSignal::CUP_PRESENT,      ScaleProfileSignal::EMPTY_READY,
      ScaleProfileSignal::IDLE_ELIGIBILITY, ScaleProfileSignal::WEIGHT_CONTROL,
      ScaleProfileSignal::BASELINE_WAITING, ScaleProfileSignal::BBW_PROTECTION,
      ScaleProfileSignal::SCALE_LOSS_GUARD,
  };

  // Idle cup, tare and link evidence kept for the next shot.
  static bool preShotKind(ScaleProfileEvent kind) {
    switch (kind) {
      case ScaleProfileEvent::SCALE_CONNECTED:
      case ScaleProfileEvent::SCALE_DISCONNECTED:
      case ScaleProfileEvent::TARE_REQUEST:
      case ScaleProfileEvent::TARE_WRITE_OK:
      case ScaleProfileEvent::TARE_WRITE_FAILED:
      case ScaleProfileEvent::TARE_BASELINE_CONFIRMED:
      case ScaleProfileEvent::TARE_BASELINE_TIMEOUT:
      case ScaleProfileEvent::TARE_RELEASED:
      case ScaleProfileEvent::TARE_PHASE:
      case ScaleProfileEvent::TARE_REJECTED:
      case ScaleProfileEvent::CUP_PLACED:
      case ScaleProfileEvent::CUP_REMOVED:
      case ScaleProfileEvent::CUP_ANCHOR_CHANGED:
      case ScaleProfileEvent::CUP_ANCHOR_REJECTED:
      case ScaleProfileEvent::CUP_QUALIFICATION_RESET:
      case ScaleProfileEvent::CUP_RESET:
      case ScaleProfileEvent::REFERENCE_CHANGED:
      case ScaleProfileEvent::FINALIZE_CANCELLED:
        return true;
      default:
        return false;
    }
  }

  uint32_t timeOfLocked_(uint32_t nowMs) const {
    const int32_t ms = static_cast<int32_t>(nowMs - capture_->header.anchorMs);
    return ms <= 0 ? 0 : static_cast<uint32_t>(ms) > SHOT_TRACE_MS_MAX
                             ? SHOT_TRACE_MS_MAX : static_cast<uint32_t>(ms);
  }

  // Constant time. Verbose records stop at 75 %; decisions use the rest.
  void appendLocked_(uint32_t head, int16_t a, uint16_t b, bool more,
                     int16_t moreA, uint16_t moreB, bool verbose, uint16_t moreHead = 0) {
    ShotTraceBuffer &buffer = *capture_;
    uint16_t &count = buffer.header.eventCount;
    const size_t limit = verbose ? SHOT_TRACE_VERBOSE_LIMIT : SHOT_TRACE_EVENT_CAPACITY;
    if (count + (more ? 2U : 1U) > limit) {
      ++(verbose ? buffer.counters.verboseDropped : buffer.counters.decisionsDropped);
      buffer.header.flags |= verbose ? SHOT_TRACE_VERBOSE_TRUNCATED
                                     : SHOT_TRACE_DECISIONS_TRUNCATED;
      return;
    }
    // Records first, then the count and checks: a reset in between leaves
    // the append unpublished, and recovery drops a torn last append.
    const ShotTraceRecord records[2] = {
        {head, a, b},
        {moreHead | static_cast<uint32_t>(ShotTraceKind::CONTINUATION) << 24, moreA, moreB}};
    const size_t added = more ? 2 : 1;
    memcpy(&buffer.events[count], records, added * sizeof(ShotTraceRecord));
    std::atomic_signal_fence(std::memory_order_seq_cst);  // Records before the group.
    ShotTraceHeader &header = buffer.header;
    header.prevCheck = header.eventsCheck;
    header.prevCount = count;
    header.eventsCheck =
        shotTraceCheck(header.eventsCheck, records, added * sizeof(ShotTraceRecord));
    count = static_cast<uint16_t>(count + added);
    mirrorCountersLocked_();  // An interrupted trace keeps them as of its last event.
  }

  // The lock-free counters, as complete() or the last append saw them.
  void mirrorCountersLocked_() {
    ShotTraceCounters &counters = capture_->counters;
    counters.readingsReceived = received_.load(std::memory_order_relaxed);
    counters.readingsAccepted = accepted_.load(std::memory_order_relaxed);
    counters.readingsHeld = held_.load(std::memory_order_relaxed);
    counters.readingGapMaxMs = gapMax_.load(std::memory_order_relaxed);
    const uint32_t gapMin = gapMin_.load(std::memory_order_relaxed);
    counters.readingGapMinMs = gapMin == UINT32_MAX ? 0 : gapMin;
    counters.readingGapMeanMs = counters.readingsReceived > 1
        ? gapSum_.load(std::memory_order_relaxed) / (counters.readingsReceived - 1) : 0;
    counters.staleFrames = staleFrames_.load(std::memory_order_relaxed);
    counters.undecodableFrames = undecodableFrames_.load(std::memory_order_relaxed);
    counters.confirmationResets = confirmationResets_.load(std::memory_order_relaxed);
    counters.loopGapMaxMs = loopGapMax_.load(std::memory_order_relaxed);
    counters.micraRxBytes = micraRx_.load(std::memory_order_relaxed);
    counters.uiStreamDeferrals = uiDeferrals_.load(std::memory_order_relaxed);
  }

  static uint32_t startCheck_(const ShotTraceBuffer &buffer) {
    const uint32_t check =
        shotTraceCheck(SHOT_TRACE_CHECK_SEED, &buffer.snapshot, sizeof(buffer.snapshot));
    return shotTraceCheck(check, buffer.preShot,
                          buffer.header.preShotCount * sizeof(ShotTracePreShot));
  }

  // The no-init address depends on the image layout: only this firmware's
  // own captures are trusted.
  static bool recoverableHeader_(const ShotTraceHeader &header) {
    char firmware[sizeof(header.firmware)];
    copyCString(firmware, sizeof(firmware), FW_VERSION);
    return header.firmwarePacked == FW_VERSION_PACKED &&
           strncmp(header.firmware, firmware, sizeof(firmware)) == 0 &&
           header.formatVersion == SHOT_TRACE_FORMAT_VERSION &&
           header.vocabularyVersion == SHOT_TRACE_VOCABULARY_VERSION &&
           header.snapshotBytes == sizeof(ShotTraceSnapshot) &&
           header.countersBytes == sizeof(ShotTraceCounters) &&
           header.preShotCapacity == SHOT_TRACE_PRESHOT_CAPACITY &&
           header.preShotCount <= SHOT_TRACE_PRESHOT_CAPACITY &&
           header.eventCount <= SHOT_TRACE_EVENT_CAPACITY &&
           header.prevCount <= header.eventCount;
  }

  // First use of a recovered trace (httpd): the start check and the event
  // checks decide what is kept. A torn last append is dropped; anything
  // else inconsistent drops the whole trace.
  bool verifyRecoveredLocked_() {
    recoveredUnverified_ = false;
    ShotTraceBuffer &buffer = *completed_;
    ShotTraceHeader &header = buffer.header;
    uint32_t check = SHOT_TRACE_CHECK_SEED;
    uint32_t prevCheck = header.prevCount == 0 ? check : 0;
    for (uint16_t i = 0; i < header.eventCount; ++i) {
      check = shotTraceCheck(check, &buffer.events[i], sizeof(ShotTraceRecord));
      if (i + 1 == header.prevCount) prevCheck = check;
    }
    if (check != header.eventsCheck) {
      if (prevCheck != header.prevCheck) header.magic = 0;
      header.eventCount = header.prevCount;
      header.flags |= SHOT_TRACE_TAIL_DROPPED;
    }
    if (header.startCheck != startCheck_(buffer)) header.magic = 0;
    if (header.magic != SHOT_TRACE_MAGIC) {
      generation_ = 0;
      return false;
    }
    header.totalBytes = static_cast<uint32_t>(
        SHOT_TRACE_FIXED_BYTES + header.eventCount * sizeof(ShotTraceRecord));
    return true;
  }

  // Latest occurrence per kind (per signal for states) with a repeat count.
  // The keys (preShotKind() plus the pre-shot states) fit the capacity.
  void notePreShotLocked_(uint32_t code, int16_t a, uint16_t b, uint32_t arg2,
                          uint32_t nowMs, uint16_t arg1High = 0) {
    const bool state =
        code == static_cast<uint32_t>(ScaleProfileEvent::STATE_CHANGED) << 24;
    const uint16_t signal = b & static_cast<uint16_t>(~SHOT_TRACE_STATE_INITIAL);
    size_t slot = 0;
    while (slot < ringCount_ &&
           (shotTraceStore.ring[slot].event.head != code ||
            (state && (shotTraceStore.ring[slot].event.b &
                       static_cast<uint16_t>(~SHOT_TRACE_STATE_INITIAL)) != signal)))
      ++slot;
    uint16_t repeats = 0;
    if (slot < ringCount_) {
      repeats = shotTraceStore.ring[slot].repeats;
      if (repeats < UINT16_MAX) ++repeats;
    } else if (ringCount_ < SHOT_TRACE_PRESHOT_CAPACITY) {
      ++ringCount_;
    } else {
      return;
    }
    shotTraceStore.ring[slot] =
        ShotTracePreShot{ShotTraceRecord{code, a, b}, arg2, arg1High, repeats};
    shotTraceStore.ringAtMs[slot] = nowMs;
  }

  // The newest completed trace (a recovered one verified first): BUSY while
  // a capture is open, EMPTY when there is none.
  ShotTraceExport completedTrace_(uint32_t &generation, size_t &total) {
    bool dropped = false;
    uint32_t droppedBootId = 0;
    {
      const TaskLockGuard lock(mux_);
      if (capturing_.load(std::memory_order_relaxed)) return ShotTraceExport::BUSY;
      if (recoveredUnverified_) {
        droppedBootId = completed_->header.bootId;
        dropped = !verifyRecoveredLocked_();
      }
      generation = generation_;
      total = completed_->header.totalBytes;
    }
    if (dropped) shotTraceLogRecoveryDropped(droppedBootId);
    return generation == 0 ? ShotTraceExport::EMPTY : ShotTraceExport::OK;
  }

  // CRC-32 of the record with its CRC field zero, chunk by chunk.
  bool crcOf_(uint32_t generation, size_t total, uint8_t *chunk, size_t capacity,
              uint32_t &crc) const {
    crc = 0;
    for (size_t offset = 0; offset < total; offset += capacity) {
      const size_t bytes = total - offset < capacity ? total - offset : capacity;
      if (!copyCompleted_(generation, offset, chunk, bytes)) return false;
      crc = scaleProfileChecksum(crc, chunk, bytes);
    }
    return true;
  }

  bool copyCompleted_(uint32_t generation, size_t offset, uint8_t *out,
                      size_t bytes) const {
    const TaskLockGuard lock(mux_);
    if (capturing_.load(std::memory_order_relaxed) || generation != generation_ ||
        offset + bytes > completed_->header.totalBytes)
      return false;
    memcpy(out, reinterpret_cast<const uint8_t *>(completed_) + offset, bytes);
    return true;
  }

  mutable TaskMutex mux_;
  std::atomic<bool> capturing_{false};
  std::atomic<bool> micraSubscribed_{false};
  ShotTraceBuffer *capture_ = &shotTraceStore.buffers[0];
  ShotTraceBuffer *completed_ = &shotTraceStore.buffers[1];
  uint32_t generation_ = 0;  // 0: nothing completed since boot
  uint32_t consumed_ = 0, tracesDropped_ = 0;  // Flash pool bookkeeping.
  bool recoveredUnverified_ = false;
  size_t ringCount_ = 0;
  // Control-loop-owned (noteState, noteReading).
  bool stateKnown_[SHOT_TRACE_STATE_COUNT] = {};
  uint32_t stateValue_[SHOT_TRACE_STATE_COUNT] = {};
  uint32_t lastReadingAtMs_ = 0;
  std::atomic<uint32_t> received_{0}, accepted_{0}, held_{0};
  std::atomic<uint32_t> gapSum_{0}, gapMin_{UINT32_MAX}, gapMax_{0};
  std::atomic<uint32_t> confirmationResets_{0}, staleFrames_{0}, undecodableFrames_{0};
  std::atomic<uint32_t> loopGapMax_{0}, micraRx_{0}, uiDeferrals_{0};
};

// Shared instance across the control and network units; the scale worker
// reaches it through the sinks declared in OpenBrewByWeightScaleProfiler.h.
inline ShotTrace &shotTrace() {
  static ShotTrace instance;
  return instance;
}

}  // namespace openbrewbyweight

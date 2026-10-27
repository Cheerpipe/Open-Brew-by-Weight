#pragma once

// =============================================================================
// LAYER: Scale profiler (manual raw-weight and firmware-event trace)
// =============================================================================
// WHAT: One manually controlled, bounded capture session of every decoded
//       scale weight (before firmware acceptance) plus correlated tare, cup,
//       touch, first-drop and control decisions. Records live in PSRAM while
//       recording; a completed trace is persisted to a dedicated flash
//       partition by the settings_persist worker and survives reboot.
//
// OWNERSHIP: the core-0 health worker owns the lifecycle (start/stop/delete,
//       capacity stop, ETA, index build, persistence dispatch). Producers (scale worker
//       task and the Arduino control loop) append fixed-size records through
//       the scaleProfileNote* functions after releasing their own locks; the
//       capture mutex is a leaf lock and is never nested under scale, debug
//       or relay locks. Disabled capture costs one atomic load per note call.
//       One mutex covers the whole capture/store state so append (32-byte
//       write), lifecycle transitions and status copies cannot deadlock.
//
// BOUNDARY: No control decisions, no machine access, no tare commands. This
//       component only observes. It must stay host-testable: flash access is
//       limited to the ESP-IDF partition API staged through the shared flash
//       scratch, and everything else is portable C++.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightScaleLink.h"
#include "OpenBrewByWeightTaskMutex.h"

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include <esp_partition.h>
#endif

namespace openbrewbyweight {

constexpr size_t SCALE_PROFILE_RECORD_CAPACITY = 8192;
constexpr size_t SCALE_PROFILE_RECORD_BYTES = 32;
// The append ordinal needs only 14 bits; upper bits extend relativeMs.
constexpr uint32_t SCALE_PROFILE_ORDINAL_BITS = 14;
static_assert(SCALE_PROFILE_RECORD_CAPACITY < (1U << SCALE_PROFILE_ORDINAL_BITS),
              "Record ordinals must fit below the timestamp extension");
// One record slot is reserved so the terminal marker always fits.
constexpr size_t SCALE_PROFILE_ORDINARY_CAPACITY =
    SCALE_PROFILE_RECORD_CAPACITY - 1;
constexpr size_t SCALE_PROFILE_INDEX_BYTES =
    SCALE_PROFILE_RECORD_CAPACITY * sizeof(uint32_t);
constexpr size_t SCALE_PROFILE_CONTEXT_BYTES = 1024;
constexpr uint32_t SCALE_PROFILE_MAGIC = 0x53505246UL;  // "SPRF"
constexpr uint16_t SCALE_PROFILE_SCHEMA_VERSION = 1;
// 4096-byte header/context sector + 256 KiB maximum record payload.
constexpr size_t SCALE_PROFILE_PARTITION_BYTES = 0x41000;
constexpr size_t SCALE_PROFILE_PAYLOAD_OFFSET = 4096;
static_assert(SCALE_PROFILE_RECORD_CAPACITY * SCALE_PROFILE_RECORD_BYTES ==
                  SCALE_PROFILE_PARTITION_BYTES - SCALE_PROFILE_PAYLOAD_OFFSET,
              "RAM capture and flash payload capacity must agree");
constexpr uint32_t SCALE_PROFILE_SAVE_RETRY_MS = 5000;
constexpr uint8_t SCALE_PROFILE_SAVE_MAX_RETRIES = 3;

// Flags shared by WEIGHT (sample discontinuity) and STALE_FRAME (the frame
// still decoded to a weight the normal path rejected).
constexpr uint16_t SCALE_PROFILE_FLAG_DISCONTINUITY = 0x0001;
constexpr uint16_t SCALE_PROFILE_FLAG_DECODED = 0x0002;

enum class ScaleProfileEvent : uint16_t {
  WEIGHT = 0,
  PROFILE_START,
  PROFILE_STOP,
  SCALE_CONNECTED,
  SCALE_DISCONNECTED,
  STALE_FRAME,
  FRAME_UNDECODABLE,
  CONTROL_FIFO_LOSS,
  PACKET_GAP,
  EVENT_DROPPED,
  TARE_REQUEST,
  TARE_WRITE_OK,
  TARE_WRITE_FAILED,
  TARE_BASELINE_CONFIRMED,
  TARE_BASELINE_TIMEOUT,
  TARE_RELEASED,
  CUP_PLACED,
  CUP_REMOVED,
  CUP_ANCHOR_CHANGED,
  WEIGHT_ACCEPTED,
  WEIGHT_REJECTED,
  FIRST_DROP,
  TOUCH_HOLD,
  TOUCH_RELEASE,
  TOUCH_SUSTAINED,
  SHOT_START,
  SHOT_END,
  CONTROL_SUSPENDED,
  CONTROL_RECOVERED,
  SETTINGS_CHANGED,
  REFERENCE_CHANGED,
  STATE_CHANGED,
  TARE_PHASE,
  TARE_REJECTED,
  CUP_QUALIFICATION_RESET,
  CUP_RESET,
  FINALIZE_CANCELLED,
  FIRST_DROP_SEEKING_FA,
  CUP_ANCHOR_REJECTED,
};

// Append-only signal IDs. Values are scalars, not another control state machine.
#define SCALE_PROFILE_SIGNALS(X) \
  X(CUP_PRESENT) X(CUP_HOLD) X(CUP_REFERENCE_KNOWN) X(CUP_TARED) \
  X(EMPTY_READY) X(UNLOAD_QUALIFIED) X(CUP_SETTLING) X(CUP_REMOVAL_PENDING) \
  X(CUP_MASS_VALID) X(EMPTY_ANCHOR_CG) X(OCCUPIED_REFERENCE_CG) X(PLACEMENT_ID) \
  X(IDLE_ELIGIBILITY) X(IDLE_READY_FOR_CUP) X(IDLE_REQUEST_ID) X(IDLE_LAST_REASON) \
  X(ACCESSORY_SETTLING) X(CYCLE_ID) X(CYCLE_ACTIVE) X(WEIGHT_CONTROL) X(STREAM) \
  X(FIRST_FLOW_PHASE) X(TOUCH_PHASE) X(TOUCH_CLASS) X(BASELINE_WAITING) \
  X(RETARE_OPEN) X(RETARE_PERFORMED) X(RETARE_EFFECT_PENDING) X(BBW_PROTECTION) \
  X(SCALE_LOSS_GUARD) X(FINALIZE_CYCLE_ID) X(CONFIG_REVISION) X(AUTO_IDLE_TARE) \
  X(AUTO_TARE) X(AUTO_RETARE) X(MIN_CUP_CG) X(REMOVED_CG) X(STABILITY_TOLERANCE_CG) \
  X(STABILITY_SAMPLES) X(STABILITY_DURATION_MS) X(STABILITY_GAP_MS) \
  X(BASELINE_GRACE_MS) X(RETARE_WINDOW_MS) X(IDLE_PLACEMENT_ID) X(IDLE_ORIGIN) \
  X(SHOT_TARE_REQUEST_ID) X(RETARE_REQUEST_ID) X(IDLE_COMMAND_PHASE) \
  X(EMPTY_SETTLING) X(EMPTY_REFERENCE_BLOCKED) X(CUP_NEGATIVE_HOLE) \
  X(IDLE_STATUS) X(IDLE_DEFERRED_REASON) X(CUP_PROTECTION_ENABLED) X(STOP_IF_REMOVED) \
  X(REQUIRE_CUP) X(TOUCH_ENABLED) X(GOAL_CG) X(OFFSET_CG) X(NO_SCALE_BBW_MODE)

enum class ScaleProfileSignal : uint16_t {
#define SCALE_PROFILE_SIGNAL_ENUM(name) name,
  SCALE_PROFILE_SIGNALS(SCALE_PROFILE_SIGNAL_ENUM)
#undef SCALE_PROFILE_SIGNAL_ENUM
  COUNT
};
inline const char *scaleProfileSignalName(uint16_t signal) {
  static const char *const names[] = {
#define SCALE_PROFILE_SIGNAL_NAME(name) #name,
    SCALE_PROFILE_SIGNALS(SCALE_PROFILE_SIGNAL_NAME)
#undef SCALE_PROFILE_SIGNAL_NAME
  };
  return signal < static_cast<uint16_t>(ScaleProfileSignal::COUNT)
      ? names[signal] : "UNKNOWN";
}
#undef SCALE_PROFILE_SIGNALS

constexpr uint16_t SCALE_PROFILE_INITIAL = 0x8000;
constexpr uint32_t SCALE_PROFILE_UNKNOWN_VALUE = UINT32_MAX;
struct ScaleProfileValue {
  ScaleProfileSignal signal;
  uint32_t value;
};

enum class ScaleProfileTareOrigin : uint8_t {
  SHOT_START,
  RETARE,
  IDLE_CUP,
  ACCESSORY,
  MANUAL,
};

enum class ScaleProfilerState : uint8_t { EMPTY, PREPARING, RECORDING, STOPPED, SAVED };
enum class ScaleProfilerPersistence : uint8_t {
  NONE,
  PENDING_SAVE,
  SAVING,
  SAVED,
  INVALIDATING,
  FAILED,
};
enum class ScaleProfilerStopReason : uint8_t {
  NONE,
  USER,
  TIMEOUT,
  BUFFER_FULL,
  TRACE_LOSS,
};
enum class ScaleProfilerError : uint8_t {
  NONE,
  NO_PARTITION,
  ALLOCATION,
  BUSY,
  INVALIDATE_FAILED,
  SAVE_FAILED,
  INDEX_FAILED,
};
enum class ScaleProfilerRequest : uint8_t { NONE, START, STOP, DELETE };
enum class ScaleProfilerWork : uint8_t { NONE, INVALIDATE, SAVE };

inline const char *scaleProfilerStateName(ScaleProfilerState state) {
  switch (state) {
    case ScaleProfilerState::EMPTY: return "empty";
    case ScaleProfilerState::PREPARING: return "preparing";
    case ScaleProfilerState::RECORDING: return "recording";
    case ScaleProfilerState::STOPPED: return "stopped";
    case ScaleProfilerState::SAVED: return "saved";
  }
  return "empty";
}

inline const char *scaleProfilerPersistenceName(ScaleProfilerPersistence state) {
  switch (state) {
    case ScaleProfilerPersistence::NONE: return "none";
    case ScaleProfilerPersistence::PENDING_SAVE: return "pending";
    case ScaleProfilerPersistence::SAVING: return "saving";
    case ScaleProfilerPersistence::SAVED: return "saved";
    case ScaleProfilerPersistence::INVALIDATING: return "invalidating";
    case ScaleProfilerPersistence::FAILED: return "failed";
  }
  return "none";
}

inline const char *scaleProfilerStopReasonName(ScaleProfilerStopReason reason) {
  switch (reason) {
    case ScaleProfilerStopReason::NONE: return "none";
    case ScaleProfilerStopReason::USER: return "user";
    case ScaleProfilerStopReason::TIMEOUT: return "timeout";
    case ScaleProfilerStopReason::BUFFER_FULL: return "buffer_full";
    case ScaleProfilerStopReason::TRACE_LOSS: return "trace_loss";
  }
  return "none";
}

inline const char *scaleProfilerErrorName(ScaleProfilerError error) {
  switch (error) {
    case ScaleProfilerError::NONE: return "none";
    case ScaleProfilerError::NO_PARTITION: return "no_partition";
    case ScaleProfilerError::ALLOCATION: return "allocation";
    case ScaleProfilerError::BUSY: return "busy";
    case ScaleProfilerError::INVALIDATE_FAILED: return "invalidate_failed";
    case ScaleProfilerError::SAVE_FAILED: return "save_failed";
    case ScaleProfilerError::INDEX_FAILED: return "index_failed";
  }
  return "none";
}

inline const char *scaleProfileEventName(ScaleProfileEvent kind) {
  switch (kind) {
    case ScaleProfileEvent::WEIGHT: return "WEIGHT";
    case ScaleProfileEvent::PROFILE_START: return "PROFILE_START";
    case ScaleProfileEvent::PROFILE_STOP: return "PROFILE_STOP";
    case ScaleProfileEvent::SCALE_CONNECTED: return "SCALE_CONNECTED";
    case ScaleProfileEvent::SCALE_DISCONNECTED: return "SCALE_DISCONNECTED";
    case ScaleProfileEvent::STALE_FRAME: return "STALE_FRAME";
    case ScaleProfileEvent::FRAME_UNDECODABLE: return "FRAME_UNDECODABLE";
    case ScaleProfileEvent::CONTROL_FIFO_LOSS: return "CONTROL_FIFO_LOSS";
    case ScaleProfileEvent::PACKET_GAP: return "PACKET_GAP";
    case ScaleProfileEvent::EVENT_DROPPED: return "EVENT_DROPPED";
    case ScaleProfileEvent::TARE_REQUEST: return "TARE_REQUEST";
    case ScaleProfileEvent::TARE_WRITE_OK: return "TARE_WRITE_OK";
    case ScaleProfileEvent::TARE_WRITE_FAILED: return "TARE_WRITE_FAILED";
    case ScaleProfileEvent::TARE_BASELINE_CONFIRMED: return "TARE_BASELINE_CONFIRMED";
    case ScaleProfileEvent::TARE_BASELINE_TIMEOUT: return "TARE_BASELINE_TIMEOUT";
    case ScaleProfileEvent::TARE_RELEASED: return "TARE_RELEASED";
    case ScaleProfileEvent::CUP_PLACED: return "CUP_PLACED";
    case ScaleProfileEvent::CUP_REMOVED: return "CUP_REMOVED";
    case ScaleProfileEvent::CUP_ANCHOR_CHANGED: return "CUP_ANCHOR_CHANGED";
    case ScaleProfileEvent::WEIGHT_ACCEPTED: return "WEIGHT_ACCEPTED";
    case ScaleProfileEvent::WEIGHT_REJECTED: return "WEIGHT_REJECTED";
    case ScaleProfileEvent::FIRST_DROP: return "FIRST_DROP";
    case ScaleProfileEvent::TOUCH_HOLD: return "TOUCH_HOLD";
    case ScaleProfileEvent::TOUCH_RELEASE: return "TOUCH_RELEASE";
    case ScaleProfileEvent::TOUCH_SUSTAINED: return "TOUCH_SUSTAINED";
    case ScaleProfileEvent::SHOT_START: return "SHOT_START";
    case ScaleProfileEvent::SHOT_END: return "SHOT_END";
    case ScaleProfileEvent::CONTROL_SUSPENDED: return "CONTROL_SUSPENDED";
    case ScaleProfileEvent::CONTROL_RECOVERED: return "CONTROL_RECOVERED";
    case ScaleProfileEvent::SETTINGS_CHANGED: return "SETTINGS_CHANGED";
    case ScaleProfileEvent::REFERENCE_CHANGED: return "REFERENCE_CHANGED";
    case ScaleProfileEvent::STATE_CHANGED: return "STATE_CHANGED";
    case ScaleProfileEvent::TARE_PHASE: return "TARE_PHASE";
    case ScaleProfileEvent::TARE_REJECTED: return "TARE_REJECTED";
    case ScaleProfileEvent::CUP_QUALIFICATION_RESET: return "CUP_QUALIFICATION_RESET";
    case ScaleProfileEvent::CUP_RESET: return "CUP_RESET";
    case ScaleProfileEvent::FINALIZE_CANCELLED: return "FINALIZE_CANCELLED";
    case ScaleProfileEvent::FIRST_DROP_SEEKING_FA: return "FIRST_DROP_SEEKING_FA";
    case ScaleProfileEvent::CUP_ANCHOR_REJECTED: return "CUP_ANCHOR_REJECTED";
  }
  return "UNKNOWN";
}

// Fixed-size capture record. Source time is session-relative milliseconds;
// ordinal breaks equal-timestamp ties and makes deferred decisions
// unambiguous. `sequence` carries the originating capture sequence for
// weights and decisions; arg1/arg2 hold event-specific correlation data.
struct ScaleProfileRecord {
  uint32_t relativeMs = 0;
  uint32_t ordinal = 0;
  uint32_t connectionGeneration = 0;
  uint32_t sequence = 0;
  float weightG = NAN;
  uint32_t arg1 = 0;
  uint32_t arg2 = 0;
  uint16_t kind = static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP);
  uint16_t flags = 0;
};
static_assert(sizeof(ScaleProfileRecord) == SCALE_PROFILE_RECORD_BYTES,
              "Scale profile record layout must stay 32 bytes");

// Durable header committed last; lives in the partition's first 4 KiB sector.
struct ScaleProfileHeader {
  uint32_t magic = 0;
  uint16_t schemaVersion = 0;
  uint16_t headerBytes = 0;
  uint32_t generation = 0;
  uint32_t sessionId = 0;
  uint32_t recordCount = 0;
  uint32_t payloadBytes = 0;
  uint32_t durationMs = 0;
  uint32_t stopReason = 0;
  uint32_t lostCount = 0;
  uint32_t weightCount = 0;
  uint32_t eventCount = 0;
  uint32_t startWallUtcSec = 0;
  uint32_t checksum = 0;
  uint32_t durationMsHigh = 0;
  uint32_t reserved[3] = {};
  char context[SCALE_PROFILE_CONTEXT_BYTES] = {};
};
static_assert(sizeof(ScaleProfileHeader) == 1092 &&
                  sizeof(ScaleProfileHeader) <= FLASH_IO_SCRATCH_BYTES,
              "Scale profile header layout; it is staged through the flash scratch");

struct ScaleProfilerStatus {
  ScaleProfilerState state = ScaleProfilerState::EMPTY;
  ScaleProfilerPersistence persistence = ScaleProfilerPersistence::NONE;
  ScaleProfilerStopReason stopReason = ScaleProfilerStopReason::NONE;
  ScaleProfilerError lastError = ScaleProfilerError::NONE;
  uint32_t generation = 0;
  uint32_t sessionId = 0;
  uint64_t elapsedMs = 0;
  uint32_t estimatedRemainingMs = UINT32_MAX;  // Unknown; JSON emits null.
  uint32_t recordCount = 0;
  uint32_t savedRecordCount = 0;
  uint32_t weightCount = 0;
  uint32_t eventCount = 0;
  uint32_t lostCount = 0;
  uint8_t saveRetries = 0;
  bool partitionAvailable = false;
  bool downloading = false;
};

inline uint64_t scaleProfileRecordTimeMs(const ScaleProfileRecord &record) {
  return (static_cast<uint64_t>(record.ordinal >> SCALE_PROFILE_ORDINAL_BITS)
          << 32) | record.relativeMs;
}

inline uint64_t scaleProfileDurationMs(const ScaleProfileHeader &header) {
  return (static_cast<uint64_t>(header.durationMsHigh) << 32) | header.durationMs;
}

// Composes the bounded initial context (firmware/build identity, scale,
// settings, starting detector state) on the health worker. Returns the UTC
// second of the capture epoch, or 0 when wall time is unknown.
using ScaleProfileContextProvider = uint32_t (*)(char *text, size_t capacity);

// Incrementally chainable CRC-32 (IEEE); chaining this function across chunks
// in order yields the standard one-shot value after the final call.
inline uint32_t scaleProfileChecksum(uint32_t crc, const void *data, size_t bytes) {
  const uint8_t *in = static_cast<const uint8_t *>(data);
  uint32_t running = ~crc;
  for (size_t i = 0; i < bytes; ++i) {
    running ^= in[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      running = (running >> 1) ^ (0xEDB88320UL & (0u - (running & 1u)));
    }
  }
  return ~running;
}

inline bool scaleProfileHeaderValid(const ScaleProfileHeader &header) {
  return header.magic == SCALE_PROFILE_MAGIC &&
         header.schemaVersion == SCALE_PROFILE_SCHEMA_VERSION &&
         header.headerBytes == sizeof(ScaleProfileHeader) &&
         header.recordCount <= SCALE_PROFILE_RECORD_CAPACITY &&
         header.payloadBytes ==
             header.recordCount * SCALE_PROFILE_RECORD_BYTES &&
         header.payloadBytes <=
             SCALE_PROFILE_PARTITION_BYTES - SCALE_PROFILE_PAYLOAD_OFFSET &&
         header.stopReason <=
             static_cast<uint32_t>(ScaleProfilerStopReason::TRACE_LOSS);
}

class ScaleProfiler {
 public:
  void setContextProvider(ScaleProfileContextProvider provider) {
    captureMux_.lock();
    contextProvider_ = provider;
    captureMux_.unlock();
  }

  bool partitionAvailable() const {
    captureMux_.lock();
    const bool available = partitionAvailable_;
    captureMux_.unlock();
    return available;
  }

  bool recording() const {
    return stateAtomic_.load(std::memory_order_acquire) ==
           ScaleProfilerState::RECORDING;
  }

  uint32_t downloadLeaseGeneration() const {
    return downloadLease_.load(std::memory_order_acquire);
  }

  ScaleProfilerStatus status(uint32_t nowMs) const {
    ScaleProfilerStatus out;
    captureMux_.lock();
    out.state = state_;
    out.persistence = persistence_;
    out.stopReason = stopReason_;
    out.lastError = lastError_;
    out.generation = generation_;
    out.sessionId = sessionId_;
    out.recordCount = state_ == ScaleProfilerState::EMPTY ||
                              state_ == ScaleProfilerState::PREPARING
                          ? 0 : recordCount_;
    out.savedRecordCount = savedRecordCount_;
    out.weightCount = weightCount_;
    out.eventCount = eventCount_;
    out.lostCount = lostCount_;
    if (state_ == ScaleProfilerState::EMPTY || state_ == ScaleProfilerState::PREPARING) {
      out.weightCount = out.eventCount = out.lostCount = 0;
    }
    out.saveRetries = saveRetries_;
    out.partitionAvailable = partitionAvailable_;
    out.downloading = downloadLease_.load(std::memory_order_acquire) != 0;
    out.elapsedMs = state_ == ScaleProfilerState::RECORDING
                        ? static_cast<uint64_t>(std::max<int64_t>(0, elapsedAt_(nowMs)))
                        : (state_ == ScaleProfilerState::EMPTY ||
                                   state_ == ScaleProfilerState::PREPARING
                               ? 0 : durationMs_);
    if (state_ == ScaleProfilerState::RECORDING) {
      out.estimatedRemainingMs = recordCount_ >= SCALE_PROFILE_ORDINARY_CAPACITY
                                    ? 0 : estimatedRemainingMs_;
    } else if ((state_ == ScaleProfilerState::STOPPED ||
                state_ == ScaleProfilerState::SAVED) &&
               stopReason_ == ScaleProfilerStopReason::BUFFER_FULL) {
      out.estimatedRemainingMs = 0;
    }
    captureMux_.unlock();
    return out;
  }

  // ---------------------------------------------------------------------
  // Producer interface: scale worker task and Arduino control loop only.
  // Never call from an ISR or NimBLE callback; never while holding a scale,
  // debug or relay lock. All functions are bounded and non-blocking.
  // ---------------------------------------------------------------------

  void noteWeight(float weightG, uint32_t receivedAtMs,
                  uint32_t connectionGeneration, uint32_t captureSequence,
                  bool discontinuity, uint32_t nowMs) {
    if (!recording()) return;
    captureMux_.lock();
    if (state_ != ScaleProfilerState::RECORDING ||
        static_cast<int64_t>(static_cast<uint32_t>(nowMs - receivedAtMs)) >
            elapsedAt_(nowMs)) {
      // Buffered pre-epoch evidence is not part of this session.
      captureMux_.unlock();
      return;
    }
    ScaleProfileRecord record;
    record.connectionGeneration = connectionGeneration;
    record.sequence = captureSequence;
    record.weightG = weightG;
    record.flags = discontinuity ? SCALE_PROFILE_FLAG_DISCONTINUITY : 0;
    record.kind = static_cast<uint16_t>(ScaleProfileEvent::WEIGHT);
    appendLocked_(record, nowMs, nowMs - receivedAtMs);
    captureMux_.unlock();
  }

  // Dropped-frame observation that keeps the frame's own reception time
  // (the library stamps it when the notification arrived), not the later
  // dequeue time.
  void noteDroppedFrame(bool stale, bool decoded, float weightG,
                        uint32_t receivedAtMs, uint32_t connectionGeneration,
                        uint32_t captureSequence, uint16_t length,
                        uint32_t nowMs) {
    if (!recording()) return;
    captureMux_.lock();
    if (state_ != ScaleProfilerState::RECORDING ||
        static_cast<int64_t>(static_cast<uint32_t>(nowMs - receivedAtMs)) >
            elapsedAt_(nowMs)) {
      captureMux_.unlock();
      return;
    }
    ScaleProfileRecord record;
    record.connectionGeneration = connectionGeneration;
    record.sequence = captureSequence;
    record.weightG = decoded ? weightG : NAN;
    record.arg1 = length;
    record.flags = decoded ? SCALE_PROFILE_FLAG_DECODED : 0;
    record.kind =
        static_cast<uint16_t>(stale ? ScaleProfileEvent::STALE_FRAME
                                    : ScaleProfileEvent::FRAME_UNDECODABLE);
    appendLocked_(record, nowMs, nowMs - receivedAtMs);
    captureMux_.unlock();
  }

  void noteEvent(ScaleProfileEvent kind, uint32_t nowMs,
                 uint32_t connectionGeneration, uint32_t sequence, float weightG,
                 uint32_t arg1, uint32_t arg2, uint16_t flags = 0) {
    if (!recording()) return;
    ScaleProfileRecord record;
    record.connectionGeneration = connectionGeneration;
    record.sequence = sequence;
    record.weightG = weightG;
    record.arg1 = arg1;
    record.arg2 = arg2;
    record.flags = flags;
    record.kind = static_cast<uint16_t>(kind);
    captureMux_.lock();
    if (state_ != ScaleProfilerState::RECORDING) {
      captureMux_.unlock();
      return;
    }
    appendLocked_(record, nowMs);
    captureMux_.unlock();
  }

  // One control-owner observation, atomically ordered within the capture.
  // Cache lives in PSRAM and resets with each capture, including mid-cycle starts.
  void noteValues(const ScaleProfileValue *values, size_t count, uint32_t nowMs,
                  uint32_t connection, uint32_t captureSequence) {
    if (!recording()) return;
    const TaskLockGuard lock(captureMux_);
    if (state_ != ScaleProfilerState::RECORDING) return;
    for (size_t i = 0; i < count; ++i) {
      const auto key = static_cast<uint16_t>(values[i].signal);
      if (key >= static_cast<uint16_t>(ScaleProfileSignal::COUNT)) continue;
      auto &previous = workspace_->observations[key];
      const bool initial = previous.ordinal == 0;
      if (!initial && previous.arg2 == values[i].value &&
          previous.connectionGeneration == connection) continue;
      ScaleProfileRecord record;
      record.kind = static_cast<uint16_t>(ScaleProfileEvent::STATE_CHANGED);
      record.flags = key | (initial ? SCALE_PROFILE_INITIAL : 0);
      record.arg1 = initial ? SCALE_PROFILE_UNKNOWN_VALUE : previous.arg2;
      record.arg2 = values[i].value;
      record.connectionGeneration = connection;
      record.sequence = captureSequence;
      appendLocked_(record, nowMs);
      previous = record;
      previous.ordinal = 1;
    }
  }

  // ---------------------------------------------------------------------
  // Lifecycle: health worker only (100 ms cadence).
  // ---------------------------------------------------------------------

  void bootInit() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    const bool available = storePartition() != nullptr;
    ScaleProfileHeader header;
    const bool loaded = available && storeReadHeader(header);
    captureMux_.lock();
    partitionAvailable_ = available;
    if (loaded) adoptSavedHeaderLocked_(header);
    captureMux_.unlock();
    stateAtomic_.store(state_, std::memory_order_release);
#endif
  }

  // Returns the persistence work the caller must enqueue on the
  // settings_persist worker; noteWorkDispatched()/noteWorkDispatchFailed()
  // report the enqueue outcome.
  ScaleProfilerWork service(ScaleProfilerRequest request, uint32_t nowMs) {
    consumeFlashResult_(nowMs);
    ScaleProfilerWork work = ScaleProfilerWork::NONE;
    captureMux_.lock();
    if (state_ == ScaleProfilerState::RECORDING) {
      elapsedMs_ += static_cast<uint32_t>(nowMs - clockAtMs_);
      clockAtMs_ = nowMs;
    }
    // Requests are serviced first so a Start/Stop/Delete is never silently
    // consumed by a retry window; the guards make them safe mid-dispatch.
    switch (request) {
      case ScaleProfilerRequest::START:
        work = beginStartLocked_(nowMs);
        break;
      case ScaleProfilerRequest::STOP:
        if (state_ == ScaleProfilerState::RECORDING) {
          stopLocked_(ScaleProfilerStopReason::USER, nowMs);
        }
        break;
      case ScaleProfilerRequest::DELETE:
        work = beginDeleteLocked_();
        break;
      case ScaleProfilerRequest::NONE:
        break;
    }
    serviceRecordingLocked_(nowMs);
    work = servicePersistenceLocked_(nowMs, work);
    if (work == ScaleProfilerWork::NONE &&
        pendingWork_ != ScaleProfilerWork::NONE && !workDispatched_) {
      work = pendingWork_;  // Re-dispatch after a failed enqueue.
    }
    const ScaleProfilerState published = state_;
    captureMux_.unlock();
    stateAtomic_.store(published, std::memory_order_release);
    if (work != ScaleProfilerWork::NONE) {
      captureMux_.lock();
      if (pendingWork_ == work) workDispatched_ = true;
      captureMux_.unlock();
    }
    return work;
  }

  void noteWorkDispatchFailed() {
    captureMux_.lock();
    workDispatched_ = false;  // service() re-dispatches on its next tick.
    captureMux_.unlock();
  }

  // One cache-off flash operation per call; runs on the settings_persist
  // worker between safe-write gates. A terminal result is published through
  // publishFlashResult() by the worker wrapper.
  FlashStoreStepResult serviceFlashStep() {
    captureMux_.lock();
    const FlashPhase phase = flashPhase_;
    if (phase == FlashPhase::IDLE || !flashInFlight_) {
      captureMux_.unlock();
      return FlashStoreStepResult::FAILED;
    }
    captureMux_.unlock();
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    if (!tryLockFlashIo()) return FlashStoreStepResult::FAILED;
    bool ok = false;
    bool complete = false;
    const esp_partition_t *part = storePartition();
    switch (phase) {
      case FlashPhase::INVALIDATE:
        ok = esp_partition_erase_range(part, 0, FLASH_IO_SECTOR_BYTES) == ESP_OK;
        complete = ok;
        break;
      case FlashPhase::SAVE_ERASE: {
        const size_t payloadSectors =
            (flashSave_.payloadBytes + FLASH_IO_SECTOR_BYTES - 1) /
            FLASH_IO_SECTOR_BYTES;
        const size_t eraseIndex = flashSave_.eraseIndex;
        const size_t offset =
            eraseIndex == 0
                ? 0
                : SCALE_PROFILE_PAYLOAD_OFFSET + (eraseIndex - 1) * FLASH_IO_SECTOR_BYTES;
        ok = offset + FLASH_IO_SECTOR_BYTES <= SCALE_PROFILE_PARTITION_BYTES &&
             esp_partition_erase_range(part, offset, FLASH_IO_SECTOR_BYTES) == ESP_OK;
        ++flashSave_.eraseIndex;
        complete = ok && flashSave_.eraseIndex > payloadSectors;
        break;
      }
      case FlashPhase::SAVE_BODY: {
        uint8_t chunk[FLASH_IO_CHUNK_BYTES];
        const size_t count = gatherChunk_(chunk, flashSave_.bodyIndex);
        if (count == 0) {
          // bodyIndex already past the end; the transition below advances
          // to VERIFY. Do not reset it here or the step would loop.
          ok = true;
        } else {
          flashSave_.checksum = scaleProfileChecksum(
              flashSave_.checksum, chunk, count * SCALE_PROFILE_RECORD_BYTES);
          const void *staged =
              copyToFlashIoScratch(chunk, count * SCALE_PROFILE_RECORD_BYTES);
          ok = staged != nullptr &&
               esp_partition_write(part,
                                   SCALE_PROFILE_PAYLOAD_OFFSET +
                                       flashSave_.bodyIndex * SCALE_PROFILE_RECORD_BYTES,
                                   staged,
                                   count * SCALE_PROFILE_RECORD_BYTES) == ESP_OK;
          flashSave_.bodyIndex += count;
        }
        break;
      }
      case FlashPhase::SAVE_VERIFY: {
        uint8_t chunk[FLASH_IO_CHUNK_BYTES];
        const size_t count = gatherChunk_(chunk, flashSave_.bodyIndex);
        if (count == 0) {
          ok = true;
        } else {
          ok = esp_partition_read(part,
                                  SCALE_PROFILE_PAYLOAD_OFFSET +
                                      flashSave_.bodyIndex * SCALE_PROFILE_RECORD_BYTES,
                                  flashIoScratchBytes(),
                                  count * SCALE_PROFILE_RECORD_BYTES) == ESP_OK &&
               memcmp(flashIoScratchBytes(), chunk,
                      count * SCALE_PROFILE_RECORD_BYTES) == 0;
          flashSave_.bodyIndex += count;
        }
        break;
      }
      case FlashPhase::SAVE_COMMIT: {
        ScaleProfileHeader header;
        buildSaveHeader_(header);
        const void *staged = copyToFlashIoScratch(&header, sizeof(header));
        ok = staged != nullptr &&
             esp_partition_write(part, 0, staged, sizeof(header)) == ESP_OK;
        complete = ok;
        break;
      }
      case FlashPhase::IDLE:
        break;
    }
    unlockFlashIo();
    feedFlashIoWatchdog();
    if (!ok) {
      captureMux_.lock();
      flashPhase_ = FlashPhase::IDLE;
      captureMux_.unlock();
      return FlashStoreStepResult::FAILED;
    }
    captureMux_.lock();
    if (phase == FlashPhase::SAVE_ERASE && complete) {
      // Header plus payload sectors erased; program the payload next.
      flashPhase_ = FlashPhase::SAVE_BODY;
      flashSave_.bodyIndex = 0;
    } else if (phase == FlashPhase::SAVE_BODY &&
               flashSave_.bodyIndex >= flashSave_.recordCount) {
      flashPhase_ = FlashPhase::SAVE_VERIFY;
      flashSave_.bodyIndex = 0;
    } else if (phase == FlashPhase::SAVE_VERIFY &&
               flashSave_.bodyIndex >= flashSave_.recordCount) {
      flashPhase_ = FlashPhase::SAVE_COMMIT;
    } else if (complete) {
      flashPhase_ = FlashPhase::IDLE;
    }
    const bool finished = flashPhase_ == FlashPhase::IDLE;
    captureMux_.unlock();
    return finished ? FlashStoreStepResult::COMPLETE : FlashStoreStepResult::MORE;
#else
    return FlashStoreStepResult::FAILED;
#endif
  }

  void publishFlashResult(bool ok) {
    captureMux_.lock();
    const uint32_t generation = flashGeneration_;
    captureMux_.unlock();
    resultMux_.lock();
    resultReady_ = true;
    resultOk_ = ok;
    resultGeneration_ = generation;
    resultMux_.unlock();
  }

  // Factory reset (network task, blocking like the other durable clears).
  // Refuses while the persistence worker owns a flash step or a download
  // pins the trace: reset must never free memory that worker still reads,
  // and a late commit must never resurrect the erased trace.
  bool clearForFactoryReset() {
    captureMux_.lock();
    if (flashInFlight_ || pendingWork_ != ScaleProfilerWork::NONE ||
        downloadLease_.load(std::memory_order_relaxed) != 0) {
      captureMux_.unlock();
      return false;
    }
    captureMux_.unlock();
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    if (partitionAvailable()) {
      const esp_partition_t *part = storePartition();
      bool erased = part != nullptr && tryLockFlashIo();
      if (erased) {
        erased = esp_partition_erase_range(part, 0, FLASH_IO_SECTOR_BYTES) ==
                 ESP_OK;
        unlockFlashIo();
      }
      if (!erased) return false;
    }
#endif
    captureMux_.lock();
    savedRecordCount_ = 0;
    persistence_ = ScaleProfilerPersistence::NONE;
    stopReason_ = ScaleProfilerStopReason::NONE;
    lastError_ = ScaleProfilerError::NONE;
    pendingWork_ = ScaleProfilerWork::NONE;
    workDispatched_ = false;
    flashInFlight_ = false;
    releaseWorkspaceLocked_();
    state_ = ScaleProfilerState::EMPTY;
    captureMux_.unlock();
    stateAtomic_.store(state_, std::memory_order_release);
    return true;
  }

  // ---------------------------------------------------------------------
  // Download support (network task). The lease pins the immutable generation
  // against Start/Delete while a chunked TXT export is in flight.
  // ---------------------------------------------------------------------

  bool acquireDownloadLease() {
    uint32_t expected = 0;
    captureMux_.lock();
    bool usable = (state_ == ScaleProfilerState::STOPPED &&
                   workspace_ != nullptr) ||
                  (state_ == ScaleProfilerState::SAVED && savedRecordCount_ != 0);
    if (usable && state_ == ScaleProfilerState::STOPPED && !indexReady_ &&
        workspace_ != nullptr) {
      if (index_ == nullptr) {
        index_ = static_cast<uint32_t *>(
            allocExternal(SCALE_PROFILE_INDEX_BYTES, AllocationOwner::PROFILER));
      }
      if (index_ != nullptr) {
        buildIndexLocked_();
        indexReady_ = true;
      } else {
        usable = false;  // Without the ordering index nothing is exportable.
      }
    }
    const uint32_t generation = generation_;
    captureMux_.unlock();
    if (!usable) return false;
    if (!downloadLease_.compare_exchange_strong(expected, generation,
                                                std::memory_order_acq_rel)) {
      return false;
    }
    captureMux_.lock();
    const bool stillUsable =
        generation == generation_ && downloadLease_.load(std::memory_order_relaxed) == generation &&
        downloadTargetLocked();
    if (!stillUsable) {
      captureMux_.unlock();
      downloadLease_.store(0, std::memory_order_release);
      return false;
    }
    captureMux_.unlock();
    return true;
  }

  void releaseDownloadLease() { downloadLease_.store(0, std::memory_order_release); }

  // True when the frozen RAM trace is the download source. Gated on the
  // lease, not the state: a save may complete mid-download and flip the
  // state to SAVED while this export still streams from memory.
  bool downloadFromRam() const {
    captureMux_.lock();
    const bool fromRam = workspace_ != nullptr && indexReady_ &&
                         downloadLease_.load(std::memory_order_relaxed) ==
                             generation_;
    captureMux_.unlock();
    return fromRam;
  }

  uint32_t downloadRecordCount() const {
    captureMux_.lock();
    const uint32_t count = state_ == ScaleProfilerState::SAVED ? savedRecordCount_
                                                               : recordCount_;
    captureMux_.unlock();
    return count;
  }

  ScaleProfileRecord downloadRecordAt(uint32_t index) const {
    ScaleProfileRecord record = {};
    captureMux_.lock();
    if (records_ != nullptr && index_ != nullptr && indexReady_ &&
        index < recordCount_ &&
        downloadLease_.load(std::memory_order_relaxed) == generation_) {
      record = records_[index_[index]];
    }
    captureMux_.unlock();
    return record;
  }

  // Workspace header of the frozen RAM trace (zeros when saved to flash).
  ScaleProfileHeader downloadHeader() const {
    captureMux_.lock();
    const ScaleProfileHeader header =
        workspace_ != nullptr ? workspace_->header : ScaleProfileHeader{};
    captureMux_.unlock();
    return header;
  }

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Flash-source download: validated header read. Flash-only, so it never
  // holds the capture mutex across the flash lock (producers must not wait
  // behind flash I/O).
  bool storeReadHeader(ScaleProfileHeader &header) const {
    const esp_partition_t *part = storePartition();
    if (part == nullptr || !tryLockFlashIo()) return false;
    const bool ok = esp_partition_read(part, 0, flashIoScratchBytes(),
                                       sizeof(header)) == ESP_OK;
    unlockFlashIo();
    if (!ok) return false;
    memcpy(&header, flashIoScratchBytes(), sizeof(header));
    return scaleProfileHeaderValid(header);
  }

  bool storeReadRecords(uint32_t startIndex, uint8_t *out, size_t recordCount) const {
    const esp_partition_t *part = storePartition();
    if (part == nullptr || !tryLockFlashIo()) return false;
    const bool ok =
        esp_partition_read(part,
                           SCALE_PROFILE_PAYLOAD_OFFSET +
                               startIndex * SCALE_PROFILE_RECORD_BYTES,
                           out, recordCount * SCALE_PROFILE_RECORD_BYTES) == ESP_OK;
    unlockFlashIo();
    return ok;
  }

  bool storeVerifyChecksum(const ScaleProfileHeader &header) const {
    uint32_t crc = 0;
    uint8_t chunk[FLASH_IO_CHUNK_BYTES];
    for (size_t done = 0; done < header.payloadBytes;) {
      const size_t bytes =
          std::min(sizeof(chunk), static_cast<size_t>(header.payloadBytes - done));
      if (!storeReadRecords(done / SCALE_PROFILE_RECORD_BYTES, chunk,
                            bytes / SCALE_PROFILE_RECORD_BYTES)) {
        return false;
      }
      crc = scaleProfileChecksum(crc, chunk, bytes);
      done += bytes;
    }
    return crc == header.checksum;
  }
#endif

  // Context text for the TXT preamble; source is the RAM workspace or flash.
  void copyDownloadContext(char *out, size_t capacity) const {
    if (out == nullptr || capacity == 0) return;
    out[0] = '\0';
    captureMux_.lock();
    if (state_ == ScaleProfilerState::STOPPED && workspace_ != nullptr) {
      copyCString(out, capacity, workspace_->header.context);
    }
    captureMux_.unlock();
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    if (out[0] == '\0') {
      ScaleProfileHeader header;
      if (storeReadHeader(header)) copyCString(out, capacity, header.context);
    }
#endif
  }

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  // Host-test access to the capture engine without flash hardware.
  void hostReset() {
    captureMux_.lock();
    releaseWorkspaceLocked_();
    state_ = ScaleProfilerState::EMPTY;
    persistence_ = ScaleProfilerPersistence::NONE;
    stopReason_ = ScaleProfilerStopReason::NONE;
    lastError_ = ScaleProfilerError::NONE;
    recordCount_ = weightCount_ = eventCount_ = 0;
    durationMs_ = 0;
    indexReady_ = false;
    flashInFlight_ = false;
    flashPhase_ = FlashPhase::IDLE;
    pendingWork_ = ScaleProfilerWork::NONE;
    workDispatched_ = false;
    captureMux_.unlock();
    stateAtomic_.store(state_, std::memory_order_release);
    downloadLease_.store(0, std::memory_order_release);
  }
  void hostBootSaved(uint32_t recordCount, uint32_t sessionId) {
    captureMux_.lock();
    state_ = ScaleProfilerState::SAVED;
    persistence_ = ScaleProfilerPersistence::SAVED;
    savedRecordCount_ = recordCount;
    sessionId_ = sessionId;
    partitionAvailable_ = true;
    ++generation_;
    captureMux_.unlock();
    stateAtomic_.store(state_, std::memory_order_release);
  }
  bool hostStartNoFlash(uint32_t nowMs) {
    captureMux_.lock();
    partitionAvailable_ = true;
    savedRecordCount_ = 0;
    const bool ok = allocateWorkspaceLocked_();
    if (ok) beginRecordingLocked_(nowMs);
    captureMux_.unlock();
    stateAtomic_.store(state_, std::memory_order_release);
    return ok;
  }
  void hostStop(ScaleProfilerStopReason reason, uint32_t nowMs) {
    captureMux_.lock();
    stopLocked_(reason, nowMs);
    captureMux_.unlock();
    stateAtomic_.store(state_, std::memory_order_release);
  }
  void hostServiceIdle(uint32_t nowMs) {
    (void)service(ScaleProfilerRequest::NONE, nowMs);
  }
  ScaleProfilerWork hostService(ScaleProfilerRequest request, uint32_t nowMs) {
    return service(request, nowMs);
  }
  void hostPublishFlashResult(bool ok) { publishFlashResult(ok); }
  uint32_t hostRecordCount() const {
    captureMux_.lock();
    const uint32_t count = recordCount_;
    captureMux_.unlock();
    return count;
  }
  ScaleProfileRecord hostRecord(size_t index) const {
    captureMux_.lock();
    const ScaleProfileRecord record =
        records_ != nullptr && index < recordCount_ ? records_[index]
                                                    : ScaleProfileRecord{};
    captureMux_.unlock();
    return record;
  }
  ScaleProfileRecord hostExportRecord(size_t index) const {
    captureMux_.lock();
    const ScaleProfileRecord record =
        records_ != nullptr && index_ != nullptr && index < recordCount_
            ? records_[index_[index]]
            : ScaleProfileRecord{};
    captureMux_.unlock();
    return record;
  }
  ScaleProfileHeader hostHeader() const {
    captureMux_.lock();
    const ScaleProfileHeader header =
        workspace_ != nullptr ? workspace_->header : ScaleProfileHeader{};
    captureMux_.unlock();
    return header;
  }
  void hostSetContextProvider(ScaleProfileContextProvider provider) {
    setContextProvider(provider);
  }
  ScaleProfilerState hostState() const {
    return stateAtomic_.load(std::memory_order_acquire);
  }
  ScaleProfilerPersistence hostPersistence() const {
    captureMux_.lock();
    const ScaleProfilerPersistence persistence = persistence_;
    captureMux_.unlock();
    return persistence;
  }
  ScaleProfilerStopReason hostStopReason() const {
    captureMux_.lock();
    const ScaleProfilerStopReason reason = stopReason_;
    captureMux_.unlock();
    return reason;
  }
  ScaleProfilerError hostLastError() const {
    captureMux_.lock();
    const ScaleProfilerError error = lastError_;
    captureMux_.unlock();
    return error;
  }
  uint32_t hostLostCount() const {
    captureMux_.lock();
    const uint32_t lost = lostCount_;
    captureMux_.unlock();
    return lost;
  }
#endif

 private:
  enum class FlashPhase : uint8_t {
    IDLE,
    INVALIDATE,
    SAVE_ERASE,
    SAVE_BODY,
    SAVE_VERIFY,
    SAVE_COMMIT,
  };

  struct Workspace {
    ScaleProfileRecord records[SCALE_PROFILE_RECORD_CAPACITY];
    ScaleProfileHeader header;
    ScaleProfileRecord observations[static_cast<size_t>(ScaleProfileSignal::COUNT)];
  };
  static_assert(sizeof(Workspace) + SCALE_PROFILE_INDEX_BYTES <= 320 * 1024,
                "Scale profiler external workspace exceeds its budget");

  struct FlashSaveState {
    uint32_t recordCount = 0;
    uint32_t payloadBytes = 0;
    size_t eraseIndex = 0;
    size_t bodyIndex = 0;
    uint32_t checksum = 0;
  };

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  static const esp_partition_t *storePartition() {
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
        "scaleprof");
    return part != nullptr && part->size == SCALE_PROFILE_PARTITION_BYTES
               ? part
               : nullptr;
  }

#endif

  bool downloadTargetLocked() const {
    return (state_ == ScaleProfilerState::STOPPED && workspace_ != nullptr &&
            indexReady_) ||
           (state_ == ScaleProfilerState::SAVED && savedRecordCount_ != 0);
  }

  // Caller holds captureMux_ and has already verified state/preamble fields.
  void appendLocked_(const ScaleProfileRecord &record, uint32_t nowMs,
                     uint32_t ageMs = 0) {
    if (recordCount_ >= SCALE_PROFILE_ORDINARY_CAPACITY) {
      ++lostCount_;
      capacityStopRequested_ = true;
      return;
    }
    ScaleProfileRecord stamped = record;
    const uint64_t atMs = elapsedAt_(nowMs) - ageMs;
    stamped.ordinal = ++ordinal_ |
        (static_cast<uint32_t>(atMs >> 32) << SCALE_PROFILE_ORDINAL_BITS);
    stamped.relativeMs = static_cast<uint32_t>(atMs);
    records_[recordCount_++] = stamped;
    capacityStopRequested_ = recordCount_ == SCALE_PROFILE_ORDINARY_CAPACITY;
    if (stamped.kind == static_cast<uint16_t>(ScaleProfileEvent::WEIGHT)) {
      ++weightCount_;
    } else {
      ++eventCount_;
    }
  }

  void consumeFlashResult_(uint32_t nowMs) {
    bool ready = false;
    bool ok = false;
    uint32_t generation = 0;
    resultMux_.lock();
    ready = resultReady_;
    if (ready) {
      resultReady_ = false;
      ok = resultOk_;
      generation = resultGeneration_;
    }
    resultMux_.unlock();
    if (!ready) return;
    captureMux_.lock();
    flashInFlight_ = false;
    pendingWork_ = ScaleProfilerWork::NONE;
    workDispatched_ = false;
    const bool current = generation == generation_;
    if (persistence_ == ScaleProfilerPersistence::INVALIDATING) {
      if (current && ok && pendingStartAfterInvalidate_) {
        pendingStartAfterInvalidate_ = false;
        persistence_ = ScaleProfilerPersistence::NONE;
        savedRecordCount_ = 0;
        beginRecordingLocked_(nowMs);
      } else if (current && ok) {
        persistence_ = ScaleProfilerPersistence::NONE;
        savedRecordCount_ = 0;
        releaseWorkspaceLocked_();
        state_ = ScaleProfilerState::EMPTY;
      } else {
        // The erase outcome is unknown; never promise the old trace back.
        lastError_ = ScaleProfilerError::INVALIDATE_FAILED;
        pendingStartAfterInvalidate_ = false;
        persistence_ = ScaleProfilerPersistence::NONE;
        releaseWorkspaceLocked_();
        state_ = ScaleProfilerState::EMPTY;
      }
    } else if (persistence_ == ScaleProfilerPersistence::SAVING) {
      if (current && ok) {
        persistence_ = ScaleProfilerPersistence::SAVED;
        savedRecordCount_ = recordCount_;
        saveRetries_ = 0;
        state_ = ScaleProfilerState::SAVED;
        if (downloadLease_.load(std::memory_order_relaxed) == 0) {
          releaseWorkspaceLocked_();
        }
      } else {
        lastError_ = ScaleProfilerError::SAVE_FAILED;
        persistence_ = ScaleProfilerPersistence::FAILED;
        if (saveRetries_ < SCALE_PROFILE_SAVE_MAX_RETRIES) {
          ++saveRetries_;
          saveRetryAtMs_ = nowMs + SCALE_PROFILE_SAVE_RETRY_MS;
        }
      }
    }
    const ScaleProfilerState published = state_;
    captureMux_.unlock();
    stateAtomic_.store(published, std::memory_order_release);
  }

  ScaleProfilerWork beginStartLocked_(uint32_t nowMs) {
    if (state_ == ScaleProfilerState::RECORDING ||
        state_ == ScaleProfilerState::PREPARING) {
      return ScaleProfilerWork::NONE;  // Retry never restarts an active session.
    }
    if (downloadLease_.load(std::memory_order_relaxed) != 0 || flashInFlight_ ||
        persistence_ == ScaleProfilerPersistence::SAVING ||
        persistence_ == ScaleProfilerPersistence::INVALIDATING) {
      lastError_ = ScaleProfilerError::BUSY;
      return ScaleProfilerWork::NONE;
    }
    if (!partitionAvailable_) {
      lastError_ = ScaleProfilerError::NO_PARTITION;
      return ScaleProfilerWork::NONE;
    }
    if (!allocateWorkspaceLocked_()) {
      lastError_ = ScaleProfilerError::ALLOCATION;
      return ScaleProfilerWork::NONE;
    }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    const bool flashTraceExists =
        savedRecordCount_ != 0 || state_ == ScaleProfilerState::SAVED;
    if (flashTraceExists) {
      // Durably drop the previous trace before producing a new one.
      persistence_ = ScaleProfilerPersistence::INVALIDATING;
      pendingStartAfterInvalidate_ = true;
      flashPhase_ = FlashPhase::INVALIDATE;
      flashGeneration_ = generation_;
      flashInFlight_ = false;
      pendingWork_ = ScaleProfilerWork::INVALIDATE;
      state_ = ScaleProfilerState::PREPARING;
      return ScaleProfilerWork::INVALIDATE;
    }
#endif
    savedRecordCount_ = 0;
    persistence_ = ScaleProfilerPersistence::NONE;
    beginRecordingLocked_(nowMs);
    return ScaleProfilerWork::NONE;
  }

  ScaleProfilerWork beginDeleteLocked_() {
    if (state_ != ScaleProfilerState::STOPPED && state_ != ScaleProfilerState::SAVED) {
      return ScaleProfilerWork::NONE;
    }
    // Mirrors the UI matrix: never delete while a save is pending, active,
    // invalidating, or while a download pins the frozen trace.
    if (downloadLease_.load(std::memory_order_relaxed) != 0 || flashInFlight_ ||
        persistence_ == ScaleProfilerPersistence::SAVING ||
        persistence_ == ScaleProfilerPersistence::PENDING_SAVE ||
        persistence_ == ScaleProfilerPersistence::INVALIDATING) {
      lastError_ = ScaleProfilerError::BUSY;
      return ScaleProfilerWork::NONE;
    }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    const bool flashTraceExists =
        savedRecordCount_ != 0 || state_ == ScaleProfilerState::SAVED;
    if (flashTraceExists) {
      persistence_ = ScaleProfilerPersistence::INVALIDATING;
      flashPhase_ = FlashPhase::INVALIDATE;
      flashGeneration_ = generation_;
      flashInFlight_ = false;
      pendingWork_ = ScaleProfilerWork::INVALIDATE;
      state_ = ScaleProfilerState::PREPARING;
      return ScaleProfilerWork::INVALIDATE;
    }
#endif
    persistence_ = ScaleProfilerPersistence::NONE;
    stopReason_ = ScaleProfilerStopReason::NONE;
    lastError_ = ScaleProfilerError::NONE;
    releaseWorkspaceLocked_();
    state_ = ScaleProfilerState::EMPTY;
    return ScaleProfilerWork::NONE;
  }

  void beginRecordingLocked_(uint32_t nowMs) {
    // Construct in place: a 256 KiB Workspace{} temporary must never reach the
    // stack (records carry floats, so memset is not used either).
    new (workspace_) Workspace();
    uint32_t utcStartSec = 0;
    if (contextProvider_ != nullptr) {
      utcStartSec =
          contextProvider_(workspace_->header.context, SCALE_PROFILE_CONTEXT_BYTES);
    }
    ++generation_;
    if (generation_ == 0) generation_ = 1;
    ++sessionId_;
    if (sessionId_ == 0) sessionId_ = 1;
    recordCount_ = 0;
    weightCount_ = 0;
    eventCount_ = 0;
    ordinal_ = 0;
    durationMs_ = 0;
    indexReady_ = false;
    stopReason_ = ScaleProfilerStopReason::NONE;
    lastError_ = ScaleProfilerError::NONE;
    saveRetries_ = 0;
    elapsedMs_ = 0;
    clockAtMs_ = nowMs;
    estimateAtMs_ = nowMs;
    estimateStartMs_ = nowMs;
    estimateCount_ = estimateStartCount_ = 0;
    lastProgressMs_ = nowMs;
    smoothedRecordsPerSecond_ = 0;
    estimatedRemainingMs_ = UINT32_MAX;
    estimateBaselineReady_ = false;
    capacityStopRequested_ = false;
    state_ = ScaleProfilerState::RECORDING;
    ScaleProfileRecord start;
    start.kind = static_cast<uint16_t>(ScaleProfileEvent::PROFILE_START);
    appendLocked_(start, nowMs);
    ScaleProfileHeader &header = workspace_->header;
    header.magic = SCALE_PROFILE_MAGIC;
    header.schemaVersion = SCALE_PROFILE_SCHEMA_VERSION;
    header.headerBytes = sizeof(ScaleProfileHeader);
    header.generation = generation_;
    header.sessionId = sessionId_;
    header.startWallUtcSec = utcStartSec;
  }

  void stopLocked_(ScaleProfilerStopReason reason, uint32_t nowMs) {
    if (state_ != ScaleProfilerState::RECORDING) return;
    ScaleProfileRecord terminal;
    terminal.kind = static_cast<uint16_t>(ScaleProfileEvent::PROFILE_STOP);
    terminal.arg1 = static_cast<uint32_t>(reason);
    durationMs_ = elapsedAt_(nowMs);
    terminal.ordinal = ++ordinal_ |
        (static_cast<uint32_t>(durationMs_ >> 32) << SCALE_PROFILE_ORDINAL_BITS);
    terminal.relativeMs = static_cast<uint32_t>(durationMs_);
    // The reserved terminal slot is only writable here, after producers can
    // no longer be admitted.
    if (recordCount_ < SCALE_PROFILE_RECORD_CAPACITY) {
      records_[recordCount_++] = terminal;
      ++eventCount_;
    }
    stopReason_ = reason;
    persistence_ = ScaleProfilerPersistence::PENDING_SAVE;
    indexReady_ = false;
    state_ = ScaleProfilerState::STOPPED;
    // Keep the RAM header download-ready with the final counters.
    workspace_->header.recordCount = recordCount_;
    workspace_->header.payloadBytes =
        recordCount_ * SCALE_PROFILE_RECORD_BYTES;
    workspace_->header.durationMs = static_cast<uint32_t>(durationMs_);
    workspace_->header.durationMsHigh = static_cast<uint32_t>(durationMs_ >> 32);
    workspace_->header.stopReason = static_cast<uint32_t>(reason);
    workspace_->header.lostCount = lostCount_;
    workspace_->header.weightCount = weightCount_;
    workspace_->header.eventCount = eventCount_;
  }

  void serviceRecordingLocked_(uint32_t nowMs) {
    if (state_ != ScaleProfilerState::RECORDING) return;
    if (capacityStopRequested_) {
      stopLocked_(ScaleProfilerStopReason::BUFFER_FULL, nowMs);
      return;
    }
    // The first service tick excludes the initial context/state burst.
    if (!estimateBaselineReady_) {
      estimateBaselineReady_ = true;
      estimateAtMs_ = estimateStartMs_ = lastProgressMs_ = nowMs;
      estimateCount_ = estimateStartCount_ = recordCount_;
      return;
    }
    const uint32_t dtMs = nowMs - estimateAtMs_;
    if (dtMs < 1000) return;
    const uint32_t added = recordCount_ - estimateCount_;
    estimateAtMs_ = nowMs;
    estimateCount_ = recordCount_;
    if (added != 0) lastProgressMs_ = nowMs;
    if (static_cast<uint32_t>(nowMs - lastProgressMs_) >= 5000) {
      smoothedRecordsPerSecond_ = 0;
      estimatedRemainingMs_ = UINT32_MAX;
      estimateStartMs_ = nowMs;
      estimateStartCount_ = recordCount_;
      return;
    }
    if (smoothedRecordsPerSecond_ == 0) {
      const uint32_t warmupMs = nowMs - estimateStartMs_;
      const uint32_t warmupCount = recordCount_ - estimateStartCount_;
      if (warmupMs < 5000 || warmupCount < 10) return;
      smoothedRecordsPerSecond_ = 1000.0 * warmupCount / warmupMs;
    } else {
      const double alpha = -std::expm1(-static_cast<double>(dtMs) / 5000.0);
      smoothedRecordsPerSecond_ += alpha *
          (1000.0 * added / dtMs - smoothedRecordsPerSecond_);
    }
    const double remainingMs = 1000.0 *
        (SCALE_PROFILE_ORDINARY_CAPACITY - recordCount_) / smoothedRecordsPerSecond_;
    estimatedRemainingMs_ = std::isfinite(remainingMs) && remainingMs < UINT32_MAX
                                ? static_cast<uint32_t>(std::ceil(remainingMs))
                                : UINT32_MAX;
  }

  int64_t elapsedAt_(uint32_t nowMs) const {
    // Readers/producers may have sampled millis just before the owner tick.
    return static_cast<int64_t>(elapsedMs_) +
           static_cast<int32_t>(nowMs - clockAtMs_);
  }

  ScaleProfilerWork servicePersistenceLocked_(uint32_t nowMs,
                                              ScaleProfilerWork work) {
    if (state_ == ScaleProfilerState::SAVED &&
        persistence_ == ScaleProfilerPersistence::SAVED &&
        downloadLease_.load(std::memory_order_relaxed) == 0 &&
        workspace_ != nullptr) {
      releaseWorkspaceLocked_();  // Saved and not streaming: reclaim PSRAM.
    }
    if (state_ == ScaleProfilerState::STOPPED && !indexReady_ &&
        records_ != nullptr) {
      if (index_ == nullptr) {
        index_ = static_cast<uint32_t *>(
            allocExternal(SCALE_PROFILE_INDEX_BYTES, AllocationOwner::PROFILER));
      }
      if (index_ != nullptr) {
        buildIndexLocked_();
        indexReady_ = true;
        if (lastError_ == ScaleProfilerError::INDEX_FAILED) {
          lastError_ = ScaleProfilerError::NONE;
        }
      } else {
        lastError_ = ScaleProfilerError::INDEX_FAILED;
      }
    }
    if (work == ScaleProfilerWork::NONE &&
        pendingWork_ == ScaleProfilerWork::NONE && !flashInFlight_ &&
        state_ == ScaleProfilerState::STOPPED && indexReady_ &&
        persistence_ == ScaleProfilerPersistence::PENDING_SAVE) {
      flashSave_ = FlashSaveState{};
      flashSave_.recordCount = recordCount_;
      flashSave_.payloadBytes = recordCount_ * SCALE_PROFILE_RECORD_BYTES;
      flashPhase_ = FlashPhase::SAVE_ERASE;
      flashGeneration_ = generation_;
      persistence_ = ScaleProfilerPersistence::SAVING;
      pendingWork_ = ScaleProfilerWork::SAVE;
      work = ScaleProfilerWork::SAVE;
    } else if (work == ScaleProfilerWork::NONE &&
               pendingWork_ == ScaleProfilerWork::NONE &&
               persistence_ == ScaleProfilerPersistence::FAILED &&
               state_ == ScaleProfilerState::STOPPED && !flashInFlight_ &&
               saveRetries_ < SCALE_PROFILE_SAVE_MAX_RETRIES &&
               static_cast<int32_t>(nowMs - saveRetryAtMs_) >= 0) {
      persistence_ = ScaleProfilerPersistence::PENDING_SAVE;
    }
    if (work != ScaleProfilerWork::NONE) flashInFlight_ = true;
    return work;
  }

  void buildIndexLocked_() {
    for (size_t i = 0; i < recordCount_; ++i) index_[i] = static_cast<uint32_t>(i);
    const ScaleProfileRecord *records = records_;
    std::sort(index_, index_ + recordCount_,
              [records](uint32_t a, uint32_t b) {
                const uint64_t at = scaleProfileRecordTimeMs(records[a]);
                const uint64_t bt = scaleProfileRecordTimeMs(records[b]);
                if (at != bt) {
                  return at < bt;
                }
                return records[a].ordinal < records[b].ordinal;
              });
  }

  // Reads are safe without the capture mutex: save work only runs while the
  // frozen STOPPED generation is pinned, and Start/Delete cannot replace it
  // while the save is in flight.
  size_t gatherChunk_(uint8_t *out, size_t startIndex) const {
    if (startIndex >= flashSave_.recordCount) return 0;
    const size_t count =
        std::min(FLASH_IO_CHUNK_BYTES / SCALE_PROFILE_RECORD_BYTES,
                 static_cast<size_t>(flashSave_.recordCount - startIndex));
    for (size_t i = 0; i < count; ++i) {
      memcpy(out + i * SCALE_PROFILE_RECORD_BYTES, &records_[index_[startIndex + i]],
             SCALE_PROFILE_RECORD_BYTES);
    }
    return count;
  }

  void buildSaveHeader_(ScaleProfileHeader &header) const {
    header = workspace_->header;
    header.recordCount = flashSave_.recordCount;
    header.payloadBytes = flashSave_.payloadBytes;
    header.durationMs = static_cast<uint32_t>(durationMs_);
    header.durationMsHigh = static_cast<uint32_t>(durationMs_ >> 32);
    header.stopReason = static_cast<uint32_t>(stopReason_);
    header.lostCount = lostCount_;
    header.weightCount = weightCount_;
    header.eventCount = eventCount_;
    header.checksum = flashSave_.checksum;
  }

  void adoptSavedHeaderLocked_(const ScaleProfileHeader &header) {
    state_ = ScaleProfilerState::SAVED;
    persistence_ = ScaleProfilerPersistence::SAVED;
    savedRecordCount_ = header.recordCount;
    sessionId_ = header.sessionId;
    durationMs_ = scaleProfileDurationMs(header);
    stopReason_ = static_cast<ScaleProfilerStopReason>(header.stopReason);
    lostCount_ = header.lostCount;
    weightCount_ = header.weightCount;
    eventCount_ = header.eventCount;
    ++generation_;
  }

  bool allocateWorkspaceLocked_() {
    if (workspace_ != nullptr) return true;
    void *block = allocExternal(sizeof(Workspace), AllocationOwner::PROFILER);
    if (block == nullptr) return false;
    workspace_ = static_cast<Workspace *>(block);
    records_ = workspace_->records;
    new (workspace_) Workspace();
    return true;
  }

  void releaseWorkspaceLocked_() {
    if (workspace_ != nullptr) {
      heapCapsFree(workspace_);
      workspace_ = nullptr;
      records_ = nullptr;
    }
    if (index_ != nullptr) {
      heapCapsFree(index_);
      index_ = nullptr;
    }
    indexReady_ = false;
  }

  static void copyCString(char *destination, size_t capacity,
                          const char *source) {
    if (destination == nullptr || capacity == 0) return;
    destination[0] = '\0';
    if (source == nullptr) return;
    strncat(destination, source, capacity - 1);
  }

  mutable TaskMutex captureMux_;
  mutable TaskMutex resultMux_;
  std::atomic<ScaleProfilerState> stateAtomic_{ScaleProfilerState::EMPTY};
  std::atomic<uint32_t> downloadLease_{0};
  ScaleProfileContextProvider contextProvider_ = nullptr;
  Workspace *workspace_ = nullptr;
  ScaleProfileRecord *records_ = nullptr;
  uint32_t *index_ = nullptr;
  ScaleProfilerState state_ = ScaleProfilerState::EMPTY;
  ScaleProfilerPersistence persistence_ = ScaleProfilerPersistence::NONE;
  ScaleProfilerStopReason stopReason_ = ScaleProfilerStopReason::NONE;
  ScaleProfilerError lastError_ = ScaleProfilerError::NONE;
  FlashPhase flashPhase_ = FlashPhase::IDLE;
  FlashSaveState flashSave_;
  uint32_t flashGeneration_ = 0;
  bool flashInFlight_ = false;
  bool pendingStartAfterInvalidate_ = false;
  bool indexReady_ = false;
  bool partitionAvailable_ = false;
  ScaleProfilerWork pendingWork_ = ScaleProfilerWork::NONE;
  bool workDispatched_ = false;
  bool capacityStopRequested_ = false;
  uint32_t generation_ = 0;
  uint32_t sessionId_ = 0;
  uint32_t recordCount_ = 0;
  uint32_t savedRecordCount_ = 0;
  uint32_t weightCount_ = 0;
  uint32_t eventCount_ = 0;
  uint32_t lostCount_ = 0;
  uint32_t ordinal_ = 0;
  uint64_t durationMs_ = 0;
  uint64_t elapsedMs_ = 0;
  uint32_t clockAtMs_ = 0;
  uint32_t estimateAtMs_ = 0;
  uint32_t estimateStartMs_ = 0;
  uint32_t estimateCount_ = 0;
  uint32_t estimateStartCount_ = 0;
  uint32_t lastProgressMs_ = 0;
  uint32_t estimatedRemainingMs_ = UINT32_MAX;
  double smoothedRecordsPerSecond_ = 0;
  bool estimateBaselineReady_ = false;
  uint32_t saveRetryAtMs_ = 0;
  uint8_t saveRetries_ = 0;
  bool resultReady_ = false;
  bool resultOk_ = false;
  uint32_t resultGeneration_ = 0;
};

// Shared instance: an inline function-local static has one object across the
// scale-worker and orchestrator translation units.
inline ScaleProfiler &scaleProfiler() {
  static ScaleProfiler instance;
  return instance;
}

// Producer entry points. Task context only; never inside a spinlock or an
// ISR/NimBLE callback; never while holding a scale, debug or relay lock.
inline void scaleProfileNoteWeight(float weightG, uint32_t receivedAtMs,
                                   uint32_t connectionGeneration,
                                   uint32_t captureSequence,
                                   bool discontinuity, uint32_t nowMs) {
  scaleProfiler().noteWeight(weightG, receivedAtMs, connectionGeneration,
                             captureSequence, discontinuity, nowMs);
}

// Shot trace sinks (OpenBrewByWeightShotTrace.h), defined once in the control
// unit. They run whether or not a manual capture is active and never nest the
// capture lock.
void shotTraceNoteProfileEvent(ScaleProfileEvent kind, uint32_t nowMs, float weightG,
                               uint32_t arg1, uint32_t arg2);
void shotTraceNoteDroppedFrame(bool stale);

inline void scaleProfileNoteEvent(ScaleProfileEvent kind, uint32_t nowMs,
                                  uint32_t connectionGeneration,
                                  uint32_t sequence, float weightG, uint32_t arg1,
                                  uint32_t arg2, uint16_t flags = 0) {
  shotTraceNoteProfileEvent(kind, nowMs, weightG, arg1, arg2);
  scaleProfiler().noteEvent(kind, nowMs, connectionGeneration, sequence, weightG,
                            arg1, arg2, flags);
}

inline void scaleProfileNoteDroppedFrame(bool stale, bool decoded,
                                         float weightG, uint32_t receivedAtMs,
                                         uint32_t connectionGeneration,
                                         uint32_t captureSequence,
                                         uint16_t length, uint32_t nowMs) {
  shotTraceNoteDroppedFrame(stale);
  scaleProfiler().noteDroppedFrame(stale, decoded, weightG, receivedAtMs,
                                   connectionGeneration, captureSequence,
                                   length, nowMs);
}

// Formats one TXT data row in the requested shape:
//   +SSSSSS.mmm – weight|NAME key=value...
// The weight round-trips the captured float32 (9 significant digits); events
// without a measured weight print an em dash instead of a stale reading.
inline void formatScaleProfileValue(char *out, size_t capacity,
                                   ScaleProfileSignal signal, uint32_t value) {
  const char *name = nullptr;
  switch (signal) {
    case ScaleProfileSignal::IDLE_STATUS:
      name = value < 256 ? idleTarePresentationName(static_cast<uint8_t>(value)) : "unknown";
      break;
    case ScaleProfileSignal::IDLE_COMMAND_PHASE: {
      static const char *const phases[] = {"none", "queued", "writing", "succeeded", "failed"};
      name = value < 5 ? phases[value] : "unknown";
      break;
    }
    case ScaleProfileSignal::FIRST_FLOW_PHASE:
      name = value == 0 ? "seeking" : value == 1 ? "touch" : "unknown";
      break;
    case ScaleProfileSignal::IDLE_ELIGIBILITY:
    case ScaleProfileSignal::IDLE_LAST_REASON:
    case ScaleProfileSignal::IDLE_DEFERRED_REASON:
      name = value <= static_cast<uint32_t>(IdleTareReason::SLOT_BUSY)
          ? idleTareReasonName(static_cast<uint8_t>(value)) : "unknown";
      break;
    case ScaleProfileSignal::WEIGHT_CONTROL:
      name = weightControlStateName(static_cast<WeightControlState>(value));
      break;
    case ScaleProfileSignal::STREAM:
      name = weightStreamStateName(static_cast<WeightStreamState>(value));
      break;
    case ScaleProfileSignal::TOUCH_PHASE:
      name = accidentalTouchPhaseName(static_cast<AccidentalTouchPhase>(value));
      break;
    case ScaleProfileSignal::TOUCH_CLASS:
      name = accidentalTouchClassName(static_cast<AccidentalTouchClass>(value));
      break;
    case ScaleProfileSignal::EMPTY_ANCHOR_CG:
    case ScaleProfileSignal::OCCUPIED_REFERENCE_CG:
    case ScaleProfileSignal::REMOVED_CG:
      if (value == static_cast<uint32_t>(INT32_MIN)) name = "unknown";
      else {
        snprintf(out, capacity, "%ld", static_cast<long>(static_cast<int32_t>(value)));
        return;
      }
      break;
    default: break;
  }
  if (name != nullptr) snprintf(out, capacity, "%s", name);
  else snprintf(out, capacity, "%lu", static_cast<unsigned long>(value));
}

inline size_t formatScaleProfileRow(char *out, size_t capacity,
                                    const ScaleProfileRecord &record) {
  if (out == nullptr || capacity == 0) return 0;
  const uint64_t atMs = scaleProfileRecordTimeMs(record);
  const uint64_t seconds = atMs / 1000U;
  const uint32_t millis = static_cast<uint32_t>(atMs % 1000U);
  size_t used = static_cast<size_t>(
      snprintf(out, capacity, "+%06llu.%03lu \xE2\x80\x93 ",
               static_cast<unsigned long long>(seconds),
               static_cast<unsigned long>(millis)));
  if (used >= capacity) return 0;
  if (std::isfinite(record.weightG)) {
    used += static_cast<size_t>(snprintf(out + used, capacity - used, "%.9g",
                                         record.weightG));
  } else {
    used += static_cast<size_t>(
        snprintf(out + used, capacity - used, "\xE2\x80\x94"));
  }
  if (used >= capacity) return 0;
  used += static_cast<size_t>(
      snprintf(out + used, capacity - used, "|%s",
               scaleProfileEventName(static_cast<ScaleProfileEvent>(record.kind))));
  if (used >= capacity) return 0;
  char suffix[160] = {};
  switch (static_cast<ScaleProfileEvent>(record.kind)) {
    case ScaleProfileEvent::STATE_CHANGED: {
      const auto signal = static_cast<ScaleProfileSignal>(record.flags & ~SCALE_PROFILE_INITIAL);
      char before[24], after[24];
      formatScaleProfileValue(before, sizeof(before), signal, record.arg1);
      formatScaleProfileValue(after, sizeof(after), signal, record.arg2);
      snprintf(suffix, sizeof(suffix), " %s from=%s to=%s capture=%lu connection=%lu",
               scaleProfileSignalName(static_cast<uint16_t>(signal)),
               (record.flags & SCALE_PROFILE_INITIAL) ? "initial" : before, after,
               static_cast<unsigned long>(record.sequence),
               static_cast<unsigned long>(record.connectionGeneration));
      break;
    }
    case ScaleProfileEvent::TARE_PHASE: {
      static const char *const phases[] = {"none", "queued", "writing", "succeeded", "failed"};
      const uint32_t phase = record.arg2 & 0xff;
      snprintf(suffix, sizeof(suffix), " idleRequest=%lu phase=%s reason=%s boundary=%lu connection=%lu",
               static_cast<unsigned long>(record.arg1),
               phase < 5 ? phases[phase] : "unknown",
               idleTareReasonName(static_cast<uint8_t>(record.arg2 >> 8)),
               static_cast<unsigned long>(record.sequence),
               static_cast<unsigned long>(record.connectionGeneration));
      break;
    }
    case ScaleProfileEvent::TARE_REJECTED:
    case ScaleProfileEvent::CUP_QUALIFICATION_RESET:
      snprintf(suffix, sizeof(suffix), " context=%lu reason=%s pkt=%lu",
               static_cast<unsigned long>(record.arg1),
               idleTareReasonName(static_cast<uint8_t>(record.arg2)),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::CUP_ANCHOR_REJECTED:
      snprintf(suffix, sizeof(suffix), " placement=%lu offset_cg=%ld pkt=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<long>(static_cast<int32_t>(record.arg2)),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::CUP_RESET:
      snprintf(suffix, sizeof(suffix), " placement=%lu reason=logical_reset",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::FINALIZE_CANCELLED:
      snprintf(suffix, sizeof(suffix), " cycle=%lu reason=%s",
               static_cast<unsigned long>(record.arg1),
               record.arg2 == 1 ? "cup_continuity" : record.arg2 == 2 ? "new_cycle"
                   : record.arg2 == 3 ? "rinse" : "unknown");
      break;
    case ScaleProfileEvent::WEIGHT:
      snprintf(suffix, sizeof(suffix), " seq=%lu connection=%lu%s",
               static_cast<unsigned long>(record.sequence),
               static_cast<unsigned long>(record.connectionGeneration),
               (record.flags & SCALE_PROFILE_FLAG_DISCONTINUITY) ? " disc=1" : "");
      break;
    case ScaleProfileEvent::PROFILE_START:
      snprintf(suffix, sizeof(suffix), " limit=capacity");
      break;
    case ScaleProfileEvent::PROFILE_STOP:
      snprintf(suffix, sizeof(suffix), " reason=%s",
               scaleProfilerStopReasonName(
                   static_cast<ScaleProfilerStopReason>(record.arg1)));
      break;
    case ScaleProfileEvent::SCALE_CONNECTED:
    case ScaleProfileEvent::SCALE_DISCONNECTED:
      snprintf(suffix, sizeof(suffix), " connection=%lu reason=%lu",
               static_cast<unsigned long>(record.connectionGeneration),
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::STALE_FRAME:
    case ScaleProfileEvent::FRAME_UNDECODABLE:
      snprintf(suffix, sizeof(suffix), " seq=%lu len=%lu%s",
               static_cast<unsigned long>(record.sequence),
               static_cast<unsigned long>(record.arg1),
               (record.flags & SCALE_PROFILE_FLAG_DECODED) ? " decoded=1" : "");
      break;
    case ScaleProfileEvent::CONTROL_FIFO_LOSS:
    case ScaleProfileEvent::EVENT_DROPPED:
      snprintf(suffix, sizeof(suffix), " count=%lu",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::PACKET_GAP:
      snprintf(suffix, sizeof(suffix), " gapMs=%lu pkt=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::TARE_REQUEST:
      snprintf(suffix, sizeof(suffix), " request=%lu origin=%lu seq=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.arg2),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::TARE_WRITE_OK:
      snprintf(suffix, sizeof(suffix), " request=%lu boundary=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::TARE_WRITE_FAILED:
      snprintf(suffix, sizeof(suffix), " request=%lu",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::TARE_BASELINE_CONFIRMED:
      snprintf(suffix, sizeof(suffix), " request=%lu seq=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::TARE_BASELINE_TIMEOUT:
    case ScaleProfileEvent::TARE_RELEASED:
      snprintf(suffix, sizeof(suffix), " request=%lu reason=%s",
               static_cast<unsigned long>(record.arg1),
               idleTareReasonName(static_cast<uint8_t>(record.arg2)));
      break;
    case ScaleProfileEvent::CUP_PLACED:
    case ScaleProfileEvent::CUP_REMOVED:
      snprintf(suffix, sizeof(suffix), " placement=%lu pkt=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.sequence));
      break;
    case ScaleProfileEvent::CUP_ANCHOR_CHANGED:
      snprintf(suffix, sizeof(suffix), " placement=%lu anchorCg=%ld",
               static_cast<unsigned long>(record.arg1),
               static_cast<long>(static_cast<int32_t>(record.arg2)));
      break;
    case ScaleProfileEvent::WEIGHT_ACCEPTED:
      snprintf(suffix, sizeof(suffix), " seq=%lu pkt=%lu connection=%lu",
               static_cast<unsigned long>(record.sequence),
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.connectionGeneration));
      break;
    case ScaleProfileEvent::WEIGHT_REJECTED:
      snprintf(suffix, sizeof(suffix), " code=%lu refCg=%ld seq=%lu connection=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<long>(static_cast<int32_t>(record.arg2)),
               static_cast<unsigned long>(record.sequence),
               static_cast<unsigned long>(record.connectionGeneration));
      break;
    case ScaleProfileEvent::FIRST_DROP:
      snprintf(suffix, sizeof(suffix), " shotMs=%lu",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::TOUCH_HOLD:
    case ScaleProfileEvent::TOUCH_RELEASE:
    case ScaleProfileEvent::TOUCH_SUSTAINED:
      snprintf(suffix, sizeof(suffix), " class=%lu",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::SHOT_START:
      snprintf(suffix, sizeof(suffix), " cycle=%lu goalCg=%ld",
               static_cast<unsigned long>(record.arg1),
               static_cast<long>(static_cast<int32_t>(record.arg2)));
      break;
    case ScaleProfileEvent::SHOT_END:
      snprintf(suffix, sizeof(suffix), " cycle=%lu reason=%lu",
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.arg2));
      break;
    case ScaleProfileEvent::CONTROL_SUSPENDED:
    case ScaleProfileEvent::CONTROL_RECOVERED:
      snprintf(suffix, sizeof(suffix), " state=%lu",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::SETTINGS_CHANGED:
      snprintf(suffix, sizeof(suffix), " revision=%lu",
               static_cast<unsigned long>(record.arg1));
      break;
    case ScaleProfileEvent::REFERENCE_CHANGED:
      snprintf(suffix, sizeof(suffix), " connection=%lu write=%lu",
               static_cast<unsigned long>(record.connectionGeneration),
               static_cast<unsigned long>(record.arg1));
      break;
    default:
      snprintf(suffix, sizeof(suffix), " kind=%u arg1=%lu arg2=%lu", record.kind,
               static_cast<unsigned long>(record.arg1),
               static_cast<unsigned long>(record.arg2));
      break;
  }
  const size_t suffixLength = strlen(suffix);
  if (used + suffixLength + 1 > capacity) return 0;
  memcpy(out + used, suffix, suffixLength + 1);
  return used + suffixLength;
}

}  // namespace openbrewbyweight

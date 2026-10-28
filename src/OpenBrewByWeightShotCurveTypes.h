#pragma once

// Accepted extraction/drip observations with reception times and event annotations,
// separate from the scalar ShotLogRecord; saved in the shot's pool record.

#include "OpenBrewByWeightShotCurveRecord.h"
#include "OpenBrewByWeightShotLogTypes.h"

#include <cmath>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace openbrewbyweight {

// Curves the PSRAM cache keeps, the newest; older ones are read from flash.
constexpr size_t SHOT_CURVE_CAPACITY = 500;
static_assert(SHOT_CURVE_CAPACITY <= SHOT_LOG_CAPACITY, "Cached curves have summaries");
// A curve lookup: BUSY when it must come from flash and flash may not be
// read now (a shot runs, the pool is not recovered yet, a read failed);
// STORED when the caller asked not to read flash and the curve may be there.
enum class ShotCurveRead : uint8_t { NONE, FOUND, BUSY, STORED };
// A per-shot trace download (docs/SHOT_TRACE.md): BUSY while a shot runs,
// PENDING while its record waits to be written, UNAVAILABLE when the pool
// failed or a read failed, NOT_MODIFIED when the caller's copy is current.
enum class ShotTraceRead : uint8_t { FOUND, NONE, BUSY, PENDING, UNAVAILABLE, NOT_MODIFIED };
// Called with each chunk and the record CRC (the response's ETag).
using ShotTraceSink = bool (*)(void *context, const uint8_t *data, size_t bytes, uint32_t crc);
// Diagnostic Shot storage: the pool in bytes (pool = live + reclaimable +
// unindexed + never written), its records (good and bad summaries, trace
// only, interrupted), the oldest stored shot, and what still waits.
struct ShotStorageStats {
  bool ready;
  uint8_t lastError;  // ShotTracePoolError
  uint32_t poolBytes, liveBytes, reclaimableBytes, unindexedBytes, unusedBytes;
  uint32_t records, meanRecordBytes, droppedRecords;
  uint32_t good, bad, traceOnly, interrupted, traces, truncated;
  uint32_t sinceShotId, sinceLocalSec;  // 0: none, or no wall time
  uint32_t pendingCurves, pendingTraces, tracesDropped;
  // Traces without a shot, newest first: interrupted (end 0) or refused by
  // the shot log. The first orphansListed are valid; traceOnly + interrupted
  // counts them all.
  struct {
    uint32_t bootId, endedAtMs;
  } orphans[16];
  uint8_t orphansListed;
};
// Worst case: the base64 stream plus 1201 break and 1201 held indices of up
// to five characters each, plus the reference point, keys and events.
constexpr size_t SHOT_CURVE_JSON_CAPACITY =
    4U * ((SHOT_CURVE_STREAM_BYTES + 2U) / 3U) + 10U * SHOT_CURVE_MAX_POINTS + 512U;

inline ShotCurveEvent missingShotCurveEvent() {
  ShotCurveEvent event = {};
  event.atMs = SHOT_LOG_METRIC_MISSING;
  event.weightCg = SHOT_LOG_WEIGHT_MISSING;
  return event;
}

inline bool shotCurveEventPresent(const ShotCurveEvent &event) {
  return event.atMs != SHOT_LOG_METRIC_MISSING &&
         !shotLogWeightIsMissing(event.weightCg);
}

// Network-owned PSRAM copy, captured with one store lock; never a stack
// local. Records only, newest first: the streaming Stats page sorts,
// filters, and aggregates its selected window from these, and each row's
// curve is fetched on demand (curves are immutable once committed), so the
// ~5 KB-per-shot curve bulk never rides the snapshot copy.
struct ShotStatsRecordsSnapshot {
  ShotLogRecord records[SHOT_LOG_CAPACITY]{};
  size_t count = 0;
  uint32_t epoch = 0;
};

inline void resetShotCurveRecord(ShotCurveRecord &curve) {
  // Only the header: the stream and bitmaps are read up to bytes/count.
  memset(&curve, 0, offsetof(ShotCurveRecord, stream));
  curve.atmClearedMs = SHOT_LOG_METRIC_MISSING;
  curve.firstDrop = missingShotCurveEvent();
  curve.extended = missingShotCurveEvent();
  curve.atm = missingShotCurveEvent();
  curve.ended = missingShotCurveEvent();
}

inline ShotCurveRecord emptyShotCurveRecord() {
  ShotCurveRecord curve{};
  resetShotCurveRecord(curve);
  return curve;
}

inline void copyShotCurve(ShotCurveRecord &out, const ShotCurveRecord &in) {
  if (&out == &in) return;
  memcpy(&out, &in, offsetof(ShotCurveRecord, stream));
  memcpy(out.stream, in.stream,
         in.bytes < SHOT_CURVE_STREAM_BYTES ? in.bytes : SHOT_CURVE_STREAM_BYTES);
  const size_t bits = in.count < SHOT_CURVE_MAX_POINTS ? (in.count + 7U) / 8U
                                                       : SHOT_CURVE_BREAK_BYTES;
  memcpy(out.breakBefore, in.breakBefore, bits);
  memcpy(out.held, in.held, bits);
}

// Sequential walk of a delta stream. The previous point starts at the
// reference (0/0 for a whole curve): one byte per step, or a marker (0xFF
// time, 0x80 weight) plus the absolute little-endian value. Corrupt input
// wraps modulo 16 bits; validShotCurveRecord rejects it.
struct ShotCurveReader {
  const uint8_t *data = nullptr;
  size_t size = 0, offset = 0;
  uint16_t atMs = 0;
  int16_t weightCg = 0;

  bool next() {
    if (offset >= size) return false;
    uint8_t byte = data[offset++];
    if (byte == 0xFF) {
      if (size - offset < 2U) return false;
      atMs = static_cast<uint16_t>(data[offset] | (data[offset + 1U] << 8));
      offset += 2U;
    } else {
      atMs = static_cast<uint16_t>(atMs + byte);
    }
    if (offset >= size) return false;
    byte = data[offset++];
    if (byte == 0x80) {
      if (size - offset < 2U) return false;
      weightCg = static_cast<int16_t>(static_cast<uint16_t>(data[offset] | (data[offset + 1U] << 8)));
      offset += 2U;
    } else {
      weightCg = static_cast<int16_t>(static_cast<uint16_t>(weightCg + static_cast<int8_t>(byte)));
    }
    return true;
  }
};

// A reader positioned after `index` points (false past the stream).
inline bool shotCurveSeek(const ShotCurveRecord &curve, uint16_t index, ShotCurveReader &reader) {
  reader = ShotCurveReader{};
  reader.data = curve.stream;
  reader.size = curve.bytes < SHOT_CURVE_STREAM_BYTES ? curve.bytes : SHOT_CURVE_STREAM_BYTES;
  for (uint16_t i = 0; i < index; ++i)
    if (!reader.next()) return false;
  return true;
}

// The PSRAM ring's bookkeeping.
struct ShotCurveHeader {
  uint16_t count;
  uint16_t writeIndex;
};

struct ShotCurveStore {
  ShotCurveHeader header;
  ShotCurveRecord records[SHOT_CURVE_CAPACITY];
};

static_assert(sizeof(ShotCurveStore) ==
                  sizeof(ShotCurveHeader) +
                      sizeof(ShotCurveRecord) * SHOT_CURVE_CAPACITY,
              "ShotCurveStore packing must stay unpadded");
static_assert(sizeof(ShotCurveStore) == 2804004,
              "ShotCurveStore size pins the PSRAM curve-store footprint");

// Records are read only below the count, so emptying the ring is the header.
inline void resetShotCurveStore(ShotCurveStore &store) {
  store.header = ShotCurveHeader{};
}

// Pack the ring into records[0..count) (oldest first) in place, mirroring
// compactShotLogStore.
inline void compactShotCurveStore(ShotCurveStore &store) {
  store.header.writeIndex = compactRecordRing(
      store.records, SHOT_CURVE_CAPACITY, sizeof(ShotCurveRecord),
      store.header.count, store.header.writeIndex);
}

inline void setShotCurveBit(uint8_t *bits, size_t index, bool value) {
  const uint8_t mask = static_cast<uint8_t>(1U << (index % 8U));
  bits[index / 8U] = static_cast<uint8_t>(value ? bits[index / 8U] | mask
                                                : bits[index / 8U] & ~mask);
}

// Appends one point to the delta stream, relative to the newest point.
inline bool appendShotCurvePoint(ShotCurveRecord &curve, int16_t cg,
                                 uint32_t elapsed, bool split, bool held = false) {
  if (curve.truncated) return false;
  if (shotLogWeightIsMissing(cg) ||
      (curve.count != 0 && elapsed < curve.lastAtMs)) return false;
  const uint32_t dt = elapsed - (curve.count != 0 ? curve.lastAtMs : 0U);
  const int dw = cg - (curve.count != 0 ? curve.lastWeightCg : 0);
  const bool shortTime = dt <= 254U, shortWeight = dw >= -127 && dw <= 127;
  // The stream bound is the valid-record worst case; checking it here only
  // keeps a broken invariant from writing past the record.
  if (elapsed > SHOT_CURVE_MAX_TIME_MS || curve.count == SHOT_CURVE_MAX_POINTS ||
      curve.bytes + (shortTime ? 1U : 3U) + (shortWeight ? 1U : 3U) >
          SHOT_CURVE_STREAM_BYTES) {
    curve.truncated = true;
    return false;
  }
  uint8_t *p = curve.stream + curve.bytes;
  if (shortTime) {
    *p++ = static_cast<uint8_t>(dt);
  } else {
    *p++ = 0xFF;
    *p++ = static_cast<uint8_t>(elapsed);
    *p++ = static_cast<uint8_t>(elapsed >> 8);
  }
  if (shortWeight) {
    *p++ = static_cast<uint8_t>(static_cast<int8_t>(dw));
  } else {
    const uint16_t absolute = static_cast<uint16_t>(cg);
    *p++ = 0x80;
    *p++ = static_cast<uint8_t>(absolute);
    *p++ = static_cast<uint8_t>(absolute >> 8);
  }
  curve.bytes = static_cast<uint16_t>(p - curve.stream);
  setShotCurveBit(curve.breakBefore, curve.count, split && curve.count != 0);
  setShotCurveBit(curve.held, curve.count, held);
  curve.lastAtMs = static_cast<uint16_t>(elapsed);
  curve.lastWeightCg = cg;
  ++curve.count;
  return true;
}

inline bool appendShotCurveObservation(ShotCurveRecord &curve, float weight,
                                       uint32_t elapsed, bool split,
                                       bool held = false) {
  return appendShotCurvePoint(curve, shotLogWeightToCentigrams(weight), elapsed,
                              split, held);
}

// The live shot's curve, captured straight into record form.
struct ShotCurveSampler : ShotCurveRecord {
  uint32_t startMs = 0;
  bool pendingBreak = false;
  bool active = false;

  ShotCurveSampler() : ShotCurveRecord{} { resetShotCurveRecord(*this); }

  void reset(uint32_t startedAtMs) {
    resetShotCurveRecord(*this);
    startMs = startedAtMs;
    pendingBreak = false;
    active = true;
  }

  bool elapsedMsFrom(uint32_t atMs, uint16_t &outMs) const {
    if (!active) {
      return false;
    }
    if (static_cast<int32_t>(atMs - startMs) < 0) {
      return false;
    }
    const uint32_t elapsedMs = atMs - startMs;
    if (elapsedMs >= UINT16_MAX) {
      return false;
    }
    outMs = static_cast<uint16_t>(elapsedMs);
    return true;
  }

  void latchEvent(ShotCurveEvent &event, uint32_t atMs, float weight) {
    if (shotCurveEventPresent(event) || !active) {
      return;
    }
    uint16_t ms = 0;
    if (!elapsedMsFrom(atMs, ms)) {
      return;
    }
    const int16_t cg = shotLogWeightToCentigrams(weight);
    if (shotLogWeightIsMissing(cg)) {
      return;
    }
    event.atMs = ms;
    event.weightCg = cg;
  }

  void latchFirstDrop(uint32_t atMs, float weight) {
    latchEvent(firstDrop, atMs, weight);
  }

  void latchExtended(uint32_t atMs, float weight) {
    latchEvent(extended, atMs, weight);
  }

  void latchAtm(uint32_t atMs, float weight) {
    if (shotCurveEventPresent(atm) &&
        atmClearedMs == SHOT_LOG_METRIC_MISSING) {
      return;
    }
    atm = missingShotCurveEvent();
    atmClearedMs = SHOT_LOG_METRIC_MISSING;
    latchEvent(atm, atMs, weight);
  }

  void latchAtmCleared(uint32_t atMs) {
    if (!shotCurveEventPresent(atm) ||
        atmClearedMs != SHOT_LOG_METRIC_MISSING) {
      return;
    }
    uint16_t ms = 0;
    if (!elapsedMsFrom(atMs, ms)) {
      return;
    }
    atmClearedMs = ms;
  }

  void markBreak() { if (active) pendingBreak = true; }

  // A pending break waits for the next accepted sample: held samples sit
  // beside the weight line, never start a segment.
  void accept(float weight, uint32_t receivedAtMs, bool isHeld = false) {
    if (!active || truncated) return;
    const bool appended = appendShotCurveObservation(
        *this, weight, receivedAtMs - startMs, pendingBreak && !isHeld, isHeld);
    pendingBreak = isHeld ? pendingBreak || !appended : !appended;
  }

  void captureEnd(uint32_t at, float weight = NAN) {
    if (!active) return;
    if (!std::isfinite(weight) && count != 0)
      weight = static_cast<float>(lastWeightCg) / 100.0f;
    latchEvent(ended, at, weight);
    active = false;
  }

  void snapshot(ShotCurveRecord &record, uint32_t shotId = 0) const {
    copyShotCurve(record, *this);
    record.shotId = shotId;
  }
};

// Post-drip yield changes only the endpoint annotation, never an observation.
inline bool settleShotCurveEndWeight(ShotCurveRecord &curve, float weight) {
  const int16_t cg = shotLogWeightToCentigrams(weight);
  if (!shotCurveEventPresent(curve.ended) || shotLogWeightIsMissing(cg))
    return false;
  curve.ended.weightCg = cg;
  return true;
}

inline bool shotCurveBreakBefore(const ShotCurveRecord &curve, size_t index) {
  return index < curve.count &&
         (curve.breakBefore[index / 8U] & (1U << (index % 8U))) != 0;
}

inline bool shotCurveHeld(const ShotCurveRecord &curve, size_t index) {
  return index < curve.count &&
         (curve.held[index / 8U] & (1U << (index % 8U))) != 0;
}

inline bool validShotCurveRecord(const ShotCurveRecord &curve) {
  ShotCurveReader reader;
  if (curve.shotId == 0 || curve.count > SHOT_CURVE_MAX_POINTS ||
      curve.bytes > SHOT_CURVE_STREAM_BYTES || !shotCurveSeek(curve, 0, reader))
    return false;
  for (size_t i = 0; i < curve.count; ++i) {
    const uint16_t previous = reader.atMs;
    if (!reader.next() || reader.atMs > SHOT_CURVE_MAX_TIME_MS ||
        (i != 0 && reader.atMs < previous) ||
        shotLogWeightIsMissing(reader.weightCg)) return false;
  }
  return reader.offset == curve.bytes &&
         (curve.count == 0 || (reader.atMs == curve.lastAtMs &&
                               reader.weightCg == curve.lastWeightCg));
}

// Standard padded base64 (the browser's atob input), NUL-terminated.
inline bool appendBase64(char *out, size_t capacity, size_t &used,
                         const uint8_t *data, size_t size) {
  static constexpr char kDigits[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (used + 4U * ((size + 2U) / 3U) + 1U > capacity) return false;
  for (size_t i = 0; i < size; i += 3U) {
    const uint32_t n = static_cast<uint32_t>(data[i]) << 16 |
                       (i + 1U < size ? static_cast<uint32_t>(data[i + 1U]) << 8 : 0U) |
                       (i + 2U < size ? data[i + 2U] : 0U);
    out[used++] = kDigits[n >> 18 & 63U];
    out[used++] = kDigits[n >> 12 & 63U];
    out[used++] = i + 1U < size ? kDigits[n >> 6 & 63U] : '=';
    out[used++] = i + 2U < size ? kDigits[n & 63U] : '=';
  }
  out[used] = '\0';
  return true;
}

// printf("%.Nf", value / 10^N) for an integer fixed-point source, byte for
// byte, without newlib's soft-double conversion (the S3 FPU is single
// precision). N is 1..3.
struct FixedText {
  char text[16];
  FixedText(long value, unsigned digits) {
    static constexpr unsigned long kScale[] = {1, 10, 100, 1000};
    const unsigned long magnitude = value < 0 ? 0UL - static_cast<unsigned long>(value)
                                              : static_cast<unsigned long>(value);
    snprintf(text, sizeof(text), "%s%lu.%0*lu", value < 0 ? "-" : "",
             magnitude / kScale[digits], static_cast<int>(digits),
             magnitude % kScale[digits]);
  }
};

// The curve scalars as JSON key/value text: Stats rows emit all of them,
// the live hero only those that changed.
template <typename Emit>
inline bool forEachShotCurveScalar(const ShotCurveRecord &curve, Emit &&emit) {
  char value[16];
  const auto seconds = [&](const char *key, uint16_t ms) {
    return ms == SHOT_LOG_METRIC_MISSING ? emit(key, "null")
                                         : emit(key, FixedText(ms, 3).text);
  };
  const auto centigrams = [&](const char *key, int16_t cg) {
    if (shotLogWeightIsMissing(cg)) strcpy(value, "null");
    else snprintf(value, sizeof(value), "%d", static_cast<int>(cg));
    return emit(key, value);
  };
  return emit("wTruncated", curve.truncated ? "true" : "false") &&
         seconds("dropS", curve.firstDrop.atMs) &&
         centigrams("dropCg", curve.firstDrop.weightCg) &&
         seconds("extendedS", curve.extended.atMs) &&
         centigrams("extCg", curve.extended.weightCg) &&
         seconds("atmS", curve.atm.atMs) &&
         centigrams("atmCg", curve.atm.weightCg) &&
         seconds("atmClearedS", curve.atmClearedMs) &&
         seconds("endS", curve.ended.atMs) &&
         centigrams("endCg", curve.ended.weightCg);
}

// Curve object body from point `start`: the points' delta stream in base64
// ("wD"), the point before them when start > 0 ("wRef"), absolute break and
// held indices, then the scalars unless the caller streams those itself. A
// fragment (start > 0) leaves out an index list it has nothing to add to.
inline bool formatShotCurveJsonBody(char *out, size_t capacity,
                                    const ShotCurveRecord &curve,
                                    uint16_t start = 0, bool scalars = true) {
  ShotCurveReader from;
  if (out == nullptr || capacity < 32 || curve.count > SHOT_CURVE_MAX_POINTS ||
      start > curve.count || !shotCurveSeek(curve, start, from)) {
    return false;
  }
  size_t used = 0;
  out[0] = '\0';
  auto append = [&](const char *text) -> bool {
    const size_t length = strlen(text);
    if (used + length + 1U > capacity) {
      return false;
    }
    memcpy(out + used, text, length + 1U);
    used += length;
    return true;
  };
  char piece[40] = {};
  if (start > 0 && start < curve.count) {
    snprintf(piece, sizeof(piece), "\"wRef\":[%u,%d],",
             static_cast<unsigned>(from.atMs), static_cast<int>(from.weightCg));
    if (!append(piece)) return false;
  }
  auto indices = [&](const char *key, size_t i,
                     bool (*marked)(const ShotCurveRecord &, size_t)) -> bool {
    bool first = true;
    for (; i < curve.count; ++i) {
      if (!marked(curve, i)) continue;
      if (first) snprintf(piece, sizeof(piece), ",\"%s\":[%u", key, static_cast<unsigned>(i));
      else snprintf(piece, sizeof(piece), ",%u", static_cast<unsigned>(i));
      if (!append(piece)) return false;
      first = false;
    }
    if (!first) return append("]");
    if (start > 0) return true;
    snprintf(piece, sizeof(piece), ",\"%s\":[]", key);
    return append(piece);
  };
  if (!append("\"wD\":\"") ||
      !appendBase64(out, capacity, used, curve.stream + from.offset,
                    from.size - from.offset) ||
      !append("\"") || !indices("wBreakBefore", start > 1 ? start : 1, shotCurveBreakBefore) ||
      !indices("wHeld", start, shotCurveHeld)) return false;
  return !scalars ||
         forEachShotCurveScalar(curve, [&](const char *key, const char *value) {
           snprintf(piece, sizeof(piece), ",\"%s\":%s", key, value);
           return append(piece);
         });
}

}  // namespace openbrewbyweight

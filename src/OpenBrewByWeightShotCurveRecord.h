#pragma once

// The shot curve record. Its observations stay one schema-3 delta stream end
// to end (capture, PSRAM, flash, Web UI); only the browser decodes them.
// Dependency-free so the control-status snapshot can embed a record.

#include <stddef.h>
#include <stdint.h>

namespace openbrewbyweight {

constexpr uint32_t SHOT_CURVE_MAX_TIME_MS = 60000;
// Capacity assumption only, never a sampling grid or rate limiter.
constexpr size_t SHOT_CURVE_MAX_POINTS = SHOT_CURVE_MAX_TIME_MS / 50U + 1U;
constexpr size_t SHOT_CURVE_BREAK_BYTES = (SHOT_CURVE_MAX_POINTS + 7U) / 8U;
// A point is one time byte and one weight byte; an escape adds two. Any
// weight may escape, a time only when dt >= 255 ms, which the 60000 ms
// domain caps at 235 points: the valid-record worst case, reserved in full.
constexpr size_t SHOT_CURVE_STREAM_BYTES =
    4U * SHOT_CURVE_MAX_POINTS + 2U * (SHOT_CURVE_MAX_TIME_MS / 255U);

struct ShotCurveEvent {
  uint16_t atMs;
  int16_t weightCg;
};

static_assert(sizeof(ShotCurveEvent) == 4,
              "ShotCurveEvent packing is part of the curve section format");

// Readers never read past `bytes` (stream) or `count` (bitmap bits): bytes
// beyond them may be stale, so copyShotCurve moves only the used ones.
struct ShotCurveRecord {
  uint32_t shotId;
  uint16_t count;
  bool truncated;
  uint8_t reserved;
  // atmClearedMs and the four events are the curve section's 18 fixed bytes.
  uint16_t atmClearedMs;
  ShotCurveEvent firstDrop;
  ShotCurveEvent extended;
  ShotCurveEvent atm;
  ShotCurveEvent ended;
  uint16_t bytes;
  // Newest point: capture appends without walking the stream.
  uint16_t lastAtMs;
  int16_t lastWeightCg;
  uint8_t stream[SHOT_CURVE_STREAM_BYTES];
  uint8_t breakBefore[SHOT_CURVE_BREAK_BYTES];
  // Samples the touch classifier held: recorded, never control weight.
  uint8_t held[SHOT_CURVE_BREAK_BYTES];
};

static_assert(offsetof(ShotCurveRecord, bytes) -
                      offsetof(ShotCurveRecord, atmClearedMs) == 18,
              "The curve section's fixed bytes are atmClearedMs and the events");
static_assert(sizeof(ShotCurveRecord) == 5608,
              "ShotCurveRecord size pins the PSRAM curve-store footprint");

}  // namespace openbrewbyweight

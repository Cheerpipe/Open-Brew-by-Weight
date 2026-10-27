#pragma once

// Host-side shot trace decoder for tests; format: docs/SHOT_TRACE.md.

#include <cstring>
#include <vector>

#include "../OpenBrewByWeightShotTrace.h"

namespace shot_trace_test {

using namespace openbrewbyweight;

struct Event {
  bool debug = false;
  uint8_t kind = 0;
  uint32_t ms = 0;
  int16_t a = 0;
  uint16_t b = 0;
  bool more = false;  // A continuation record followed.
  uint16_t moreHead = 0;  // Continuation head bits 0-15.
  int16_t moreA = 0;
  uint16_t moreB = 0;
  // DebugCode namespace: the full arguments.
  int32_t argument1() const {
    return more ? static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint16_t>(moreA)) << 16 |
                                       static_cast<uint16_t>(a))
                : a;
  }
  int32_t argument2() const {
    return more ? static_cast<int32_t>(static_cast<uint32_t>(moreB) << 16 | b) : b;
  }
  // Profile namespace: the event's two arguments.
  uint32_t profileArg1() const { return static_cast<uint32_t>(moreHead) << 16 | b; }
  uint32_t profileArg2() const {
    return more ? static_cast<uint32_t>(moreB) << 16 | static_cast<uint16_t>(moreA) : 0;
  }
};

struct Trace {
  ShotTraceHeader header = {};
  ShotTraceSnapshot snapshot = {};
  ShotTraceCounters counters = {};
  std::vector<ShotTracePreShot> preShot;
  std::vector<Event> events;
};

// Validates magic, versions, declared sizes and the CRC before decoding.
inline bool decode(const std::vector<uint8_t> &bytes, Trace &out) {
  out = Trace{};
  if (bytes.size() < SHOT_TRACE_FIXED_BYTES) return false;
  memcpy(&out.header, bytes.data(), sizeof(out.header));
  const ShotTraceHeader &header = out.header;
  if (header.magic != SHOT_TRACE_MAGIC || header.formatVersion != SHOT_TRACE_FORMAT_VERSION ||
      header.totalBytes != bytes.size() || header.snapshotBytes != sizeof(ShotTraceSnapshot) ||
      header.countersBytes != sizeof(ShotTraceCounters) ||
      header.preShotCount > header.preShotCapacity)
    return false;
  std::vector<uint8_t> unsealed = bytes;
  memset(unsealed.data() + offsetof(ShotTraceHeader, crc32), 0, sizeof(header.crc32));
  if (scaleProfileChecksum(0, unsealed.data(), unsealed.size()) != header.crc32) return false;
  size_t offset = sizeof(ShotTraceHeader);
  memcpy(&out.snapshot, &bytes[offset], sizeof(out.snapshot));
  offset += header.snapshotBytes;
  memcpy(&out.counters, &bytes[offset], sizeof(out.counters));
  offset += header.countersBytes;
  out.preShot.resize(header.preShotCount);
  if (header.preShotCount != 0)
    memcpy(out.preShot.data(), &bytes[offset], header.preShotCount * sizeof(ShotTracePreShot));
  offset += header.preShotCapacity * sizeof(ShotTracePreShot);
  if (offset + header.eventCount * sizeof(ShotTraceRecord) != bytes.size()) return false;
  for (size_t i = 0; i < header.eventCount; ++i) {
    ShotTraceRecord record;
    memcpy(&record, &bytes[offset + i * sizeof(record)], sizeof(record));
    const bool debug = (record.head & SHOT_TRACE_DEBUG_NAMESPACE) != 0;
    const uint8_t kind = static_cast<uint8_t>(record.head >> 24);
    if (!debug && kind == static_cast<uint8_t>(ShotTraceKind::CONTINUATION)) {
      if (out.events.empty() || out.events.back().more) return false;
      if ((record.head & 0x00FF0000U) != 0) return false;  // Bits 16-23 stay zero.
      out.events.back().more = true;
      out.events.back().moreHead = static_cast<uint16_t>(record.head);
      out.events.back().moreA = record.a;
      out.events.back().moreB = record.b;
      continue;
    }
    Event event;
    event.debug = debug;
    event.kind = kind;
    event.ms = record.head & SHOT_TRACE_MS_MAX;
    event.a = record.a;
    event.b = record.b;
    out.events.push_back(event);
  }
  return true;
}

// The HTTP export path with a small chunk so the CRC patch and the chunking
// are both exercised.
inline std::vector<uint8_t> exportBytes(ShotTraceExport *result = nullptr) {
  std::vector<uint8_t> bytes;
  uint8_t chunk[300];
  const ShotTraceExport outcome = shotTrace().exportCompleted(
      chunk, sizeof(chunk), [&](const uint8_t *data, size_t size) {
        bytes.insert(bytes.end(), data, data + size);
        return true;
      });
  if (result != nullptr) *result = outcome;
  return bytes;
}

inline bool decodeCompleted(Trace &out) {
  ShotTraceExport result = ShotTraceExport::EMPTY;
  const std::vector<uint8_t> bytes = exportBytes(&result);
  return result == ShotTraceExport::OK && decode(bytes, out);
}

inline uint8_t kindOf(ScaleProfileEvent kind) { return static_cast<uint8_t>(kind); }
inline uint8_t kindOf(DebugCode code) { return static_cast<uint8_t>(code); }
inline uint8_t kindOf(ShotTraceKind kind) { return static_cast<uint8_t>(kind); }
inline bool isDebug(ScaleProfileEvent) { return false; }
inline bool isDebug(DebugCode) { return true; }
inline bool isDebug(ShotTraceKind) { return false; }

template <typename Kind>
const Event *find(const Trace &trace, Kind kind, size_t nth = 0) {
  for (const Event &event : trace.events)
    if (event.debug == isDebug(kind) && event.kind == kindOf(kind) && nth-- == 0) return &event;
  return nullptr;
}

template <typename Kind>
size_t count(const Trace &trace, Kind kind) {
  size_t matches = 0;
  for (const Event &event : trace.events)
    if (event.debug == isDebug(kind) && event.kind == kindOf(kind)) ++matches;
  return matches;
}

// Pre-shot entry of one kind (namespace 0), or nullptr.
inline const ShotTracePreShot *findPreShot(const Trace &trace, uint8_t kind) {
  for (const ShotTracePreShot &item : trace.preShot)
    if ((item.event.head >> 24) == kind) return &item;
  return nullptr;
}

// STATE_CHANGED record of one signal, initial or not.
inline const Event *findState(const Trace &trace, ScaleProfileSignal signal, size_t nth = 0) {
  for (const Event &event : trace.events)
    if (!event.debug && event.kind == kindOf(ScaleProfileEvent::STATE_CHANGED) &&
        (event.b & ~SHOT_TRACE_STATE_INITIAL) == static_cast<uint16_t>(signal) && nth-- == 0)
      return &event;
  return nullptr;
}

}  // namespace shot_trace_test

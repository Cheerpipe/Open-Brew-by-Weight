#pragma once

// Internal-heap block maps for field diagnostics. Free totals and the largest
// block hide *where* allocations land; Wi-Fi bring-up placement decides
// whether the central DRAM run survives, so the boot stages that shape it are
// mapped block by block, plus one on-demand view. The payload is allocated
// once in PSRAM (never internal fallback); the Diagnostic page downloads it.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "OpenBrewByWeightTaskMutex.h"

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include "esp_heap_caps.h"
#endif

namespace openbrewbyweight {

// Before Wi-Fi bring-up, after it (HTTP ready), after the first WebSocket
// outcome, at the end of the boot capture, and the on-demand view.
enum class HeapMapSlot : uint8_t { NETWORK_START, HTTP_READY, WS_DONE, SETTLED, NOW, COUNT };

inline const char *heapMapSlotName(HeapMapSlot slot) {
  constexpr const char *names[] = {"network_start", "http_ready", "ws_done", "settled", "now"};
  return names[static_cast<size_t>(slot)];
}

struct HeapMapBlock {
  uint32_t address;
  uint32_t size : 31;
  uint32_t used : 1;
};
struct HeapMapRegion {
  uint32_t start, end;
};
constexpr size_t HEAP_MAP_BLOCKS = 640, HEAP_MAP_REGIONS = 12;
struct HeapMapSnapshot {
  uint32_t atMs;
  uint16_t blocks;
  uint8_t regions;
  bool truncated;
  HeapMapRegion region[HEAP_MAP_REGIONS];
  HeapMapBlock block[HEAP_MAP_BLOCKS];
};

inline TaskMutex heapMapMutex;
inline HeapMapSnapshot *heapMaps = nullptr;

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
inline bool heapMapWalk(walker_heap_into_t heap, walker_block_info_t info, void *user) {
  auto &capture = *static_cast<HeapMapSnapshot *>(user);
  const uint32_t start = static_cast<uint32_t>(heap.start);
  if ((capture.regions == 0 || capture.region[capture.regions - 1].start != start) &&
      capture.regions < HEAP_MAP_REGIONS)
    capture.region[capture.regions++] = {start, static_cast<uint32_t>(heap.end)};
  if (capture.blocks == HEAP_MAP_BLOCKS) {
    capture.truncated = true;
    return true;
  }
  HeapMapBlock &block = capture.block[capture.blocks++];
  block.address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info.ptr));
  block.size = static_cast<uint32_t>(info.size);
  block.used = info.used;
  return true;
}
#endif

// Boot slots keep their first capture; NOW is replaced on every request. The
// walk holds each heap's lock for well under a millisecond, so callers skip
// it while a shot runs.
inline void captureHeapMap(HeapMapSlot slot, uint32_t nowMs) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  TaskLockGuard lock(heapMapMutex);
  if (heapMaps == nullptr)
    heapMaps = static_cast<HeapMapSnapshot *>(heap_caps_calloc(
        static_cast<size_t>(HeapMapSlot::COUNT), sizeof(HeapMapSnapshot),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (heapMaps == nullptr) return;
  HeapMapSnapshot &capture = heapMaps[static_cast<size_t>(slot)];
  if (capture.blocks != 0 && slot != HeapMapSlot::NOW) return;
  capture.atMs = nowMs;
  capture.blocks = 0;
  capture.regions = 0;
  capture.truncated = false;
  heap_caps_walk(MALLOC_CAP_INTERNAL, heapMapWalk, &capture);
#else
  (void)slot;
  (void)nowMs;
#endif
}

// One CSV data row per call, in slot then address-walk order; false once
// exhausted. Each row is read under the lock so a slow HTTP client never
// blocks a capture.
inline bool heapMapRow(size_t &cursor, char *out, size_t capacity) {
  TaskLockGuard lock(heapMapMutex);
  if (heapMaps == nullptr) return false;
  for (;;) {
    const size_t slot = cursor / HEAP_MAP_BLOCKS, index = cursor % HEAP_MAP_BLOCKS;
    if (slot >= static_cast<size_t>(HeapMapSlot::COUNT)) return false;
    const HeapMapSnapshot &capture = heapMaps[slot];
    if (index >= capture.blocks) {
      cursor = (slot + 1) * HEAP_MAP_BLOCKS;
      continue;
    }
    const HeapMapBlock &block = capture.block[index];
    uint32_t regionStart = 0, regionEnd = 0;
    for (uint8_t r = 0; r < capture.regions; ++r)
      if (block.address >= capture.region[r].start && block.address < capture.region[r].end) {
        regionStart = capture.region[r].start;
        regionEnd = capture.region[r].end;
      }
    snprintf(out, capacity, "%s,%lu,%08lx,%08lx,%08lx,%lu,%s%s",
             heapMapSlotName(static_cast<HeapMapSlot>(slot)),
             static_cast<unsigned long>(capture.atMs), static_cast<unsigned long>(regionStart),
             static_cast<unsigned long>(regionEnd), static_cast<unsigned long>(block.address),
             static_cast<unsigned long>(block.size), block.used ? "used" : "free",
             capture.truncated && index + 1 == capture.blocks ? ",truncated" : "");
    ++cursor;
    return true;
  }
}

}  // namespace openbrewbyweight

#pragma once

#include "OpenBrewByWeightHeapMap.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "OpenBrewByWeightTimingTypes.h"

namespace openbrewbyweight {

// Memory at each network bring-up milestone of the current boot, from network
// start, plus the minima of the first minute after it and after the first
// WebSocket outcome. Retained in RAM for BOOT_HEAP and the debug export.
enum class BootHeapStage : uint8_t {
  NETWORK_START, WIFI_INIT, MDNS_READY, HTTP_READY, NTP_DONE, CLOUD_DONE,
  WS_START, WS_DONE, WS_INIT, WS_TASK, COUNT
};
enum class BootHeapResult : uint8_t { OK, FAILED };
constexpr uint32_t BOOT_HEAP_WINDOW_MS = 60000;

inline const char *bootHeapStageName(BootHeapStage stage) {
  constexpr const char *names[] = {
      "network_start", "wifi_init", "mdns_ready", "http_ready", "ntp_done",
      "cloud_done", "ws_start", "ws_done", "ws_init", "ws_task"};
  return names[static_cast<size_t>(stage)];
}
// Stages whose internal-heap block map is kept (HeapMapSlot::COUNT = none).
constexpr HeapMapSlot BOOT_HEAP_MAP_SLOT[] = {
    HeapMapSlot::NETWORK_START, HeapMapSlot::COUNT, HeapMapSlot::COUNT,
    HeapMapSlot::HTTP_READY, HeapMapSlot::COUNT, HeapMapSlot::COUNT,
    HeapMapSlot::COUNT, HeapMapSlot::WS_DONE, HeapMapSlot::COUNT, HeapMapSlot::COUNT};
static_assert(sizeof(BOOT_HEAP_MAP_SLOT) / sizeof(BOOT_HEAP_MAP_SLOT[0]) ==
              static_cast<size_t>(BootHeapStage::COUNT), "one map slot per stage");
inline const char *bootHeapResultName(BootHeapResult result) {
  return result == BootHeapResult::OK ? "ok" : "failed";
}

struct BootHeapRecord {
  uint32_t atMs = 0;
  HeapLifecycleSample internal{};
  uint32_t dmaFree = 0;
  uint32_t dmaLargest = 0;
  uint32_t psramFree = 0;
  uint32_t psramLargest = 0;
  BootHeapResult result{};
};
struct BootHeapMinimum {
  uint32_t samples = 0;
  uint32_t free = 0;
  uint32_t largest = 0;
  uint32_t dmaFree = 0;
  uint32_t dmaLargest = 0;
};
struct BootHeapCapture {
  BootHeapRecord records[static_cast<size_t>(BootHeapStage::COUNT)]{};
  BootHeapMinimum post{}, ws{};
  uint32_t stages = 0;
  bool complete = false;
};
static_assert(sizeof(BootHeapCapture) <= 768, "Boot heap capture exceeds its PSRAM budget");

// One capture owner, shared by task-only producers and the serial reader.
// Synchronization stays internal; retained payload never consumes heap DRAM.
inline TaskMutex bootHeapMutex;
inline OPEN_BREW_BY_WEIGHT_PSRAM_BSS BootHeapCapture bootHeapCapture;
inline std::atomic<bool> bootHeapSampling{false};

// True while `now` is inside the window opened by `stage` (recorded).
inline bool bootHeapWindowOpen(const BootHeapCapture &capture, BootHeapStage stage,
                               uint32_t now) {
  return (capture.stages & (1U << static_cast<unsigned>(stage))) &&
         static_cast<uint32_t>(now - capture.records[static_cast<size_t>(stage)].atMs) <
             BOOT_HEAP_WINDOW_MS;
}

inline BootHeapRecord sampleBootHeap(uint32_t now, const HeapCapSnapshot &heap) {
  BootHeapRecord record;
  record.atMs = now;
  record.internal = heapLifecycleSample(heap);
  record.psramFree = heap.psramFree;
  record.psramLargest = heap.psramLargest;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  multi_heap_info_t dma{};
  heap_caps_get_info(&dma, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  record.dmaFree = static_cast<uint32_t>(dma.total_free_bytes);
  record.dmaLargest = static_cast<uint32_t>(dma.largest_free_block);
#endif
  return record;
}

inline void updateBootHeapMinimum(BootHeapMinimum &minimum, const BootHeapRecord &record) {
  if (minimum.samples++ == 0) {
    minimum.free = record.internal.freeBytes;
    minimum.largest = record.internal.largestBlock;
    minimum.dmaFree = record.dmaFree;
    minimum.dmaLargest = record.dmaLargest;
    return;
  }
  if (record.internal.freeBytes < minimum.free) minimum.free = record.internal.freeBytes;
  if (record.internal.largestBlock < minimum.largest) minimum.largest = record.internal.largestBlock;
  if (record.dmaFree < minimum.dmaFree) minimum.dmaFree = record.dmaFree;
  if (record.dmaLargest < minimum.dmaLargest) minimum.dmaLargest = record.dmaLargest;
}

inline void recordBootHeap(BootHeapStage stage, BootHeapResult result, uint32_t now,
                           const HeapCapSnapshot &heap) {
  const uint32_t bit = 1U << static_cast<unsigned>(stage);
  {
    TaskLockGuard lock(bootHeapMutex);
    if (bootHeapCapture.complete || (bootHeapCapture.stages & bit)) return;
    if (stage != BootHeapStage::NETWORK_START && bootHeapCapture.stages == 0) return;
  }
  BootHeapRecord record = sampleBootHeap(now, heap);
  record.result = result;
  TaskLockGuard lock(bootHeapMutex);
  auto &capture = bootHeapCapture;
  if (capture.complete || (capture.stages & bit)) return;
  capture.records[static_cast<size_t>(stage)] = record;
  capture.stages |= bit;
  if (BOOT_HEAP_MAP_SLOT[static_cast<size_t>(stage)] != HeapMapSlot::COUNT)
    captureHeapMap(BOOT_HEAP_MAP_SLOT[static_cast<size_t>(stage)], now);
  bootHeapSampling.store(true, std::memory_order_release);
  if (bootHeapWindowOpen(capture, BootHeapStage::NETWORK_START, now))
    updateBootHeapMinimum(capture.post, record);
  if (bootHeapWindowOpen(capture, BootHeapStage::WS_DONE, now))
    updateBootHeapMinimum(capture.ws, record);
}

inline void recordBootHeap(BootHeapStage stage, BootHeapResult result, uint32_t now) {
  // Avoid sampling on every reconnection after the one-shot stage is retained.
  {
    TaskLockGuard lock(bootHeapMutex);
    if (bootHeapCapture.complete || bootHeapCapture.stages == 0 ||
        (bootHeapCapture.stages & (1U << static_cast<unsigned>(stage)))) return;
  }
  recordBootHeap(stage, result, now, sampleHeapCaps());
}

inline void serviceBootHeap(uint32_t now) {
  if (!bootHeapSampling.load(std::memory_order_acquire)) return;
  {
    TaskLockGuard lock(bootHeapMutex);
    auto &capture = bootHeapCapture;
    if (capture.complete || capture.stages == 0) return;
    // A WS attempt without an outcome still closes: its window runs from start.
    const bool wsDone = capture.stages & (1U << static_cast<unsigned>(BootHeapStage::WS_DONE));
    const bool wsStarted = wsDone || (capture.stages & (1U << static_cast<unsigned>(BootHeapStage::WS_START)));
    const uint32_t wsAtMs = capture.records[static_cast<size_t>(
        wsDone ? BootHeapStage::WS_DONE : BootHeapStage::WS_START)].atMs;
    const auto elapsed = [now](uint32_t atMs) {
      return ticksSince(now, atMs) >= static_cast<int32_t>(BOOT_HEAP_WINDOW_MS);
    };
    if (elapsed(capture.records[static_cast<size_t>(BootHeapStage::NETWORK_START)].atMs) &&
        (!wsStarted || elapsed(wsAtMs))) {
      capture.complete = true;
      bootHeapSampling.store(false, std::memory_order_release);
      captureHeapMap(HeapMapSlot::SETTLED, now);
      return;
    }
  }
  const BootHeapRecord record = sampleBootHeap(now, sampleHeapCaps());
  TaskLockGuard lock(bootHeapMutex);
  auto &capture = bootHeapCapture;
  if (capture.complete) return;
  if (bootHeapWindowOpen(capture, BootHeapStage::NETWORK_START, now))
    updateBootHeapMinimum(capture.post, record);
  if (bootHeapWindowOpen(capture, BootHeapStage::WS_DONE, now))
    updateBootHeapMinimum(capture.ws, record);
}

}  // namespace openbrewbyweight

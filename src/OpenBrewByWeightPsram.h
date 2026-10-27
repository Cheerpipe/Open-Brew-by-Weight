#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <atomic>
#include <limits.h>
#include <type_traits>

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <esp_attr.h>
#endif

// Place large BSS in PSRAM (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y,
// checked by the build). Keep flash DMA sources, OTA chunks, and
// flash-writing task stacks internal: some flash paths still disable cache,
// making PSRAM inaccessible.
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                         \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#define OPEN_BREW_BY_WEIGHT_PSRAM_BSS
#define OPEN_BREW_BY_WEIGHT_PSRAM_NOINIT
#else
#define OPEN_BREW_BY_WEIGHT_PSRAM_BSS EXT_RAM_BSS_ATTR
// Kept across panic, watchdog and software resets (not power loss); without
// the Kconfig, EXT_RAM_NOINIT_ATTR would silently fall back to internal RAM.
#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error "CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY is required"
#endif
#define OPEN_BREW_BY_WEIGHT_PSRAM_NOINIT EXT_RAM_NOINIT_ATTR
#endif

namespace openbrewbyweight {

enum class AllocationOwner : uint8_t {
  OTHER, NETWORK, WEBHOOK, PROFILER, FLASH_IO, OTA, BUZZER, JSON,
  SERIAL_LOG, COUNT
};

struct AllocationMetrics {
  std::atomic<uint32_t> successes{0};
  std::atomic<uint32_t> failures{0};
  std::atomic<uint32_t> largestRequestBytes{0};
  std::atomic<uint32_t> lastFailureBytes{0};
};

namespace detail {

inline std::atomic<uint32_t> g_allocExternalOk{0};
inline std::atomic<uint32_t> g_allocExternalFallback{0};
inline std::atomic<bool> g_workBufExternal{false};
inline AllocationMetrics g_allocations[static_cast<size_t>(AllocationOwner::COUNT)];
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
// Deterministic allocation-failure injection; never compiled into firmware.
inline std::atomic<int32_t> g_hostAllocationsUntilFailure{-1};
inline bool hostAllocationFails() {
  int32_t left = g_hostAllocationsUntilFailure.load(std::memory_order_relaxed);
  while (left >= 0) {
    if (left == 0) return true;
    if (g_hostAllocationsUntilFailure.compare_exchange_weak(
            left, left - 1, std::memory_order_relaxed)) return false;
  }
  return false;
}
#endif

}  // namespace detail

inline void noteAllocation(AllocationOwner owner, size_t bytes, bool success) {
  auto &metrics = detail::g_allocations[static_cast<size_t>(owner)];
  const uint32_t size = bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(bytes);
  uint32_t largest = metrics.largestRequestBytes.load(std::memory_order_relaxed);
  while (size > largest && !metrics.largestRequestBytes.compare_exchange_weak(
             largest, size, std::memory_order_relaxed)) {}
  if (success) {
    metrics.successes.fetch_add(1, std::memory_order_relaxed);
  } else {
    metrics.failures.fetch_add(1, std::memory_order_relaxed);
    metrics.lastFailureBytes.store(size, std::memory_order_relaxed);
  }
}

inline uint32_t allocExternalOkCount() {
  return detail::g_allocExternalOk.load(std::memory_order_relaxed);
}

inline uint32_t allocExternalFallbackCount() {
  return detail::g_allocExternalFallback.load(std::memory_order_relaxed);
}

inline bool workBufIsExternal() {
  return detail::g_workBufExternal.load(std::memory_order_acquire);
}

inline void noteWorkBufExternal(bool isExternal) {
  detail::g_workBufExternal.store(isExternal, std::memory_order_release);
}

inline bool pointerIsExternal(const void *block) {
  if (block == nullptr) {
    return false;
  }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                         \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  return true;
#else
  return esp_ptr_external_ram(block);
#endif
}

// SPIRAM only. Large blobs that must not punch a hole in internal DRAM
// (NetworkWorkBuf, Wi-Fi AP records). Returns nullptr if SPIRAM cannot
// satisfy — callers fail closed.
inline void *allocExternal(size_t bytes, AllocationOwner owner = AllocationOwner::OTHER) {
  if (bytes == 0) {
    return nullptr;
  }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                         \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  void *block = detail::hostAllocationFails() ? nullptr : malloc(bytes);
  noteAllocation(owner, bytes, block != nullptr);
  if (block != nullptr) {
    detail::g_allocExternalOk.fetch_add(1, std::memory_order_relaxed);
  }
  return block;
#else
  void *block = nullptr;
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) {
    block = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
#endif
  if (block != nullptr && pointerIsExternal(block)) {
    noteAllocation(owner, bytes, true);
    detail::g_allocExternalOk.fetch_add(1, std::memory_order_relaxed);
    return block;
  }
  if (block != nullptr) {
    heap_caps_free(block);
  }
  noteAllocation(owner, bytes, false);
  return nullptr;
#endif
}

inline void *allocInternal(size_t bytes, AllocationOwner owner = AllocationOwner::OTHER) {
  if (bytes == 0) {
    return nullptr;
  }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  void *block = detail::hostAllocationFails() ? nullptr : malloc(bytes);
#else
  void *block = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
  noteAllocation(owner, bytes, block != nullptr);
  return block;
}

inline void heapCapsFree(void *block) {
  if (block == nullptr) {
    return;
  }
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  free(block);
#else
  heap_caps_free(block);
#endif
}

struct HeapCapSnapshot {
  uint32_t internalTotal = 0;
  uint32_t internalFree = 0;
  uint32_t internalMinimum = 0;
  uint32_t internalLargest = 0;
  uint32_t internalAllocatedBlocks = 0;
  uint32_t internalFreeBlocks = 0;
  uint16_t internalFragmentationPermille = 0;
  uint32_t psramTotal = 0;
  uint32_t psramFree = 0;
  uint32_t psramMinimum = 0;
  uint32_t psramLargest = 0;
};

inline uint16_t heapFragmentationPermille(uint32_t freeBytes,
                                          uint32_t largestBlock) {
  if (freeBytes == 0 || largestBlock >= freeBytes) {
    return 0;
  }
  return static_cast<uint16_t>(
      (static_cast<uint64_t>(freeBytes - largestBlock) * 1000U) / freeBytes);
}

enum class HeapLifecycleEvent : uint8_t {
  NONE,
  HTTP_START,
  HTTP_STOP,
  TLS_REQUEST,
  WIFI_CONNECT,
  WIFI_DISCONNECT,
  AP_START,
  AP_STOP,
  OTA_SESSION_BEGIN,
  OTA_FLASH_BEGIN,
  OTA_ABORT
};

enum class HeapLifecycleResult : uint8_t {
  NONE,
  SUCCESS,
  FAILURE,
  CANCELLED
};

struct HeapLifecycleDelta {
  int32_t freeBytes = 0;
  int32_t largestBlock = 0;
  int32_t allocatedBlocks = 0;
  int32_t freeBlocks = 0;
  int32_t fragmentationPermille = 0;
};

struct HeapLifecycleSample {
  uint32_t freeBytes = 0;
  uint32_t largestBlock = 0;
  uint32_t allocatedBlocks = 0;
  uint32_t freeBlocks = 0;
  uint16_t fragmentationPermille = 0;
};

inline HeapLifecycleSample heapLifecycleSample(const HeapCapSnapshot &snapshot) {
  return {snapshot.internalFree, snapshot.internalLargest,
          snapshot.internalAllocatedBlocks, snapshot.internalFreeBlocks,
          snapshot.internalFragmentationPermille};
}

struct HeapLifecycleAggregate {
  uint32_t cycles = 0;
  uint32_t staleReplacements = 0;
  HeapLifecycleEvent lastEvent = HeapLifecycleEvent::NONE;
  HeapLifecycleResult lastResult = HeapLifecycleResult::NONE;
  HeapLifecycleSample before = {};
  HeapLifecycleSample after = {};
  HeapLifecycleDelta lastDelta = {};
  int32_t worstFreeDelta = 0;
  int32_t worstLargestDelta = 0;
  int32_t maximumFreeBlocksIncrease = 0;
};

struct HeapLifecycleTracker {
  HeapLifecycleAggregate aggregate = {};
  HeapLifecycleSample pendingBefore = {};
  HeapLifecycleEvent pendingEvent = HeapLifecycleEvent::NONE;
  bool pending = false;
};

inline int32_t boundedHeapDelta(uint32_t before, uint32_t after) {
  const int64_t delta = static_cast<int64_t>(after) - before;
  if (delta > INT32_MAX) return INT32_MAX;
  if (delta < INT32_MIN) return INT32_MIN;
  return static_cast<int32_t>(delta);
}

inline void beginHeapLifecycle(HeapLifecycleTracker &tracker,
                               HeapLifecycleEvent event,
                               const HeapCapSnapshot &before) {
  if (tracker.pending) {
    ++tracker.aggregate.staleReplacements;
  }
  tracker.pendingBefore = heapLifecycleSample(before);
  tracker.pendingEvent = event;
  tracker.pending = true;
}

inline bool finishHeapLifecycle(HeapLifecycleTracker &tracker,
                                HeapLifecycleResult result,
                                const HeapCapSnapshot &after) {
  if (!tracker.pending) return false;
  HeapLifecycleAggregate &out = tracker.aggregate;
  out.before = tracker.pendingBefore;
  out.after = heapLifecycleSample(after);
  out.lastEvent = tracker.pendingEvent;
  out.lastResult = result;
  out.lastDelta.freeBytes =
      boundedHeapDelta(out.before.freeBytes, out.after.freeBytes);
  out.lastDelta.largestBlock =
      boundedHeapDelta(out.before.largestBlock, out.after.largestBlock);
  out.lastDelta.allocatedBlocks = boundedHeapDelta(
      out.before.allocatedBlocks, out.after.allocatedBlocks);
  out.lastDelta.freeBlocks = boundedHeapDelta(
      out.before.freeBlocks, out.after.freeBlocks);
  out.lastDelta.fragmentationPermille = boundedHeapDelta(
      out.before.fragmentationPermille, out.after.fragmentationPermille);
  if (out.lastDelta.freeBytes < out.worstFreeDelta) {
    out.worstFreeDelta = out.lastDelta.freeBytes;
  }
  if (out.lastDelta.largestBlock < out.worstLargestDelta) {
    out.worstLargestDelta = out.lastDelta.largestBlock;
  }
  if (out.lastDelta.freeBlocks > out.maximumFreeBlocksIncrease) {
    out.maximumFreeBlocksIncrease = out.lastDelta.freeBlocks;
  }
  ++out.cycles;
  tracker.pending = false;
  tracker.pendingEvent = HeapLifecycleEvent::NONE;
  return true;
}

static_assert(std::is_trivially_copyable<HeapCapSnapshot>::value,
              "Heap snapshots must remain lock-copyable");
static_assert(std::is_trivially_copyable<HeapLifecycleAggregate>::value,
              "Lifecycle telemetry must remain lock-copyable");
static_assert(sizeof(HeapLifecycleAggregate) <= 84 &&
                  sizeof(HeapLifecycleTracker) <= 108,
              "Lifecycle telemetry exceeds its internal-RAM budget");

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                         \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline HeapCapSnapshot &hostHeapCapsSnapshot() {
  static HeapCapSnapshot snapshot = {327680, 200000, 180000, 100000,
                                     120, 8, 500};
  return snapshot;
}
#endif

// Free-block layout of the internal DRAM heap, used by the HEAP serial
// command to show which gaps bound the largest allocatable block.
struct HeapFreeBlockInfo {
  uint32_t sizeBytes = 0;
  uintptr_t startAddress = 0;
};

constexpr size_t HEAP_FREE_BLOCK_SAMPLE_CAPACITY = 12;

struct HeapFreeBlockList {
  uint32_t walkedFreeBlocks = 0;
  uint32_t listedCount = 0;
  HeapFreeBlockInfo blocks[HEAP_FREE_BLOCK_SAMPLE_CAPACITY] = {};
};

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                         \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline HeapFreeBlockList sampleInternalFreeBlocks() {
  static const uint32_t sizes[3] = {100000, 60000, 40000};
  static const uintptr_t starts[3] = {0x3FC88000u, 0x3FCB0000u, 0x3FCE0000u};
  HeapFreeBlockList list;
  for (size_t i = 0; i < 3; ++i) {
    list.blocks[i] = HeapFreeBlockInfo{sizes[i], starts[i]};
  }
  list.walkedFreeBlocks = 3;
  list.listedCount = 3;
  return list;
}
#else
inline bool recordInternalFreeBlock(walker_heap_into_t,
                                    walker_block_info_t block, void *user) {
  HeapFreeBlockList *list = static_cast<HeapFreeBlockList *>(user);
  if (block.used) {
    return true;
  }
  ++list->walkedFreeBlocks;
  if (list->listedCount < HEAP_FREE_BLOCK_SAMPLE_CAPACITY) {
    list->blocks[list->listedCount] = HeapFreeBlockInfo{
        static_cast<uint32_t>(block.size),
        reinterpret_cast<uintptr_t>(block.ptr)};
    ++list->listedCount;
  }
  return true;
}

inline HeapFreeBlockList sampleInternalFreeBlocks() {
  HeapFreeBlockList list;
  heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                 recordInternalFreeBlock, &list);
  return list;
}
#endif

inline HeapCapSnapshot sampleHeapCaps() {
  HeapCapSnapshot snap;
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  snap = hostHeapCapsSnapshot();
#else
  const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  multi_heap_info_t internalInfo = {};
  heap_caps_get_info(&internalInfo, internalCaps);
  snap.internalTotal =
      static_cast<uint32_t>(heap_caps_get_total_size(internalCaps));
  snap.internalFree = static_cast<uint32_t>(internalInfo.total_free_bytes);
  snap.internalMinimum = static_cast<uint32_t>(internalInfo.minimum_free_bytes);
  snap.internalLargest = static_cast<uint32_t>(internalInfo.largest_free_block);
  snap.internalAllocatedBlocks =
      static_cast<uint32_t>(internalInfo.allocated_blocks);
  snap.internalFreeBlocks = static_cast<uint32_t>(internalInfo.free_blocks);
  snap.internalFragmentationPermille =
      heapFragmentationPermille(snap.internalFree, snap.internalLargest);
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) {
    const uint32_t psramCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    snap.psramTotal =
        static_cast<uint32_t>(heap_caps_get_total_size(psramCaps));
    snap.psramFree = static_cast<uint32_t>(heap_caps_get_free_size(psramCaps));
    snap.psramMinimum = static_cast<uint32_t>(heap_caps_get_minimum_free_size(psramCaps));
    snap.psramLargest = static_cast<uint32_t>(
        heap_caps_get_largest_free_block(psramCaps));
  }
#endif
#endif
  return snap;
}

}  // namespace openbrewbyweight

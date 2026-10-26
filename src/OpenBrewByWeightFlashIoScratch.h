#pragma once

// Shared internal-SRAM workspace for Preferences getBytes/putBytes and chunked
// partition transfers. XIP retains cache for most writes, but mapping and
// driver paths may still disable it and make PSRAM inaccessible. Settings
// dual-slot I/O, the last-shot blob, and per-chunk staging bytes reuse the same
// allocation; every path must hold
// tryLockFlashIo() for the whole use of the scratch.

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightPersistedSettings.h"
#include "OpenBrewByWeightPsram.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <esp_memory_utils.h>
#include <esp_partition.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace openbrewbyweight {

// One staged step of a partition-store transfer. Store sizes stay multiples
// of 4: IDF 6.1 plain (non-encrypted) writes accept any alignment, but
// word-sized chunks keep the transfer path uniform and fast.
constexpr size_t FLASH_IO_CHUNK_BYTES = 1024;
constexpr size_t FLASH_IO_SECTOR_BYTES = 4096;
// Settings slots are processed sequentially; the shared internal workspace
// therefore needs one record or one partition-transfer chunk, whichever is
// larger. Larger stores transfer in FLASH_IO_CHUNK_BYTES steps.
constexpr size_t FLASH_IO_SCRATCH_BYTES =
    sizeof(PersistedSettings) > FLASH_IO_CHUNK_BYTES
        ? sizeof(PersistedSettings)
        : FLASH_IO_CHUNK_BYTES;
static_assert(FLASH_IO_CHUNK_BYTES % 4 == 0,
              "Flash I/O chunks must stay 4-byte aligned");
static_assert(FLASH_IO_CHUNK_BYTES <= FLASH_IO_SCRATCH_BYTES,
              "Flash I/O chunks must stage through the scratch");
constexpr uint32_t FLASH_IO_LOCK_TIMEOUT_MS = 3000;
// Control-loop NVS must fail fast; the durable timeout itself retains two
// seconds of margin beneath the task watchdog budget.
constexpr uint32_t FLASH_IO_CONTROL_LOCK_TIMEOUT_MS = 50;

static_assert(FLASH_IO_CONTROL_LOCK_TIMEOUT_MS < FLASH_IO_LOCK_TIMEOUT_MS,
              "Control flash lock must be shorter than durable I/O");

inline uint8_t *&flashIoScratchBlock() {
  static uint8_t *block = nullptr;
  return block;
}

// Internal SRAM, heap-accounted. Do not use BSS: ALLOW_BSS would put this block in
// PSRAM (SET_WIFI load fails), and a forced .dram0.bss object can overlap the
// heap (IWDT on a malloc spinlock overwritten with settings-blob bytes).
inline uint8_t *flashIoScratchBytes() {
  return flashIoScratchBlock();
}

inline bool ensureFlashIoScratch() {
  uint8_t *&block = flashIoScratchBlock();
  if (block != nullptr) {
    return true;
  }
  block = static_cast<uint8_t *>(allocInternal(FLASH_IO_SCRATCH_BYTES, AllocationOwner::FLASH_IO));
  if (block == nullptr) {
    return false;
  }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  if (!esp_ptr_internal(block)) {
    heapCapsFree(block);
    block = nullptr;
    return false;
  }
#endif
  memset(block, 0, FLASH_IO_SCRATCH_BYTES);
  return true;
}

inline std::atomic<uint32_t> &flashIoLockTimeoutCount() {
  static std::atomic<uint32_t> count{0};
  return count;
}

inline std::atomic<uint32_t> &flashIoForbiddenAttemptCount() {
  static std::atomic<uint32_t> count{0};
  return count;
}

inline uint32_t flashIoLockTimeouts() {
  return flashIoLockTimeoutCount().load(std::memory_order_relaxed);
}

// Flash I/O activity for loop-gap attribution, written by the lock holder:
// heldSinceMs is nonzero while held and releasedAtMs is the last release.
struct FlashIoActivity {
  std::atomic<uint32_t> heldSinceMs{0}, releasedAtMs{0};
  uint8_t depth = 0;
};

inline FlashIoActivity &flashIoActivity() {
  static FlashIoActivity activity;
  return activity;
}

inline void noteFlashIoHeld(bool held, uint32_t nowMs) {
  FlashIoActivity &activity = flashIoActivity();
  if (held ? activity.depth++ != 0
           : activity.depth == 0 || --activity.depth != 0) return;
  if (!held) activity.releasedAtMs.store(nowMs, std::memory_order_relaxed);
  activity.heldSinceMs.store(held ? nowMs | 1U : 0U, std::memory_order_release);
}

inline bool flashIoActiveSince(uint32_t sinceMs) {
  const FlashIoActivity &activity = flashIoActivity();
  return activity.heldSinceMs.load(std::memory_order_acquire) != 0 ||
         static_cast<int32_t>(activity.releasedAtMs.load(
             std::memory_order_relaxed) - sinceMs) >= 0;
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline SemaphoreHandle_t &flashIoMutexSlot() {
  static SemaphoreHandle_t handle = nullptr;
  return handle;
}

inline bool ensureFlashIoMutex() {
  SemaphoreHandle_t &handle = flashIoMutexSlot();
  if (handle != nullptr) {
    return true;
  }
  handle = xSemaphoreCreateRecursiveMutex();
  return handle != nullptr;
}

inline SemaphoreHandle_t flashIoMutexHandle() { return flashIoMutexSlot(); }

inline TaskHandle_t &flashIoForbiddenTaskSlot() {
  static TaskHandle_t task = nullptr;
  return task;
}

inline void forbidFlashIoFromCurrentTask() {
  flashIoForbiddenTaskSlot() = xTaskGetCurrentTaskHandle();
}

// Out of line: flash I/O sites are many and the activity note is not free.
[[gnu::noinline]] inline bool tryLockFlashIo(
    uint32_t timeoutMs = FLASH_IO_LOCK_TIMEOUT_MS) {
  if (flashIoForbiddenTaskSlot() == xTaskGetCurrentTaskHandle()) {
    flashIoForbiddenAttemptCount().fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!ensureFlashIoMutex()) {
    return false;
  }
  SemaphoreHandle_t handle = flashIoMutexHandle();
  if (xSemaphoreTakeRecursive(handle, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) {
    if (!ensureFlashIoScratch()) {
      xSemaphoreGiveRecursive(handle);
      return false;
    }
    noteFlashIoHeld(true, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    return true;
  }
  flashIoLockTimeoutCount().fetch_add(1, std::memory_order_relaxed);
  return false;
}

[[gnu::noinline]] inline void unlockFlashIo() {
  SemaphoreHandle_t handle = flashIoMutexHandle();
  if (handle != nullptr) {
    noteFlashIoHeld(false, static_cast<uint32_t>(esp_timer_get_time() / 1000));
    xSemaphoreGiveRecursive(handle);
  }
}
#else
inline bool g_hostFlashIoMutexAvailable = true;

inline bool ensureFlashIoMutex() { return g_hostFlashIoMutexAvailable; }
inline void forbidFlashIoFromCurrentTask() {}

inline bool tryLockFlashIo(uint32_t = FLASH_IO_LOCK_TIMEOUT_MS) {
  if (!g_hostFlashIoMutexAvailable) {
    flashIoLockTimeoutCount().fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return ensureFlashIoScratch();
}
inline void unlockFlashIo() {}
#endif

inline void yieldFlashIo() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  vTaskDelay(pdMS_TO_TICKS(1));
#endif
}

inline void feedFlashIoWatchdog() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  // HTTP workers are not TWDT subscribers. Resetting from them logs
  // "esp_task_wdt_reset: task not found" and does not feed loop/network/scale.
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    (void)esp_task_wdt_reset();
  }
#endif
}

// Compatibility alias used by existing call sites.
inline bool lockFlashIo() { return tryLockFlashIo(); }

// Preferences putBytes/getBytes must not touch PSRAM while flash cache is
// off. Caller must already hold the flash I/O lock so the scratch exists.
inline void *copyToFlashIoScratch(const void *source, size_t bytes) {
  if (source == nullptr || bytes == 0 || bytes > FLASH_IO_SCRATCH_BYTES) {
    return nullptr;
  }
  uint8_t *scratch = flashIoScratchBytes();
  if (scratch == nullptr) {
    return nullptr;
  }
  memcpy(scratch, source, bytes);
  return scratch;
}

enum class FlashStoreStepResult : uint8_t { MORE, COMPLETE, FAILED };

struct FlashStoreTransaction {
  enum class Phase : uint8_t { IDLE, ERASE, BODY, COMMIT };
  Phase phase = Phase::IDLE;
  size_t targetOffset = 0;
  size_t offset = 0;
  size_t eraseBytes = 0;
  size_t headerBytes = 0;
  size_t totalBytes = 0;
};

// eraseBytes is the sector-aligned span erased before the body is written; it
// may be shorter than the slot when the store does not fill it.
inline void beginFlashStoreTransaction(FlashStoreTransaction &tx,
                                       size_t targetOffset, size_t eraseBytes,
                                       size_t headerBytes,
                                       size_t totalBytes) {
  tx.phase = FlashStoreTransaction::Phase::ERASE;
  tx.targetOffset = targetOffset;
  tx.offset = 0;
  tx.eraseBytes = eraseBytes;
  tx.headerBytes = headerBytes;
  tx.totalBytes = totalBytes;
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
// Chunked partition transfer for PSRAM-backed stores larger than the scratch.
// Each step copies one chunk between PSRAM and the internal scratch while the
// flash cache is on; only the internal staging bytes are reachable inside the
// partition call while it is off. Caller must already hold the flash I/O lock
// for the whole transfer.
inline bool flashIoReadChunked(const esp_partition_t *part, size_t offset,
                               void *destination, size_t bytes) {
  uint8_t *scratch = flashIoScratchBytes();
  if (part == nullptr || destination == nullptr || scratch == nullptr) {
    return false;
  }
  uint8_t *out = static_cast<uint8_t *>(destination);
  for (size_t done = 0; done < bytes;) {
    const size_t chunk = bytes - done < FLASH_IO_CHUNK_BYTES
                             ? bytes - done
                             : FLASH_IO_CHUNK_BYTES;
    if (esp_partition_read(part, offset + done, scratch, chunk) != ESP_OK) {
      return false;
    }
    memcpy(out + done, scratch, chunk);
    done += chunk;
    yieldFlashIo();
    feedFlashIoWatchdog();
  }
  return true;
}

inline bool flashIoWriteChunked(const esp_partition_t *part, size_t offset,
                                const void *source, size_t bytes) {
  uint8_t *scratch = flashIoScratchBytes();
  if (part == nullptr || source == nullptr || scratch == nullptr) {
    return false;
  }
  const uint8_t *in = static_cast<const uint8_t *>(source);
  for (size_t done = 0; done < bytes;) {
    const size_t chunk = bytes - done < FLASH_IO_CHUNK_BYTES
                             ? bytes - done
                             : FLASH_IO_CHUNK_BYTES;
    memcpy(scratch, in + done, chunk);
    if (esp_partition_write(part, offset + done, scratch, chunk) != ESP_OK) {
      return false;
    }
    done += chunk;
    yieldFlashIo();
    feedFlashIoWatchdog();
  }
  return true;
}

// Perform exactly one cache-off erase/program operation. The body is written
// first and the validity-bearing header last, so an interrupted replacement
// never supersedes the previous valid slot.
inline FlashStoreStepResult flashIoStoreStep(
    const esp_partition_t *part, const void *source,
    FlashStoreTransaction &tx,
    uint32_t lockTimeoutMs = FLASH_IO_LOCK_TIMEOUT_MS) {
  if (part == nullptr || source == nullptr ||
      tx.phase == FlashStoreTransaction::Phase::IDLE ||
      !tryLockFlashIo(lockTimeoutMs)) {
    return FlashStoreStepResult::FAILED;
  }
  bool ok = false;
  if (tx.phase == FlashStoreTransaction::Phase::ERASE) {
    ok = esp_partition_erase_range(part, tx.targetOffset + tx.offset,
                                   FLASH_IO_SECTOR_BYTES) == ESP_OK;
    if (ok) {
      tx.offset += FLASH_IO_SECTOR_BYTES;
      if (tx.offset == tx.eraseBytes) {
        tx.phase = FlashStoreTransaction::Phase::BODY;
        tx.offset = tx.headerBytes;
      }
    }
  } else {
    const uint8_t *bytes = static_cast<const uint8_t *>(source);
    const size_t remaining = tx.phase == FlashStoreTransaction::Phase::COMMIT
                                 ? tx.headerBytes
                                 : tx.totalBytes - tx.offset;
    const size_t chunk = remaining < FLASH_IO_CHUNK_BYTES
                             ? remaining
                             : FLASH_IO_CHUNK_BYTES;
    void *scratch = copyToFlashIoScratch(
        bytes + (tx.phase == FlashStoreTransaction::Phase::COMMIT ? 0
                                                                  : tx.offset),
        chunk);
    ok = scratch != nullptr &&
         esp_partition_write(
             part,
             tx.targetOffset +
                 (tx.phase == FlashStoreTransaction::Phase::COMMIT ? 0
                                                                    : tx.offset),
             scratch, chunk) == ESP_OK;
    if (ok && tx.phase == FlashStoreTransaction::Phase::COMMIT) {
      tx.phase = FlashStoreTransaction::Phase::IDLE;
    } else if (ok) {
      tx.offset += chunk;
      if (tx.offset == tx.totalBytes) {
        tx.phase = FlashStoreTransaction::Phase::COMMIT;
        tx.offset = 0;
      }
    }
  }
  unlockFlashIo();
  feedFlashIoWatchdog();
  if (!ok) {
    tx.phase = FlashStoreTransaction::Phase::IDLE;
    return FlashStoreStepResult::FAILED;
  }
  return tx.phase == FlashStoreTransaction::Phase::IDLE
             ? FlashStoreStepResult::COMPLETE
             : FlashStoreStepResult::MORE;
}
#endif

}  // namespace openbrewbyweight

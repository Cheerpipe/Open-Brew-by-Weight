#pragma once

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightPreferences.h"
#include "OpenBrewByWeightResetGuard.h"

namespace openbrewbyweight {

constexpr uint32_t RESET_HISTORY_STORE_MAGIC = 0x52534854UL;  // "RSHT"

struct ResetHistoryStoreBlob {
  uint32_t magic = RESET_HISTORY_STORE_MAGIC;
  uint8_t count = 0;
  uint8_t reserved[3] = {};
  ResetHistoryEntry entries[RESET_HISTORY_CAPACITY] = {};
  uint32_t currentUptimeMs = 0;
  uint32_t checksum = 0;
};
static_assert(sizeof(ResetHistoryStoreBlob) == 96, "reset history blob layout changed");

inline uint32_t resetHistoryStoreChecksum(const ResetHistoryStoreBlob &blob) {
  const uint32_t value =
      resetHistoryChecksum(blob.entries, blob.count,
                           RESET_HISTORY_STORE_MAGIC ^ blob.count);
  return (value * 16777619UL) ^ blob.currentUptimeMs;
}

// The NVS copy of the running uptime is only the fallback "uptime before
// reset" when a power loss wipes the RTC record (recordResetUptime keeps the
// one-minute RTC checkpoint for every other reset); five minutes between NVS
// writes bounds the wear of that diagnostic value.
constexpr uint32_t RESET_UPTIME_NVS_CHECKPOINT_INTERVAL_MS = 300000UL;

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
inline ResetHistoryStoreBlob resetHistoryStoreLive;
inline std::atomic<uint32_t> resetHistoryStoreLastCheckpointMs{0};
#endif

inline bool validResetHistoryStore(const ResetHistoryStoreBlob &blob) {
  return blob.magic == RESET_HISTORY_STORE_MAGIC &&
         blob.count <= RESET_HISTORY_CAPACITY &&
         blob.checksum == resetHistoryStoreChecksum(blob);
}

inline void finalizeResetHistoryStore(ResetHistoryStoreBlob &blob) {
  blob.magic = RESET_HISTORY_STORE_MAGIC;
  blob.checksum = resetHistoryStoreChecksum(blob);
}

inline void persistResetHistoryAfterBoot(SafetyResetSnapshot &snapshot) {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  (void)snapshot;
#else
  ResetHistoryStoreBlob stored = {};
  bool loaded = false;
  if (lockFlashIo()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::RESET_HISTORY);
    if (preferences.begin("rsthist", true)) {
      loaded = preferences.getBytesLength("history") == sizeof(stored) &&
               preferences.getBytes("history", &stored, sizeof(stored)) ==
                   sizeof(stored) &&
               validResetHistoryStore(stored);
      preferences.end();
    }
    unlockFlashIo();
  }

  ResetHistoryStoreBlob next = {};
  if (loaded) {
    if (snapshot.resetHistory[0].uptimeMs == 0)
      snapshot.resetHistory[0].uptimeMs = stored.currentUptimeMs;
    next = stored;
    const uint8_t count = next.count < RESET_HISTORY_CAPACITY
                              ? next.count + 1
                              : RESET_HISTORY_CAPACITY;
    for (int i = count - 1; i > 0; --i) next.entries[i] = next.entries[i - 1];
    next.entries[0] = snapshot.resetHistory[0];
    next.count = count;
  } else {
    next.count = snapshot.resetHistoryCount;
    for (uint8_t i = 0; i < next.count; ++i)
      next.entries[i] = snapshot.resetHistory[i];
  }
  next.currentUptimeMs = 0;
  finalizeResetHistoryStore(next);

  if (lockFlashIo()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::RESET_HISTORY);
    if (preferences.begin("rsthist", false)) {
      (void)preferences.putBytes("history", &next, sizeof(next));
      preferences.end();
    }
    unlockFlashIo();
  }

  snapshot.resetHistoryCount = next.count;
  for (uint8_t i = 0; i < next.count; ++i) snapshot.resetHistory[i] = next.entries[i];
  initializeSafetyResetRecord(SAFETY_RELAY_OPEN_MARKER,
                              snapshot.unsafeResetCount,
                              snapshot.resetHistory,
                              snapshot.resetHistoryCount);
  resetHistoryStoreLive = next;
  resetHistoryStoreLastCheckpointMs.store(0, std::memory_order_relaxed);
#endif
}

inline bool persistResetUptimeCheckpoint(uint32_t uptimeMs) {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  (void)uptimeMs;
  return true;
#else
  if (uptimeMs < RESET_UPTIME_NVS_CHECKPOINT_INTERVAL_MS ||
      uptimeMs - resetHistoryStoreLastCheckpointMs.load(
                     std::memory_order_relaxed) <
          RESET_UPTIME_NVS_CHECKPOINT_INTERVAL_MS) {
    return true;
  }
  if (!tryLockFlashIo(FLASH_IO_LOCK_TIMEOUT_MS)) return false;
  if (uptimeMs - resetHistoryStoreLastCheckpointMs.load(
                     std::memory_order_relaxed) <
      RESET_UPTIME_NVS_CHECKPOINT_INTERVAL_MS) {
    unlockFlashIo();
    return true;
  }
  ResetHistoryStoreBlob next = resetHistoryStoreLive;
  next.currentUptimeMs = uptimeMs;
  finalizeResetHistoryStore(next);
  OpenBrewByWeightPreferences preferences(NvsSubsystem::RESET_HISTORY);
  bool ok = false;
  if (preferences.begin("rsthist", false)) {
    ok = preferences.putBytes("history", &next, sizeof(next)) == sizeof(next);
    preferences.end();
  }
  if (ok) {
    resetHistoryStoreLive = next;
    resetHistoryStoreLastCheckpointMs.store(uptimeMs,
                                            std::memory_order_relaxed);
  }
  unlockFlashIo();
  return ok;
#endif
}

// Clears the diagnostic reset list from both durable NVS and RTC memory. The
// caller retains the current boot's reset reason separately, so clearing this
// list never removes the "Last reset" diagnostic value.
inline bool clearPersistedResetHistory(uint32_t unsafeResetCount) {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  initializeSafetyResetRecord(SAFETY_RELAY_OPEN_MARKER, unsafeResetCount);
  return true;
#else
  ResetHistoryStoreBlob next = {};
  finalizeResetHistoryStore(next);
  if (!tryLockFlashIo(FLASH_IO_LOCK_TIMEOUT_MS)) return false;
  OpenBrewByWeightPreferences preferences(NvsSubsystem::RESET_HISTORY);
  bool ok = false;
  if (preferences.begin("rsthist", false)) {
    ok = preferences.putBytes("history", &next, sizeof(next)) == sizeof(next);
    preferences.end();
  }
  if (ok) {
    initializeSafetyResetRecord(SAFETY_RELAY_OPEN_MARKER, unsafeResetCount);
    resetHistoryStoreLive = next;
    resetHistoryStoreLastCheckpointMs.store(0, std::memory_order_relaxed);
  }
  unlockFlashIo();
  if (!ok) return false;

  return true;
#endif
}

}  // namespace openbrewbyweight

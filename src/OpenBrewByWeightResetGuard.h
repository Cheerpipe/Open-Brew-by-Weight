#pragma once

#include <stdint.h>

#include "OpenBrewByWeightResetHistory.h"

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
#include <esp_attr.h>
#include <esp_system.h>
#endif

namespace openbrewbyweight {

constexpr uint32_t SAFETY_RESET_RECORD_MAGIC = 0x534E3947UL;
constexpr uint32_t SAFETY_RELAY_OPEN_MARKER = 0x4F50454EUL;
constexpr uint32_t SAFETY_RELAY_CLOSED_MARKER = 0x434C5344UL;
constexpr uint32_t SAFETY_BOOT_LOOP_THRESHOLD = 3;
constexpr uint32_t RESET_UPTIME_CHECKPOINT_INTERVAL_MS = 60000UL;
struct SafetyResetRecord {
  uint32_t magic;
  uint32_t magicInverse;
  uint32_t relayMarker;
  uint32_t relayMarkerInverse;
  uint32_t unsafeResetCount;
  uint32_t unsafeResetCountInverse;
  uint32_t historyCount;
  uint32_t historyCountInverse;
  ResetHistoryEntry history[RESET_HISTORY_CAPACITY];
  uint32_t historyChecksum;
  uint32_t currentUptimeMs;
  uint32_t currentUptimeMsInverse;
};

struct SafetyResetSnapshot {
  uint32_t reasonCode = 0;
  uint32_t unsafeResetCount = 0;
  bool resetDuringClose = false;
  bool unsafeReset = false;
  bool bootLoopDetected = false;
  uint8_t resetHistoryCount = 0;
  ResetHistoryEntry resetHistory[RESET_HISTORY_CAPACITY] = {};
};

namespace detail {

// The target definitions live in OpenBrewByWeightResetGuard.cpp so every consumer
// observes the same RTC record and checkpoint clock. C++17 inline variables
// provide the same one-instance semantics to header-only host harnesses.
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
extern volatile SafetyResetRecord safetyResetRecord;
extern uint32_t resetUptimeLastCheckpointMs;
#else
inline volatile SafetyResetRecord safetyResetRecord = {};
inline uint32_t resetUptimeLastCheckpointMs = 0;
#endif

}  // namespace detail

inline bool safetyResetRecordValid() {
  volatile SafetyResetRecord &safetyResetRecord = detail::safetyResetRecord;
  uint32_t checksum =
      resetHistoryChecksum(safetyResetRecord.history,
                           safetyResetRecord.historyCount <
                                   RESET_HISTORY_CAPACITY
                               ? safetyResetRecord.historyCount
                               : RESET_HISTORY_CAPACITY,
                           SAFETY_RESET_RECORD_MAGIC ^
                               safetyResetRecord.historyCount);
  return safetyResetRecord.magic == SAFETY_RESET_RECORD_MAGIC &&
         safetyResetRecord.magicInverse == ~SAFETY_RESET_RECORD_MAGIC &&
         safetyResetRecord.relayMarkerInverse ==
             ~safetyResetRecord.relayMarker &&
         safetyResetRecord.unsafeResetCountInverse ==
             ~safetyResetRecord.unsafeResetCount &&
         safetyResetRecord.historyCount <= RESET_HISTORY_CAPACITY &&
         safetyResetRecord.historyCountInverse == ~safetyResetRecord.historyCount &&
         safetyResetRecord.historyChecksum == checksum &&
         (safetyResetRecord.relayMarker == SAFETY_RELAY_OPEN_MARKER ||
          safetyResetRecord.relayMarker == SAFETY_RELAY_CLOSED_MARKER);
}

inline void initializeSafetyResetRecord(
    uint32_t relayMarker, uint32_t unsafeResetCount,
    const ResetHistoryEntry *history = nullptr, uint32_t historyCount = 0) {
  volatile SafetyResetRecord &safetyResetRecord = detail::safetyResetRecord;
  safetyResetRecord.magic = 0;
  safetyResetRecord.magicInverse = 0;
  safetyResetRecord.relayMarker = relayMarker;
  safetyResetRecord.relayMarkerInverse = ~relayMarker;
  safetyResetRecord.unsafeResetCount = unsafeResetCount;
  safetyResetRecord.unsafeResetCountInverse = ~unsafeResetCount;
  safetyResetRecord.historyCount = historyCount > RESET_HISTORY_CAPACITY ? RESET_HISTORY_CAPACITY : historyCount;
  safetyResetRecord.historyCountInverse = ~safetyResetRecord.historyCount;
  for (uint32_t i = 0; i < RESET_HISTORY_CAPACITY; ++i) {
    safetyResetRecord.history[i].reasonCode =
        history && i < safetyResetRecord.historyCount
            ? history[i].reasonCode
            : 0;
    safetyResetRecord.history[i].uptimeMs =
        history && i < safetyResetRecord.historyCount ? history[i].uptimeMs
                                                       : 0;
  }
  safetyResetRecord.historyChecksum =
      resetHistoryChecksum(safetyResetRecord.history,
                           safetyResetRecord.historyCount,
                           SAFETY_RESET_RECORD_MAGIC ^
                               safetyResetRecord.historyCount);
  safetyResetRecord.currentUptimeMs = 0;
  safetyResetRecord.currentUptimeMsInverse = UINT32_MAX;
  safetyResetRecord.magicInverse = ~SAFETY_RESET_RECORD_MAGIC;
  safetyResetRecord.magic = SAFETY_RESET_RECORD_MAGIC;
}

inline void recordRelayCommandedClosed(bool closed) {
  volatile SafetyResetRecord &safetyResetRecord = detail::safetyResetRecord;
  // setup() initializes the record before machine circuit can close. The fallback keeps
  // host/fault-injection calls deterministic without adding work to the ISR.
  if (!safetyResetRecordValid()) {
    initializeSafetyResetRecord(SAFETY_RELAY_OPEN_MARKER, 0);
  }
  const uint32_t marker = closed ? SAFETY_RELAY_CLOSED_MARKER
                                 : SAFETY_RELAY_OPEN_MARKER;
  safetyResetRecord.relayMarkerInverse = ~marker;
  safetyResetRecord.relayMarker = marker;
}

inline void recordResetUptime(uint32_t uptimeMs, bool force = false) {
  volatile SafetyResetRecord &safetyResetRecord = detail::safetyResetRecord;
  if (!force &&
      (uptimeMs < RESET_UPTIME_CHECKPOINT_INTERVAL_MS ||
       uptimeMs - detail::resetUptimeLastCheckpointMs <
           RESET_UPTIME_CHECKPOINT_INTERVAL_MS)) {
    return;
  }
  if (!safetyResetRecordValid()) return;
  safetyResetRecord.currentUptimeMsInverse = ~uptimeMs;
  safetyResetRecord.currentUptimeMs = uptimeMs;
  detail::resetUptimeLastCheckpointMs = uptimeMs;
}

inline uint32_t currentSafetyResetReasonCode() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  return hostSafetyResetReasonCode;
#else
  return static_cast<uint32_t>(esp_reset_reason());
#endif
}

inline bool currentSafetyResetIsUnsafe() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  return hostSafetyResetReasonUnsafe;
#else
  switch (esp_reset_reason()) {
    case ESP_RST_UNKNOWN:
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
    case ESP_RST_EFUSE:
    case ESP_RST_PWR_GLITCH:
    case ESP_RST_CPU_LOCKUP:
      return true;
    case ESP_RST_POWERON:
    case ESP_RST_EXT:
    case ESP_RST_SW:
    case ESP_RST_DEEPSLEEP:
    case ESP_RST_SDIO:
    case ESP_RST_USB:
    case ESP_RST_JTAG:
      return false;
  }
  return true;
#endif
}

inline bool currentSafetyResetIsPowerOn() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  return hostSafetyResetReasonPowerOn;
#else
  return esp_reset_reason() == ESP_RST_POWERON;
#endif
}

inline const char *safetyResetReasonName(uint32_t code) {
  switch (code) {
    case 0: return "Unknown";
    case 1: return "Power-on";
    case 2: return "External";
    case 3: return "Software";
    case 4: return "Panic";
    case 5: return "Interrupt WDT";
    case 6: return "Task WDT";
    case 7: return "Watchdog";
    case 8: return "Deep sleep";
    case 9: return "Brownout";
    case 10: return "SDIO";
    case 11: return "USB";
    case 12: return "JTAG";
    case 13: return "eFuse";
    case 14: return "Power glitch";
    case 15: return "CPU lockup";
  }
  return "Unknown";
}

inline SafetyResetSnapshot beginSafetyResetGuard() {
  volatile SafetyResetRecord &safetyResetRecord = detail::safetyResetRecord;
  SafetyResetSnapshot snapshot;
  const bool valid = safetyResetRecordValid();
  const bool resetDuringClose =
      valid &&
      safetyResetRecord.relayMarker == SAFETY_RELAY_CLOSED_MARKER;
  const uint32_t previousCount =
      valid ? safetyResetRecord.unsafeResetCount : 0;

  snapshot.reasonCode = currentSafetyResetReasonCode();
  const uint32_t previousUptimeMs =
      valid && safetyResetRecord.currentUptimeMsInverse ==
                   ~safetyResetRecord.currentUptimeMs
          ? safetyResetRecord.currentUptimeMs
          : 0;
  if (valid) {
    snapshot.resetHistoryCount = static_cast<uint8_t>(safetyResetRecord.historyCount);
    for (uint8_t i = 0; i < snapshot.resetHistoryCount; ++i) {
      snapshot.resetHistory[i].reasonCode =
          safetyResetRecord.history[i].reasonCode;
      snapshot.resetHistory[i].uptimeMs = safetyResetRecord.history[i].uptimeMs;
    }
  }
  const uint8_t oldCount = snapshot.resetHistoryCount;
  const uint8_t newCount = oldCount < RESET_HISTORY_CAPACITY ? oldCount + 1 : RESET_HISTORY_CAPACITY;
  for (int i = newCount - 1; i > 0; --i) snapshot.resetHistory[i] = snapshot.resetHistory[i - 1];
  snapshot.resetHistory[0] = {snapshot.reasonCode, previousUptimeMs};
  snapshot.resetHistoryCount = newCount;
  snapshot.resetDuringClose = resetDuringClose;
  // A torn or corrupted redundant record is unsafe after any warm reset. A
  // real power-on may legitimately start with undefined RTC no-init memory.
  snapshot.unsafeReset = currentSafetyResetIsUnsafe() || resetDuringClose ||
                         (!valid && !currentSafetyResetIsPowerOn());
  snapshot.unsafeResetCount =
      snapshot.unsafeReset && previousCount < UINT32_MAX
          ? previousCount + 1
          : (snapshot.unsafeReset ? previousCount : 0);
  snapshot.bootLoopDetected =
      snapshot.unsafeResetCount >= SAFETY_BOOT_LOOP_THRESHOLD;
  // Reset history is diagnostic only: setup() has already driven the relay
  // OPEN, so a panic or reset during close never latches the next boot or
  // requires a local recovery gesture. Publish that safe fact while retaining
  // the crash count.
  initializeSafetyResetRecord(SAFETY_RELAY_OPEN_MARKER, snapshot.unsafeResetCount,
                              snapshot.resetHistory, snapshot.resetHistoryCount);
  return snapshot;
}

// A pending recovery intent replays on every boot, so a crash inside the
// operation (panic, stack overflow, watchdog) would loop forever and brick
// the unit. Once a boot loop is latched, the latch must be dropped instead.
inline bool bootRecoveryMayResume(const SafetyResetSnapshot &reset) {
  return !reset.bootLoopDetected;
}

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
inline const volatile SafetyResetRecord &safetyResetRecordForHost() {
  return detail::safetyResetRecord;
}

inline void resetSafetyResetGuardForHost() {
  volatile SafetyResetRecord &safetyResetRecord = detail::safetyResetRecord;
  safetyResetRecord.magic = 0;
  safetyResetRecord.magicInverse = 0;
  safetyResetRecord.relayMarker = 0;
  safetyResetRecord.relayMarkerInverse = 0;
  safetyResetRecord.unsafeResetCount = 0;
  safetyResetRecord.unsafeResetCountInverse = 0;
  detail::resetUptimeLastCheckpointMs = 0;
  hostSafetyResetReasonCode = 1;
  hostSafetyResetReasonUnsafe = false;
  hostSafetyResetReasonPowerOn = true;
}
#endif

}  // namespace openbrewbyweight

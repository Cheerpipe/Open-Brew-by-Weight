#pragma once

#include "OpenBrewByWeightDomain.h"

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#endif

namespace openbrewbyweight {

enum class TimeSyncState : uint8_t {
  OFF = 0,
  SYNCING,
  SYNCED,
  FAILED,
  STALE
};

struct TimeStatusSnapshot {
  TimeSyncState state = TimeSyncState::OFF;
  uint32_t utcSec = 0;
  uint32_t lastSyncAgeMs = 0;
  uint32_t lastSyncUtcSec = 0;
  uint32_t nextRetryInMs = 0;
  uint8_t consecutiveFailures = 0;
  char activeServer[NTP_SERVER_HOST_CAPACITY] = {};
};

inline const char *timeSyncStateName(TimeSyncState state) {
  switch (state) {
    case TimeSyncState::OFF: return "OFF";
    case TimeSyncState::SYNCING: return "SYNCING";
    case TimeSyncState::SYNCED: return "SYNCED";
    case TimeSyncState::FAILED: return "FAILED";
    case TimeSyncState::STALE: return "STALE";
  }
  return "UNKNOWN";
}

// Takes the two NTP-related RuntimeConfig fields instead of the whole struct:
// callers snapshot them under dataMux_ because the control task rewrites
// settings_.runtime wholesale under that lock.
inline void resolveNtpServerHost(
    uint8_t ntpServerPreset,
    const char (&ntpServerCustom)[NTP_SERVER_HOST_CAPACITY],
    uint8_t failoverIndex, char output[NTP_SERVER_HOST_CAPACITY]) {
  const char *candidates[4] = {};
  size_t count = 0;
  if (ntpServerCustom[0] != '\0') {
    candidates[count++] = ntpServerCustom;
  }
  candidates[count++] = ntpPresetHostname(ntpServerPreset);
  candidates[count++] = "pool.ntp.org";
  candidates[count++] = "time.google.com";
  const char *selected = candidates[failoverIndex % count];
  copyCString(output, NTP_SERVER_HOST_CAPACITY, selected);
}

inline uint32_t ntpRetryDelayMs(uint8_t /*consecutiveFailures*/) {
  return NTP_UNSYNCED_RETRY_MS;
}

// millis() wraps at ~49.7 d. Unsigned subtraction is the wrap-safe elapsed.
// Remaining-until-deadline uses int32 so intervals stay < ~24.8 d (NTP retry
// is 15 s; stale/resync are 24 h / 1 h).
inline uint32_t monotonicElapsedMs(uint32_t nowMs, uint32_t thenMs) {
  return nowMs - thenMs;
}

inline uint32_t monotonicRemainingMs(uint32_t deadlineMs, uint32_t nowMs) {
  const int32_t remaining = static_cast<int32_t>(deadlineMs - nowMs);
  return remaining > 0 ? static_cast<uint32_t>(remaining) : 0U;
}

class WallClock {
 public:
  void reset(TimeSyncState state = TimeSyncState::OFF) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    state_ = state;
    anchorUtcSec_ = lastSyncUtcSec_ = 0;
    anchorMonotonicMs_ = 0;
    lastSyncMonotonicMs_ = 0;
    nextRetryAtMs_ = 0;
    consecutiveFailures_ = 0;
    activeServer_[0] = '\0';
    pendingSync_ = false;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  void queueSyncFromCallback(uint32_t utcSec) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    pendingUtcSec_ = utcSec;
    pendingSync_ = true;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  bool applyPendingSync(uint32_t monotonicMs) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    if (!pendingSync_ || pendingUtcSec_ < 1000000000U) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
      portEXIT_CRITICAL(&mux_);
#endif
      return false;
    }
    anchorUtcSec_ = lastSyncUtcSec_ = pendingUtcSec_;
    anchorMonotonicMs_ = monotonicMs;
    lastSyncMonotonicMs_ = monotonicMs;
    state_ = TimeSyncState::SYNCED;
    consecutiveFailures_ = 0;
    nextRetryAtMs_ = 0;
    pendingSync_ = false;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
    return true;
  }

  void setSyncing(const char *server, uint32_t monotonicMs) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    state_ = TimeSyncState::SYNCING;
    if (server != nullptr) {
      copyCString(activeServer_, sizeof(activeServer_), server);
    }
    nextRetryAtMs_ = 0;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
    (void)monotonicMs;
  }

  // Abort an in-flight SNTP attempt without counting a failure. Restores
  // SYNCED when a prior anchor exists; otherwise OFF.
  void cancelSyncing() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    pendingSync_ = false;
    nextRetryAtMs_ = 0;
    if (anchorUtcSec_ >= 1000000000U) {
      state_ = TimeSyncState::SYNCED;
    } else {
      state_ = TimeSyncState::OFF;
    }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  void cancelPendingSync() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    pendingSync_ = false;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  void markFailed(uint32_t monotonicMs, uint8_t maxConsecutiveFailures) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    state_ = TimeSyncState::FAILED;
    if (consecutiveFailures_ < maxConsecutiveFailures) {
      ++consecutiveFailures_;
    }
    const uint32_t delayMs = ntpRetryDelayMs(consecutiveFailures_);
    nextRetryAtMs_ = monotonicMs + delayMs;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  void markDisabled() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    state_ = TimeSyncState::OFF;
    pendingSync_ = false;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  // Moves the anchor forward before millis() can wrap past it (~49.7 d), and
  // clamps the sync age so a long-unsynced clock keeps reading as stale.
  void rebaseAnchor(uint32_t monotonicMs) {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    const uint32_t elapsedSec =
        monotonicElapsedMs(monotonicMs, anchorMonotonicMs_) / 1000U;
    if (anchorUtcSec_ >= 1000000000U && elapsedSec >= NTP_ANCHOR_REBASE_SEC) {
      anchorUtcSec_ += elapsedSec;
      anchorMonotonicMs_ += elapsedSec * 1000U;
      if (monotonicElapsedMs(monotonicMs, lastSyncMonotonicMs_) >
          NTP_STALE_AFTER_MS) {
        lastSyncMonotonicMs_ = monotonicMs - NTP_STALE_AFTER_MS - 1U;
      }
    }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
  }

  bool synced() const {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    // An anchor stays valid through resync attempts, failures and STA loss;
    // the snapshot reports staleness separately.
    const bool ready = anchorUtcSec_ >= 1000000000U;
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
    return ready;
  }

  uint32_t nowUtcSec(uint32_t monotonicMs) const {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    uint32_t utcSec = 0;
    if (anchorUtcSec_ >= 1000000000U) {
      utcSec = anchorUtcSec_ +
               monotonicElapsedMs(monotonicMs, anchorMonotonicMs_) / 1000U;
    }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
    return utcSec;
  }

  TimeStatusSnapshot snapshot(uint32_t monotonicMs) const {
    TimeStatusSnapshot output = {};
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portENTER_CRITICAL(&mux_);
#endif
    output.state = state_;
    output.lastSyncUtcSec = lastSyncUtcSec_;
    if (anchorUtcSec_ >= 1000000000U) {
      output.utcSec = anchorUtcSec_ +
                      monotonicElapsedMs(monotonicMs, anchorMonotonicMs_) /
                          1000U;
      output.lastSyncAgeMs =
          monotonicElapsedMs(monotonicMs, lastSyncMonotonicMs_);
      if (state_ == TimeSyncState::SYNCED &&
          output.lastSyncAgeMs > NTP_STALE_AFTER_MS) {
        output.state = TimeSyncState::STALE;
      }
    }
    output.consecutiveFailures = consecutiveFailures_;
    if (nextRetryAtMs_ != 0) {
      output.nextRetryInMs = monotonicRemainingMs(nextRetryAtMs_, monotonicMs);
    }
    copyCString(output.activeServer, sizeof(output.activeServer), activeServer_);
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    portEXIT_CRITICAL(&mux_);
#endif
    return output;
  }

 private:
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
  TimeSyncState state_ = TimeSyncState::OFF;
  uint32_t anchorUtcSec_ = 0;
  uint32_t lastSyncUtcSec_ = 0;
  uint32_t anchorMonotonicMs_ = 0;
  uint32_t lastSyncMonotonicMs_ = 0;
  uint32_t nextRetryAtMs_ = 0;
  uint8_t consecutiveFailures_ = 0;
  char activeServer_[NTP_SERVER_HOST_CAPACITY] = {};
  bool pendingSync_ = false;
  uint32_t pendingUtcSec_ = 0;
};

inline bool wallClockNeedsActivityNtpSync(const WallClock &clock,
                                          uint32_t monotonicMs) {
  const TimeStatusSnapshot snap = clock.snapshot(monotonicMs);
  return snap.state == TimeSyncState::OFF ||
         snap.state == TimeSyncState::FAILED;
}

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline WallClock g_wallClock;
#else
extern WallClock g_wallClock;
#endif

}  // namespace openbrewbyweight

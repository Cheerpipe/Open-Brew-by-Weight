#pragma once

// Data layer for the activation stores: the stats shot log, its flash curve
// sidecar, and the independent activation history. One explicit owner for the
// three PSRAM ring stores and their deferred flash persistence.
//
// Concurrency contract:
// 1. Every read or write of shotLog/shotCurves/historyLog (append in the
//    control loop, HTTP queries and mutations, deferred flush, delete/clear)
//    runs under the caller's TaskLockGuard(shotStoreMutex). This component
//    takes no hidden internal locks, so it can never recurse into that mutex.
// 2. Control captures a selective immutable image under that mutex; the worker
//    owns its stepped flash transaction after the mutex is released.
// 3. No FreeRTOS task or ISR is created here. Live stores are touched only
//    from control and network through the mutex; the worker touches its image.
// 4. Flash reads/writes copy through the internal-SRAM FlashIoScratch while
//    a flash path may still disable cache despite XIP; PSRAM is not passed to
//    partition I/O. Multi-word records are only read or rewritten under the
//    store mutex, so readers never observe a torn record.
// 5. begin() completes all loads before the network task serves endpoints;
//    callers invoke it under the store mutex for uniformity.

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightHistory.h"
#include "OpenBrewByWeightPreferences.h"
#include "OpenBrewByWeightShotCurve.h"
#include "OpenBrewByWeightShotLog.h"

namespace openbrewbyweight {

// The boot identity: one per boot, never 0, in its own small NVS key next to
// the reset history, so a boot never rewrites the shot log. Returns whether
// the advanced value is durable.
inline bool advanceBootId(uint32_t &bootId) {
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                       \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  if (bootId < UINT32_MAX) ++bootId;
  return true;
#else
  uint32_t next = 0;  // Stack copy: flash I/O must not read PSRAM.
  bool saved = false;
  if (lockFlashIo()) {
    OpenBrewByWeightPreferences preferences(NvsSubsystem::RESET_HISTORY);
    if (preferences.begin("rsthist", false)) {
      if (preferences.getBytesLength("bootId") != sizeof(next) ||
          preferences.getBytes("bootId", &next, sizeof(next)) != sizeof(next))
        next = 0;
      if (next < UINT32_MAX) ++next;
      saved = preferences.putBytes("bootId", &next, sizeof(next)) == sizeof(next);
      preferences.end();
    }
    unlockFlashIo();
  }
  if (next != 0) bootId = next;
  return saved;
#endif
}

// Latched SHOT_LOG_PERSIST_FAILED emission stays with the component so every
// store shares one wiring; the scheduler (retry/backoff) stays with the
// control-loop servant that calls service().
using ActivationStoreEvent =
    void (*)(DebugCategory category, DebugCode code, int32_t argument1,
             int32_t argument2);

struct ActivationStoresFlushReport {
  bool anyFail = false;
  bool anyIoFail = false;
  bool complete = true;
};

// Live stores and the worker's persistence image differ only in the curve
// log: the image carries a bounded batch of unpersisted curves.
template <typename CurveLog>
class BasicActivationStores {
 public:
  // Boot load. Returns whether the boot identity advanced durably. Call
  // before the network task serves endpoints; lastShotStore is loaded
  // separately by the platform boot.
  bool begin() {
    shotLog.load();
    shotCurves.load();
    historyLog.load();
    return advanceBootId(bootId_);
  }

  // Written only by begin(), before any other task reads it: lock-free.
  uint32_t bootId() const { return bootId_; }

  // Advance at most one erase/program operation. This keeps every cache-off
  // window bounded and lets the worker re-check safety gates between steps.
  ActivationStoresFlushReport serviceStep(uint32_t tryLockMs,
                                           ActivationStoreEvent emit) {
    ActivationStoresFlushReport report;
    while (persistCursor_ < 3) {
      FlashStoreStepResult result = FlashStoreStepResult::COMPLETE;
      bool *latched = nullptr;
      bool hadDirty = false;
      int32_t discriminator = 0;
      size_t count = 0;
      const uint32_t lockTimeoutsBefore = flashIoLockTimeouts();
      if (persistCursor_ == 0) {
        hadDirty = shotLog.dirty();
        result = shotLog.flushStep(tryLockMs);
        latched = &shotLogPersistFailLatched_;
        count = shotLog.count();
      } else if (persistCursor_ == 1) {
        hadDirty = shotCurves.dirty();
        result = shotCurves.flushStep(tryLockMs);
        latched = &shotCurvePersistFailLatched_;
        discriminator = 1;
        count = shotCurves.count();
      } else {
        hadDirty = historyLog.dirty();
        result = historyLog.flushStep(tryLockMs);
        latched = &historyPersistFailLatched_;
        discriminator = 3;
        count = historyLog.count();
      }
      if (result == FlashStoreStepResult::FAILED) {
        report.anyFail = true;
        report.anyIoFail = flashIoLockTimeouts() == lockTimeoutsBefore;
        if (!*latched && emit != nullptr) {
          *latched = true;
          emit(DebugCategory::CONFIG, DebugCode::SHOT_LOG_PERSIST_FAILED,
               static_cast<int32_t>(count), discriminator);
        }
        persistCursor_ = 0;
        return report;
      }
      if (result == FlashStoreStepResult::MORE) {
        report.complete = false;
        return report;
      }
      *latched = false;
      ++persistCursor_;
      if (persistCursor_ < 3 && hadDirty) {
        report.complete = false;
        return report;
      }
    }
    persistCursor_ = 0;
    return report;
  }

  ShotLog shotLog;
  CurveLog shotCurves;
  HistoryLog historyLog;

  // Harness support: reset and inspect the latched failure dedup state.
  void clearPersistFailLatches() {
    shotLogPersistFailLatched_ = false;
    shotCurvePersistFailLatched_ = false;
    historyPersistFailLatched_ = false;
  }

  bool anyPersistFailLatched() const {
    return shotLogPersistFailLatched_ || shotCurvePersistFailLatched_ ||
           historyPersistFailLatched_;
  }

  void capturePersistenceImage(BasicActivationStores<ShotCurvePersistImage> &image) const {
    shotLog.capturePersistenceImage(image.shotLog);
    shotCurves.capturePersistenceImage(image.shotCurves);
    historyLog.capturePersistenceImage(image.historyLog);
    image.shotLogPersistFailLatched_ = shotLogPersistFailLatched_;
    image.shotCurvePersistFailLatched_ = shotCurvePersistFailLatched_;
    image.historyPersistFailLatched_ = historyPersistFailLatched_;
    image.persistCursor_ = 0;
  }

  void acknowledgePersisted(const BasicActivationStores<ShotCurvePersistImage> &image,
                            bool clearDirty) {
    shotLog.acknowledgePersisted(image.shotLog, clearDirty);
    shotCurves.acknowledgePersisted(image.shotCurves, clearDirty);
    historyLog.acknowledgePersisted(image.historyLog, clearDirty);
    if (clearDirty) {
      shotLogPersistFailLatched_ = image.shotLogPersistFailLatched_;
      shotCurvePersistFailLatched_ = image.shotCurvePersistFailLatched_;
      historyPersistFailLatched_ = image.historyPersistFailLatched_;
    }
  }

 private:
  bool shotLogPersistFailLatched_ = false;
  bool shotCurvePersistFailLatched_ = false;
  bool historyPersistFailLatched_ = false;
  uint8_t persistCursor_ = 0;
  uint32_t bootId_ = 1;

  template <typename> friend class BasicActivationStores;
};

using ActivationStores = BasicActivationStores<ShotCurveLog>;
using ActivationStoresPersistImage = BasicActivationStores<ShotCurvePersistImage>;

}  // namespace openbrewbyweight

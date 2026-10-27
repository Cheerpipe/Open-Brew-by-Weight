#pragma once

#include "OpenBrewByWeightBbwQuickStart.h"
#include "OpenBrewByWeightBleScanPersistence.h"
#include "OpenBrewByWeightFirmwareMode.h"
#include "OpenBrewByWeightHistory.h"
#include "OpenBrewByWeightLastShot.h"
#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightShotCurve.h"
#include "OpenBrewByWeightShotLog.h"
#include "OpenBrewByWeightShotTracePool.h"
#include "OpenBrewByWeightCrashArchive.h"
#include "OpenBrewByWeightAuditStore.h"
#include "OpenBrewByWeightWebAccessStore.h"

namespace openbrewbyweight {

inline bool verifyFactorySettings(const PersistedSettings &settings) {
  if (!validPersistedSettings(settings) || settings.staConfigured ||
      settings.lkgValid || !passwordIsFactoryDefault(settings) ||
      settings.preferredScaleMac[0] != '\0' ||
      settings.preferredScaleName[0] != '\0' ||
      settings.runtime.scaleMacCacheMode !=
          static_cast<uint8_t>(ScaleMacCacheMode::PREFER)) {
    return false;
  }
  for (const ScaleHistoryEntry &entry : settings.scaleHistory) {
    if (entry.mac[0] != '\0' || entry.name[0] != '\0' ||
        entry.friendlyName[0] != '\0' || entry.lastSeenSeq != 0) {
      return false;
    }
  }
  return true;
}

// Boot recovery only: setup() reloads settings from NVS afterwards, so the
// caller's copy is the working copy (a second ~3.4 KB one overflowed loopTask).
inline bool resetPersistedNetworkAccess(PersistedSettings &settings) {
  if (!settingsSchemaWritesAdmitted.load(std::memory_order_acquire)) return false;
  if (!loadPersistedSettings(settings) && !initializeDefaultSettings(settings)) {
    return false;
  }
  clearStaNetwork(settings);
  return initializeDefaultDevicePassword(settings) &&
         savePersistedSettings(settings) && loadPersistedSettings(settings) &&
         !settings.staConfigured && !settings.lkgValid &&
         settings.staIpMode == static_cast<uint8_t>(StaIpMode::DHCP) &&
         passwordIsFactoryDefault(settings);
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
// Boot half of the onboarding transaction: rolls the "by weight" choice
// forward once its Wi-Fi is durable (starting preset, then the firmware mode,
// then the record), drops it when that Wi-Fi was reverted, and keeps it while
// the Wi-Fi is still pending. Each step is idempotent, so an interrupted
// roll-forward resumes on the next boot. Runs before the mode is latched.
inline void resolveSetupIntent(PersistedSettings &settings) {
  SetupIntent intent;
  if (!loadSetupIntent(intent)) return;
  const SetupIntentVerdict verdict = setupIntentVerdict(intent, settings);
  if (verdict == SetupIntentVerdict::WAIT) return;
  if (verdict == SetupIntentVerdict::APPLY) {
    if (setActiveShotPreset(settings.presets, intent.presetId))
      applyShotPresetToConfig(activeShotPreset(settings.presets), settings.runtime);
    if (!savePersistedSettings(settings) || !saveFirmwareMode(FirmwareMode::FULL))
      return;
  }
  (void)clearSetupIntent();
}
#endif

// Erase only expendable NVS records. This is the narrow recovery step used
// when a factory-reset intent cannot be committed because NVS is full. The
// shot log and activation history live in their own flash partitions and are
// cleared by resetAllDurableStores below.
inline bool releaseNvsSpaceForFactoryReset(LastShotStore &lastShot) {
  return lastShot.erasePersisted();
}

// Erase the flash-backed activation stores first, then
// overwrite dual-slot settings without clearing the shared NVS namespace,
// BLE scan settings last. Every store is verified before success. Idempotent
// except that a mid-fail may already have dropped history.
// LastShotStore owns the status aggregate. Callers only need to drop transient
// dirty state after this durable reset succeeds. The shot trace pool, when
// given, is cleared and verified by a fresh recovery; the caller holds its
// writer mutex.
inline bool resetAllDurableStores(PersistedSettings &settings,
                                  BleScanPersistedSettings &ble,
                                  ShotLog &shotLog,
                                  HistoryLog &historyLog,
                                  LastShotStore &lastShot,
                                  ShotCurveLog &shotCurves,
                                  ShotTracePool *shotTraces = nullptr) {
  if (!settingsSchemaWritesAdmitted.load(std::memory_order_acquire)) return false;
  yieldFlashIo();
  feedFlashIoWatchdog();
  // Drop the last-shot NVS record first so the NVS partition has room for
  // factory settings writes. A later failure may already have erased it;
  // settings stay until resetPersistedSettingsToFactory succeeds.
  if (!releaseNvsSpaceForFactoryReset(lastShot)) {
    return false;
  }
  yieldFlashIo();
  feedFlashIoWatchdog();
  if (!shotLog.clear() || !historyLog.clear() || !shotCurves.clear() ||
      !lastShot.clear() || (shotTraces != nullptr && !shotTraces->clear())) {
    return false;
  }
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  if (!crashArchiveClear()) return false;
  if (!clearAuditStores()) return false;
#endif
  yieldFlashIo();
  feedFlashIoWatchdog();
  if (!resetPersistedSettingsToFactory(settings)) {
    return false;
  }
  yieldFlashIo();
  feedFlashIoWatchdog();
  if (!resetBleScanSettings(ble)) {
    return false;
  }
  // Factory reset clears the master switch too, so the controller comes back
  // transparent (compatibility mode) and offers onboarding again; without a
  // quick start record the next boot quick starts every factory preset.
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  if (!clearFirmwareMode() || !eraseWebAccessStored() ||
      !eraseSettingsNvsKey(NvsSubsystem::SETTINGS, QUICK_START_KEY)) {
    return false;
  }
#endif

  yieldFlashIo();
  feedFlashIoWatchdog();
  // Read back into the caller's copy, which already holds the factory values:
  // a second ~3.4 KB PersistedSettings overflowed the 8 KiB loopTask when a
  // pending factory reset resumes during setup().
  BleScanPersistedSettings verifiedBle;
  const bool shotLogVerified = shotLog.load() && shotLog.count() == 0;
  const bool historyVerified = historyLog.load() && historyLog.count() == 0;
  const bool shotCurvesVerified = shotCurves.load() && shotCurves.count() == 0;
  const bool lastShotVerified = lastShot.load() && !lastShot.get().valid &&
                                !lastShot.getGood().valid;
  const bool shotTracesVerified =
      shotTraces == nullptr || (shotTraces->recover() && shotTraces->count() == 0);
  return loadPersistedSettings(settings) &&
         verifyFactorySettings(settings) &&
         readLatestBleScanSettings(verifiedBle) &&
         verifyFactoryBleScanSettings(verifiedBle) &&
         shotLogVerified && historyVerified && shotCurvesVerified &&
         lastShotVerified && shotTracesVerified;
}

}  // namespace openbrewbyweight

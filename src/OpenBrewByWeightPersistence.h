#pragma once

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightNvsDualSlot.h"
#include "OpenBrewByWeightPersistedNetwork.h"
#include "OpenBrewByWeightPersistedSettings.h"
#include "OpenBrewByWeightPreferences.h"
#include "OpenBrewByWeightSettingsCodec.h"
#include "OpenBrewByWeightSettingsWriteAdmission.h"

#include <stdint.h>

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) &&                                        \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace openbrewbyweight {

inline void initializeSettingsSchemaWriteAdmission() {
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (!ensureFlashIoMutex() || !lockFlashIo()) return;
  const esp_err_t error = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
  unlockFlashIo();
  settingsSchemaWritesAdmitted.store(error == ESP_ERR_NOT_FOUND ||
      (error == ESP_OK && (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED)),
      std::memory_order_release);
#endif
}

inline void ensurePersistedPresetBank(PersistedSettings &settings) {
  ensureShotPresetBank(settings.presets, settings.runtime.retareWindowMs,
                       settings.runtime.autoRetare);
}

inline bool validPersistedSettings(const PersistedSettings &settings) {
  if (validateRuntimeConfig(settings.runtime) != ConfigValidationError::NONE ||
      settings.runtime.timezoneSource >= 3 ||
      settings.runtime.timezoneAutomatic > 1 ||
      !validBullseyeMelodyConfig(settings.bullseyeMelody) ||
      !validateShotPresetBank(settings.presets, settings.runtime.retareWindowMs,
                              settings.runtime.autoRetare) ||
      !validDevicePassword(settings.devicePassword) ||
      !validDeviceName(settings.deviceName) ||
      !validLineaMicraSettings(settings.lineaMicra) ||
      !validPreferredScaleMac(settings.preferredScaleMac) ||
      !validPreferredScaleName(settings.preferredScaleName) ||
      !validScaleHistoryEntries(settings.scaleHistory) ||
      !validWebhookConfig(settings.webhook) ||
      !validPersistedStaNetwork(settings)) {
    return false;
  }
  return true;
}

inline void finalizePersistedSettings(PersistedSettings &settings) {
  ensurePersistedPresetBank(settings);
  uint32_t seedSeq = 0;
  for (auto & i : settings.scaleHistory) {
    if (i.lastSeenSeq > seedSeq) {
      seedSeq = i.lastSeenSeq;
    }
  }
  seedScaleHistoryFromPreferred(settings.scaleHistory, seedSeq,
                                settings.preferredScaleMac,
                                settings.preferredScaleName);
}

// An intact blob (checksum and schema match) whose content no longer
// validates, for example a field a newer image checks more strictly: reset
// only the failing sections to factory values so Wi-Fi, the device password
// and every valid section survive. Returns true when something changed.
inline bool repairPersistedSettings(PersistedSettings &s) {
  bool repaired = false;
  const auto reset = [&repaired](auto &section) {
    section = std::remove_reference_t<decltype(section)>{};
    repaired = true;
  };
  RuntimeConfig &r = s.runtime;
  if (r.scheduledRestartEnabled > 1 || r.scheduledRestartMinuteOfDay >= 24 * 60) {
    r.scheduledRestartEnabled = RuntimeConfig{}.scheduledRestartEnabled;
    r.scheduledRestartMinuteOfDay = RuntimeConfig{}.scheduledRestartMinuteOfDay;
    repaired = true;
  }
  if (validateRuntimeConfig(r) != ConfigValidationError::NONE ||
      r.timezoneSource >= 3 || r.timezoneAutomatic > 1) {
    reset(r);
  }
  if (!validateShotPresetBank(s.presets, r.retareWindowMs, r.autoRetare)) reset(s.presets);
  if (!validBullseyeMelodyConfig(s.bullseyeMelody)) reset(s.bullseyeMelody);
  if (!validLineaMicraSettings(s.lineaMicra)) reset(s.lineaMicra);
  if (!validWebhookConfig(s.webhook)) reset(s.webhook);
  if (!validPreferredScaleMac(s.preferredScaleMac) ||
      !validPreferredScaleName(s.preferredScaleName) ||
      !validScaleHistoryEntries(s.scaleHistory)) {
    memset(s.preferredScaleMac, 0, sizeof(s.preferredScaleMac));
    memset(s.preferredScaleName, 0, sizeof(s.preferredScaleName));
    memset(s.scaleHistory, 0, sizeof(s.scaleHistory));
    repaired = true;
  }
  if (!validDevicePassword(s.devicePassword)) repaired |= initializeDefaultDevicePassword(s);
  if (!validDeviceName(s.deviceName)) repaired |= initializeDefaultDeviceName(s);
  if (repaired) finalizePersistedSettings(s);
  return repaired;
}

inline bool lockSettingsNvs() { return lockFlashIo(); }
inline void unlockSettingsNvs() { unlockFlashIo(); }
inline void yieldSettingsNvs() { yieldFlashIo(); }
inline void feedSettingsNvsWatchdog() { feedFlashIoWatchdog(); }

// One encoded blob staged in the shared flash-I/O scratch (see
// FlashIoScratch), allocated from internal SRAM: source/destination of
// Preferences putBytes/getBytes if a flash path disables cache despite XIP.
inline uint8_t *settingsBlobScratch() {
  static_assert(PERSISTED_SETTINGS_NVS_BUDGET <= FLASH_IO_SCRATCH_BYTES,
                "Settings blob exceeds flash I/O buffer");
  return flashIoScratchBytes();
}

// Reads one slot into the scratch and checks its framing and CRC; returns
// the blob length (0 when absent, torn, foreign or oversized).
__attribute__((noinline))
inline size_t readSettingsBlob(OpenBrewByWeightPreferences &preferences, const char *key,
                               uint32_t &revision) {
  const size_t stored = preferences.isKey(key) ? preferences.getBytesLength(key) : 0;
  uint8_t *blob = settingsBlobScratch();
  if (stored < SETTINGS_BLOB_MIN_BYTES || stored > PERSISTED_SETTINGS_NVS_BUDGET ||
      preferences.getBytes(key, blob, stored) != stored ||
      !settingsBlobIntact(blob, stored, revision)) {
    return 0;
  }
  return stored;
}

// Decodes one slot over factory defaults. Without `repair` it succeeds only
// when every value decoded and validates; with it, failing sections are reset
// as repairPersistedSettings does. An unusable STA record never loads.
__attribute__((noinline))
inline bool readSettingsSlot(OpenBrewByWeightPreferences &preferences, const char *key,
                             PersistedSettings &settings, bool repair = false) {
  uint32_t revision = 0;
  const size_t length = readSettingsBlob(preferences, key, revision);
  if (length == 0) return false;
  new (&settings) PersistedSettings{};
  uint32_t malformed = 0;
  if (!decodePersistedSettings(settingsBlobScratch(), length, settings, malformed)) {
    return false;
  }
  settings.storageRevision = revision;
  if (!validPersistedStaNetwork(settings)) return false;
  if (!repair) return malformed == 0 && validPersistedSettings(settings);
  (void)repairPersistedSettings(settings);
  return validPersistedSettings(settings);
}

// Encodes `settings` at `revision` into one slot and verifies the read-back
// blob (length, revision and CRC).
inline bool writeSettingsSlot(OpenBrewByWeightPreferences &preferences, const char *key,
                              const PersistedSettings &settings, uint32_t revision) {
  uint8_t *blob = settingsBlobScratch();
  const size_t length =
      encodePersistedSettings(settings, revision, blob, PERSISTED_SETTINGS_NVS_BUDGET);
  if (length == 0) return false;
  uint32_t crc = 0;
  memcpy(&crc, blob + length - 4U, 4);
  yieldSettingsNvs();
  feedSettingsNvsWatchdog();
  if (preferences.putBytes(key, blob, length) != length) return false;
  uint32_t storedRevision = 0, storedCrc = 0;
  if (readSettingsBlob(preferences, key, storedRevision) != length) return false;
  memcpy(&storedCrc, blob + length - 4U, 4);
  return storedRevision == revision && storedCrc == crc;
}

// Removes one key of the settings namespace; an absent key already counts.
inline bool eraseSettingsNvsKey(NvsSubsystem subsystem, const char *key) {
  if (!lockSettingsNvs()) return false;
  OpenBrewByWeightPreferences preferences(subsystem);
  bool erased = false;
  if (preferences.begin(SETTINGS_NAMESPACE, false)) {
    erased = !preferences.isKey(key) || preferences.remove(key);
    preferences.end();
  }
  unlockSettingsNvs();
  return erased;
}

inline bool savePersistedSettings(PersistedSettings &settings);

// Revisions of the intact slots, newest first; returns how many there are.
inline uint8_t settingsSlotsNewestFirst(OpenBrewByWeightPreferences &preferences,
                                        const char *(&keys)[2], uint32_t *newest = nullptr) {
  uint32_t revisionA = 0, revisionB = 0;
  const bool a = readSettingsBlob(preferences, SETTINGS_SLOT_A, revisionA) != 0;
  const bool b = readSettingsBlob(preferences, SETTINGS_SLOT_B, revisionB) != 0;
  const bool bFirst = b && (!a || secondRevisionIsNewer(revisionA, revisionB));
  keys[0] = bFirst ? SETTINGS_SLOT_B : SETTINGS_SLOT_A;
  keys[1] = bFirst ? SETTINGS_SLOT_A : SETTINGS_SLOT_B;
  if (newest != nullptr) *newest = bFirst ? revisionB : revisionA;
  return static_cast<uint8_t>(a) + static_cast<uint8_t>(b);
}

// Loads the newest slot that decodes and validates as is; failing that, the
// newest one that repairs. `settings` is overwritten even when nothing loads.
inline bool loadPersistedSettings(PersistedSettings &settings,
                                  bool *repaired = nullptr) {
  if (!lockSettingsNvs()) {
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
  if (!preferences.begin(SETTINGS_NAMESPACE, true)) {
    unlockSettingsNvs();
    return false;
  }
  const char *keys[2] = {};
  const uint8_t intact = settingsSlotsNewestFirst(preferences, keys);
  bool loaded = false, loadedAsIs = false;
  for (const bool repair : {false, true}) {
    for (uint8_t i = 0; i < intact && !loaded; ++i) {
      loaded = readSettingsSlot(preferences, keys[i], settings, repair);
    }
    if (loaded) {
      loadedAsIs = !repair;
      break;
    }
  }
  if (repaired != nullptr) *repaired = loaded && !loadedAsIs;
  preferences.end();
  if (loaded) durableTimezoneSaved().store(settings.runtime.timezoneId[0] != '\0');
  unlockSettingsNvs();
  return loaded;
}

inline void overlayLivePersistedSettings(PersistedSettings &settings,
                                         const RuntimeConfig &runtime,
                                         const ShotPresetBank &presets) {
  settings.runtime = runtime;
  settings.presets = presets;
}

inline uint32_t &durableStorageRevision() {
  static uint32_t revision = 0;
  return revision;
}

inline bool &durableStorageRevisionValid() {
  static bool valid = false;
  return valid;
}

inline void noteDurableStorageRevision(uint32_t revision) {
  if (!lockSettingsNvs()) {
    return;
  }
  durableStorageRevision() = revision;
  durableStorageRevisionValid() = revision != 0;
  unlockSettingsNvs();
}

inline void resetDurableStorageRevision() {
  if (!lockSettingsNvs()) {
    return;
  }
  durableStorageRevision() = 0;
  durableStorageRevisionValid() = false;
  durableTimezoneSaved().store(false);
  unlockSettingsNvs();
}

inline bool savePersistedSettings(PersistedSettings &settings) {
  if (!settingsSchemaWritesAdmitted.load(std::memory_order_acquire)) return false;
  // The shared scratch is reused for revision probes, the candidate blob and
  // read-back verification. Never call loadPersistedSettings while locked.
  yieldSettingsNvs();
  feedSettingsNvsWatchdog();
  if (!lockSettingsNvs()) {
    feedSettingsNvsWatchdog();
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    unlockSettingsNvs();
    feedSettingsNvsWatchdog();
    return false;
  }
  uint32_t revision = settings.storageRevision;
  if (durableStorageRevisionValid()) {
    revision = durableStorageRevision();
  } else if (revision == 0) {
    const char *keys[2] = {};
    (void)settingsSlotsNewestFirst(preferences, keys, &revision);
  }
  uint32_t candidateRevision = revision + 1U;
  if (candidateRevision == 0) candidateRevision = 1;
  finalizePersistedSettings(settings);
  const char *target = (candidateRevision & 1U) ? SETTINGS_SLOT_A : SETTINGS_SLOT_B;
  const bool saved = writeSettingsSlot(preferences, target, settings, candidateRevision);
  preferences.end();
  if (saved) {
    settings.storageRevision = candidateRevision;
    durableStorageRevision() = candidateRevision;
    durableStorageRevisionValid() = true;
    durableTimezoneSaved().store(settings.runtime.timezoneId[0] != '\0');
  }
  unlockSettingsNvs();
  yieldSettingsNvs();
  feedSettingsNvsWatchdog();
  return saved;
}

inline bool initializeDefaultSettings(PersistedSettings &settings) {
  // Construct in place: assigning PersistedSettings{} stages a ~3.4 KB
  // temporary on the stack of the boot and network factory-reset paths.
  new (&settings) PersistedSettings{};
  if (!initializeDefaultDevicePassword(settings) ||
      !initializeDefaultDeviceName(settings)) {
    return false;
  }
  finalizePersistedSettings(settings);
  return true;
}

inline bool resetPersistedSettingsToFactory(PersistedSettings &settings) {
  if (!settingsSchemaWritesAdmitted.load(std::memory_order_acquire)) return false;
  if (!lockSettingsNvs()) {
    return false;
  }
  OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    unlockSettingsNvs();
    return false;
  }

  uint32_t existingMax = 0;
  for (const char *key : {SETTINGS_SLOT_A, SETTINGS_SLOT_B}) {
    uint32_t revision = 0;
    if (readSettingsBlob(preferences, key, revision) != 0 && revision > existingMax) {
      existingMax = revision;
    }
  }
  if (durableStorageRevisionValid() &&
      durableStorageRevision() > existingMax) {
    existingMax = durableStorageRevision();
  }
  if (existingMax > UINT32_MAX - 2U) {
    existingMax = UINT32_MAX - 2U;
  }

  if (!initializeDefaultSettings(settings)) {
    preferences.end();
    unlockSettingsNvs();
    return false;
  }
  const bool firstVerified =
      writeSettingsSlot(preferences, SETTINGS_SLOT_A, settings, existingMax + 1U);
  const bool secondVerified =
      writeSettingsSlot(preferences, SETTINGS_SLOT_B, settings, existingMax + 2U);
  preferences.end();
  unlockSettingsNvs();

  if (!firstVerified && !secondVerified) {
    return false;
  }
  settings.storageRevision = existingMax + (secondVerified ? 2U : 1U);
  noteDurableStorageRevision(settings.storageRevision);
  durableTimezoneSaved().store(false);
  return true;
}

}  // namespace openbrewbyweight

#pragma once

#include "OpenBrewByWeightFlashIoScratch.h"
#include "OpenBrewByWeightNvsDualSlot.h"
#include "OpenBrewByWeightPersistedNetwork.h"
#include "OpenBrewByWeightPersistedSettings.h"
#include "OpenBrewByWeightPreferences.h"
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

inline bool persistedSettingsIntact(const PersistedSettings &settings) {
  return settings.magic == PERSISTED_SETTINGS_MAGIC &&
         settings.schemaVersion == CONFIG_SCHEMA_VERSION &&
         settings.structureSize == sizeof(PersistedSettings) &&
         settings.checksum == persistedSettingsChecksum(settings);
}

inline bool validPersistedSettings(const PersistedSettings &settings) {
  if (!persistedSettingsIntact(settings) ||
      validateRuntimeConfig(settings.runtime) != ConfigValidationError::NONE ||
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
  settings.magic = PERSISTED_SETTINGS_MAGIC;
  settings.schemaVersion = CONFIG_SCHEMA_VERSION;
  settings.structureSize = sizeof(PersistedSettings);
  settings.checksum = 0;
  settings.checksum = persistedSettingsChecksum(settings);
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

// Single-record staging inside the shared flash-I/O scratch (see FlashIoScratch).
// Allocated from internal SRAM (heap, not BSS): source/destination of
// Preferences putBytes/getBytes if a flash path disables cache despite XIP.
inline PersistedSettings &persistedSettingsScratch() {
  static_assert(sizeof(PersistedSettings) <= FLASH_IO_SCRATCH_BYTES,
                "PersistedSettings exceeds flash I/O buffer");
  return *reinterpret_cast<PersistedSettings *>(flashIoScratchBytes());
}

// Shared by the load/save/reset/revision-probe paths; must stay out-of-line
// so the compiler does not embed one copy at every call site.
// A shorter blob of the current schema, written by an earlier image before
// tail fields were appended, loads as a prefix of the current struct with the
// appended fields zeroed once its own checksum verifies (the last word, or the
// one before 4 B of alignment padding). Appended fields must therefore take
// zero as their legacy default. The oldest such layout ends right after
// deviceName, where the Micra record now starts; anything shorter, longer,
// torn or of another schema is rejected. Only size and checksum are restamped
// so the stored values reach validation untouched. Load never writes back;
// the next save stores the full layout.
constexpr size_t PERSISTED_SETTINGS_MIN_PREFIX_BYTES =
    (offsetof(PersistedSettings, lineaMicra) + sizeof(uint32_t) - 1) / sizeof(uint32_t) *
        sizeof(uint32_t) +
    sizeof(uint32_t);
__attribute__((noinline))
inline bool readSettingsSlot(OpenBrewByWeightPreferences &preferences, const char *key,
                             PersistedSettings &settings, bool repair = false) {
  const size_t stored = preferences.isKey(key)
                            ? preferences.getBytesLength(key)
                            : 0;
  if (stored < PERSISTED_SETTINGS_MIN_PREFIX_BYTES ||
      stored > sizeof(PersistedSettings) || stored % sizeof(uint32_t) != 0 ||
      preferences.getBytes(key, &settings, stored) != stored) {
    return false;
  }
  if (stored < sizeof(PersistedSettings)) {
    auto *bytes = reinterpret_cast<uint8_t *>(&settings);
    memset(bytes + stored, 0, sizeof(PersistedSettings) - stored);
    if (settings.magic != PERSISTED_SETTINGS_MAGIC ||
        settings.schemaVersion != CONFIG_SCHEMA_VERSION ||
        settings.structureSize != stored) {
      return false;
    }
    size_t at = stored - sizeof(uint32_t);
    uint32_t checksum = 0;
    memcpy(&checksum, bytes + at, sizeof(checksum));
    if (checksum != crc32(bytes, at)) {
      at -= sizeof(uint32_t);
      memcpy(&checksum, bytes + at, sizeof(checksum));
      if (checksum != crc32(bytes, at)) return false;
    }
    memset(bytes + at, 0, stored - at);
    settings.structureSize = sizeof(PersistedSettings);
    settings.checksum = persistedSettingsChecksum(settings);
  }
  if (!persistedSettingsIntact(settings) || !validPersistedStaNetwork(settings)) {
    return false;
  }
  if (repair) (void)repairPersistedSettings(settings);
  return validPersistedSettings(settings);
}

inline bool lockSettingsNvs() { return lockFlashIo(); }
inline void unlockSettingsNvs() { unlockFlashIo(); }
inline void yieldSettingsNvs() { yieldFlashIo(); }
inline void feedSettingsNvsWatchdog() { feedFlashIoWatchdog(); }

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
  PersistedSettings &scratch = persistedSettingsScratch();
  bool loaded = readSettingsSlot(preferences, SETTINGS_SLOT_A, scratch);
  uint32_t loadedRevision = 0;
  if (loaded) {
    settings = scratch;
    loadedRevision = scratch.storageRevision;
  }
  if (readSettingsSlot(preferences, SETTINGS_SLOT_B, scratch) &&
      (!loaded || secondRevisionIsNewer(loadedRevision,
                                        scratch.storageRevision))) {
    settings = scratch;
    loadedRevision = scratch.storageRevision;
    loaded = true;
  }
  // Last resort before factory defaults: repair the newest intact slot.
  const bool loadedAsIs = loaded;
  for (const char *key : {SETTINGS_SLOT_A, SETTINGS_SLOT_B}) {
    if (!loadedAsIs && readSettingsSlot(preferences, key, scratch, true) &&
        (!loaded || secondRevisionIsNewer(loadedRevision, scratch.storageRevision))) {
      settings = scratch;
      loadedRevision = scratch.storageRevision;
      loaded = true;
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
  // One internal record is reused for revision probes, candidate write, and
  // read-back verification. Never call loadPersistedSettings while locked.
  yieldSettingsNvs();
  feedSettingsNvsWatchdog();
  if (!lockSettingsNvs()) {
    feedSettingsNvsWatchdog();
    return false;
  }
  PersistedSettings &scratch = persistedSettingsScratch();
  uint32_t revision = settings.storageRevision;
  if (durableStorageRevisionValid()) {
    revision = durableStorageRevision();
  } else if (revision == 0) {
    bool haveRevision = false;
    OpenBrewByWeightPreferences probe(NvsSubsystem::SETTINGS);
    if (probe.begin(SETTINGS_NAMESPACE, true)) {
      if (readSettingsSlot(probe, SETTINGS_SLOT_A, scratch)) {
        revision = scratch.storageRevision;
        haveRevision = true;
      }
      if (readSettingsSlot(probe, SETTINGS_SLOT_B, scratch)) {
        if (!haveRevision || secondRevisionIsNewer(
                                 revision, scratch.storageRevision)) {
          revision = scratch.storageRevision;
        }
        haveRevision = true;
      }
      probe.end();
    }
  }
  const uint32_t originalRevision = settings.storageRevision;
  const uint32_t originalChecksum = settings.checksum;
  // Zero first: the shared scratch carries stale bytes from earlier reads,
  // and memberwise assignment leaves padding indeterminate — stored blobs
  // must stay byte-reproducible for the read-back verification.
  memset(&scratch, 0, sizeof(scratch));
  scratch = settings;
  scratch.storageRevision = revision + 1U;
  if (scratch.storageRevision == 0) {
    scratch.storageRevision = 1;
  }
  finalizePersistedSettings(scratch);

  OpenBrewByWeightPreferences preferences(NvsSubsystem::SETTINGS);
  if (!preferences.begin(SETTINGS_NAMESPACE, false)) {
    unlockSettingsNvs();
    feedSettingsNvsWatchdog();
    return false;
  }
  const char *target =
      (scratch.storageRevision & 1U) ? SETTINGS_SLOT_A : SETTINGS_SLOT_B;
  const uint32_t candidateRevision = scratch.storageRevision;
  const bool written =
      preferences.putBytes(target, &scratch, sizeof(scratch)) == sizeof(scratch);
  if (written) {
    // Bitwise copy: the read-back verification below compares every stored
    // byte, including tail padding that struct assignment need not preserve.
    memcpy(&settings, &scratch, sizeof(settings));
  }
  const bool saved = written && readSettingsSlot(preferences, target, scratch) &&
                     memcmp(&settings, &scratch, sizeof(scratch)) == 0;
  preferences.end();
  if (saved) {
    durableStorageRevision() = candidateRevision;
    durableStorageRevisionValid() = true;
    durableTimezoneSaved().store(settings.runtime.timezoneId[0] != '\0');
  } else {
    settings.storageRevision = originalRevision;
    settings.checksum = originalChecksum;
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
  PersistedSettings &probe = persistedSettingsScratch();
  if (readSettingsSlot(preferences, SETTINGS_SLOT_A, probe) &&
      probe.storageRevision > existingMax) {
    existingMax = probe.storageRevision;
  }
  if (readSettingsSlot(preferences, SETTINGS_SLOT_B, probe) &&
      probe.storageRevision > existingMax) {
    existingMax = probe.storageRevision;
  }
  if (durableStorageRevisionValid() &&
      durableStorageRevision() > existingMax) {
    existingMax = durableStorageRevision();
  }
  if (existingMax > UINT32_MAX - 2U) {
    existingMax = UINT32_MAX - 2U;
  }

  PersistedSettings &scratch = persistedSettingsScratch();
  if (!initializeDefaultSettings(scratch)) {
    preferences.end();
    unlockSettingsNvs();
    return false;
  }
  scratch.storageRevision = existingMax + 1U;
  finalizePersistedSettings(scratch);

  yieldSettingsNvs();
  feedSettingsNvsWatchdog();
  const bool firstSaved =
      preferences.putBytes(SETTINGS_SLOT_A, &scratch, sizeof(scratch)) ==
      sizeof(scratch);
  const bool firstVerified =
      firstSaved && readSettingsSlot(preferences, SETTINGS_SLOT_A, scratch);
  if (firstVerified) {
    settings = scratch;
  } else if (!initializeDefaultSettings(scratch)) {
    preferences.end();
    unlockSettingsNvs();
    return false;
  }
  scratch.storageRevision = existingMax + 2U;
  finalizePersistedSettings(scratch);
  yieldSettingsNvs();
  feedSettingsNvsWatchdog();
  const bool secondSaved =
      preferences.putBytes(SETTINGS_SLOT_B, &scratch, sizeof(scratch)) ==
      sizeof(scratch);
  if (secondSaved) {
    // Same invariant as savePersistedSettings: bitwise copy into the caller
    // record before the byte-exact verification memcmp below.
    memcpy(&settings, &scratch, sizeof(settings));
  }
  const bool secondVerified = secondSaved &&
      readSettingsSlot(preferences, SETTINGS_SLOT_B, scratch) &&
      memcmp(&scratch, &settings, sizeof(scratch)) == 0;
  preferences.end();
  unlockSettingsNvs();

  if (!firstVerified && !secondVerified) {
    return false;
  }
  if (secondVerified) {
    settings = scratch;
  } else {
    settings.storageRevision = existingMax + 1U;
    finalizePersistedSettings(settings);
  }
  noteDurableStorageRevision(settings.storageRevision);
  durableTimezoneSaved().store(false);
  return true;
}

}  // namespace openbrewbyweight

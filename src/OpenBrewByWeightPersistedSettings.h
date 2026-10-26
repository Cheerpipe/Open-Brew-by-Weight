#pragma once

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightBullseye.h"
#include "OpenBrewByWeightPresets.h"
#include "OpenBrewByWeightWebhook.h"
#include "machine/OpenBrewByWeightLineaMicraSettings.h"

namespace openbrewbyweight {

constexpr uint32_t PERSISTED_SETTINGS_MAGIC = 0x4F424257U;  // "OBBW"
// Shortened on purpose: "openbrewbyweight" exceeds the NVS name limit.
constexpr char SETTINGS_NAMESPACE[] = "openbrew";
static_assert(sizeof(SETTINGS_NAMESPACE) <= 16,
              "NVS namespace names are limited to 15 characters");
constexpr const char *SETTINGS_SLOT_A = "settingsA";
constexpr const char *SETTINGS_SLOT_B = "settingsB";
constexpr const char *DEFAULT_DEVICE_PASSWORD = "ineedacoffee";
struct PersistedSettings {
  uint32_t magic = PERSISTED_SETTINGS_MAGIC;
  uint32_t schemaVersion = CONFIG_SCHEMA_VERSION;
  uint32_t structureSize = 0;
  uint32_t storageRevision = 0;
  RuntimeConfig runtime = {};
  BullseyeMelodyConfig bullseyeMelody = {};
  ShotPresetBank presets = {};
  bool staConfigured = false;
  bool staOpen = false;
  bool staWifiSleep = true;
  char staSsid[WIFI_SSID_CAPACITY] = {};
  char staPassword[WIFI_PASSWORD_CAPACITY] = {};
  uint8_t staIpMode = static_cast<uint8_t>(StaIpMode::DHCP);
  uint8_t staIp[4] = {};
  uint8_t staNetmask[4] = {};
  uint8_t staGateway[4] = {};
  uint8_t staDns1[4] = {};
  uint8_t staDns2[4] = {};
  uint8_t staConfigState = static_cast<uint8_t>(StaConfigState::CONFIRMED);
  bool lkgValid = false;
  bool lkgOpen = false;
  char lkgSsid[WIFI_SSID_CAPACITY] = {};
  char lkgPassword[WIFI_PASSWORD_CAPACITY] = {};
  uint8_t lkgIpMode = static_cast<uint8_t>(StaIpMode::DHCP);
  uint8_t lkgIp[4] = {};
  uint8_t lkgNetmask[4] = {};
  uint8_t lkgGateway[4] = {};
  uint8_t lkgDns1[4] = {};
  uint8_t lkgDns2[4] = {};
  char devicePassword[WIFI_PASSWORD_CAPACITY] = {};
  char preferredScaleMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char preferredScaleName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  ScaleHistoryEntry scaleHistory[SCALE_HISTORY_CAPACITY] = {};
  WebhookConfig webhook = {};
  // mDNS device name.
  char deviceName[DEVICE_NAME_CAPACITY] = {};
  // Optional Micra cloud account, selected machine and independent options.
  LineaMicraPersistedSettings lineaMicra = {};
  uint32_t checksum = 0;
};

struct SettingsPersistRequest {
  PersistedSettings blob;
  uint32_t runtimeRevision = 0;
};

static_assert(sizeof(SettingsPersistRequest) <=
                  PERSISTED_SETTINGS_NVS_BUDGET + 16,
              "SettingsPersistRequest must stay a single NVS blob plus revision");

struct PersistedSettingsHeader {
  uint32_t magic = 0;
  uint32_t schemaVersion = 0;
  uint32_t structureSize = 0;
  uint32_t storageRevision = 0;
};

static_assert(offsetof(PersistedSettings, storageRevision) + sizeof(uint32_t) ==
                  sizeof(PersistedSettingsHeader),
              "PersistedSettings header layout changed");

static_assert(sizeof(PersistedSettings) <= PERSISTED_SETTINGS_NVS_BUDGET,
              "PersistedSettings exceeds NVS dual-slot budget");
static_assert(sizeof(PersistedSettings) == 3056,
              "Settings schema 1: append only tail fields whose legacy default is "
              "zero (readSettingsSlot loads shorter schema-1 blobs as a prefix); "
              "any other layout change bumps CONFIG_SCHEMA_VERSION");
static_assert(offsetof(PersistedSettings, lineaMicra) == 2735);
static_assert(offsetof(PersistedSettings, checksum) == 3048);

inline uint32_t persistedSettingsChecksum(const PersistedSettings &settings) {
  return crc32(reinterpret_cast<const uint8_t *>(&settings),
               offsetof(PersistedSettings, checksum));
}

}  // namespace openbrewbyweight

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

// Working copy of the settings; NVS stores it through
// OpenBrewByWeightSettingsCodec.h, where every member needs a field row.
struct PersistedSettings {
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
};

struct SettingsPersistRequest {
  PersistedSettings blob;
  uint32_t runtimeRevision = 0;
};

}  // namespace openbrewbyweight

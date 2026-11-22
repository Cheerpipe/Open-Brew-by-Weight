#pragma once

// The Mahlkönig pairing identity (docs/MAHLKONIG_GBS.md): this controller's
// brewDeviceId, the grinder's pairing id and its certificate pin, one NVS
// blob (`gbs_ident`) of its own. Never part of the settings, so no backup or
// export carries it and restoring one never clones a brew device; Unpair,
// Forget and a factory reset erase it, an OTA update keeps it. Callers pass
// a stack copy: flash I/O must not read PSRAM.

#include "OpenBrewByWeightMahlkonig.h"
#include "OpenBrewByWeightPersistence.h"
#include "OpenBrewByWeightWebAccess.h"

namespace openbrewbyweight {

constexpr const char *GBS_IDENTITY_KEY = "gbs_ident";
constexpr uint32_t GBS_IDENTITY_MAGIC = 0x47425331U;  // "GBS1"

struct GbsIdentityStored {
  uint32_t magic = GBS_IDENTITY_MAGIC;
  uint16_t version = 1;
  uint16_t structureSize = sizeof(GbsIdentityStored);
  GbsIdentity identity;
  uint32_t checksum = 0;
};
static_assert(sizeof(GbsIdentityStored) == 136, "gbs_ident blob layout changed");

inline uint32_t gbsIdentityChecksum(const GbsIdentityStored &stored) {
  return webAccessFnv(&stored, offsetof(GbsIdentityStored, checksum));
}

inline bool validGbsIdentityStored(const GbsIdentityStored &stored) {
  const GbsIdentity &id = stored.identity;
  return stored.magic == GBS_IDENTITY_MAGIC && stored.version == 1 &&
         stored.structureSize == sizeof(GbsIdentityStored) && id.paired <= 1 && id.pinned <= 1 &&
         memchr(id.brewDeviceId, '\0', sizeof(id.brewDeviceId)) != nullptr &&
         (id.brewDeviceId[0] == '\0' || gbsUuidValid(id.brewDeviceId, sizeof(id.brewDeviceId))) &&
         memchr(id.host, '\0', sizeof(id.host)) != nullptr && (id.host[0] == '\0' || gbsHostValid(id.host)) &&
         stored.checksum == gbsIdentityChecksum(stored);
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline SettingsBlobLoad loadGbsIdentity(GbsIdentity &identity) {
  GbsIdentityStored stored;
  const SettingsBlobLoad result =
      loadSettingsBlob(NvsSubsystem::MAHLKONIG, GBS_IDENTITY_KEY, stored, validGbsIdentityStored);
  identity = stored.identity;
  return result;
}

inline bool saveGbsIdentity(const GbsIdentity &identity) {
  GbsIdentityStored stored;
  stored.identity = identity;
  stored.checksum = gbsIdentityChecksum(stored);
  return saveSettingsBlob(NvsSubsystem::MAHLKONIG, GBS_IDENTITY_KEY, stored);
}

inline bool eraseGbsIdentity() { return eraseSettingsNvsKey(NvsSubsystem::MAHLKONIG, GBS_IDENTITY_KEY); }
#endif

}  // namespace openbrewbyweight

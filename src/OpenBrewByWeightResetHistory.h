#pragma once

#include <stdint.h>

namespace openbrewbyweight {

constexpr uint8_t RESET_HISTORY_CAPACITY = 10;

// Trivial on purpose: SafetyResetRecord keeps these entries in RTC no-init
// memory, where a default member initializer becomes a constructor that runs
// at every boot and erases them. Value-initialize ({}) everywhere else.
struct ResetHistoryEntry {
  uint32_t reasonCode;
  uint32_t uptimeMs;
};

// FNV-style mix over the history entries, seeded per persisted record (seed
// first, as in esp_rom_crc32_le). The byte-exact mix order is part of on-boot
// checksum validation; compute persisted checksums only through this helper.
// Takes a volatile pointer so callers reading RTC/NOINIT memory keep volatile
// semantics.
inline uint32_t resetHistoryChecksum(uint32_t seed,
                                     const volatile ResetHistoryEntry *history,
                                     uint32_t count) {
  uint32_t checksum = seed;
  for (uint32_t i = 0; i < count; ++i) {
    checksum = (checksum * 16777619UL) ^ history[i].reasonCode;
    checksum = (checksum * 16777619UL) ^ history[i].uptimeMs;
  }
  return checksum;
}

}  // namespace openbrewbyweight

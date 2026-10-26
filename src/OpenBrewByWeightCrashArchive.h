#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef OPEN_BREW_BY_WEIGHT_CRASH_HOST_TEST
#include "tests/crash_archive_host_stubs.h"
#elif !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <Arduino.h>
#include <esp_attr.h>
#endif

namespace openbrewbyweight {

constexpr size_t CRASH_ADDRESS_CAPACITY = 60;
constexpr size_t CRASH_HISTORY_SLOT_BYTES = 0xB0000;
constexpr size_t CRASH_HISTORY_DATA_BYTES = CRASH_HISTORY_SLOT_BYTES - 4096;

struct CrashAddressRecord {
  uint32_t magic;
  uint32_t bootId;
  uint32_t core;
  uint32_t pc;
  uint32_t count;
  uint32_t flags;
  uint32_t addresses[CRASH_ADDRESS_CAPACITY];
  uint8_t elfSha256[32];
  uint32_t checksum;
};

struct CrashArchiveEntry {
  uint32_t magic;
  uint32_t version;
  uint32_t sequence;
  uint32_t bootId;
  uint32_t resetReason;
  uint32_t dumpBytes;
  uint8_t dumpSha256[32];
  uint8_t elfSha256[32];
  char crashedTask[16];
  uint32_t crashedPc;
  CrashAddressRecord addresses;
  uint32_t checksum;
};

enum class CrashArchiveState : uint8_t {
  READY, UNSUPPORTED, INVALID_CAPTURE, TOO_LARGE, IO_ERROR,
  BOOT_ID_UNAVAILABLE
};

struct CrashArchiveStatus {
  uint8_t count;
  CrashArchiveState state;
  CrashArchiveEntry entries[2];
  uint8_t slots[2];
};

void crashArchivePrepareBoot();
void crashArchiveSetBootId(uint32_t bootId);
#if defined(OPEN_BREW_BY_WEIGHT_CRASH_HOST_TEST) || (!defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST))
void crashArchiveRecordPanic(const arduino_panic_info_t *info);
#endif
void crashArchivePromote(uint32_t currentBootId, uint32_t resetReason);
// verifyDumps re-checks each listed dump against its stored SHA-256 (reading
// it chunk by chunk under the flash lock) and drops records that fail; call it
// before serving dumps, never from the periodic streams.
CrashArchiveStatus crashArchiveStatus(bool verifyDumps = false);
bool crashArchiveRead(uint8_t slot, size_t offset, void *output, size_t length);
bool crashArchiveClear();

}  // namespace openbrewbyweight

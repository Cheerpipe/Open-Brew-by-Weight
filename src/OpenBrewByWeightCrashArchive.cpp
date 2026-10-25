#include "OpenBrewByWeightCrashArchive.h"

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_CRASH_HOST_TEST)
#ifndef OPEN_BREW_BY_WEIGHT_CRASH_HOST_TEST
#include "OpenBrewByWeightFlashIoScratch.h"
#include <esp_app_desc.h>
#include <esp_core_dump.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <psa/crypto.h>
#include <string.h>
#else
#include <string.h>
#endif

namespace openbrewbyweight {
namespace {

constexpr uint32_t kAddressMagic = 0x43524144;
constexpr uint32_t kEntryMagic = 0x43524153;
constexpr uint32_t kEntryVersion = 0;
RTC_NOINIT_ATTR volatile CrashAddressRecord g_panicRecord;
RTC_NOINIT_ATTR volatile uint32_t g_activeBootId;
RTC_NOINIT_ATTR volatile uint8_t g_activeElfSha[32];
CrashArchiveStatus g_status = {0, CrashArchiveState::UNSUPPORTED, {}, {}};

uint32_t IRAM_ATTR recordChecksum(const volatile CrashAddressRecord &record) {
  uint32_t checksum = 2166136261U;
  const volatile uint32_t *words = reinterpret_cast<const volatile uint32_t *>(&record);
  for (size_t i = 1; i < offsetof(CrashAddressRecord, checksum) / 4; ++i)
    checksum = (checksum ^ words[i]) * 16777619U;
  return checksum;
}

bool validRecord(const volatile CrashAddressRecord &record) {
  return record.magic == kAddressMagic &&
         record.count <= CRASH_ADDRESS_CAPACITY &&
         record.checksum == recordChecksum(record);
}

// Partition lookups allocate an iterator; the table is immutable, so each
// partition is looked up once.
const esp_partition_t *historyPartition() {
  static const esp_partition_t *const part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
      "crashhist");
  return part && part->size == 2 * CRASH_HISTORY_SLOT_BYTES ? part : nullptr;
}

const esp_partition_t *capturePartition() {
  static const esp_partition_t *const part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
  return part;
}

size_t metaOffset(uint8_t slot) {
  return (slot + 1) * CRASH_HISTORY_SLOT_BYTES - 4096;
}

bool digestPartition(const esp_partition_t *part, size_t offset, size_t length,
                     uint8_t digest[32]) {
  // Callers hold tryLockFlashIo(); a 1 KiB stack chunk here pushed the Web UI
  // factory reset past the network task stack.
  uint8_t *chunk = flashIoScratchBytes();
  psa_hash_operation_t context = PSA_HASH_OPERATION_INIT;
  bool ok = psa_hash_setup(&context, PSA_ALG_SHA_256) == PSA_SUCCESS;
  for (size_t done = 0; ok && done < length; done += FLASH_IO_CHUNK_BYTES) {
    const size_t bytes = length - done < FLASH_IO_CHUNK_BYTES
                             ? length - done : FLASH_IO_CHUNK_BYTES;
    ok = esp_partition_read(part, offset + done, chunk, bytes) == ESP_OK &&
         psa_hash_update(&context, chunk, bytes) == PSA_SUCCESS;
    feedFlashIoWatchdog();
  }
  size_t digestBytes = 0;
  if (ok) ok = psa_hash_finish(&context, digest, 32, &digestBytes) == PSA_SUCCESS &&
               digestBytes == 32;
  (void)psa_hash_abort(&context);
  return ok;
}

uint32_t entryChecksum(const CrashArchiveEntry &entry) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t *>(&entry),
                          offsetof(CrashArchiveEntry, checksum));
}

bool decodeSha256(const uint8_t *text, uint8_t output[32]) {
  for (size_t i = 0; i < 32; ++i) {
    uint8_t value = 0;
    for (size_t j = 0; j < 2; ++j) {
      const uint8_t character = text[2 * i + j];
      if (character >= '0' && character <= '9') value = value * 16 + character - '0';
      else if (character >= 'a' && character <= 'f') value = value * 16 + character - 'a' + 10;
      else return false;
    }
    output[i] = value;
  }
  return true;
}

bool readEntry(const esp_partition_t *part, uint8_t slot,
               CrashArchiveEntry &entry, bool *ioError = nullptr) {
  if (esp_partition_read(part, metaOffset(slot), &entry, sizeof(entry)) != ESP_OK) {
    if (ioError) *ioError = true;
    return false;
  }
  if (entry.magic != kEntryMagic || entry.version != kEntryVersion ||
      entry.dumpBytes < 64 || entry.dumpBytes > CRASH_HISTORY_DATA_BYTES ||
      entry.checksum != entryChecksum(entry)) return false;
  uint8_t digest[32];
  if (!digestPartition(part, slot * CRASH_HISTORY_SLOT_BYTES,
                       entry.dumpBytes, digest)) {
    if (ioError) *ioError = true;
    return false;
  }
  return memcmp(digest, entry.dumpSha256, sizeof(digest)) == 0;
}

bool eraseSectors(const esp_partition_t *part, size_t offset, size_t length) {
  for (size_t done = 0; done < length; done += 4096) {
    if (esp_partition_erase_range(part, offset + done, 4096) != ESP_OK)
      return false;
    feedFlashIoWatchdog();
    yieldFlashIo();
  }
  return true;
}

bool scanHistory(const esp_partition_t *part) {
  g_status.count = 0;
  bool ioError = false;
  for (uint8_t slot = 0; slot < 2; ++slot) {
    CrashArchiveEntry entry = {};
    if (!readEntry(part, slot, entry, &ioError)) continue;
    const uint8_t index = g_status.count++;
    g_status.entries[index] = entry;
    g_status.slots[index] = slot;
  }
  if (g_status.count == 2 &&
      g_status.entries[0].sequence < g_status.entries[1].sequence) {
    const CrashArchiveEntry entry = g_status.entries[0];
    g_status.entries[0] = g_status.entries[1];
    g_status.entries[1] = entry;
    const uint8_t slot = g_status.slots[0];
    g_status.slots[0] = g_status.slots[1];
    g_status.slots[1] = slot;
  }
  return !ioError;
}

}  // namespace

void crashArchivePrepareBoot() { g_activeBootId = 0; }
void crashArchiveSetBootId(uint32_t bootId) {
  const uint8_t *sha = esp_app_get_description()->app_elf_sha256;
  for (size_t i = 0; i < sizeof(g_activeElfSha); ++i)
    g_activeElfSha[i] = sha[i];
  g_activeBootId = bootId;
}

void IRAM_ATTR crashArchiveRecordPanic(const arduino_panic_info_t *info) {
  if (!info) return;
  g_panicRecord.magic = 0;
  g_panicRecord.bootId = g_activeBootId;
  g_panicRecord.core = static_cast<uint32_t>(info->core);
  g_panicRecord.pc = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->pc));
  g_panicRecord.count = info->backtrace_len < CRASH_ADDRESS_CAPACITY
                            ? info->backtrace_len : CRASH_ADDRESS_CAPACITY;
  g_panicRecord.flags = (info->backtrace_corrupt ? 1U : 0U) |
                        (info->backtrace_continues ||
                         info->backtrace_len > CRASH_ADDRESS_CAPACITY ? 2U : 0U);
  for (size_t i = 0; i < CRASH_ADDRESS_CAPACITY; ++i)
    g_panicRecord.addresses[i] =
        i < g_panicRecord.count ? info->backtrace[i] : 0;
  for (size_t i = 0; i < sizeof(g_activeElfSha); ++i)
    g_panicRecord.elfSha256[i] = g_activeElfSha[i];
  g_panicRecord.checksum = recordChecksum(g_panicRecord);
  g_panicRecord.magic = kAddressMagic;
}

void crashArchivePromote(uint32_t currentBootId, uint32_t resetReason) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
  const esp_partition_t *history = historyPartition();
  const esp_partition_t *capture = capturePartition();
  g_status.state = history && capture && capture->size == 0xA0000
                       ? CrashArchiveState::READY : CrashArchiveState::UNSUPPORTED;
  if (g_status.state != CrashArchiveState::READY) return;
  if (!tryLockFlashIo()) {
    g_status.state = CrashArchiveState::IO_ERROR;
    return;
  }
  if (!scanHistory(history)) {
    g_status.state = CrashArchiveState::IO_ERROR;
    unlockFlashIo();
    return;
  }
  size_t address = 0, length = 0;
  const esp_err_t found = esp_core_dump_image_get(&address, &length);
  if (found == ESP_ERR_NOT_FOUND) {
    unlockFlashIo();
    return;
  }
  if (found != ESP_OK || address != capture->address ||
      esp_core_dump_image_check() != ESP_OK) {
    g_status.state = CrashArchiveState::INVALID_CAPTURE;
    unlockFlashIo();
    return;
  }
  if (length > CRASH_HISTORY_DATA_BYTES) {
    g_status.state = CrashArchiveState::TOO_LARGE;
    unlockFlashIo();
    return;
  }
  const bool hasPanicRecord =
      validRecord(g_panicRecord) && g_panicRecord.bootId != 0;
  const uint32_t crashBootId = hasPanicRecord
                                   ? g_panicRecord.bootId
                                   : currentBootId > 1 ? currentBootId - 1 : 0;
  if (crashBootId == 0) {
    g_status.state = CrashArchiveState::BOOT_ID_UNAVAILABLE;
    unlockFlashIo();
    return;
  }
  uint8_t digest[32];
  if (!digestPartition(capture, 0, length, digest)) {
    g_status.state = CrashArchiveState::IO_ERROR;
    unlockFlashIo();
    return;
  }
  for (uint8_t i = 0; i < g_status.count; ++i) {
    const CrashArchiveEntry &old = g_status.entries[i];
    if (old.dumpBytes == length &&
        memcmp(old.dumpSha256, digest, sizeof(digest)) == 0 &&
        !hasPanicRecord && old.bootId != crashBootId) {
      g_status.state = CrashArchiveState::BOOT_ID_UNAVAILABLE;
      unlockFlashIo();
      return;
    }
    if (old.bootId == crashBootId && old.dumpBytes == length &&
        memcmp(old.dumpSha256, digest, sizeof(digest)) == 0) {
      if (!eraseSectors(capture, 0, 4096)) {
        g_status.state = CrashArchiveState::IO_ERROR;
      } else {
        g_panicRecord.magic = 0;
      }
      unlockFlashIo();
      return;
    }
  }
  const uint8_t slot = g_status.count == 2 ? g_status.slots[1]
                         : g_status.count == 1 ? 1 - g_status.slots[0] : 0;
  CrashArchiveEntry entry = {};
  entry.magic = kEntryMagic;
  entry.version = kEntryVersion;
  entry.sequence = g_status.count ? g_status.entries[0].sequence + 1 : 1;
  entry.bootId = crashBootId;
  entry.resetReason = resetReason;
  entry.dumpBytes = length;
  memcpy(entry.dumpSha256, digest, sizeof(digest));
  esp_core_dump_summary_t summary = {};
  bool summaryIdentity = false;
  if (esp_core_dump_get_summary(&summary) == ESP_OK) {
    memcpy(entry.crashedTask, summary.exc_task, sizeof(entry.crashedTask));
    entry.crashedPc = summary.exc_pc;
    summaryIdentity = decodeSha256(summary.app_elf_sha256, entry.elfSha256);
  }
  if (validRecord(g_panicRecord) && g_panicRecord.bootId == crashBootId) {
    const volatile uint8_t *source =
        reinterpret_cast<const volatile uint8_t *>(&g_panicRecord);
    uint8_t *dest = reinterpret_cast<uint8_t *>(&entry.addresses);
    for (size_t i = 0; i < sizeof(entry.addresses); ++i) dest[i] = source[i];
    if (!summaryIdentity)
      memcpy(entry.elfSha256, entry.addresses.elfSha256,
             sizeof(entry.elfSha256));
  }
  bool ok = eraseSectors(history, slot * CRASH_HISTORY_SLOT_BYTES,
                         CRASH_HISTORY_SLOT_BYTES);
  uint8_t chunk[1024];
  for (size_t done = 0; ok && done < length; done += sizeof(chunk)) {
    const size_t bytes = length - done < sizeof(chunk)
                             ? length - done : sizeof(chunk);
    const size_t target = slot * CRASH_HISTORY_SLOT_BYTES + done;
    ok = esp_partition_read(capture, done, chunk, bytes) == ESP_OK &&
         esp_partition_write(history, target, chunk, bytes) == ESP_OK;
    feedFlashIoWatchdog();
    yieldFlashIo();
  }
  if (ok) ok = digestPartition(history, slot * CRASH_HISTORY_SLOT_BYTES,
                               length, chunk) &&
               memcmp(chunk, digest, sizeof(digest)) == 0;
  entry.checksum = entryChecksum(entry);
  if (ok) ok = esp_partition_write(history, metaOffset(slot), &entry,
                                   sizeof(entry)) == ESP_OK;
  CrashArchiveEntry verified = {};
  if (ok) ok = readEntry(history, slot, verified);
  if (ok) {
    ok = scanHistory(history);
    if (ok) ok = eraseSectors(capture, 0, 4096);
    if (ok) g_panicRecord.magic = 0;
  }
  if (!ok) g_status.state = CrashArchiveState::IO_ERROR;
  unlockFlashIo();
#else
  (void)currentBootId;
  (void)resetReason;
#endif
}

CrashArchiveStatus crashArchiveStatus() { return g_status; }

bool crashArchiveRead(uint8_t slot, size_t offset, void *output, size_t length) {
  const esp_partition_t *part = historyPartition();
  if (!part || slot > 1 || offset > CRASH_HISTORY_DATA_BYTES ||
      length > CRASH_HISTORY_DATA_BYTES - offset || !tryLockFlashIo())
    return false;
  const bool ok = esp_partition_read(part, slot * CRASH_HISTORY_SLOT_BYTES +
                                              offset, output, length) == ESP_OK;
  unlockFlashIo();
  return ok;
}

bool crashArchiveClear() {
  const esp_partition_t *history = historyPartition();
  const esp_partition_t *capture = capturePartition();
  if (!history) return true;
  if (!capture || !tryLockFlashIo()) return false;
  const bool ok = eraseSectors(capture, 0, capture->size) &&
                  eraseSectors(history, 0, history->size);
  const bool scanned = scanHistory(history);
  g_status.state = ok && scanned ? CrashArchiveState::READY
                                  : CrashArchiveState::IO_ERROR;
  if (ok) {
    g_panicRecord.magic = 0;
  }
  unlockFlashIo();
  return ok && scanned;
}

}  // namespace openbrewbyweight
#endif

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#define RTC_NOINIT_ATTR
#define IRAM_ATTR
#define CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH 1

struct arduino_panic_info_t {
  int core = 0;
  const char *reason = nullptr;
  const void *pc = nullptr;
  bool backtrace_corrupt = false;
  bool backtrace_continues = false;
  unsigned backtrace_len = 0;
  unsigned backtrace[60] = {};
};

using esp_err_t = int;
using esp_partition_subtype_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_ERR_NOT_FOUND = -2;
constexpr int ESP_ERR_INVALID_SIZE = -3;
constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 3;

struct esp_partition_t {
  uint32_t address;
  size_t size;
  const char *label;
  std::vector<uint8_t> bytes;
};

inline esp_partition_t fakeCapture{0xE00000, 0xA0000, "coredump",
                                   std::vector<uint8_t>(0xA0000, 0xFF)};
inline esp_partition_t fakeHistory{0xEA0000, 0x160000, "crashhist",
                                   std::vector<uint8_t>(0x160000, 0xFF)};
inline size_t fakeErases = 0, fakeWrites = 0, fakeReads = 0;
inline size_t fakeFailEraseAt = 0, fakeFailWriteAt = 0, fakeFailReadAt = 0;
inline bool fakeCaptureValid = true;

inline const esp_partition_t *esp_partition_find_first(
    int, esp_partition_subtype_t subtype, const char *label) {
  if (label && std::strcmp(label, "crashhist") == 0 && subtype == 0x40)
    return &fakeHistory;
  return subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP ? &fakeCapture : nullptr;
}

inline esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset,
                                    void *output, size_t length) {
  ++fakeReads;
  if (fakeFailReadAt && fakeReads == fakeFailReadAt) return ESP_FAIL;
  if (!part || offset > part->size || length > part->size - offset) return ESP_FAIL;
  std::memcpy(output, part->bytes.data() + offset, length);
  return ESP_OK;
}

inline esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset,
                                     const void *input, size_t length) {
  ++fakeWrites;
  if ((fakeFailWriteAt && fakeWrites == fakeFailWriteAt) || !part ||
      offset > part->size || length > part->size - offset) return ESP_FAIL;
  auto *mutablePart = const_cast<esp_partition_t *>(part);
  const auto *source = static_cast<const uint8_t *>(input);
  for (size_t i = 0; i < length; ++i)
    mutablePart->bytes[offset + i] &= source[i];
  return ESP_OK;
}

inline esp_err_t esp_partition_erase_range(const esp_partition_t *part,
                                           size_t offset, size_t length) {
  ++fakeErases;
  if ((fakeFailEraseAt && fakeErases == fakeFailEraseAt) || !part ||
      offset % 4096 || length % 4096 || offset > part->size ||
      length > part->size - offset) return ESP_FAIL;
  auto *mutablePart = const_cast<esp_partition_t *>(part);
  std::fill(mutablePart->bytes.begin() + offset,
            mutablePart->bytes.begin() + offset + length, 0xFF);
  return ESP_OK;
}

inline esp_err_t esp_core_dump_image_get(size_t *address, size_t *length) {
  uint32_t size = 0;
  std::memcpy(&size, fakeCapture.bytes.data(), sizeof(size));
  if (size == UINT32_MAX) return ESP_ERR_NOT_FOUND;
  if (size < 64 || size > fakeCapture.size) return ESP_ERR_INVALID_SIZE;
  *address = fakeCapture.address;
  *length = size;
  return ESP_OK;
}

inline esp_err_t esp_core_dump_image_check() {
  size_t address = 0, length = 0;
  return fakeCaptureValid && esp_core_dump_image_get(&address, &length) == ESP_OK
             ? ESP_OK : ESP_FAIL;
}

struct esp_core_dump_summary_t {
  char exc_task[16] = {};
  uint32_t exc_pc = 0;
  uint8_t app_elf_sha256[65] = {};
};
inline esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t *) {
  return ESP_FAIL;
}

struct esp_app_desc_t { uint8_t app_elf_sha256[32]; };
inline const esp_app_desc_t *esp_app_get_description() {
  static const esp_app_desc_t desc{{1}};
  return &desc;
}

inline uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *data,
                                  size_t length) {
  uint32_t value = seed ^ 2166136261U;
  for (size_t i = 0; i < length; ++i) value = (value ^ data[i]) * 16777619U;
  return value;
}

constexpr int PSA_SUCCESS = 0;
constexpr int PSA_ALG_SHA_256 = 1;
struct psa_hash_operation_t { uint32_t words[8] = {}; };
#define PSA_HASH_OPERATION_INIT psa_hash_operation_t{}
inline int psa_hash_setup(psa_hash_operation_t *op, int) {
  for (size_t i = 0; i < 8; ++i) op->words[i] = 2166136261U + i;
  return PSA_SUCCESS;
}
inline int psa_hash_update(psa_hash_operation_t *op, const uint8_t *data,
                           size_t length) {
  for (size_t i = 0; i < length; ++i)
    for (size_t j = 0; j < 8; ++j)
      op->words[j] = (op->words[j] ^ (data[i] + j)) * 16777619U;
  return PSA_SUCCESS;
}
inline int psa_hash_finish(psa_hash_operation_t *op, uint8_t *output,
                           size_t capacity, size_t *length) {
  if (capacity < 32) return ESP_FAIL;
  std::memcpy(output, op->words, 32);
  *length = 32;
  return PSA_SUCCESS;
}
inline int psa_hash_abort(psa_hash_operation_t *) { return PSA_SUCCESS; }

namespace openbrewbyweight {
inline bool tryLockFlashIo() { return true; }
inline void unlockFlashIo() {}
inline void feedFlashIoWatchdog() {}
constexpr size_t FLASH_IO_CHUNK_BYTES = 1024;
inline uint8_t *flashIoScratchBytes() {
  static uint8_t scratch[FLASH_IO_CHUNK_BYTES];
  return scratch;
}
inline void yieldFlashIo() {}
}

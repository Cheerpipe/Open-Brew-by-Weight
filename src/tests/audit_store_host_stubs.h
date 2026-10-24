#pragma once

// RAM-backed partition fake for the audit store host test: programs can only
// clear bits, erases restore 0xFF, and the fail-at counters inject I/O
// failures at deterministic operation indices.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

using esp_err_t = int;
using esp_partition_subtype_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_FAIL = -1;
constexpr int ESP_PARTITION_TYPE_DATA = 1;

struct esp_partition_t {
  uint32_t address = 0;
  size_t size = 0;
  const char *label = "auditlog";
  std::vector<uint8_t> bytes;
};

inline esp_partition_t fakeAuditLog{0x6B9000, 0x10000, "auditlog",
                                    std::vector<uint8_t>(0x10000, 0xFF)};
inline bool fakeAuditLogPresent = true;
inline size_t fakeAuditOps = 0;
inline size_t fakeAuditFailReadAt = 0, fakeAuditFailWriteAt = 0,
              fakeAuditFailEraseAt = 0;

inline const esp_partition_t *esp_partition_find_first(
    int, esp_partition_subtype_t subtype, const char *label) {
  if (!fakeAuditLogPresent || std::strcmp(label, "auditlog") != 0 ||
      subtype != 0x40) {
    return nullptr;
  }
  return &fakeAuditLog;
}

inline esp_err_t esp_partition_read(const esp_partition_t *part, size_t offset,
                                    void *output, size_t length) {
  ++fakeAuditOps;
  if (fakeAuditFailReadAt && fakeAuditOps == fakeAuditFailReadAt) {
    return ESP_FAIL;
  }
  if (!part || offset > part->size || length > part->size - offset) {
    return ESP_FAIL;
  }
  std::memcpy(output, part->bytes.data() + offset, length);
  return ESP_OK;
}

inline esp_err_t esp_partition_write(const esp_partition_t *part, size_t offset,
                                     const void *input, size_t length) {
  ++fakeAuditOps;
  if (fakeAuditFailWriteAt && fakeAuditOps == fakeAuditFailWriteAt) {
    return ESP_FAIL;
  }
  if (!part || offset > part->size || length > part->size - offset) {
    return ESP_FAIL;
  }
  auto *mutablePart = const_cast<esp_partition_t *>(part);
  const auto *source = static_cast<const uint8_t *>(input);
  for (size_t i = 0; i < length; ++i) {
    mutablePart->bytes[offset + i] &= source[i];
  }
  return ESP_OK;
}

inline esp_err_t esp_partition_erase_range(const esp_partition_t *part,
                                           size_t offset, size_t length) {
  ++fakeAuditOps;
  if (fakeAuditFailEraseAt && fakeAuditOps == fakeAuditFailEraseAt) {
    return ESP_FAIL;
  }
  if (!part || offset % 4096 || length % 4096 || offset > part->size ||
      length > part->size - offset) {
    return ESP_FAIL;
  }
  auto *mutablePart = const_cast<esp_partition_t *>(part);
  std::fill(mutablePart->bytes.begin() + offset,
            mutablePart->bytes.begin() + offset + length, 0xFF);
  return ESP_OK;
}

namespace openbrewbyweight {
inline bool tryLockFlashIo() { return true; }
inline void unlockFlashIo() {}
inline void feedFlashIoWatchdog() {}
inline void yieldFlashIo() {}
}  // namespace openbrewbyweight

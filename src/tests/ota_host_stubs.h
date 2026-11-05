#pragma once

#include "open_brew_by_weight_host_stubs.h"
#include "persistence_host_stubs.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

using esp_err_t = int;
using esp_ota_handle_t = uint32_t;

constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
constexpr size_t OTA_WITH_SEQUENTIAL_WRITES = static_cast<size_t>(-2);

enum esp_ota_img_states_t {
  ESP_OTA_IMG_NEW = 0,
  ESP_OTA_IMG_PENDING_VERIFY = 1,
  ESP_OTA_IMG_VALID = 2,
  ESP_OTA_IMG_INVALID = 3,
  ESP_OTA_IMG_ABORTED = 4,
  ESP_OTA_IMG_UNDEFINED = -1,
};

struct esp_partition_t {
  uint32_t address = 0;
  uint32_t size = 4U * 1024U * 1024U;
};
struct esp_partition_pos_t {
  uint32_t offset;
  uint32_t size;
};
struct esp_image_header_t {
  uint8_t hash_appended = 1;
};
struct esp_image_metadata_t {
  esp_image_header_t image;
  uint8_t image_digest[32];
};

inline esp_partition_t otaHostRunningPartition;
inline esp_partition_t otaHostTargetPartition;

// Deterministic flash/IDF model; the existing persistence hash is a test
// checksum, not a cryptographic implementation or ESP image validator.
namespace ota_host {
inline std::array<uint8_t, 4U * 1024U * 1024U> flash;
inline esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
inline esp_ota_img_states_t spareState = ESP_OTA_IMG_VALID;
inline esp_err_t stateError = ESP_OK, writeError = ESP_OK, endError = ESP_OK;
inline esp_err_t confirmError = ESP_OK, rollbackError = ESP_OK, commitError = ESP_OK;
inline bool rollbackPossible = true, active = false;
inline size_t cursor = 0, erasedThrough = 0, resumeMode = 0;
inline unsigned begins = 0, writes = 0, ends = 0, aborts = 0;
inline unsigned resumes = 0, confirms = 0, rollbacks = 0, invalidHandles = 0;
inline esp_ota_handle_t serial = 0;

inline void reset() {
  flash.fill(0xFF);
  state = ESP_OTA_IMG_UNDEFINED;
  spareState = ESP_OTA_IMG_VALID;
  stateError = writeError = endError = confirmError = rollbackError = commitError = ESP_OK;
  rollbackPossible = true;
  active = false;
  cursor = erasedThrough = resumeMode = 0;
  begins = writes = ends = aborts = resumes = confirms = rollbacks = invalidHandles = 0;
  serial = 0;
}
inline bool valid(esp_ota_handle_t handle) {
  if (active && handle == serial) return true;
  ++invalidHandles;
  return false;
}
}  // namespace ota_host

inline const esp_partition_t *esp_ota_get_running_partition() {
  return &otaHostRunningPartition;
}

inline const esp_partition_t *esp_ota_get_next_update_partition(const void *) {
  return &otaHostTargetPartition;
}

inline esp_err_t esp_ota_get_state_partition(const esp_partition_t *partition,
                                             esp_ota_img_states_t *state) {
  if (state == nullptr) return ESP_FAIL;
  *state = partition == &otaHostTargetPartition ? ota_host::spareState : ota_host::state;
  return ota_host::stateError;
}

inline esp_err_t esp_partition_read(const esp_partition_t *, size_t offset, void *out,
                                    size_t length) {
  if (out == nullptr || offset + length > ota_host::flash.size()) return ESP_FAIL;
  std::memcpy(out, ota_host::flash.data() + offset, length);
  return ESP_OK;
}

// Both report the slot's appended digest; only the first re-hashes the slot.
inline unsigned otaHostFullSlotHashes = 0;
inline esp_err_t esp_partition_get_sha256(const esp_partition_t *, uint8_t *out) {
  ++otaHostFullSlotHashes;
  std::memset(out, 0xAB, 32);
  return ESP_OK;
}
inline esp_err_t esp_image_get_metadata(const esp_partition_pos_t *,
                                        esp_image_metadata_t *metadata) {
  std::memset(metadata->image_digest, 0xAB, 32);
  return ESP_OK;
}

inline esp_err_t esp_ota_resume(const esp_partition_t *, size_t mode, size_t offset,
                                esp_ota_handle_t *handle) {
  if (handle == nullptr || ota_host::active || offset % 4096 != 0) return ESP_FAIL;
  ota_host::resumeMode = mode;
  ota_host::cursor = ota_host::erasedThrough = offset;
  ota_host::active = true;
  ++ota_host::resumes;
  *handle = ++ota_host::serial;
  return ESP_OK;
}

inline esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t mode,
                               esp_ota_handle_t *handle) {
  ++ota_host::begins;
  const esp_err_t result = esp_ota_resume(partition, mode, 0, handle);
  --ota_host::resumes;
  return result;
}

inline esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size) {
  using namespace ota_host;
  if (!valid(handle) || cursor + size > flash.size()) return ESP_FAIL;
  if (writeError != ESP_OK) return writeError;
  const size_t eraseEnd = (cursor + size + 4095U) & ~size_t(4095U);
  if (resumeMode == OTA_WITH_SEQUENTIAL_WRITES && eraseEnd > erasedThrough) {
    std::fill(flash.begin() + erasedThrough, flash.begin() + eraseEnd, 0xFF);
    erasedThrough = eraseEnd;
  }
  const auto *bytes = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i) {
    if ((flash[cursor + i] & bytes[i]) != bytes[i]) return ESP_FAIL;
    flash[cursor + i] &= bytes[i];
  }
  cursor += size;
  ++writes;
  return ESP_OK;
}

inline esp_err_t esp_ota_end(esp_ota_handle_t handle) {
  if (!ota_host::valid(handle)) return ESP_FAIL;
  ++ota_host::ends;
  ota_host::active = false;  // IDF consumes the handle even on validation failure.
  return ota_host::endError;
}
inline esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
  if (!ota_host::valid(handle)) return ESP_FAIL;
  ++ota_host::aborts;
  ota_host::active = false;
  return ESP_OK;
}
inline esp_err_t esp_ota_set_boot_partition(const esp_partition_t *) {
  return ota_host::commitError;
}
inline esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
  ++ota_host::confirms;
  if (ota_host::confirmError != ESP_OK) return ota_host::confirmError;
  ota_host::state = ESP_OTA_IMG_VALID;
  return ESP_OK;
}
inline bool esp_ota_check_rollback_is_possible() { return ota_host::rollbackPossible; }
inline esp_err_t esp_ota_mark_app_invalid_rollback() {
  ++ota_host::rollbacks;
  if (ota_host::rollbackError != ESP_OK) return ota_host::rollbackError;
  ota_host::state = ESP_OTA_IMG_INVALID;
  return ESP_OK;
}

inline psa_status_t psa_hash_clone(const psa_hash_operation_t *source,
                                   psa_hash_operation_t *destination) {
  if (source == nullptr || !source->active || destination == nullptr ||
      destination->active) return PSA_ERROR_BAD_STATE;
  *destination = *source;
  return PSA_SUCCESS;
}

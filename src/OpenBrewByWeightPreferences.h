#pragma once

// Instrumented Preferences facade. It preserves the exact NVS error for I/O
// while exposing aggregate capacity only; keys and values never enter
// diagnostics.

#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include "tests/persistence_host_stubs.h"
#elif !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include <Preferences.h>
#include <esp_err.h>
#include <esp_partition.h>
#include <nvs.h>
#include <nvs_flash.h>
#endif

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace openbrewbyweight {

constexpr size_t EXPECTED_NVS_PARTITION_BYTES = 0x65000;
constexpr int32_t HOST_NVS_ERROR = -1;
constexpr int32_t HOST_NVS_NOT_ENOUGH_SPACE = -1001;

enum class NvsSubsystem : uint8_t {
  UNKNOWN = 0,
  SETTINGS,
  LAST_SHOT,
  BLE_SCAN,
  RESET_HISTORY,
  RECOVERY_INTENT,
  OTA_JOURNAL,
  FIRMWARE_MODE,
  WEB_ACCESS,
};

enum class NvsOperation : uint8_t {
  NONE = 0,
  OPEN,
  READ_BLOB,
  WRITE_BLOB,
  COMMIT,
  REMOVE,
  CLEAR,
};

struct NvsFailureSnapshot {
  bool present = false;
  NvsSubsystem subsystem = NvsSubsystem::UNKNOWN;
  NvsOperation operation = NvsOperation::NONE;
  int32_t errorCode = 0;
  size_t requiredEntries = 0;
  size_t availableEntriesAtFailure = 0;
};

struct NvsDiagnosticSnapshot {
  bool statsValid = false;
  int32_t statsErrorCode = 0;
  size_t partitionBytes = 0;
  size_t usedEntries = 0;
  size_t freeEntries = 0;
  size_t availableEntries = 0;
  size_t totalEntries = 0;
  size_t namespaceCount = 0;
  uint32_t failureCount = 0;
  NvsFailureSnapshot lastFailure;
};

inline const char *nvsSubsystemName(NvsSubsystem subsystem) {
  switch (subsystem) {
    case NvsSubsystem::SETTINGS: return "settings";
    case NvsSubsystem::LAST_SHOT: return "lastShot";
    case NvsSubsystem::BLE_SCAN: return "bleScan";
    case NvsSubsystem::RESET_HISTORY: return "resetHistory";
    case NvsSubsystem::RECOVERY_INTENT: return "recoveryIntent";
    case NvsSubsystem::OTA_JOURNAL: return "otaJournal";
    case NvsSubsystem::FIRMWARE_MODE: return "firmwareMode";
    case NvsSubsystem::WEB_ACCESS: return "webAccess";
    default: return "unknown";
  }
}

inline const char *nvsOperationName(NvsOperation operation) {
  switch (operation) {
    case NvsOperation::OPEN: return "open";
    case NvsOperation::READ_BLOB: return "readBlob";
    case NvsOperation::WRITE_BLOB: return "writeBlob";
    case NvsOperation::COMMIT: return "commit";
    case NvsOperation::REMOVE: return "remove";
    case NvsOperation::CLEAR: return "clear";
    default: return "none";
  }
}

inline const char *nvsErrorName(int32_t errorCode) {
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  if (errorCode == HOST_NVS_NOT_ENOUGH_SPACE) return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
  if (errorCode == 0) return "ESP_OK";
  return "HOST_NVS_FAILURE";
#else
  return esp_err_to_name(static_cast<esp_err_t>(errorCode));
#endif
}

inline int32_t nvsNotEnoughSpaceErrorCode() {
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  return HOST_NVS_NOT_ENOUGH_SPACE;
#else
  return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
#endif
}

struct NvsFailureState {
  std::atomic<uint32_t> sequence{0};
  std::atomic<uint32_t> count{0};
  std::atomic<uint8_t> subsystem{0};
  std::atomic<uint8_t> operation{0};
  std::atomic<int32_t> errorCode{0};
  std::atomic<size_t> requiredEntries{0};
  std::atomic<size_t> availableEntries{0};
};

inline NvsFailureState &nvsFailureState() {
  static NvsFailureState state;
  return state;
}

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
inline void resetNvsDiagnosticsForHostTest() {
  NvsFailureState &state = nvsFailureState();
  state.sequence.store(0, std::memory_order_relaxed);
  state.count.store(0, std::memory_order_relaxed);
  state.subsystem.store(0, std::memory_order_relaxed);
  state.operation.store(0, std::memory_order_relaxed);
  state.errorCode.store(0, std::memory_order_relaxed);
  state.requiredEntries.store(0, std::memory_order_relaxed);
  state.availableEntries.store(0, std::memory_order_relaxed);
}
#endif

inline NvsDiagnosticSnapshot captureNvsDiagnostics() {
  NvsDiagnosticSnapshot snapshot;
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) || defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  snapshot.statsValid = true;
  snapshot.partitionBytes = EXPECTED_NVS_PARTITION_BYTES;
#else
  nvs_stats_t stats = {};
  const esp_err_t statsError = nvs_get_stats(nullptr, &stats);
  snapshot.statsErrorCode = statsError;
  if (statsError == ESP_OK) {
    snapshot.statsValid = true;
    snapshot.usedEntries = stats.used_entries;
    snapshot.freeEntries = stats.free_entries;
    snapshot.availableEntries = stats.available_entries;
    snapshot.totalEntries = stats.total_entries;
    snapshot.namespaceCount = stats.namespace_count;
  }
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
  if (partition != nullptr) snapshot.partitionBytes = partition->size;
#endif

  NvsFailureState &state = nvsFailureState();
  snapshot.failureCount = state.count.load(std::memory_order_relaxed);
  if (snapshot.failureCount == 0) return snapshot;
  for (;;) {
    const uint32_t before = state.sequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0) continue;
    snapshot.lastFailure.present = true;
    snapshot.lastFailure.subsystem = static_cast<NvsSubsystem>(
        state.subsystem.load(std::memory_order_relaxed));
    snapshot.lastFailure.operation = static_cast<NvsOperation>(
        state.operation.load(std::memory_order_relaxed));
    snapshot.lastFailure.errorCode =
        state.errorCode.load(std::memory_order_relaxed);
    snapshot.lastFailure.requiredEntries =
        state.requiredEntries.load(std::memory_order_relaxed);
    snapshot.lastFailure.availableEntriesAtFailure =
        state.availableEntries.load(std::memory_order_relaxed);
    if (before == state.sequence.load(std::memory_order_acquire)) break;
  }
  return snapshot;
}

inline void recordNvsFailure(NvsSubsystem subsystem, NvsOperation operation,
                             int32_t errorCode, size_t requiredEntries = 0) {
  const NvsDiagnosticSnapshot capacity = captureNvsDiagnostics();
  NvsFailureState &state = nvsFailureState();
  state.sequence.fetch_add(1, std::memory_order_acq_rel);
  state.subsystem.store(static_cast<uint8_t>(subsystem),
                        std::memory_order_relaxed);
  state.operation.store(static_cast<uint8_t>(operation),
                        std::memory_order_relaxed);
  state.errorCode.store(errorCode, std::memory_order_relaxed);
  state.requiredEntries.store(requiredEntries, std::memory_order_relaxed);
  state.availableEntries.store(capacity.availableEntries,
                               std::memory_order_relaxed);
  state.count.fetch_add(1, std::memory_order_relaxed);
  state.sequence.fetch_add(1, std::memory_order_release);
}

constexpr size_t nvsBlobRequiredEntries(size_t bytes) {
  return 2U + ((bytes + 31U) / 32U);
}

#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) ||                                      \
    defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
class OpenBrewByWeightPreferences {
 public:
  explicit OpenBrewByWeightPreferences(
      NvsSubsystem subsystem = NvsSubsystem::UNKNOWN)
      : subsystem_(subsystem) {}
  ~OpenBrewByWeightPreferences() { end(); }
  OpenBrewByWeightPreferences(const OpenBrewByWeightPreferences &) = delete;
  OpenBrewByWeightPreferences &operator=(const OpenBrewByWeightPreferences &) = delete;

  bool begin(const char *name, bool readOnly = false,
             const char *partitionLabel = nullptr) {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    (void)partitionLabel;
    const bool opened = preferences_.begin(name, readOnly);
    if (!opened) recordNvsFailure(subsystem_, NvsOperation::OPEN, HOST_NVS_ERROR);
    return opened;
#else
    if (started_) {
      recordNvsFailure(subsystem_, NvsOperation::OPEN,
                       ESP_ERR_NVS_INVALID_HANDLE);
      return false;
    }
    readOnly_ = readOnly;
    esp_err_t error = ESP_OK;
    if (partitionLabel != nullptr) {
      error = nvs_flash_init_partition(partitionLabel);
      if (error == ESP_OK) {
        error = nvs_open_from_partition(partitionLabel, name,
            readOnly ? NVS_READONLY : NVS_READWRITE, &handle_);
      }
    } else {
      error = nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE,
                       &handle_);
    }
    if (error != ESP_OK) {
      if (!(readOnly && error == ESP_ERR_NVS_NOT_FOUND)) {
        recordNvsFailure(subsystem_, NvsOperation::OPEN, error);
      }
      return false;
    }
    started_ = true;
    return true;
#endif
  }

  void end() {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    preferences_.end();
#else
    if (started_) nvs_close(handle_);
    started_ = false;
#endif
  }

  size_t getBytesLength(const char *key) {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    return preferences_.getBytesLength(key);
#else
    if (!started_ || key == nullptr) return 0;
    size_t length = 0;
    const esp_err_t error = nvs_get_blob(handle_, key, nullptr, &length);
    if (error == ESP_OK) return length;
    if (error != ESP_ERR_NVS_NOT_FOUND) {
      recordNvsFailure(subsystem_, NvsOperation::READ_BLOB, error);
    }
    return 0;
#endif
  }

  size_t getBytes(const char *key, void *value, size_t capacity) {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    return preferences_.getBytes(key, value, capacity);
#else
    if (!started_ || key == nullptr || value == nullptr) return 0;
    size_t length = capacity;
    const esp_err_t error = nvs_get_blob(handle_, key, value, &length);
    if (error == ESP_OK) return length;
    if (error != ESP_ERR_NVS_NOT_FOUND) {
      recordNvsFailure(subsystem_, NvsOperation::READ_BLOB, error);
    }
    return 0;
#endif
  }

  bool isKey(const char *key) {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    return preferences_.isKey(key);
#else
    if (!started_ || key == nullptr) return false;
    size_t length = 0;
    const esp_err_t error = nvs_get_blob(handle_, key, nullptr, &length);
    if (error == ESP_OK) return true;
    if (error != ESP_ERR_NVS_NOT_FOUND) {
      recordNvsFailure(subsystem_, NvsOperation::READ_BLOB, error);
    }
    return false;
#endif
  }

  size_t putBytes(const char *key, const void *value, size_t length) {
    const size_t required = nvsBlobRequiredEntries(length);
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    const size_t written = preferences_.putBytes(key, value, length);
    if (written != length) {
      const int32_t error = persistence_host::lastOperationError == 0
                                ? HOST_NVS_ERROR
                                : persistence_host::lastOperationError;
      recordNvsFailure(subsystem_, NvsOperation::WRITE_BLOB, error, required);
    }
    return written;
#else
    if (!started_ || key == nullptr || value == nullptr || readOnly_) {
      recordNvsFailure(subsystem_, NvsOperation::WRITE_BLOB,
                       ESP_ERR_NVS_INVALID_HANDLE, required);
      return 0;
    }
    esp_err_t error = nvs_set_blob(handle_, key, value, length);
    if (error != ESP_OK) {
      recordNvsFailure(subsystem_, NvsOperation::WRITE_BLOB, error, required);
      return 0;
    }
    error = nvs_commit(handle_);
    if (error != ESP_OK) {
      recordNvsFailure(subsystem_, NvsOperation::COMMIT, error, required);
      return 0;
    }
    return length;
#endif
  }

  bool remove(const char *key) {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    const bool removed = preferences_.remove(key);
    if (!removed) recordNvsFailure(subsystem_, NvsOperation::REMOVE, HOST_NVS_ERROR);
    return removed;
#else
    if (!started_ || key == nullptr || readOnly_) {
      recordNvsFailure(subsystem_, NvsOperation::REMOVE,
                       ESP_ERR_NVS_INVALID_HANDLE);
      return false;
    }
    esp_err_t error = nvs_erase_key(handle_, key);
    if (error != ESP_OK) {
      recordNvsFailure(subsystem_, NvsOperation::REMOVE, error);
      return false;
    }
    error = nvs_commit(handle_);
    if (error != ESP_OK) {
      recordNvsFailure(subsystem_, NvsOperation::COMMIT, error);
      return false;
    }
    return true;
#endif
  }

  bool clear() {
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
    const bool cleared = preferences_.clear();
    if (!cleared) recordNvsFailure(subsystem_, NvsOperation::CLEAR, HOST_NVS_ERROR);
    return cleared;
#else
    if (!started_ || readOnly_) {
      recordNvsFailure(subsystem_, NvsOperation::CLEAR,
                       ESP_ERR_NVS_INVALID_HANDLE);
      return false;
    }
    esp_err_t error = nvs_erase_all(handle_);
    if (error != ESP_OK) {
      recordNvsFailure(subsystem_, NvsOperation::CLEAR, error);
      return false;
    }
    error = nvs_commit(handle_);
    if (error != ESP_OK) {
      recordNvsFailure(subsystem_, NvsOperation::COMMIT, error);
      return false;
    }
    return true;
#endif
  }

 private:
  NvsSubsystem subsystem_;
#if defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
  Preferences preferences_;
#else
  nvs_handle_t handle_ = 0;
  bool started_ = false;
  bool readOnly_ = false;
#endif
};
#endif

}  // namespace openbrewbyweight

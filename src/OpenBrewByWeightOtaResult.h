#pragma once

// OTA operation results and their stable names, shared by the OTA service,
// its HTTP routes, and the Web UI log formatter.

#include <stdint.h>

namespace openbrewbyweight {

enum class OtaResult : uint8_t {
  OK = 0,
  UNAVAILABLE,
  BUSY,
  PENDING_VERIFY,  // this boot has not proven itself yet; esp_ota_begin refuses
  BAD_LENGTH,
  TOO_LARGE,
  RECEIVE_FAILED,
  BAD_IMAGE,
  NO_TAG,
  ARCH_MISMATCH,
  DOWNGRADE,
  WRITE_FAILED,
  VERIFY_FAILED,
  SAFETY_LOST,
  NOTHING_STAGED,
  COMMIT_FAILED,
  NO_MEMORY,
  // This build never declared which board it is for, so no image can be proven
  // compatible with it. Reachable on firmware not produced by ./scripts/dev build.
  NO_IDENTITY,
  SESSION_REQUIRED,
  SESSION_CONFLICT,
  SESSION_IDENTITY_MISMATCH,
  OFFSET_MISMATCH,
  INVALID_RANGE,
  HASH_MISMATCH,
  SESSION_EXPIRED,
  JOURNAL_FAILED,
  INTERNAL,
  // A verified image is already selected for the next boot; only a restart
  // remains. Appended so existing numeric results keep their values.
  COMMITTED,
};

inline const char *otaResultName(OtaResult result) {
  switch (result) {
    case OtaResult::OK: return "OK";
    case OtaResult::UNAVAILABLE: return "UNAVAILABLE";
    case OtaResult::BUSY: return "BUSY";
    case OtaResult::PENDING_VERIFY: return "PENDING_VERIFY";
    case OtaResult::BAD_LENGTH: return "BAD_LENGTH";
    case OtaResult::TOO_LARGE: return "TOO_LARGE";
    case OtaResult::RECEIVE_FAILED: return "RECEIVE_FAILED";
    case OtaResult::BAD_IMAGE: return "BAD_IMAGE";
    case OtaResult::NO_TAG: return "NO_TAG";
    case OtaResult::ARCH_MISMATCH: return "ARCH_MISMATCH";
    case OtaResult::DOWNGRADE: return "DOWNGRADE";
    case OtaResult::WRITE_FAILED: return "WRITE_FAILED";
    case OtaResult::VERIFY_FAILED: return "VERIFY_FAILED";
    case OtaResult::SAFETY_LOST: return "SAFETY_LOST";
    case OtaResult::NOTHING_STAGED: return "NOTHING_STAGED";
    case OtaResult::COMMIT_FAILED: return "COMMIT_FAILED";
    case OtaResult::NO_MEMORY: return "NO_MEMORY";
    case OtaResult::NO_IDENTITY: return "NO_IDENTITY";
    case OtaResult::SESSION_REQUIRED: return "SESSION_REQUIRED";
    case OtaResult::SESSION_CONFLICT: return "OTA_SESSION_CONFLICT";
    case OtaResult::SESSION_IDENTITY_MISMATCH:
      return "OTA_SESSION_IDENTITY_MISMATCH";
    case OtaResult::OFFSET_MISMATCH: return "OTA_OFFSET_MISMATCH";
    case OtaResult::INVALID_RANGE: return "OTA_INVALID_RANGE";
    case OtaResult::HASH_MISMATCH: return "OTA_SHA256_MISMATCH";
    case OtaResult::SESSION_EXPIRED: return "OTA_SESSION_EXPIRED";
    case OtaResult::JOURNAL_FAILED: return "OTA_JOURNAL_FAILED";
    case OtaResult::INTERNAL: return "INTERNAL";
    case OtaResult::COMMITTED: return "OTA_ALREADY_FLASHED";
  }
  return "UNKNOWN";
}

}  // namespace openbrewbyweight

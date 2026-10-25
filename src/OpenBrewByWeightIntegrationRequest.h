#pragma once

#include "OpenBrewByWeightIntegrationState.h"

#include <cJSON.h>
#include <cmath>
#include <cstring>

namespace openbrewbyweight {

struct IntegrationQuickSettingRequest {
  uint32_t baseRevision = 0;
  // Settings present (bit = QuickSettingField index) and their values.
  uint8_t mask = 0;
  uint8_t value[QUICK_SETTING_FIELD_COUNT] = {};
  // QUICK_REVERT_*_ACTION for `revertAfter` (with its policy) / `revertNow`,
  // else 0. Either one stands alone; settings may arrive together.
  uint8_t revertAction = 0;
  uint8_t revertPolicy = 0;
};

inline bool parseIntegrationQuickSettingRequest(
    const cJSON *root, IntegrationQuickSettingRequest &output) {
  if (!cJSON_IsObject(root)) return false;
  const cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "baseRevision");
  if (!cJSON_IsNumber(revision) || !std::isfinite(revision->valuedouble) ||
      revision->valuedouble < 0 || revision->valuedouble > UINT32_MAX ||
      std::floor(revision->valuedouble) != revision->valuedouble) return false;

  int selected = 0;
  for (const cJSON *item = root->child; item != nullptr; item = item->next) {
    if (item->string == nullptr || strcmp(item->string, "baseRevision") == 0)
      continue;
    ++selected;
    struct BooleanField {
      const char *name;
      QuickSettingField field;
    };
    static constexpr BooleanField fields[] = {
        {"brewByWeight", QuickSettingField::BREW_BY_WEIGHT},
        {"autoToManualGuardEnabled", QuickSettingField::AUTO_TO_MANUAL_GUARD},
        {"slowExtractionGuardEnabled", QuickSettingField::SLOW_EXTRACTION_GUARD},
        {"fastExtractionGuardEnabled", QuickSettingField::FAST_EXTRACTION_GUARD},
        {"avoidAccidentalTouchEnabled", QuickSettingField::AVOID_ACCIDENTAL_TOUCH},
        {"cupProtectionEnabled", QuickSettingField::CUP_PROTECTION},
    };
    int field = -1;
    uint8_t value = 0;
    for (const auto &candidate : fields) {
      if (strcmp(item->string, candidate.name) != 0) continue;
      if (!cJSON_IsBool(item)) return false;
      field = static_cast<int>(candidate.field);
      value = cJSON_IsTrue(item) ? 1 : 0;
      break;
    }
    if (field < 0 && strcmp(item->string, "noScaleBbwMode") == 0) {
      if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
      field = static_cast<int>(QuickSettingField::NO_SCALE_BBW_MODE);
      if (strcmp(item->valuestring, "off") == 0) {
        value = static_cast<uint8_t>(NoScaleBbwMode::OFF);
      } else if (strcmp(item->valuestring, "warn_once") == 0) {
        value = static_cast<uint8_t>(NoScaleBbwMode::WARN_ONCE);
      } else if (strcmp(item->valuestring, "require_scale") == 0) {
        value = static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE);
      } else {
        return false;
      }
    }
    if (field >= 0) {
      const auto bit = static_cast<uint8_t>(1U << field);
      if (output.mask & bit) return false;
      output.mask |= bit;
      output.value[field] = value;
    } else if (strcmp(item->string, "revertAfter") == 0) {
      if (!cJSON_IsString(item) || item->valuestring == nullptr) return false;
      while (output.revertPolicy < QUICK_REVERT_POLICY_COUNT &&
             strcmp(item->valuestring, QUICK_REVERT_POLICY_IDS[output.revertPolicy]) != 0)
        ++output.revertPolicy;
      if (output.revertPolicy == QUICK_REVERT_POLICY_COUNT) return false;
      output.revertAction = QUICK_REVERT_POLICY_ACTION;
    } else if (strcmp(item->string, "revertNow") == 0 && cJSON_IsTrue(item)) {
      output.revertAction = QUICK_REVERT_NOW_ACTION;
    } else {
      return false;
    }
  }
  output.baseRevision = static_cast<uint32_t>(revision->valuedouble);
  // One baseRevision; a revert action alone, or one or more settings.
  return cJSON_GetArraySize(root) == selected + 1 &&
         (output.revertAction != 0 ? selected == 1 : output.mask != 0);
}

inline bool parseIntegrationRestartRequest(const cJSON *root) {
  return cJSON_IsObject(root) && cJSON_GetArraySize(root) == 0;
}

inline void rollbackIntegrationStagedRequest(uint32_t &stagedRequestId,
                                             uint32_t rejectedRequestId) {
  if (stagedRequestId == rejectedRequestId) stagedRequestId = 0;
}

}  // namespace openbrewbyweight

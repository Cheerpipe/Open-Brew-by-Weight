#include "../OpenBrewByWeightIntegrationRequest.h"
#include "../OpenBrewByWeightShotLogTypes.h"

#include <cJSON.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>

namespace openbrewbyweight {
#include "../network/OpenBrewByWeightStatsJson.inc"
}

using namespace openbrewbyweight;

namespace {

int failures = 0;

#define CHECK(expression)                                                     \
  do {                                                                        \
    if (!(expression)) {                                                      \
      std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: "          \
                << #expression << '\n';                                       \
      ++failures;                                                             \
    }                                                                         \
  } while (false)

bool parse(const char *json, IntegrationQuickSettingRequest &request) {
  cJSON *root = cJSON_Parse(json);
  const bool valid = parseIntegrationQuickSettingRequest(root, request);
  cJSON_Delete(root);
  return valid;
}

void valid_quick_settings_map_exactly() {
  struct Case {
    const char *json;
    QuickSettingField field;
    uint8_t value;
  } cases[] = {
      {R"({"baseRevision":7,"brewByWeight":false})",
       QuickSettingField::BREW_BY_WEIGHT, 0},
      {R"({"baseRevision":7,"noScaleBbwMode":"require_scale"})",
       QuickSettingField::NO_SCALE_BBW_MODE,
       static_cast<uint8_t>(NoScaleBbwMode::REQUIRE_SCALE)},
      {R"({"baseRevision":7,"autoToManualGuardEnabled":false})",
       QuickSettingField::AUTO_TO_MANUAL_GUARD, 0},
      {R"({"baseRevision":7,"slowExtractionGuardEnabled":false})",
       QuickSettingField::SLOW_EXTRACTION_GUARD, 0},
      {R"({"baseRevision":7,"fastExtractionGuardEnabled":false})",
       QuickSettingField::FAST_EXTRACTION_GUARD, 0},
      {R"({"baseRevision":7,"avoidAccidentalTouchEnabled":false})",
       QuickSettingField::AVOID_ACCIDENTAL_TOUCH, 0},
      {R"({"baseRevision":7,"cupProtectionEnabled":false})",
       QuickSettingField::CUP_PROTECTION, 0},
  };
  for (const auto &item : cases) {
    IntegrationQuickSettingRequest request;
    CHECK(parse(item.json, request));
    const auto index = static_cast<uint8_t>(item.field);
    CHECK(request.baseRevision == 7);
    CHECK(request.mask == (1U << index));
    CHECK(request.value[index] == item.value);
    CHECK(request.revertAction == 0);
  }
  // A patch carries every changed setting at once.
  IntegrationQuickSettingRequest patch;
  CHECK(parse(R"({"baseRevision":7,"cupProtectionEnabled":true,)"
              R"("noScaleBbwMode":"off","brewByWeight":false})", patch));
  CHECK(patch.mask == ((1U << static_cast<uint8_t>(QuickSettingField::CUP_PROTECTION)) |
                       (1U << static_cast<uint8_t>(QuickSettingField::NO_SCALE_BBW_MODE)) |
                       (1U << static_cast<uint8_t>(QuickSettingField::BREW_BY_WEIGHT))));
  CHECK(patch.value[static_cast<uint8_t>(QuickSettingField::CUP_PROTECTION)] == 1);
  CHECK(patch.value[static_cast<uint8_t>(QuickSettingField::BREW_BY_WEIGHT)] == 0);
  CHECK(patch.revertAction == 0);
  IntegrationQuickSettingRequest revert;
  CHECK(parse(R"({"baseRevision":7,"revertAfter":"30m"})", revert));
  CHECK(revert.revertAction == QUICK_REVERT_POLICY_ACTION);
  CHECK(revert.revertPolicy == static_cast<uint8_t>(QuickRevertPolicy::MINUTES_30));
  CHECK(revert.mask == 0);
  IntegrationQuickSettingRequest now;
  CHECK(parse(R"({"baseRevision":7,"revertNow":true})", now));
  CHECK(now.revertAction == QUICK_REVERT_NOW_ACTION);
}

void malformed_quick_settings_fail_closed() {
  const char *cases[] = {
      "{}",
      R"({"baseRevision":7,"brewByWeight":1})",
      R"({"baseRevision":7})",
      R"({"baseRevision":7,"baseRevision":7,"cupProtectionEnabled":true})",
      R"({"baseRevision":7,"cupProtectionEnabled":true,"revertAfter":"keep"})",
      R"({"baseRevision":7,"revertNow":true,"cupProtectionEnabled":true})",
      R"({"baseRevision":7,"revertAfter":"keep","revertAfter":"15m"})",
      R"({"baseRevision":7,"cupProtectionEnabled":true,"extra":false})",
      R"({"baseRevision":7,"cupProtectionEnabled":true,"cupProtectionEnabled":false})",
      R"({"baseRevision":true,"cupProtectionEnabled":true})",
      R"({"baseRevision":1.5,"cupProtectionEnabled":true})",
      R"({"baseRevision":-1,"cupProtectionEnabled":true})",
      R"({"baseRevision":4294967296,"cupProtectionEnabled":true})",
      R"({"baseRevision":7,"cupProtectionEnabled":1})",
      R"({"baseRevision":7,"noScaleBbwMode":"invalid"})",
      R"({"baseRevision":7,"noScaleBbwMode":false})",
      R"({"baseRevision":7,"revertAfter":"2h"})",
      R"({"baseRevision":7,"revertAfter":15})",
      R"({"baseRevision":7,"revertNow":false})",
      R"({"baseRevision":7,"revertNow":true,"revertAfter":"keep"})",
      "[]",
      "null",
  };
  for (const char *json : cases) {
    IntegrationQuickSettingRequest request;
    CHECK(!parse(json, request));
  }
}

void restart_requires_exact_empty_object() {
  const char *cases[] = {"{}", "null", "[]", R"({"extra":true})"};
  for (size_t index = 0; index < 4; ++index) {
    cJSON *root = cJSON_Parse(cases[index]);
    CHECK(parseIntegrationRestartRequest(root) == (index == 0));
    cJSON_Delete(root);
  }
  CHECK(!parseIntegrationRestartRequest(nullptr));
}

void queue_rejection_rolls_back_only_its_staging_slot() {
  uint32_t staged = 17;
  rollbackIntegrationStagedRequest(staged, 16);
  CHECK(staged == 17);
  rollbackIntegrationStagedRequest(staged, 17);
  CHECK(staged == 0);
}

void stats_json_preserves_independent_metrics() {
  ShotLogRecord rows[3] = {};
  for (unsigned i = 0; i < 3; ++i) {
    rows[i].durationDs = 280 + i * 20;
    rows[i].actualWeightCg = 3690 + i * 100;
    rows[i].goalWeightDg = 360;
    rows[i].avgFlowCgS = 142 + i * 10;
    rows[i].hasWallTime = true;
    rows[i].endedAtLocalSec = 86400;
    rows[i].shotType = static_cast<uint8_t>(ShotLogType::AUTO);
    rows[i].stopDetail = static_cast<uint8_t>(ShotLogStopDetail::ACTIVATOR);
  }
  const auto view = [&rows](size_t count, size_t window = SHOT_LOG_STATS_WINDOW) {
    return shotLogStatsView(count, window, [&rows](size_t n) -> const ShotLogRecord & {
      return rows[n];
    });
  };
  const auto build = [](const ShotStatsView &stats, char *output, size_t capacity) {
    return buildIntegrationStats(stats, output, capacity, SHOT_LOG_STATS_WINDOW, true);
  };
  char output[384];
  for (unsigned count = 1; count <= 3; ++count) {
    const ShotStatsView stats = view(count);
    CHECK(build(stats, output, sizeof(output)));
    cJSON *root = cJSON_Parse(output);
    CHECK(root != nullptr);
    const auto number = [root](const char *key) {
      return cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(root, key));
    };
    CHECK(number("window") == SHOT_LOG_STATS_WINDOW);
    CHECK(number("shotCount") == count);
    CHECK(number("avgDurationS") == 27.0 + count);
    CHECK(std::fabs(number("avgYieldG") - (36.4 + count * 0.5)) < 0.001);
    CHECK(std::fabs(number("avgFlowGps") - (1.37 + count * 0.05)) < 0.001);
    CHECK(number("shotsPerDay") == count);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "avgErrorPct")));
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "durationsS")) ==
          static_cast<int>(count));
    cJSON_Delete(root);
  }
  // Weight cuts average their own newest window, past the shot window.
  rows[2].stopDetail = static_cast<uint8_t>(ShotLogStopDetail::NORMAL_TARGET);
  CHECK(shotLogBbwErrorEligible(rows[2]));
  const ShotStatsView bbwStats = view(3, 1);
  CHECK(bbwStats.shotCount == 1 && bbwStats.bbwCount == 1);
  CHECK(build(bbwStats, output, sizeof(output)));
  CHECK(strstr(output, "\"avgErrorPct\":8.0") != nullptr);
  // Ineligible records never count; a wider window keeps ten durations.
  rows[1].actualWeightCg = 150;
  CHECK(view(3).shotCount == 2);
  CHECK(buildIntegrationStats(view(3), output, sizeof(output), 50, false));
  CHECK(strstr(output, "\"window\"") == nullptr && strstr(output, "totalDurationS") == nullptr &&
        strstr(output, "durationsS") == nullptr);
  rows[1].actualWeightCg = 3790;
  rows[0].avgFlowCgS = SHOT_LOG_METRIC_MISSING;
  rows[0].hasWallTime = false;
  CHECK(build(view(1), output, sizeof(output)));
  CHECK(strstr(output, "\"avgFlowGps\":null") != nullptr);
  CHECK(strstr(output, "\"shotsPerDay\":null") != nullptr);
  CHECK(strstr(output, "\"avgDurationS\":28.0") != nullptr);
  CHECK(build({}, output, sizeof(output)));
  for (const char *field : {"avgDurationS", "avgYieldG", "avgFlowGps",
                            "avgErrorPct", "shotsPerDay"}) {
    cJSON *root = cJSON_Parse(output);
    CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, field)));
    cJSON_Delete(root);
  }
  const ShotStatsView stats = view(3);
  CHECK(build(stats, output, sizeof(output)));
  const size_t length = strlen(output);
  for (size_t capacity = 0; capacity <= length; ++capacity)
    CHECK(!build(stats, output, capacity));
  CHECK(build(stats, output, length + 1));
}

}  // namespace

int main() {
  valid_quick_settings_map_exactly();
  malformed_quick_settings_fail_closed();
  restart_requires_exact_empty_object();
  queue_rejection_rolls_back_only_its_staging_slot();
  stats_json_preserves_independent_metrics();
  std::cout << "integration request parser: " << failures << " failures\n";
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

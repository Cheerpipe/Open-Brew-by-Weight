#pragma once

#include "OpenBrewByWeightDomain.h"

namespace openbrewbyweight {

enum class QuickSettingField : uint8_t {
  BREW_BY_WEIGHT,
  NO_SCALE_BBW_MODE,
  AUTO_TO_MANUAL_GUARD,
  SLOW_EXTRACTION_GUARD,
  FAST_EXTRACTION_GUARD,
  AVOID_ACCIDENTAL_TOUCH,
  CUP_PROTECTION
};

struct QuickSettingsSnapshot {
  uint32_t revision = 0;
  uint8_t activePresetId = 0;
  bool brewByWeight = false;
  uint8_t noScaleBbwMode = static_cast<uint8_t>(NoScaleBbwMode::OFF);
  bool autoToManualGuardEnabled = false;
  bool slowExtractionGuardEnabled = false;
  bool fastExtractionGuardEnabled = false;
  bool avoidAccidentalTouchEnabled = false;
  bool cupProtectionEnabled = false;
};

inline QuickSettingsSnapshot quickSettingsSnapshot(const RecipeSnapshot &recipe) {
  QuickSettingsSnapshot snapshot;
  snapshot.revision = recipe.runtime.revision;
  snapshot.activePresetId = recipe.presets.activeId;
  snapshot.brewByWeight = !recipe.runtime.timerOnly;
  snapshot.noScaleBbwMode = recipe.runtime.noScaleBbwMode;
  snapshot.autoToManualGuardEnabled = recipe.runtime.scaleLossEnabled;
  snapshot.slowExtractionGuardEnabled = recipe.runtime.slowSideEnabled;
  snapshot.fastExtractionGuardEnabled = recipe.runtime.fastSideEnabled;
  snapshot.avoidAccidentalTouchEnabled =
      recipe.runtime.avoidAccidentalTouchEnabled;
  snapshot.cupProtectionEnabled = recipe.runtime.cupProtectionEnabled;
  return snapshot;
}

constexpr uint8_t QUICK_SETTING_FIELD_COUNT =
    static_cast<uint8_t>(QuickSettingField::CUP_PROTECTION) + 1;
// Wire names in field order, shared by the quick-settings request and stream.
constexpr const char *QUICK_SETTING_FIELD_IDS[QUICK_SETTING_FIELD_COUNT] = {
    "brewByWeight", "noScaleBbwMode", "autoToManualGuardEnabled", "slowExtractionGuardEnabled",
    "fastExtractionGuardEnabled", "avoidAccidentalTouchEnabled", "cupProtectionEnabled"};
static_assert(sizeof(ControlStatusFields::quickSaved) == QUICK_SETTING_FIELD_COUNT,
              "one published saved value per Quick Settings field");

[[gnu::noinline]] inline uint8_t quickSettingValue(const RuntimeConfig &config, QuickSettingField field) {
  switch (field) {
    case QuickSettingField::BREW_BY_WEIGHT: return !config.timerOnly;
    case QuickSettingField::NO_SCALE_BBW_MODE: return noScaleBbwModeValue(config.noScaleBbwMode);
    case QuickSettingField::AUTO_TO_MANUAL_GUARD: return config.scaleLossEnabled;
    case QuickSettingField::SLOW_EXTRACTION_GUARD: return config.slowSideEnabled;
    case QuickSettingField::FAST_EXTRACTION_GUARD: return config.fastSideEnabled;
    case QuickSettingField::AVOID_ACCIDENTAL_TOUCH: return config.avoidAccidentalTouchEnabled;
    case QuickSettingField::CUP_PROTECTION: return config.cupProtectionEnabled;
  }
  return 0;
}

[[gnu::noinline]] inline void setQuickSettingValue(RuntimeConfig &config, QuickSettingField field, uint8_t value) {
  switch (field) {
    case QuickSettingField::BREW_BY_WEIGHT: config.timerOnly = value == 0; break;
    case QuickSettingField::NO_SCALE_BBW_MODE:
      config.noScaleBbwMode = static_cast<uint8_t>(
          packNoScaleBbwMode(value, noScaleAllowRinseWhileArmed(config.noScaleBbwMode)) |
          (config.noScaleBbwMode & IDLE_ACCESSORY_RETARE));
      break;
    case QuickSettingField::AUTO_TO_MANUAL_GUARD: config.scaleLossEnabled = value != 0; break;
    case QuickSettingField::SLOW_EXTRACTION_GUARD: config.slowSideEnabled = value != 0; break;
    case QuickSettingField::FAST_EXTRACTION_GUARD: config.fastSideEnabled = value != 0; break;
    case QuickSettingField::AVOID_ACCIDENTAL_TOUCH: config.avoidAccidentalTouchEnabled = value != 0; break;
    case QuickSettingField::CUP_PROTECTION: config.cupProtectionEnabled = value != 0; break;
  }
}

// How long session overrides last (Web UI "Duration"). One timer covers every
// override and restarts on each change; KEEP waits for a preset change or reboot.
enum class QuickRevertPolicy : uint8_t { NEXT_SHOT, MINUTES_15, MINUTES_30, MINUTES_60, KEEP };
constexpr uint8_t QUICK_REVERT_POLICY_COUNT = static_cast<uint8_t>(QuickRevertPolicy::KEEP) + 1;
constexpr const char *QUICK_REVERT_POLICY_IDS[QUICK_REVERT_POLICY_COUNT] = {
    "next_shot", "15m", "30m", "60m", "keep"};
inline const char *quickRevertPolicyId(uint8_t policy) {
  return QUICK_REVERT_POLICY_IDS[policy < QUICK_REVERT_POLICY_COUNT ? policy : QUICK_REVERT_POLICY_COUNT - 1];
}
// Quick Settings command actions past the fields: set the policy, revert now.
constexpr uint8_t QUICK_REVERT_POLICY_ACTION = 0x80;
constexpr uint8_t QUICK_REVERT_NOW_ACTION = 0x81;

// Quick Settings changed for this session only. RAM-resident, never persisted:
// the revert policy, a preset change, or a reboot drops them. `saved` holds the
// persisted value each override hides, so persistence and Settings edits never
// see the override.
struct SessionQuickSettings {
  uint8_t mask = 0;
  uint8_t presetId = 0;
  uint8_t value[QUICK_SETTING_FIELD_COUNT] = {};
  uint8_t saved[QUICK_SETTING_FIELD_COUNT] = {};
  QuickRevertPolicy revertPolicy = QuickRevertPolicy::KEEP;
  uint32_t changedAtMs = 0;
  bool shotEnded = false;
};

inline uint32_t quickRevertRemainingMs(const SessionQuickSettings &session, uint32_t nowMs) {
  static constexpr uint8_t minutes[QUICK_REVERT_POLICY_COUNT] = {0, 15, 30, 60, 0};
  const uint32_t delayMs = minutes[static_cast<uint8_t>(session.revertPolicy)] * 60000UL;
  const uint32_t elapsed = nowMs - session.changedAtMs;
  return session.mask == 0 || delayMs == 0 || elapsed >= delayMs ? 0 : delayMs - elapsed;
}

inline bool quickRevertDue(const SessionQuickSettings &session, uint32_t nowMs) {
  if (session.mask == 0 || session.revertPolicy == QuickRevertPolicy::KEEP) return false;
  return session.revertPolicy == QuickRevertPolicy::NEXT_SHOT ? session.shotEnded
                                                               : quickRevertRemainingMs(session, nowMs) == 0;
}

// Brew by weight and the fast/slow sides belong to the active preset; the
// other fields are device settings.
inline bool quickSettingFromPreset(QuickSettingField field) {
  return field == QuickSettingField::BREW_BY_WEIGHT ||
         field == QuickSettingField::SLOW_EXTRACTION_GUARD ||
         field == QuickSettingField::FAST_EXTRACTION_GUARD;
}

inline uint8_t presetQuickSettingValue(const ShotPreset &preset, QuickSettingField field) {
  return field == QuickSettingField::BREW_BY_WEIGHT ? preset.brewByWeight
       : field == QuickSettingField::SLOW_EXTRACTION_GUARD ? preset.slowSideEnabled
                                                           : preset.fastSideEnabled;
}

// Applies each override's session value, or with `saved` the persisted value it
// hides (the config as it must reach flash and the Settings page).
[[gnu::noinline]] inline RuntimeConfig overlaySessionQuickSettings(const SessionQuickSettings &session,
                                                 RuntimeConfig config, bool saved = false) {
  for (uint8_t index = 0; index < QUICK_SETTING_FIELD_COUNT; ++index) {
    if (session.mask & (1U << index))
      setQuickSettingValue(config, static_cast<QuickSettingField>(index),
                           saved ? session.saved[index] : session.value[index]);
  }
  return config;
}

// Sets a session value on the effective config; matching the saved value
// simply drops the override.
[[gnu::noinline]] inline void setSessionQuickSetting(SessionQuickSettings &session, uint8_t activePresetId,
                                   RuntimeConfig &effective, QuickSettingField field,
                                   uint8_t value) {
  const auto index = static_cast<uint8_t>(field);
  const auto bit = static_cast<uint8_t>(1U << index);
  if (session.mask == 0) session.presetId = activePresetId;
  if ((session.mask & bit) == 0) session.saved[index] = quickSettingValue(effective, field);
  session.value[index] = value;
  session.mask = static_cast<uint8_t>(session.saved[index] == value ? session.mask & ~bit
                                                                    : session.mask | bit);
  setQuickSettingValue(effective, field, value);
}

// Drops overrides a persisted change replaced, then re-applies the rest to a
// newly composed config. A preset change drops every override and restores the
// saved device values (the new preset supplies its own fields). `incoming` is
// the saved config a Settings edit sent, or nullptr otherwise.
[[gnu::noinline]] inline void reconcileSessionQuickSettings(SessionQuickSettings &session,
                                          const ShotPreset &active,
                                          const RuntimeConfig *incoming,
                                          RuntimeConfig &composed) {
  const bool presetChanged = active.id != session.presetId;
  for (uint8_t index = 0; index < QUICK_SETTING_FIELD_COUNT; ++index) {
    const auto field = static_cast<QuickSettingField>(index);
    if ((session.mask & (1U << index)) == 0) continue;
    const bool fromPreset = quickSettingFromPreset(field);
    if (presetChanged && !fromPreset) setQuickSettingValue(composed, field, session.saved[index]);
    const bool edited = presetChanged ||
        (fromPreset ? presetQuickSettingValue(active, field) != session.saved[index]
                    : incoming != nullptr &&
                          quickSettingValue(*incoming, field) != session.saved[index]);
    if (edited) session.mask &= static_cast<uint8_t>(~(1U << index));
  }
  composed = overlaySessionQuickSettings(session, composed);
}

inline bool quickSettingsEqual(const QuickSettingsSnapshot &left,
                               const QuickSettingsSnapshot &right) {
  return left.activePresetId == right.activePresetId &&
         left.brewByWeight == right.brewByWeight &&
         left.noScaleBbwMode == right.noScaleBbwMode &&
         left.autoToManualGuardEnabled == right.autoToManualGuardEnabled &&
         left.slowExtractionGuardEnabled == right.slowExtractionGuardEnabled &&
         left.fastExtractionGuardEnabled == right.fastExtractionGuardEnabled &&
         left.avoidAccidentalTouchEnabled == right.avoidAccidentalTouchEnabled &&
         left.cupProtectionEnabled == right.cupProtectionEnabled;
}

}  // namespace openbrewbyweight

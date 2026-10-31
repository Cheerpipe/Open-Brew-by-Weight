#pragma once

#include <initializer_list>

#include "OpenBrewByWeightDomain.h"
#include "machine/OpenBrewByWeightLineaMicraSettings.h"

namespace openbrewbyweight {

// Double · Adaptive recipe values; every new or reset preset starts here.
inline void fillDoubleFirmwareDefaults(ShotPreset &preset) {
  preset = ShotPreset{};
  preset.weightOffsetBaselineG = DEFAULT_WEIGHT_OFFSET_G;
  preset.bbwEwmaOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  preset.fastCurveHardnessPct = rampHardnessPctOf(RAMP_HARDNESS_SOFT);
  preset.slowCurveHardnessPct = rampHardnessPctOf(RAMP_HARDNESS_SOFT);
}

// Cold path (seed, restore, repair, boot upgrade): kept out of line for flash.
[[gnu::noinline]] inline void fillFactoryShotPreset(ShotPreset &preset, uint8_t id) {
  fillDoubleFirmwareDefaults(preset);
  preset.id = id;
  preset.isFactory = true;
  const bool single = id == FACTORY_PRESET_ID_SINGLE_ADAPTIVE ||
                      id == FACTORY_PRESET_ID_SINGLE_FIXED;
  const bool adaptive = id == FACTORY_PRESET_ID_DOUBLE_ADAPTIVE ||
                        id == FACTORY_PRESET_ID_SINGLE_ADAPTIVE;
  const bool manual = id == FACTORY_PRESET_ID_MANUAL;
  copyCString(preset.name, SHOT_PRESET_NAME_CAPACITY,
              manual   ? "Manual"
              : single ? (adaptive ? "Single \xC2\xB7 Adaptive" : "Single \xC2\xB7 Fixed")
                       : (adaptive ? "Double \xC2\xB7 Adaptive" : "Double \xC2\xB7 Fixed"));
  if (single) {
    preset.goalWeightDg = FACTORY_SINGLE_GOAL_WEIGHT_DG;
    preset.doseDg = FACTORY_SINGLE_DOSE_DG;
    preset.fastMarginDg =
        adaptive ? FACTORY_SINGLE_ADAPTIVE_FAST_MARGIN_DG : FACTORY_SINGLE_FAST_MARGIN_DG;
    preset.slowMarginDg = FACTORY_SINGLE_SLOW_MARGIN_DG;
    preset.fastBeforeMs = FACTORY_SINGLE_FAST_BEFORE_MS;
    preset.slowAfterMs = FACTORY_SINGLE_SLOW_AFTER_MS;
    preset.weightOffsetBaselineG = FACTORY_SINGLE_WEIGHT_OFFSET_G;
    preset.bbwEwmaOffsetG = FACTORY_SINGLE_WEIGHT_OFFSET_G;
  }
  // Fixed presets keep their margins stored but always stop at the target;
  // Manual keeps the Double recipe but never stops by weight. Only the
  // Adaptive presets carry the factory transition curves.
  preset.fastSideEnabled = adaptive;
  preset.slowSideEnabled = adaptive;
  if (adaptive) {
    preset.fastRampMs = preset.slowRampMs = FACTORY_RAMP_MS;
    preset.fastCurve = RAMP_CURVE_EARLY;
    preset.slowCurve = RAMP_CURVE_LATE;
  }
  preset.brewByWeight = !manual;
}

inline void seedDefaultShotPresetBank(ShotPresetBank &bank) {
  bank = ShotPresetBank{};
  bank.count = FACTORY_PRESET_COUNT + 1;
  bank.activeId = FACTORY_PRESET_ID_MANUAL;
  bank.nextId = FACTORY_PRESET_COUNT + 1;
  for (uint8_t i = 0; i < FACTORY_PRESET_COUNT; ++i)
    fillFactoryShotPreset(bank.presets[i], static_cast<uint8_t>(i + 1));
  fillFactoryShotPreset(bank.presets[FACTORY_PRESET_COUNT], FACTORY_PRESET_ID_MANUAL);
}

inline int findShotPresetIndex(const ShotPresetBank &bank, uint8_t id) {
  for (uint8_t i = 0; i < bank.count && i < MAX_SHOT_PRESETS; ++i) {
    if (bank.presets[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

inline ShotPreset *mutableShotPreset(ShotPresetBank &bank, uint8_t id) {
  const int index = findShotPresetIndex(bank, id);
  return index < 0 ? nullptr : &bank.presets[index];
}

inline const ShotPreset *findShotPreset(const ShotPresetBank &bank, uint8_t id) {
  const int index = findShotPresetIndex(bank, id);
  return index < 0 ? nullptr : &bank.presets[index];
}

inline bool shotPresetNameExists(const ShotPresetBank &bank, const char *name,
                                 uint8_t ignoreId) {
  if (name == nullptr || name[0] == '\0') {
    return false;
  }
  for (uint8_t i = 0; i < bank.count && i < MAX_SHOT_PRESETS; ++i) {
    if (bank.presets[i].id == ignoreId) {
      continue;
    }
    if (strncmp(bank.presets[i].name, name, SHOT_PRESET_NAME_CAPACITY) == 0) {
      return true;
    }
  }
  return false;
}

inline bool validShotPresetName(const char *name) {
  if (name == nullptr) {
    return false;
  }
  const size_t length = strnlen(name, SHOT_PRESET_NAME_CAPACITY);
  return length > 0 && length < SHOT_PRESET_NAME_CAPACITY;
}

inline void applyShotPresetToConfig(const ShotPreset &preset,
                                    RuntimeConfig &config);

inline bool validateShotPresetRecipe(const ShotPreset &preset,
                                     uint32_t machineRetareWindowMs,
                                     bool machineAutoRetare) {
  if (!validBbwAlpha(preset.bbwEwmaAlpha) ||
      !validBbwAlpha(preset.bbwAlphaBaseline) || preset.bbwAlphaLearned > 1 ||
      preset.lineaMicraBrewTargetDeciC < LINEA_MICRA_BREW_TARGET_MIN_DECI_C ||
      preset.lineaMicraBrewTargetDeciC > LINEA_MICRA_BREW_TARGET_MAX_DECI_C ||
      preset.doseDg < MIN_DOSE_DG || preset.doseDg > MAX_DOSE_DG ||
      !std::isfinite(preset.bbwEwmaOffsetG) || preset.bbwEwmaOffsetG < 0.0f ||
      preset.bbwEwmaOffsetG > MAX_OFFSET_G) return false;
  RuntimeConfig probe;
  probe.autoRetare = machineAutoRetare;
  probe.retareWindowMs = machineRetareWindowMs;
  applyShotPresetToConfig(preset, probe);
  return validateRuntimeConfig(probe) == ConfigValidationError::NONE;
}

inline bool shotPresetBankStructurallyValid(const ShotPresetBank &bank) {
  if (bank.count == 0 || bank.count > MAX_SHOT_PRESETS) {
    return false;
  }
  if (findShotPresetIndex(bank, bank.activeId) < 0) {
    return false;
  }
  for (uint8_t i = 0; i < bank.count; ++i) {
    const ShotPreset &preset = bank.presets[i];
    if (preset.id == 0 || !validShotPresetName(preset.name)) {
      return false;
    }
    for (uint8_t j = i + 1; j < bank.count; ++j) {
      if (bank.presets[j].id == preset.id) {
        return false;
      }
    }
  }
  return true;
}

inline bool validateShotPresetBank(const ShotPresetBank &bank,
                                   uint32_t machineRetareWindowMs,
                                   bool machineAutoRetare) {
  if (!shotPresetBankStructurallyValid(bank)) {
    return false;
  }
  for (uint8_t i = 0; i < bank.count; ++i) {
    if (!validateShotPresetRecipe(bank.presets[i], machineRetareWindowMs,
                                  machineAutoRetare)) {
      return false;
    }
  }
  return true;
}

inline uint32_t roundUpToWindowStep(uint32_t ms) {
  return (ms + WINDOW_STEP_MS - 1) / WINDOW_STEP_MS * WINDOW_STEP_MS;
}

// Structural repair only; a recipe that still fails validation is reset to
// its factory (or Double · Adaptive) values, keeping identity and name.
inline void ensureShotPresetBank(ShotPresetBank &bank,
                                 uint32_t machineRetareWindowMs,
                                 bool machineAutoRetare) {
  if (bank.count == 0 || bank.count > MAX_SHOT_PRESETS) {
    seedDefaultShotPresetBank(bank);
    return;
  }
  if (findShotPresetIndex(bank, bank.activeId) < 0) {
    bank.activeId = bank.presets[0].id;
  }
  for (uint8_t i = 0; i < bank.count; ++i) {
    ShotPreset &preset = bank.presets[i];
    if (!validShotPresetName(preset.name)) {
      snprintf(preset.name, SHOT_PRESET_NAME_CAPACITY, "Preset %u",
               static_cast<unsigned>(preset.id));
    }
    if (preset.operationalWallMs < 5000 ||
        preset.operationalWallMs > HARD_MAX_CIRCUIT_CLOSED_MS) {
      preset.operationalWallMs = DEFAULT_OPERATIONAL_WALL_MS;
    }
    if (machineAutoRetare &&
        preset.bbwProtectionMs <
            machineRetareWindowMs + MIN_BBW_PROTECTION_AFTER_RETARE_MS) {
      preset.bbwProtectionMs =
          machineRetareWindowMs + MIN_BBW_PROTECTION_AFTER_RETARE_MS;
    }
    if (preset.fastBeforeMs < preset.bbwProtectionMs + MIN_WINDOW_AFTER_DELAY_MS) {
      preset.fastBeforeMs =
          roundUpToWindowStep(preset.bbwProtectionMs + MIN_WINDOW_AFTER_DELAY_MS);
    }
    if (preset.slowAfterMs < preset.fastBeforeMs + MIN_WINDOW_SPAN_MS) {
      preset.slowAfterMs = preset.fastBeforeMs + MIN_WINDOW_SPAN_MS;
    }
    if (preset.scaleLossFixedMs != 0 &&
        (preset.scaleLossFixedMs < preset.bbwProtectionMs ||
         preset.scaleLossFixedMs > preset.operationalWallMs)) {
      preset.scaleLossFixedMs = 0;
    }
    // Transitions out of range fall back to the default 5 s Even + Soft.
    for (uint16_t *ramp : {&preset.fastRampMs, &preset.slowRampMs})
      if (*ramp > MAX_RAMP_MS || *ramp % RAMP_STEP_MS != 0) *ramp = DEFAULT_RAMP_MS;
    for (uint8_t *curve : {&preset.fastCurve, &preset.slowCurve})
      if (*curve >= RAMP_CURVE_TYPES) *curve = RAMP_CURVE_EVEN;
    // Continuous hardness: a pct a pre-continuous image never wrote (or a bad
    // value) derives from its legacy enum; a valid pct refreshes the enum
    // mirror to the nearest level, so both stay consistent and an OTA
    // downgrade still reads a sane enum.
    for (auto &side : {std::pair(&preset.fastCurveHardness, &preset.fastCurveHardnessPct),
                       std::pair(&preset.slowCurveHardness, &preset.slowCurveHardnessPct)}) {
      if (*side.first >= RAMP_HARDNESS_LEVELS) *side.first = RAMP_HARDNESS_SOFT;
      if (*side.second > RAMP_HARDNESS_PCT_MAX)
        *side.second = rampHardnessPctOf(*side.first);
      else
        *side.first = rampNearestHardness(*side.second);
    }
    if (!std::isfinite(preset.weightOffsetBaselineG) ||
        preset.weightOffsetBaselineG < 0.0f ||
        preset.weightOffsetBaselineG > MAX_OFFSET_G) {
      preset.weightOffsetBaselineG = DEFAULT_WEIGHT_OFFSET_G;
    }
    if (!validateShotPresetRecipe(preset, machineRetareWindowMs,
                                  machineAutoRetare)) {
      ShotPreset reset;
      if (factoryShotPresetId(preset.id) && preset.isFactory) {
        fillFactoryShotPreset(reset, preset.id);
      } else {
        fillDoubleFirmwareDefaults(reset);
        reset.id = preset.id;
      }
      memcpy(reset.name, preset.name, sizeof(reset.name));
      reset.isFactory = preset.isFactory;
      if (machineAutoRetare &&
          reset.bbwProtectionMs <
              machineRetareWindowMs + MIN_BBW_PROTECTION_AFTER_RETARE_MS) {
        reset.bbwProtectionMs =
            machineRetareWindowMs + MIN_BBW_PROTECTION_AFTER_RETARE_MS;
      }
      preset = reset;
    }
  }
  if (!shotPresetBankStructurallyValid(bank)) {
    // Last resort only when structurally irreparable.
    seedDefaultShotPresetBank(bank);
  }
}

inline const ShotPreset &activeShotPreset(const ShotPresetBank &bank) {
  const ShotPreset *preset = findShotPreset(bank, bank.activeId);
  if (preset != nullptr) {
    return *preset;
  }
  return bank.presets[0];
}

inline ShotPreset &mutableActiveShotPreset(ShotPresetBank &bank) {
  ShotPreset *preset = mutableShotPreset(bank, bank.activeId);
  if (preset != nullptr) {
    return *preset;
  }
  bank.activeId = bank.presets[0].id;
  return bank.presets[0];
}

inline bool setActiveShotPreset(ShotPresetBank &bank, uint8_t id) {
  if (findShotPresetIndex(bank, id) < 0) {
    return false;
  }
  bank.activeId = id;
  return true;
}

inline bool setShotPresetLineaMicraBrewTarget(ShotPresetBank &bank, uint8_t id,
                                              uint16_t targetDeciC) {
  ShotPreset *preset = mutableShotPreset(bank, id);
  if (preset == nullptr || targetDeciC < LINEA_MICRA_BREW_TARGET_MIN_DECI_C ||
      targetDeciC > LINEA_MICRA_BREW_TARGET_MAX_DECI_C) {
    return false;
  }
  preset->lineaMicraBrewTargetDeciC = targetDeciC;
  return true;
}

// Overlays the active preset's recipe on the device config. Device-level
// fields (cup protection, touch, scale-loss switch) are never overlaid; the
// stop mode comes only from the preset (Manual is a preset, not a session).
inline void applyShotPresetToConfig(const ShotPreset &preset,
                                    RuntimeConfig &config) {
  config.goalWeightDg = preset.goalWeightDg;
  config.weightOffsetG = preset.bbwEwmaOffsetG;
  config.weightOffsetBaselineG = preset.weightOffsetBaselineG;
  config.bbwProtectionMs = preset.bbwProtectionMs;
  config.operationalWallMs = preset.operationalWallMs;
  config.fastSideEnabled = preset.fastSideEnabled;
  config.fastMarginDg = preset.fastMarginDg;
  config.fastBeforeMs = preset.fastBeforeMs;
  config.slowSideEnabled = preset.slowSideEnabled;
  config.slowMarginDg = preset.slowMarginDg;
  config.slowAfterMs = preset.slowAfterMs;
  config.fastRampMs = preset.fastRampMs;
  config.slowRampMs = preset.slowRampMs;
  config.fastCurve = preset.fastCurve;
  config.fastCurveHardness = preset.fastCurveHardness;
  config.fastCurveHardnessPct = preset.fastCurveHardnessPct;
  config.slowCurve = preset.slowCurve;
  config.slowCurveHardness = preset.slowCurveHardness;
  config.slowCurveHardnessPct = preset.slowCurveHardnessPct;
  config.scaleLossFixedMs = preset.scaleLossFixedMs;
  config.timerOnly = !preset.brewByWeight;
}

inline RuntimeConfig composeEffectiveConfig(const RuntimeConfig &machine,
                                            const ShotPresetBank &bank) {
  RuntimeConfig effective = machine;
  applyShotPresetToConfig(activeShotPreset(bank), effective);
  return effective;
}

// Device-level protections shared by every preset.
inline void copyDeviceProtectionFromConfig(const RuntimeConfig &from,
                                           RuntimeConfig &to) {
  to.cupProtectionEnabled = from.cupProtectionEnabled;
  to.stopIfCupRemoved = from.stopIfCupRemoved;
  to.requireCupToStart = from.requireCupToStart;
  to.avoidAccidentalTouchEnabled = from.avoidAccidentalTouchEnabled;
  to.touchStopFallbackEnabled = from.touchStopFallbackEnabled;
  to.scaleLossEnabled = from.scaleLossEnabled;
}

// Writes the editable recipe; learned drip state is never copied here.
// `fields` limits the copy to the PresetRecipeField bits a partial save sent.
inline void copyUserRecipeFromConfig(const RuntimeConfig &config,
                                     ShotPreset &preset,
                                     uint32_t fields = PRESET_RECIPE_ALL) {
  if (fields & PRESET_RECIPE_BREW_BY_WEIGHT) preset.brewByWeight = !config.timerOnly;
  if (fields & PRESET_RECIPE_GOAL_WEIGHT) preset.goalWeightDg = config.goalWeightDg;
  if (fields & PRESET_RECIPE_OPERATIONAL_WALL) preset.operationalWallMs = config.operationalWallMs;
  if (fields & PRESET_RECIPE_BBW_PROTECTION) preset.bbwProtectionMs = config.bbwProtectionMs;
  preset.weightOffsetBaselineG = config.weightOffsetBaselineG;
  if (fields & PRESET_RECIPE_FAST_ENABLED) preset.fastSideEnabled = config.fastSideEnabled;
  if (fields & PRESET_RECIPE_FAST_MARGIN) preset.fastMarginDg = config.fastMarginDg;
  if (fields & PRESET_RECIPE_FAST_BEFORE) preset.fastBeforeMs = config.fastBeforeMs;
  if (fields & PRESET_RECIPE_SLOW_ENABLED) preset.slowSideEnabled = config.slowSideEnabled;
  if (fields & PRESET_RECIPE_SLOW_MARGIN) preset.slowMarginDg = config.slowMarginDg;
  if (fields & PRESET_RECIPE_SLOW_AFTER) preset.slowAfterMs = config.slowAfterMs;
  if (fields & PRESET_RECIPE_SCALE_LOSS_FIXED) preset.scaleLossFixedMs = config.scaleLossFixedMs;
  if (fields & PRESET_RECIPE_FAST_RAMP) preset.fastRampMs = config.fastRampMs;
  if (fields & PRESET_RECIPE_FAST_CURVE) preset.fastCurve = config.fastCurve;
  if (fields & PRESET_RECIPE_FAST_CURVE_HARDNESS) {
    preset.fastCurveHardness = config.fastCurveHardness;
    preset.fastCurveHardnessPct = config.fastCurveHardnessPct;
  }
  if (fields & PRESET_RECIPE_SLOW_RAMP) preset.slowRampMs = config.slowRampMs;
  if (fields & PRESET_RECIPE_SLOW_CURVE) preset.slowCurve = config.slowCurve;
  if (fields & PRESET_RECIPE_SLOW_CURVE_HARDNESS) {
    preset.slowCurveHardness = config.slowCurveHardness;
    preset.slowCurveHardnessPct = config.slowCurveHardnessPct;
  }
}

inline bool allocateShotPresetId(ShotPresetBank &bank, uint8_t &outId) {
  for (uint16_t attempt = 0; attempt < 256; ++attempt) {
    const uint8_t candidate = bank.nextId == 0 ? 1 : bank.nextId;
    bank.nextId = static_cast<uint8_t>(candidate + 1);
    if (candidate == 0) {
      continue;
    }
    if (findShotPresetIndex(bank, candidate) < 0) {
      outId = candidate;
      return true;
    }
  }
  return false;
}

inline bool createUntitledShotPreset(ShotPresetBank &bank, uint8_t &outId) {
  if (bank.count >= MAX_SHOT_PRESETS) {
    return false;
  }
  uint8_t id = 0;
  if (!allocateShotPresetId(bank, id)) {
    return false;
  }
  ShotPreset &slot = bank.presets[bank.count];
  fillDoubleFirmwareDefaults(slot);
  slot.id = id;
  slot.isFactory = false;
  copyCString(slot.name, SHOT_PRESET_NAME_CAPACITY, "Untitled");
  if (shotPresetNameExists(bank, slot.name, 0)) {
    for (uint8_t n = 2; n < 100; ++n) {
      snprintf(slot.name, SHOT_PRESET_NAME_CAPACITY, "Untitled %u",
               static_cast<unsigned>(n));
      if (!shotPresetNameExists(bank, slot.name, 0)) {
        break;
      }
    }
  }
  ++bank.count;
  bank.activeId = id;
  outId = id;
  return true;
}

inline void stripCopySuffix(const char *name, char *base, size_t baseCap) {
  if (baseCap == 0) {
    return;
  }
  copyCString(base, baseCap, name != nullptr ? name : "");
  char *copyToken = strstr(base, " copy");
  if (copyToken == nullptr) {
    return;
  }
  const char *after = copyToken + 5;
  if (*after == '\0') {
    *copyToken = '\0';
    return;
  }
  if (*after == ' ') {
    ++after;
    bool digits = *after != '\0';
    for (const char *p = after; *p; ++p) {
      if (*p < '0' || *p > '9') {
        digits = false;
        break;
      }
    }
    if (digits) {
      *copyToken = '\0';
    }
  }
}

inline bool makeDuplicatePresetName(const ShotPresetBank &bank,
                                    const char *sourceName, char *outName,
                                    size_t outCap) {
  if (outCap == 0) {
    return false;
  }
  char base[SHOT_PRESET_NAME_CAPACITY];
  stripCopySuffix(sourceName, base, sizeof(base));
  if (base[0] == '\0') {
    copyCString(base, sizeof(base), "Preset");
  }
  // Longest suffix is " copy 99" (8) + NUL. Cap base so the composed name
  // always fits outCap without snprintf (-Wformat-truncation under -O2).
  constexpr size_t kSuffixRoom = 9;
  if (outCap <= kSuffixRoom) {
    return false;
  }
  const size_t maxBase = outCap - kSuffixRoom;
  if (strnlen(base, sizeof(base)) > maxBase) {
    base[maxBase] = '\0';
  }
  for (uint8_t n = 1; n < 100; ++n) {
    copyCString(outName, outCap, base);
    const size_t baseUsed = strnlen(outName, outCap);
    if (baseUsed >= outCap) {
      continue;
    }
    char *tail = outName + baseUsed;
    size_t tailCap = outCap - baseUsed;
    if (n == 1) {
      copyCString(tail, tailCap, " copy");
    } else {
      copyCString(tail, tailCap, " copy ");
      const size_t prefixUsed = strnlen(outName, outCap);
      if (prefixUsed >= outCap) {
        continue;
      }
      tail = outName + prefixUsed;
      tailCap = outCap - prefixUsed;
      if (n >= 10) {
        if (tailCap < 3) {
          continue;
        }
        tail[0] = static_cast<char>('0' + (n / 10));
        tail[1] = static_cast<char>('0' + (n % 10));
        tail[2] = '\0';
      } else {
        if (tailCap < 2) {
          continue;
        }
        tail[0] = static_cast<char>('0' + n);
        tail[1] = '\0';
      }
    }
    if (!shotPresetNameExists(bank, outName, 0)) {
      return true;
    }
  }
  return false;
}

inline bool duplicateShotPreset(ShotPresetBank &bank, uint8_t sourceId,
                                uint8_t &outId) {
  if (bank.count >= MAX_SHOT_PRESETS) {
    return false;
  }
  const ShotPreset *source = findShotPreset(bank, sourceId);
  if (source == nullptr) {
    return false;
  }
  const uint8_t savedNextId = bank.nextId;
  uint8_t id = 0;
  if (!allocateShotPresetId(bank, id)) {
    return false;
  }
  ShotPreset &slot = bank.presets[bank.count];
  slot = *source;
  slot.id = id;
  slot.isFactory = false;
  if (!makeDuplicatePresetName(bank, source->name, slot.name,
                               SHOT_PRESET_NAME_CAPACITY)) {
    slot = ShotPreset{};
    bank.nextId = savedNextId;
    return false;
  }
  ++bank.count;
  bank.activeId = id;
  outId = id;
  return true;
}

inline bool renameShotPreset(ShotPresetBank &bank, uint8_t id,
                             const char *newName) {
  if (!validShotPresetName(newName) ||
      shotPresetNameExists(bank, newName, id)) {
    return false;
  }
  ShotPreset *preset = mutableShotPreset(bank, id);
  if (preset == nullptr) {
    return false;
  }
  memset(preset->name, 0, sizeof(preset->name));
  copyCString(preset->name, SHOT_PRESET_NAME_CAPACITY, newName);
  return true;
}

inline bool deleteShotPreset(ShotPresetBank &bank, uint8_t id) {
  if (bank.count <= 1) {
    return false;
  }
  const int index = findShotPresetIndex(bank, id);
  if (index < 0) {
    return false;
  }
  // Factory presets are permanent slots — edit or restore only.
  if (bank.presets[index].isFactory || factoryShotPresetId(id)) {
    return false;
  }
  for (uint8_t i = static_cast<uint8_t>(index); i + 1 < bank.count; ++i) {
    bank.presets[i] = bank.presets[i + 1];
  }
  --bank.count;
  bank.presets[bank.count] = ShotPreset{};
  if (bank.activeId == id) {
    bank.activeId = bank.presets[0].id;
  }
  return true;
}

inline bool restoreFactoryShotPresetValues(ShotPresetBank &bank, uint8_t id) {
  ShotPreset *preset = mutableShotPreset(bank, id);
  if (preset == nullptr || !preset->isFactory || !factoryShotPresetId(id)) {
    return false;
  }
  char name[SHOT_PRESET_NAME_CAPACITY];
  memcpy(name, preset->name, sizeof(name));
  fillFactoryShotPreset(*preset, id);
  // Keep user-facing name.
  memcpy(preset->name, name, sizeof(preset->name));
  return true;
}

}  // namespace openbrewbyweight

// Preset recipe, included inside the domain namespace. Every member needs a
// row in OpenBrewByWeightSettingsCodec.h. The cut line is the target held
// through the shot time window [fastBeforeMs, slowAfterMs]; each side's margin
// (decigrams, relative to the target) applies only while its switch is on and
// is reached over that side's transition (length, curve type and hardness).
struct ShotPreset {
  uint8_t id = 0;
  char name[SHOT_PRESET_NAME_CAPACITY] = {};
  bool isFactory = false;
  bool brewByWeight = true;
  uint16_t goalWeightDg = DEFAULT_GOAL_WEIGHT_DG;
  uint16_t fastMarginDg = DEFAULT_FAST_MARGIN_DG;
  uint16_t slowMarginDg = DEFAULT_SLOW_MARGIN_DG;
  uint16_t doseDg = DEFAULT_DOSE_DG;  // Reference only; never affects the cut.
  uint32_t fastBeforeMs = DEFAULT_FAST_BEFORE_MS;
  uint32_t slowAfterMs = DEFAULT_SLOW_AFTER_MS;
  uint32_t bbwProtectionMs = DEFAULT_BBW_PROTECTION_MS;
  uint32_t operationalWallMs = DEFAULT_OPERATIONAL_WALL_MS;
  uint32_t scaleLossFixedMs = 0;  // 0 = use recent shots or the window.
  uint32_t learningEpoch = 0;     // First ShotLog record id of the statistics.
  float bbwEwmaOffsetG = DEFAULT_WEIGHT_OFFSET_G;
  float weightOffsetBaselineG = DEFAULT_WEIGHT_OFFSET_G;
  bool fastSideEnabled = true;
  bool slowSideEnabled = true;
  bool grindHintsEnabled = true;
  uint8_t bbwEwmaAlpha = DEFAULT_BBW_EWMA_ALPHA;
  uint8_t bbwAlphaLearned = 0;
  uint8_t bbwAlphaBaseline = DEFAULT_BBW_EWMA_ALPHA;  // Hundredths.
  // Deci-Celsius (80.0–100.0 °C), applied only by the Linea Micra integration.
  uint16_t lineaMicraBrewTargetDeciC = 930;
  uint16_t fastRampMs = DEFAULT_RAMP_MS;
  uint16_t slowRampMs = DEFAULT_RAMP_MS;
  uint8_t fastCurve = RAMP_CURVE_EVEN;
  uint8_t fastCurveHardness = RAMP_HARDNESS_SOFT;
  uint8_t slowCurve = RAMP_CURVE_EVEN;
  uint8_t slowCurveHardness = RAMP_HARDNESS_SOFT;
};

struct ShotPresetBank {
  uint8_t count = 0;
  uint8_t activeId = 0;
  uint8_t nextId = FACTORY_PRESET_COUNT + 1;
  ShotPreset presets[MAX_SHOT_PRESETS] = {};
};

#pragma once

#include "OpenBrewByWeightDomain.h"

// Validation-failure emission model for the parameterized API error rules:
// the mapping from ConfigValidationError to its stable rule code and
// pre-formatted params. The sentence itself lives in the err.* locale
// catalog and is resolved at emission from the generated table; the
// generator (scripts/generate_error_reason_texts.js) validates every row.

namespace openbrewbyweight {

// A validation failure to emit: the stable rule code (one per validation
// RULE; the sentence lives in the err.* locale catalog, docs/
// INTEGRATION_API.md) plus its pre-formatted params in template order.
struct ValidationFailure {
  const char *reason = nullptr;
  const char *params[3] = {};
};

inline ValidationFailure validationFailure(const char *reason,
                                           const char *p0 = nullptr,
                                           const char *p1 = nullptr,
                                           const char *p2 = nullptr) {
  return ValidationFailure{reason, {p0, p1, p2}};
}

// The INVALID_CONFIG failure for a rejected config: which rule failed and
// the params that reproduce the catalog sentence. The prose itself is
// resolved at emission from the generated catalog table.
inline ValidationFailure configValidationFailure(ConfigValidationError error) {
  switch (error) {
    case ConfigValidationError::NONE: return ValidationFailure{};
    case ConfigValidationError::GOAL_WEIGHT:
      return validationFailure("FIELD_RANGE_INT", "Target", "10", "200 g");
    case ConfigValidationError::WEIGHT_OFFSET:
      return validationFailure("CONFIG_WEIGHT_OFFSET_INTERNAL");
    case ConfigValidationError::RINSE_GESTURE:
      return validationFailure("FIELD_RANGE", "Rinse gesture", "0.1", "5 s");
    case ConfigValidationError::RINSE_DURATION:
      return validationFailure("FIELD_RANGE", "Rinse duration", "0.5", "10 s");
    case ConfigValidationError::RETARE_WINDOW:
      return validationFailure("FIELD_RANGE", "Retare window", "0.5", "10 s");
    case ConfigValidationError::MINIMUM_CUP_WEIGHT:
      return validationFailure("FIELD_RANGE", "Minimum cup weight", "1", "500 g");
    case ConfigValidationError::RETARE_STABILITY_SAMPLES:
      return validationFailure("FIELD_RANGE", "Retare stable samples", "2", "10");
    case ConfigValidationError::RETARE_STABILITY_TOLERANCE:
      return validationFailure("FIELD_RANGE", "Retare stability tolerance", "0.1", "20 g");
    case ConfigValidationError::RETARE_STABILITY_MAX_GAP:
      return validationFailure("FIELD_RANGE", "Retare sample gap", "0.1", "5 s");
    case ConfigValidationError::RETARE_STABILITY_MIN_DURATION:
      return validationFailure("FIELD_RANGE", "Retare min stable time", "0", "2 s");
    case ConfigValidationError::RETARE_STABILITY_RELATION:
      return validationFailure("CONFIG_RETARE_STABILITY_RELATION");
    case ConfigValidationError::BBW_PROTECTION_TIMEOUT:
      return validationFailure("FIELD_RANGE", "Earliest weight stop", "0.5", "30 s");
    case ConfigValidationError::BBW_PROTECTION_RETARE_RELATION:
      return validationFailure("CONFIG_BBW_RETARE_RELATION");
    case ConfigValidationError::OPERATIONAL_WALL:
      return validationFailure("FIELD_RANGE", "Max shot time", "5", "60 s");
    case ConfigValidationError::PADDLE_REMINDER_INTERVAL:
      return validationFailure("FIELD_RANGE", "Paddle reminder interval", "5", "60 s");
    case ConfigValidationError::PADDLE_REMINDER_MAX_DURATION:
      return validationFailure("CONFIG_PADDLE_REMINDER_RELATION");
    case ConfigValidationError::TIMING_RELATION:
      return validationFailure("CONFIG_TIMING_RELATION");
    case ConfigValidationError::COMBINED_TARE_REQUIRES_AUTOTARE:
      return validationFailure("CONFIG_COMBINED_TARE_REQUIRES_AUTOTARE");
    case ConfigValidationError::POST_TARE_BASELINE_GRACE:
      return validationFailure("FIELD_RANGE", "Post-tare grace", "0.5", "10 s");
    case ConfigValidationError::SCALE_TIMER_STOP_EXTRA_DELAY:
      return validationFailure("FIELD_RANGE", "Scale timer stop extra delay", "0", "1000 ms");
    case ConfigValidationError::TIMEZONE_ID:
      return validationFailure("TIMEZONE_ID_CHOOSE");
    case ConfigValidationError::NTP_SERVER_PRESET:
      return validationFailure("FIELD_ENUM", "NTP server preset",
                               "pool, google, cloudflare, or nist");
    case ConfigValidationError::NTP_SERVER_CUSTOM:
      return validationFailure("CONFIG_NTP_CUSTOM_INVALID");
    case ConfigValidationError::FAST_MARGIN:
      return validationFailure("CONFIG_FAST_MARGIN_RULES");
    case ConfigValidationError::FAST_BEFORE:
      return validationFailure("CONFIG_FAST_BEFORE_RULES");
    case ConfigValidationError::SLOW_MARGIN:
      return validationFailure("CONFIG_SLOW_MARGIN_RULES");
    case ConfigValidationError::SLOW_AFTER:
      return validationFailure("CONFIG_SLOW_AFTER_RULES");
    case ConfigValidationError::SCALE_LOSS_FIXED:
      return validationFailure("CONFIG_SCALE_LOSS_WINDOW");
    case ConfigValidationError::WEIGHT_OFFSET_BASELINE:
      return validationFailure("FIELD_RANGE", "Offset baseline", "0", "5.0 g");
    case ConfigValidationError::SCALE_MAC_CACHE_MODE:
      return validationFailure("FIELD_ENUM", "Scale preference",
                               "first, prefer, or only");
    case ConfigValidationError::ALERT_OUTPUT_CHANNEL:
      return validationFailure("FIELD_ENUM", "Alert output channel",
                               "scale_only, buzzer_only, or scale_priority");
    case ConfigValidationError::EXTENDED_PULSE_RATE:
      return validationFailure("FIELD_ENUM", "Fast-shot pulse",
                               "disabled, slow, medium, fast, or rapid");
    case ConfigValidationError::SLOW_EXTENDED_PULSE_RATE:
      return validationFailure("FIELD_ENUM", "Slow-shot pulse",
                               "disabled, slow, medium, fast, or rapid");
    case ConfigValidationError::BOOKOO_CONNECT_BEEP_LEVEL:
      return validationFailure("CONFIG_BOOKOO_VOLUME_RANGE");
    case ConfigValidationError::NO_SCALE_BBW_MODE:
      return validationFailure("FIELD_ENUM", "No-scale BBW mode",
                               "off, warn_once, or require_scale");
    case ConfigValidationError::LAST_SHOT_COOLDOWN:
      return validationFailure("FIELD_RANGE", "Last shot cooldown", "5", "240 min");
    case ConfigValidationError::DRIP_DELAY:
      return validationFailure("FIELD_RANGE", "Drip delay", "0", "10 s");
    case ConfigValidationError::SERIAL_LOG_LEVEL:
      return validationFailure("FIELD_ENUM", "Serial log level",
                               "none, critical, error, warning, info, or debug");
    case ConfigValidationError::STOP_PULSE:
      return validationFailure("FIELD_RANGE", "Auto-stop pulse", "50", "1000 ms");
    case ConfigValidationError::MAX_SINGLE_PRESS:
      return validationFailure("FIELD_RANGE", "Single-press limit", "100", "5000 ms");
    case ConfigValidationError::REED_CONFIRM_TIMEOUT:
      return validationFailure("FIELD_RANGE", "Reed confirm timeout", "0.2", "5 s");
    case ConfigValidationError::SHOT_REACT_TIMEOUT:
      return validationFailure("CONFIG_SHOT_REACT_TIMEOUT_RANGE");
    case ConfigValidationError::SCHEDULED_RESTART:
      return validationFailure("FIELD_RANGE", "Scheduled restart time", "00:00", "23:59");
    case ConfigValidationError::FAST_RAMP:
      return validationFailure("CONFIG_FAST_RAMP_STEPS");
    case ConfigValidationError::FAST_CURVE:
      return validationFailure("FIELD_ENUM", "Fast transition curve",
                               "early, even or late, soft, medium or hard");
    case ConfigValidationError::RING_RETAIN_LOG_LEVEL:
      return validationFailure("FIELD_ENUM", "Ring log level",
                               "none, critical, error, warning, info, or debug");
    case ConfigValidationError::SLOW_RAMP:
      return validationFailure("CONFIG_SLOW_RAMP_STEPS");
    case ConfigValidationError::SLOW_CURVE:
      return validationFailure("FIELD_ENUM", "Slow transition curve",
                               "early, even or late, soft, medium or hard");
    case ConfigValidationError::CUP_REMOVED_WEIGHT:
      return validationFailure("FIELD_RANGE", "Cup-removed threshold", "-50", "-0.1 g");
    case ConfigValidationError::PADDLE_MODE:
      return validationFailure("FIELD_ENUM", "Paddle mode",
                               "auto, natural or original");
  }
  return validationFailure("CONFIG_INVALID");
}

}  // namespace openbrewbyweight

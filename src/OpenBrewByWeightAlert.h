#pragma once

#include <stdint.h>

#include "OpenBrewByWeightDomain.h"

namespace openbrewbyweight {

enum class AlertEvent : uint8_t {
  TARE = 0,
  START_TIMER,
  STOP_TIMER,
  TARE_START,
  FIRST_DROP,
  PADDLE_REMINDER,
  COMPLETION_EXTRA,
  SCALE_LOST,
  ATM_END,
  MANUAL_NO_SCALE,
  CUP_START_BLOCKED,
  EXTENDED_PULSE,
  SCALE_CONNECTED,
  BACKFLUSH_READY
};

enum class AlertKind : uint8_t {
  Independent = 0,
  CommandImmediate,
  CommandFallback,
  Recovery
};

enum class AlertSink : uint8_t { None = 0, Scale, Buzzer };

struct AlertChannelContext {
  AlertOutputChannel channel = AlertOutputChannel::SCALE_ONLY;
  bool soundAlertsEnabled = true;
  bool buzzerSupportEnabled = false;
  bool buzzerReady = false;
  bool scaleAvailable = false;
  bool scaleSupportsIndependentBeep = false;
  bool scaleSupportsCommandFeedback = false;
  bool commandFeedbackExpected = false;
  bool commandAttempted = false;
  bool writeSucceeded = false;
};

// -1 leaves the scale's speaker untouched; 0 explicitly silences it.
inline int8_t bookooDesiredVolume(bool alertsEnabled,
                                  AlertOutputChannel channel,
                                  bool muteOnBuzzerOnly, uint8_t level) {
  if (!alertsEnabled) return 0;
  if (channel == AlertOutputChannel::BUZZER_ONLY)
    return muteOnBuzzerOnly ? 0 : -1;
  return level <= BOOKOO_BEEP_LEVEL_MAX ? static_cast<int8_t>(level) : int8_t{-1};
}

inline bool alertEventScaleCapable(AlertEvent event) {
  switch (event) {
    case AlertEvent::SCALE_LOST:
    case AlertEvent::ATM_END:
    case AlertEvent::MANUAL_NO_SCALE:
    case AlertEvent::CUP_START_BLOCKED:
    case AlertEvent::EXTENDED_PULSE:
    case AlertEvent::SCALE_CONNECTED:
    case AlertEvent::BACKFLUSH_READY:
      return false;
    default:
      return true;
  }
}

inline bool alertEventQueuesScaleBeep(AlertEvent event) {
  return event == AlertEvent::FIRST_DROP ||
         event == AlertEvent::PADDLE_REMINDER ||
         event == AlertEvent::COMPLETION_EXTRA;
}

}  // namespace openbrewbyweight

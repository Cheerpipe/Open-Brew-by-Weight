// Flash persistence owns this flag; HTTP/control only observe verified saves.

inline std::atomic<bool> &durableTimezoneSaved() {
  static std::atomic<bool> saved{false};
  return saved;
}

inline bool validTimeZoneState(const RuntimeConfig &config) {
  const size_t length = strnlen(config.timezoneId, TIMEZONE_ID_CAPACITY);
  if (length >= TIMEZONE_ID_CAPACITY ||
      strspn(config.timezoneId,
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_/-+") !=
          length)
    return false;
  if (config.timezoneSource > 4 ||
      (length == 0) != (config.timezoneSource == 0) ||
      config.firstTimezoneAutoClockQuality > 3)
    return false;
  if (config.firstTimezoneAutoClockQuality == 0 &&
      (config.firstTimezoneAutoUtcSec != 0 ||
       config.firstTimezoneAutoUptimeUs != 0 ||
       config.firstTimezoneAutoBootId != 0))
    return false;
  return config.firstTimezoneAutoClockQuality < 2 ||
         config.firstTimezoneAutoUtcSec != 0;
}

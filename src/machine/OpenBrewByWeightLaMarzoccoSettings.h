#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace openbrewbyweight {

constexpr size_t LA_MARZOCCO_USERNAME_CAPACITY = 97;
constexpr size_t LA_MARZOCCO_PASSWORD_CAPACITY = 97;
constexpr size_t LA_MARZOCCO_SERIAL_CAPACITY = 33;
constexpr size_t LA_MARZOCCO_NAME_CAPACITY = 49;
constexpr size_t LA_MARZOCCO_PRIVATE_KEY_BYTES = 32;
constexpr uint8_t LA_MARZOCCO_APPLY_TEMPERATURE = 1U << 0;
constexpr uint8_t LA_MARZOCCO_OBSERVE_STATE = 1U << 1;
constexpr uint8_t LA_MARZOCCO_RECOGNIZE_WAKE = 1U << 2;
constexpr uint8_t LA_MARZOCCO_SHUTDOWN_WITH_SCALE = 1U << 3;
constexpr uint8_t LA_MARZOCCO_POWER_ON_WITH_SCALE = 1U << 7;
// Grace delay for the scale-triggered shutdown, packed into spare option bits
// so the option byte stays fully allocated. Codes 0..4 map to seconds below.
constexpr uint8_t LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT = 4;
constexpr uint8_t LA_MARZOCCO_SHUTDOWN_GRACE_MASK = 0x70U;
constexpr uint8_t LA_MARZOCCO_SHUTDOWN_GRACE_CODE_MAX = 4;
constexpr uint8_t LA_MARZOCCO_KNOWN_OPTIONS =
    LA_MARZOCCO_APPLY_TEMPERATURE | LA_MARZOCCO_OBSERVE_STATE |
    LA_MARZOCCO_RECOGNIZE_WAKE | LA_MARZOCCO_SHUTDOWN_WITH_SCALE |
    LA_MARZOCCO_POWER_ON_WITH_SCALE | LA_MARZOCCO_SHUTDOWN_GRACE_MASK;
// Remote power-on stays off by default; scale-off standby waits 15 s (code 2).
constexpr uint8_t LA_MARZOCCO_DEFAULT_OPTIONS =
    LA_MARZOCCO_APPLY_TEMPERATURE | LA_MARZOCCO_OBSERVE_STATE |
    LA_MARZOCCO_RECOGNIZE_WAKE | LA_MARZOCCO_SHUTDOWN_WITH_SCALE |
    (2U << LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT);
// Scale-directed machine-link options live in their own byte so the option
// layout above stays stable. Bit 0 powers the scale off when the machine is
// confirmed off; bits 1-2 select scale-discovery duty from machine power;
// the remaining bits are reserved and must stay zero.
constexpr uint8_t LA_MARZOCCO_SCALE_OFF_WITH_MACHINE = 1U << 0;
constexpr uint8_t LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON = 1U << 1;
constexpr uint8_t LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF = 1U << 2;
constexpr uint8_t LA_MARZOCCO_KNOWN_SCALE_OPTIONS =
    LA_MARZOCCO_SCALE_OFF_WITH_MACHINE |
    LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON |
    LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF;
constexpr uint16_t LA_MARZOCCO_SHUTDOWN_GRACE_SECONDS[] = {0, 5, 15, 30, 60};
// A brew target's absolute bounds: whatever range a machine reports, a
// stored preset, an observed target and a sent command stay inside them.
constexpr uint16_t LA_MARZOCCO_BREW_TARGET_MIN_DECI_C = 600;
constexpr uint16_t LA_MARZOCCO_BREW_TARGET_MAX_DECI_C = 1100;
// The range of a machine whose cloud report names none.
constexpr uint16_t LA_MARZOCCO_BREW_RANGE_DEFAULT_MIN_DECI_C = 800;
constexpr uint16_t LA_MARZOCCO_BREW_RANGE_DEFAULT_MAX_DECI_C = 1000;
enum class LaMarzoccoConnectionType : uint8_t { WEBSOCKET = 0, API = 1 };
inline const char *laMarzoccoConnectionTypeName(uint8_t value) {
  return value == static_cast<uint8_t>(LaMarzoccoConnectionType::API) ? "api" : "websocket";
}

// Cloud account record. Access and refresh tokens are deliberately absent:
// they are short-lived worker state and are never written to flash.
struct LaMarzoccoPersistedSettings {
  char username[LA_MARZOCCO_USERNAME_CAPACITY] = {};
  char password[LA_MARZOCCO_PASSWORD_CAPACITY] = {};
  uint8_t installationPrivateKey[LA_MARZOCCO_PRIVATE_KEY_BYTES] = {};
  char selectedSerial[LA_MARZOCCO_SERIAL_CAPACITY] = {};
  char selectedName[LA_MARZOCCO_NAME_CAPACITY] = {};
  uint8_t options = LA_MARZOCCO_DEFAULT_OPTIONS;
  uint8_t scaleOptions = LA_MARZOCCO_KNOWN_SCALE_OPTIONS;
  bool accountConfigured = false;
  uint8_t connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::WEBSOCKET);
};

inline bool laMarzoccoBoundedText(const char *value, size_t capacity,
                                  bool allowEmpty) {
  if (value == nullptr || capacity < 2) return false;
  const size_t length = strnlen(value, capacity);
  if (length >= capacity || (!allowEmpty && length == 0)) return false;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t byte = static_cast<uint8_t>(value[i]);
    if (byte < 0x20 || byte == 0x7f) return false;
  }
  return true;
}

inline bool laMarzoccoPrivateKeyConfigured(
    const LaMarzoccoPersistedSettings &settings) {
  return std::any_of(std::begin(settings.installationPrivateKey),
                     std::end(settings.installationPrivateKey),
                     [](uint8_t byte) { return byte != 0; });
}

inline uint8_t laMarzoccoShutdownGraceCode(uint8_t options) {
  return (options & LA_MARZOCCO_SHUTDOWN_GRACE_MASK) >>
         LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT;
}

inline uint16_t laMarzoccoShutdownGraceSeconds(uint8_t options) {
  const uint8_t code = laMarzoccoShutdownGraceCode(options);
  return code <= LA_MARZOCCO_SHUTDOWN_GRACE_CODE_MAX
             ? LA_MARZOCCO_SHUTDOWN_GRACE_SECONDS[code]
             : 0;
}

inline bool validLaMarzoccoShutdownGrace(uint8_t options) {
  return laMarzoccoShutdownGraceCode(options) <=
         LA_MARZOCCO_SHUTDOWN_GRACE_CODE_MAX;
}

inline bool validLaMarzoccoSerial(const char *serial) {
  if (!laMarzoccoBoundedText(serial, LA_MARZOCCO_SERIAL_CAPACITY, false)) {
    return false;
  }
  for (size_t i = 0; serial[i] != '\0'; ++i) {
    const char value = serial[i];
    if (!((value >= 'A' && value <= 'Z') ||
          (value >= 'a' && value <= 'z') ||
          (value >= '0' && value <= '9') || value == '-' || value == '_')) {
      return false;
    }
  }
  return true;
}

inline bool validLaMarzoccoSettings(
    const LaMarzoccoPersistedSettings &settings) {
  if ((settings.options & ~LA_MARZOCCO_KNOWN_OPTIONS) != 0 ||
      (settings.scaleOptions & ~LA_MARZOCCO_KNOWN_SCALE_OPTIONS) != 0 ||
      settings.connectionType > static_cast<uint8_t>(LaMarzoccoConnectionType::API) ||
      !validLaMarzoccoShutdownGrace(settings.options)) {
    return false;
  }
  if (!settings.accountConfigured) {
    return settings.username[0] == '\0' && settings.password[0] == '\0' &&
           settings.selectedSerial[0] == '\0' &&
           settings.selectedName[0] == '\0' &&
           !laMarzoccoPrivateKeyConfigured(settings);
  }
  if (!laMarzoccoBoundedText(settings.username, sizeof(settings.username), false)) return false;
  const char *at = strchr(settings.username, '@');
  return at != nullptr && at != settings.username && at[1] != '\0' &&
         laMarzoccoBoundedText(settings.password, sizeof(settings.password),
                               false) &&
         laMarzoccoPrivateKeyConfigured(settings) &&
         validLaMarzoccoSerial(settings.selectedSerial) &&
         laMarzoccoBoundedText(settings.selectedName,
                               sizeof(settings.selectedName), true);
}

// The account La Marzocco last refused, kept so a restart does not sign in
// with it again: an FNV-1a of the username and password (never 0), stored in
// NVS key LA_MARZOCCO_REJECTED_KEY of the settings namespace (0 = none).
constexpr const char *LA_MARZOCCO_REJECTED_KEY = "micraRejected";

static_assert(LA_MARZOCCO_USERNAME_CAPACITY == LA_MARZOCCO_PASSWORD_CAPACITY,
              "laMarzoccoAccountKey bounds both fields alike");
inline uint32_t laMarzoccoAccountKey(const LaMarzoccoPersistedSettings &settings) {
  uint32_t hash = 2166136261U;
  for (const char *text : {settings.username, settings.password}) {
    const size_t length = strnlen(text, LA_MARZOCCO_PASSWORD_CAPACITY - 1);
    for (size_t index = 0; index <= length; ++index) {
      hash ^= static_cast<uint8_t>(text[index]);
      hash *= 16777619U;
    }
  }
  return hash | 1U;
}

inline void wipeLaMarzoccoSettings(LaMarzoccoPersistedSettings &settings) {
  volatile uint8_t *bytes = reinterpret_cast<volatile uint8_t *>(&settings);
  for (size_t index = 0; index < sizeof(settings); ++index) bytes[index] = 0;
}

inline void disconnectLaMarzocco(LaMarzoccoPersistedSettings &settings) {
  const uint8_t options = settings.options;
  const uint8_t scaleOptions = settings.scaleOptions;
  const uint8_t connectionType = settings.connectionType;
  wipeLaMarzoccoSettings(settings);
  settings.options = options;
  settings.scaleOptions = scaleOptions;
  settings.connectionType = connectionType;
}

inline void setLaMarzoccoOptions(LaMarzoccoPersistedSettings &settings,
                                 bool applyTemperature, bool observeState,
                                 bool recognizeWake, bool shutdownWithScale,
                                 bool powerOnWithScale,
                                 uint8_t shutdownGraceCode,
                                 bool scaleOffWithMachine,
                                 bool boostScaleDetectionWhenOn,
                                 bool reduceScaleScanningWhenOff) {
  if (shutdownGraceCode > LA_MARZOCCO_SHUTDOWN_GRACE_CODE_MAX) {
    shutdownGraceCode = 0;
  }
  settings.options =
      (applyTemperature ? LA_MARZOCCO_APPLY_TEMPERATURE : 0U) |
      (observeState ? LA_MARZOCCO_OBSERVE_STATE : 0U) |
      (recognizeWake ? LA_MARZOCCO_RECOGNIZE_WAKE : 0U) |
      (shutdownWithScale ? LA_MARZOCCO_SHUTDOWN_WITH_SCALE : 0U) |
      (powerOnWithScale ? LA_MARZOCCO_POWER_ON_WITH_SCALE : 0U) |
      static_cast<uint8_t>(static_cast<uint8_t>(shutdownGraceCode)
                           << LA_MARZOCCO_SHUTDOWN_GRACE_SHIFT);
  settings.scaleOptions =
      (scaleOffWithMachine ? LA_MARZOCCO_SCALE_OFF_WITH_MACHINE : 0U) |
      (boostScaleDetectionWhenOn
           ? LA_MARZOCCO_SCALE_SCAN_BOOST_WHEN_ON : 0U) |
      (reduceScaleScanningWhenOff
           ? LA_MARZOCCO_SCALE_SCAN_RELAX_WHEN_OFF : 0U);
}

static_assert(offsetof(LaMarzoccoPersistedSettings, options) == 308);
static_assert(offsetof(LaMarzoccoPersistedSettings, scaleOptions) == 309);
static_assert(offsetof(LaMarzoccoPersistedSettings, accountConfigured) == 310);
static_assert(offsetof(LaMarzoccoPersistedSettings, connectionType) == 311);
static_assert(sizeof(LaMarzoccoPersistedSettings) == 312,
              "La Marzocco cloud settings ABI changed");

}  // namespace openbrewbyweight

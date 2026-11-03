#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace openbrewbyweight {

constexpr size_t LINEA_MICRA_USERNAME_CAPACITY = 97;
constexpr size_t LINEA_MICRA_PASSWORD_CAPACITY = 97;
constexpr size_t LINEA_MICRA_SERIAL_CAPACITY = 33;
constexpr size_t LINEA_MICRA_NAME_CAPACITY = 49;
constexpr size_t LINEA_MICRA_PRIVATE_KEY_BYTES = 32;
constexpr uint8_t LINEA_MICRA_APPLY_TEMPERATURE = 1U << 0;
constexpr uint8_t LINEA_MICRA_OBSERVE_STATE = 1U << 1;
constexpr uint8_t LINEA_MICRA_RECOGNIZE_WAKE = 1U << 2;
constexpr uint8_t LINEA_MICRA_SHUTDOWN_WITH_SCALE = 1U << 3;
constexpr uint8_t LINEA_MICRA_POWER_ON_WITH_SCALE = 1U << 7;
// Grace delay for the scale-triggered shutdown, packed into spare option bits
// so the option byte stays fully allocated. Codes 0..4 map to seconds below.
constexpr uint8_t LINEA_MICRA_SHUTDOWN_GRACE_SHIFT = 4;
constexpr uint8_t LINEA_MICRA_SHUTDOWN_GRACE_MASK = 0x70U;
constexpr uint8_t LINEA_MICRA_SHUTDOWN_GRACE_CODE_MAX = 4;
constexpr uint8_t LINEA_MICRA_KNOWN_OPTIONS =
    LINEA_MICRA_APPLY_TEMPERATURE | LINEA_MICRA_OBSERVE_STATE |
    LINEA_MICRA_RECOGNIZE_WAKE | LINEA_MICRA_SHUTDOWN_WITH_SCALE |
    LINEA_MICRA_POWER_ON_WITH_SCALE | LINEA_MICRA_SHUTDOWN_GRACE_MASK;
// Remote power-on stays off by default; scale-off standby waits 15 s (code 2).
constexpr uint8_t LINEA_MICRA_DEFAULT_OPTIONS =
    LINEA_MICRA_APPLY_TEMPERATURE | LINEA_MICRA_OBSERVE_STATE |
    LINEA_MICRA_RECOGNIZE_WAKE | LINEA_MICRA_SHUTDOWN_WITH_SCALE |
    (2U << LINEA_MICRA_SHUTDOWN_GRACE_SHIFT);
// Scale-directed machine-link options live in their own byte so the option
// layout above stays stable. Bit 0 powers the scale off when the machine is
// confirmed off; bits 1-2 select scale-discovery duty from machine power;
// the remaining bits are reserved and must stay zero.
constexpr uint8_t LINEA_MICRA_SCALE_OFF_WITH_MACHINE = 1U << 0;
constexpr uint8_t LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON = 1U << 1;
constexpr uint8_t LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF = 1U << 2;
constexpr uint8_t LINEA_MICRA_KNOWN_SCALE_OPTIONS =
    LINEA_MICRA_SCALE_OFF_WITH_MACHINE |
    LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON |
    LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF;
constexpr uint16_t LINEA_MICRA_SHUTDOWN_GRACE_SECONDS[] = {0, 5, 15, 30, 60};
constexpr uint16_t LINEA_MICRA_BREW_TARGET_MIN_DECI_C = 800;
constexpr uint16_t LINEA_MICRA_BREW_TARGET_MAX_DECI_C = 1000;
enum class MicraConnectionType : uint8_t { WEBSOCKET = 0, API = 1 };
inline const char *micraConnectionTypeName(uint8_t value) {
  return value == static_cast<uint8_t>(MicraConnectionType::API) ? "api" : "websocket";
}

// Cloud account record. Access and refresh tokens are deliberately absent:
// they are short-lived worker state and are never written to flash.
struct LineaMicraPersistedSettings {
  char username[LINEA_MICRA_USERNAME_CAPACITY] = {};
  char password[LINEA_MICRA_PASSWORD_CAPACITY] = {};
  uint8_t installationPrivateKey[LINEA_MICRA_PRIVATE_KEY_BYTES] = {};
  char selectedSerial[LINEA_MICRA_SERIAL_CAPACITY] = {};
  char selectedName[LINEA_MICRA_NAME_CAPACITY] = {};
  uint8_t options = LINEA_MICRA_DEFAULT_OPTIONS;
  uint8_t scaleOptions = LINEA_MICRA_KNOWN_SCALE_OPTIONS;
  bool accountConfigured = false;
  uint8_t connectionType = static_cast<uint8_t>(MicraConnectionType::WEBSOCKET);
};

inline bool lineaMicraBoundedText(const char *value, size_t capacity,
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

inline bool lineaMicraPrivateKeyConfigured(
    const LineaMicraPersistedSettings &settings) {
  return std::any_of(std::begin(settings.installationPrivateKey),
                     std::end(settings.installationPrivateKey),
                     [](uint8_t byte) { return byte != 0; });
}

inline uint8_t lineaMicraShutdownGraceCode(uint8_t options) {
  return (options & LINEA_MICRA_SHUTDOWN_GRACE_MASK) >>
         LINEA_MICRA_SHUTDOWN_GRACE_SHIFT;
}

inline uint16_t lineaMicraShutdownGraceSeconds(uint8_t options) {
  const uint8_t code = lineaMicraShutdownGraceCode(options);
  return code <= LINEA_MICRA_SHUTDOWN_GRACE_CODE_MAX
             ? LINEA_MICRA_SHUTDOWN_GRACE_SECONDS[code]
             : 0;
}

inline bool validLineaMicraShutdownGrace(uint8_t options) {
  return lineaMicraShutdownGraceCode(options) <=
         LINEA_MICRA_SHUTDOWN_GRACE_CODE_MAX;
}

inline bool validLineaMicraSerial(const char *serial) {
  if (!lineaMicraBoundedText(serial, LINEA_MICRA_SERIAL_CAPACITY, false)) {
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

inline bool validLineaMicraSettings(
    const LineaMicraPersistedSettings &settings) {
  if ((settings.options & ~LINEA_MICRA_KNOWN_OPTIONS) != 0 ||
      (settings.scaleOptions & ~LINEA_MICRA_KNOWN_SCALE_OPTIONS) != 0 ||
      settings.connectionType > static_cast<uint8_t>(MicraConnectionType::API) ||
      !validLineaMicraShutdownGrace(settings.options)) {
    return false;
  }
  if (!settings.accountConfigured) {
    return settings.username[0] == '\0' && settings.password[0] == '\0' &&
           settings.selectedSerial[0] == '\0' &&
           settings.selectedName[0] == '\0' &&
           !lineaMicraPrivateKeyConfigured(settings);
  }
  if (!lineaMicraBoundedText(settings.username, sizeof(settings.username), false)) return false;
  const char *at = strchr(settings.username, '@');
  return at != nullptr && at != settings.username && at[1] != '\0' &&
         lineaMicraBoundedText(settings.password, sizeof(settings.password),
                               false) &&
         lineaMicraPrivateKeyConfigured(settings) &&
         validLineaMicraSerial(settings.selectedSerial) &&
         lineaMicraBoundedText(settings.selectedName,
                               sizeof(settings.selectedName), true);
}

inline void wipeLineaMicraSettings(LineaMicraPersistedSettings &settings) {
  volatile uint8_t *bytes = reinterpret_cast<volatile uint8_t *>(&settings);
  for (size_t index = 0; index < sizeof(settings); ++index) bytes[index] = 0;
}

inline void disconnectLineaMicra(LineaMicraPersistedSettings &settings) {
  const uint8_t options = settings.options;
  const uint8_t scaleOptions = settings.scaleOptions;
  const uint8_t connectionType = settings.connectionType;
  wipeLineaMicraSettings(settings);
  settings.options = options;
  settings.scaleOptions = scaleOptions;
  settings.connectionType = connectionType;
}

inline void setLineaMicraOptions(LineaMicraPersistedSettings &settings,
                                 bool applyTemperature, bool observeState,
                                 bool recognizeWake, bool shutdownWithScale,
                                 bool powerOnWithScale,
                                 uint8_t shutdownGraceCode,
                                 bool scaleOffWithMachine,
                                 bool boostScaleDetectionWhenOn,
                                 bool reduceScaleScanningWhenOff) {
  if (shutdownGraceCode > LINEA_MICRA_SHUTDOWN_GRACE_CODE_MAX) {
    shutdownGraceCode = 0;
  }
  settings.options =
      (applyTemperature ? LINEA_MICRA_APPLY_TEMPERATURE : 0U) |
      (observeState ? LINEA_MICRA_OBSERVE_STATE : 0U) |
      (recognizeWake ? LINEA_MICRA_RECOGNIZE_WAKE : 0U) |
      (shutdownWithScale ? LINEA_MICRA_SHUTDOWN_WITH_SCALE : 0U) |
      (powerOnWithScale ? LINEA_MICRA_POWER_ON_WITH_SCALE : 0U) |
      static_cast<uint8_t>(static_cast<uint8_t>(shutdownGraceCode)
                           << LINEA_MICRA_SHUTDOWN_GRACE_SHIFT);
  settings.scaleOptions =
      (scaleOffWithMachine ? LINEA_MICRA_SCALE_OFF_WITH_MACHINE : 0U) |
      (boostScaleDetectionWhenOn
           ? LINEA_MICRA_SCALE_SCAN_BOOST_WHEN_ON : 0U) |
      (reduceScaleScanningWhenOff
           ? LINEA_MICRA_SCALE_SCAN_RELAX_WHEN_OFF : 0U);
}

static_assert(offsetof(LineaMicraPersistedSettings, options) == 308);
static_assert(offsetof(LineaMicraPersistedSettings, scaleOptions) == 309);
static_assert(offsetof(LineaMicraPersistedSettings, accountConfigured) == 310);
static_assert(offsetof(LineaMicraPersistedSettings, connectionType) == 311);
static_assert(sizeof(LineaMicraPersistedSettings) == 312,
              "Linea Micra cloud settings ABI changed");

}  // namespace openbrewbyweight

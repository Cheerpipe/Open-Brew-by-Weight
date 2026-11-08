#pragma once

#include <stdint.h>

#include "OpenBrewByWeightDomain.h"

namespace openbrewbyweight {

// Board hardware — ESP32-S3 only. Named profiles resolve assembled variants;
// the generated profile header overrides the defaults below.
#if !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#if !defined(ARDUINO_ESP32S3_DEV)
#error "Unsupported board: Open Brew by Weight requires ESP32-S3 (esp32:esp32:esp32s3)"
#endif
#if !defined(BOARD_HAS_PSRAM)
#error "Open Brew by Weight requires PSRAM. Compile n16r8 (OPI 8MB)"
#endif
#endif

#if defined(ARDUINO_ESP32S3_DEV)
#ifndef OPEN_BREW_BY_WEIGHT_ACTIVATOR_GPIO
#define OPEN_BREW_BY_WEIGHT_ACTIVATOR_GPIO 21
#endif
#ifndef OPEN_BREW_BY_WEIGHT_RELAY_GPIO
#define OPEN_BREW_BY_WEIGHT_RELAY_GPIO 2
#endif
#ifndef OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_GPIO
#define OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_GPIO 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_BUZZER_GPIO
#define OPEN_BREW_BY_WEIGHT_BUZZER_GPIO 14
#endif
#ifndef OPEN_BREW_BY_WEIGHT_BUZZER_ACTIVE_LEVEL
#define OPEN_BREW_BY_WEIGHT_BUZZER_ACTIVE_LEVEL LOW
#endif
#ifndef OPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO
#define OPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO 4
#endif
#ifndef OPEN_BREW_BY_WEIGHT_ACTIVATOR_ACTIVE_LEVEL
#define OPEN_BREW_BY_WEIGHT_ACTIVATOR_ACTIVE_LEVEL LOW
#endif
#ifndef OPEN_BREW_BY_WEIGHT_RELAY_CLOSED_LEVEL
#define OPEN_BREW_BY_WEIGHT_RELAY_CLOSED_LEVEL HIGH
#endif
#ifndef OPEN_BREW_BY_WEIGHT_RELAY_OPEN_LEVEL
#define OPEN_BREW_BY_WEIGHT_RELAY_OPEN_LEVEL LOW
#endif
#ifndef OPEN_BREW_BY_WEIGHT_ACTIVATOR_DEBOUNCE_MS
#define OPEN_BREW_BY_WEIGHT_ACTIVATOR_DEBOUNCE_MS 30
#endif
#ifndef OPEN_BREW_BY_WEIGHT_SCALE_STATUS_LED_PRESENT
#define OPEN_BREW_BY_WEIGHT_SCALE_STATUS_LED_PRESENT 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_SPEAKER_PRESENT
#define OPEN_BREW_BY_WEIGHT_SPEAKER_PRESENT 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_USB_CONSOLE_JUMPER_PRESENT
#define OPEN_BREW_BY_WEIGHT_USB_CONSOLE_JUMPER_PRESENT 1
#endif
#ifndef OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_ACTIVE_LEVEL
#define OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_ACTIVE_LEVEL HIGH
#endif
#ifndef OPEN_BREW_BY_WEIGHT_USB_CONSOLE_ACTIVE_LEVEL
#define OPEN_BREW_BY_WEIGHT_USB_CONSOLE_ACTIVE_LEVEL LOW
#endif
#ifndef OPEN_BREW_BY_WEIGHT_REED_PRESENT
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
#define OPEN_BREW_BY_WEIGHT_REED_PRESENT 1
#else
#define OPEN_BREW_BY_WEIGHT_REED_PRESENT 0
#endif
#endif
#else
#error "Unsupported board: Open Brew by Weight requires ESP32-S3"
#endif

constexpr uint8_t ACTIVATOR_GPIO = OPEN_BREW_BY_WEIGHT_ACTIVATOR_GPIO;
constexpr uint8_t RELAY_GPIO = OPEN_BREW_BY_WEIGHT_RELAY_GPIO;
constexpr bool SCALE_STATUS_LED_PRESENT =
    OPEN_BREW_BY_WEIGHT_SCALE_STATUS_LED_PRESENT == 1;
constexpr bool SPEAKER_PRESENT = OPEN_BREW_BY_WEIGHT_SPEAKER_PRESENT == 1;
constexpr bool USB_CONSOLE_JUMPER_PRESENT =
    OPEN_BREW_BY_WEIGHT_USB_CONSOLE_JUMPER_PRESENT == 1;
constexpr bool REED_PRESENT = OPEN_BREW_BY_WEIGHT_REED_PRESENT == 1;
constexpr uint8_t SCALE_CONNECTED_LED_GPIO =
    OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_GPIO;
constexpr uint8_t SCALE_CONNECTED_LED_ACTIVE_LEVEL =
    OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_ACTIVE_LEVEL;
constexpr uint8_t SCALE_CONNECTED_LED_INACTIVE_LEVEL =
    SCALE_CONNECTED_LED_ACTIVE_LEVEL == HIGH ? LOW : HIGH;
// Fast blink while GATT connecting (5 Hz); slow blink while weight is stale (1 Hz).
constexpr uint32_t SCALE_LED_FAST_BLINK_MS = 100;
constexpr uint32_t SCALE_LED_SLOW_BLINK_MS = 500;
constexpr uint8_t BUZZER_GPIO = OPEN_BREW_BY_WEIGHT_BUZZER_GPIO;
// Input level that drives the transducer: LOW for PNP (S8550) modules,
// HIGH for NPN (S8050) modules and bare piezos. Silence holds the other level.
constexpr uint8_t BUZZER_ACTIVE_LEVEL = OPEN_BREW_BY_WEIGHT_BUZZER_ACTIVE_LEVEL;
constexpr uint8_t USB_CONSOLE_GPIO = OPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO;
// Dupont IO4 to a GND pad (never the EN column). Sampled once at boot.
constexpr uint8_t USB_CONSOLE_ACTIVE_LEVEL =
    OPEN_BREW_BY_WEIGHT_USB_CONSOLE_ACTIVE_LEVEL;

constexpr uint8_t ACTIVATOR_ACTIVE_LEVEL =
    OPEN_BREW_BY_WEIGHT_ACTIVATOR_ACTIVE_LEVEL;
// Active-HIGH relay: GPIO HIGH energizes the coil and closes NO.
constexpr uint8_t RELAY_CLOSED_LEVEL = OPEN_BREW_BY_WEIGHT_RELAY_CLOSED_LEVEL;
constexpr uint8_t RELAY_OPEN_LEVEL = OPEN_BREW_BY_WEIGHT_RELAY_OPEN_LEVEL;

#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
#ifndef OPEN_BREW_BY_WEIGHT_REED_GPIO
#define OPEN_BREW_BY_WEIGHT_REED_GPIO 13
#endif
#ifndef OPEN_BREW_BY_WEIGHT_REED_ACTIVE_LEVEL
#define OPEN_BREW_BY_WEIGHT_REED_ACTIVE_LEVEL LOW
#endif
#ifndef OPEN_BREW_BY_WEIGHT_REED_DEBOUNCE_MS
#define OPEN_BREW_BY_WEIGHT_REED_DEBOUNCE_MS 30
#endif
constexpr uint8_t REED_GPIO = OPEN_BREW_BY_WEIGHT_REED_GPIO;
constexpr uint8_t REED_ACTIVE_LEVEL = OPEN_BREW_BY_WEIGHT_REED_ACTIVE_LEVEL;
constexpr uint32_t REED_DEBOUNCE_MS = OPEN_BREW_BY_WEIGHT_REED_DEBOUNCE_MS;
#endif

constexpr uint32_t ACTIVATOR_DEBOUNCE_MS =
    OPEN_BREW_BY_WEIGHT_ACTIVATOR_DEBOUNCE_MS;
#ifndef OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_TOGGLE_MS
#define OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_TOGGLE_MS 50
#endif
#ifndef OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_SETTLE_MS
#define OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_SETTLE_MS 100
#endif
constexpr uint32_t SAFETY_HEARTBEAT_TOGGLE_MS =
    OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_TOGGLE_MS;
constexpr uint32_t CIRCUIT_FEEDBACK_SETTLE_MS =
    OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_SETTLE_MS;

#if defined(OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO) != \
    defined(OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_GPIO)
#error "Define both safety heartbeat and circuit feedback GPIOs, or neither"
#endif

#if defined(OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO)
constexpr bool EXTERNAL_SAFETY_HARDWARE_PRESENT = true;
constexpr uint8_t SAFETY_HEARTBEAT_GPIO =
    OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO;
constexpr uint8_t CIRCUIT_FEEDBACK_GPIO = OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_GPIO;
#ifndef OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_CLOSED_LEVEL
#define OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_CLOSED_LEVEL LOW
#endif
#ifndef OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_IDLE_LEVEL
#define OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_IDLE_LEVEL LOW
#endif
constexpr uint8_t SAFETY_HEARTBEAT_IDLE_LEVEL =
    OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_IDLE_LEVEL;
constexpr uint8_t CIRCUIT_FEEDBACK_CLOSED_LEVEL =
    OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_CLOSED_LEVEL;
#else
constexpr bool EXTERNAL_SAFETY_HARDWARE_PRESENT = false;
constexpr uint8_t SAFETY_HEARTBEAT_GPIO = 0;
constexpr uint8_t SAFETY_HEARTBEAT_IDLE_LEVEL = LOW;
constexpr uint8_t CIRCUIT_FEEDBACK_GPIO = 0;
constexpr uint8_t CIRCUIT_FEEDBACK_CLOSED_LEVEL = LOW;
#endif

static_assert(ACTIVATOR_GPIO != RELAY_GPIO,
              "Activator and relay must use different GPIOs");
static_assert(ACTIVATOR_ACTIVE_LEVEL == LOW,
              "Activator wiring requires INPUT_PULLUP and active LOW");
static_assert(RELAY_CLOSED_LEVEL != RELAY_OPEN_LEVEL,
              "Relay open and closed levels must differ");
static_assert((RELAY_OPEN_LEVEL == LOW || RELAY_OPEN_LEVEL == HIGH) &&
                  (RELAY_CLOSED_LEVEL == LOW || RELAY_CLOSED_LEVEL == HIGH),
              "Relay levels must be LOW or HIGH");
static_assert(!SCALE_STATUS_LED_PRESENT ||
                  (SCALE_CONNECTED_LED_GPIO != ACTIVATOR_GPIO &&
                   SCALE_CONNECTED_LED_GPIO != RELAY_GPIO),
              "Scale-connected LED GPIO must not share activator or relay GPIOs");
static_assert(!SPEAKER_PRESENT ||
                  (BUZZER_GPIO != ACTIVATOR_GPIO && BUZZER_GPIO != RELAY_GPIO &&
                   (!SCALE_STATUS_LED_PRESENT ||
                    BUZZER_GPIO != SCALE_CONNECTED_LED_GPIO)),
              "Buzzer GPIO must be distinct from activator, relay, and LED GPIOs");
static_assert(!USB_CONSOLE_JUMPER_PRESENT ||
                  (USB_CONSOLE_GPIO != 0 && USB_CONSOLE_GPIO != 45 &&
                   USB_CONSOLE_GPIO != 46),
              "USB console jumper must not use BOOT or ESP32-S3 strapping pins");
static_assert(!USB_CONSOLE_JUMPER_PRESENT ||
                  (USB_CONSOLE_GPIO != ACTIVATOR_GPIO &&
                   USB_CONSOLE_GPIO != RELAY_GPIO &&
                   (!SCALE_STATUS_LED_PRESENT ||
                    USB_CONSOLE_GPIO != SCALE_CONNECTED_LED_GPIO) &&
                   (!SPEAKER_PRESENT || USB_CONSOLE_GPIO != BUZZER_GPIO)),
              "USB console jumper GPIO must be distinct from activator, relay, LED, and buzzer");
static_assert(USB_CONSOLE_ACTIVE_LEVEL == LOW,
              "USB console jumper requires INPUT_PULLUP and active LOW");
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
static_assert(!SCALE_STATUS_LED_PRESENT ||
                  GPIO_IS_VALID_OUTPUT_GPIO(SCALE_CONNECTED_LED_GPIO),
              "Scale-connected LED must use a valid output-capable GPIO");
static_assert(!BUZZER_SUPPORT_ENABLED ||
                  GPIO_IS_VALID_OUTPUT_GPIO(BUZZER_GPIO),
              "Buzzer must use a valid output-capable GPIO");
static_assert(!USB_CONSOLE_JUMPER_PRESENT || GPIO_IS_VALID_GPIO(USB_CONSOLE_GPIO),
              "USB console jumper must use a valid input GPIO");
#endif
#if defined(OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO)
static_assert(SAFETY_HEARTBEAT_GPIO != RELAY_GPIO &&
                  SAFETY_HEARTBEAT_GPIO != ACTIVATOR_GPIO &&
                  CIRCUIT_FEEDBACK_GPIO != RELAY_GPIO &&
                  CIRCUIT_FEEDBACK_GPIO != ACTIVATOR_GPIO &&
                  CIRCUIT_FEEDBACK_GPIO != SAFETY_HEARTBEAT_GPIO,
              "Safety GPIOs must be unique");
static_assert(!SCALE_STATUS_LED_PRESENT ||
                  (SAFETY_HEARTBEAT_GPIO != SCALE_CONNECTED_LED_GPIO &&
                   CIRCUIT_FEEDBACK_GPIO != SCALE_CONNECTED_LED_GPIO),
              "Safety GPIOs must not share the scale-connected LED pin");
static_assert(!SPEAKER_PRESENT ||
                  (BUZZER_GPIO != SAFETY_HEARTBEAT_GPIO &&
                   BUZZER_GPIO != CIRCUIT_FEEDBACK_GPIO),
              "Buzzer GPIO must be distinct from safety GPIOs");
static_assert(!USB_CONSOLE_JUMPER_PRESENT ||
                  (USB_CONSOLE_GPIO != SAFETY_HEARTBEAT_GPIO &&
                   USB_CONSOLE_GPIO != CIRCUIT_FEEDBACK_GPIO),
              "USB console jumper GPIO must be distinct from safety GPIOs");
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
static_assert(GPIO_IS_VALID_OUTPUT_GPIO(SAFETY_HEARTBEAT_GPIO),
              "Heartbeat must use a valid output-capable GPIO");
static_assert(GPIO_IS_VALID_GPIO(CIRCUIT_FEEDBACK_GPIO),
              "circuit feedback must use a valid input GPIO");
#endif
static_assert(CIRCUIT_FEEDBACK_CLOSED_LEVEL == LOW ||
                  CIRCUIT_FEEDBACK_CLOSED_LEVEL == HIGH,
              "circuit feedback level must be LOW or HIGH");
static_assert(SAFETY_HEARTBEAT_IDLE_LEVEL == LOW,
              "External-safety heartbeat must idle LOW");
#endif
static_assert(ACTIVATOR_DEBOUNCE_MS > 0,
              "Activator debounce must be greater than zero");
static_assert(ACTIVATOR_DEBOUNCE_MS < 100,
              "Activator debounce must fit every valid rinse gesture");
#if OPEN_BREW_BY_WEIGHT_MACHINE_TYPE == 2
static_assert(REED_PRESENT,
              "Momentary-reed machine requires installed reed hardware");
static_assert(REED_GPIO != ACTIVATOR_GPIO && REED_GPIO != RELAY_GPIO &&
                  (!SCALE_STATUS_LED_PRESENT ||
                   REED_GPIO != SCALE_CONNECTED_LED_GPIO) &&
                  (!SPEAKER_PRESENT || REED_GPIO != BUZZER_GPIO) &&
                  (!USB_CONSOLE_JUMPER_PRESENT ||
                   REED_GPIO != USB_CONSOLE_GPIO),
              "Reed GPIO must be distinct from activator, relay, LED, buzzer, and USB console");
#if defined(OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO)
static_assert(REED_GPIO != SAFETY_HEARTBEAT_GPIO &&
                  REED_GPIO != CIRCUIT_FEEDBACK_GPIO,
              "Reed GPIO must be distinct from safety GPIOs");
#endif
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
static_assert(GPIO_IS_VALID_GPIO(REED_GPIO),
              "Reed must use a valid input GPIO");
#endif
static_assert(REED_ACTIVE_LEVEL == LOW,
              "Reed wiring requires INPUT_PULLUP and active LOW");
static_assert(REED_DEBOUNCE_MS > 0 && REED_DEBOUNCE_MS < 100,
              "Reed debounce must match the activator window");
#endif

}  // namespace openbrewbyweight

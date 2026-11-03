#pragma once

#include <stdint.h>

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
#include <atomic>
#else
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#if !CONFIG_ESP_TASK_WDT_EN
#error "Open Brew by Weight requires CONFIG_ESP_TASK_WDT_EN"
#endif
#if !CONFIG_ESP_TASK_WDT_PANIC
#error "Open Brew by Weight requires CONFIG_ESP_TASK_WDT_PANIC for automatic reset"
#endif
#if !CONFIG_ESP_INT_WDT
#error "Open Brew by Weight requires CONFIG_ESP_INT_WDT"
#endif
#if !CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT && \
    !CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT
#error "Open Brew by Weight production builds require panic to reboot"
#endif
#endif

namespace openbrewbyweight {

constexpr uint32_t TASK_WATCHDOG_TIMEOUT_MS = 5000;

constexpr uint32_t SAFETY_EVENT_CRITICAL_TASK_WATCHDOG = 1U << 0;
constexpr uint32_t SAFETY_EVENT_SAFE_RESTART = 1U << 1;
constexpr uint32_t SAFETY_EVENT_ALL =
    SAFETY_EVENT_CRITICAL_TASK_WATCHDOG | SAFETY_EVENT_SAFE_RESTART;

// ESP-IDF disables C++ hardware atomics for this Xtensa target. A statically
// allocated event group gives task/callback producers and the control task a
// FreeRTOS synchronization primitive without heap allocation or ISR usage.
class SafetyEventFlags {
 public:
  SafetyEventFlags() {
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
    handle_ = xEventGroupCreateStatic(&storage_);
    configASSERT(handle_ != nullptr);
#endif
  }

  void set(uint32_t bits) {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    bits_.fetch_or(bits, std::memory_order_release);
#else
    (void)xEventGroupSetBits(handle_, static_cast<EventBits_t>(bits));
#endif
  }

  bool isSet(uint32_t bits) const {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    return (bits_.load(std::memory_order_acquire) & bits) != 0U;
#else
    return (xEventGroupGetBits(handle_) & static_cast<EventBits_t>(bits)) !=
           0U;
#endif
  }

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  // Firmware never retracts a safety event; harnesses reset them between cases.
  bool consume(uint32_t bits) {
    return (bits_.fetch_and(~bits, std::memory_order_acq_rel) & bits) != 0U;
  }

  void clear(uint32_t bits) { (void)consume(bits); }
#endif

 private:
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  std::atomic<uint32_t> bits_{0U};
  static_assert(std::atomic<uint32_t>::is_always_lock_free,
                "Host safety event model must remain lock-free");
#else
  StaticEventGroup_t storage_ = {};
  EventGroupHandle_t handle_ = nullptr;
#endif
};

inline SafetyEventFlags safetyEventFlags;

// This is the sole production configuration of the global watchdog. OTA and
// flash paths retain their local yields/feeds and independent relay cutoff;
// they must never weaken the liveness budget for unrelated tasks.
inline bool configureTaskWatchdog() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  hostTaskWatchdogConfigured = hostTaskWatchdogOperationsSucceed;
  return hostTaskWatchdogConfigured;
#else
  esp_task_wdt_config_t config = {};
  config.timeout_ms = TASK_WATCHDOG_TIMEOUT_MS;
  config.idle_core_mask = (1U << portNUM_PROCESSORS) - 1U;
  config.trigger_panic = true;

  esp_err_t result = esp_task_wdt_reconfigure(&config);
  if (result == ESP_ERR_INVALID_STATE) {
    result = esp_task_wdt_init(&config);
  }
  return result == ESP_OK;
#endif
}

inline bool subscribeCurrentTaskToWatchdog() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (!hostTaskWatchdogConfigured || !hostTaskWatchdogOperationsSucceed) {
    return false;
  }
  ++hostTaskWatchdogSubscriptions;
  return true;
#else
  const esp_err_t status = esp_task_wdt_status(nullptr);
  return status == ESP_OK || esp_task_wdt_add(nullptr) == ESP_OK;
#endif
}

inline bool feedCurrentTaskWatchdog() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  if (!hostTaskWatchdogConfigured || !hostTaskWatchdogOperationsSucceed) {
    return false;
  }
  ++hostTaskWatchdogFeeds;
  return true;
#else
  return esp_task_wdt_reset() == ESP_OK;
#endif
}

}  // namespace openbrewbyweight

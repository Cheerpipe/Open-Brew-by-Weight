#pragma once

#include <stdint.h>

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
#include <mutex>
#else
#include <driver/gptimer.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>

#if !CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM
#error "Open Brew by Weight requires the GPTimer ISR handler in IRAM"
#endif
#endif

namespace openbrewbyweight {

// A one-shot deadline backed by an ESP32 general-purpose hardware timer.
// Its callback runs in interrupt context, independently of the Arduino loop,
// the BLE worker, Wi-Fi, and the esp_timer service task.
class IndependentSafetyTimer {
 public:
  using Callback = void (*)(void *context);

  IndependentSafetyTimer() = default;
  IndependentSafetyTimer(const IndependentSafetyTimer &) = delete;
  IndependentSafetyTimer &operator=(const IndependentSafetyTimer &) = delete;

  bool begin(Callback callback, void *context) {
    if (callback == nullptr || ready()) {
      return false;
    }
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!hostGptimerCreateSucceeds) {
      return false;
    }
    callback_ = callback;
    context_ = context;
    ready_ = true;
    return true;
#else
    gptimer_config_t config = {};
    // Stable 1-us timebase across DFS; enabled timer retains NO_LIGHT_SLEEP.
    config.clk_src = GPTIMER_CLK_SRC_XTAL;
    config.direction = GPTIMER_COUNT_UP;
    config.resolution_hz = 1000000;

    if (gptimer_new_timer(&config, &timer_) != ESP_OK) {
      return false;
    }
    gptimer_event_callbacks_t callbacks = {};
    callbacks.on_alarm = &alarmCallback;
    if (gptimer_register_event_callbacks(timer_, &callbacks, this) != ESP_OK) {
      gptimer_del_timer(timer_);
      timer_ = nullptr;
      return false;
    }
    portENTER_CRITICAL(&stateMux_);
    callback_ = callback;
    context_ = context;
    portEXIT_CRITICAL(&stateMux_);
    if (gptimer_enable(timer_) != ESP_OK) {
      gptimer_del_timer(timer_);
      timer_ = nullptr;
      portENTER_CRITICAL(&stateMux_);
      callback_ = nullptr;
      context_ = nullptr;
      portEXIT_CRITICAL(&stateMux_);
      return false;
    }
    portENTER_CRITICAL(&stateMux_);
    ready_ = true;
    portEXIT_CRITICAL(&stateMux_);
    return true;
#endif
  }

  bool arm(uint32_t timeoutMs) {
    if (!ready() || timeoutMs == 0) {
      return false;
    }
    if (!stop()) {
      return false;
    }
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!hostGptimerArmSucceeds) {
      return false;
    }
    dueAtUs_ = static_cast<uint64_t>(millis()) * 1000ULL +
               static_cast<uint64_t>(timeoutMs) * 1000ULL;
    running_ = true;
    return true;
#else
    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = static_cast<uint64_t>(timeoutMs) * 1000ULL;
    if (gptimer_set_raw_count(timer_, 0) != ESP_OK ||
        gptimer_set_alarm_action(timer_, &alarm) != ESP_OK) {
      return false;
    }

    // Publish RUNNING before start. A very short alarm may fire before
    // gptimer_start() returns; the shared stateMux_ lets the ISR clear this
    // state without a C++ data race and prevents this task from restoring a
    // completed arm afterward.
    portENTER_CRITICAL(&stateMux_);
    running_ = true;
    portEXIT_CRITICAL(&stateMux_);
    if (gptimer_start(timer_) != ESP_OK) {
      portENTER_CRITICAL(&stateMux_);
      running_ = false;
      portEXIT_CRITICAL(&stateMux_);
      return false;
    }
    return true;
#endif
  }

  bool stop() {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (callbackInFlight_) return false;
    running_ = false;
    return true;
#else
    bool wasRunning;
    portENTER_CRITICAL(&stateMux_);
    if (!ready_) {
      portEXIT_CRITICAL(&stateMux_);
      return true;
    }
    if (callbackInFlight_) {
      portEXIT_CRITICAL(&stateMux_);
      return false;
    }
    wasRunning = running_;
    portEXIT_CRITICAL(&stateMux_);

    // A one-shot alarm disables the hardware alarm but leaves the GPTimer
    // counter running, so always stop the driver even after the
    // ISR has cleared the logical RUNNING state. INVALID_STATE is benign only
    // when the ISR had already completed this arm; while logically running it
    // indicates an unexpected driver transition and must fail the next arm.
    const esp_err_t result = gptimer_stop(timer_);
    if (result != ESP_OK &&
        !(result == ESP_ERR_INVALID_STATE && !wasRunning)) {
      return false;
    }
    portENTER_CRITICAL(&stateMux_);
    // cppcheck-suppress knownConditionTrueFalse ; the alarmCallback() ISR can set callbackInFlight_ while gptimer_stop() runs between the critical sections
    if (callbackInFlight_) {
      portEXIT_CRITICAL(&stateMux_);
      return false;
    }
    running_ = false;
    portEXIT_CRITICAL(&stateMux_);
    return true;
#endif
  }

  bool ready() const {
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    std::lock_guard<std::mutex> lock(stateMutex_);
#else
    portENTER_CRITICAL(&stateMux_);
#endif
    const bool value = ready_;
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
    return value;
#else
    portEXIT_CRITICAL(&stateMux_);
    return value;
#endif
  }

#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  bool running() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return running_;
  }

  void serviceForHost() {
    serviceForHostAt(static_cast<uint64_t>(millis()) * 1000ULL);
  }

  void serviceForHostAt(uint64_t nowUs) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (!running_ || nowUs < dueAtUs_) {
      return;
    }
    running_ = false;
    callbackInFlight_ = true;
    Callback callback = callback_;
    void *context = context_;
    lock.unlock();
    callback(context);
    lock.lock();
    callbackInFlight_ = false;
  }

  void resetForHost() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    dueAtUs_ = 0;
    callback_ = nullptr;
    context_ = nullptr;
    ready_ = false;
    running_ = false;
    callbackInFlight_ = false;
  }
#endif

 private:
#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
  static bool IRAM_ATTR alarmCallback(
      gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
      void *userContext) {
    (void)timer;
    (void)event;
    auto *self = static_cast<IndependentSafetyTimer *>(userContext);
    Callback callback = nullptr;
    void *context = nullptr;
    portENTER_CRITICAL_ISR(&self->stateMux_);
    if (self->running_) {
      self->running_ = false;
      self->callbackInFlight_ = true;
      // callback_ and context_ are published once by begin() before ready_ and
      // remain immutable for the timer's enabled lifetime.
      callback = self->callback_;
      context = self->context_;
    }
    portEXIT_CRITICAL_ISR(&self->stateMux_);
    if (callback != nullptr) {
      callback(context);
      portENTER_CRITICAL_ISR(&self->stateMux_);
      self->callbackInFlight_ = false;
      portEXIT_CRITICAL_ISR(&self->stateMux_);
    }
    return false;
  }

  gptimer_handle_t timer_ = nullptr;
  mutable portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;
#else
  uint64_t dueAtUs_ = 0;
  mutable std::mutex stateMutex_;
#endif
  Callback callback_ = nullptr;
  void *context_ = nullptr;
  bool ready_ = false;
  bool running_ = false;
  bool callbackInFlight_ = false;
};

}  // namespace openbrewbyweight

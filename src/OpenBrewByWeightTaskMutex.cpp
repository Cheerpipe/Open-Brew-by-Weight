#include "OpenBrewByWeightTaskMutex.h"

// Host builds embed TaskMutex in host-test fragments; only the firmware
// target compiles this file (registered in idf/components/openBrewByWeight).
#if !defined(OPEN_BREW_BY_WEIGHT_TASK_MUTEX_HOST)
#include "OpenBrewByWeightSemaphore.h"

namespace openbrewbyweight {

void TaskMutex::lock_() {
  const BaseType_t taken = xSemaphoreTake(handle_, portMAX_DELAY);
  configASSERT(taken == pdTRUE);
  (void)taken;
  ownedSinceUs_ = static_cast<uint32_t>(esp_timer_get_time());
}

bool TaskMutex::tryLock_() {
  if (xSemaphoreTake(handle_, 0) != pdTRUE) return false;
  ownedSinceUs_ = static_cast<uint32_t>(esp_timer_get_time());
  return true;
}

void TaskMutex::unlock_() {
  // Keep the longest hold since the last reset, before releasing: the fields
  // are only touched while the mutex is held, so the mutex itself serializes
  // every access. A short re-lock after a long section must not hide it. The
  // name is copied because the owner handle would dangle for tasks deleted
  // after giving (e.g. the websocket client's worker).
  const uint32_t holdUs =
      static_cast<uint32_t>(esp_timer_get_time()) - ownedSinceUs_;
  if (holdUs >= longestHoldUs_) {
    longestHoldUs_ = holdUs;
    const char *name = pcTaskGetName(nullptr);
    size_t index = 0;
    for (; index < sizeof(longestOwner_) - 1 && name[index] != '\0'; ++index) {
      longestOwner_[index] = name[index];
    }
    longestOwner_[index] = '\0';
  }
  const BaseType_t given = semaphoreGive(handle_);
  configASSERT(given == pdTRUE);
  (void)given;
}

}  // namespace openbrewbyweight

#endif

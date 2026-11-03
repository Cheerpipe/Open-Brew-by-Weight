#pragma once

// FreeRTOS spells xSemaphoreGive and vSemaphoreDelete as macros that C-cast
// the handle to QueueHandle_t, which SemaphoreHandle_t already is. On the
// target these call the queue functions the macros expand to, without the
// cast; host builds keep the stubs their platform headers declare.
#if defined(ESP_PLATFORM)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

inline BaseType_t semaphoreGive(SemaphoreHandle_t semaphore) {
  return xQueueGenericSend(semaphore, nullptr, semGIVE_BLOCK_TIME, queueSEND_TO_BACK);
}

inline void semaphoreDelete(SemaphoreHandle_t semaphore) { vQueueDelete(semaphore); }
#else
inline auto semaphoreGive(SemaphoreHandle_t semaphore) { return xSemaphoreGive(semaphore); }

inline void semaphoreDelete(SemaphoreHandle_t semaphore) { vSemaphoreDelete(semaphore); }
#endif

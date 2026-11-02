#include "OpenBrewByWeightBleRuntime.h"

#if defined(OPEN_BREW_BY_WEIGHT_BLE_RUNTIME_HOST_TEST)
#include "ble_runtime_host_stubs.h"
#else
#include "sdkconfig.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

namespace {

constexpr EventBits_t kReadyBit = BIT0;
constexpr EventBits_t kHostStoppedBit = BIT1;
constexpr char kTag[] = "ble.runtime";

StaticEventGroup_t gEventStorage;
EventGroupHandle_t gEvents = nullptr;
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
OpenBrewByWeightBleHealth gHealth = {
    OpenBrewByWeightBleRuntimeState::Stopped, 0, 0, 0, 0, UINT32_MAX, 0, 0, 0, 0, 0, 0};
uint8_t gOwnAddressType = 0xff;
bool gPortInitialized = false;
TaskHandle_t gHostTask = nullptr;
StaticSemaphore_t gHostTaskMutexStorage;
SemaphoreHandle_t gHostTaskMutex = xSemaphoreCreateMutexStatic(&gHostTaskMutexStorage);

struct MemorySnapshot {
  uint32_t internalFreeBytes;
  uint32_t internalMinimumFreeBytes;
  uint32_t internalLargestBlockBytes;
  uint32_t psramFreeBytes;
  uint32_t psramMinimumFreeBytes;
  uint32_t psramLargestBlockBytes;
};

MemorySnapshot captureMemory() {
  const uint32_t internalCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  return {heap_caps_get_free_size(internalCaps),
          heap_caps_get_minimum_free_size(internalCaps),
          heap_caps_get_largest_free_block(internalCaps),
          heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)};
}

void storeMemoryLocked(const MemorySnapshot &memory) {
  gHealth.internalFreeBytes = memory.internalFreeBytes;
  gHealth.internalMinimumFreeBytes = memory.internalMinimumFreeBytes;
  gHealth.internalLargestBlockBytes = memory.internalLargestBlockBytes;
  gHealth.psramFreeBytes = memory.psramFreeBytes;
  gHealth.psramMinimumFreeBytes = memory.psramMinimumFreeBytes;
  gHealth.psramLargestBlockBytes = memory.psramLargestBlockBytes;
}

void onReset(int reason) {
  const MemorySnapshot memory = captureMemory();
  portENTER_CRITICAL(&gMux);
  gHealth.state = OpenBrewByWeightBleRuntimeState::Unsynced;
  gHealth.lastResetReason = reason;
  ++gHealth.resetCount;
  storeMemoryLocked(memory);
  portEXIT_CRITICAL(&gMux);
  if (gEvents != nullptr) {
    xEventGroupClearBits(gEvents, kReadyBit);
  }
}

void onSync() {
  uint8_t addressType = 0xff;
  const int rc = ble_hs_id_infer_auto(0, &addressType);
  const MemorySnapshot memory = captureMemory();
  portENTER_CRITICAL(&gMux);
  gHealth.lastError = rc;
  if (rc == 0) {
    gOwnAddressType = addressType;
    gHealth.state = OpenBrewByWeightBleRuntimeState::Ready;
    ++gHealth.syncGeneration;
    if (gHealth.syncGeneration == 0) {
      gHealth.syncGeneration = 1;
    }
  } else {
    gOwnAddressType = 0xff;
    gHealth.state = OpenBrewByWeightBleRuntimeState::Failed;
  }
  storeMemoryLocked(memory);
  portEXIT_CRITICAL(&gMux);
  if (gEvents != nullptr && rc == 0) {
    xEventGroupSetBits(gEvents, kReadyBit);
  }
}

void hostTask(void *) {
  portENTER_CRITICAL(&gMux);
  gHostTask = xTaskGetCurrentTaskHandle();
  portEXIT_CRITICAL(&gMux);
  nimble_port_run();
  const uint32_t highWater =
      static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
  portENTER_CRITICAL(&gMux);
  gHealth.hostTaskStackHighWaterBytes = highWater;
  portEXIT_CRITICAL(&gMux);
  if (gEvents != nullptr) {
    xEventGroupSetBits(gEvents, kHostStoppedBit);
  }
  // The lifecycle owner deletes this task through
  // nimble_port_freertos_deinit(). Suspending here makes the join explicit and
  // avoids placing completion code after an API that deletes the current task.
  vTaskSuspend(nullptr);
}

TickType_t timeoutTicks(uint32_t timeoutMs) {
  const TickType_t ticks = pdMS_TO_TICKS(timeoutMs);
  return ticks == 0 && timeoutMs != 0 ? 1 : ticks;
}

}  // namespace

bool openBrewByWeightBleRuntimeStart(uint32_t timeoutMs) {
  if (gEvents == nullptr) {
    gEvents = xEventGroupCreateStatic(&gEventStorage);
    if (gEvents == nullptr) {
      return false;
    }
  }
  if (openBrewByWeightBleRuntimeReady()) {
    return true;
  }

  const MemorySnapshot startingMemory = captureMemory();
  portENTER_CRITICAL(&gMux);
  const bool canStart = !gPortInitialized;
  if (canStart) {
    gHostTask = nullptr;
    gHealth.state = OpenBrewByWeightBleRuntimeState::Starting;
    gHealth.lastError = 0;
    gHealth.lastResetReason = 0;
    gHealth.hostTaskStackHighWaterBytes = UINT32_MAX;
    storeMemoryLocked(startingMemory);
  }
  portEXIT_CRITICAL(&gMux);

  if (canStart) {
    xEventGroupClearBits(gEvents, kReadyBit | kHostStoppedBit);
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
      const MemorySnapshot failedMemory = captureMemory();
      portENTER_CRITICAL(&gMux);
      gHealth.state = OpenBrewByWeightBleRuntimeState::Failed;
      gHealth.lastError = static_cast<int32_t>(err);
      storeMemoryLocked(failedMemory);
      portEXIT_CRITICAL(&gMux);
      ESP_LOGE(kTag, "nimble_port_init failed: %s", esp_err_to_name(err));
      return false;
    }
    gPortInitialized = true;
    ble_hs_cfg.reset_cb = onReset;
    ble_hs_cfg.sync_cb = onSync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    nimble_port_freertos_init(hostTask);
  }

  const EventBits_t bits = xEventGroupWaitBits(
      gEvents, kReadyBit, pdFALSE, pdTRUE, timeoutTicks(timeoutMs));
  if ((bits & kReadyBit) != 0) {
    return true;
  }
  const MemorySnapshot timeoutMemory = captureMemory();
  portENTER_CRITICAL(&gMux);
  if (gHealth.state == OpenBrewByWeightBleRuntimeState::Starting) {
    gHealth.state = OpenBrewByWeightBleRuntimeState::Failed;
    gHealth.lastError = BLE_HS_ETIMEOUT;
  }
  storeMemoryLocked(timeoutMemory);
  portEXIT_CRITICAL(&gMux);
  return false;
}

bool openBrewByWeightBleRuntimeReady() {
  portENTER_CRITICAL(&gMux);
  const bool ready = gHealth.state == OpenBrewByWeightBleRuntimeState::Ready;
  portEXIT_CRITICAL(&gMux);
  return ready;
}

uint8_t openBrewByWeightBleRuntimeOwnAddressType() {
  portENTER_CRITICAL(&gMux);
  const uint8_t type = gOwnAddressType;
  portEXIT_CRITICAL(&gMux);
  return type;
}

uint32_t openBrewByWeightBleRuntimeSyncGeneration() {
  portENTER_CRITICAL(&gMux);
  const uint32_t generation = gHealth.syncGeneration;
  portEXIT_CRITICAL(&gMux);
  return generation;
}

OpenBrewByWeightBleHealth openBrewByWeightBleRuntimeHealth(bool sampleMemory) {
  const MemorySnapshot memory = sampleMemory ? captureMemory() : MemorySnapshot{};
  // Telemetry never waits for teardown; a live sample retains task lifetime.
  const bool sampleLive = xSemaphoreTake(gHostTaskMutex, 0) == pdTRUE;
  portENTER_CRITICAL(&gMux);
  TaskHandle_t hostTaskHandle = sampleLive ? gHostTask : nullptr;
  portEXIT_CRITICAL(&gMux);
  const uint32_t liveHighWater = hostTaskHandle == nullptr
                                     ? 0
                                     : static_cast<uint32_t>(
                                           uxTaskGetStackHighWaterMark(
                                               hostTaskHandle));
  portENTER_CRITICAL(&gMux);
  if (sampleMemory) storeMemoryLocked(memory);
  if (hostTaskHandle != nullptr &&
      liveHighWater < gHealth.hostTaskStackHighWaterBytes) {
    gHealth.hostTaskStackHighWaterBytes = liveHighWater;
  }
  const OpenBrewByWeightBleHealth health = gHealth;
  portEXIT_CRITICAL(&gMux);
  if (sampleLive) xSemaphoreGive(gHostTaskMutex);
  return health;
}

bool openBrewByWeightBleRuntimeStop(uint32_t timeoutMs) {
  if (!gPortInitialized || gEvents == nullptr) {
    return true;
  }
  const TickType_t startedAt = xTaskGetTickCount();
  const TickType_t budget = timeoutTicks(timeoutMs);
  if (xSemaphoreTake(gHostTaskMutex, budget) != pdTRUE) return false;
  portENTER_CRITICAL(&gMux);
  gHealth.state = OpenBrewByWeightBleRuntimeState::Stopping;
  portEXIT_CRITICAL(&gMux);
  xEventGroupClearBits(gEvents, kReadyBit | kHostStoppedBit);
  const int rc = nimble_port_stop();
  if (rc != 0) {
    portENTER_CRITICAL(&gMux);
    gHealth.state = OpenBrewByWeightBleRuntimeState::Failed;
    gHealth.lastError = rc;
    portEXIT_CRITICAL(&gMux);
    xSemaphoreGive(gHostTaskMutex);
    return false;
  }
  const TickType_t elapsed = xTaskGetTickCount() - startedAt;
  const EventBits_t bits = xEventGroupWaitBits(
      gEvents, kHostStoppedBit, pdFALSE, pdTRUE,
      elapsed < budget ? budget - elapsed : 0);
  if ((bits & kHostStoppedBit) == 0) {
    xSemaphoreGive(gHostTaskMutex);
    return false;
  }
  nimble_port_freertos_deinit();
  portENTER_CRITICAL(&gMux);
  gHostTask = nullptr;
  portEXIT_CRITICAL(&gMux);
  const esp_err_t deinitError = nimble_port_deinit();
  const MemorySnapshot stoppedMemory = captureMemory();
  portENTER_CRITICAL(&gMux);
  gHealth.lastError = static_cast<int32_t>(deinitError);
  if (deinitError == ESP_OK) {
    gPortInitialized = false;
    gOwnAddressType = 0xff;
    gHealth.state = OpenBrewByWeightBleRuntimeState::Stopped;
  } else {
    gHealth.state = OpenBrewByWeightBleRuntimeState::Failed;
  }
  storeMemoryLocked(stoppedMemory);
  portEXIT_CRITICAL(&gMux);
  xSemaphoreGive(gHostTaskMutex);
  return deinitError == ESP_OK;
}

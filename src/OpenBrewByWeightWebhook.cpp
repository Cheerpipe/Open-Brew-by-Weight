#include "OpenBrewByWeightWebhook.h"
#include "OpenBrewByWeightOutboundAdmission.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightHttpDiagnostics.h"

#if defined(OPEN_BREW_BY_WEIGHT_WEBHOOK_TEST_PLATFORM) || \
    (!defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
     !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST))

#if !defined(OPEN_BREW_BY_WEIGHT_WEBHOOK_TEST_PLATFORM)
#include <Arduino.h>
#include <WiFi.h>
#include <esp_http_client.h>
#include <esp_mac.h>
#endif
void serialTraceCategoryf(openbrewbyweight::LogLevel level,
                          openbrewbyweight::DebugCategory category,
                          const char *fmt, ...);

namespace openbrewbyweight {
namespace {

constexpr size_t kWebhookQueueDepth = 4;
constexpr size_t kWebhookPayloadCapacity = 2048;
// A cold TLS send (client create + handshake) left 1 400 B free on 4 096
// (2026-09-26 target capture), below the 1 536 B release gate.
constexpr uint32_t kWebhookWorkerStackBytes = 4608;
constexpr int kWebhookTimeoutMs = 1800;
// Each async progress call blocks at most this long between admission checks.
constexpr int kWebhookPollMs = 25;
// IDF 6.1 reports a plain-TCP async connect as established while the
// handshake is still pending, so the header write must wait for it (slow under
// modem sleep); HEADERS_SENT restores the poll interval.
constexpr int kWebhookHandshakeWaitMs = 500;
constexpr uint32_t kWebhookStopTimeoutMs = 2500;
// Backoff before each retry of a failed delivery, after the first attempt.
constexpr uint32_t kWebhookRetryDelaysMs[] = {3000, 6000, 9000};
constexpr uint8_t kWebhookAttempts =
    1 + sizeof(kWebhookRetryDelaysMs) / sizeof(kWebhookRetryDelaysMs[0]);

// Events whose newest copy carries the whole state, so a newer queued one
// makes a failing older one obsolete; -1 for events where every copy matters.
int stateSlot(WebhookEventType type) {
  switch (type) {
    case WebhookEventType::BREWING:
    case WebhookEventType::IDLE: return 0;
    case WebhookEventType::PRESETS_CHANGED: return 1;
    case WebhookEventType::QUICK_SETTINGS_CHANGED: return 2;
    case WebhookEventType::IP_CHANGED: return 3;
    default: return -1;
  }
}

const char *eventName(WebhookEventType type) {
  switch (type) {
    case WebhookEventType::BREWING: return "brew_state";
    case WebhookEventType::IDLE: return "brew_state";
    case WebhookEventType::FIRST_DROP: return "first_drop";
    case WebhookEventType::END: return "end";
    case WebhookEventType::TEST: return "test";
    case WebhookEventType::PRESETS_CHANGED: return "presets_changed";
    case WebhookEventType::QUICK_SETTINGS_CHANGED:
      return "quick_settings_changed";
    case WebhookEventType::CONTROLLER_STARTED: return "controller_started";
    case WebhookEventType::IP_CHANGED: return "ip_changed";
    case WebhookEventType::ACTIVATION_HISTORY:
      return "integration_history_end";
  }
  return "unknown";
}

}  // namespace

void deviceId(char *output, size_t capacity) {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    snprintf(output, capacity, "unknown");
    return;
  }
  snprintf(output, capacity, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool WebhookDispatcher::begin(const WebhookConfig &config) {
  if (lifecycleMutex_ != nullptr || workerStopped_ != nullptr) return false;
  mux_.lock();
  config_ = config;
  configGeneration_ = 1;
  deferDuringShot_.store(config.deferDuringShot, std::memory_order_release);
  mux_.unlock();
  lifecycleMutex_ = xSemaphoreCreateMutex();
  if (lifecycleMutex_ == nullptr) {
    mux_.lock();
    ++status_.workerStartFailures;
    mux_.unlock();
    return false;
  }
  workerStopped_ = xSemaphoreCreateBinary();
  if (workerStopped_ == nullptr) {
    vSemaphoreDelete(lifecycleMutex_);
    lifecycleMutex_ = nullptr;
    mux_.lock();
    ++status_.workerStartFailures;
    mux_.unlock();
    return false;
  }
  if (!config.enabled || startWorker()) return true;
  // startWorker() already released its queue/payload attempt. Complete the
  // failed transaction by releasing the passive lifecycle objects as well.
  (void)stop();
  return false;
}

bool WebhookDispatcher::stop() {
  if (lifecycleMutex_ == nullptr) {
    if (workerStopped_ != nullptr) {
      vSemaphoreDelete(workerStopped_);
      workerStopped_ = nullptr;
    }
    return true;
  }

  TaskHandle_t worker = nullptr;
  if (xSemaphoreTake(lifecycleMutex_, pdMS_TO_TICKS(kWebhookStopTimeoutMs)) !=
      pdTRUE) {
    return false;
  }
  worker = task_;
  if (worker != nullptr) {
    mux_.lock();
    workerState_ = WorkerState::STOPPING;
    stopAfterDrain_ = false;
    mux_.unlock();
  }
  xSemaphoreGive(lifecycleMutex_);

  if (worker != nullptr) {
    xTaskNotifyGive(worker);
    if (workerStopped_ == nullptr ||
        xSemaphoreTake(workerStopped_, pdMS_TO_TICKS(kWebhookStopTimeoutMs)) !=
            pdTRUE) {
      return false;
    }
  }

  vSemaphoreDelete(lifecycleMutex_);
  lifecycleMutex_ = nullptr;
  vSemaphoreDelete(workerStopped_);
  workerStopped_ = nullptr;
  return true;
}

bool WebhookDispatcher::startWorker() {
  if (lifecycleMutex_ == nullptr ||
      xSemaphoreTake(lifecycleMutex_, portMAX_DELAY) != pdTRUE) return false;
  // Enabling while a disabled test drains keeps the running worker alive.
  mux_.lock();
  if (config_.enabled) stopAfterDrain_ = false;
  const WorkerState state = workerState_;
  if (state == WorkerState::STOPPED) workerState_ = WorkerState::STARTING;
  mux_.unlock();
  if (state != WorkerState::STOPPED) {
    xSemaphoreGive(lifecycleMutex_);
    return state == WorkerState::READY;
  }
  // A worker disabled by setConfig() may have left an unconsumed completion.
  // Never let that stale acknowledgement satisfy a later stop()/join.
  if (workerStopped_ != nullptr) (void)xSemaphoreTake(workerStopped_, 0);

  // FreeRTOS copies the bounded event snapshots under its spinlock: keep this
  // small internal allocation deterministic.
  // internal. The larger HTTP payload remains external and fails closed.
  const size_t queueStorageBytes =
      kWebhookQueueDepth * sizeof(QueuedWebhook);
  uint8_t *queueStorage =
      static_cast<uint8_t *>(allocInternal(queueStorageBytes, AllocationOwner::WEBHOOK));
  char *payload =
      static_cast<char *>(allocExternal(kWebhookPayloadCapacity, AllocationOwner::WEBHOOK));
  QueueHandle_t queue = nullptr;
  if (queueStorage != nullptr) {
    queue = xQueueCreateStatic(kWebhookQueueDepth, sizeof(QueuedWebhook),
                               queueStorage, &queueControl_);
  }
  if (queue == nullptr || payload == nullptr) {
    if (queue != nullptr) vQueueDelete(queue);
    heapCapsFree(queueStorage);
    heapCapsFree(payload);
    mux_.lock();
    workerState_ = WorkerState::STOPPED;
    status_.workerReady = false;
    ++status_.workerStartFailures;
    mux_.unlock();
    xSemaphoreGive(lifecycleMutex_);
    return false;
  }
  mux_.lock();
  queue_ = queue;
  queueStorage_ = queueStorage;
  payload_ = payload;
  mux_.unlock();
  if (xTaskCreatePinnedToCore(taskEntry, "webhook", kWebhookWorkerStackBytes,
                              this,
                             tskIDLE_PRIORITY, &task_, 0) != pdPASS) {
    vQueueDelete(queue);
    heapCapsFree(queueStorage);
    heapCapsFree(payload);
    mux_.lock();
    queue_ = nullptr;
    queueStorage_ = nullptr;
    payload_ = nullptr;
    task_ = nullptr;
    workerState_ = WorkerState::STOPPED;
    status_.workerReady = false;
    ++status_.workerStartFailures;
    mux_.unlock();
    xSemaphoreGive(lifecycleMutex_);
    return false;
  }
  mux_.lock();
  workerState_ = WorkerState::READY;
  status_.workerReady = true;
  ++status_.workerStarts;
  mux_.unlock();
  xSemaphoreGive(lifecycleMutex_);
  return true;
}

void WebhookDispatcher::releaseWorkerFromTask() {
  cleanupHttpClient();
  QueueHandle_t queue = nullptr;
  uint8_t *queueStorage = nullptr;
  char *payload = nullptr;
  if (lifecycleMutex_ != nullptr &&
      xSemaphoreTake(lifecycleMutex_, portMAX_DELAY) == pdTRUE) {
    mux_.lock();
    queue = queue_;
    queueStorage = queueStorage_;
    payload = payload_;
    queue_ = nullptr;
    queueStorage_ = nullptr;
    payload_ = nullptr;
    task_ = nullptr;
    workerState_ = WorkerState::STOPPED;
    stopAfterDrain_ = false;
    memset(queuedStates_, 0, sizeof(queuedStates_));
    status_.workerReady = false;
    status_.sending = false;
    ++status_.workerStops;
    mux_.unlock();
    xSemaphoreGive(lifecycleMutex_);
  }
  if (queue != nullptr) vQueueDelete(queue);
  heapCapsFree(queueStorage);
  heapCapsFree(payload);
  if (workerStopped_ != nullptr) xSemaphoreGive(workerStopped_);
}

esp_http_client_handle_t WebhookDispatcher::ensureHttpClient(const char *url) {
  if (!validWebhookUrl(url)) return nullptr;
  if (httpClient_ &&
      !webhookClientMustRecreate(httpClientUrl_, url)) {
    mux_.lock();
    ++status_.clientReuses;
    mux_.unlock();
    return httpClient_.get();
  }

  cleanupHttpClient();
  esp_http_client_config_t config = {};
  config.url = url;
  config.timeout_ms = kWebhookPollMs;
  // IDF 6.1 TCP transport registers async connect with non-blocking sockets;
  // perform preserves header/body offsets across EAGAIN, including plain HTTP.
  config.is_async = true;
  config.disable_auto_redirect = true;
  config.event_handler = httpEvent;
  config.user_data = this;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) return nullptr;
  // Reconnect per delivery: receivers close idle keep-alive sockets (Home
  // Assistant after 75 s), and a write into one loses the event.
  if (esp_http_client_set_header(client, "Content-Type", "application/json") !=
          ESP_OK ||
      esp_http_client_set_header(client, "Connection", "close") != ESP_OK ||
      esp_http_client_set_header(client, "User-Agent", "OpenBrewByWeight/1") !=
          ESP_OK) {
    (void)esp_http_client_cleanup(client);
    return nullptr;
  }
  httpClient_.reset(client);
  const size_t length = strnlen(url, sizeof(httpClientUrl_) - 1U);
  memcpy(httpClientUrl_, url, length);
  httpClientUrl_[length] = '\0';
  mux_.lock();
  ++status_.clientCreates;
  mux_.unlock();
  return client;
}

esp_err_t WebhookDispatcher::httpEvent(esp_http_client_event_t *event) {
  if (event->event_id == HTTP_EVENT_HEADERS_SENT) {
    static_cast<WebhookDispatcher *>(event->user_data)->requestSent_ = true;
    (void)esp_http_client_set_timeout_ms(event->client, kWebhookPollMs);
  }
  return ESP_OK;
}

void WebhookDispatcher::lookupDone(const char *, const ip_addr_t *address,
                                   void *self) {
  static_cast<WebhookDispatcher *>(self)->hostLookup_.store(
      address != nullptr ? HostLookup::RESOLVED : HostLookup::FAILED);
}

void WebhookDispatcher::lookupStart(void *self) {
  auto *dispatcher = static_cast<WebhookDispatcher *>(self);
  ip_addr_t address;
  const err_t result = dns_gethostbyname(dispatcher->lookupHost_, &address,
                                         lookupDone, self);
  if (result != ERR_INPROGRESS) lookupDone(nullptr,
      result == ERR_OK ? &address : nullptr, self);
}

// getaddrinfo() inside perform() blocks for lwIP's whole DNS retry window,
// beyond the delivery deadline and the admission gate. Resolve through the
// asynchronous lwIP API first so perform() is answered from the DNS cache.
template <typename Admitted>
bool WebhookDispatcher::resolveHost(const char *url, uint32_t started,
                                    Admitted admitted) {
  const char *host = url + 7;
  if (*host == '[') return true;
  const size_t length = strcspn(host, ":/?");
  for (;;) {
    const HostLookup state = hostLookup_.load();
    const bool current = strncmp(lookupHost_, host, length) == 0 &&
        lookupHost_[length] == '\0';
    if (state != HostLookup::PENDING && (!current || state == HostLookup::IDLE)) {
      memcpy(lookupHost_, host, length);
      lookupHost_[length] = '\0';
      hostLookup_.store(HostLookup::PENDING);
      if (tcpip_try_callback(lookupStart, this) == ERR_OK) continue;
      hostLookup_.store(HostLookup::IDLE);
      return false;
    }
    if (state != HostLookup::PENDING) {
      hostLookup_.store(HostLookup::IDLE);
      return state == HostLookup::RESOLVED;
    }
    if (!admitted() || millis() - started >= kWebhookTimeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void WebhookDispatcher::cleanupHttpClient() {
  if (!httpClient_) {
    httpClientUrl_[0] = '\0';
    return;
  }
  esp_http_client_handle_t client = httpClient_.release();
  httpClientUrl_[0] = '\0';
  const esp_err_t cleanupError = esp_http_client_cleanup(client);
  mux_.lock();
  ++status_.clientCleanups;
  if (cleanupError != ESP_OK) status_.lastError = cleanupError;
  mux_.unlock();
}

void WebhookDispatcher::setConfig(const WebhookConfig &config) {
  // A previous begin() can fail transactionally (for example, no PSRAM).
  // Permit a later configuration update to retry the full initialization.
  if (lifecycleMutex_ == nullptr) {
    (void)begin(config);
    return;
  }
  bool changed = false;
  mux_.lock();
  changed = memcmp(&config_, &config, sizeof(config)) != 0;
  if (changed) {
    if (!config.enabled || strcmp(config_.url, config.url) != 0)
      cleanupRequested_.store(true, std::memory_order_release);
    config_ = config;
    deferDuringShot_.store(config.deferDuringShot, std::memory_order_release);
    ++configGeneration_;
    if (configGeneration_ == 0) configGeneration_ = 1;
  }
  mux_.unlock();
  // Once created, keep the worker and its PSRAM buffers idle across ordinary
  // enable/disable changes. This avoids task/queue/stack churn under repeated
  // configuration updates. stop() remains the sole lifecycle teardown and
  // still performs stop/ack/join before releasing resources.
  if (config.enabled) (void)startWorker();
}

WebhookConfig WebhookDispatcher::config() const {
  WebhookConfig copy;
  mux_.lock();
  copy = config_;
  mux_.unlock();
  return copy;
}

WebhookStatus WebhookDispatcher::status() const {
  WebhookStatus copy;
  mux_.lock();
  copy = status_;
  copy.queued = queue_ != nullptr
      ? static_cast<uint8_t>(uxQueueMessagesWaiting(queue_)) : 0;
  mux_.unlock();
  copy.held = !dispatchAllowed();
  return copy;
}

HeapLifecycleAggregate WebhookDispatcher::heapTelemetry() const {
  HeapLifecycleAggregate copy;
  mux_.lock();
  copy = tlsHeap_.aggregate;
  mux_.unlock();
  return copy;
}

void WebhookDispatcher::setControlCritical(bool active) {
  controlCritical_.store(active, std::memory_order_release);
}

bool WebhookDispatcher::dispatchAllowed() const {
  return !outboundScaleInhibited() &&
      (!deferDuringShot_.load(std::memory_order_acquire) ||
       !controlCritical_.load(std::memory_order_acquire));
}

bool WebhookDispatcher::enqueue(const WebhookEvent &event) {
  if (event.type == WebhookEventType::TEST) (void)startWorker();
  WebhookConfig live;
  QueuedWebhook queued;
  queued.event = event;
  QueueHandle_t queue = nullptr;
  WorkerState workerState = WorkerState::STOPPED;
  mux_.lock();
  live = config_;
  queued.configGeneration = configGeneration_;
  mux_.unlock();
  bool selected = event.type == WebhookEventType::TEST;
  if (event.type == WebhookEventType::BREWING ||
      event.type == WebhookEventType::IDLE) selected = live.brewState;
  if (event.type == WebhookEventType::FIRST_DROP) selected = live.firstDrop;
  if (event.type == WebhookEventType::END) selected = live.end;
  if (event.type == WebhookEventType::PRESETS_CHANGED ||
      event.type == WebhookEventType::QUICK_SETTINGS_CHANGED ||
      event.type == WebhookEventType::CONTROLLER_STARTED ||
      event.type == WebhookEventType::IP_CHANGED ||
      event.type == WebhookEventType::ACTIVATION_HISTORY)
    selected = live.presetChanges;
  // An event the receiver did not subscribe to is consumed, not dropped.
  if (event.type != WebhookEventType::TEST && (!live.enabled || !selected))
    return true;
  // The worker never holds lifecycleMutex_ while idle or sending, so this
  // non-blocking take only fails during a worker start/stop transition.
  const bool locked = lifecycleMutex_ != nullptr &&
      xSemaphoreTake(lifecycleMutex_, 0) == pdTRUE;
  if (locked) {
    mux_.lock();
    queue = queue_;
    workerState = workerState_;
    mux_.unlock();
  }
  const bool accepted = locked && workerState == WorkerState::READY &&
      queue != nullptr && validWebhookUrl(live.url) &&
      xQueueSend(queue, &queued, 0) == pdTRUE;
  if (accepted) (void)queuedStates(event.type, 1);
  mux_.lock();
  if (!accepted) ++status_.dropped;
  else if (event.type == WebhookEventType::TEST && !live.enabled)
    stopAfterDrain_ = true;
  mux_.unlock();
  if (locked) xSemaphoreGive(lifecycleMutex_);
  return accepted;
}

void WebhookDispatcher::taskEntry(void *parameter) {
  static_cast<WebhookDispatcher *>(parameter)->task();
}

void WebhookDispatcher::task() {
  QueuedWebhook queued;
  bool haveQueued = false;
  uint8_t failures = 0;
  uint32_t failedAt = 0;
  for (;;) {
    mux_.lock();
    QueueHandle_t queue = queue_;
    WorkerState state = workerState_;
    mux_.unlock();
    if (state == WorkerState::STOPPING || queue == nullptr) break;
    if (cleanupRequested_.exchange(false, std::memory_order_acq_rel))
      cleanupHttpClient();
    if (!dispatchAllowed()) {
      if (httpClient_) (void)esp_http_client_close(httpClient_.get());
      completeOutboundAcquisitionPause(OutboundClient::WEBHOOK, millis());
    }
    // Leave events queued while a shot/rinse or scale connection attempt owns
    // radio time. Queue operations and HTTP remain entirely off control/BLE.
    bool waitedForQueue = false;
    if (!haveQueued && dispatchAllowed()) {
      waitedForQueue = true;
      haveQueued =
          xQueueReceive(queue, &queued, pdMS_TO_TICKS(50)) == pdTRUE;
      if (haveQueued) (void)queuedStates(queued.event.type, -1);
    }
    if (haveQueued && failures != 0 && queuedStates(queued.event.type, 0) > 0) {
      serialTraceCategoryf(LogLevel::WARNING, DebugCategory::NETWORK,
                           "webhook %s superseded by a newer queued copy after %u/%u attempts",
                           eventName(queued.event.type), static_cast<unsigned>(failures),
                           static_cast<unsigned>(kWebhookAttempts));
      haveQueued = false;
      failures = 0;
    }
    // The gate may change while xQueueReceive is blocked. Keep the dequeued
    // item locally and recheck so no request starts after the critical edge.
    if (haveQueued && queued.event.type == WebhookEventType::CONTROLLER_STARTED &&
        WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(25));
    } else if (haveQueued && dispatchAllowed() && (failures == 0 ||
               millis() - failedAt >= kWebhookRetryDelaysMs[failures - 1])) {
      const SendOutcome outcome = send(queued, failures);
      haveQueued = outcome == SendOutcome::HELD || outcome == SendOutcome::RETRY;
      if (outcome == SendOutcome::RETRY) {
        ++failures;
        failedAt = millis();
      } else if (!haveQueued) {
        failures = 0;
      }
    } else if (haveQueued || !waitedForQueue) {
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    mux_.lock();
    const bool drainStop = stopAfterDrain_ && !haveQueued;
    mux_.unlock();
    // Only a disabled test drain takes the lifecycle lock, so enqueue() can
    // never append to a queue that is about to be released.
    if (drainStop && xSemaphoreTake(lifecycleMutex_, portMAX_DELAY) == pdTRUE) {
      mux_.lock();
      if (workerState_ == WorkerState::READY && stopAfterDrain_ &&
          uxQueueMessagesWaiting(queue_) == 0) {
        workerState_ = WorkerState::STOPPING;
      }
      state = workerState_;
      mux_.unlock();
      xSemaphoreGive(lifecycleMutex_);
    }
    if (state == WorkerState::STOPPING) break;
  }
  releaseWorkerFromTask();
  vTaskDelete(nullptr);
}

int8_t WebhookDispatcher::queuedStates(WebhookEventType type, int8_t delta) {
  const int slot = stateSlot(type);
  if (slot < 0) return 0;
  mux_.lock();
  // Signed: the worker may receive an event before enqueue() counts it.
  const int8_t count = queuedStates_[slot] += delta;
  mux_.unlock();
  return count;
}

bool WebhookDispatcher::buildPayload(const WebhookEvent &event, char *output,
                                     size_t capacity) {
  char id[18] = {};
  char presetName[sizeof(event.presetName) * 6] = {};
  if (!escapeJsonString(event.presetName, presetName, sizeof(presetName)))
    return false;
  deviceId(id, sizeof(id));
  int written = snprintf(
      output, capacity,
      "{\"schemaVersion\":1,\"event\":\"%s\",\"deviceId\":\"%s\","
      "\"bootId\":%lu,\"cycleId\":%lu,\"uptimeMs\":%lu,\"timestamp\":%lu,"
      "\"sentAtUptimeMs\":%lu",
      eventName(event.type), id, static_cast<unsigned long>(event.bootId),
      static_cast<unsigned long>(event.cycleId),
      static_cast<unsigned long>(event.uptimeMs),
      static_cast<unsigned long>(event.unixSec),
      static_cast<unsigned long>(millis()));
  if (written <= 0 || static_cast<size_t>(written) >= capacity) return false;
  size_t used = static_cast<size_t>(written);
  auto append = [&](const char *format, auto... args) {
    if (used >= capacity) return false;
    // The lambda always receives a string literal at every call site; the
    // non-literal parameter is what makes -Wformat-security fire. Suppress it
    // here only: NOLINT is not honored for compiler diagnostics, and the
    // GCC-style pragma is the form both GCC and clang accept.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
    const int count =
        snprintf(output + used, capacity - used, format, args...);
#pragma GCC diagnostic pop
    if (count <= 0 || static_cast<size_t>(count) >= capacity - used) return false;
    used += static_cast<size_t>(count);
    return true;
  };
  switch (event.type) {
    case WebhookEventType::BREWING:
    case WebhookEventType::IDLE:
      if (!append(",\"state\":\"%s\",\"durationMs\":%lu,"
                  "\"targetWeightG\":%.1f,\"presetId\":%u,\"presetName\":\"%s\","
                  "\"stopDetail\":\"%s\"",
                  event.type == WebhookEventType::BREWING ? "brewing" : "idle",
                  static_cast<unsigned long>(event.durationMs),
                  event.targetWeightG, static_cast<unsigned>(event.presetId),
                  presetName,
                  event.stopDetail)) return false;
      break;
    case WebhookEventType::FIRST_DROP:
      if (!append(",\"firstDropMs\":%lu,\"weightG\":%.2f,"
                  "\"targetWeightG\":%.1f,\"presetId\":%u,\"presetName\":\"%s\"",
                  static_cast<unsigned long>(event.firstDropMs), event.weightG,
                  event.targetWeightG, static_cast<unsigned>(event.presetId),
                  presetName)) {
        return false;
      }
      break;
    case WebhookEventType::END:
      if (!append(",\"durationMs\":%lu,\"targetWeightG\":%.1f,"
                  "\"presetId\":%u,\"presetName\":\"%s\",\"shotType\":\"%s\","
                  "\"stopDetail\":\"%s\"",
                  static_cast<unsigned long>(event.durationMs),
                  event.targetWeightG, static_cast<unsigned>(event.presetId),
                  presetName, event.shotType, event.stopDetail)) return false;
      if (event.firstDropValid &&
          !append(",\"firstDropMs\":%lu",
                  static_cast<unsigned long>(event.firstDropMs))) return false;
      if (event.weightValid && !append(",\"weightG\":%.2f", event.weightG))
        return false;
      if (event.averageFlowValid &&
          !append(",\"averageFlowGps\":%.2f", event.averageFlowGps))
        return false;
      break;
    case WebhookEventType::TEST:
      if (event.correlationId[0] != '\0' &&
          !append(",\"correlationId\":\"%s\"", event.correlationId))
        return false;
      break;
    case WebhookEventType::PRESETS_CHANGED:
      if (!append(",\"activeId\":%u,\"revision\":%lu,\"items\":[",
                  static_cast<unsigned>(event.presetId),
                  static_cast<unsigned long>(event.presetRevision))) return false;
      for (uint8_t i = 0; i < event.presetCount && i < 8; ++i) {
        const WebhookPresetItem &item = event.presets[i];
        char itemName[sizeof(item.name) * 6] = {};
        if (!escapeJsonString(item.name, itemName, sizeof(itemName))) return false;
        if (!append("%s{\"id\":%u,\"name\":\"%s\",\"isFactory\":%s}",
                    i == 0 ? "" : ",", static_cast<unsigned>(item.id),
                    itemName, item.isFactory ? "true" : "false")) return false;
      }
      if (!append("]")) return false;
      break;
    case WebhookEventType::QUICK_SETTINGS_CHANGED: {
      const QuickSettingsSnapshot &quick = event.quickSettings;
      if (!append(",\"revision\":%lu,\"activePresetId\":%u,"
                  "\"brewByWeight\":%s,\"noScaleBbwMode\":\"%s\","
                  "\"autoToManualGuardEnabled\":%s,"
                  "\"slowExtractionGuardEnabled\":%s,"
                  "\"fastExtractionGuardEnabled\":%s,"
                  "\"avoidAccidentalTouchEnabled\":%s,"
                  "\"cupProtectionEnabled\":%s",
                  static_cast<unsigned long>(quick.revision),
                  static_cast<unsigned>(quick.activePresetId),
                  quick.brewByWeight ? "true" : "false",
                  noScaleBbwModeId(quick.noScaleBbwMode),
                  quick.autoToManualGuardEnabled ? "true" : "false",
                  quick.slowExtractionGuardEnabled ? "true" : "false",
                  quick.fastExtractionGuardEnabled ? "true" : "false",
                  quick.avoidAccidentalTouchEnabled ? "true" : "false",
                  quick.cupProtectionEnabled ? "true" : "false")) return false;
      break;
    }
    case WebhookEventType::CONTROLLER_STARTED:
      if (!append(",\"revision\":%lu",
                  static_cast<unsigned long>(event.presetRevision))) return false;
      break;
    case WebhookEventType::ACTIVATION_HISTORY:
      if (!append(",\"id\":%lu,\"type\":\"%s\",\"durationS\":%.1f,"
                  "\"hasWallTime\":%s,\"endedAtUnixSec\":%lu,"
                  "\"endedAtLocalSec\":%lu",
                  static_cast<unsigned long>(event.activationId),
                  historyTypeName(static_cast<HistoryType>(event.activationType)),
                  static_cast<double>(event.durationMs) / 1000.0,
                  event.activationHasWallTime ? "true" : "false",
                  static_cast<unsigned long>(event.activationEndedAtUnixSec),
                  static_cast<unsigned long>(event.activationEndedAtLocalSec)))
        return false;
      break;
    case WebhookEventType::IP_CHANGED: {
      char address[sizeof(event.ipAddress) * 6] = {};
      if (!escapeJsonString(event.ipAddress, address, sizeof(address)))
        return false;
      if (!append(",\"ip\":\"%s\"", address)) return false;
      break;
    }
  }
  return append("}");
}

WebhookDispatcher::SendOutcome WebhookDispatcher::send(
    const QueuedWebhook &queued, uint8_t attempt) {
  WebhookConfig live;
  uint32_t generation = 0;
  mux_.lock();
  live = config_;
  generation = configGeneration_;
  mux_.unlock();
  if (queued.configGeneration != generation) {
    mux_.lock();
    ++status_.dropped;
    ++status_.staleConfigDropped;
    mux_.unlock();
    return SendOutcome::DROPPED;
  }
  const HeapCapSnapshot heapBefore = sampleHeapCaps();
  const WebhookEvent &event = queued.event;
  char endpoint[sizeof(status_.lastEndpoint)] = {};
  formatSafeHttpEndpoint(live.url, endpoint, sizeof(endpoint));
  mux_.lock();
  status_.sending = true;
  status_.lastAttemptAtMs = millis();
  status_.lastHttpStatus = 0;
  status_.lastError = 0;
  strncpy(status_.lastEvent, eventName(event.type),
          sizeof(status_.lastEvent) - 1);
  copyCString(status_.lastEndpoint, sizeof(status_.lastEndpoint), endpoint);
  status_.lastPhase = WebhookRequestPhase::PREPARE;
  status_.lastCancellation = WebhookCancellationReason::NONE;
  beginHeapLifecycle(tlsHeap_, HeapLifecycleEvent::TLS_REQUEST, heapBefore);
  mux_.unlock();

  bool ok = false;
  bool dispatched = false;
  bool interrupted = false;
  WebhookCancellationReason cancellation = WebhookCancellationReason::NONE;
  const uint32_t acquisitionGeneration = outboundAcquisitionGeneration.load(std::memory_order_acquire);
  const auto admitted = [&] {
    return dispatchAllowed() && acquisitionGeneration ==
        outboundAcquisitionGeneration.load(std::memory_order_acquire);
  };
  WebhookRequestPhase phase = WebhookRequestPhase::PREPARE;
  int statusCode = 0;
  [[maybe_unused]] int transportErrno = 0;
  esp_err_t error = ESP_FAIL;
  const bool prepared = validWebhookUrl(live.url) &&
      buildPayload(event, payload_, kWebhookPayloadCapacity);
  if (WiFi.status() == WL_CONNECTED && prepared) {
    phase = WebhookRequestPhase::CLIENT;
    const uint32_t started = millis();
    const bool resolved = resolveHost(live.url, started, admitted);
    esp_http_client_handle_t client =
        resolved ? ensureHttpClient(live.url) : nullptr;
    if (!resolved) error = ESP_ERR_HTTP_CONNECT;
    interrupted = !resolved && !admitted();
    if (client != nullptr) {
      phase = WebhookRequestPhase::CONFIGURE;
      error = static_cast<esp_err_t>(configureWebhookHttpRequest(
          [&]() {
            return static_cast<int32_t>(
                esp_http_client_set_method(client, HTTP_METHOD_POST));
          },
          [&]() {
            return static_cast<int32_t>(esp_http_client_set_post_field(
                client, payload_, strlen(payload_)));
          }));
      if (error == ESP_OK) {
        phase = WebhookRequestPhase::PERFORM;
        error = ESP_ERR_HTTP_EAGAIN;
        requestSent_ = false;
        (void)esp_http_client_set_timeout_ms(client, kWebhookHandshakeWaitMs);
        while (error == ESP_ERR_HTTP_EAGAIN && admitted() &&
               millis() - started < kWebhookTimeoutMs) {
          error = esp_http_client_perform(client);
          if (error == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(1));
        }
        // Connect and DNS failures never reach the receiver: not sent.
        dispatched = requestSent_;
        interrupted = !admitted();
        if (interrupted) error = ESP_ERR_INVALID_STATE;
        else if (error == ESP_ERR_HTTP_EAGAIN ||
                 (error == ESP_OK && millis() - started >= kWebhookTimeoutMs))
          error = ESP_ERR_TIMEOUT;
        // A failed perform() leaves the previous response's status behind.
        if (error == ESP_OK) {
          phase = WebhookRequestPhase::RESPONSE;
          statusCode = esp_http_client_get_status_code(client);
        } else {
          transportErrno = esp_http_client_get_errno(client);
        }
        ok = error == ESP_OK && statusCode >= 200 && statusCode < 300;
      }
      if (!ok) {
        // Keep the allocated client/configuration, but discard any socket/TLS
        // session left by a timeout or protocol failure. A
        // later event can reconnect without repeating handle allocation.
        const esp_err_t closeError = esp_http_client_close(client);
        if (closeError == ESP_OK || closeError == ESP_ERR_INVALID_STATE)
          completeOutboundAcquisitionPause(OutboundClient::WEBHOOK, millis());
        if (closeError != ESP_OK && closeError != ESP_ERR_INVALID_STATE &&
            error == ESP_OK) {
          error = closeError;
        }
        mux_.lock();
        ++status_.transportResets;
        mux_.unlock();
      }
    }
  }

  if (interrupted) {
    cancellation = acquisitionGeneration != outboundAcquisitionGeneration.load() ||
        outboundScaleInhibited() ? WebhookCancellationReason::SCALE_CONNECTING
                                 : WebhookCancellationReason::CONTROL_CRITICAL;
    if (phase == WebhookRequestPhase::CLIENT)
      completeOutboundAcquisitionPause(OutboundClient::WEBHOOK, millis());
  }
  const HeapCapSnapshot heapAfter = sampleHeapCaps();
  // Tests and malformed or rejected requests fail the same way every time.
  // Receivers deduplicate an identical resent payload, so an unknown delivery
  // is retried too.
  const bool permanent = event.type == WebhookEventType::TEST || !prepared ||
      (statusCode >= 400 && statusCode < 500 && statusCode != 408 && statusCode != 429);
  const SendOutcome outcome = ok ? SendOutcome::DELIVERED
      : !dispatched && cancellation != WebhookCancellationReason::NONE ? SendOutcome::HELD
      : permanent || attempt + 1 >= kWebhookAttempts ? SendOutcome::DROPPED
                                                     : SendOutcome::RETRY;
  // Ring + serial, so release images surface failures in Diagnostic → Log.
  if (!ok) {
    serialTraceCategoryf(
        outcome == SendOutcome::DROPPED ? LogLevel::ERROR : LogLevel::WARNING,
        DebugCategory::NETWORK,
        "webhook %s attempt %u/%u failed phase=%s cancel=%s http=%d err=0x%x errno=%d next=%s retryInS=%lu",
        eventName(event.type), attempt + 1U, static_cast<unsigned>(kWebhookAttempts),
        webhookRequestPhaseName(phase), webhookCancellationReasonName(cancellation),
        statusCode, static_cast<unsigned>(error), transportErrno,
        outcome == SendOutcome::RETRY ? "retry"
            : outcome == SendOutcome::HELD ? "held" : "drop",
        outcome == SendOutcome::RETRY
            ? static_cast<unsigned long>(kWebhookRetryDelaysMs[attempt] / 1000) : 0UL);
  } else if (attempt != 0) {
    serialTraceCategoryf(LogLevel::INFO, DebugCategory::NETWORK,
                         "webhook %s delivered on attempt %u/%u",
                         eventName(event.type), attempt + 1U,
                         static_cast<unsigned>(kWebhookAttempts));
  }
  mux_.lock();
  status_.sending = false;
  status_.lastSuccess = ok;
  status_.lastHttpStatus = statusCode > 0 ? static_cast<uint16_t>(statusCode) : 0;
  status_.lastError = static_cast<int32_t>(error);
  status_.lastPhase = phase;
  status_.lastCancellation = cancellation;
  status_.lastDispatched = dispatched;
  status_.lastDeliveryUnknown = !ok && dispatched;
  if (ok) ++status_.sent;
  else if (outcome == SendOutcome::DROPPED) ++status_.dropped;
  ++status_.heapSamples;
  status_.internalFreeBefore = heapBefore.internalFree;
  status_.internalFreeAfter = heapAfter.internalFree;
  status_.internalLargestBefore = heapBefore.internalLargest;
  status_.internalLargestAfter = heapAfter.internalLargest;
  if (status_.internalLargestMinimum == 0 ||
      heapAfter.internalLargest < status_.internalLargestMinimum) {
    status_.internalLargestMinimum = heapAfter.internalLargest;
  }
  status_.psramLargestBefore = heapBefore.psramLargest;
  status_.psramLargestAfter = heapAfter.psramLargest;
  (void)finishHeapLifecycle(tlsHeap_, ok ? HeapLifecycleResult::SUCCESS
                                        : HeapLifecycleResult::FAILURE,
                            heapAfter);
  mux_.unlock();
  return outcome;
}

}  // namespace openbrewbyweight

#endif

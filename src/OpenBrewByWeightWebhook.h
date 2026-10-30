#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <atomic>

#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightHistoryTypes.h"
#include "OpenBrewByWeightIntegrationState.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "OpenBrewByWeightResourceOwner.h"

#if defined(OPEN_BREW_BY_WEIGHT_WEBHOOK_TEST_PLATFORM)
#include OPEN_BREW_BY_WEIGHT_WEBHOOK_TEST_PLATFORM
#elif !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
    !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST)
#include <Arduino.h>
#include <esp_http_client.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace openbrewbyweight {

// The controller's stable LAN identity (its station MAC as text): the
// webhook's `deviceId`, and the companion log store's device key.
void deviceId(char *output, size_t capacity);

constexpr size_t WEBHOOK_URL_CAPACITY = 192;
constexpr uint32_t WEBHOOK_START_MAX_DELAY_MS = 2000;
constexpr uint32_t WEBHOOK_START_MIN_DELAY_MS = 500;

struct WebhookConfig {
  bool enabled = false;
  bool brewState = true;
  bool firstDrop = true;
  bool end = true;
  char url[WEBHOOK_URL_CAPACITY] = {};
  // Deliver in real time by default. Queueing protects the scale link, but
  // defers all shot events until the shot ends.
  bool deferDuringShot = false;
  bool presetChanges = false;
};

inline bool escapeJsonString(const char *input, char *output, size_t capacity) {
  size_t used = 0;
  if (capacity == 0) return false;
  for (size_t i = 0; input != nullptr && input[i] != '\0'; ++i) {
    const unsigned char value = static_cast<unsigned char>(input[i]);
    const char *escape = nullptr;
    char unicode[7] = {};
    if (value == '"') escape = "\\\"";
    else if (value == '\\') escape = "\\\\";
    else if (value == '\b') escape = "\\b";
    else if (value == '\f') escape = "\\f";
    else if (value == '\n') escape = "\\n";
    else if (value == '\r') escape = "\\r";
    else if (value == '\t') escape = "\\t";
    else if (value < 0x20U) {
      snprintf(unicode, sizeof(unicode), "\\u%04x", value);
      escape = unicode;
    }
    const size_t count = escape == nullptr ? 1 : strlen(escape);
    if (used + count >= capacity) return false;
    if (escape == nullptr) output[used++] = static_cast<char>(value);
    else {
      memcpy(output + used, escape, count);
      used += count;
    }
  }
  output[used] = '\0';
  return true;
}

inline bool validWebhookUrl(const char *url) {
  if (url == nullptr) return false;
  const size_t length = strnlen(url, WEBHOOK_URL_CAPACITY);
  if (length < 10 || length >= WEBHOOK_URL_CAPACITY ||
      strncmp(url, "http://", 7) != 0) {
    return false;
  }
  const char *host = url + 7;
  if (*host == '\0' || *host == '/' || strchr(host, '#') != nullptr ||
      strchr(host, '@') != nullptr) {
    return false;
  }
  for (const char *cursor = host; *cursor != '\0'; ++cursor) {
    const unsigned char value = static_cast<unsigned char>(*cursor);
    if (value <= 0x20U || *cursor == '"' || *cursor == '\\') return false;
  }
  const char *authorityEnd = strpbrk(host, "/?");
  if (authorityEnd == nullptr) authorityEnd = url + length;
  if (authorityEnd <= host) return false;

  const char *port = nullptr;
  const char *hostEnd = authorityEnd;
  if (*host == '[') {
    const char *close = static_cast<const char *>(
        memchr(host + 1, ']', static_cast<size_t>(authorityEnd - host - 1)));
    if (close == nullptr || close == host + 1) return false;
    for (const char *cursor = host + 1; cursor < close; ++cursor) {
      if (!(isxdigit(static_cast<unsigned char>(*cursor)) || *cursor == ':' ||
            *cursor == '.')) return false;
    }
    if (close + 1 < authorityEnd) {
      if (close[1] != ':') return false;
      port = close + 2;
    }
  } else {
    const char *colon = static_cast<const char *>(
        memchr(host, ':', static_cast<size_t>(authorityEnd - host)));
    if (colon != nullptr) {
      hostEnd = colon;
      port = colon + 1;
      if (memchr(port, ':', static_cast<size_t>(authorityEnd - port)) != nullptr)
        return false;
    }
    if (hostEnd <= host || *host == '.' || hostEnd[-1] == '.' ||
        *host == '-' || hostEnd[-1] == '-') return false;
    for (const char *cursor = host; cursor < hostEnd; ++cursor) {
      if (!(isalnum(static_cast<unsigned char>(*cursor)) || *cursor == '.' ||
            *cursor == '-' || *cursor == '_')) return false;
    }
  }
  if (port != nullptr) {
    if (port >= authorityEnd) return false;
    uint32_t value = 0;
    for (const char *cursor = port; cursor < authorityEnd; ++cursor) {
      if (!isdigit(static_cast<unsigned char>(*cursor))) return false;
      value = value * 10U + static_cast<uint32_t>(*cursor - '0');
      if (value > 65535U) return false;
    }
    if (value == 0U) return false;
  }
  return true;
}

inline bool validWebhookConfig(const WebhookConfig &config) {
  return config.enabled ? validWebhookUrl(config.url)
                        : config.url[0] == '\0' || validWebhookUrl(config.url);
}

inline bool webhookClientMustRecreate(const char *currentUrl,
                                      const char *nextUrl) {
  return currentUrl == nullptr || nextUrl == nullptr || currentUrl[0] == '\0' ||
         strncmp(currentUrl, nextUrl, WEBHOOK_URL_CAPACITY) != 0;
}

// Keep HTTP client setup fail-fast and host-testable. ESP_OK is zero; the
// concrete callbacks return esp_err_t converted to its stable int32_t ABI.
template <typename MethodSetter, typename BodySetter>
inline int32_t configureWebhookHttpRequest(MethodSetter methodSetter,
                                           BodySetter bodySetter) {
  int32_t error = methodSetter();
  if (error == 0) error = bodySetter();
  return error;
}

enum class WebhookEventType : uint8_t {
  BREWING,
  IDLE,
  FIRST_DROP,
  END,
  TEST,
  PRESETS_CHANGED,
  QUICK_SETTINGS_CHANGED,
  CONTROLLER_STARTED,
  IP_CHANGED,
  // Integration-state event mirroring the newest activation-history record;
  // sent only while the native integration owns the callback.
  ACTIVATION_HISTORY
};

enum class WebhookRequestPhase : uint8_t {
  PREPARE,
  CLIENT,
  CONFIGURE,
  DISPATCH,
  PERFORM,
  RESPONSE
};

enum class WebhookCancellationReason : uint8_t {
  NONE,
  CONTROL_CRITICAL,
  SCALE_CONNECTING
};

inline const char *webhookRequestPhaseName(WebhookRequestPhase phase) {
  switch (phase) {
    case WebhookRequestPhase::PREPARE: return "prepare";
    case WebhookRequestPhase::CLIENT: return "client";
    case WebhookRequestPhase::CONFIGURE: return "configure";
    case WebhookRequestPhase::DISPATCH: return "dispatch";
    case WebhookRequestPhase::PERFORM: return "perform";
    case WebhookRequestPhase::RESPONSE: return "response";
  }
  return "unknown";
}

inline const char *webhookCancellationReasonName(
    WebhookCancellationReason reason) {
  switch (reason) {
    case WebhookCancellationReason::NONE: return "none";
    case WebhookCancellationReason::CONTROL_CRITICAL:
      return "control_critical";
    case WebhookCancellationReason::SCALE_CONNECTING:
      return "scale_connecting";
  }
  return "unknown";
}

struct WebhookPresetItem {
  uint8_t id = 0;
  bool isFactory = false;
  char name[24] = {};
};

struct WebhookEvent {
  WebhookEventType type = WebhookEventType::TEST;
  uint32_t cycleId = 0;
  uint32_t uptimeMs = 0;
  uint32_t unixSec = 0;
  uint32_t bootId = 0;
  uint32_t durationMs = 0;
  // ACTIVATION_HISTORY payload: newest History entry mirrored as-is.
  uint32_t activationId = 0;
  uint32_t activationEndedAtUnixSec = 0;
  uint32_t activationEndedAtLocalSec = 0;
  uint8_t activationType = 0;
  uint8_t activationHasWallTime = 0;
  uint32_t firstDropMs = 0;
  float weightG = 0.0f;
  float targetWeightG = 0.0f;
  float averageFlowGps = 0.0f;
  bool weightValid = false;
  bool firstDropValid = false;
  bool averageFlowValid = false;
  uint8_t presetId = 0;
  char presetName[24] = {};
  char correlationId[65] = {};
  uint32_t presetRevision = 0;
  QuickSettingsSnapshot quickSettings = {};
  uint8_t presetCount = 0;
  WebhookPresetItem presets[8] = {};
  char shotType[16] = {};
  char stopDetail[32] = {};
  char ipAddress[16] = {};
};

struct WebhookStatus {
  bool workerReady = false;
  bool sending = false;
  bool lastSuccess = false;
  bool lastDispatched = false;
  bool lastDeliveryUnknown = false;
  // Snapshot-only: outbound admission currently withholds queued events.
  bool held = false;
  uint8_t queued = 0;
  uint16_t lastHttpStatus = 0;
  int32_t lastError = 0;
  uint32_t lastAttemptAtMs = 0;
  char lastEvent[24] = {};
  char lastEndpoint[160] = {};
  WebhookRequestPhase lastPhase = WebhookRequestPhase::PREPARE;
  WebhookCancellationReason lastCancellation =
      WebhookCancellationReason::NONE;
  uint32_t sent = 0;
  uint32_t dropped = 0;
  uint32_t staleConfigDropped = 0;
  uint32_t workerStarts = 0;
  uint32_t workerStops = 0;
  uint32_t workerStartFailures = 0;
  uint32_t clientCreates = 0;
  uint32_t clientReuses = 0;
  uint32_t transportResets = 0;
  uint32_t clientCleanups = 0;
  uint32_t heapSamples = 0;
  uint32_t internalFreeBefore = 0;
  uint32_t internalFreeAfter = 0;
  uint32_t internalLargestBefore = 0;
  uint32_t internalLargestAfter = 0;
  uint32_t internalLargestMinimum = 0;
  uint32_t psramLargestBefore = 0;
  uint32_t psramLargestAfter = 0;
};

#if defined(OPEN_BREW_BY_WEIGHT_WEBHOOK_TEST_PLATFORM) || \
    (!defined(OPEN_BREW_BY_WEIGHT_HOST_TEST) && \
     !defined(OPEN_BREW_BY_WEIGHT_PERSISTENCE_HOST_TEST))

class WebhookDispatcher {
 public:
  bool begin(const WebhookConfig &config);
  // Stops and joins the worker before releasing lifecycle resources. Safe to
  // call after a partial begin() and more than once.
  bool stop();
  void setConfig(const WebhookConfig &config);
  WebhookConfig config() const;
  WebhookStatus status() const;
  HeapLifecycleAggregate heapTelemetry() const;
  bool enqueue(const WebhookEvent &event);
  // When configured, queued delivery is deferred while control owns the
  // machine. An HTTP request that already started is always allowed to finish.
  // The setter is lock-free so control never waits on webhook lifecycle or
  // network locks.
  void setControlCritical(bool active);

 private:
#if defined(OPEN_BREW_BY_WEIGHT_WEBHOOK_TEST_PLATFORM)
  friend struct WebhookDispatcherTest;
#endif
  struct HttpClientDeleter {
    void operator()(esp_http_client_handle_t client) const {
      (void)esp_http_client_cleanup(client);
    }
  };

  struct QueuedWebhook {
    WebhookEvent event;
    uint32_t configGeneration = 0;
  };

  // HELD: cancelled before dispatch, resent once admitted without counting an
  // attempt. RETRY: failed attempt, resent after the next backoff delay.
  enum class SendOutcome : uint8_t { DELIVERED, HELD, RETRY, DROPPED };
  enum class WorkerState : uint8_t { STOPPED, STARTING, READY, STOPPING };
  enum class HostLookup : uint8_t { IDLE, PENDING, RESOLVED, FAILED };

  bool startWorker();
  void releaseWorkerFromTask();
  static void taskEntry(void *parameter);
  void task();
  SendOutcome send(const QueuedWebhook &queued, uint8_t attempt);
  esp_http_client_handle_t ensureHttpClient(const char *url);
  template <typename Admitted>
  bool resolveHost(const char *url, uint32_t started, Admitted admitted);
  static void lookupStart(void *self);
  static void lookupDone(const char *, const ip_addr_t *address, void *self);
  static esp_err_t httpEvent(esp_http_client_event_t *event);
  void cleanupHttpClient();
  bool buildPayload(const WebhookEvent &event, char *output, size_t capacity);
  bool dispatchAllowed() const;
  // Adds delta to the queued count of the event's full-state kind; returns it.
  int8_t queuedStates(WebhookEventType type, int8_t delta);

  mutable TaskMutex mux_;
  WebhookConfig config_ = {};
  WebhookStatus status_ = {};
  HeapLifecycleTracker tlsHeap_ = {};
  uint32_t configGeneration_ = 1;
  WorkerState workerState_ = WorkerState::STOPPED;
  SemaphoreHandle_t lifecycleMutex_ = nullptr;
  SemaphoreHandle_t workerStopped_ = nullptr;
  bool stopAfterDrain_ = false;
  int8_t queuedStates_[4] = {};
  QueueHandle_t queue_ = nullptr;
  StaticQueue_t queueControl_ = {};
  uint8_t *queueStorage_ = nullptr;
  TaskHandle_t task_ = nullptr;
  char *payload_ = nullptr;
  std::atomic<bool> controlCritical_{false};
  std::atomic<bool> deferDuringShot_{false};
  std::atomic<bool> cleanupRequested_{false};
  // Set by the client event handler once request headers reach the socket.
  bool requestSent_ = false;
  // lwIP's DNS cache warmed off the worker so perform() never blocks in DNS.
  std::atomic<HostLookup> hostLookup_{HostLookup::IDLE};
  char lookupHost_[WEBHOOK_URL_CAPACITY] = {};
  // Worker-owned and reused for every request with the same URL. The owner is
  // a final rollback guard; normal cleanup still runs after join so its result
  // can be published in WebhookStatus.
  UniqueResource<esp_http_client_handle_t, HttpClientDeleter> httpClient_;
  char httpClientUrl_[WEBHOOK_URL_CAPACITY] = {};
};

#endif

}  // namespace openbrewbyweight

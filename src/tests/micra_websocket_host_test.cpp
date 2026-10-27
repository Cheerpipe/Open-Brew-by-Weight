#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>
#include "machine/OpenBrewByWeightMicraPublicIdentityCache.h"
#include "machine/OpenBrewByWeightMicraService.h"
#include "machine/OpenBrewByWeightMicraStomp.h"
#include "OpenBrewByWeightOutboundAdmission.h"
#include "OpenBrewByWeightJsonArena.h"
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightTaskProfiler.h"

static std::atomic<uint64_t> now{1000};
static std::atomic<unsigned> timerReads{0};
uint32_t millis() { return static_cast<uint32_t>(now); }
int64_t esp_timer_get_time() { ++timerReads; return static_cast<int64_t>(now * 1000); }
void xTaskNotifyGive(TaskHandle_t) {}
static bool callback = false, locked = false;
static unsigned stops = 0, destroys = 0;
static std::string transmitted;
static std::string disconnectLog;
void serialTraceCategoryf(openbrewbyweight::LogLevel level, openbrewbyweight::DebugCategory category,
                          const char *format, ...) {
  assert(!locked && (level == openbrewbyweight::LogLevel::WARNING ||
                     level == openbrewbyweight::LogLevel::INFO) &&
         category == openbrewbyweight::DebugCategory::NETWORK);
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  disconnectLog = line;
}
static int muxWaitEvents = 0;
static int32_t lastMuxWaitArgs[2] = {0, 0};
static std::string lastMuxWaitDetail;
// Lock depth across every TaskMutex, kept by the observer tests install, and
// the burn countdown that forces a deterministic >=2 ms acquisition wait.
static int muxObserverDepth = 0;
static int muxBurnMicrosCalls = 0;
static int muxWaitDepthAtEvent = -1;
void addDebugEvent(openbrewbyweight::DebugCategory category,
                   openbrewbyweight::DebugCode code, int32_t argument1,
                   int32_t argument2) {
  assert(!locked && category == openbrewbyweight::DebugCategory::NETWORK);
  if (code == openbrewbyweight::DebugCode::MACHINE_MUX_WAIT) {
    ++muxWaitEvents;
    lastMuxWaitArgs[0] = argument1;
    lastMuxWaitArgs[1] = argument2;
  }
}
void addDebugEventDetail(openbrewbyweight::DebugCategory category,
                         openbrewbyweight::DebugCode code, int32_t argument1,
                         int32_t argument2, const char *detail) {
  assert(!locked && category == openbrewbyweight::DebugCategory::NETWORK);
  if (code == openbrewbyweight::DebugCode::MACHINE_MUX_WAIT) {
    ++muxWaitEvents;
    lastMuxWaitArgs[0] = argument1;
    lastMuxWaitArgs[1] = argument2;
    lastMuxWaitDetail = detail != nullptr ? detail : "";
    muxWaitDepthAtEvent = muxObserverDepth;
  }
}
// Shot trace hooks: received bytes counted outside the facade mutex.
static uint64_t traceRxBytes = 0;
static bool traceSubscribed = false;
void shotTraceNoteMicraRx(uint32_t bytes) {
  assert(!locked);
  traceRxBytes += bytes;
}
void shotTraceNoteMicraSubscribed(bool subscribed) { traceSubscribed = subscribed; }
static int sendLimit = -1;
static bool initFails = false, startFails = false;
static bool sessionFails = false, inhibitAfterSession = false, inhibitAfterInit = false;
static uint32_t stopDelayMs = 2, sessionDelayMs = 0, startDelayMs = 0;
static unsigned initCalls = 0, startCalls = 0;
constexpr int ESP_OK = 0;
constexpr int WEBSOCKET_EVENT_ANY = 0, WEBSOCKET_EVENT_CONNECTED = 1;
constexpr int WEBSOCKET_EVENT_DATA = 2, WEBSOCKET_EVENT_ERROR = 3, WEBSOCKET_EVENT_DISCONNECTED = 4;
constexpr int WEBSOCKET_EVENT_CLOSED = 5;
using esp_websocket_client_handle_t = void *;
struct esp_websocket_event_data_t {
  void *client = nullptr;
  struct {
    int esp_ws_handshake_status_code = 0, esp_tls_last_esp_err = 0;
    int esp_tls_stack_err = 0, esp_tls_cert_verify_flags = 0, esp_transport_sock_errno = 0;
  } error_handle;
  int data_len = 0, payload_len = 0, payload_offset = 0, op_code = 1;
  bool fin = true;
  const char *data_ptr = nullptr;
};
using Handler = void (*)(void *, const char *, int32_t, void *);
struct esp_websocket_client_config_t {
  const char *uri = nullptr, *headers = nullptr;
  int (*crt_bundle_attach)(void *) = nullptr;
  bool disable_auto_reconnect = false, task_core_id_set = false;
  int buffer_size = 0, task_prio = 0, task_stack = 0, task_core_id = -1;
  int network_timeout_ms = 0, ping_interval_sec = 0, pingpong_timeout_sec = 0;
};
int esp_crt_bundle_attach(void *) { return 0; }
int pdMS_TO_TICKS(int value) { return value; }
void taskYIELD() {}
void *esp_websocket_client_init(const esp_websocket_client_config_t *config) {
  assert(!locked && !callback && config->disable_auto_reconnect);
  assert(config->task_core_id == 0 && config->task_prio == 1 && config->buffer_size == 4096);
  assert(config->network_timeout_ms == 10000);
  ++initCalls;
  if (inhibitAfterInit) openbrewbyweight::outboundBleQuiet.store(true);
  return initFails ? nullptr : reinterpret_cast<void *>(1);
}
int esp_websocket_register_events(void *, int, Handler, void *) { assert(!locked); return ESP_OK; }
int esp_websocket_unregister_events(void *, int, Handler) { assert(!locked && !callback); return ESP_OK; }
int esp_websocket_client_start(void *) {
  assert(!locked && !callback);
  ++startCalls;
  now += startDelayMs;
  return startFails ? -1 : ESP_OK;
}
int esp_websocket_client_stop(void *) { assert(!locked && !callback); ++stops; now += stopDelayMs; return ESP_OK; }
int esp_websocket_client_destroy(void *) { assert(!locked && !callback); ++destroys; return ESP_OK; }
int esp_websocket_client_send_text(void *, const char *bytes, int count, int) {
  assert(!locked);
  transmitted.assign(bytes, count);
  return sendLimit < 0 ? count : std::min(sendLimit, count);
}
#include "micra_websocket_work.inc"
namespace openbrewbyweight {
void secureWipe(void *bytes, size_t count) { std::memset(bytes, 0, count); }
bool OpenBrewByWeightMicraService::networkEligible(LineaMicraError &error, bool) const {
  error = LineaMicraError::NONE;
  return true;
}
LineaMicraError OpenBrewByWeightMicraService::classifyFailure() const { return LineaMicraError::TRANSPORT; }
bool OpenBrewByWeightMicraService::ensureWorkBuffer() {
  if (!work_) work_ = new WorkBuffer;
  return true;
}
bool OpenBrewByWeightMicraService::ensureSession(LineaMicraPersistedSettings &, bool, bool *) {
  now += sessionDelayMs;
  if (inhibitAfterSession) outboundBleQuiet.store(true);
  std::strcpy(work_->accessToken, "synthetic-token");
  work_->accessTokenIssuedAtMs = now;
  return !sessionFails;
}
void OpenBrewByWeightMicraService::clearSession() { work_->accessToken[0] = '\0'; }
bool OpenBrewByWeightMicraService::applySignedHeaders(const LineaMicraPersistedSettings &, char *headers, size_t) {
  std::strcpy(headers, "synthetic-signed-upgrade\r\n");
  return true;
}
#include "machine/OpenBrewByWeightMicraWebSocket.inc"
#include "micra_websocket_status.inc"
static unsigned temperatureWrites = 0, powerWrites = 0, dashboardReads = 0;
static std::function<void()> notifyHook;
constexpr int pdTRUE = 1;
unsigned ulTaskNotifyTake(int, int ticks) {
  now += ticks;
  if (notifyHook) notifyHook();
  return 0;
}
static uint16_t cloudTargetDeciC = 0;
static bool cloudIgnoresTemperature = false;
static LineaMicraPowerState cloudPower = LineaMicraPowerState::UNKNOWN;
bool OpenBrewByWeightMicraService::writeTemperature(const LineaMicraPersistedSettings &,
                                                    uint16_t targetDeciC) {
  ++temperatureWrites;
  if (!cloudIgnoresTemperature) cloudTargetDeciC = targetDeciC;
  return true;
}
bool OpenBrewByWeightMicraService::writeStandby(const LineaMicraPersistedSettings &) {
  ++powerWrites;
  cloudPower = LineaMicraPowerState::OFF;
  return true;
}
bool OpenBrewByWeightMicraService::writePowerOn(const LineaMicraPersistedSettings &) {
  ++powerWrites;
  cloudPower = LineaMicraPowerState::ON;
  return true;
}
bool OpenBrewByWeightMicraService::readDashboard(const LineaMicraPersistedSettings &,
    LineaMicraStatus &result, const MicraObservationStamp *) {
  ++dashboardReads;
  result.targetValid = true;
  result.targetDeciC = cloudTargetDeciC;
  result.powerState = cloudPower;
  return true;
}
void OpenBrewByWeightMicraService::waitRetry(uint32_t) {}
#include "micra_websocket_commands.inc"
struct MicraWebSocketTest {
  static void setupAdmissionAndBackoff() {
    for (unsigned gate = 0; gate < 4; ++gate) {
      now = 1000;
      OpenBrewByWeightMicraService service;
      service.config_.accountConfigured = true;
      std::strcpy(service.config_.selectedSerial, "synthetic");
      service.staConnected_.store(true);
      auto &inhibited = gate == 0 ? outboundAcquisitionHeld : gate == 1 ? outboundScaleSetup
          : gate == 2 ? outboundBleQuiet : service.scaleConnecting_;
      inhibited.store(true);
      service.serviceWebSocket();
      assert(!service.websocket_ && !service.work_);
      assert(service.websocketStatus().state == MicraSocketState::PAUSED);
      assert(service.websocketStatus(false).state == MicraSocketState::PAUSED);
      inhibited.store(false);
      service.serviceWebSocket();
      subscribe(service);
      inhibited.store(true);
      service.serviceWebSocket();
      // Established WSS keeps its subscription through scale setup.
      assert(service.websocket_->client && service.websocketStatus().subscribed);
      service.websocket_->failed.store(true);
      service.serviceWebSocket();
      now = service.websocketStatus().retryAtMs;
      service.serviceWebSocket();
      assert(!service.websocket_->client && service.websocket_->attempts == 1);
      assert(service.websocketStatus(false).state == service.websocketStatus().state);
      inhibited.store(false);
      service.serviceWebSocket();
      assert(service.websocket_->client);
      service.stopWebSocket(true);
      delete service.work_;
    }
    for (unsigned phase = 0; phase < 2; ++phase) {
      OpenBrewByWeightMicraService service;
      service.config_.accountConfigured = true;
      service.staConnected_.store(true);
      inhibitAfterSession = phase == 0;
      inhibitAfterInit = phase == 1;
      const auto inits = initCalls, starts = startCalls;
      service.serviceWebSocket();
      assert(initCalls == inits + phase && startCalls == starts);
      assert(service.websocket_ && !service.websocket_->client);
      inhibitAfterSession = inhibitAfterInit = false;
      outboundBleQuiet.store(false);
      service.stopWebSocket(true);
      delete service.work_;
    }
    for (unsigned phase = 0; phase < 3; ++phase) {
      now = UINT32_MAX - 2000ULL;  // Cleanup and backoff cross millis rollover.
      OpenBrewByWeightMicraService service;
      service.config_.accountConfigured = true;
      service.staConnected_.store(true);
      if (phase == 0) {
        service.serviceWebSocket();
        service.websocket_->failed.store(true);
        stopDelayMs = 10000;
      } else if (phase == 1) {
        sessionFails = true;
        sessionDelayMs = 10000;
      } else {
        startFails = true;
        startDelayMs = 10000;
      }
      service.serviceWebSocket();
      assert(service.websocket_ && !service.websocket_->client);
      assert(service.websocketStatus().retryAtMs == millis() + 3000);
      stopDelayMs = 2;
      sessionDelayMs = startDelayMs = 0;
      sessionFails = startFails = false;
      now += 2999;
      service.serviceWebSocket();
      assert(!service.websocket_->client);
      ++now;
      service.serviceWebSocket();
      assert(service.websocket_->client);
      service.stopWebSocket(true);
      delete service.work_;
    }
  }
  static void pongPublicationRace() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.serviceWebSocket();
    subscribe(service);
    static OpenBrewByWeightMicraService *subject;
    static unsigned acquisitions;
    subject = &service;
    acquisitions = 0;
    const auto before = stops;
    TaskMutex::hostObserver = [](const TaskMutex *, bool acquired) {
      if (acquired && ++acquisitions == 3) {
        // Callback publishes after the worker sampled time, before its status copy.
        subject->websocketStatus_.pongAtMs = ++now;
        TaskMutex::hostObserver = nullptr;
      }
    };
    service.serviceWebSocket();
    TaskMutex::hostObserver = nullptr;
    assert(acquisitions == 3 && stops == before && service.websocketStatus().subscribed);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void renewalKeepsSubscription() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.serviceWebSocket();
    subscribe(service);
    const auto stopsBefore = stops;
    const auto initsBefore = initCalls;
    const auto errorsBefore = service.websocketStatus().errors;
    const auto sent = transmitted;
    // A due token renews over HTTP while the same socket stays subscribed.
    now += micra_timing::kAccessTokenRefreshAgeMs;
    deliver(service, "", true, 0xA);
    service.serviceWebSocket();
    assert(stops == stopsBefore && initCalls == initsBefore && transmitted == sent);
    assert(service.websocket_->client && service.websocketStatus().subscribed);
    assert(service.work_->accessTokenIssuedAtMs == now);
    assert(service.websocketStatus().plannedConnections == 1);
    // An obsolete observation epoch still reconnects as planned, with its own reason.
    ++service.observationFence_.epoch;
    service.serviceWebSocket();
    assert(stops == stopsBefore + 1);
    assert(std::strcmp(service.websocketStatus().reason, "obsolete") == 0);
    assert(service.websocketStatus().errors == errorsBefore);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void slowHandshakeIsNotAReconnect() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.serviceWebSocket();
    const auto stopsBefore = stops;
    now += 12000;  // Slow DNS/TLS: the transport is still connecting.
    service.serviceWebSocket();
    assert(stops == stopsBefore && service.websocket_ && service.websocket_->client);
    subscribe(service);
    assert(stops == stopsBefore && service.websocketStatus().unexpectedConnections == 0);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void connectionClassification() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.serviceWebSocket();
    assert(service.websocketStatus().plannedConnections == 0);
    subscribe(service);
    uint32_t planned = 1, unexpected = 0;
    // Every intentional pause and obsolete observation stays planned; shots,
    // scale gates and token renewal keep the open subscription.
    // A firmware update is not a cause: the subscription stays live through OTA.
    for (int cause = 0; cause < 10; ++cause) {
      switch (cause) {
        case 0: service.shotActive_.store(true); outboundScaleConnected.store(true); break;
        case 1: publishOutboundAcquisition(true, ++now); break;
        case 2: outboundBleQuiet.store(true); break;
        case 3: outboundMaintenance.store(true); break;
        case 4: now += micra_timing::kAccessTokenRefreshAgeMs; deliver(service, "", true, 0xA); break;
        case 5:
          deliver(service, "MESS", false);
          now += 10000;
          ++service.observationFence_.epoch;
          break;
        case 6: service.apActive_.store(true); break;
        case 7: service.config_.options &= ~LINEA_MICRA_OBSERVE_STATE; break;
        case 8: service.config_.connectionType = static_cast<uint8_t>(MicraConnectionType::API); break;
        case 9: service.clearSessionRequested_.store(true); break;
      }
      service.serviceWebSocket();
      const bool kept = cause <= 2 || cause == 4;
      assert(!kept || (service.websocketStatus().subscribed && service.websocket_->client));
      service.shotActive_.store(false);
      outboundScaleConnected.store(false);
      publishOutboundAcquisition(false, now);
      outboundBleQuiet.store(false);
      outboundMaintenance.store(false);
      service.apActive_.store(false);
      service.config_.options |= LINEA_MICRA_OBSERVE_STATE;
      service.config_.connectionType = static_cast<uint8_t>(MicraConnectionType::WEBSOCKET);
      service.clearSessionRequested_.store(false);
      service.serviceWebSocket();
      if (!kept) { subscribe(service); ++planned; }
      else assert(service.websocketStatus().subscribed);
      assert(service.websocketStatus().plannedConnections == planned);
      assert(service.websocketStatus().unexpectedConnections == unexpected);
    }
    // Cleaning supervision keeps its pushes through scale acquisition/setup/quiet.
    service.backflush_.observe(MachineBackflushPhase::INACTIVE, 0);
    service.backflush_.observe(MachineBackflushPhase::AWAITING, 0);
    outboundBleQuiet.store(true);
    service.serviceWebSocket();
    assert(service.backflush_.status().busy() && service.websocketStatus().subscribed);
    outboundBleQuiet.store(false);
    service.backflush_.observe(MachineBackflushPhase::INACTIVE, 0);
    // Failures remain unexpected through a maintenance release or radio pause.
    for (int cause = 0; cause < 8; ++cause) {
      switch (cause) {
        case 0: {
          esp_websocket_event_data_t event;
          service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_CLOSED, &event);
          break;
        }
        case 1: deliver(service, std::string("ERROR\n\n") + '\0'); break;
        case 2: service.websocketStatus_.subscribed = false; now += 10000; break;
        case 3: deliver(service, "MESS", false); now += 10000; break;
        case 4: service.staConnected_.store(false); break;
        case 5: case 6:
          service.stopWebSocket();
          initFails = cause == 5;
          startFails = cause == 6;
          break;
        case 7: {
          esp_websocket_event_data_t event;
          event.error_handle.esp_ws_handshake_status_code = 401;
          service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_ERROR, &event);
          break;
        }
      }
      service.serviceWebSocket();
      assert(service.websocketStatus().plannedConnections == planned);
      assert(service.websocketStatus().unexpectedConnections == unexpected);
      outboundMaintenance.store(true);
      service.serviceWebSocket();
      assert(!service.websocket_);
      outboundMaintenance.store(false);
      initFails = startFails = false;
      service.staConnected_.store(true);
      now = std::max(now.load(), uint64_t(service.websocketStatus().retryAtMs));
      service.serviceWebSocket();
      subscribe(service);
      assert(service.websocketStatus().plannedConnections == planned);
      assert(service.websocketStatus().unexpectedConnections == ++unexpected);
    }
    service.stopWebSocket(true);
    delete service.work_;
    service.work_ = nullptr;
  }
  static void lifecycleRegressions() {
    TaskMutex::hostObserver = [](const TaskMutex *, bool acquired) { locked = acquired; };
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.serviceWebSocket();
    subscribe(service);
    const char *off = R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}}]})";
    deliver(service, observation(service, off));
    esp_websocket_event_data_t event;
    event.client = service.websocket_->client;
    callback = true;
    service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_CLOSED, &event);
    callback = false;
    service.serviceWebSocket();
    assert(!service.websocket_->client && service.websocketStatus().state == MicraSocketState::BACKOFF);
    now += 3000;
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, off));
    assert(service.powerState_.notePhysicalStart(service.published_, true, true, now));
    service.stopWebSocket();  // Also used by the HTTP progress cancellation path.
    assert(service.powerState_.retained());
    now += 100000;
    assert(service.powerState_.effectiveStatus(service.published_, true, millis()).effectiveOn);
    service.serviceWebSocket();
    subscribe(service);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
      event.client = service.websocket_->client;
      event.error_handle.esp_ws_handshake_status_code = 401;
      callback = true;
      service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_ERROR, &event);
      if (attempt == 0) {
        event.error_handle.esp_tls_last_esp_err = 32774;
        event.error_handle.esp_tls_stack_err = -29312;
        event.error_handle.esp_tls_cert_verify_flags = 8;
        event.error_handle.esp_transport_sock_errno = 110;
        event.data_ptr = "synthetic-token-and-signed-header";
        service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_DISCONNECTED, &event);
        assert(disconnectLog == "Micra WS disconnected http=401 tls=32774 stack=-29312 verify=8 errno=110");
      }
      callback = false;
      service.serviceWebSocket();
      if (attempt == 0) {
        now = service.websocketStatus().retryAtMs;
        outboundMaintenance.store(true);
        service.serviceWebSocket();
        assert(!service.websocket_);
        outboundMaintenance.store(false);
        service.serviceWebSocket();
      }
    }
    assert(service.websocketStatus().state == MicraSocketState::AUTH_ERROR);
    publishOutboundAcquisition(true, now);
    service.serviceWebSocket();
    publishOutboundAcquisition(false, now);
    now += 60000;
    service.serviceWebSocket();
    assert(service.websocketStatus().state == MicraSocketState::AUTH_ERROR && !service.websocket_->client);
    outboundMaintenance.store(true);
    service.serviceWebSocket();
    assert(!service.websocket_);
    outboundMaintenance.store(false);
    service.serviceWebSocket();
    assert(service.websocketStatus().state == MicraSocketState::AUTH_ERROR && !service.websocket_);
    service.websocketRetryRequested_.store(true);
    service.serviceWebSocket();
    assert(service.websocket_ && service.websocket_->client);
    service.stopWebSocket(true);
    delete service.work_;
    service.work_ = nullptr;
    TaskMutex::hostObserver = nullptr;
  }
  static void deliver(OpenBrewByWeightMicraService &service, const std::string &bytes,
                      bool fin = true, int opcode = 1) {
    esp_websocket_event_data_t message;
    message.client = service.websocket_->client;
    message.data_ptr = bytes.data();
    message.data_len = message.payload_len = static_cast<int>(bytes.size());
    message.fin = fin;
    message.op_code = opcode;
    callback = true;
    const uint64_t traced = traceRxBytes;
    service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_DATA, &message);
    assert(traceRxBytes == traced + (opcode <= 2 ? bytes.size() : 0));
    callback = false;
  }
  static std::string observation(OpenBrewByWeightMicraService &service, const char *body) {
    return std::string("MESSAGE\ndestination:") + service.websocket_->destination +
        "\nsubscription:" + service.websocket_->subscription + "\n\n" + body + std::string(1, '\0');
  }
  static void subscribe(OpenBrewByWeightMicraService &service) {
    esp_websocket_event_data_t message;
    message.client = service.websocket_->client;
    callback = true;
    service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_CONNECTED, &message);
    callback = false;
    assert(transmitted.find("Authorization:Bearer synthetic-token") != std::string::npos);
    assert(service.websocket_->connect[0] == '\0');
    deliver(service, std::string("CONNECTED\nversion:1.2\n\n") + std::string(1, '\0'));
    assert(service.websocketStatus().subscribed && traceSubscribed);
    assert(service.snapshotPending_);
    assert(service.snapshotStamp_.epoch == service.observationFence_.epoch);
    assert(service.snapshotStamp_.connectionRevision == service.observationFence_.connectionRevision);
    assert(transmitted.find(service.websocket_->destination) != std::string::npos);
  }
  static void run() {
    TaskMutex::hostObserver = [](const TaskMutex *, bool acquired) { locked = acquired; };
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.serviceWebSocket();
    assert(service.websocket_ && service.websocket_->client);
    service.websocketStatus_.transmitBytes = UINT32_MAX;
    subscribe(service);
    assert(service.websocketStatus().transmitBytes > UINT32_MAX);
    const char *off = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    deliver(service, observation(service, off));
    assert(service.published_.powerState == LineaMicraPowerState::OFF);
    assert(service.websocketStatus().cleaning == MicraCleaningState::REQUESTED);
    // A relay-only wake keeps the socket and accepts confirming push evidence.
    const auto connectionsBeforeWake = service.websocketStatus().reconnects;
    assert(service.powerState_.notePhysicalStart(service.published_, true, true, ++now));
    service.serviceWebSocket();
    assert(stops == 0 && destroys == 0);
    assert(service.websocketStatus().reconnects == connectionsBeforeWake);
    deliver(service, observation(service, R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})"));
    assert(!service.powerState_.effectiveStatus(service.published_, true, now).optimisticOn);
    // Callback publication does not depend on the HTTP owner's progress.
    service.active_ = true;
    const char *on = R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})";
    // The SDK's 64-bit monotonic clock keeps bins intact across millis rollover.
    const auto onFrame = observation(service, on), offFrame = observation(service, off);
    service.websocketStatus_.receiveBytes = UINT32_MAX;
    now = UINT32_MAX - 400ULL;
    deliver(service, onFrame);
    now += 800;
    deliver(service, offFrame);
    auto rates = service.websocketStatus();
    assert(rates.receiveBytes > UINT32_MAX);
    assert(rates.receivePerSecond == offFrame.size());
    assert(rates.receivePerMinute == onFrame.size() + offFrame.size());
    // Home needs a coherent cleaning snapshot, without reading the rate clock.
    const auto reads = timerReads.load();
    for (unsigned i = 0; i < 40; ++i) {
      const auto home = service.websocketStatus(false);
      assert(home.cleaning == rates.cleaning && home.cleaningAvailable == rates.cleaningAvailable);
      assert(home.receivePerMinute == 0 && home.receivePerSecond == 0);
    }
    assert(timerReads == reads);
    assert(service.websocketStatus().receivePerMinute == rates.receivePerMinute);
    assert(timerReads == reads + 1);
    now += 61000;
    rates = service.websocketStatus();
    assert(rates.receivePerSecond == 0 && rates.receivePerMinute == 0);
    std::thread receiver([&] { deliver(service, observation(service, on)); });
    receiver.join();
    assert(service.published_.powerState == LineaMicraPowerState::ON);
    service.active_ = false;
    const auto fresh = service.published_.sampleAtMs;
    deliver(service, observation(service, R"({"connected":false,"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}}]})"));
    assert(service.published_.sampleAtMs == fresh && service.powerState_.retained());
    // Discovery retains WebSocket observation and accepts fresh frames.
    publishOutboundAcquisition(true, ++now);
    deliver(service, observation(service, off));
    assert(service.published_.sampleAtMs == millis());
    service.websocketStatus_.pongAtMs = millis();
    service.serviceWebSocket();
    assert(stops == 0 && destroys == 0 && service.websocket_->client);
    assert(service.websocketStatus().state == MicraSocketState::STREAMING);
    publishOutboundAcquisition(false, now);
    service.serviceWebSocket();
    deliver(service, observation(service, off));
    assert(!service.powerState_.retained());
    // Unscaled paddle shots retain both the subscription and fresh push evidence.
    const auto stopsBeforeShot = stops;
    const auto epoch = service.observationFence_.epoch;
    outboundShotActive.store(true);
    service.shotActive_.store(true);
    service.inhibitCloud();
    service.serviceWebSocket();
    deliver(service, observation(service, on));
    assert(stops == stopsBeforeShot && service.websocketStatus().subscribed);
    assert(service.observationFence_.epoch == epoch);
    assert(service.status().quality == LineaMicraObservationQuality::CURRENT);
    assert(!service.status().shotPaused && !service.abortRequested_.load());
    // A live scale keeps the subscription through the shot: pushes stay
    // evidence, and nothing is resent or re-read afterwards.
    outboundScaleConnected.store(true);
    service.serviceWebSocket();
    assert(stops == stopsBeforeShot && service.websocketStatus().subscribed);
    const auto sentBeforeShot = transmitted;
    deliver(service, observation(service, off));
    assert(!service.websocket_->failed.load() && service.status().shotPaused);
    assert(service.status().quality == LineaMicraObservationQuality::CURRENT);
    assert(service.published_.powerState == LineaMicraPowerState::OFF);
    outboundScaleConnected.store(false);
    service.shotActive_.store(false);
    outboundShotActive.store(false);
    service.serviceWebSocket();
    assert(stops == stopsBeforeShot && transmitted == sentBeforeShot);
    // A whole shot elapsing between owner turns needs no resync either.
    outboundScaleConnected.store(true);
    outboundShotActive.store(true);
    service.inhibitCloud();
    outboundShotActive.store(false);
    deliver(service, observation(service, on));
    assert(service.published_.powerState == LineaMicraPowerState::ON);
    service.serviceWebSocket();
    assert(stops == stopsBeforeShot && transmitted == sentBeforeShot);
    outboundScaleConnected.store(false);
    deliver(service, observation(service, off));
    assert(!service.powerState_.retained());
    // A fragmented message started before an intent cannot confirm that intent.
    const auto bytes = observation(service, off);
    deliver(service, bytes.substr(0, 15), false);
    assert(service.powerState_.notePhysicalStart(service.published_, true, true, ++now));
    deliver(service, bytes.substr(15), true, 0);
    assert(service.powerState_.effectiveStatus(service.published_, true, now).optimisticOn);
    // Routing and malformed complete documents fail without partial publication.
    const auto before = service.published_.sampleAtMs;
    auto wrong = observation(service, on);
    wrong.replace(wrong.find("/ws/sn/"), 7, "/other/");
    deliver(service, wrong);
    assert(service.websocket_->failed.load() && service.published_.sampleAtMs == before);
    service.stopWebSocket(true);
    assert(!service.websocket_ && service.websocketStatus().retainedBytes == 0);
    service.websocketStatus_.retryAtMs = 0;
    service.serviceWebSocket();
    const auto transmittedBefore = service.websocketStatus().transmitBytes;
    sendLimit = 5;
    esp_websocket_event_data_t partial;
    partial.client = service.websocket_->client;
    callback = true;
    service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_CONNECTED, &partial);
    callback = false;
    assert(service.websocketStatus().transmitBytes == transmittedBefore + 5);
    assert(service.websocket_->failed.load() && service.websocket_->connect[0] == '\0');
    sendLimit = -1;
    outboundMaintenance.store(true);
    service.serviceWebSocket();
    assert(!service.websocket_ && service.websocketStatus().state == MicraSocketState::PAUSED);
    assert(std::strcmp(service.websocketStatus().reason, "maintenance") == 0);
    outboundMaintenance.store(false);
    detail::g_hostAllocationsUntilFailure.store(0);
    service.serviceWebSocket();
    assert(!service.websocket_ && service.websocketStatus().allocationFailures == 1);
    detail::g_hostAllocationsUntilFailure.store(-1);
    delete service.work_;
    service.work_ = nullptr;
    TaskMutex::hostObserver = nullptr;
  }
  static void initialSynchronization() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.published_.connectionFreshness = true;
    service.staConnected_.store(true);
    service.serviceWebSocket();
    subscribe(service);
    const auto firstStamp = service.snapshotStamp_;
    MicraObservation initial;
    initial.stamp = firstStamp;
    initial.source = MicraObservationSource::HTTP_INITIAL;
    initial.powerPresent = true;
    initial.mode = LineaMicraObservedMode::BREWING;
    initial.receivedAtMs = now;
    assert(service.observationFence_.merge(service.published_, service.powerState_, initial, true));
    now += 600000;
    assert(service.status().quality == LineaMicraObservationQuality::CURRENT);
    assert(service.status().powerSource == MicraObservationSource::HTTP_INITIAL);
    outboundAcquisitionHeld.store(true);
    assert(service.status().quality == LineaMicraObservationQuality::CURRENT);
    outboundAcquisitionHeld.store(false);
    deliver(service, observation(service, R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}}]})"));
    assert(service.published_.powerSource == MicraObservationSource::WEBSOCKET);
    assert(service.snapshotStamp_.powerRevision == firstStamp.powerRevision);
    service.snapshotPending_ = false;  // Owner has dispatched initialization.
    deliver(service, observation(service, R"({"connected":false})"));
    assert(service.observationFence_.offline && service.powerState_.retained());
    deliver(service, observation(service, R"({"connected":true})"));
    // Back online without power queues no HTTPS read; the next push resyncs.
    assert(!service.snapshotPending_ && !service.observationFence_.synchronized);
    deliver(service, observation(service, R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})"));
    assert(service.observationFence_.synchronized);
    ++outboundAcquisitionGeneration;
    assert(service.status().quality == LineaMicraObservationQuality::CURRENT);
    esp_websocket_event_data_t event;
    service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_CLOSED, &event);
    assert(!service.observationFence_.synchronized && !service.snapshotPending_);
    assert(service.powerState_.retained());  // Invalidated before owner teardown.
    assert(service.status().quality == LineaMicraObservationQuality::STALE);
    service.stopWebSocket();
    assert(!service.snapshotPending_);
    service.serviceWebSocket();
    subscribe(service);
    assert(service.snapshotStamp_.connectionRevision != firstStamp.connectionRevision);
    assert(service.status().quality == LineaMicraObservationQuality::STALE);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void commandsSkipUnchangedMachineState() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    service.config_.options = LINEA_MICRA_APPLY_TEMPERATURE | LINEA_MICRA_OBSERVE_STATE |
        LINEA_MICRA_POWER_ON_WITH_SCALE | LINEA_MICRA_SHUTDOWN_WITH_SCALE;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.published_.connectionFreshness = true;
    service.staConnected_.store(true);
    service.serviceWebSocket();
    subscribe(service);
    const char *brewing93 = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMCoffeeBoiler","output":{"status":"Ready","targetTemperature":93.0}}]})";
    deliver(service, observation(service, brewing93));
    assert(service.observationFence_.temperatureTrusted && service.observationFence_.powerTrusted);
    uint32_t requestId = 0;
    const auto temperature = [&](uint16_t targetDeciC) {
      LineaMicraRequest request;
      request.type = LineaMicraRequestType::APPLY_TEMPERATURE;
      request.presetId = 1;
      request.targetDeciC = targetDeciC;
      request.requestId = ++requestId;
      service.desiredTemperature_ = {};
      service.desiredTemperature_.request = request;
      service.desiredTemperature_.machineConfigGeneration = service.configGeneration_;
      service.desiredTemperature_.present = true;
      return request;
    };
    const auto power = [&](LineaMicraRequestType type) {
      LineaMicraRequest request;
      request.type = type;
      request.requestId = ++requestId;
      service.desiredPower_ = {};
      service.desiredPower_.request = request;
      service.desiredPower_.machineConfigGeneration = service.configGeneration_;
      service.desiredPower_.shotGeneration = service.shotGeneration_.load();
      service.desiredPower_.present = true;
      return service.executePowerApplication(request, service.configGeneration_);
    };
    // A trusted equal target needs no cloud command; a different one does.
    assert(service.executeTemperatureApplication(temperature(930), service.configGeneration_));
    assert(temperatureWrites == 0 && !service.desiredTemperature_.present);
    assert(service.published_.temperatureState == LineaMicraTemperatureState::CONFIRMED);
    // The push showing the new target confirms it without an HTTPS read.
    const auto readsBefore = dashboardReads;
    notifyHook = [&] {
      notifyHook = nullptr;
      deliver(service, observation(service, R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready","targetTemperature":94.0}}]})"));
    };
    assert(service.executeTemperatureApplication(temperature(940), service.configGeneration_));
    assert(temperatureWrites == 1 && !service.desiredTemperature_.present);
    assert(dashboardReads == readsBefore);
    deliver(service, observation(service, brewing93));
    // A gap in observation removes the trust, so the equal target is written.
    service.observationFence_.invalidate();
    assert(service.executeTemperatureApplication(temperature(930), service.configGeneration_));
    assert(temperatureWrites == 2);
    // Measured power on a live subscription skips a command that changes nothing.
    deliver(service, observation(service, brewing93));
    assert(power(LineaMicraRequestType::SET_POWER_ON));
    assert(powerWrites == 0 && !service.desiredPower_.present);
    assert(power(LineaMicraRequestType::SET_STANDBY) && powerWrites == 1);
    // An offline machine defers a target locally and drops power commands.
    deliver(service, observation(service, R"({"connected":false})"));
    assert(!service.executeTemperatureApplication(temperature(950), service.configGeneration_));
    assert(temperatureWrites == 2 && service.desiredTemperature_.present);
    assert(!power(LineaMicraRequestType::SET_POWER_ON));
    assert(powerWrites == 1 && !service.desiredPower_.present);
    // An accepted command the machine never confirms stops after a few cycles.
    deliver(service, observation(service, R"({"connected":true})"));
    cloudIgnoresTemperature = true;
    const auto unconfirmed = temperature(960);
    for (uint8_t cycle = 1; cycle <= micra_timing::kMaxCommandCycles; ++cycle) {
      assert(!service.executeTemperatureApplication(unconfirmed, service.configGeneration_));
      assert(service.desiredTemperature_.present == (cycle < micra_timing::kMaxCommandCycles));
    }
    assert(temperatureWrites == 3);
    cloudIgnoresTemperature = false;
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushContract() {
    const char *inactive = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    const char *active = R"({"widgets":[{"code":"CMBackFlush","output":{"status":"Cleaning"}}]})";
    for (bool scalePresent : {false, true}) for (int scenario = 0; scenario < 5; ++scenario) {
      outboundScaleConnected.store(scalePresent);
      now = 1000;
      OpenBrewByWeightMicraService service;
      service.config_.accountConfigured = true;
      std::strcpy(service.config_.selectedSerial, "synthetic");
      service.staConnected_.store(true);
      service.tokenAvailable_.store(true);
      service.tokenIssuedAtMs_.store(millis());
      service.serviceWebSocket();
      subscribe(service);
      if (scenario != 1) deliver(service, observation(service, inactive));
      deliver(service, observation(service, awaiting));
      const auto fragmented = observation(service, active);
      if (scenario == 2) deliver(service, fragmented.substr(0, 15), false);
      const auto before = service.backflush(true);
      assert(before.valid && before.ready && before.phase == MachineBackflushPhase::AWAITING);
      service.powerActive_.store(true);
      assert(!service.backflush().ready);
      service.powerActive_.store(false);
      service.tokenIssuedAtMs_.store(now - micra_timing::kAccessTokenRefreshAgeMs + 210000);
      assert(!service.backflush().ready);
      service.tokenIssuedAtMs_.store(now);
      MachineBackflushPermit permit;
      service.active_ = true;  // A parallel API read must survive the gesture.
      const auto stopsBeforeGesture = stops;
      assert(service.physicalStart(&permit) == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE);
      service.serviceWebSocket();
      assert(stops == stopsBeforeGesture && service.websocketStatus().subscribed);
      assert(!service.abortRequested_.load() && service.websocketAdmitted());
      service.active_ = false;
      assert(permit.attempt && permit.extendable == (scenario != 1));
      if (scenario == 2) deliver(service, fragmented.substr(15), true, 0);
      else deliver(service, fragmented);
      auto batch = service.backflush(true);
      assert(batch.valid && batch.count == 1);
      assert((batch.changes[0].permit.attempt == permit.attempt) == (scenario != 2));
      if (scenario == 3) {
        deliver(service, observation(service, R"({"connected":false,"widgets":[{"code":"CMBackFlush","output":{"status":"Cleaning"}}]})"));
        deliver(service, observation(service, R"({"connected":true,"widgets":[]})"));
        batch = service.backflush(true);
        assert(!batch.valid && batch.permit.continuity != permit.continuity);
      } else if (scenario == 4) {
        now += 30000;
        deliver(service, observation(service, R"({"widgets":[]})"));
        assert(!service.backflush().valid);  // Traffic is not a validated pong.
        esp_websocket_event_data_t pong;
        pong.op_code = 0xA;
        service.websocketEvent(&service, nullptr, WEBSOCKET_EVENT_DATA, &pong);
        assert(!service.backflush().valid);  // No retained-data reauthorization.
      } else {
        deliver(service, observation(service, inactive));
        assert(service.backflush(true).phase == MachineBackflushPhase::INACTIVE);
      }
      service.finishBackflush(permit.attempt);
      service.stopWebSocket(true);
      delete service.work_;
    }
    outboundScaleConnected.store(false);
  }
  static void backflushIdlePoll() {
    const char *standby = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}}]})";
    const char *requested = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    const char *inactive = R"({"widgets":[{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    static unsigned locks;
    TaskMutex::hostObserver = [](const TaskMutex *, bool acquired) { locks += acquired; };
    const auto poll = [](OpenBrewByWeightMicraService &service, bool locking) {
      const unsigned before = locks;
      const auto result = service.backflush(true);
      assert((locks != before) == locking);
      const auto reference = service.backflush();  // Locked read; never caches.
      assert(result.valid == reference.valid && result.phase == reference.phase &&
             result.sequence == reference.sequence && result.ready == reference.ready &&
             result.unresolved == reference.unresolved && result.consumed == reference.consumed &&
             result.permit.continuity == reference.permit.continuity &&
             result.permit.attempt == reference.permit.attempt);
      return result;
    };
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    poll(service, true);  // First poll always reads under the lock.
    // The stub clock advances 1 ms per micros() call, so a locked poll owns
    // the mutex for a measurable hold; the shortcut records none.
    assert(micraBackflushHoldLastUsStat().load() > 0);
    assert(micraBackflushHoldMaxUsStat().load() >= micraBackflushHoldLastUsStat().load());
    poll(service, false);
    (void)service.backflush(true);  // The shortcut alone records no hold.
    assert(micraBackflushHoldLastUsStat().load() == 0);
    deliver(service, observation(service, requested));
    assert(poll(service, true).count == 1);  // Consumed exactly once.
    assert(poll(service, true).count == 0);  // Valid state keeps qualifying.
    deliver(service, observation(service, standby));
    assert(poll(service, true).unresolved);  // Busy never takes the shortcut.
    poll(service, true);
    deliver(service, observation(service, inactive));
    poll(service, true);
    deliver(service, observation(service, standby));
    assert(!poll(service, true).valid);
    poll(service, false);
    poll(service, false);
    {
      TaskLockGuard lock(service.mux_);
      service.backflush_.finish(0);  // Any mutation re-arms the locked read.
    }
    poll(service, true);
    poll(service, false);
    // A StandBy dashboard that still carries the cleaning widget keeps the
    // handoff valid; the shortcut engages on power OFF regardless, and the
    // INACTIVE provenance survives for the next request after power ON.
    const char *standbyOff = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}},{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    const char *brewing = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})";
    deliver(service, observation(service, standbyOff));
    assert(poll(service, true).count == 1);
    assert(poll(service, true).valid);  // Idle read settles valid while OFF.
    poll(service, false);
    deliver(service, observation(service, brewing));
    poll(service, true);  // Power ON re-arms without a handoff mutation.
    poll(service, true);  // Valid while ON never shortcuts.
    deliver(service, observation(service, requested));
    const auto armed = poll(service, true);
    assert(armed.count == 1 && armed.permit.extendable);
    TaskMutex::hostObserver = nullptr;
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void concurrentStatusLifecycle() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    service.published_.connectionFreshness = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    std::atomic<bool> done{false};
    std::atomic<unsigned> samples{0};
    std::thread reader([&] {
      while (!done.load()) {
        (void)service.backflush_.ingressAttempt();
        const auto state = service.status();
        assert(state.powerState != LineaMicraPowerState::OFF);
        // The control task's non-blocking poll races the same lifecycle
        // churn; its single-reader cache must never tear into a phantom OFF.
        const auto polled = service.statusPoll();
        assert(polled.powerState != LineaMicraPowerState::OFF);
        (void)service.websocketStatus();
        ++samples;
      }
    });
    while (!samples.load()) std::this_thread::yield();
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
      {
        TaskLockGuard lock(service.mux_);
        const auto permit = service.backflush_.start();
        service.backflush_.finish(permit.attempt);
      }
      service.serviceWebSocket();
      subscribe(service);
      deliver(service, observation(service, R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})"));
      service.stopWebSocket(true);
    }
    done.store(true);
    reader.join();
    delete service.work_;
  }
  static void backflushWaitAttribution() {
    now = 20000;  // The rate-limit window measures from a zero-initialized latch.
    muxWaitEvents = 0;
    disconnectLog.clear();
    lastMuxWaitDetail.clear();
    OpenBrewByWeightMicraService service;
    // With the stub clock each micros() call is 1 ms, so an uncontended
    // acquisition waits ~1 ms and stays below the 2 ms report bar.
    service.backflush(false);
    assert(muxWaitEvents == 0);
    assert(disconnectLog.empty());
    uint32_t latch = 0;
    // Just below the 2 ms threshold: not due.
    assert(!OpenBrewByWeightMicraService::muxWaitAnomalyDue(1999, latch, now));
    assert(muxWaitEvents == 0);
    // At and past the threshold the report names the gathered holder and
    // carries the wait as the ring event's first argument; the audit excerpt
    // keeps the holder for the detail view.
    assert(OpenBrewByWeightMicraService::muxWaitAnomalyDue(31000, latch, now));
    OpenBrewByWeightMicraService::reportMuxWaitAnomaly(31000, "host", 0);
    assert(muxWaitEvents == 1);
    assert(lastMuxWaitArgs[0] == 31000 && lastMuxWaitArgs[1] == 0);
    assert(lastMuxWaitDetail == "holder=host");
    assert(disconnectLog.find("Micra mux wait=31000 us") != std::string::npos);
    assert(disconnectLog.find("holder=host") != std::string::npos);
    // Rate limit: a second anomaly inside the 10 s window stays due-false...
    now += 100;
    assert(!OpenBrewByWeightMicraService::muxWaitAnomalyDue(31000, latch, now));
    assert(muxWaitEvents == 1);
    // ...and clears once the window has passed. The re-fire waits exactly at
    // the threshold, probing the boundary after the 1999 probe below it.
    now += 10000;
    assert(OpenBrewByWeightMicraService::muxWaitAnomalyDue(2000, latch, now));
    OpenBrewByWeightMicraService::reportMuxWaitAnomaly(2000, "host", 0);
    assert(muxWaitEvents == 2);
    assert(lastMuxWaitArgs[0] == 2000);
  }
  static void statusPollCache() {
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    service.published_.connectionFreshness = true;
    service.published_.sampleAtMs = now;
    service.published_.powerState = LineaMicraPowerState::OFF;
    service.published_.effectiveOn = false;
    service.staConnected_.store(true);
    // Before any successful poll the cache counts as unknown — effectiveOn,
    // Disabled quality — and this late on the stub clock it is already stale.
    assert(service.statusPollStale());
    const auto &boot = service.statusCache_;
    assert(boot.effectiveOn &&
           boot.quality == LineaMicraObservationQuality::Disabled);
    // Uncontended, the poll serves exactly the locked status() resolution;
    // the trivially copyable snapshot must match field for field, so a
    // never-refreshed cache cannot pass as a fresh poll.
    const auto fresh = service.statusPoll();
    const auto reference = service.status();
    assert(fresh.powerState == LineaMicraPowerState::OFF && !fresh.effectiveOn);
    assert(std::memcmp(&fresh, &reference, sizeof(fresh)) == 0);
    assert(!service.statusPollStale());
    // A mutex held by another task serves the previous cache — never torn,
    // never the publication that task made under the lock — and misses age it.
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      service.published_.powerState = LineaMicraPowerState::ON;
      service.published_.effectiveOn = true;
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto served = service.statusPoll();
    assert(served.powerState == LineaMicraPowerState::OFF && !served.effectiveOn);
    assert(std::memcmp(&served, &fresh, sizeof(served)) == 0);
    now += 249;
    assert(!service.statusPollStale());
    ++now;
    assert(service.statusPollStale());
    // Staleness is a gate, not a payload change: a poll deep inside the
    // stale window still serves the same snapshot unchanged.
    const auto stillServed = service.statusPoll();
    assert(std::memcmp(&stillServed, &fresh, sizeof(stillServed)) == 0);
    release.store(true);
    holder.join();
    const auto refreshed = service.statusPoll();
    assert(refreshed.powerState == LineaMicraPowerState::ON &&
           refreshed.effectiveOn);
    assert(!service.statusPollStale());
    // The age subtraction is unsigned-wraparound-safe: a cache stamped just
    // before the ~49.7-day millis() rollover ages across the seam.
    now = UINT32_MAX - 100ULL;
    const auto wrapped = service.statusPoll();
    assert(wrapped.powerState == LineaMicraPowerState::ON && wrapped.effectiveOn);
    assert(!service.statusPollStale());
    now = 148;  // stamp + 249 across the wrap: one ms short of the bound.
    assert(!service.statusPollStale());
    ++now;
    assert(service.statusPollStale());
  }
  // Forces a deterministic >=2 ms acquisition wait (each micros() call is
  // 1 ms on the stub clock) on the next armed acquire, while keeping the
  // lock depth the mux-wait assertions read at event time.
  static void armMuxWaitBurn(int calls) {
    muxBurnMicrosCalls = calls;
    TaskMutex::hostObserver = [](const TaskMutex *, bool acquired) {
      muxObserverDepth += acquired ? 1 : -1;
      if (acquired && muxBurnMicrosCalls > 0) {
        while (muxBurnMicrosCalls-- > 0) (void)micros();
      }
    };
  }
  static void backflushForcedMissDuringEpisode() {
    const char *inactive = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    const char *active = R"({"widgets":[{"code":"CMBackFlush","output":{"status":"Cleaning"}}]})";
    muxWaitEvents = 0;
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, inactive));
    deliver(service, observation(service, awaiting));
    (void)service.backflush(true);  // Consume the AWAITING edge under the lock.
    MachineBackflushPermit permit;
    service.active_ = true;
    assert(service.physicalStart(&permit) == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE);
    service.active_ = false;
    deliver(service, observation(service, active));
    const auto batch = service.backflush(true);
    assert(batch.valid && batch.count == 1 && batch.busy());
    // A mutex held by another task serves the last locked snapshot with no
    // new edges; repeated misses are stable and never re-serve drained ones.
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto first = service.backflush(true);
    const auto second = service.backflush(true);
    release.store(true);
    holder.join();
    assert(first.valid && first.count == 0 && first.busy() == batch.busy());
    assert(first.busy() && first.phase == batch.phase &&
           first.permit.continuity == batch.permit.continuity &&
           first.permit.episode == batch.permit.episode &&
           first.permit.attempt == batch.permit.attempt &&
           first.permit.sequence == batch.permit.sequence);
    assert(second.valid && second.count == 0 && second.busy() == first.busy());
    assert(second.permit.continuity == first.permit.continuity &&
           second.permit.episode == first.permit.episode &&
           second.permit.attempt == first.permit.attempt &&
           second.permit.sequence == first.permit.sequence &&
           second.phase == first.phase);
    assert(muxWaitEvents == 0);  // The detector no longer lives in backflush().
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushForcedMissAfterConsume() {
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, awaiting));
    const auto batch = service.backflush(true);  // Drains the AWAITING edge.
    assert(batch.valid && batch.count == 1);
    // The holder publishes the next edge under the lock, as the cloud
    // callback would; the miss must not observe or re-serve it.
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      service.backflush_.observe(MachineBackflushPhase::ACTIVE, 0);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto missed = service.backflush(true);
    assert(missed.count == 0 && missed.sequence == batch.sequence);
    release.store(true);
    holder.join();
    // After release the accumulated edge arrives exactly once, sequenced.
    const auto next = service.backflush(true);
    assert(next.valid && next.count == 1);
    assert(next.changes[0].sequence == batch.sequence + 1);
    assert(service.backflush(true).count == 0);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushMissNeverServesIdleSnapshot() {
    const char *standbyOff = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}},{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    const char *brewing = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})";
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    const char *active = R"({"widgets":[{"code":"CMBackFlush","output":{"status":"Cleaning"}}]})";
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, standbyOff));
    const auto drained = service.backflush(true);  // Drains the INACTIVE edge.
    assert(drained.valid && drained.phase == MachineBackflushPhase::INACTIVE &&
           drained.count == 1);
    const auto idle = service.backflush(true);  // Settles the idle handoff.
    assert(idle.valid && idle.phase == MachineBackflushPhase::INACTIVE);
    deliver(service, observation(service, brewing));
    deliver(service, observation(service, awaiting));
    (void)service.backflush(true);
    MachineBackflushPermit permit;
    service.active_ = true;
    assert(service.physicalStart(&permit) == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE);
    service.active_ = false;
    deliver(service, observation(service, active));
    const auto batch = service.backflush(true);
    assert(batch.valid && batch.busy());
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto missed = service.backflush(true);
    release.store(true);
    holder.join();
    // Mid-episode the miss is the live snapshot, never the stale idle one.
    assert(service.idleBackflush_.phase == MachineBackflushPhase::INACTIVE);
    assert(missed.valid && missed.phase != MachineBackflushPhase::INACTIVE);
    assert(missed.phase == batch.phase && missed.count == 0);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushMissSurvivesLongHold() {
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    const char *active = R"({"widgets":[{"code":"CMBackFlush","output":{"status":"Cleaning"}}]})";
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, awaiting));
    (void)service.backflush(true);
    MachineBackflushPermit permit;
    service.active_ = true;
    assert(service.physicalStart(&permit) == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE);
    service.active_ = false;
    deliver(service, observation(service, active));
    const auto batch = service.backflush(true);
    assert(batch.valid && batch.busy());
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto beforeAging = service.backflush(true);
    now += 60000;  // Far past every freshness window a locked poll would
                   // qualify against; the miss path never qualifies.
    const auto afterAging = service.backflush(true);
    release.store(true);
    holder.join();
    assert(afterAging.valid && afterAging.count == 0);
    assert(std::memcmp(&beforeAging, &afterAging, sizeof(afterAging)) == 0);
    // The first uncontended poll after release does qualify and invalidates.
    assert(!service.backflush(true).valid);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushDetectorSilentAfterConversion() {
    muxWaitEvents = 0;
    disconnectLog.clear();
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    (void)service.backflush();
    (void)service.backflush(true);
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    (void)service.backflush();
    (void)service.backflush(true);
    release.store(true);
    holder.join();
    assert(muxWaitEvents == 0);
    assert(disconnectLog.empty());
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void queueMuxWaitDetectorFires() {
    now = 20000;  // Past the zero-initialized latch's rate-limit window.
    muxWaitEvents = 0;
    disconnectLog.clear();
    lastMuxWaitDetail.clear();
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.config_.options |= LINEA_MICRA_SHUTDOWN_WITH_SCALE;
    service.task_ = reinterpret_cast<TaskHandle_t>(1);
    LineaMicraRequest request;
    request.type = LineaMicraRequestType::SET_STANDBY;
    // Uncontended (~1 ms on the stub clock): accepted, no report.
    armMuxWaitBurn(0);
    assert(service.queue(request));
    assert(muxWaitEvents == 0 && disconnectLog.empty());
    // One burned micros() call lands the wait exactly at the 2 ms bar.
    armMuxWaitBurn(1);
    assert(service.queue(request));
    assert(muxWaitEvents == 1);
    assert(lastMuxWaitArgs[0] == 2000 && lastMuxWaitArgs[1] == 0);
    assert(lastMuxWaitDetail == "holder=host");
    assert(disconnectLog.find("Micra mux wait=2000 us") != std::string::npos);
    assert(disconnectLog.find("holder=host") != std::string::npos);
    // Rate limit: a second forced wait inside the 10 s window stays silent.
    armMuxWaitBurn(1);
    assert(service.queue(request));
    assert(muxWaitEvents == 1);
    // ...and fires again once the window has passed.
    now += 10000;
    armMuxWaitBurn(1);
    assert(service.queue(request));
    assert(muxWaitEvents == 2 && lastMuxWaitArgs[0] == 2000);
    TaskMutex::hostObserver = nullptr;
  }
  template <typename Body>
  static void withMuxHeld(OpenBrewByWeightMicraService &service, Body body) {
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      service.mux_.lock();
      held.store(true);
      while (!release.load()) std::this_thread::yield();
      service.mux_.unlock();
    });
    while (!held.load()) std::this_thread::yield();
    body();
    release.store(true);
    holder.join();
  }
  static void loopEntryPointsNeverWaitOnMux() {
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.config_.options |= LINEA_MICRA_APPLY_TEMPERATURE;
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, awaiting));
    assert(service.backflush(true).busy());
    service.task_ = reinterpret_cast<TaskHandle_t>(1);
    LineaMicraRequest request;
    request.type = LineaMicraRequestType::APPLY_TEMPERATURE;
    request.presetId = 1;
    request.targetDeciC = LINEA_MICRA_BREW_TARGET_MIN_DECI_C;
    withMuxHeld(service, [&] {
      // A busy cleaning episode refuses the contended edge with no permit,
      // still fencing in-flight bytes like a locked evaluation.
      const uint32_t ingress = service.backflush_.ingressAttempt();
      MachineBackflushPermit permit;
      permit.attempt = 7;
      assert(service.physicalStart(&permit) ==
             MachinePhysicalStartDisposition::BLOCKED_CLEANING);
      assert(permit.attempt == 0 && service.backflush_.ingressAttempt() != ingress);
      service.post(request);
      service.finishBackflush(5);
      // A permit-less release (after a refused edge) only fences and must
      // not displace the pending release.
      const uint32_t fenced = service.backflush_.ingressAttempt();
      service.finishBackflush(0);
      assert(service.backflush_.ingressAttempt() != fenced);
      assert(service.postedTemperaturePresent_ && service.postedFinishAttempt_ == 5);
      assert(!service.desiredTemperature_.present);
      // Contended polls serve the last resolution and keep the deferral.
      assert(service.backflush(true).busy() && service.postedPending());
    });
    // The next uncontended poll applies the deferred mutations first.
    (void)service.backflush(true);
    assert(!service.postedPending() && service.desiredTemperature_.present);
    service.stopWebSocket(true);
    delete service.work_;

    // Without a busy episode a contended edge is a normal start.
    OpenBrewByWeightMicraService idle;
    withMuxHeld(idle, [&] {
      MachineBackflushPermit permit;
      assert(idle.physicalStart(&permit) == MachinePhysicalStartDisposition::NORMAL);
      assert(permit.attempt == 0);
    });
  }
  static void muxWaitReportEmittedAfterUnlock() {
    now = 50000;  // Clears the queue latch's rate-limit window.
    muxWaitEvents = 0;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    service.config_.options |= LINEA_MICRA_SHUTDOWN_WITH_SCALE;
    service.task_ = reinterpret_cast<TaskHandle_t>(1);
    LineaMicraRequest request;
    request.type = LineaMicraRequestType::SET_STANDBY;
    armMuxWaitBurn(1);
    assert(service.queue(request));
    assert(muxWaitEvents == 1 && muxWaitDepthAtEvent == 0);
    TaskMutex::hostObserver = nullptr;
  }
  static void backflushConsumeFastPathUnchanged() {
    const char *standbyOff = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}},{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    static unsigned locks;
    TaskMutex::hostObserver = [](const TaskMutex *, bool acquired) { locks += acquired; };
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, standbyOff));
    assert(service.backflush(true).count == 1);  // Drain the INACTIVE edge.
    const auto settled = service.backflush(true);  // Latches the idle handoff.
    assert(settled.valid && settled.phase == MachineBackflushPhase::INACTIVE &&
           settled.count == 0);
    {
      // Even with mux_ held the shortcut neither locks nor waits.
      TaskLockGuard lock(service.mux_);
      const unsigned before = locks;
      const auto served = service.backflush(true);
      assert(locks == before);
      assert(std::memcmp(&served, &service.idleBackflush_, sizeof(served)) == 0);
      assert(served.valid && served.phase == MachineBackflushPhase::INACTIVE);
    }
    TaskMutex::hostObserver = nullptr;
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushMissAfterAdmission() {
    const char *inactive = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Off"}}]})";
    const char *awaiting = R"({"connected":true,"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMBackFlush","output":{"status":"Requested"}}]})";
    now = 1000;
    OpenBrewByWeightMicraService service;
    service.config_.accountConfigured = true;
    std::strcpy(service.config_.selectedSerial, "synthetic");
    service.staConnected_.store(true);
    service.tokenAvailable_.store(true);
    service.tokenIssuedAtMs_.store(millis());
    service.serviceWebSocket();
    subscribe(service);
    deliver(service, observation(service, inactive));
    deliver(service, observation(service, awaiting));
    (void)service.backflush(true);  // The cached poll still shows attempt 0.
    assert(service.lastPollSnapshot_.valid &&
           service.lastPollSnapshot_.permit.attempt == 0);
    MachineBackflushPermit permit;
    service.active_ = true;
    assert(service.physicalStart(&permit) == MachinePhysicalStartDisposition::BACKFLUSH_CANDIDATE);
    service.active_ = false;
    assert(permit.attempt != 0);
    // The admission-following poll (WakeGesture calls it consume=false) may
    // miss; without the admission-time cache write that miss would fail
    // backflushProvenanceCurrent and the next supervision tick would abort
    // with supervision_lost.
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto missed = service.backflush();
    release.store(true);
    holder.join();
    assert(missed.valid && missed.count == 0);
    assert(missed.phase == MachineBackflushPhase::AWAITING);
    assert(missed.permit.continuity == permit.continuity &&
           missed.permit.episode == permit.episode &&
           missed.permit.attempt == permit.attempt &&
           missed.permit.sequence == permit.sequence);
    service.stopWebSocket(true);
    delete service.work_;
  }
  static void backflushMissBeforeFirstPoll() {
    // A miss before any successful poll serves the untouched cache: invalid,
    // not busy, in both consume modes — the same fail-closed snapshot an
    // uncontended poll of an empty ring produces. The miss never stamps it.
    now = 1000;
    OpenBrewByWeightMicraService service;
    std::atomic<bool> held{false}, release{false};
    std::thread holder([&] {
      TaskLockGuard lock(service.mux_);
      held.store(true);
      while (!release.load()) std::this_thread::yield();
    });
    while (!held.load()) std::this_thread::yield();
    const auto missed = service.backflush(true);
    const auto missedIdle = service.backflush();
    release.store(true);
    holder.join();
    assert(!missed.valid && !missed.busy() && missed.count == 0);
    assert(missed.phase == MachineBackflushPhase::UNSUPPORTED);
    assert(std::memcmp(&missed, &missedIdle, sizeof(missed)) == 0);
    assert(!service.lastPollSnapshot_.valid);
  }
};
}
int main() {
  openbrewbyweight::initJsonParser();
  openbrewbyweight::MicraWebSocketTest::run();
  openbrewbyweight::MicraWebSocketTest::lifecycleRegressions();
  openbrewbyweight::MicraWebSocketTest::connectionClassification();
  openbrewbyweight::MicraWebSocketTest::slowHandshakeIsNotAReconnect();
  openbrewbyweight::MicraWebSocketTest::renewalKeepsSubscription();
  openbrewbyweight::MicraWebSocketTest::initialSynchronization();
  openbrewbyweight::MicraWebSocketTest::commandsSkipUnchangedMachineState();
  openbrewbyweight::MicraWebSocketTest::backflushContract();
  openbrewbyweight::MicraWebSocketTest::backflushIdlePoll();
  openbrewbyweight::MicraWebSocketTest::pongPublicationRace();
  openbrewbyweight::MicraWebSocketTest::concurrentStatusLifecycle();
  openbrewbyweight::MicraWebSocketTest::setupAdmissionAndBackoff();
  openbrewbyweight::MicraWebSocketTest::backflushWaitAttribution();
  openbrewbyweight::MicraWebSocketTest::statusPollCache();
  openbrewbyweight::MicraWebSocketTest::backflushForcedMissDuringEpisode();
  openbrewbyweight::MicraWebSocketTest::backflushForcedMissAfterConsume();
  openbrewbyweight::MicraWebSocketTest::backflushMissNeverServesIdleSnapshot();
  openbrewbyweight::MicraWebSocketTest::backflushMissSurvivesLongHold();
  openbrewbyweight::MicraWebSocketTest::backflushDetectorSilentAfterConversion();
  openbrewbyweight::MicraWebSocketTest::queueMuxWaitDetectorFires();
  openbrewbyweight::MicraWebSocketTest::loopEntryPointsNeverWaitOnMux();
  openbrewbyweight::MicraWebSocketTest::muxWaitReportEmittedAfterUnlock();
  openbrewbyweight::MicraWebSocketTest::backflushConsumeFastPathUnchanged();
  openbrewbyweight::MicraWebSocketTest::backflushMissAfterAdmission();
  openbrewbyweight::MicraWebSocketTest::backflushMissBeforeFirstPoll();
}

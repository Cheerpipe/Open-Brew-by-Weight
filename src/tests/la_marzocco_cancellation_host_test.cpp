#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include "machine/OpenBrewByWeightLaMarzoccoPublicIdentityCache.h"
#include "machine/OpenBrewByWeightLaMarzoccoService.h"
#include "la_marzocco_power_test_helpers.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightOutboundAdmission.h"

// Only scheduling and transport are simulated; extracted service code is unchanged.
static uint32_t testNowMs = 1000, performAdvanceMs = 0, lastWaitTicks = 0;
uint32_t millis() { return testNowMs; }
void xTaskNotifyGive(TaskHandle_t) {}
// The queue() mux-wait detector's emit path; uncontended waits never reach it.
// It also records the unrecognized cleaning notice.
static std::string cleaningTrace;
static unsigned cleaningTraces = 0;
void serialTraceCategoryf(openbrewbyweight::LogLevel, openbrewbyweight::DebugCategory,
                          const char *format, ...) {
  char line[160];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (!std::strstr(line, "cleaning status")) return;
  cleaningTrace = line;
  ++cleaningTraces;
}
void addDebugEventDetail(openbrewbyweight::DebugCategory, openbrewbyweight::DebugCode,
                         int32_t, int32_t, const char *) {}
void shotTraceNoteLaMarzoccoRx(uint32_t) {}
void shotTraceNoteLaMarzoccoSubscribed(bool) {}
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_HTTP_EAGAIN = 1;
constexpr int ESP_ERR_HTTP_READ_TIMEOUT = 2, pdTRUE = 1;
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
struct WorkerStopped {};
static std::function<int()> performHook;
static std::function<void()> executeHook;
static bool initialSnapshot = false;
static openbrewbyweight::LaMarzoccoObservationStamp dispatchedStamp;
static std::function<bool(const openbrewbyweight::LaMarzoccoObservationStamp *)> dashboardHook;
static unsigned authentications = 0, retries = 0, failures = 0;
static unsigned waits = 0;
static unsigned websocketStops = 0;
static bool lastRegisterKey = false;
static std::function<bool(openbrewbyweight::LaMarzoccoDiscoverySnapshot &)> listHook;
static std::function<void()> notifyTakeHook;
int esp_http_client_perform(void *) {
  const int result = performHook();
  testNowMs += performAdvanceMs;
  return result;
}
void esp_http_client_cleanup(void *) {}
unsigned ulTaskNotifyTake(int, uint32_t ticks) {
  if (ticks == 250) throw WorkerStopped{};
  lastWaitTicks = ticks;
  ++waits;
  if (notifyTakeHook) notifyTakeHook();
  return 0;
}
namespace openbrewbyweight {
bool firmwareCompatibilityMode() { return false; }
int testGettimeofday(timeval *now, void *) { now->tv_sec = 1700000010L; return 0; }
void OpenBrewByWeightLaMarzoccoService::releaseIoBuffer(bool) {}
void OpenBrewByWeightLaMarzoccoService::releaseWorkBuffer() { clearSession(); }
void OpenBrewByWeightLaMarzoccoService::serviceWebSocket() {}
void OpenBrewByWeightLaMarzoccoService::stopWebSocket(bool) { ++websocketStops; }
bool OpenBrewByWeightLaMarzoccoService::ensureSession(const LaMarzoccoPersistedSettings &, bool registerKey,
                                                  bool *renewed) {
  ++authentications;
  lastRegisterKey = registerKey;
  if (renewed != nullptr) *renewed = true;
  return true;
}
bool OpenBrewByWeightLaMarzoccoService::ensureWorkBuffer() { return true; }
bool OpenBrewByWeightLaMarzoccoService::listMachines(const LaMarzoccoPersistedSettings &,
    LaMarzoccoDiscoverySnapshot &found) { return listHook(found); }
bool OpenBrewByWeightLaMarzoccoService::readDashboard(const LaMarzoccoPersistedSettings &,
    LaMarzoccoStatus &, const LaMarzoccoObservationStamp *snapshot) { return dashboardHook(snapshot); }
void OpenBrewByWeightLaMarzoccoService::waitRetry(uint32_t) { ++retries; }
static bool liveSubscription = false;
bool OpenBrewByWeightLaMarzoccoService::liveSubscriptionLocked() const { return liveSubscription; }
LaMarzoccoError OpenBrewByWeightLaMarzoccoService::classifyFailure() const { return LaMarzoccoError::HTTP_ERROR; }
void OpenBrewByWeightLaMarzoccoService::fail(LaMarzoccoStatus &, LaMarzoccoError) { ++failures; }
void OpenBrewByWeightLaMarzoccoService::execute(PendingRequest &pending) {
  initialSnapshot = pending.initialSnapshot;
  dispatchedStamp = pending.snapshotStamp;
  executeHook();
}
bool OpenBrewByWeightLaMarzoccoService::executeTemperatureApplication(
    const LaMarzoccoRequest &, uint32_t) {
  executeHook();
  desiredTemperature_.present = false;
  return true;
}
bool OpenBrewByWeightLaMarzoccoService::executePowerApplication(
    const LaMarzoccoRequest &, uint32_t) {
  executeHook();
  desiredPower_.present = false;
  return true;
}
}
#define gettimeofday testGettimeofday
#include "la_marzocco_cancellation_methods.inc"
#undef gettimeofday
// The companion route runs unchanged; observation publication is out of scope.
namespace openbrewbyweight {
bool decodeLaMarzoccoDashboard(const cJSON *, LaMarzoccoObservation &) { return false; }
void OpenBrewByWeightLaMarzoccoService::applyPushLocked(const LaMarzoccoObservation &, uint32_t) {}
bool OpenBrewByWeightLaMarzoccoService::mergeReadLocked(const LaMarzoccoObservation &) { return false; }
#include "machine/OpenBrewByWeightLaMarzoccoCompanionRoute.inc"
}

using namespace openbrewbyweight;
namespace openbrewbyweight {
struct LaMarzoccoCancellationTest {
  static void progress(OpenBrewByWeightLaMarzoccoService &service, int outcome,
                       bool cancel, bool recoverDuringPerform, bool scale = false,
                       bool blockedBefore = false, bool abortOnly = false);
  static void retryCancellation(OpenBrewByWeightLaMarzoccoService &service);
  static void retireBackflushConfiguration();
  static void initializationAndGates();
  static void initializationRetries();
  static void queuedObservationAndPowerOrdering();
  static void unscaledShotAdmission();
  static void lateResponseAndResultLabels(OpenBrewByWeightLaMarzoccoService &service);
  static void invalidAuthLatchesApiObservation();
  static void tokenDiscardedOnCloudRefusal();
  static void relistKeepsSavedMachine();
  static void idleClosesWebSocketModeClient();
  static void unrecognizedCleaningLogged();
  static void postWakeSettledFromPush();
  static void companionRouteNeverReads();
  static void manualPowerGate();
  static void run();
};
}

void LaMarzoccoCancellationTest::progress(OpenBrewByWeightLaMarzoccoService &service, int outcome,
                                     bool cancel, bool recoverDuringPerform, bool scale,
                                     bool blockedBefore, bool abortOnly) {
  auto &shotActive_ = service.shotActive_;
  auto &scaleConnecting_ = service.scaleConnecting_;
  auto &abortRequested_ = service.abortRequested_;
  auto &powerActive_ = service.powerActive_;
  auto &temperatureActive_ = service.temperatureActive_;
  auto shotTransportPaused = [&] { return service.shotTransportPaused(); };
  auto networkEligible = [&](LaMarzoccoError &error) {
    return service.networkEligible(error);
  };
  auto stopWebSocket = [&] { service.stopWebSocket(); };
  auto websocketAdmitted = [&] { return service.websocketAdmitted(); };
  LaMarzoccoError gateError = LaMarzoccoError::NONE;
  OpenBrewByWeightLaMarzoccoService::WorkBuffer work;
  auto *work_ = &work;
  const uint32_t requestStartedAtMs = millis();
  unsigned calls = 0;
  waits = 0;
  performHook = [&] {
    ++calls;
    if (cancel) {
      if (abortOnly) service.abortRequested_.store(true);
      else service.publishNetworkState(true, false, !scale, scale);
      if (recoverDuringPerform)
        service.publishNetworkState(true, false, false, false);
    }
    return calls == 1 ? outcome : ESP_OK;
  };
  if (blockedBefore) service.publishNetworkState(true, false, !scale, scale);
  const auto stopsBefore = websocketStops;
#include "la_marzocco_cancellation_progress.inc"
  assert(canceled == cancel);
  assert(performed == (cancel ? ESP_FAIL : ESP_OK));
  assert(calls == (blockedBefore ? 0U : cancel || outcome != ESP_ERR_HTTP_EAGAIN ? 1U : 2U));
  assert(waits == (cancel || outcome != ESP_ERR_HTTP_EAGAIN ? 0U : 1U));
  assert(waits == 0 || lastWaitTicks == 10);  // Cancellation notifies; no 1 ms spin.
  if ((scale || abortOnly) && websocketAdmitted())
    assert(websocketStops == stopsBefore);  // HTTP cancellation cannot stop admitted WSS.
  performHook = {};
}

void LaMarzoccoCancellationTest::retryCancellation(OpenBrewByWeightLaMarzoccoService &service) {
  auto &abortRequested_ = service.abortRequested_;
  auto networkEligible = [&](LaMarzoccoError &error) { return service.networkEligible(error); };
  auto websocketAdmitted = [&] { return service.websocketAdmitted(); };
  auto stopWebSocket = [&] { service.stopWebSocket(); };
  const uint32_t delayMs = 100;
  assert(websocketAdmitted());  // Shots park the WSS subscription; they never stop it.
#include "la_marzocco_cancellation_retry.inc"
  assert(false);  // A pre-existing cancellation must return before waiting.
}

void LaMarzoccoCancellationTest::retireBackflushConfiguration() {
  for (int change = 0; change < 4; ++change) for (bool active : {false, true}) {
    OpenBrewByWeightLaMarzoccoService service;
    LaMarzoccoPersistedSettings settings;
    settings.accountConfigured = true;
    std::strcpy(settings.selectedSerial, "BEFORE");
    service.publishConfig(settings, 1);
    service.backflush_.observe(MachineBackflushPhase::INACTIVE, 0);
    service.backflush_.observe(MachineBackflushPhase::AWAITING, 0);
    const auto permit = active ? service.backflush_.start() : MachineBackflushPermit{};
    if (change == 0) settings.options &= ~LA_MARZOCCO_OBSERVE_STATE;
    if (change == 1) settings.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
    if (change == 2) settings.accountConfigured = false;
    if (change == 3) std::strcpy(settings.selectedSerial, "AFTER");
    service.publishConfig(settings, 2);
    assert(!service.backflush_.status().valid);
    assert(service.backflush_.status().busy() == active);
    service.backflush_.finish(permit.attempt);
    assert(!service.backflush_.status().busy());
  }
}

void LaMarzoccoCancellationTest::run() {
  outboundScaleConnected.store(true);
  OpenBrewByWeightLaMarzoccoService service;
  service.task_ = &service;
  LaMarzoccoPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.username, "test@example.invalid");
  std::strcpy(settings.selectedSerial, "TEST");
  settings.options = LA_MARZOCCO_APPLY_TEMPERATURE | LA_MARZOCCO_SHUTDOWN_WITH_SCALE;
  service.publishConfig(settings, 1);
  service.publishNetworkState(true, false, false, false);
  LaMarzoccoRequest request;
  request.type = LaMarzoccoRequestType::APPLY_TEMPERATURE;
  request.presetId = 1;
  request.targetDeciC = 930;
  assert(service.queue(request));

  // The worker must consume old cancellation before admission, never after it.
  unsigned executions = 0;
  executeHook = [&] {
    ++executions;
    assert(!service.abortRequested_.load());
  };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 1);
  assert(service.queue(request));
  static OpenBrewByWeightLaMarzoccoService *admitting = &service;
  TaskMutex::hostObserver = [](const TaskMutex *, bool locked) {
    if (!locked && admitting->active_) {
      admitting->abortRequested_.store(true);  // Producer at admission boundary.
      TaskMutex::hostObserver = nullptr;
    }
  };
  executeHook = [&] {
    ++executions;
    assert(service.abortRequested_.load());
  };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 2);
  TaskMutex::hostObserver = nullptr;

  for (bool power : {false, true}) {
    service.abortRequested_.store(false);
    service.temperatureActive_.store(!power);
    service.powerActive_.store(power);
    request.type = power ? LaMarzoccoRequestType::SET_STANDBY
                         : LaMarzoccoRequestType::APPLY_TEMPERATURE;
    assert(service.queue(request));
    service.publishNetworkState(true, false, false, false);
    assert(service.abortRequested_.load());
  }
  service.abortRequested_.store(false);
  settings.options = 0;
  service.websocketUnexpectedReconnect_ = true;
  service.publishConfig(settings, 2);
  assert(!service.websocketUnexpectedReconnect_);
  service.publishNetworkState(true, false, false, false);
  assert(service.abortRequested_.load());

  // Cleanup during installation/session renewal must preserve cancellation.
  OpenBrewByWeightLaMarzoccoService::WorkBuffer work;
  service.work_ = &work;
  service.clearSession();
  assert(service.abortRequested_.load());
  service.work_ = nullptr;

  for (int event = 0; event < 4; ++event) {
    service.abortRequested_.store(false);
    service.websocketStatus_.state = LaMarzoccoSocketState::STREAMING;
    service.websocketUnexpectedReconnect_ = false;
    service.publishNetworkState(event != 0, event == 1, event == 2, event == 3);
    service.publishNetworkState(true, false, false, false);
    assert(service.abortRequested_.load());
    assert(service.websocketUnexpectedReconnect_ == (event == 0));
  }
  for (bool recover : {false, true}) {
    for (int outcome : {ESP_ERR_HTTP_EAGAIN, ESP_OK, ESP_FAIL}) {
      service.publishNetworkState(true, false, false, false);
      service.abortRequested_.store(false);
      progress(service, outcome, true, recover);
    }
  }
  service.publishNetworkState(true, false, false, false);
  service.abortRequested_.store(false);
  progress(service, ESP_ERR_HTTP_EAGAIN, false, false);
  lateResponseAndResultLabels(service);

  // A canceled API observation retains evidence even after a short pause ends.
  service.pending_ = {};
  service.powerState_.reset();
  service.config_.accountConfigured = true;
  service.config_.options |= LA_MARZOCCO_OBSERVE_STATE;
  service.published_.sampleAtMs = 900;
  service.published_.powerState = LaMarzoccoPowerState::OFF;
  service.published_.effectiveOn = false;
  service.published_.quality = LaMarzoccoObservationQuality::CURRENT;
  OpenBrewByWeightLaMarzoccoService::PendingRequest paused;
  paused.identityGeneration = service.identityGeneration_;
  service.deferObservation(paused, service.published_, LaMarzoccoError::CANCELED);
  assert(service.pending_.present && retained(service.powerState_));
  const auto held = service.powerState_.effectiveStatus(service.published_, true, 100000);
  assert(!held.effectiveOn && held.quality == LaMarzoccoObservationQuality::STALE);
  assert(service.published_.sampleAtMs == 900);

  // Until a subscription is admitted, WS mode does not poll the dashboard.
  service.desiredTemperature_ = {};
  service.desiredPower_ = {};
  service.pending_ = {};
  service.shotActive_.store(false);
  service.scaleConnecting_.store(false);
  service.observationSchedule_.dueNow(millis());
  executeHook = [&] { assert(false && "WS resume must not enqueue an automatic GET"); };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  assert(service.queue(request));
  executeHook = [&] { ++executions; };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 3);
  service.observationSchedule_.armPostEvent(millis() - la_marzocco_timing::kPostWakeObservationDelayMs);
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 4);
  service.config_.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
  service.observationSchedule_.dueNow(millis());
  executeHook = [&] { ++executions; service.observationSchedule_.suspendPeriodic(); };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 5);

  // Network publication cannot erase cancellation from a concurrent producer.
  service.abortRequested_.store(false);
  std::thread publisher([&] {
    for (unsigned i = 0; i < 10000; ++i)
      service.publishNetworkState(true, false, false, false);
  });
  service.abortRequested_.store(true);
  publisher.join();
  assert(service.abortRequested_.load());
}

void LaMarzoccoCancellationTest::initializationAndGates() {
  OpenBrewByWeightLaMarzoccoService service;
  service.config_.accountConfigured = true;
  service.config_.options |= LA_MARZOCCO_OBSERVE_STATE;
  service.staConnected_.store(true);
  service.snapshotStamp_ = service.observationFence_.stamp(0, service.powerState_.generation());
  service.snapshotPending_ = true;
  unsigned calls = 0;
  executeHook = [&] { ++calls; };
  auto run = [&] { try { service.taskLoop(); } catch (const WorkerStopped &) {} };
  // A manual read and the initial read share one dispatch.
  service.pending_.present = true;
  service.pending_.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  run();
  assert(calls == 1 && initialSnapshot && !service.snapshotPending_);
  assert(dispatchedStamp.connectionRevision == service.snapshotStamp_.connectionRevision);
  run();
  assert(calls == 1);
  // Post-wake reconciliation must not inherit the subscription's old intent.
  service.powerState_.reset();
  service.snapshotPending_ = true;
  service.observationSchedule_.armPostEvent(millis());
  run();
  assert(calls == 1 && service.snapshotPending_);
  service.observationSchedule_.armPostEvent(millis() - la_marzocco_timing::kPostWakeObservationDelayMs);
  run();
  assert(calls == 2 && !initialSnapshot);

  for (auto type : {LaMarzoccoRequestType::CONNECT, LaMarzoccoRequestType::OBSERVE_STATE,
                    LaMarzoccoRequestType::APPLY_TEMPERATURE, LaMarzoccoRequestType::SET_STANDBY,
                    LaMarzoccoRequestType::SET_POWER_ON}) {
    const auto before = calls;
    service.pending_.present = true;
    service.pending_.request.type = type;
    outboundShotActive.store(true);
    outboundAcquisitionHeld.store(true);
    run();
    assert(calls == before && service.pending_.present);
    outboundShotActive.store(false);
    run();
    assert(calls == before);
    outboundScaleSetup.store(true);
    outboundAcquisitionHeld.store(false);
    run();
    assert(calls == before);
    outboundBleQuiet.store(true);
    outboundScaleSetup.store(false);
    run();
    assert(calls == before);
    outboundShotActive.store(true);
    outboundBleQuiet.store(false);
    run();
    assert(calls == before);
    outboundShotActive.store(false);
    run();
    assert(calls == before + 1 && !service.pending_.present);
  }
  for (bool scale : {false, true}) {
    for (bool before : {false, true}) {
      for (int outcome : {ESP_ERR_HTTP_EAGAIN, ESP_OK, ESP_FAIL}) {
        service.publishNetworkState(true, false, false, false);
        service.abortRequested_.store(false);
        progress(service, outcome, true, true, scale, before);
      }
    }
  }
  service.publishNetworkState(true, false, false, false);
  service.abortRequested_.store(false);
  progress(service, ESP_ERR_HTTP_EAGAIN, true, false, false, false, true);
  for (bool shot : {false, true}) {
    service.publishNetworkState(true, false, shot, false);
    service.abortRequested_.store(true);
    const auto before = websocketStops;
    retryCancellation(service);
    assert(websocketStops == before);
  }
  outboundScaleConnected.store(false);
}

void LaMarzoccoCancellationTest::unscaledShotAdmission() {
  assert(!outboundScaleConnected.load() && !outboundScaleInhibited());
  OpenBrewByWeightLaMarzoccoService service;
  service.task_ = &service;
  service.config_.accountConfigured = true;
  service.config_.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
  service.publishNetworkState(true, false, false, false);
  assert(!service.abortRequested_.load());
  service.pending_.present = true;
  service.pending_.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  const auto epoch = service.observationFence_.epoch;
  const auto generation = service.disconnectGeneration_.load();
  outboundShotActive.store(true);
  service.inhibitCloud();
  assert(!service.abortRequested_.load());
  service.publishNetworkState(true, false, true, false);
  assert(!service.abortRequested_.load() && !service.shotTransportPaused());
  assert(service.observationFence_.epoch == epoch);
  assert(service.disconnectGeneration_.load() == generation);
  progress(service, ESP_ERR_HTTP_EAGAIN, false, false);
  unsigned calls = 0;
  executeHook = [&] { ++calls; throw WorkerStopped{}; };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(calls == 1 && !service.pending_.present);
  for (bool temperature : {false, true}) {
    service.powerActive_.store(!temperature);
    service.temperatureActive_.store(temperature);
    service.inhibitCloud();
    assert(service.abortRequested_.load());
    assert(service.disconnectGeneration_.load() == generation);
    service.abortRequested_.store(false);
  }
  service.powerActive_.store(false);
  service.temperatureActive_.store(false);
  outboundShotActive.store(false);
  service.publishNetworkState(true, false, false, false);
  executeHook = {};
}

void LaMarzoccoCancellationTest::initializationRetries() {
  OpenBrewByWeightLaMarzoccoService service;
  service.config_.accountConfigured = true;
  service.config_.options |= LA_MARZOCCO_OBSERVE_STATE;
  service.staConnected_.store(true);
  service.websocketStatus_.subscribed = true;
  service.published_.sampleAtMs = 100;
  OpenBrewByWeightLaMarzoccoService::PendingRequest pending;
  pending.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  pending.initialSnapshot = true;
  pending.snapshotStamp = service.observationFence_.stamp(0, service.powerState_.generation());
  unsigned reads = 0;
  dashboardHook = [&](const LaMarzoccoObservationStamp *stamp) {
    assert(stamp && stamp->powerRevision == pending.snapshotStamp.powerRevision);
    ++reads;
    return reads == 2;
  };
  assert(service.executeObservation(pending));
  assert(reads == 2 && authentications == 2 && retries == 1);  // Renewed session still reads.
  reads = authentications = retries = failures = 0;
  dashboardHook = [&](const LaMarzoccoObservationStamp *) { ++reads; return false; };
  assert(!service.executeObservation(pending));
  assert(reads == 4 && retries == 3 && failures == 1);
  assert(!service.observationSchedule_.automaticDue(millis() + 60000));
  reads = authentications = retries = 0;
  service.observationFence_.invalidate();
  assert(service.executeObservation(pending));
  assert(reads == 0 && authentications == 0);  // Old deferred session is terminal.
  pending.snapshotStamp = service.observationFence_.stamp(0, service.powerState_.generation());
  service.observationFence_.offline = true;
  assert(service.executeObservation(pending));
  assert(reads == 0);
  service.observationFence_.offline = false;
  dashboardHook = [&](const LaMarzoccoObservationStamp *) {
    ++reads;
    service.observationFence_.invalidate();  // Socket failed during HTTP.
    return false;
  };
  assert(service.executeObservation(pending));
  assert(reads == 1 && authentications == 1);
  pending.initialSnapshot = false;
  reads = authentications = 0;
  dashboardHook = [&](const LaMarzoccoObservationStamp *stamp) {
    assert(!stamp);  // A coalesced post-command read uses the current baseline.
    ++reads;
    return true;
  };
  assert(service.executeObservation(pending));
  assert(reads == 1 && authentications == 1);
  service.config_.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
  assert(service.executeObservation(pending));
  assert(reads == 1 && authentications == 2);  // API renewal optimization is unchanged.
}

void LaMarzoccoCancellationTest::queuedObservationAndPowerOrdering() {
  OpenBrewByWeightLaMarzoccoService service;
  service.task_ = &service;
  service.config_.accountConfigured = true;
  service.config_.options |= LA_MARZOCCO_SHUTDOWN_WITH_SCALE;
  service.staConnected_.store(true);
  unsigned calls = 0;
  executeHook = [&] { ++calls; };
  auto run = [&] { try { service.taskLoop(); } catch (const WorkerStopped &) {} };
  OpenBrewByWeightLaMarzoccoService::PendingRequest deferred;
  deferred.present = deferred.initialSnapshot = true;
  deferred.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  service.deferObservation(deferred, service.published_, LaMarzoccoError::CANCELED);
  run();
  LaMarzoccoRequest refresh;
  refresh.type = LaMarzoccoRequestType::OBSERVE_STATE;
  assert(service.queue(refresh));
  run();
  const bool freshManualRead = !initialSnapshot;

  // Wake before subscription; contrary push then advances the power revision.
  service.powerState_.reset();
  service.published_.sampleAtMs = 1;
  service.published_.powerState = LaMarzoccoPowerState::OFF;
  service.published_.quality = LaMarzoccoObservationQuality::CURRENT;
  const auto wakeAt = millis() - la_marzocco_timing::kPostWakeObservationDelayMs;
  assert(service.powerState_.notePhysicalStart(service.published_, true, true, wakeAt));
  service.observationSchedule_.armPostEvent(wakeAt);
  service.snapshotStamp_ = service.observationFence_.stamp(0, service.powerState_.generation());
  service.snapshotPending_ = true;
  LaMarzoccoObservation contrary;
  contrary.source = LaMarzoccoObservationSource::WEBSOCKET;
  contrary.stamp = service.snapshotStamp_;
  contrary.receivedAtMs = millis() - 1;
  contrary.powerPresent = true;
  contrary.mode = LaMarzoccoObservedMode::STANDBY;
  assert(service.observationFence_.merge(service.published_, service.powerState_, contrary, true));
  run();
  const bool currentReconciliation = !initialSnapshot;

  LaMarzoccoRequest power;
  power.type = LaMarzoccoRequestType::SET_STANDBY;
  assert(service.queue(power));
  const auto beforeShot = calls;
  service.publishNetworkState(true, false, true, false);
  service.publishNetworkState(true, false, false, false);
  run();
  const bool noPowerReplay = calls == beforeShot;
  if (!freshManualRead || !currentReconciliation || !noPowerReplay)
    std::fprintf(stderr, "review: fresh manual=%d, reconciliation=%d, no power replay=%d\n",
                 freshManualRead, currentReconciliation, noPowerReplay);
  assert(freshManualRead && currentReconciliation && noPowerReplay);
  // A complete short shot between network/worker turns still cancels power.
  assert(service.queue(power));
  const auto oldRequest = service.desiredPower_.request;
  outboundShotActive.store(true);
  service.inhibitCloud();
  assert(!service.queue(power));
  outboundShotActive.store(false);
  assert(!service.powerRequestCurrent(oldRequest, service.configGeneration_));
  run();
  assert(calls == beforeShot && !service.desiredPower_.present);
  // A new trigger after the shot survives; acquisition alone only defers it.
  assert(service.queue(power));
  service.inhibitCloud();
  assert(service.powerRequestCurrent(service.desiredPower_.request, service.configGeneration_));
  run();
  assert(calls == beforeShot + 1);
  executeHook = {};
}

void LaMarzoccoCancellationTest::lateResponseAndResultLabels(OpenBrewByWeightLaMarzoccoService &service) {
  performAdvanceMs = 20000;  // A complete response after the deadline is used.
  progress(service, ESP_OK, false, false);
  performAdvanceMs = 0;
  testNowMs = 1000;
  assert(std::strcmp(laMarzoccoCloudCallResult(false, false, false, 401), "http_error") == 0);
  assert(std::strcmp(laMarzoccoCloudCallResult(false, false, false, 0), "transport_error") == 0);
  assert(std::strcmp(laMarzoccoCloudCallResult(false, true, false, 200), "response_too_large") == 0);
  assert(std::strcmp(laMarzoccoCloudCallResult(false, false, true, 200), "success") == 0);
}

void LaMarzoccoCancellationTest::invalidAuthLatchesApiObservation() {
  OpenBrewByWeightLaMarzoccoService service;
  service.task_ = &service;
  LaMarzoccoPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.selectedSerial, "TEST");
  settings.options = LA_MARZOCCO_OBSERVE_STATE;
  settings.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
  service.publishConfig(settings, 1);
  LaMarzoccoStatus status = service.published_;
  status.error = LaMarzoccoError::INVALID_AUTH;
  service.publish(status);
  service.scheduleAutomatic(millis(), false);
  assert(!service.observationSchedule_.automaticDue(millis() + 3600000));
  status.error = LaMarzoccoError::NONE;  // A manual refresh succeeded.
  service.publish(status);
  service.scheduleAutomatic(millis(), false);
  assert(service.observationSchedule_.automaticDue(millis() + 3600000));
  status.error = LaMarzoccoError::INVALID_AUTH;
  service.publish(status);
  service.scheduleAutomatic(millis(), true);
  std::strcpy(settings.selectedSerial, "OTHER");  // Editing the account re-arms.
  service.publishConfig(settings, 2);
  assert(service.observationSchedule_.automaticDue(millis()));
}

// A 401/403 dashboard answer rejects the token itself: each retry attempt
// re-acquires a session, and a successful read leaves the session alone.
void LaMarzoccoCancellationTest::tokenDiscardedOnCloudRefusal() {
  OpenBrewByWeightLaMarzoccoService service;
  service.config_.accountConfigured = true;
  service.config_.options |= LA_MARZOCCO_OBSERVE_STATE;
  service.staConnected_.store(true);
  OpenBrewByWeightLaMarzoccoService::PendingRequest pending;
  pending.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
  OpenBrewByWeightLaMarzoccoService::WorkBuffer work;
  service.work_ = &work;
  const auto armSession = [&] {
    std::strcpy(work.accessToken, "live");
    work.accessTokenIssuedAtMs = millis() - 60000;
    service.tokenAvailable_.store(true);
  };
  for (const uint16_t refused : {uint16_t(403), uint16_t(401)}) {
    unsigned reads = 0;
    authentications = 0;
    dashboardHook = [&](const LaMarzoccoObservationStamp *) {
      ++reads;
      work.httpStatus = refused;
      return false;
    };
    armSession();
    assert(!service.executeObservation(pending));
    assert(reads == la_marzocco_timing::kMaxAttempts &&
           authentications == la_marzocco_timing::kMaxAttempts);
    assert(work.accessToken[0] == '\0' && work.accessTokenIssuedAtMs == 0 &&
           !service.tokenAvailable_.load());
  }
  authentications = 0;
  dashboardHook = [&](const LaMarzoccoObservationStamp *) {
    work.httpStatus = 200;
    return true;
  };
  armSession();
  assert(service.executeObservation(pending));
  assert(authentications == 1 && std::strcmp(work.accessToken, "live") == 0 &&
         work.accessTokenIssuedAtMs == millis() - 60000);
  service.work_ = nullptr;
  authentications = retries = failures = 0;
  dashboardHook = nullptr;
}

// Changing machine re-lists on the saved key and live session; nothing
// observable changes until the chosen machine is saved.
void LaMarzoccoCancellationTest::relistKeepsSavedMachine() {
  OpenBrewByWeightLaMarzoccoService service;
  LaMarzoccoPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.username, "test@example.invalid");
  std::strcpy(settings.password, "secret");
  settings.installationPrivateKey[0] = 7;
  std::strcpy(settings.selectedSerial, "SAVED");
  std::strcpy(settings.selectedName, "Saved");
  assert(validLaMarzoccoSettings(settings));
  service.publishConfig(settings, 1);
  assert(!service.queueRelist(1));  // No worker yet.
  service.task_ = &service;
  service.publishNetworkState(true, false, false, false);
  const LaMarzoccoStatus before = service.published_;
  const uint32_t identity = service.identityGeneration_;
  const auto take = [&] {
    OpenBrewByWeightLaMarzoccoService::PendingRequest pending = service.pending_;
    service.pending_.present = false;
    return pending;
  };
  assert(service.queueRelist(2));
  assert(!service.queueRelist(3));
  assert(service.discovery_.listing && service.discovery_.count == 0);
  auto pending = take();
  assert(pending.request.type == LaMarzoccoRequestType::LIST_MACHINES);
  assert(sameSessionIdentity(pending.credentials, settings));
  listHook = [](LaMarzoccoDiscoverySnapshot &found) {
    found.count = 2;
    std::strcpy(found.machines[0].serial, "SAVED");
    std::strcpy(found.machines[1].serial, "OTHER");
    std::strcpy(found.machines[1].name, "Other");
    return true;
  };
  lastRegisterKey = true;
  service.executeRelist(pending);
  assert(!lastRegisterKey);
  LaMarzoccoDiscoverySnapshot listed = service.discovery_;
  assert(!listed.listing && listed.error == LaMarzoccoError::NONE &&
         listed.count == 2 && listed.accountMachines == 2 && listed.requestId == 2);
  assert(service.identityGeneration_ == identity &&
         service.published_.phase == before.phase &&
         service.published_.requestId == before.requestId &&
         std::strcmp(service.config_.selectedSerial, "SAVED") == 0);
  LaMarzoccoPersistedSettings chosen;
  assert(service.selectDiscoveredMachine("OTHER", chosen));
  assert(std::strcmp(chosen.selectedSerial, "OTHER") == 0 &&
         std::strcmp(chosen.selectedName, "Other") == 0 &&
         std::strcmp(chosen.password, "secret") == 0 &&
         std::memcmp(chosen.installationPrivateKey, settings.installationPrivateKey,
                     sizeof(settings.installationPrivateKey)) == 0);
  service.publishConfig(chosen, 2);
  assert(service.identityGeneration_ != identity);
  assert(service.discovery_.count == 0 && service.discovery_.accountMachines == 2);

  assert(service.queueRelist(4));
  pending = take();
  listHook = [](LaMarzoccoDiscoverySnapshot &) { return false; };
  service.abortRequested_.store(false);  // The idle task loop consumes it.
  const unsigned retriesBefore = retries;
  service.executeRelist(pending);
  listed = service.discovery_;
  assert(!listed.listing && listed.error == LaMarzoccoError::HTTP_ERROR &&
         listed.count == 0 && listed.accountMachines == 2 &&
         retries - retriesBefore == la_marzocco_timing::kRetryDelaysMs.size());

  // One dropped connection is retried instead of leaving Change empty.
  assert(service.queueRelist(4));
  pending = take();
  listHook = [calls = 0](LaMarzoccoDiscoverySnapshot &found) mutable {
    return ++calls > 1 && (found.count = 1) != 0;
  };
  service.executeRelist(pending);
  listed = service.discovery_;
  assert(!listed.listing && listed.error == LaMarzoccoError::NONE && listed.count == 1);

  assert(service.queueRelist(5));
  pending = take();
  service.publishConfig(LaMarzoccoPersistedSettings{}, 3);  // Disconnected meanwhile.
  listHook = [](LaMarzoccoDiscoverySnapshot &found) { return (found.count = 2) != 0; };
  service.executeRelist(pending);
  listed = service.discovery_;
  assert(!listed.listing && listed.count == 0 && listed.accountMachines == 0);
  assert(!service.queueRelist(6));
  authentications = retries = 0;
}

// WebSocket mode never carries an idle HTTP connection into the next
// operation; API polling keeps it for the reuse window.
void LaMarzoccoCancellationTest::idleClosesWebSocketModeClient() {
  for (const auto type : {LaMarzoccoConnectionType::WEBSOCKET, LaMarzoccoConnectionType::API}) {
    OpenBrewByWeightLaMarzoccoService service;
    LaMarzoccoPersistedSettings settings;
    settings.connectionType = static_cast<uint8_t>(type);
    service.publishConfig(settings, 1);
    service.clearSessionRequested_.store(false);
    OpenBrewByWeightLaMarzoccoService::WorkBuffer work;
    work.client = &work;
    service.work_ = &work;
    try { service.taskLoop(); } catch (const WorkerStopped &) {}
    assert((work.client == nullptr) == (type == LaMarzoccoConnectionType::WEBSOCKET));
    service.work_ = nullptr;
  }
}

// The Web UI log names an unrecognized cleaning status once per change.
void LaMarzoccoCancellationTest::unrecognizedCleaningLogged() {
  OpenBrewByWeightLaMarzoccoService service;
  service.publishConfig(LaMarzoccoPersistedSettings{}, 1);
  service.clearSessionRequested_.store(false);
  auto &ws = service.websocketStatus_;
  const auto loop = [&] { try { service.taskLoop(); } catch (const WorkerStopped &) {} };
  loop();
  assert(cleaningTraces == 0);  // Nothing received yet.
  ws.cleaningAtMs = millis();
  ws.cleaningAvailable = true;
  std::strcpy(ws.cleaningLabel, "Rinsing");
  loop();
  loop();
  assert(cleaningTraces == 1 && cleaningTrace ==
         "La Marzocco cleaning status \"Rinsing\" not recognized: paddle starts refused for up to 3 min");
  ws.cleaningAvailable = false;
  loop();
  assert(cleaningTraces == 2 && cleaningTrace.find("\"(removed)\"") != std::string::npos);
  ws.cleaning = LaMarzoccoCleaningState::OFF;
  loop();
  ws.cleaning = LaMarzoccoCleaningState::UNKNOWN;
  loop();
  assert(cleaningTraces == 3);
}

void LaMarzoccoCancellationTest::postWakeSettledFromPush() {
  for (const bool live : {true, false}) {
    OpenBrewByWeightLaMarzoccoService service;
    service.config_.accountConfigured = true;
    service.config_.options |= LA_MARZOCCO_OBSERVE_STATE;
    service.staConnected_.store(true);
    service.published_.connectionFreshness = true;
    service.published_.sampleAtMs = millis();
    service.published_.powerState = LaMarzoccoPowerState::OFF;
    service.published_.quality = LaMarzoccoObservationQuality::CURRENT;
    service.observationFence_.powerTrusted = true;
    assert(service.powerState_.notePowerOnCommandAccepted(service.published_, true, millis()));
    testNowMs += la_marzocco_timing::kPostWakeObservationDelayMs;
    service.observationSchedule_.armPostEvent(millis() - la_marzocco_timing::kPostWakeObservationDelayMs);
    liveSubscription = live;
    unsigned calls = 0;
    executeHook = [&] { ++calls; };
    try { service.taskLoop(); } catch (const WorkerStopped &) {}
    // A live subscription settles the wake from pushed evidence; HTTPS is the fallback.
    assert(calls == (live ? 0U : 1U));
    assert(service.powerState_.effectiveStatus(service.published_, true, millis()).optimisticOn == !live);
  }
  liveSubscription = false;
}

// On the companion route the task never reads or polls: wakes settle from the
// companion's state and a user refresh becomes one `refresh` command.
void LaMarzoccoCancellationTest::companionRouteNeverReads() {
  for (const bool live : {true, false}) {
    OpenBrewByWeightLaMarzoccoService service;
    service.config_.accountConfigured = true;
    service.config_.options |= LA_MARZOCCO_OBSERVE_STATE;
    service.config_.connectionType = static_cast<uint8_t>(LaMarzoccoConnectionType::API);
    service.staConnected_.store(true);
    service.companionLink_.store(true);
    service.companionRoute_.store(true);
    service.companion_.status.route = LaMarzoccoCompanionRoute::COMPANION;
    service.companion_.lmSwitch = service.companion_.up = true;
    service.companion_.pingAtMs = millis();
    service.websocketStatus_.subscribed = true;  // The full frame's basis.
    service.published_.sampleAtMs = millis();
    service.published_.powerState = LaMarzoccoPowerState::OFF;
    service.published_.quality = LaMarzoccoObservationQuality::CURRENT;
    service.observationFence_.powerTrusted = true;
    assert(service.powerState_.notePowerOnCommandAccepted(service.published_, true, millis()));
    testNowMs += la_marzocco_timing::kPostWakeObservationDelayMs;
    service.companion_.pingAtMs = millis();
    service.observationSchedule_.armPostEvent(millis() - la_marzocco_timing::kPostWakeObservationDelayMs);
    service.observationSchedule_.dueNow(millis());  // API polling would be due too.
    liveSubscription = live;
    unsigned calls = 0;
    executeHook = [&] { ++calls; };
    try { service.taskLoop(); } catch (const WorkerStopped &) {}
    assert(calls == 0 && service.companion_.status.route == LaMarzoccoCompanionRoute::COMPANION);
    assert(service.powerState_.effectiveStatus(service.published_, true, millis()).optimisticOn == !live);
    // A refresh is one companion command: no session, no dashboard read.
    OpenBrewByWeightLaMarzoccoService::WorkBuffer work;
    service.work_ = &work;
    OpenBrewByWeightLaMarzoccoService::PendingRequest refresh;
    refresh.request.type = LaMarzoccoRequestType::OBSERVE_STATE;
    refresh.identityGeneration = service.identityGeneration_;
    const unsigned sessions = authentications;
    dashboardHook = [](const LaMarzoccoObservationStamp *) { assert(false && "no read"); return false; };
    notifyTakeHook = [&] {
      const auto box = service.companionOutbox();
      assert(box.commandId != 0 && box.commandOp == LaMarzoccoCompanionCommandOp::REFRESH);
      service.companionCommandResult(box.commandId, live, live ? 0 : 503);
    };
    assert(service.executeObservation(refresh) == live);
    notifyTakeHook = nullptr;
    dashboardHook = nullptr;
    assert(authentications == sessions && service.companionOutbox().commandId == 0);
    assert(work.httpStatus == (live ? 200 : 503) && !work.transportFailure);
    service.work_ = nullptr;
  }
  liveSubscription = false;
}

// Admin → Machine control is the only gate a manual power command meets in
// the service, however the request arrives; the scale options keep gating
// only scale commands, and turning the setting off drops a pending manual
// command but never a scale one (AC3, AC4).
void LaMarzoccoCancellationTest::manualPowerGate() {
  OpenBrewByWeightLaMarzoccoService service;
  service.task_ = &service;
  LaMarzoccoPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.selectedSerial, "TEST");
  settings.options = LA_MARZOCCO_OBSERVE_STATE | LA_MARZOCCO_POWER_ON_WITH_SCALE |
                     LA_MARZOCCO_SHUTDOWN_WITH_SCALE;
  service.publishConfig(settings, 1);
  LaMarzoccoRequest manual;
  manual.type = LaMarzoccoRequestType::SET_POWER_ON;
  manual.origin = LaMarzoccoRequestOrigin::MANUAL;
  LaMarzoccoRequest scale;
  scale.type = LaMarzoccoRequestType::SET_STANDBY;
  // Off by default: the scale options never authorize a manual command.
  assert(!service.queue(manual) && !service.desiredPower_.present);
  // Scale commands keep their own options whatever the setting says.
  assert(service.queue(scale));
  service.publishManualPower(false);
  assert(service.desiredPower_.present &&
         service.desiredPower_.request.origin == LaMarzoccoRequestOrigin::SCALE);
  service.desiredPower_ = {};
  // On: only the setting matters for a manual command, not the scale bits.
  service.publishManualPower(true);
  settings.options = LA_MARZOCCO_OBSERVE_STATE;
  service.publishConfig(settings, 2);
  assert(!service.queue(scale));
  assert(service.queue(manual));
  const LaMarzoccoRequest pending = service.desiredPower_.request;
  assert(pending.origin == LaMarzoccoRequestOrigin::MANUAL &&
         service.powerRequestCurrent(pending, 2));
  // A settings change keeps an allowed manual command current.
  settings.options |= LA_MARZOCCO_RECOGNIZE_WAKE;
  service.publishConfig(settings, 3);
  assert(service.powerRequestCurrent(pending, 3));
  // One manual command at a time: a second one is refused, never replaces it.
  LaMarzoccoRequest off = manual;
  off.type = LaMarzoccoRequestType::SET_STANDBY;
  assert(!service.queue(off) && service.desiredPower_.request.type == LaMarzoccoRequestType::SET_POWER_ON);
  // Turning the setting off drops the pending manual command and aborts it.
  service.powerActive_.store(true);
  service.abortRequested_.store(false);
  service.publishManualPower(false);
  service.powerActive_.store(false);
  assert(!service.desiredPower_.present && service.abortRequested_.load() &&
         !service.powerRequestCurrent(pending, 3));
  assert(!service.queue(manual));
  // Enabled again: a shot still refuses it.
  service.publishManualPower(true);
  service.shotActive_.store(true);
  assert(!service.queue(manual));
  service.shotActive_.store(false);
  assert(service.queue(manual));
  service.desiredPower_ = {};
  // A manual command is never authorized without an account.
  settings.accountConfigured = false;
  service.publishConfig(settings, 4);
  assert(!service.queue(manual));
}

int main() {
  LaMarzoccoCancellationTest::manualPowerGate();
  LaMarzoccoCancellationTest::companionRouteNeverReads();
  LaMarzoccoCancellationTest::postWakeSettledFromPush();
  LaMarzoccoCancellationTest::relistKeepsSavedMachine();
  LaMarzoccoCancellationTest::idleClosesWebSocketModeClient();
  LaMarzoccoCancellationTest::unrecognizedCleaningLogged();
  LaMarzoccoCancellationTest::retireBackflushConfiguration();
  LaMarzoccoCancellationTest::invalidAuthLatchesApiObservation();
  LaMarzoccoCancellationTest::tokenDiscardedOnCloudRefusal();
  LaMarzoccoCancellationTest::run();
  LaMarzoccoCancellationTest::initializationAndGates();
  LaMarzoccoCancellationTest::unscaledShotAdmission();
  LaMarzoccoCancellationTest::initializationRetries();
  LaMarzoccoCancellationTest::queuedObservationAndPowerOrdering();
  // Opening admission must publish the new fence before another core sees it.
  outboundAcquisitionHeld.store(false);
  outboundAcquisitionGeneration.store(0);
  std::atomic<unsigned> round{0}, observed{0};
  std::thread receiver([&] {
    for (unsigned i = 1; i <= 100000; ++i) {
      while (round.load(std::memory_order_acquire) != i) std::this_thread::yield();
      while (outboundAcquisitionHeld.load(std::memory_order_acquire)) {}
      assert(outboundAcquisitionGeneration.load(std::memory_order_acquire) == 2 * i);
      observed.store(i, std::memory_order_release);
    }
  });
  for (unsigned i = 1; i <= 100000; ++i) {
    publishOutboundAcquisition(true, i);
    round.store(i, std::memory_order_release);
    publishOutboundAcquisition(false, i);
    while (observed.load(std::memory_order_acquire) != i) std::this_thread::yield();
  }
  receiver.join();
}

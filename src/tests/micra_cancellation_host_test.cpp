#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <sys/time.h>
#include "machine/OpenBrewByWeightMicraPublicIdentityCache.h"
#include "machine/OpenBrewByWeightMicraService.h"
#include "OpenBrewByWeightAuditLog.h"
#include "OpenBrewByWeightOutboundAdmission.h"

// Only scheduling and transport are simulated; extracted service code is unchanged.
static uint32_t testNowMs = 1000, performAdvanceMs = 0, lastWaitTicks = 0;
uint32_t millis() { return testNowMs; }
void xTaskNotifyGive(TaskHandle_t) {}
// The queue() mux-wait detector's emit path; uncontended waits never reach it.
void serialTraceCategoryf(openbrewbyweight::LogLevel, openbrewbyweight::DebugCategory,
                          const char *, ...) {}
void addDebugEventDetail(openbrewbyweight::DebugCategory, openbrewbyweight::DebugCode,
                         int32_t, int32_t, const char *) {}
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_HTTP_EAGAIN = 1;
constexpr int ESP_ERR_HTTP_READ_TIMEOUT = 2, pdTRUE = 1;
uint32_t pdMS_TO_TICKS(uint32_t ms) { return ms; }
struct WorkerStopped {};
static std::function<int()> performHook;
static std::function<void()> executeHook;
static bool initialSnapshot = false;
static openbrewbyweight::MicraObservationStamp dispatchedStamp;
static std::function<bool(const openbrewbyweight::MicraObservationStamp *)> dashboardHook;
static unsigned authentications = 0, retries = 0, failures = 0;
static unsigned waits = 0;
static unsigned websocketStops = 0;
static bool lastRegisterKey = false;
static std::function<bool(openbrewbyweight::LineaMicraDiscoverySnapshot &)> listHook;
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
  return 0;
}
namespace openbrewbyweight {
bool firmwareCompatibilityMode() { return false; }
int testGettimeofday(timeval *now, void *) { now->tv_sec = 1700000010L; return 0; }
void OpenBrewByWeightMicraService::releaseIoBuffer(bool) {}
void OpenBrewByWeightMicraService::releaseWorkBuffer() { clearSession(); }
void OpenBrewByWeightMicraService::serviceWebSocket() {}
void OpenBrewByWeightMicraService::stopWebSocket(bool) { ++websocketStops; }
bool OpenBrewByWeightMicraService::ensureSession(LineaMicraPersistedSettings &, bool registerKey,
                                            bool *renewed) {
  ++authentications;
  lastRegisterKey = registerKey;
  if (renewed != nullptr) *renewed = true;
  return true;
}
bool OpenBrewByWeightMicraService::ensureWorkBuffer() { return true; }
bool OpenBrewByWeightMicraService::listMachines(const LineaMicraPersistedSettings &,
    LineaMicraDiscoverySnapshot &found) { return listHook(found); }
bool OpenBrewByWeightMicraService::readDashboard(const LineaMicraPersistedSettings &,
    LineaMicraStatus &, const MicraObservationStamp *snapshot) { return dashboardHook(snapshot); }
void OpenBrewByWeightMicraService::waitRetry(uint32_t) { ++retries; }
LineaMicraError OpenBrewByWeightMicraService::classifyFailure() const { return LineaMicraError::HTTP_ERROR; }
void OpenBrewByWeightMicraService::fail(LineaMicraStatus &, LineaMicraError) { ++failures; }
void OpenBrewByWeightMicraService::execute(PendingRequest &pending) {
  initialSnapshot = pending.initialSnapshot;
  dispatchedStamp = pending.snapshotStamp;
  executeHook();
}
bool OpenBrewByWeightMicraService::executeTemperatureApplication(
    const LineaMicraRequest &, uint32_t) {
  executeHook();
  desiredTemperature_.present = false;
  return true;
}
bool OpenBrewByWeightMicraService::executePowerApplication(
    const LineaMicraRequest &, uint32_t) {
  executeHook();
  desiredPower_.present = false;
  return true;
}
}
#define gettimeofday testGettimeofday
#include "micra_cancellation_methods.inc"
#undef gettimeofday

using namespace openbrewbyweight;
namespace openbrewbyweight {
struct MicraCancellationTest {
  static void progress(OpenBrewByWeightMicraService &service, int outcome,
                       bool cancel, bool recoverDuringPerform, bool scale = false,
                       bool blockedBefore = false, bool abortOnly = false);
  static void retryCancellation(OpenBrewByWeightMicraService &service);
  static void retireBackflushConfiguration();
  static void initializationAndGates();
  static void initializationRetries();
  static void queuedObservationAndPowerOrdering();
  static void unscaledShotAdmission();
  static void lateResponseAndResultLabels(OpenBrewByWeightMicraService &service);
  static void invalidAuthLatchesApiObservation();
  static void relistKeepsSavedMachine();
  static void idleClosesWebSocketModeClient();
  static void run();
};
}

void MicraCancellationTest::progress(OpenBrewByWeightMicraService &service, int outcome,
                                     bool cancel, bool recoverDuringPerform, bool scale,
                                     bool blockedBefore, bool abortOnly) {
  auto &shotActive_ = service.shotActive_;
  auto &scaleConnecting_ = service.scaleConnecting_;
  auto &abortRequested_ = service.abortRequested_;
  auto &powerActive_ = service.powerActive_;
  auto &temperatureActive_ = service.temperatureActive_;
  auto shotTransportPaused = [&] { return service.shotTransportPaused(); };
  auto networkEligible = [&](LineaMicraError &error) {
    return service.networkEligible(error);
  };
  auto stopWebSocket = [&] { service.stopWebSocket(); };
  auto websocketAdmitted = [&] { return service.websocketAdmitted(); };
  LineaMicraError gateError = LineaMicraError::NONE;
  OpenBrewByWeightMicraService::WorkBuffer work;
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
#include "micra_cancellation_progress.inc"
  assert(canceled == cancel);
  assert(performed == (cancel ? ESP_FAIL : ESP_OK));
  assert(calls == (blockedBefore ? 0U : cancel || outcome != ESP_ERR_HTTP_EAGAIN ? 1U : 2U));
  assert(waits == (cancel || outcome != ESP_ERR_HTTP_EAGAIN ? 0U : 1U));
  assert(waits == 0 || lastWaitTicks == 10);  // Cancellation notifies; no 1 ms spin.
  if ((scale || abortOnly) && websocketAdmitted())
    assert(websocketStops == stopsBefore);  // HTTP cancellation cannot stop admitted WSS.
  performHook = {};
}

void MicraCancellationTest::retryCancellation(OpenBrewByWeightMicraService &service) {
  auto &abortRequested_ = service.abortRequested_;
  auto networkEligible = [&](LineaMicraError &error) { return service.networkEligible(error); };
  auto websocketAdmitted = [&] { return service.websocketAdmitted(); };
  auto stopWebSocket = [&] { service.stopWebSocket(); };
  const uint32_t delayMs = 100;
  assert(websocketAdmitted());  // Shots park the WSS subscription; they never stop it.
#include "micra_cancellation_retry.inc"
  assert(false);  // A pre-existing cancellation must return before waiting.
}

void MicraCancellationTest::retireBackflushConfiguration() {
  for (int change = 0; change < 4; ++change) for (bool active : {false, true}) {
    OpenBrewByWeightMicraService service;
    LineaMicraPersistedSettings settings;
    settings.accountConfigured = true;
    std::strcpy(settings.selectedSerial, "BEFORE");
    service.publishConfig(settings, 1);
    service.backflush_.observe(MachineBackflushPhase::INACTIVE, 0);
    service.backflush_.observe(MachineBackflushPhase::AWAITING, 0);
    const auto permit = active ? service.backflush_.start() : MachineBackflushPermit{};
    if (change == 0) settings.options &= ~LINEA_MICRA_OBSERVE_STATE;
    if (change == 1) settings.connectionType = static_cast<uint8_t>(MicraConnectionType::API);
    if (change == 2) settings.accountConfigured = false;
    if (change == 3) std::strcpy(settings.selectedSerial, "AFTER");
    service.publishConfig(settings, 2);
    assert(!service.backflush_.status().valid);
    assert(service.backflush_.status().busy() == active);
    service.backflush_.finish(permit.attempt);
    assert(!service.backflush_.status().busy());
  }
}

void MicraCancellationTest::run() {
  outboundScaleConnected.store(true);
  OpenBrewByWeightMicraService service;
  service.task_ = &service;
  LineaMicraPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.username, "test@example.invalid");
  std::strcpy(settings.selectedSerial, "TEST");
  settings.options = LINEA_MICRA_APPLY_TEMPERATURE | LINEA_MICRA_SHUTDOWN_WITH_SCALE;
  service.publishConfig(settings, 1);
  service.publishNetworkState(true, false, false, false);
  LineaMicraRequest request;
  request.type = LineaMicraRequestType::APPLY_TEMPERATURE;
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
  static OpenBrewByWeightMicraService *admitting = &service;
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
    request.type = power ? LineaMicraRequestType::SET_STANDBY
                         : LineaMicraRequestType::APPLY_TEMPERATURE;
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
  OpenBrewByWeightMicraService::WorkBuffer work;
  service.work_ = &work;
  service.clearSession();
  assert(service.abortRequested_.load());
  service.work_ = nullptr;

  for (int event = 0; event < 4; ++event) {
    service.abortRequested_.store(false);
    service.websocketStatus_.state = MicraSocketState::STREAMING;
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
  service.config_.options |= LINEA_MICRA_OBSERVE_STATE;
  service.published_.sampleAtMs = 900;
  service.published_.powerState = LineaMicraPowerState::OFF;
  service.published_.effectiveOn = false;
  service.published_.quality = LineaMicraObservationQuality::CURRENT;
  OpenBrewByWeightMicraService::PendingRequest paused;
  paused.identityGeneration = service.identityGeneration_;
  service.deferObservation(paused, service.published_, LineaMicraError::CANCELED);
  assert(service.pending_.present && service.powerState_.retained());
  const auto held = service.powerState_.effectiveStatus(service.published_, true, 100000);
  assert(!held.effectiveOn && held.quality == LineaMicraObservationQuality::STALE);
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
  request.type = LineaMicraRequestType::OBSERVE_STATE;
  assert(service.queue(request));
  executeHook = [&] { ++executions; };
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 3);
  service.observationSchedule_.armPostEvent(millis() - micra_timing::kPostWakeObservationDelayMs);
  try { service.taskLoop(); } catch (const WorkerStopped &) {}
  assert(executions == 4);
  service.config_.connectionType = static_cast<uint8_t>(MicraConnectionType::API);
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

void MicraCancellationTest::initializationAndGates() {
  OpenBrewByWeightMicraService service;
  service.config_.accountConfigured = true;
  service.config_.options |= LINEA_MICRA_OBSERVE_STATE;
  service.staConnected_.store(true);
  service.snapshotStamp_ = service.observationFence_.stamp(0, service.powerState_.generation());
  service.snapshotPending_ = true;
  unsigned calls = 0;
  executeHook = [&] { ++calls; };
  auto run = [&] { try { service.taskLoop(); } catch (const WorkerStopped &) {} };
  // A manual read and the initial read share one dispatch.
  service.pending_.present = true;
  service.pending_.request.type = LineaMicraRequestType::OBSERVE_STATE;
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
  service.observationSchedule_.armPostEvent(millis() - micra_timing::kPostWakeObservationDelayMs);
  run();
  assert(calls == 2 && !initialSnapshot);

  for (auto type : {LineaMicraRequestType::CONNECT, LineaMicraRequestType::OBSERVE_STATE,
                    LineaMicraRequestType::APPLY_TEMPERATURE, LineaMicraRequestType::SET_STANDBY,
                    LineaMicraRequestType::SET_POWER_ON}) {
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

void MicraCancellationTest::unscaledShotAdmission() {
  assert(!outboundScaleConnected.load() && !outboundScaleInhibited());
  OpenBrewByWeightMicraService service;
  service.task_ = &service;
  service.config_.accountConfigured = true;
  service.config_.connectionType = static_cast<uint8_t>(MicraConnectionType::API);
  service.publishNetworkState(true, false, false, false);
  assert(!service.abortRequested_.load());
  service.pending_.present = true;
  service.pending_.request.type = LineaMicraRequestType::OBSERVE_STATE;
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

void MicraCancellationTest::initializationRetries() {
  OpenBrewByWeightMicraService service;
  service.config_.accountConfigured = true;
  service.config_.options |= LINEA_MICRA_OBSERVE_STATE;
  service.staConnected_.store(true);
  service.websocketStatus_.subscribed = true;
  service.published_.sampleAtMs = 100;
  OpenBrewByWeightMicraService::PendingRequest pending;
  pending.request.type = LineaMicraRequestType::OBSERVE_STATE;
  pending.initialSnapshot = true;
  pending.snapshotStamp = service.observationFence_.stamp(0, service.powerState_.generation());
  unsigned reads = 0;
  dashboardHook = [&](const MicraObservationStamp *stamp) {
    assert(stamp && stamp->powerRevision == pending.snapshotStamp.powerRevision);
    ++reads;
    return reads == 2;
  };
  assert(service.executeObservation(pending));
  assert(reads == 2 && authentications == 2 && retries == 1);  // Renewed session still reads.
  reads = authentications = retries = failures = 0;
  dashboardHook = [&](const MicraObservationStamp *) { ++reads; return false; };
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
  dashboardHook = [&](const MicraObservationStamp *) {
    ++reads;
    service.observationFence_.invalidate();  // Socket failed during HTTP.
    return false;
  };
  assert(service.executeObservation(pending));
  assert(reads == 1 && authentications == 1);
  pending.initialSnapshot = false;
  reads = authentications = 0;
  dashboardHook = [&](const MicraObservationStamp *stamp) {
    assert(!stamp);  // A coalesced post-command read uses the current baseline.
    ++reads;
    return true;
  };
  assert(service.executeObservation(pending));
  assert(reads == 1 && authentications == 1);
  service.config_.connectionType = static_cast<uint8_t>(MicraConnectionType::API);
  assert(service.executeObservation(pending));
  assert(reads == 1 && authentications == 2);  // API renewal optimization is unchanged.
}

void MicraCancellationTest::queuedObservationAndPowerOrdering() {
  OpenBrewByWeightMicraService service;
  service.task_ = &service;
  service.config_.accountConfigured = true;
  service.config_.options |= LINEA_MICRA_SHUTDOWN_WITH_SCALE;
  service.staConnected_.store(true);
  unsigned calls = 0;
  executeHook = [&] { ++calls; };
  auto run = [&] { try { service.taskLoop(); } catch (const WorkerStopped &) {} };
  OpenBrewByWeightMicraService::PendingRequest deferred;
  deferred.present = deferred.initialSnapshot = true;
  deferred.request.type = LineaMicraRequestType::OBSERVE_STATE;
  service.deferObservation(deferred, service.published_, LineaMicraError::CANCELED);
  run();
  LineaMicraRequest refresh;
  refresh.type = LineaMicraRequestType::OBSERVE_STATE;
  assert(service.queue(refresh));
  run();
  const bool freshManualRead = !initialSnapshot;

  // Wake before subscription; contrary push then advances the power revision.
  service.powerState_.reset();
  service.published_.sampleAtMs = 1;
  service.published_.powerState = LineaMicraPowerState::OFF;
  service.published_.quality = LineaMicraObservationQuality::CURRENT;
  const auto wakeAt = millis() - micra_timing::kPostWakeObservationDelayMs;
  assert(service.powerState_.notePhysicalStart(service.published_, true, true, wakeAt));
  service.observationSchedule_.armPostEvent(wakeAt);
  service.snapshotStamp_ = service.observationFence_.stamp(0, service.powerState_.generation());
  service.snapshotPending_ = true;
  MicraObservation contrary;
  contrary.source = MicraObservationSource::WEBSOCKET;
  contrary.stamp = service.snapshotStamp_;
  contrary.receivedAtMs = millis() - 1;
  contrary.powerPresent = true;
  contrary.mode = LineaMicraObservedMode::STANDBY;
  assert(service.observationFence_.merge(service.published_, service.powerState_, contrary, true));
  run();
  const bool currentReconciliation = !initialSnapshot;

  LineaMicraRequest power;
  power.type = LineaMicraRequestType::SET_STANDBY;
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

void MicraCancellationTest::lateResponseAndResultLabels(OpenBrewByWeightMicraService &service) {
  performAdvanceMs = 20000;  // A complete response after the deadline is used.
  progress(service, ESP_OK, false, false);
  performAdvanceMs = 0;
  testNowMs = 1000;
  assert(std::strcmp(micraCloudCallResult(false, false, false, 401), "http_error") == 0);
  assert(std::strcmp(micraCloudCallResult(false, false, false, 0), "transport_error") == 0);
  assert(std::strcmp(micraCloudCallResult(false, true, false, 200), "response_too_large") == 0);
  assert(std::strcmp(micraCloudCallResult(false, false, true, 200), "success") == 0);
}

void MicraCancellationTest::invalidAuthLatchesApiObservation() {
  OpenBrewByWeightMicraService service;
  service.task_ = &service;
  LineaMicraPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.selectedSerial, "TEST");
  settings.options = LINEA_MICRA_OBSERVE_STATE;
  settings.connectionType = static_cast<uint8_t>(MicraConnectionType::API);
  service.publishConfig(settings, 1);
  LineaMicraStatus status = service.published_;
  status.error = LineaMicraError::INVALID_AUTH;
  service.publish(status);
  service.scheduleAutomatic(millis(), false);
  assert(!service.observationSchedule_.automaticDue(millis() + 3600000));
  status.error = LineaMicraError::NONE;  // A manual refresh succeeded.
  service.publish(status);
  service.scheduleAutomatic(millis(), false);
  assert(service.observationSchedule_.automaticDue(millis() + 3600000));
  status.error = LineaMicraError::INVALID_AUTH;
  service.publish(status);
  service.scheduleAutomatic(millis(), true);
  std::strcpy(settings.selectedSerial, "OTHER");  // Editing the account re-arms.
  service.publishConfig(settings, 2);
  assert(service.observationSchedule_.automaticDue(millis()));
}

// Changing machine re-lists on the saved key and live session; nothing
// observable changes until the chosen machine is saved.
void MicraCancellationTest::relistKeepsSavedMachine() {
  OpenBrewByWeightMicraService service;
  LineaMicraPersistedSettings settings;
  settings.accountConfigured = true;
  std::strcpy(settings.username, "test@example.invalid");
  std::strcpy(settings.password, "secret");
  settings.installationPrivateKey[0] = 7;
  std::strcpy(settings.selectedSerial, "SAVED");
  std::strcpy(settings.selectedName, "Saved");
  assert(validLineaMicraSettings(settings));
  service.publishConfig(settings, 1);
  assert(!service.queueRelist(1));  // No worker yet.
  service.task_ = &service;
  service.publishNetworkState(true, false, false, false);
  const LineaMicraStatus before = service.published_;
  const uint32_t identity = service.identityGeneration_;
  const auto take = [&] {
    OpenBrewByWeightMicraService::PendingRequest pending = service.pending_;
    service.pending_.present = false;
    return pending;
  };
  assert(service.queueRelist(2));
  assert(!service.queueRelist(3));
  assert(service.discovery_.listing && service.discovery_.count == 0);
  auto pending = take();
  assert(pending.request.type == LineaMicraRequestType::LIST_MACHINES);
  assert(sameSessionIdentity(pending.credentials, settings));
  listHook = [](LineaMicraDiscoverySnapshot &found) {
    found.count = 2;
    std::strcpy(found.machines[0].serial, "SAVED");
    std::strcpy(found.machines[1].serial, "OTHER");
    std::strcpy(found.machines[1].name, "Other");
    return true;
  };
  lastRegisterKey = true;
  service.executeRelist(pending);
  assert(!lastRegisterKey);
  LineaMicraDiscoverySnapshot listed = service.discovery_;
  assert(!listed.listing && listed.error == LineaMicraError::NONE &&
         listed.count == 2 && listed.accountMachines == 2 && listed.requestId == 2);
  assert(service.identityGeneration_ == identity &&
         service.published_.phase == before.phase &&
         service.published_.requestId == before.requestId &&
         std::strcmp(service.config_.selectedSerial, "SAVED") == 0);
  LineaMicraPersistedSettings chosen;
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
  listHook = [](LineaMicraDiscoverySnapshot &) { return false; };
  service.abortRequested_.store(false);  // The idle task loop consumes it.
  const unsigned retriesBefore = retries;
  service.executeRelist(pending);
  listed = service.discovery_;
  assert(!listed.listing && listed.error == LineaMicraError::HTTP_ERROR &&
         listed.count == 0 && listed.accountMachines == 2 &&
         retries - retriesBefore == micra_timing::kRetryDelaysMs.size());

  // One dropped connection is retried instead of leaving Change empty.
  assert(service.queueRelist(4));
  pending = take();
  listHook = [calls = 0](LineaMicraDiscoverySnapshot &found) mutable {
    return ++calls > 1 && (found.count = 1) != 0;
  };
  service.executeRelist(pending);
  listed = service.discovery_;
  assert(!listed.listing && listed.error == LineaMicraError::NONE && listed.count == 1);

  assert(service.queueRelist(5));
  pending = take();
  service.publishConfig(LineaMicraPersistedSettings{}, 3);  // Disconnected meanwhile.
  listHook = [](LineaMicraDiscoverySnapshot &found) { return (found.count = 2) != 0; };
  service.executeRelist(pending);
  listed = service.discovery_;
  assert(!listed.listing && listed.count == 0 && listed.accountMachines == 0);
  assert(!service.queueRelist(6));
  authentications = retries = 0;
}

// WebSocket mode never carries an idle HTTP connection into the next
// operation; API polling keeps it for the reuse window.
void MicraCancellationTest::idleClosesWebSocketModeClient() {
  for (const auto type : {MicraConnectionType::WEBSOCKET, MicraConnectionType::API}) {
    OpenBrewByWeightMicraService service;
    LineaMicraPersistedSettings settings;
    settings.connectionType = static_cast<uint8_t>(type);
    service.publishConfig(settings, 1);
    service.clearSessionRequested_.store(false);
    OpenBrewByWeightMicraService::WorkBuffer work;
    work.client = &work;
    service.work_ = &work;
    try { service.taskLoop(); } catch (const WorkerStopped &) {}
    assert((work.client == nullptr) == (type == MicraConnectionType::WEBSOCKET));
    service.work_ = nullptr;
  }
}

int main() {
  MicraCancellationTest::relistKeepsSavedMachine();
  MicraCancellationTest::idleClosesWebSocketModeClient();
  MicraCancellationTest::retireBackflushConfiguration();
  MicraCancellationTest::invalidAuthLatchesApiObservation();
  MicraCancellationTest::run();
  MicraCancellationTest::initializationAndGates();
  MicraCancellationTest::unscaledShotAdmission();
  MicraCancellationTest::initializationRetries();
  MicraCancellationTest::queuedObservationAndPowerOrdering();
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

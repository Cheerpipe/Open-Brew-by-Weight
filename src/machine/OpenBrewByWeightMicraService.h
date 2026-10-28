#pragma once

#include "OpenBrewByWeightLineaMicraSettings.h"
#include "OpenBrewByWeightLineaMicraTypes.h"
#include "OpenBrewByWeightMachineIntegration.h"
#include "OpenBrewByWeightMicraCompanion.h"
#include "OpenBrewByWeightMicraPowerState.h"
#include "OpenBrewByWeightMicraObservation.h"
#include "OpenBrewByWeightMicraWebSocket.h"
#include "OpenBrewByWeightMicraTiming.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightBootHeap.h"
#include "OpenBrewByWeightTaskMutex.h"
#include "OpenBrewByWeightOutboundAdmission.h"

#include <atomic>
#include <Arduino.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace openbrewbyweight {

class OpenBrewByWeightMicraService {
 public:
  bool begin();
  void inhibitCloud() {
    const bool shot = outboundShotActive.load(std::memory_order_acquire);
    const bool pause = !shot || outboundScaleConnected.load(std::memory_order_acquire);
    if (shot) {
      shotGeneration_.fetch_add(1, std::memory_order_acq_rel);
      if (pause) disconnectGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }
    if (pause || powerActive_.load() || temperatureActive_.load())
      abortRequested_.store(true, std::memory_order_release);
    if (task_ != nullptr) xTaskNotifyGive(task_);
  }
  void publishConfig(const LineaMicraPersistedSettings &settings,
                     uint32_t configGeneration);
  void publishNetworkState(bool staConnected, bool apActive, bool shotActive,
                           bool scaleConnecting);
  bool queueConnect(uint32_t requestId, const char *username,
                    const char *password);
  bool queueRelist(uint32_t requestId);
  bool queue(const LineaMicraRequest &request);
  // Control task only, for temperature and power commands: never waits on
  // mux_. Contention defers the command to the task's next successful lock,
  // latest wins per kind, as queue() itself would keep only the newest.
  void post(const LineaMicraRequest &request);
  bool selectDiscoveredMachine(const char *serial,
                               LineaMicraPersistedSettings &settings);
  void clearDiscovery();
  LineaMicraStatus status() const;
  // Control task only: never waits on mux_ — tryLocks, refreshes the
  // task-owned cache, and serves the last snapshot on contention.
  LineaMicraStatus statusPoll() const;
  // True when that served cache is older than the poll staleness bound.
  bool statusPollStale() const;
  LineaMicraCloudCall cloudCall() const;
  HeapLifecycleAggregate heapTelemetry() const;
  MicraWebSocketStatus websocketStatus(bool includeTraffic = true) const;
  LineaMicraDiscoverySnapshot discovery() const;
  // Control task only: physicalStart() and finishBackflush() never wait on
  // mux_ either (see their definitions for the contended resolution).
  MachinePhysicalStartDisposition physicalStart(MachineBackflushPermit *permit = nullptr);
  MachineBackflushSnapshot backflush(bool consume = false);
  void finishBackflush(uint32_t attempt);
  // Companion route (docs/COMPANION_LINK.md): the link calls these from the
  // HTTP or network task; each takes mux_ briefly and never logs under it.
  void companionLink(bool ready, bool lmSwitch);
  void companionPing(bool upstreamAlive);
  void companionUpstream(bool subscribed, uint32_t epoch, uint32_t tokenRenewInS);
  void companionCredentials(uint32_t generation, bool ok);
  bool companionState(MicraCompanionStateKind kind, uint32_t epoch,
                      uint32_t afterCommand, const cJSON *dashboard);
  void companionCommandResult(uint32_t id, bool accepted, uint16_t httpStatus);
  MicraCompanionOutbox companionOutbox() const;
  MicraCompanionRouteStatus companionRouteStatus() const;
  bool companionCopyCredentials(uint32_t generation, char *email, size_t emailCapacity,
                                char *password, size_t passwordCapacity, char *serial,
                                size_t serialCapacity);
  void setCompanionNotify(void (*notify)()) { companionNotify_.store(notify); }

 private:
#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  friend struct MicraCancellationTest;
  friend struct MicraWebSocketTest;
#endif
  struct IoBuffer;
  struct WorkBuffer;
  struct WebSocketBuffer;
  struct RequestStateGuard;
  struct PendingRequest {
    LineaMicraRequest request = {};
    LineaMicraPersistedSettings credentials = {};
    uint32_t identityGeneration = 0;
    MicraObservationStamp snapshotStamp;
    bool initialSnapshot = false;
    bool present = false;
  };
  struct DesiredTemperature {
    LineaMicraRequest request = {};
    uint32_t machineConfigGeneration = 0;
    uint32_t retryAtMs = 0;
    uint8_t cycles = 0;  // Exhausted cycles while the machine was online.
    bool present = false;
    bool commandAccepted = false;
  };
  struct DesiredPower {
    LineaMicraRequest request = {};
    uint32_t machineConfigGeneration = 0;
    uint32_t shotGeneration = 0;
    uint32_t retryAtMs = 0;
    uint8_t cycles = 0;  // Exhausted cycles while the machine was online.
    bool present = false;
    bool commandAccepted = false;
  };

  static void taskEntry(void *context);
  static esp_err_t httpEvent(esp_http_client_event_t *event);
  void taskLoop();
  static void websocketEvent(void *context, const char *, int32_t event, void *data);
  static bool stompFrame(void *context, const struct MicraStompFrame &frame);
  bool websocketAdmitted(bool starting = false) const;
  bool shotTransportPaused() const {
    return outboundScaleConnected.load(std::memory_order_acquire) &&
        (outboundShotActive.load(std::memory_order_acquire) ||
         shotActive_.load(std::memory_order_acquire));
  }
  bool sendSubscription();
  void applyPushLocked(const MicraObservation &update, uint32_t ingressAttempt);
  bool mergeReadLocked(const MicraObservation &update);
  void serviceWebSocket();
  void stopWebSocket(bool release = false);
  bool serviceCompanion();
  bool companionCommand(MicraCompanionCommandOp op, uint16_t value);
  void leaveCompanionLocked(const char *reason, bool loss);
  void dropCompanionFeedLocked();
  bool companionFeedLiveLocked() const;
  uint32_t companionIntentLocked(uint32_t afterCommand) const;
  void recordCompanionIntentLocked(uint32_t command);
  void refreshCompanionConfigLocked();
  void notifyCompanion() const {
    if (auto notify = companionNotify_.load()) notify();
  }
  static void logCompanionRoute(MicraCompanionRoute from, MicraCompanionRoute to,
                                const char *reason);
  void waitRetry(uint32_t delayMs);
  void execute(PendingRequest &pending);
  bool executeConnect(PendingRequest &pending);
  void executeRelist(PendingRequest &pending);
  bool executeObservation(PendingRequest &pending);
  bool executeTemperatureApplication(const LineaMicraRequest &request,
                                     uint32_t machineConfigGeneration);
  bool executePowerApplication(const LineaMicraRequest &request,
                               uint32_t machineConfigGeneration);
  bool ensureSession(LineaMicraPersistedSettings &settings, bool registerKey,
                     bool *renewed = nullptr);
  bool generateInstallationKey(LineaMicraPersistedSettings &settings);
  bool registerInstallation(const LineaMicraPersistedSettings &settings);
  bool signIn(const LineaMicraPersistedSettings &settings);
  bool refreshToken(const LineaMicraPersistedSettings &settings);
  bool listMachines(const LineaMicraPersistedSettings &settings,
                    LineaMicraDiscoverySnapshot &result);
  bool readDashboard(const LineaMicraPersistedSettings &settings,
                     LineaMicraStatus &result,
                     const MicraObservationStamp *snapshot = nullptr);
  bool writeTemperature(const LineaMicraPersistedSettings &settings,
                        uint16_t targetDeciC);
  bool writeStandby(const LineaMicraPersistedSettings &settings);
  bool writePowerOn(const LineaMicraPersistedSettings &settings);
  bool request(const LineaMicraPersistedSettings &settings, const char *url,
               esp_http_client_method_t method, const char *body,
               bool authenticated, const char *purpose, const char *endpoint,
               bool installationInit = false);
  LineaMicraError classifyFailure() const;
  bool applySignedHeaders(const LineaMicraPersistedSettings &settings,
                          char *headers = nullptr, size_t capacity = 0);
  void clearRequestState();
  bool networkEligible(LineaMicraError &error, bool observation = false) const;
  LineaMicraStatus statusLocked() const;
  bool liveSubscriptionLocked() const;
  bool pushConfirms(uint16_t targetDeciC, LineaMicraPowerState power);
  bool queueLocked(const LineaMicraRequest &request);
  void qualifyBackflushLocked();
  bool backflushReadyLocked() const;
  MachinePhysicalStartDisposition physicalStartLocked(
      MachineBackflushPermit *permit);
  // Anomaly gating runs while the caller holds mux_ (which serializes the
  // latch); reportMuxWaitAnomaly must run after unlock so logging never
  // nests inside mux_, with the holder attribution gathered under the lock.
  static bool muxWaitAnomalyDue(uint32_t waitedUs, uint32_t &lastReportAtMs,
                                uint32_t nowMs);
  static void reportMuxWaitAnomaly(uint32_t waitedUs, const char *holder,
                                   uint32_t holdUs);
  bool ensureIoBuffer();
  bool ensureWorkBuffer();
  void clearSession();
  void releaseIoBuffer(bool responseValid = true);
  void releaseWorkBuffer();
  void publish(const LineaMicraStatus &status);
  void deferObservation(const PendingRequest &pending,
                        LineaMicraStatus status, LineaMicraError reason);
  void fail(LineaMicraStatus &status, LineaMicraError error);
  void scheduleAutomatic(uint32_t now, bool failed);
  bool temperatureRequestCurrent(const LineaMicraRequest &request,
                                 uint32_t machineConfigGeneration) const;
  bool powerRequestCurrent(const LineaMicraRequest &request,
                           uint32_t machineConfigGeneration) const;
  bool identityCurrent(uint32_t identityGeneration) const;
  bool observationCurrent(uint32_t identityGeneration) const;
  bool temperatureEligible(LineaMicraError &error) const;
  void deferTemperature(const LineaMicraRequest &request,
                        LineaMicraError error, uint32_t delayMs,
                        bool retryable = true, bool exhausted = false);
  void deferPower(const LineaMicraRequest &request,
                  LineaMicraError error, uint32_t delayMs,
                  bool retryable = true, bool exhausted = false);

  mutable TaskMutex mux_;
  LineaMicraPersistedSettings config_ = {};
  LineaMicraPersistedSettings candidate_ = {};
  PendingRequest pending_ = {};
  DesiredTemperature desiredTemperature_ = {};
  DesiredPower desiredPower_ = {};
  LineaMicraStatus published_ = {};
  LineaMicraCloudCall pendingCloudCall_ = {};  // Cloud worker only.
  LineaMicraCloudCall publishedCloudCall_ = {};  // Protected by mux_.
  HeapLifecycleTracker tlsHeap_ = {};
  LineaMicraDiscoverySnapshot discovery_ = {};
  uint32_t configGeneration_ = 0;
  uint32_t identityGeneration_ = 0;
  uint32_t nextAutomaticRequestId_ = 0x80000000UL;
  micra_timing::ObservationSchedule observationSchedule_;
  LineaMicraPowerStateTracker powerState_;
  MicraObservationFence observationFence_;
  MachineBackflushObservations backflush_;
  MachineBackflushSnapshot idleBackflush_;  // Control task only; see backflush().
  // Control task only, like idleBackflush_: backflush()'s slow path stores
  // its last locked snapshot here and the same task reads it on tryLock
  // misses, with count zeroed so no stale edges are re-served.
  MachineBackflushSnapshot lastPollSnapshot_;
  // Control task only, like lastPollSnapshot_: mutations a busy mux_ deferred
  // from post() and finishBackflush(), applied first at the task's next lock.
  LineaMicraRequest postedTemperature_ = {}, postedPower_ = {};
  uint32_t postedFinishAttempt_ = 0;  // 0: no release pending
  bool postedTemperaturePresent_ = false, postedPowerPresent_ = false;
  bool postedPending() const {
    return postedTemperaturePresent_ || postedPowerPresent_ ||
           postedFinishAttempt_ != 0;
  }
  void applyPostedLocked();
  // Control task only, like idleBackflush_: statusPoll() writes both under
  // mux_ and the same task reads them, so nothing else guards them.
  mutable LineaMicraStatus statusCache_ = {};
  mutable uint32_t statusCacheWrittenAtMs_ = 0;
  // Mirrors published_.powerState == OFF for the lock-free poll; written
  // under mux_ wherever published_.powerState changes.
  std::atomic<bool> powerOff_{false};
  void mirrorPowerLocked() {
    powerOff_.store(published_.powerState == LineaMicraPowerState::OFF,
                    std::memory_order_release);
  }
  std::atomic<uint32_t> tokenIssuedAtMs_{0};
  std::atomic<bool> tokenAvailable_{false};
  MicraObservationStamp snapshotStamp_;  // Protected by mux_, captured at subscription.
  bool snapshotPending_ = false;
  bool active_ = false;
  TaskHandle_t task_ = nullptr;
  IoBuffer *io_ = nullptr;
  WorkBuffer *work_ = nullptr;
  WebSocketBuffer *websocket_ = nullptr;
  MicraWebSocketStatus websocketStatus_;
  bool websocketUnexpectedReconnect_ = false;  // Protected by mux_; survives pauses.
  uint8_t websocketAuthRecoveries_ = 0;  // Cloud owner; survives workspace parking.
  std::atomic<bool> websocketRetryRequested_{false};
  std::atomic<bool> staConnected_{false};
  std::atomic<bool> apActive_{false};
  std::atomic<bool> shotActive_{false};
  std::atomic<bool> scaleConnecting_{false};
  std::atomic<bool> observationActive_{false};
  std::atomic<bool> temperatureActive_{false};
  std::atomic<bool> powerActive_{false};
  std::atomic<bool> abortRequested_{false};
  std::atomic<uint32_t> disconnectGeneration_{0};
  std::atomic<uint32_t> shotGeneration_{0};
  std::atomic<bool> clearSessionRequested_{false};
  bool wasNetworkReady_ = false;
  // Companion route. Protected by mux_; the cloud task alone moves DIRECT to
  // PENDING, the full frame moves PENDING to COMPANION, and any side returns
  // to DIRECT. Without a link the route stays DIRECT and nothing here runs.
  struct CompanionFeed {
    MicraCompanionOutbox outbox;
    MicraCompanionRouteStatus status;
    MicraObservationStamp stamp;  // Basis set by the last full frame.
    struct Intent { uint32_t command = 0, generation = 0; } intents[8];
    uint32_t losses[4] = {};
    uint32_t routeIntent = 0, epoch = 0, requestedAtMs = 0, pingAtMs = 0;
    uint32_t upAtMs = 0, downSinceMs = 0, tokenRenewAtMs = 0, flapUntilMs = 0;
    uint32_t lastCommand = 0, resultId = 0;
    uint16_t resultHttp = 0;
    uint8_t nextIntent = 0, nextLoss = 0, lossCount = 0;
    bool lmSwitch = false, up = false, resultAccepted = false, flapping = false;
  } companion_;
  // Lock-free mirrors: no link means the cloud task skips the route entirely,
  // and command paths read the route without taking mux_.
  std::atomic<bool> companionLink_{false};
  std::atomic<bool> companionRoute_{false};
  std::atomic<void (*)()> companionNotify_{nullptr};
};

}  // namespace openbrewbyweight

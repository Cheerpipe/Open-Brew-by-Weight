#pragma once
#include "OpenBrewByWeightMicraObservation.h"
#include "OpenBrewByWeightPsram.h"

namespace openbrewbyweight {
enum class MicraSocketState : uint8_t {
  Disabled, WAITING_FOR_NETWORK, WAITING_FOR_TIME, PAUSED, AUTHENTICATING,
  CONNECTING, STOMP_CONNECTING, WAITING_FOR_DATA, STREAMING, STOPPING,
  BACKOFF, AUTH_ERROR
};
inline const char *micraSocketStateName(MicraSocketState state) {
  static constexpr const char *names[] = {
      "disabled", "waiting_for_network", "waiting_for_time", "paused",
      "authenticating", "connecting", "stomp_connecting", "waiting_for_data",
      "streaming", "stopping", "backoff", "auth_error"};
  return names[static_cast<unsigned>(state)];
}
struct MicraWebSocketStatus {
  MicraSocketState state = MicraSocketState::Disabled;
  const char *reason = "disabled";
  uint32_t epoch = 0, connectedAtMs = 0, messageAtMs = 0, powerAtMs = 0;
  uint32_t pongAtMs = 0, retryAtMs = 0, pauseAtMs = 0;
  uint32_t stoppedLatencyMs = 0, maxStoppedLatencyMs = 0;
  uint64_t receiveBytes = 0, transmitBytes = 0;
  uint32_t messages = 0;
  uint32_t reconnects = 0, errors = 0, allocationFailures = 0;
  uint32_t plannedConnections = 0, unexpectedConnections = 0;
  uint32_t receivePerSecond = 0, transmitPerSecond = 0;
  uint32_t receivePerMinute = 0, transmitPerMinute = 0;
  uint32_t publishLatencyMs = 0, maxPublishLatencyMs = 0;
  uint32_t cleaningAtMs = 0, retainedBytes = 0;
  uint32_t internalMinimum = 0, psramMinimum = 0, psramFree = 0, psramLargest = 0;
  MicraCleaningState cleaning = MicraCleaningState::UNKNOWN;
  char cleaningLabel[33] = {};
  bool subscribed = false, machineConnectedKnown = false, machineConnected = false;
  bool cleaningAvailable = false, workspaceExternal = false;
  HeapLifecycleAggregate heap = {};
  HeapLifecycleAggregate stopHeap = {};
};
}  // namespace openbrewbyweight

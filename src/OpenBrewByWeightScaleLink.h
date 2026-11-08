#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "OpenBrewByWeightScaleTypes.h"

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include "../libraries/EspressoScaleBLE/src/ScaleFeatures.h"
#else
#include <ScaleFeatures.h>
#endif

namespace openbrewbyweight {

// =============================================================================
// LAYER: Scale link port (worker ↔ orchestrator)
// =============================================================================
// WHAT: Command/event/snapshot types for the BLE scale worker. The worker
//       owns the radio, ATT writes, and bounded ordered weight handoff. The
//       orchestrator consumes events and decides brew.
//
// BOUNDARY: No CycleSession, stopper state, cup, or brew policy.

enum class ScaleLinkState : uint8_t { DISCONNECTED, CONNECTED };

enum class ScaleCommandType : uint8_t {
  START_TIMER_AND_TARE,
  TARE_ONLY,
  STOP_TIMER
};

enum class ScaleEventType : uint8_t {
  WEIGHT,
  TIMER_START_RESULT,
  TARE_RESULT,
  TIMER_STOP_RESULT,
  REFERENCE_CHANGED
};

inline const char *scaleEventTypeName(ScaleEventType type) {
  switch (type) {
    case ScaleEventType::WEIGHT: return "weight";
    case ScaleEventType::TIMER_START_RESULT: return "timer_start_result";
    case ScaleEventType::TARE_RESULT: return "tare_result";
    case ScaleEventType::TIMER_STOP_RESULT: return "timer_stop_result";
    case ScaleEventType::REFERENCE_CHANGED: return "reference_changed";
  }
  return "unknown";
}

inline const char *scaleCommandTypeName(ScaleCommandType type) {
  switch (type) {
    case ScaleCommandType::START_TIMER_AND_TARE: return "start_timer_and_tare";
    case ScaleCommandType::TARE_ONLY: return "tare_only";
    case ScaleCommandType::STOP_TIMER: return "stop_timer";
  }
  return "unknown";
}

// Worker-owned command lifetime. Terminal state survives a dropped result.
enum class IdleTarePhase : uint8_t { NONE, QUEUED, WRITING, SUCCEEDED, FAILED };
enum class IdleTareReason : uint8_t {
  NONE, FEATURE_DISABLED, NOT_READY, ACTIVE_CYCLE, MAINTENANCE, RELAY_BLOCKED,
  STOP_STATE, MACHINE_NOT_OFF, SCALE_UNAVAILABLE, NO_ABSENCE, UNSUPPORTED,
  QUEUE_FULL, EXPIRED, REMOVED, CONFIG_CHANGED, STALE_SAMPLE, UNSTABLE,
  WRITE_FAILED, EFFECT_CONFIRMED, EFFECT_UNCONFIRMED, START_REQUEST,
  CONNECTION_CHANGED, SAMPLE_GAP, SLOT_BUSY
};
inline const char *idleTareReasonName(uint8_t reason) {
  static const char *const names[] = {
      "none", "disabled", "not_ready", "active_cycle", "maintenance",
      "relay_blocked", "stop_state", "machine_not_off", "scale_unavailable",
      "no_absence", "unsupported", "queue_full", "expired", "removed",
      "config_changed", "stale_sample", "unstable", "write_failed",
      "effect_confirmed", "effect_unconfirmed", "start_request",
      "connection_changed", "sample_gap", "slot_busy"};
  return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "unknown";
}
// What a tare write claims a queued idle request against: the approved
// packet it expects (0 accepts any) and the capture sequence it starts after.
struct IdleTareClaim {
  uint32_t expectedPacketSequence = 0;
  uint32_t captureBoundary = 0;
};
struct IdleTareStatus {
  uint32_t requestId = 0;
  uint32_t connectionGeneration = 0;
  uint32_t approvedPacketSequence = 0;
  uint32_t startedAtMs = 0;
  uint32_t captureBoundary = 0;
  uint32_t writtenAtMs = 0;
  float preTareWeightG = NAN;
  IdleTarePhase phase = IdleTarePhase::NONE;
  IdleTareReason reason = IdleTareReason::NONE;
};

// Shared Web/profiler presentation; these codes do not authorize a tare.
enum IdleTarePresentation : uint8_t {
  IDLE_WAITING_FOR_SETTLE = 32, IDLE_READY_FOR_CUP, IDLE_PENDING, IDLE_TARED,
  IDLE_REMOVE, IDLE_RETRY, IDLE_UNCERTAIN
};
inline uint8_t idleTarePresentationCode(const CupTareDiagnostics &tare, bool present) {
  if (tare.eligibilityReason != 0) return tare.eligibilityReason;
  return tare.requestId != 0 ? IDLE_PENDING
      : !tare.referenceKnown || tare.emptyReferenceBlocked ? IDLE_UNCERTAIN
      : present ? (tare.tared ? IDLE_TARED
          : tare.requestPlacementId == tare.placementId && tare.lastTerminalRequestId != 0 ? IDLE_RETRY : IDLE_REMOVE)
      : tare.absentObserved ? IDLE_READY_FOR_CUP : IDLE_WAITING_FOR_SETTLE;
}
inline const char *idleTarePresentationName(uint8_t code) {
  static const char *const names[] = {"waiting_for_settle", "ready_for_cup", "pending", "tared",
                                     "remove", "retry", "uncertain"};
  return code < IDLE_WAITING_FOR_SETTLE ? idleTareReasonName(code)
      : code <= IDLE_UNCERTAIN ? names[code - IDLE_WAITING_FOR_SETTLE] : "unknown";
}

struct ScaleCommand {
  uint32_t cupWeightRequestId = 0;
  ScaleCommandType type = ScaleCommandType::STOP_TIMER;
  uint32_t cycleId = 0;
  uint32_t connectionGeneration = 0;
  uint32_t idleTareRequestId = 0;
  uint32_t expiresAtMs = 0;
  uint32_t qualifiedPacketSequence = 0;
  bool autoTare = false;
  bool canTareStartTimer = false;
  bool commandFeedbackExpected = false;
};

struct ScaleEvent {
  uint32_t cupWeightRequestId = 0;
  ScaleEventType type = ScaleEventType::WEIGHT;
  uint32_t cycleId = 0;
  uint32_t idleTareRequestId = 0;
  uint32_t receivedAtMs = 0;
  uint32_t connectionGeneration = 0;
  uint32_t disconnectSequence = 0;
  uint32_t packetSequence = 0;
  uint32_t captureSequence = 0;
  float weightG = 0.0f;
  float preTareWeightG = NAN;
  bool sampleDiscontinuity = false;
  bool commandAttempted = false;
  bool writeSucceeded = false;
  bool tareAttempted = false;
  bool tareSucceeded = false;
  bool usedCombinedTareStart = false;
  bool commandFeedbackExpected = false;
  bool discardedStaleConnection = false;
};

struct ScaleLinkSnapshot {
  ScaleLinkState state = ScaleLinkState::DISCONNECTED;
  bool connecting = false;
  uint32_t disconnectSequence = 0;
  uint32_t connectionGeneration = 0;
  uint32_t packetSequence = 0;
  uint32_t packetGaps = 0;
  uint32_t maxPacketGapMs = 0;
  uint32_t weightUpdateIntervalMs = 0;
  uint32_t rejectedPackets = 0;
  uint32_t reconnects = 0;
  uint8_t lastDisconnectReason = 0;
  ScaleBleDiagnostics bleDiagnostics = {};
  uint32_t workerProgressAtMs = 0;
  bool timerValid = false;
  uint32_t timerMs = 0;
  uint32_t timerAgeMs = 0;
  // Latest scale-reported flow (ScaleFeatureFlowRate) and its frame time.
  bool flowRateValid = false;
  float flowRateGps = 0.0f;
  uint32_t flowRateAtMs = 0;
  char protocolName[20] = {};
  char connectedMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char shotScaleName[SCALE_FRIENDLY_NAME_MAX_LEN + 1] = {};
  ScaleModel model = ScaleModel::Unknown;
  ScaleFeatureSet features = {};
  // Static library strings of the connected protocol; nulls while down.
  ScaleStockTones stockTones = {};
  bool rssiValid = false;
  int8_t rssi = 0;
  // Display-only scale battery (ScaleFeatureBatteryLevel), never a control input.
  bool batteryValid = false;
  uint8_t batteryPct = 0;
};

}  // namespace openbrewbyweight

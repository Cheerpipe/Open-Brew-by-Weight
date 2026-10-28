#pragma once

#include <cmath>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../libraries/EspressoScaleBLE/src/ScaleBleTypes.h"
#include "../libraries/EspressoScaleBLE/src/ScaleProtocol.h"

namespace openbrewbyweight {

// Compile-time string equality, for the *_COUNT checks of the name tables.
constexpr bool constexprTextEquals(const char *a, const char *b) {
  return *a == *b && (*a == '\0' || constexprTextEquals(a + 1, b + 1));
}

// Single source of truth for scale disconnect reason labels, mirroring the
// BLE library's ScaleDisconnectReason by code without coupling the domain,
// network, or serial layers to the NimBLE implementation. API consumers get
// SCREAMING_CASE names; serial/debug consumers get human-readable labels.
struct ScaleDisconnectReasonLabel {
  uint8_t code;
  const char *apiName;
  const char *debugName;
};

inline constexpr ScaleDisconnectReasonLabel kScaleDisconnectReasonLabels[] = {
    {0, "NONE", "none"},
    {1, "USER_REQUEST", "user request"},
    {2, "SCAN_START_FAILED", "scan start failed"},
    {3, "SCAN_TIMEOUT", "scan timeout"},
    {4, "CONNECT_FAILED", "connect failed"},
    {5, "DISCOVERY_FAILED", "discovery failed"},
    {6, "UNSUPPORTED_SCALE", "unsupported scale"},
    {7, "SUBSCRIBE_FAILED", "subscribe failed"},
    {8, "INITIALIZATION_WRITE_FAILED", "initialization write failed"},
    {9, "REMOTE_DISCONNECTED", "remote disconnected"},
    {10, "FIRST_PACKET_TIMEOUT", "first packet timeout"},
    {11, "PACKET_TIMEOUT", "packet timeout"},
    {12, "INVALID_PACKET_STREAM", "invalid packet stream"},
    {13, "COMMAND_WRITE_FAILED", "command write failed"},
    {14, "SUPERVISION_TIMEOUT", "supervision timeout"},
    {15, "CONNECTION_FAILED_TO_ESTABLISH",
     "connection failed to be established"},
    {16, "RX_QUEUE_OVERFLOW", "rx queue overflow"},
    {17, "EVENT_QUEUE_OVERFLOW", "event queue overflow"},
    {18, "HOST_RESET", "host reset"},
    {19, "OPERATION_TIMEOUT", "operation timeout"},
    {20, "MBUF_ALLOCATION_FAILED", "mbuf allocation failed"},
};

inline const char *scaleDisconnectReasonName(uint8_t reason) {
  for (const auto &label : kScaleDisconnectReasonLabels) {
    if (label.code == reason) return label.apiName;
  }
  return "UNKNOWN";
}

inline const char *scaleDisconnectReasonDebugName(int32_t reason) {
  for (const auto &label : kScaleDisconnectReasonLabels) {
    if (label.code == reason) return label.debugName;
  }
  return "unknown";
}

inline int32_t weightToCentigrams(float weightG) {
  if (!std::isfinite(weightG)) {
    return 0;
  }
  const float centigrams = weightG * 100.0f;
  // float(INT32_MAX) rounds to 2^31; compare before any integer conversion.
  if (centigrams >= static_cast<float>(INT32_MAX)) return INT32_MAX;
  if (centigrams <= static_cast<float>(INT32_MIN)) return INT32_MIN;
  return static_cast<int32_t>(centigrams);
}

inline void formatWeightCentigrams(int32_t centigrams, char *buffer,
                                   size_t capacity) {
  if (buffer == nullptr || capacity == 0) {
    return;
  }
  // Unsigned magnitude also handles INT32_MIN without signed negation.
  const uint32_t magnitude = centigrams < 0
                                 ? 0U - static_cast<uint32_t>(centigrams)
                                 : static_cast<uint32_t>(centigrams);
  snprintf(buffer, capacity, "%s%lu.%02lug", centigrams < 0 ? "-" : "",
           static_cast<unsigned long>(magnitude / 100U),
           static_cast<unsigned long>(magnitude % 100U));
}


// Observational fields shared by the coherent control-status publication.
// Field names remain flat for existing snapshot consumers.
struct ScaleLinkMetrics {
  uint32_t scaleConnectionGeneration = 0;
  uint32_t scalePacketSequence = 0;
  uint32_t scalePacketGaps = 0;
  uint32_t scaleMaxPacketGapMs = 0;
  uint32_t scaleWeightUpdateIntervalMs = 0;
  uint32_t scaleRejectedPackets = 0;
  uint32_t scaleReconnects = 0;
  uint32_t scaleRecoveredStaleCount = 0;
  uint32_t scaleRecoveredStaleMs = 0;
  uint8_t scaleLastDisconnectReason = 0;
  ScaleBleDiagnostics scaleBleDiagnostics = {};
  bool scaleRssiValid = false;
  int8_t scaleRssi = 0;
  uint8_t scaleSignalLevel = 0;
};

// =============================================================================
// LAYER: Scale types (link, weight stream, cup contracts)
// =============================================================================
// WHAT: Scale pairing/MAC modes, weight stream/control states, cup presence
//       enums, and shared scale constants used by sense + BLE link.
//
// BOUNDARY: Scale/cup vocabulary only. Machine paddle/momentary details do not
// belong here. Upper layers (brew, guards, stopper) consume these types; they
// must not push machine-specialization knowledge into this header.

inline void copyCString(char *destination, size_t capacity, const char *source) {
  if (destination == nullptr || capacity == 0) {
    return;
  }
  if (source == nullptr) {
    destination[0] = '\0';
    return;
  }
  // Bound the copy and always NUL-terminate. Prefer memcpy over strncpy so
  // GCC -O2 does not treat intentional truncation as -Wstringop-truncation.
  size_t n = 0;
  while (n < capacity - 1 && source[n] != '\0') ++n;
  memcpy(destination, source, n);
  destination[n] = '\0';
}

constexpr size_t PREFERRED_SCALE_MAC_CAPACITY = 18;
constexpr size_t PREFERRED_SCALE_NAME_CAPACITY = 32;
constexpr size_t SCALE_HISTORY_CAPACITY = 8;
// User-facing rename budget for the Web UI friendly-name override.
constexpr size_t SCALE_FRIENDLY_NAME_MAX_LEN = 30;
constexpr uint32_t SCALE_PAIRING_DISCOVERY_PAUSE_MS = 30000;
// PREFER: wait this long for the preferred MAC before connecting any other.
constexpr uint32_t SCALE_PREFER_FALLBACK_MS = 5000;

enum class ScaleMacCacheMode : uint8_t {
  // Numeric values are stored in NVS.
  FIRST = 0,  // Name scan; first compatible; do not lock preferred.
  ONLY = 1,   // Bootstrap by name if empty; then preferred MAC only.
  PREFER = 2, // Bootstrap if empty; else prefer MAC, then fall back to any.
};

// GAP scan duty while discovering a scale. Numeric values are stored in NVS;
// 0 is Balanced, the factory duty.
enum class BleScanIntensity : uint8_t {
  BALANCED = 0,    // 50% — 31.25 ms / 62.5 ms
  AGGRESSIVE = 1,  // 100% — 20 ms / 20 ms
  RELAXED = 2      // 25% — 28.75 ms / 115 ms
};

inline bool validBleScanIntensity(uint8_t value) {
  return value <= static_cast<uint8_t>(BleScanIntensity::RELAXED);
}

inline BleScanIntensity clampBleScanIntensity(uint8_t value) {
  return validBleScanIntensity(value)
             ? static_cast<BleScanIntensity>(value)
             : BleScanIntensity::BALANCED;
}

inline const char *bleScanIntensityName(BleScanIntensity intensity) {
  switch (intensity) {
    case BleScanIntensity::AGGRESSIVE:
      return "aggressive";
    case BleScanIntensity::RELAXED:
      return "relaxed";
    case BleScanIntensity::BALANCED:
    default:
      return "balanced";
  }
}

// One source for the factory scan intensity: the persisted-settings default,
// the worker's live reset, the serial dump fallback, and factory verification
// must never drift apart.
constexpr BleScanIntensity BLE_SCAN_FACTORY_INTENSITY =
    BleScanIntensity::BALANCED;

// Scan-settings command vocabulary owned by ScaleService: the fields, flag
// bits, and defaults of one Admin ble-scan request. It lives here, with the
// scan policy it configures, rather than in the Domain root so that legacy
// concentration cap keeps shrinking instead of absorbing scan settings.
// `specified` records which optional fields a request carried so each one can
// be applied on its own.
struct BleScanCommandPayload {
  static constexpr uint8_t INTENSITY = 0x01;
  static constexpr uint8_t ENABLED = 0x08;
  uint8_t specified = 0;
  uint8_t intensity = static_cast<uint8_t>(BLE_SCAN_FACTORY_INTENSITY);
  uint8_t enabled = 1;
};

inline bool parseBleScanIntensityId(const char *id, BleScanIntensity &out) {
  if (id == nullptr || id[0] == '\0') {
    return false;
  }
  if (strcmp(id, "aggressive") == 0) {
    out = BleScanIntensity::AGGRESSIVE;
    return true;
  }
  if (strcmp(id, "relaxed") == 0) {
    out = BleScanIntensity::RELAXED;
    return true;
  }
  if (strcmp(id, "balanced") == 0) {
    out = BleScanIntensity::BALANCED;
    return true;
  }
  return false;
}

// BLE-seen scales remembered for the preferred-scale dropdown (NVS + status).
struct ScaleHistoryEntry {
  char mac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  char name[PREFERRED_SCALE_NAME_CAPACITY] = {};
  // User-assigned UI override; empty = show the advertised name.
  char friendlyName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  uint32_t lastSeenSeq = 0;
};

constexpr uint32_t SCALE_HISTORY_SESSION_CONSUMED = 0x80000000UL;
constexpr uint32_t SCALE_HISTORY_SEQUENCE_MASK = 0x7fffffffUL;

inline bool scaleHistorySessionConsumed(const ScaleHistoryEntry &entry) {
  return (entry.lastSeenSeq & SCALE_HISTORY_SESSION_CONSUMED) != 0;
}

inline void clearScaleHistorySessionMarker(ScaleHistoryEntry &entry) {
  entry.lastSeenSeq &= SCALE_HISTORY_SEQUENCE_MASK;
}

inline bool validScaleMacCacheMode(uint8_t mode) {
  return mode == static_cast<uint8_t>(ScaleMacCacheMode::FIRST) ||
         mode == static_cast<uint8_t>(ScaleMacCacheMode::ONLY) ||
         mode == static_cast<uint8_t>(ScaleMacCacheMode::PREFER);
}

inline const char *scaleMacCacheModeId(uint8_t mode) {
  switch (static_cast<ScaleMacCacheMode>(mode)) {
    case ScaleMacCacheMode::FIRST:
      return "first";
    case ScaleMacCacheMode::PREFER:
      return "prefer";
    case ScaleMacCacheMode::ONLY:
      return "only";
    default:
      return "first";
  }
}

inline bool parseScaleMacCacheMode(const char *text, uint8_t &mode) {
  if (text == nullptr) {
    return false;
  }
  if (strcmp(text, "first") == 0) {
    mode = static_cast<uint8_t>(ScaleMacCacheMode::FIRST);
    return true;
  }
  if (strcmp(text, "prefer") == 0) {
    mode = static_cast<uint8_t>(ScaleMacCacheMode::PREFER);
    return true;
  }
  if (strcmp(text, "only") == 0) {
    mode = static_cast<uint8_t>(ScaleMacCacheMode::ONLY);
    return true;
  }
  return false;
}

constexpr uint32_t MAX_AUTOMATION_WEIGHT_AGE_MS = 1000;
constexpr float MIN_AUTOMATION_WEIGHT_G = -500.0f;
constexpr float MAX_AUTOMATION_WEIGHT_G = 1000.0f;
constexpr float MAX_AUTOMATION_WEIGHT_SLEW_G_PER_S = 100.0f;
constexpr float AUTOMATION_WEIGHT_SLEW_ALLOWANCE_G = 20.0f;
constexpr float POST_TARE_BASELINE_MAX_ABS_G = 50.0f;
constexpr uint32_t DEFAULT_POST_TARE_BASELINE_GRACE_MS = 2000;
constexpr uint32_t MIN_POST_TARE_BASELINE_GRACE_MS = 500;
constexpr uint32_t MAX_POST_TARE_BASELINE_GRACE_MS = 10000;
// Ignore tare noise around 0; a lifted cup drops several grams below this.
constexpr float DEFAULT_CUP_REMOVED_WEIGHT_G = -3.0f;
constexpr float MIN_CUP_REMOVED_WEIGHT_G = -50.0f;
constexpr float MAX_CUP_REMOVED_WEIGHT_G = -0.1f;
constexpr float MAX_PARSED_WEIGHT_G = 10000.0f;
constexpr uint8_t DIRECT_STOP_CONFIRMATION_SAMPLES = 2;
constexpr uint8_t WEIGHT_RECOVERY_CONFIRMATION_SAMPLES = 3;
constexpr uint32_t DIRECT_STOP_CONFIRMATION_WINDOW_MS = 1000;
// An in-shot dip shallower than half the known cup mass (a tilt) must persist.
constexpr uint8_t CUP_PARTIAL_REMOVAL_SAMPLES = 3;
constexpr uint32_t CUP_PARTIAL_REMOVAL_MS = 400;
constexpr float FIRST_DROP_THRESHOLD_G = 0.3f;
// Noise-adaptive floor: the effective first-drop threshold is
// max(FIRST_DROP_THRESHOLD_G, k·σ̂) once the noise estimator warms up.
constexpr float FIRST_DROP_NOISE_EWMA_BETA = 0.1f;
// Deviations fed to σ̂ are capped at the fixed floor: k·σ̂ then can never
// exceed 3×0.3 g, so even a noisy onset (vibration resetting the rise-freeze)
// still crosses the threshold instead of being chased away forever.
constexpr float FIRST_DROP_NOISE_MAX_DEVIATION_G = 0.3f;
constexpr uint8_t FIRST_DROP_NOISE_WARMUP_SAMPLES = 8;
// A strictly rising run this long is signal (onset or creep), not noise:
// the noise estimator must not chase it, or the k·σ̂ floor would run away
// from any gentle linear onset and the first drop could never fire.
constexpr uint8_t FIRST_DROP_NOISE_RISE_FREEZE = 3;
constexpr float FIRST_DROP_SIGMA_K = 3.0f;
// Mean rate required across a confirmation streak; thermal creep (~0.03 g/s)
// stays below it while gentle 0.1 g/s onsets pass.
constexpr float FIRST_DROP_MIN_RATE_G_S = 0.08f;
constexpr float FIRST_DROP_FINGER_STEP_G = 2.0f;
constexpr float FIRST_DROP_BASELINE_SETTLE_G = 0.5f;
// Rewrite tare zero only for noise this close to 0; 0.25 g of coffee is not zero.
constexpr float FIRST_DROP_BASELINE_LOCK_G = 0.15f;
constexpr uint8_t FIRST_DROP_CONFIRMATION_SAMPLES = 2;
// Matches DEFAULT_MINIMUM_CUP_WEIGHT_G. Callers pass runtime min-cup.
constexpr float FIRST_DROP_DEFAULT_CUP_MIN_G = 10.0f;
constexpr float MAX_RECOVERY_WEIGHT_DROP_G = 2.0f;

// Post-shot sample is still the brew if it did not collapse vs last accepted
// (cup off / tare reads ~0). Drip may add weight or drop a little from settle.
inline bool plausibleSettledBrewWeight(float candidateG, float lastKnownG,
                                       bool lastKnownValid) {
  if (!std::isfinite(candidateG)) {
    return false;
  }
  if (!lastKnownValid || !std::isfinite(lastKnownG)) {
    return true;
  }
  return candidateG >= lastKnownG - MAX_RECOVERY_WEIGHT_DROP_G;
}
// The touch reference lags the anchor so a contact cannot drag it; flow is
// the secant of the accepted second ending at the reference.
constexpr float ACCIDENTAL_TOUCH_REFERENCE_LAG_S = 0.5f;
constexpr float ACCIDENTAL_TOUCH_FLOW_WINDOW_S = 1.0f;
constexpr float ACCIDENTAL_TOUCH_MIN_FLOW_SPAN_S = 0.2f;
constexpr float ACCIDENTAL_TOUCH_RESIDUAL_G = 1.5f;
constexpr float ACCIDENTAL_TOUCH_HOLD_RESIDUAL_G = 1.0f;
constexpr uint32_t ACCIDENTAL_TOUCH_MAX_HOLD_MS = 2500;
constexpr float ACCIDENTAL_TOUCH_SUSTAINED_SPAN_G = 1.0f;
// A flat plateau this far from the trend is a resting load, never a spurt.
constexpr float ACCIDENTAL_TOUCH_SUSTAINED_MAX_DELTA_G = 5.0f;
constexpr uint8_t ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES = 10;

// Sliding window for brew-by-weight prediction. Keep this tiny and in
// internal RAM — do not allocate on heap or PSRAM on the weight path.
constexpr size_t MAX_SHOT_DATAPOINTS = 32;

// Post-stop drip tail: the settled final weight is the median of the last
// POST_STOP_TAIL_SAMPLE_COUNT plausible samples once they span at least
// POST_STOP_TAIL_MIN_COVERAGE_MS; sparser tails fall back to the running max.
// Dense streams are decimated to one sample per spacing window so the ring
// actually reaches the required coverage (~10 Hz scales would otherwise span
// only 0.4 s and the median could never engage).
constexpr size_t POST_STOP_TAIL_SAMPLE_COUNT = 5;
constexpr uint32_t POST_STOP_TAIL_MIN_COVERAGE_MS = 1000;
constexpr uint32_t POST_STOP_TAIL_SAMPLE_SPACING_MS =
    POST_STOP_TAIL_MIN_COVERAGE_MS / (POST_STOP_TAIL_SAMPLE_COUNT - 1U);

enum class CupPresenceState : uint8_t {
  ABSENT = 0,
  PRESENT = 1
};

inline const char *cupPresenceStateName(CupPresenceState state) {
  switch (state) {
    case CupPresenceState::ABSENT: return "ABSENT";
    case CupPresenceState::PRESENT: return "PRESENT";
  }
  return "ABSENT";
}

enum class CupPresenceEvent : uint8_t {
  NONE = 0,
  PLACED = 1,
  REMOVED = 2
};

enum class WeightStreamState : uint8_t {
  NO_SAMPLE,
  FRESH,
  STALE,
  ANOMALOUS,
  OVERLOAD
};

// Item tared on top of a cup ("Tare items added to a cup"), as shown on Home.
enum class AccessoryState : uint8_t { OFF, UNKNOWN, ABSENT, NOT_TRACKED, SETTLING, PRESENT };

inline const char *accessoryStateName(AccessoryState state) {
  switch (state) {
    case AccessoryState::OFF: return "off";
    case AccessoryState::UNKNOWN: return "unknown";
    case AccessoryState::ABSENT: return "absent";
    case AccessoryState::NOT_TRACKED: return "not_tracked";
    case AccessoryState::SETTLING: return "settling";
    case AccessoryState::PRESENT: return "present";
  }
  return "unknown";
}

struct CupTareDiagnostics {
  float weightG = 0.0f;
  bool weightValid = false;
  AccessoryState accessoryState = AccessoryState::UNKNOWN;
  float accessoryG = 0.0f;
  uint8_t cupState = 0;
  bool holdTransitions = false;
  bool inNegativeHole = false;
  bool tared = false;
  bool referenceKnown = true;
  bool emptyReferenceBlocked = false;
  bool absentObserved = false;
  uint8_t removedConfirmations = 0;
  uint8_t stabilitySamples = 0;
  uint8_t phase = 0;
  uint8_t eligibilityReason = 0;
  uint8_t lastReason = 0;
  float holeWeightG = 0.0f;
  float candidateWeightG = 0.0f;
  float occupiedReferenceG = 0.0f;
  float requestMinimumG = 0.0f;
  float requestMaximumG = 0.0f;
  float lastWeightG = 0.0f;
  float anchorDeltaG = NAN;  // Idle reading minus the empty reference while it waits.
  uint32_t anchorWaitMs = 0;  // Age of that stable rejected plateau; 0 when none.
  uint32_t placementId = 0;
  uint32_t requestPlacementId = 0;
  uint32_t requestId = 0;
  uint32_t connectionGeneration = 0;
  uint32_t configRevision = 0;
  uint32_t requestedAtMs = 0;
  uint32_t startedAtMs = 0;
  uint32_t writtenAtMs = 0;
  uint32_t effectAtMs = 0;
  uint32_t captureBoundary = 0;
  uint32_t lastSampleAtMs = 0;
  uint32_t lastPacketSequence = 0;
  uint32_t lastCaptureSequence = 0;
  uint32_t lastTerminalRequestId = 0;
  uint32_t lastTerminalAtMs = 0;
  uint32_t rejectedSamples = 0;
  uint32_t droppedSamples = 0;
};

enum class WeightControlState : uint8_t {
  INACTIVE,
  VALIDATING,
  ACTIVE,
  SUSPENDED,
  FAULT_STOPPED
};

enum class AccidentalTouchPhase : uint8_t {
  STARTUP = 0,
  TREND = 1
};

inline const char *accidentalTouchPhaseName(AccidentalTouchPhase phase) {
  switch (phase) {
    case AccidentalTouchPhase::STARTUP: return "STARTUP";
    case AccidentalTouchPhase::TREND: return "TREND";
  }
  return "STARTUP";
}

enum class AccidentalTouchClass : uint8_t {
  OK = 0,
  TOUCH = 1,
  SUSTAINED = 2
};

inline const char *accidentalTouchClassName(AccidentalTouchClass classified) {
  switch (classified) {
    case AccidentalTouchClass::OK: return "OK";
    case AccidentalTouchClass::TOUCH: return "TOUCH";
    case AccidentalTouchClass::SUSTAINED: return "SUSTAINED";
  }
  return "OK";
}

enum class FirstFlowPhase : uint8_t {
  SEEKING = 0,
  TOUCH = 1
};

enum class FirstFlowClass : uint8_t {
  NONE = 0,
  CANDIDATE = 1,
  TOUCH = 2,
  FIRE = 3
};

struct FirstFlowState {
  FirstFlowPhase phase = FirstFlowPhase::SEEKING;
  uint8_t confirmations = 0;
  uint8_t residualConfirmations = 0;
  bool hasLastSample = false;
  float lastWeightG = 0.0f;
  uint32_t lastAtMs = 0;
  uint32_t lastPacketSequence = 0;
  float peakG = 0.0f;
  float postJumpG = 0.0f;
  uint32_t jumpAtMs = 0;
  uint32_t candidateMs = 0;
  float candidateWeightG = 0.0f;
  // Noise estimator: warmup mean of squared deviations initializes σ̂², then
  // an EWMA (β) tracks it. Frozen mid-streak and through monotone rises —
  // a rising run is signal (onset or creep), never noise evidence.
  float noiseVarHatG = 0.0f;
  float noiseWarmSumG = 0.0f;
  uint8_t noiseSamples = 0;
  uint8_t riseRun = 0;
  uint32_t lastFaAtMs = 0;
};
static_assert(sizeof(FirstFlowState) <= 56,
              "FirstFlowState sits in per-cycle BSS; keep it tiny");

struct FirstFlowObservation {
  uint32_t atMs = 0;
  float weightG = 0.0f;
};

inline void resetFirstFlowState(FirstFlowState &state) {
  state = FirstFlowState{};
}

inline bool firstFlowPacketsConsecutive(const FirstFlowState &state,
                                        uint32_t receivedAtMs,
                                        uint32_t packetSequence) {
  return state.hasLastSample &&
         packetSequence == state.lastPacketSequence + 1U &&
         static_cast<int32_t>(receivedAtMs - state.lastAtMs) >= 0 &&
         static_cast<uint32_t>(receivedAtMs - state.lastAtMs) <=
             DIRECT_STOP_CONFIRMATION_WINDOW_MS;
}

inline void noteFirstFlowSample(FirstFlowState &state, float weight,
                                uint32_t receivedAtMs, uint32_t packetSequence) {
  state.hasLastSample = true;
  state.lastWeightG = weight;
  state.lastAtMs = receivedAtMs;
  state.lastPacketSequence = packetSequence;
}

inline FirstFlowClass enterFirstFlowTouch(FirstFlowState &state, float weight,
                                          uint32_t receivedAtMs) {
  state.phase = FirstFlowPhase::TOUCH;
  state.postJumpG = weight;
  state.peakG = weight;
  state.jumpAtMs = receivedAtMs;
  state.confirmations = 0;
  state.residualConfirmations = 0;
  state.candidateMs = receivedAtMs;
  state.candidateWeightG = weight;
  return FirstFlowClass::TOUCH;
}

inline float firstFlowCupMinG(float cupMinG) {
  return (std::isfinite(cupMinG) && cupMinG > FIRST_DROP_THRESHOLD_G)
             ? cupMinG
             : FIRST_DROP_DEFAULT_CUP_MIN_G;
}

inline bool firstFlowIsCupMass(float delta, float cupMinG) {
  return delta >= firstFlowCupMinG(cupMinG);
}

inline bool firstFlowIsCoffeeLeftover(float delta, float cupMinG,
                                      float thresholdG = FIRST_DROP_THRESHOLD_G) {
  return delta >= thresholdG &&
         delta < FIRST_DROP_FINGER_STEP_G && !firstFlowIsCupMass(delta, cupMinG);
}

inline float firstFlowEffectiveThresholdG(const FirstFlowState &state) {
  if (state.noiseSamples < FIRST_DROP_NOISE_WARMUP_SAMPLES) {
    return FIRST_DROP_THRESHOLD_G;
  }
  return fmaxf(FIRST_DROP_THRESHOLD_G,
               FIRST_DROP_SIGMA_K * sqrtf(state.noiseVarHatG));
}

inline void noteFirstFlowNoiseSample(FirstFlowState &state, float deviationG) {
  if (state.noiseSamples < FIRST_DROP_NOISE_WARMUP_SAMPLES) {
    state.noiseWarmSumG += deviationG * deviationG;
    if (++state.noiseSamples == FIRST_DROP_NOISE_WARMUP_SAMPLES) {
      state.noiseVarHatG =
          state.noiseWarmSumG / FIRST_DROP_NOISE_WARMUP_SAMPLES;
    }
    return;
  }
  state.noiseVarHatG =
      (1.0f - FIRST_DROP_NOISE_EWMA_BETA) * state.noiseVarHatG +
      FIRST_DROP_NOISE_EWMA_BETA * deviationG * deviationG;
}

// SEEKING: small coffee steps (< 2 g, below cup min) confirm first flow.
// TOUCH: a ≥2 g jump or mass ≥ cup min is a finger or cup, not first drop.
// Residual FIRE only if leftover is coffee-sized and the peak was below cup min.
// Through FIRE only below cup min, or within 2 g above the rise needed after
// the jump, so a small cup min or a large jump keeps a non-empty window.
inline FirstFlowClass stepFirstFlow(
    FirstFlowState &state, float weight, uint32_t receivedAtMs,
    uint32_t packetSequence, float baselineG,
    float cupMinG = FIRST_DROP_DEFAULT_CUP_MIN_G) {
  if (!std::isfinite(weight) || !std::isfinite(baselineG)) {
    return FirstFlowClass::NONE;
  }

  const bool rising = state.hasLastSample && weight > state.lastWeightG;
  state.riseRun = rising
                      ? (state.riseRun < FIRST_DROP_NOISE_RISE_FREEZE
                             ? static_cast<uint8_t>(state.riseRun + 1U)
                             : FIRST_DROP_NOISE_RISE_FREEZE)
                      : 0;

  const float objectMinG = firstFlowCupMinG(cupMinG);
  const float thresholdG = firstFlowEffectiveThresholdG(state);
  const float delta = weight - baselineG;
  const float step =
      state.hasLastSample ? (weight - state.lastWeightG) : delta;
  const bool consecutive =
      firstFlowPacketsConsecutive(state, receivedAtMs, packetSequence);

  if (state.phase == FirstFlowPhase::SEEKING) {
    // Vibration alternates sign; flow never steps down inside a streak.
    if (delta < thresholdG || (state.hasLastSample && step < 0.0f)) {
      // σ̂ is fed only by idle sub-threshold samples — real packets, never
      // the monotone lock minimum — capped, and frozen during a live streak
      // and through rising runs.
      if (delta < thresholdG && state.confirmations == 0 &&
          state.riseRun < FIRST_DROP_NOISE_RISE_FREEZE) {
        noteFirstFlowNoiseSample(
            state, fminf(fabsf(delta), FIRST_DROP_NOISE_MAX_DEVIATION_G));
      }
      state.confirmations = 0;
      state.residualConfirmations = 0;
      state.candidateMs = 0;
      state.candidateWeightG = 0.0f;
      noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
      return FirstFlowClass::NONE;
    }
    // A finger is a fast step (8 g/s); the same step spread over a packet
    // gap is coffee that accumulated while the stream was silent.
    const uint32_t stepMs = receivedAtMs - state.lastAtMs;
    if ((step >= FIRST_DROP_FINGER_STEP_G &&
         (!state.hasLastSample || step * 125.0f > (stepMs > 100U ? stepMs : 100U))) ||
        delta >= objectMinG) {
      const FirstFlowClass classified =
          enterFirstFlowTouch(state, weight, receivedAtMs);
      noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
      return classified;
    }
    state.confirmations =
        consecutive && state.confirmations > 0
            ? static_cast<uint8_t>(state.confirmations < FIRST_DROP_CONFIRMATION_SAMPLES
                                       ? state.confirmations + 1U
                                       : FIRST_DROP_CONFIRMATION_SAMPLES)
            : 1U;
    if (state.confirmations == 1U) {
      state.candidateMs = receivedAtMs;
      state.candidateWeightG = weight;
    }
    noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
    if (state.confirmations >= FIRST_DROP_CONFIRMATION_SAMPLES) {
      // Mean rate across the streak separates flow from creep; a zero span
      // carries no independent evidence and keeps the streak waiting.
      const uint32_t spanMs = receivedAtMs - state.candidateMs;
      const float meanRateG_S =
          spanMs != 0
              ? (weight - state.candidateWeightG) /
                    (static_cast<float>(spanMs) / 1000.0f)
              : 0.0f;
      if (meanRateG_S >= FIRST_DROP_MIN_RATE_G_S) {
        return FirstFlowClass::FIRE;
      }
      return FirstFlowClass::CANDIDATE;
    }
    return FirstFlowClass::CANDIDATE;
  }

  state.peakG = fmaxf(state.peakG, weight);
  if (step >= FIRST_DROP_FINGER_STEP_G) {
    state.postJumpG = weight;
    state.peakG = fmaxf(state.peakG, weight);
    state.jumpAtMs = receivedAtMs;
    state.confirmations = 0;
    state.residualConfirmations = 0;
    state.candidateMs = receivedAtMs;
    state.candidateWeightG = weight;
    noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
    return FirstFlowClass::TOUCH;
  }

  const bool released = weight <= state.peakG - FIRST_DROP_FINGER_STEP_G;
  if (released) {
    if (firstFlowIsCoffeeLeftover(delta, cupMinG, thresholdG) &&
        !firstFlowIsCupMass(state.peakG - baselineG, cupMinG)) {
      state.confirmations = 0;
      state.residualConfirmations =
          consecutive && state.residualConfirmations > 0
              ? static_cast<uint8_t>(state.residualConfirmations + 1U)
              : 1U;
      if (state.residualConfirmations == 1U) {
        state.candidateMs = receivedAtMs;
        state.candidateWeightG = weight;
      }
      noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
      if (state.residualConfirmations >= FIRST_DROP_CONFIRMATION_SAMPLES) {
        return FirstFlowClass::FIRE;
      }
      return FirstFlowClass::TOUCH;
    }
    if (delta >= thresholdG) {
      state.confirmations = 0;
      state.residualConfirmations = 0;
      noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
      return FirstFlowClass::TOUCH;
    }
    state.phase = FirstFlowPhase::SEEKING;
    state.confirmations = 0;
    state.residualConfirmations = 0;
    state.candidateMs = 0;
    state.candidateWeightG = 0.0f;
    noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
    return FirstFlowClass::NONE;
  }

  if (delta < fmaxf(objectMinG, state.postJumpG - baselineG +
                                    2.0f * FIRST_DROP_FINGER_STEP_G) &&
      weight >= state.postJumpG + FIRST_DROP_FINGER_STEP_G &&
      step < FIRST_DROP_FINGER_STEP_G) {
    state.residualConfirmations = 0;
    state.confirmations =
        consecutive && state.confirmations > 0
            ? static_cast<uint8_t>(state.confirmations + 1U)
            : 1U;
    noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
    if (state.confirmations >= FIRST_DROP_CONFIRMATION_SAMPLES) {
      // Same mean-rate law as SEEKING, measured from the touch jump, so a
      // slowly creeping extra load cannot latch a bogus first drop. The
      // residual (finger-release) path stays ungated: leftover coffee
      // settles at a near-zero rate that a gate would false-reject.
      const uint32_t sinceJumpMs = receivedAtMs - state.jumpAtMs;
      const float meanRateG_S =
          sinceJumpMs != 0
              ? (weight - state.postJumpG) /
                    (static_cast<float>(sinceJumpMs) / 1000.0f)
              : 0.0f;
      if (meanRateG_S >= FIRST_DROP_MIN_RATE_G_S) {
        state.candidateMs = state.jumpAtMs;
        state.candidateWeightG = state.postJumpG;
        return FirstFlowClass::FIRE;
      }
    }
    return FirstFlowClass::TOUCH;
  }

  state.confirmations = 0;
  state.residualConfirmations = 0;
  noteFirstFlowSample(state, weight, receivedAtMs, packetSequence);
  return FirstFlowClass::TOUCH;
}

inline const char *weightStreamStateName(WeightStreamState state) {
  switch (state) {
    case WeightStreamState::NO_SAMPLE: return "NO_SAMPLE";
    case WeightStreamState::FRESH: return "FRESH";
    case WeightStreamState::STALE: return "STALE";
    case WeightStreamState::ANOMALOUS: return "ANOMALOUS";
    case WeightStreamState::OVERLOAD: return "OVERLOAD";
  }
  return "UNKNOWN";
}

inline const char *weightControlStateName(WeightControlState state) {
  switch (state) {
    case WeightControlState::INACTIVE: return "INACTIVE";
    case WeightControlState::VALIDATING: return "VALIDATING";
    case WeightControlState::ACTIVE: return "ACTIVE";
    case WeightControlState::SUSPENDED: return "SUSPENDED";
    case WeightControlState::FAULT_STOPPED: return "FAULT_STOPPED";
  }
  return "UNKNOWN";
}

struct AccidentalTouchResidual {
  bool frozen = false;  // A lagged reference exists (TREND phase).
  float residualG = 0.0f;
};

// Residual of a candidate against the accepted trend since the last level
// shift (epochS). The reference is the newest accepted point at least
// REFERENCE_LAG_S older than the anchor, so it moves only with the anchor and
// held samples never shift it; until one exists the anchor is the reference.
// An empty trajectory (pre-cycle anchor) carries no trend: residual 0.
inline AccidentalTouchResidual accidentalTouchResidual(
    const float *timeS, const float *weightG, size_t datapoints, float epochS,
    float anchorG, float anchorS, float weight, float nowS) {
  AccidentalTouchResidual out;
  if (timeS == nullptr || weightG == nullptr || datapoints == 0) return out;
  const size_t newest = datapoints - 1U;
  size_t first = newest;
  while (first > 0 && timeS[first - 1U] >= epochS) --first;
  size_t end = newest;
  float refG = anchorG;
  float refS = anchorS;
  for (size_t i = datapoints; i-- > first;) {
    if (timeS[i] <= anchorS - ACCIDENTAL_TOUCH_REFERENCE_LAG_S) {
      out.frozen = true;
      end = i;
      refG = weightG[i];
      refS = timeS[i];
      break;
    }
  }
  size_t lo = end;
  while (lo > first &&
         timeS[lo - 1U] >= timeS[end] - ACCIDENTAL_TOUCH_FLOW_WINDOW_S) {
    --lo;
  }
  // Young or sparse history: the only flow evidence reaches the newest point.
  if (timeS[end] - timeS[lo] < ACCIDENTAL_TOUCH_MIN_FLOW_SPAN_S) end = newest;
  const float span = timeS[end] - timeS[lo];
  const float flow = span >= ACCIDENTAL_TOUCH_MIN_FLOW_SPAN_S
                         ? fmaxf((weightG[end] - weightG[lo]) / span, 0.0f)
                         : 0.0f;
  out.residualG = weight - (refG + flow * (nowS - refS));
  return out;
}

// TOUCH holds the direct weight-stop law (automaticScaleStopDue); only the
// touch fallback, calibration-ineligible, may end the shot from a hold. A
// hold releases on returning to the trend (hysteresis), as a SUSTAINED level
// shift once its residual stays flat for SUSTAINED_SAMPLES, or as SUSTAINED
// after MAX_HOLD_MS so held mass below the cut line still ends the shot. A
// SUSTAINED shift is accepted as weight but never trains learning, and the
// stopper treats one above the cut line as touch evidence: a finger resting
// on the platform must never stop the machine through its own mass. Host
// tests pin this.
inline AccidentalTouchClass classifyAccidentalTouch(
    float residualG, bool holding, uint32_t heldMs,
    const float *pendingResidualG, uint8_t pendingCount) {
  if (!std::isfinite(residualG) ||
      fabsf(residualG) < (holding ? ACCIDENTAL_TOUCH_HOLD_RESIDUAL_G
                                  : ACCIDENTAL_TOUCH_RESIDUAL_G)) {
    return AccidentalTouchClass::OK;
  }
  if (holding && heldMs >= ACCIDENTAL_TOUCH_MAX_HOLD_MS) {
    return AccidentalTouchClass::SUSTAINED;
  }
  float lo = residualG;
  float hi = residualG;
  for (uint8_t i = 0; pendingResidualG != nullptr && i < pendingCount; ++i) {
    lo = fminf(lo, pendingResidualG[i]);
    hi = fmaxf(hi, pendingResidualG[i]);
  }
  if (static_cast<uint8_t>(pendingCount + 1U) >=
          ACCIDENTAL_TOUCH_SUSTAINED_SAMPLES &&
      (hi - lo) <= ACCIDENTAL_TOUCH_SUSTAINED_SPAN_G &&
      fabsf(residualG) < ACCIDENTAL_TOUCH_SUSTAINED_MAX_DELTA_G) {
    return AccidentalTouchClass::SUSTAINED;
  }
  return AccidentalTouchClass::TOUCH;
}

inline bool validBleMacNibble(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
         (c >= 'a' && c <= 'f');
}

// Empty = no preferred scale. Otherwise canonical "AA:BB:CC:DD:EE:FF".
inline bool validPreferredScaleMac(const char *mac) {
  if (mac == nullptr) {
    return false;
  }
  const size_t length = strnlen(mac, PREFERRED_SCALE_MAC_CAPACITY);
  if (length == 0) {
    return mac[0] == '\0';
  }
  if (length != 17 || mac[17] != '\0') {
    return false;
  }
  for (size_t index = 0; index < 17; ++index) {
    if ((index % 3) == 2) {
      if (mac[index] != ':') {
        return false;
      }
    } else if (!validBleMacNibble(mac[index])) {
      return false;
    }
  }
  return true;
}

// Empty name is allowed. Otherwise printable ASCII without quotes/controls.
inline bool validPreferredScaleName(const char *name) {
  if (name == nullptr) {
    return false;
  }
  const size_t length = strnlen(name, PREFERRED_SCALE_NAME_CAPACITY);
  if (length >= PREFERRED_SCALE_NAME_CAPACITY) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const unsigned char c = static_cast<unsigned char>(name[index]);
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') {
      return false;
    }
  }
  return true;
}

// Empty = no friendly override. Otherwise up to SCALE_FRIENDLY_NAME_MAX_LEN
// letters, digits, spaces, or hyphens; no leading/trailing space or hyphen.
inline bool validScaleFriendlyName(const char *name) {
  if (name == nullptr) {
    return false;
  }
  const size_t length = strnlen(name, PREFERRED_SCALE_NAME_CAPACITY);
  if (length == 0) {
    return true;
  }
  if (length > SCALE_FRIENDLY_NAME_MAX_LEN) {
    return false;
  }
  for (size_t index = 0; index < length; ++index) {
    const char c = name[index];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == ' ' || c == '-')) {
      return false;
    }
  }
  return name[0] != ' ' && name[0] != '-' &&
         name[length - 1] != ' ' && name[length - 1] != '-';
}

inline bool validScaleHistoryEntries(const ScaleHistoryEntry *entries) {
  if (entries == nullptr) {
    return false;
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (!validPreferredScaleMac(entries[i].mac) ||
        !validPreferredScaleName(entries[i].name) ||
        !validScaleFriendlyName(entries[i].friendlyName)) {
      return false;
    }
  }
  return true;
}

inline char preferredScaleMacUpper(char c) {
  if (c >= 'a' && c <= 'z') {
    return static_cast<char>(c - ('a' - 'A'));
  }
  return c;
}

// Case-insensitive BLE MAC compare (AA:BB:… vs aa:bb:…).
inline bool preferredScaleMacEqual(const char *left, const char *right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  for (size_t i = 0; i < PREFERRED_SCALE_MAC_CAPACITY; ++i) {
    const char a = preferredScaleMacUpper(left[i]);
    const char b = preferredScaleMacUpper(right[i]);
    if (a != b) {
      return false;
    }
    if (a == '\0') {
      return true;
    }
  }
  return true;
}

inline void canonicalizePreferredScaleMac(char *mac, size_t capacity) {
  if (mac == nullptr || capacity == 0) {
    return;
  }
  for (size_t i = 0; i + 1 < capacity && mac[i] != '\0'; ++i) {
    mac[i] = preferredScaleMacUpper(mac[i]);
  }
}

inline size_t scaleHistoryOccupiedCount(const ScaleHistoryEntry *entries) {
  size_t count = 0;
  if (entries == nullptr) {
    return 0;
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (entries[i].mac[0] != '\0') {
      ++count;
    }
  }
  return count;
}

// MAC+name identity for NVS. lastSeenSeq is RAM LRU only and must not
// trigger a flash write on every connect.
inline bool scaleHistoryIdentityEqual(const ScaleHistoryEntry *left,
                                      const ScaleHistoryEntry *right) {
  if (left == nullptr || right == nullptr) {
    return left == right;
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (!preferredScaleMacEqual(left[i].mac, right[i].mac)) {
      return false;
    }
    if (strncmp(left[i].name, right[i].name, PREFERRED_SCALE_NAME_CAPACITY) !=
        0) {
      return false;
    }
    if (strncmp(left[i].friendlyName, right[i].friendlyName,
                PREFERRED_SCALE_NAME_CAPACITY) != 0) {
      return false;
    }
  }
  return true;
}

inline bool scaleMacNvsWriteAllowed(bool connecting, bool connected) {
  return !connecting && !connected;
}

// NVS/flash cache-off during a live GATT session drops Bookoo/Themis links.
enum class ScaleMacNvsAction : uint8_t { CLEAR_DIRTY = 0, DEFER = 1, QUEUE = 2 };

inline ScaleMacNvsAction decideScaleMacNvsAction(bool identityUnchanged,
                                                 bool connecting,
                                                 bool connected) {
  if (identityUnchanged) {
    return ScaleMacNvsAction::CLEAR_DIRTY;
  }
  if (!scaleMacNvsWriteAllowed(connecting, connected)) {
    return ScaleMacNvsAction::DEFER;
  }
  return ScaleMacNvsAction::QUEUE;
}

// Upsert by MAC (case-insensitive). Stores canonical uppercase MAC.
// Returns true if MAC/name membership changed. lastSeenSeq always advances
// for in-session LRU and is not by itself a persist reason.
// The persisted friendlyName is an override; the library supplies defaults.
inline bool upsertScaleHistory(ScaleHistoryEntry *entries, uint32_t &seqCounter,
                               const char *mac, const char *name) {
  if (entries == nullptr || mac == nullptr || !validPreferredScaleMac(mac) ||
      mac[0] == '\0') {
    return false;
  }
  char canonicalMac[PREFERRED_SCALE_MAC_CAPACITY] = {};
  copyCString(canonicalMac, sizeof(canonicalMac), mac);
  canonicalizePreferredScaleMac(canonicalMac, sizeof(canonicalMac));
  char safeName[PREFERRED_SCALE_NAME_CAPACITY] = {};
  if (name != nullptr && validPreferredScaleName(name)) {
    copyCString(safeName, sizeof(safeName), name);
  }
  seqCounter = (seqCounter + 1U) & SCALE_HISTORY_SEQUENCE_MASK;
  if (seqCounter == 0) {
    seqCounter = 1;
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (preferredScaleMacEqual(entries[i].mac, canonicalMac)) {
      bool changed = false;
      if (strncmp(entries[i].mac, canonicalMac, PREFERRED_SCALE_MAC_CAPACITY) !=
          0) {
        memcpy(entries[i].mac, canonicalMac, sizeof(entries[i].mac));
        changed = true;
      }
      if (strncmp(entries[i].name, safeName, PREFERRED_SCALE_NAME_CAPACITY) !=
          0) {
        memcpy(entries[i].name, safeName, sizeof(entries[i].name));
        changed = true;
      }
      entries[i].lastSeenSeq =
          (entries[i].lastSeenSeq & SCALE_HISTORY_SESSION_CONSUMED) |
          seqCounter;
      return changed;
    }
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (entries[i].mac[0] == '\0') {
      entries[i].friendlyName[0] = '\0';
      memcpy(entries[i].mac, canonicalMac, sizeof(entries[i].mac));
      memcpy(entries[i].name, safeName, sizeof(entries[i].name));
      entries[i].lastSeenSeq = seqCounter;
      return true;
    }
  }
  size_t victim = SCALE_HISTORY_CAPACITY;
  uint32_t oldest = 0;
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (scaleHistorySessionConsumed(entries[i])) {
      continue;
    }
    const uint32_t sequence =
        entries[i].lastSeenSeq & SCALE_HISTORY_SEQUENCE_MASK;
    if (victim == SCALE_HISTORY_CAPACITY || sequence < oldest) {
      oldest = sequence;
      victim = i;
    }
  }
  if (victim == SCALE_HISTORY_CAPACITY) {
    return false;
  }
  // A replaced slot belongs to a different scale: its friendly override must
  // not leak onto the new occupant.
  entries[victim].friendlyName[0] = '\0';
  memcpy(entries[victim].mac, canonicalMac, sizeof(entries[victim].mac));
  memcpy(entries[victim].name, safeName, sizeof(entries[victim].name));
  entries[victim].lastSeenSeq = seqCounter;
  return true;
}

inline bool consumeScaleHistorySessionConnection(
    ScaleHistoryEntry *entries, uint32_t &seqCounter, const char *mac,
    const char *name) {
  if (entries == nullptr || mac == nullptr || !validPreferredScaleMac(mac) ||
      mac[0] == '\0') {
    return false;
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (preferredScaleMacEqual(entries[i].mac, mac)) {
      if (scaleHistorySessionConsumed(entries[i])) {
        return false;
      }
      entries[i].lastSeenSeq |= SCALE_HISTORY_SESSION_CONSUMED;
      return true;
    }
  }
  (void)upsertScaleHistory(entries, seqCounter, mac, name);
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (preferredScaleMacEqual(entries[i].mac, mac)) {
      entries[i].lastSeenSeq &= SCALE_HISTORY_SEQUENCE_MASK;
      entries[i].lastSeenSeq |= SCALE_HISTORY_SESSION_CONSUMED;
      return true;
    }
  }
  return false;
}

inline void reopenScaleHistorySessionConnection(ScaleHistoryEntry *entries,
                                                const char *mac) {
  if (entries == nullptr || mac == nullptr || mac[0] == '\0') {
    return;
  }
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (preferredScaleMacEqual(entries[i].mac, mac)) {
      clearScaleHistorySessionMarker(entries[i]);
      return;
    }
  }
}

inline void seedScaleHistoryFromPreferred(ScaleHistoryEntry *entries,
                                          uint32_t &seqCounter,
                                          const char *mac, const char *name) {
  if (entries == nullptr || scaleHistoryOccupiedCount(entries) > 0) {
    return;
  }
  if (mac == nullptr || mac[0] == '\0' || !validPreferredScaleMac(mac)) {
    return;
  }
  upsertScaleHistory(entries, seqCounter, mac, name);
}

inline bool findScaleHistoryName(const ScaleHistoryEntry *entries,
                                 const char *mac, char *nameOut,
                                 size_t nameOutCapacity) {
  if (entries == nullptr || mac == nullptr || nameOut == nullptr ||
      nameOutCapacity == 0) {
    return false;
  }
  nameOut[0] = '\0';
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (preferredScaleMacEqual(entries[i].mac, mac)) {
      copyCString(nameOut, nameOutCapacity, entries[i].name);
      return true;
    }
  }
  return false;
}

inline bool findScaleHistoryFriendlyName(const ScaleHistoryEntry *entries,
                                         const char *mac, char *nameOut,
                                         size_t nameOutCapacity) {
  if (entries == nullptr || mac == nullptr || nameOut == nullptr ||
      nameOutCapacity == 0) {
    return false;
  }
  nameOut[0] = '\0';
  for (size_t i = 0; i < SCALE_HISTORY_CAPACITY; ++i) {
    if (preferredScaleMacEqual(entries[i].mac, mac)) {
      copyCString(nameOut, nameOutCapacity,
                  entries[i].friendlyName[0] != '\0'
                      ? entries[i].friendlyName
                      : scaleDefaultFriendlyName(entries[i].name));
      return true;
    }
  }
  return false;
}

}  // namespace openbrewbyweight

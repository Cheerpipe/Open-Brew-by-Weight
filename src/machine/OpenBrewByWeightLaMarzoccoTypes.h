#pragma once

#include "OpenBrewByWeightLaMarzoccoSettings.h"

#include <cstdint>
#include <type_traits>

namespace openbrewbyweight {

constexpr size_t LA_MARZOCCO_MAX_ACCOUNT_MACHINES = 8;

enum class LaMarzoccoRequestType : uint8_t {
  CONNECT,
  OBSERVE_STATE,
  APPLY_TEMPERATURE,
  SET_STANDBY,
  SET_POWER_ON,
  LIST_MACHINES,
  ACCEPT_POLICY
};
enum class LaMarzoccoPowerState : uint8_t { UNKNOWN, ON, OFF };
enum class LaMarzoccoObservationSource : uint8_t { NONE, HTTP, HTTP_INITIAL, WEBSOCKET };

inline const char *laMarzoccoPowerSourceName(LaMarzoccoObservationSource source) {
  switch (source) {
    case LaMarzoccoObservationSource::HTTP: return "api";
    case LaMarzoccoObservationSource::HTTP_INITIAL: return "api_initial";
    case LaMarzoccoObservationSource::WEBSOCKET: return "websocket";
    case LaMarzoccoObservationSource::NONE: return "none";
  }
  return "none";
}
enum class LaMarzoccoObservedMode : uint8_t {
  NONE,
  STANDBY,
  BREWING,
  ECO,
  UNSUPPORTED
};
enum class LaMarzoccoBoilerState : uint8_t {
  UNKNOWN,
  STANDBY,
  OFF,
  HEATING_UP,
  READY,
  NO_WATER,
  ECO,
  UNSUPPORTED
};
enum class LaMarzoccoSteamLevel : uint8_t {
  UNKNOWN,
  LEVEL_1,
  LEVEL_2,
  LEVEL_3,
  UNSUPPORTED
};
enum class LaMarzoccoReadiness : uint8_t {
  UNKNOWN,
  OFF,
  WARMING_UP,
  WAITING_FOR_STEAM,
  READY,
  NEEDS_WATER
};
enum class LaMarzoccoObservationQuality : uint8_t {
  Disabled,
  UNCONFIGURED,
  CURRENT,
  OPTIMISTIC,
  STALE,
  COMMUNICATION_ERROR,
  UNSUPPORTED
};
enum class LaMarzoccoPhase : uint8_t {
  Disabled,
  IDLE,
  QUEUED,
  PAUSED,
  AUTHENTICATING,
  LISTING,
  RUNNING,
  BACKOFF,
  CONFIRMED,
  FAILED
};
enum class LaMarzoccoError : uint8_t {
  NONE,
  NO_STA,
  AP_MODE,
  CLOCK_UNSYNCED,
  INVALID_AUTH,
  NO_MACHINES,
  HTTP_ERROR,
  TRANSPORT,
  CANCELED,
  REJECTED,
  UNCONFIRMED,
  POLICY_REQUIRED
};
enum class LaMarzoccoTemperatureState : uint8_t {
  Disabled,
  IDLE,
  PENDING,
  RUNNING,
  CONFIRMED,
  FAILED,
  CANCELED
};

inline LaMarzoccoPowerState laMarzoccoPowerStateForMode(
    LaMarzoccoObservedMode mode) {
  if (mode == LaMarzoccoObservedMode::STANDBY)
    return LaMarzoccoPowerState::OFF;
  if (mode == LaMarzoccoObservedMode::BREWING)
    return LaMarzoccoPowerState::ON;
  return LaMarzoccoPowerState::UNKNOWN;
}

inline const char *laMarzoccoPowerStateName(LaMarzoccoPowerState state) {
  switch (state) {
    case LaMarzoccoPowerState::ON: return "ON";
    case LaMarzoccoPowerState::OFF: return "OFF";
    case LaMarzoccoPowerState::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

inline const char *laMarzoccoObservedModeName(LaMarzoccoObservedMode mode) {
  switch (mode) {
    case LaMarzoccoObservedMode::STANDBY: return "StandBy";
    case LaMarzoccoObservedMode::BREWING: return "BrewingMode";
    case LaMarzoccoObservedMode::ECO: return "EcoMode";
    case LaMarzoccoObservedMode::UNSUPPORTED: return "unsupported";
    case LaMarzoccoObservedMode::NONE: return "none";
  }
  return "none";
}

inline const char *laMarzoccoBoilerStateName(LaMarzoccoBoilerState state) {
  switch (state) {
    case LaMarzoccoBoilerState::STANDBY: return "standby";
    case LaMarzoccoBoilerState::OFF: return "off";
    case LaMarzoccoBoilerState::HEATING_UP: return "heating_up";
    case LaMarzoccoBoilerState::READY: return "ready";
    case LaMarzoccoBoilerState::NO_WATER: return "no_water";
    case LaMarzoccoBoilerState::ECO: return "eco";
    case LaMarzoccoBoilerState::UNSUPPORTED: return "unsupported";
    case LaMarzoccoBoilerState::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char *laMarzoccoSteamLevelName(LaMarzoccoSteamLevel level) {
  switch (level) {
    case LaMarzoccoSteamLevel::LEVEL_1: return "level_1";
    case LaMarzoccoSteamLevel::LEVEL_2: return "level_2";
    case LaMarzoccoSteamLevel::LEVEL_3: return "level_3";
    case LaMarzoccoSteamLevel::UNSUPPORTED: return "unsupported";
    case LaMarzoccoSteamLevel::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char *laMarzoccoReadinessName(LaMarzoccoReadiness readiness) {
  switch (readiness) {
    case LaMarzoccoReadiness::OFF: return "off";
    case LaMarzoccoReadiness::WARMING_UP: return "warming_up";
    case LaMarzoccoReadiness::WAITING_FOR_STEAM: return "waiting_for_steam";
    case LaMarzoccoReadiness::READY: return "ready";
    case LaMarzoccoReadiness::NEEDS_WATER: return "needs_water";
    case LaMarzoccoReadiness::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char *laMarzoccoObservationQualityName(
    LaMarzoccoObservationQuality quality) {
  switch (quality) {
    case LaMarzoccoObservationQuality::Disabled: return "disabled";
    case LaMarzoccoObservationQuality::UNCONFIGURED: return "unconfigured";
    case LaMarzoccoObservationQuality::CURRENT: return "current";
    case LaMarzoccoObservationQuality::OPTIMISTIC: return "optimistic";
    case LaMarzoccoObservationQuality::STALE: return "stale";
    case LaMarzoccoObservationQuality::COMMUNICATION_ERROR:
      return "communication_error";
    case LaMarzoccoObservationQuality::UNSUPPORTED: return "unsupported";
  }
  return "unavailable";
}

inline const char *laMarzoccoPhaseName(LaMarzoccoPhase phase) {
  switch (phase) {
    case LaMarzoccoPhase::Disabled: return "disabled";
    case LaMarzoccoPhase::IDLE: return "idle";
    case LaMarzoccoPhase::QUEUED: return "queued";
    case LaMarzoccoPhase::PAUSED: return "paused";
    case LaMarzoccoPhase::AUTHENTICATING: return "authenticating";
    case LaMarzoccoPhase::LISTING: return "listing";
    case LaMarzoccoPhase::RUNNING: return "running";
    case LaMarzoccoPhase::BACKOFF: return "backoff";
    case LaMarzoccoPhase::CONFIRMED: return "confirmed";
    case LaMarzoccoPhase::FAILED: return "failed";
  }
  return "disabled";
}

inline const char *laMarzoccoErrorName(LaMarzoccoError error) {
  switch (error) {
    case LaMarzoccoError::NONE: return "none";
    case LaMarzoccoError::NO_STA: return "sta_required";
    case LaMarzoccoError::AP_MODE: return "ap_mode";
    case LaMarzoccoError::CLOCK_UNSYNCED: return "clock_unsynchronized";
    case LaMarzoccoError::INVALID_AUTH: return "invalid_auth";
    case LaMarzoccoError::NO_MACHINES: return "no_machines";
    case LaMarzoccoError::HTTP_ERROR: return "communication_error";
    case LaMarzoccoError::TRANSPORT: return "transport_error";
    case LaMarzoccoError::CANCELED: return "canceled";
    case LaMarzoccoError::REJECTED: return "rejected";
    case LaMarzoccoError::UNCONFIRMED: return "unconfirmed";
    case LaMarzoccoError::POLICY_REQUIRED: return "policy_required";
  }
  return "unknown";
}

inline const char *laMarzoccoTemperatureStateName(
    LaMarzoccoTemperatureState state) {
  switch (state) {
    case LaMarzoccoTemperatureState::Disabled: return "disabled";
    case LaMarzoccoTemperatureState::IDLE: return "idle";
    case LaMarzoccoTemperatureState::PENDING: return "pending";
    case LaMarzoccoTemperatureState::RUNNING: return "running";
    case LaMarzoccoTemperatureState::CONFIRMED: return "confirmed";
    case LaMarzoccoTemperatureState::FAILED: return "failed";
    case LaMarzoccoTemperatureState::CANCELED: return "canceled";
  }
  return "disabled";
}

inline bool laMarzoccoHttpRetryable(uint16_t status) {
  return status < 200 || status == 401 || status == 408 || status == 425 ||
         status == 429 || status >= 500;
}

inline bool laMarzoccoTemperatureCycleRetryable(LaMarzoccoError error) {
  return error == LaMarzoccoError::HTTP_ERROR ||
         error == LaMarzoccoError::TRANSPORT ||
         error == LaMarzoccoError::UNCONFIRMED;
}

struct LaMarzoccoRequest {
  uint32_t requestId = 0;
  uint32_t configGeneration = 0;
  uint16_t targetDeciC = 0;
  uint8_t presetId = 0;
  LaMarzoccoRequestType type = LaMarzoccoRequestType::OBSERVE_STATE;
};

struct LaMarzoccoMachineSummary {
  char serial[LA_MARZOCCO_SERIAL_CAPACITY] = {};
  char name[LA_MARZOCCO_NAME_CAPACITY] = {};
};

struct LaMarzoccoDiscoverySnapshot {
  uint32_t requestId = 0;
  uint32_t generation = 0;
  uint8_t count = 0;
  // Machines on the saved account at its latest listing; 0 until listed.
  uint8_t accountMachines = 0;
  bool listing = false;
  LaMarzoccoError error = LaMarzoccoError::NONE;
  LaMarzoccoMachineSummary machines[LA_MARZOCCO_MAX_ACCOUNT_MACHINES] = {};
};

// Only immutable, firmware-owned labels belong here, never cloud response data.
struct LaMarzoccoCloudCall {
  const char *api = nullptr;
  const char *method = "";
  const char *result = "setup_error";
  uint32_t startedAtMs = 0;
  uint32_t startedAtUtcSec = 0;
  uint32_t durationMs = 0;
  int32_t transportStatus = 0;
  int32_t socketErrno = 0;
  int32_t tlsError = 0;  // Last esp-tls (mbedTLS) code and cert-verify flags.
  int32_t tlsFlags = 0;
  uint32_t idleMs = 0;  // Since the reused client's previous request ended.
  uint16_t httpStatus = 0;
  bool clientReused = false;
};

// The one assumed power state the firmware acts on, resolved by the service
// from the confirmed reading and the optimistic or retained overlays. ON and
// OFF are current confirmations; ON_STALE and OFF_STALE are assumptions
// (quality says which kind: OPTIMISTIC after an accepted command, STALE for a
// retained reading); UNKNOWN means "behave like a machine that does not
// report power".
// Consumers switch on this value and never recombine powerState, quality or
// the overlay flags themselves.
enum class LaMarzoccoAssumedPower : uint8_t { UNKNOWN, ON, OFF, ON_STALE, OFF_STALE };

struct LaMarzoccoStatus {
  uint32_t requestId = 0;
  uint32_t sampleAtMs = 0;
  uint32_t temperatureAtMs = 0;
  uint32_t coffeeReadyAtUtcSec = 0;
  uint32_t steamReadyAtUtcSec = 0;
  uint32_t configGeneration = 0;
  uint32_t identityGeneration = 0;
  uint16_t targetDeciC = 0;
  // The brew range the selected machine reported through the cloud; 0 until
  // it has, and then no target can be set or sent.
  uint16_t targetMinDeciC = 0;
  uint16_t targetMaxDeciC = 0;
  uint16_t requestedTargetDeciC = 0;
  uint16_t appliedTargetDeciC = 0;
  LaMarzoccoAssumedPower assumedPower = LaMarzoccoAssumedPower::UNKNOWN;  // Fills an alignment gap.
  int32_t transportStatus = 0;
  int32_t temperatureTransportStatus = 0;
  uint16_t httpStatus = 0;
  uint16_t temperatureHttpStatus = 0;
  LaMarzoccoPhase phase = LaMarzoccoPhase::Disabled;
  LaMarzoccoError error = LaMarzoccoError::NONE;
  LaMarzoccoPowerState powerState = LaMarzoccoPowerState::UNKNOWN;
  LaMarzoccoObservationSource powerSource = LaMarzoccoObservationSource::NONE;
  LaMarzoccoObservedMode observedMode = LaMarzoccoObservedMode::NONE;
  LaMarzoccoObservationQuality quality = LaMarzoccoObservationQuality::Disabled;
  LaMarzoccoBoilerState coffeeBoiler = LaMarzoccoBoilerState::UNKNOWN;
  LaMarzoccoBoilerState steamBoiler = LaMarzoccoBoilerState::UNKNOWN;
  LaMarzoccoSteamLevel steamLevel = LaMarzoccoSteamLevel::UNKNOWN;
  LaMarzoccoTemperatureState temperatureState =
      LaMarzoccoTemperatureState::Disabled;
  LaMarzoccoError temperatureError = LaMarzoccoError::NONE;
  bool transportFailure = false;
  bool connectionFreshness = false;
  bool effectiveOn = true;
  bool targetValid = false;
  bool accountConfigured = false;
  bool staConnected = false;
  bool apActive = false;
  bool shotPaused = false;
  bool scalePaused = false;
  bool optimisticOn = false;
  bool optimisticOff = false;
  bool temperatureCommandAccepted = false;
  bool temperatureRetryable = false;
};

// Resolution the service applies when it publishes a status: a live overlay
// replaces the confirmed value; a confirmed value is current only with
// CURRENT quality and an assumption otherwise; no confirmed value is UNKNOWN.
inline LaMarzoccoAssumedPower laMarzoccoAssumedPower(const LaMarzoccoStatus &status) {
  if (status.optimisticOn) return LaMarzoccoAssumedPower::ON_STALE;
  if (status.optimisticOff) return LaMarzoccoAssumedPower::OFF_STALE;
  const bool current = status.quality == LaMarzoccoObservationQuality::CURRENT;
  switch (status.powerState) {
    case LaMarzoccoPowerState::ON:
      return current ? LaMarzoccoAssumedPower::ON : LaMarzoccoAssumedPower::ON_STALE;
    case LaMarzoccoPowerState::OFF:
      return current ? LaMarzoccoAssumedPower::OFF : LaMarzoccoAssumedPower::OFF_STALE;
    case LaMarzoccoPowerState::UNKNOWN: break;
  }
  return LaMarzoccoAssumedPower::UNKNOWN;
}

// Informational rollup of the dashboard boiler widgets. Evidence-driven: a
// HeatingUp or EcoMode boiler is below temperature (warming), a Ready boiler
// is at temperature, and an Off steam boiler never blocks readiness because
// it was disabled on purpose. Contradictory or absent evidence (standby,
// off, unknown, or unsupported boilers while the machine reports ON) stays
// UNKNOWN instead of guessing, so the Home lamp can fall back to a neutral
// label. This layer never feeds control: power remains ON/OFF only.
inline LaMarzoccoReadiness laMarzoccoReadiness(const LaMarzoccoStatus &status) {
  switch (status.assumedPower) {
    case LaMarzoccoAssumedPower::OFF:
    case LaMarzoccoAssumedPower::OFF_STALE:
      return LaMarzoccoReadiness::OFF;
    case LaMarzoccoAssumedPower::UNKNOWN:
      return LaMarzoccoReadiness::UNKNOWN;
    case LaMarzoccoAssumedPower::ON_STALE:
      // An optimistic ON means the machine is waking while the confirmed
      // state still reads OFF: any retained boiler evidence predates the
      // standby, so the lamp stays neutral rather than claim a readiness the
      // reheating machine cannot have yet. A retained ON keeps its evidence.
      if (status.quality == LaMarzoccoObservationQuality::OPTIMISTIC)
        return LaMarzoccoReadiness::UNKNOWN;
      break;
    case LaMarzoccoAssumedPower::ON:
      break;
  }
  const LaMarzoccoBoilerState coffee = status.coffeeBoiler;
  const LaMarzoccoBoilerState steam = status.steamBoiler;
  if (coffee == LaMarzoccoBoilerState::NO_WATER ||
      steam == LaMarzoccoBoilerState::NO_WATER)
    return LaMarzoccoReadiness::NEEDS_WATER;
  const auto warming = [](LaMarzoccoBoilerState boiler) {
    return boiler == LaMarzoccoBoilerState::HEATING_UP ||
        boiler == LaMarzoccoBoilerState::ECO;
  };
  if (coffee == LaMarzoccoBoilerState::READY) {
    if (steam == LaMarzoccoBoilerState::READY ||
        steam == LaMarzoccoBoilerState::OFF)
      return LaMarzoccoReadiness::READY;
    if (warming(steam)) return LaMarzoccoReadiness::WAITING_FOR_STEAM;
    return LaMarzoccoReadiness::UNKNOWN;
  }
  if (warming(coffee)) return LaMarzoccoReadiness::WARMING_UP;
  return LaMarzoccoReadiness::UNKNOWN;
}

// A preset's brew target can be set only inside the range the selected
// machine reported. Before a cloud session has read that range there is
// none, and no brew temperature can be set.
inline bool laMarzoccoBrewTargetSettable(const LaMarzoccoStatus &status, long deciC) {
  return status.targetMinDeciC != 0 && deciC >= status.targetMinDeciC &&
         deciC <= status.targetMaxDeciC;
}

// The target to send for a preset: the preset's own, brought inside the range
// the machine reported. 0 when the machine has reported none: a temperature
// the machine has not said it accepts is never sent.
inline uint16_t laMarzoccoSendableBrewTarget(const LaMarzoccoStatus &status,
                                             uint16_t presetDeciC) {
  if (status.targetMinDeciC == 0) return 0;
  return presetDeciC < status.targetMinDeciC ? status.targetMinDeciC
      : presetDeciC > status.targetMaxDeciC ? status.targetMaxDeciC : presetDeciC;
}

static_assert(sizeof(LaMarzoccoRequest) <= 16,
              "La Marzocco request must stay compact");
// Boiler readiness (states, steam level, readyAt timestamps) widened the
// observation payload beyond the historic 64-byte budget; 80 keeps it
// copy-cheap for the per-tick effective-status handoff.
static_assert(sizeof(LaMarzoccoStatus) <= 80,
              "La Marzocco status must stay compact");
static_assert(std::is_trivially_copyable<LaMarzoccoRequest>::value);
static_assert(std::is_trivially_copyable<LaMarzoccoStatus>::value);
static_assert(std::is_trivially_copyable<LaMarzoccoDiscoverySnapshot>::value);

}  // namespace openbrewbyweight

#pragma once

#include "OpenBrewByWeightLineaMicraSettings.h"

#include <cstdint>
#include <type_traits>

namespace openbrewbyweight {

constexpr size_t LINEA_MICRA_MAX_ACCOUNT_MACHINES = 8;

enum class LineaMicraRequestType : uint8_t {
  CONNECT,
  OBSERVE_STATE,
  APPLY_TEMPERATURE,
  SET_STANDBY,
  SET_POWER_ON,
  LIST_MACHINES,
  ACCEPT_POLICY
};
enum class LineaMicraPowerState : uint8_t { UNKNOWN, ON, OFF };
enum class MicraObservationSource : uint8_t { NONE, HTTP, HTTP_INITIAL, WEBSOCKET };

inline const char *micraPowerSourceName(MicraObservationSource source) {
  switch (source) {
    case MicraObservationSource::HTTP: return "api";
    case MicraObservationSource::HTTP_INITIAL: return "api_initial";
    case MicraObservationSource::WEBSOCKET: return "websocket";
    case MicraObservationSource::NONE: return "none";
  }
  return "none";
}
enum class LineaMicraObservedMode : uint8_t {
  NONE,
  STANDBY,
  BREWING,
  ECO,
  UNSUPPORTED
};
enum class LineaMicraBoilerState : uint8_t {
  UNKNOWN,
  STANDBY,
  OFF,
  HEATING_UP,
  READY,
  NO_WATER,
  ECO,
  UNSUPPORTED
};
enum class LineaMicraSteamLevel : uint8_t {
  UNKNOWN,
  LEVEL_1,
  LEVEL_2,
  LEVEL_3,
  UNSUPPORTED
};
enum class LineaMicraReadiness : uint8_t {
  UNKNOWN,
  OFF,
  WARMING_UP,
  WAITING_FOR_STEAM,
  READY,
  NEEDS_WATER
};
enum class LineaMicraObservationQuality : uint8_t {
  Disabled,
  UNCONFIGURED,
  CURRENT,
  OPTIMISTIC,
  STALE,
  COMMUNICATION_ERROR,
  UNSUPPORTED
};
enum class LineaMicraPhase : uint8_t {
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
enum class LineaMicraError : uint8_t {
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
enum class LineaMicraTemperatureState : uint8_t {
  Disabled,
  IDLE,
  PENDING,
  RUNNING,
  CONFIRMED,
  FAILED,
  CANCELED
};

inline LineaMicraPowerState lineaMicraPowerStateForMode(
    LineaMicraObservedMode mode) {
  if (mode == LineaMicraObservedMode::STANDBY)
    return LineaMicraPowerState::OFF;
  if (mode == LineaMicraObservedMode::BREWING)
    return LineaMicraPowerState::ON;
  return LineaMicraPowerState::UNKNOWN;
}

inline const char *lineaMicraPowerStateName(LineaMicraPowerState state) {
  switch (state) {
    case LineaMicraPowerState::ON: return "ON";
    case LineaMicraPowerState::OFF: return "OFF";
    case LineaMicraPowerState::UNKNOWN: return "UNKNOWN";
  }
  return "UNKNOWN";
}

inline const char *lineaMicraObservedModeName(LineaMicraObservedMode mode) {
  switch (mode) {
    case LineaMicraObservedMode::STANDBY: return "StandBy";
    case LineaMicraObservedMode::BREWING: return "BrewingMode";
    case LineaMicraObservedMode::ECO: return "EcoMode";
    case LineaMicraObservedMode::UNSUPPORTED: return "unsupported";
    case LineaMicraObservedMode::NONE: return "none";
  }
  return "none";
}

inline const char *lineaMicraBoilerStateName(LineaMicraBoilerState state) {
  switch (state) {
    case LineaMicraBoilerState::STANDBY: return "standby";
    case LineaMicraBoilerState::OFF: return "off";
    case LineaMicraBoilerState::HEATING_UP: return "heating_up";
    case LineaMicraBoilerState::READY: return "ready";
    case LineaMicraBoilerState::NO_WATER: return "no_water";
    case LineaMicraBoilerState::ECO: return "eco";
    case LineaMicraBoilerState::UNSUPPORTED: return "unsupported";
    case LineaMicraBoilerState::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char *lineaMicraSteamLevelName(LineaMicraSteamLevel level) {
  switch (level) {
    case LineaMicraSteamLevel::LEVEL_1: return "level_1";
    case LineaMicraSteamLevel::LEVEL_2: return "level_2";
    case LineaMicraSteamLevel::LEVEL_3: return "level_3";
    case LineaMicraSteamLevel::UNSUPPORTED: return "unsupported";
    case LineaMicraSteamLevel::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char *lineaMicraReadinessName(LineaMicraReadiness readiness) {
  switch (readiness) {
    case LineaMicraReadiness::OFF: return "off";
    case LineaMicraReadiness::WARMING_UP: return "warming_up";
    case LineaMicraReadiness::WAITING_FOR_STEAM: return "waiting_for_steam";
    case LineaMicraReadiness::READY: return "ready";
    case LineaMicraReadiness::NEEDS_WATER: return "needs_water";
    case LineaMicraReadiness::UNKNOWN: return "unknown";
  }
  return "unknown";
}

inline const char *lineaMicraObservationQualityName(
    LineaMicraObservationQuality quality) {
  switch (quality) {
    case LineaMicraObservationQuality::Disabled: return "disabled";
    case LineaMicraObservationQuality::UNCONFIGURED: return "unconfigured";
    case LineaMicraObservationQuality::CURRENT: return "current";
    case LineaMicraObservationQuality::OPTIMISTIC: return "optimistic";
    case LineaMicraObservationQuality::STALE: return "stale";
    case LineaMicraObservationQuality::COMMUNICATION_ERROR:
      return "communication_error";
    case LineaMicraObservationQuality::UNSUPPORTED: return "unsupported";
  }
  return "unavailable";
}

inline const char *lineaMicraPhaseName(LineaMicraPhase phase) {
  switch (phase) {
    case LineaMicraPhase::Disabled: return "disabled";
    case LineaMicraPhase::IDLE: return "idle";
    case LineaMicraPhase::QUEUED: return "queued";
    case LineaMicraPhase::PAUSED: return "paused";
    case LineaMicraPhase::AUTHENTICATING: return "authenticating";
    case LineaMicraPhase::LISTING: return "listing";
    case LineaMicraPhase::RUNNING: return "running";
    case LineaMicraPhase::BACKOFF: return "backoff";
    case LineaMicraPhase::CONFIRMED: return "confirmed";
    case LineaMicraPhase::FAILED: return "failed";
  }
  return "disabled";
}

inline const char *lineaMicraErrorName(LineaMicraError error) {
  switch (error) {
    case LineaMicraError::NONE: return "none";
    case LineaMicraError::NO_STA: return "sta_required";
    case LineaMicraError::AP_MODE: return "ap_mode";
    case LineaMicraError::CLOCK_UNSYNCED: return "clock_unsynchronized";
    case LineaMicraError::INVALID_AUTH: return "invalid_auth";
    case LineaMicraError::NO_MACHINES: return "no_machines";
    case LineaMicraError::HTTP_ERROR: return "communication_error";
    case LineaMicraError::TRANSPORT: return "transport_error";
    case LineaMicraError::CANCELED: return "canceled";
    case LineaMicraError::REJECTED: return "rejected";
    case LineaMicraError::UNCONFIRMED: return "unconfirmed";
    case LineaMicraError::POLICY_REQUIRED: return "policy_required";
  }
  return "unknown";
}

inline const char *lineaMicraTemperatureStateName(
    LineaMicraTemperatureState state) {
  switch (state) {
    case LineaMicraTemperatureState::Disabled: return "disabled";
    case LineaMicraTemperatureState::IDLE: return "idle";
    case LineaMicraTemperatureState::PENDING: return "pending";
    case LineaMicraTemperatureState::RUNNING: return "running";
    case LineaMicraTemperatureState::CONFIRMED: return "confirmed";
    case LineaMicraTemperatureState::FAILED: return "failed";
    case LineaMicraTemperatureState::CANCELED: return "canceled";
  }
  return "disabled";
}

inline bool lineaMicraHttpRetryable(uint16_t status) {
  return status < 200 || status == 401 || status == 408 || status == 425 ||
         status == 429 || status >= 500;
}

inline bool lineaMicraTemperatureCycleRetryable(LineaMicraError error) {
  return error == LineaMicraError::HTTP_ERROR ||
         error == LineaMicraError::TRANSPORT ||
         error == LineaMicraError::UNCONFIRMED;
}

struct LineaMicraRequest {
  uint32_t requestId = 0;
  uint32_t configGeneration = 0;
  uint16_t targetDeciC = 0;
  uint8_t presetId = 0;
  LineaMicraRequestType type = LineaMicraRequestType::OBSERVE_STATE;
};

struct LineaMicraMachineSummary {
  char serial[LINEA_MICRA_SERIAL_CAPACITY] = {};
  char name[LINEA_MICRA_NAME_CAPACITY] = {};
};

struct LineaMicraDiscoverySnapshot {
  uint32_t requestId = 0;
  uint32_t generation = 0;
  uint8_t count = 0;
  // Machines on the saved account at its latest listing; 0 until listed.
  uint8_t accountMachines = 0;
  bool listing = false;
  LineaMicraError error = LineaMicraError::NONE;
  LineaMicraMachineSummary machines[LINEA_MICRA_MAX_ACCOUNT_MACHINES] = {};
};

// Only immutable, firmware-owned labels belong here, never cloud response data.
struct LineaMicraCloudCall {
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

struct LineaMicraStatus {
  uint32_t requestId = 0;
  uint32_t sampleAtMs = 0;
  uint32_t temperatureAtMs = 0;
  uint32_t coffeeReadyAtUtcSec = 0;
  uint32_t steamReadyAtUtcSec = 0;
  uint32_t configGeneration = 0;
  uint32_t identityGeneration = 0;
  uint16_t targetDeciC = 0;
  uint16_t requestedTargetDeciC = 0;
  uint16_t appliedTargetDeciC = 0;
  int32_t transportStatus = 0;
  int32_t temperatureTransportStatus = 0;
  uint16_t httpStatus = 0;
  uint16_t temperatureHttpStatus = 0;
  LineaMicraPhase phase = LineaMicraPhase::Disabled;
  LineaMicraError error = LineaMicraError::NONE;
  LineaMicraPowerState powerState = LineaMicraPowerState::UNKNOWN;
  MicraObservationSource powerSource = MicraObservationSource::NONE;
  LineaMicraObservedMode observedMode = LineaMicraObservedMode::NONE;
  LineaMicraObservationQuality quality = LineaMicraObservationQuality::Disabled;
  LineaMicraBoilerState coffeeBoiler = LineaMicraBoilerState::UNKNOWN;
  LineaMicraBoilerState steamBoiler = LineaMicraBoilerState::UNKNOWN;
  LineaMicraSteamLevel steamLevel = LineaMicraSteamLevel::UNKNOWN;
  LineaMicraTemperatureState temperatureState =
      LineaMicraTemperatureState::Disabled;
  LineaMicraError temperatureError = LineaMicraError::NONE;
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

// Informational rollup of the dashboard boiler widgets. Evidence-driven: a
// HeatingUp or EcoMode boiler is below temperature (warming), a Ready boiler
// is at temperature, and an Off steam boiler never blocks readiness because
// it was disabled on purpose. Contradictory or absent evidence (standby,
// off, unknown, or unsupported boilers while the machine reports ON) stays
// UNKNOWN instead of guessing, so the Home lamp can fall back to a neutral
// label. This layer never feeds control: power remains ON/OFF only.
inline LineaMicraReadiness lineaMicraReadiness(const LineaMicraStatus &status) {
  if (status.optimisticOff) return LineaMicraReadiness::OFF;
  // An optimistic ON overlay means the machine is waking while the confirmed
  // state still reads OFF: any retained boiler evidence predates the standby,
  // so the rollup must stay UNKNOWN and the lamp neutral rather than claim a
  // readiness the reheating machine cannot have yet.
  if (status.powerState == LineaMicraPowerState::OFF && !status.optimisticOn)
    return LineaMicraReadiness::OFF;
  if (status.powerState != LineaMicraPowerState::ON) return LineaMicraReadiness::UNKNOWN;
  const LineaMicraBoilerState coffee = status.coffeeBoiler;
  const LineaMicraBoilerState steam = status.steamBoiler;
  if (coffee == LineaMicraBoilerState::NO_WATER ||
      steam == LineaMicraBoilerState::NO_WATER)
    return LineaMicraReadiness::NEEDS_WATER;
  const auto warming = [](LineaMicraBoilerState boiler) {
    return boiler == LineaMicraBoilerState::HEATING_UP ||
        boiler == LineaMicraBoilerState::ECO;
  };
  if (coffee == LineaMicraBoilerState::READY) {
    if (steam == LineaMicraBoilerState::READY ||
        steam == LineaMicraBoilerState::OFF)
      return LineaMicraReadiness::READY;
    if (warming(steam)) return LineaMicraReadiness::WAITING_FOR_STEAM;
    return LineaMicraReadiness::UNKNOWN;
  }
  if (warming(coffee)) return LineaMicraReadiness::WARMING_UP;
  return LineaMicraReadiness::UNKNOWN;
}

static_assert(sizeof(LineaMicraRequest) <= 16,
              "Linea Micra request must stay compact");
// Boiler readiness (states, steam level, readyAt timestamps) widened the
// observation payload beyond the historic 64-byte budget; 80 keeps it
// copy-cheap for the per-tick effective-status handoff.
static_assert(sizeof(LineaMicraStatus) <= 80,
              "Linea Micra status must stay compact");
static_assert(std::is_trivially_copyable<LineaMicraRequest>::value);
static_assert(std::is_trivially_copyable<LineaMicraStatus>::value);
static_assert(std::is_trivially_copyable<LineaMicraDiscoverySnapshot>::value);

}  // namespace openbrewbyweight

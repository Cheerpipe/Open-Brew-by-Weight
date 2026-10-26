#pragma once

#include <cstring>

#include "OpenBrewByWeightMicraPowerState.h"

struct cJSON;

namespace openbrewbyweight {

enum class MicraCleaningState : uint8_t { UNKNOWN, OFF, REQUESTED, CLEANING };

struct MicraObservationStamp {
  uint32_t identity = 0;
  uint32_t epoch = 0;
  uint32_t intent = 0;
  uint32_t powerRevision = 0;
  uint32_t temperatureRevision = 0;
  uint32_t connectionRevision = 0;
  uint32_t onlineRevision = 0;
};

struct MicraObservation {
  MicraObservationStamp stamp;
  uint32_t receivedAtMs = 0;
  uint32_t coffeeReadyAtUtcSec = 0;
  uint32_t steamReadyAtUtcSec = 0;
  uint16_t targetDeciC = 0;
  MicraObservationSource source = MicraObservationSource::HTTP;
  LineaMicraObservedMode mode = LineaMicraObservedMode::NONE;
  LineaMicraBoilerState coffeeBoiler = LineaMicraBoilerState::UNKNOWN;
  LineaMicraBoilerState steamBoiler = LineaMicraBoilerState::UNKNOWN;
  LineaMicraSteamLevel steamLevel = LineaMicraSteamLevel::UNKNOWN;
  MicraCleaningState cleaning = MicraCleaningState::UNKNOWN;
  char cleaningLabel[33] = {};
  bool powerPresent = false;
  bool temperaturePresent = false;
  bool temperatureValid = false;
  bool coffeeBoilerPresent = false;
  bool steamBoilerPresent = false;
  bool cleaningPresent = false;
  bool cleaningAvailable = false;
  bool connectedPresent = false;
  bool connected = false;
};

bool decodeMicraDashboard(const cJSON *root, MicraObservation &update);

// Copies vendor text, truncating an over-long value at a UTF-8 character
// boundary instead of rejecting it.
inline void copyMicraText(char *destination, size_t capacity, const char *source) {
  size_t length = std::strlen(source);
  if (length >= capacity) {
    length = capacity - 1;
    while (length && (static_cast<unsigned char>(source[length]) & 0xc0) == 0x80) --length;
  }
  std::memcpy(destination, source, length);
  destination[length] = '\0';
}

// The facade owns this revision fence and the authoritative status; no copy
// of machine power lives in an adapter. Call only under the facade mutex.
struct MicraObservationFence {
  uint32_t epoch = 0;
  uint32_t powerRevision = 0;
  uint32_t temperatureRevision = 0;
  uint32_t connectionRevision = 0;
  uint32_t onlineRevision = 0;
  bool offline = false;
  bool synchronized = false;
  // Evidence accepted on this subscription with no rejected push since: while
  // it holds, every later change arrives as a push, so a command asking for
  // the trusted value would change nothing.
  bool temperatureTrusted = false;
  bool powerTrusted = false;

  void invalidate() {
    ++connectionRevision;
    synchronized = temperatureTrusted = powerTrusted = false;
  }

  MicraObservationStamp stamp(uint32_t identity, uint32_t intent) const {
    return {identity, epoch, intent, powerRevision, temperatureRevision, connectionRevision, onlineRevision};
  }

  bool merge(LineaMicraStatus &status, LineaMicraPowerStateTracker &power,
             const MicraObservation &update, bool observing,
             bool reconcile = false) {
    if (update.stamp.identity != status.identityGeneration ||
        update.stamp.epoch != epoch) return false;
    const bool push = update.source == MicraObservationSource::WEBSOCKET;
    if (!push && (update.stamp.connectionRevision != connectionRevision ||
        (update.connectedPresent && !update.connected &&
         (update.stamp.powerRevision != powerRevision ||
          update.stamp.onlineRevision != onlineRevision)))) return true;
    if (update.connectedPresent && (push || update.stamp.onlineRevision == onlineRevision)) {
      ++onlineRevision;
      if (offline != !update.connected) {
        offline = !update.connected;
        invalidate();
      }
    }
    if (offline) {
      power.hold(status, observing, update.receivedAtMs);
      status.quality = LineaMicraObservationQuality::STALE;
      return true;
    }
    if (update.powerPresent && observing &&
        (push || update.stamp.powerRevision == powerRevision) &&
        power.noteEvidence(update.stamp.intent,
                           lineaMicraPowerStateForMode(update.mode),
                           update.receivedAtMs, reconcile)) {
      status.observedMode = update.mode;
      status.powerState = lineaMicraPowerStateForMode(update.mode);
      status.effectiveOn = status.powerState != LineaMicraPowerState::OFF;
      status.sampleAtMs = update.receivedAtMs;
      status.powerSource = update.source;
      synchronized = powerTrusted = status.powerState != LineaMicraPowerState::UNKNOWN;
      status.quality = update.mode == LineaMicraObservedMode::UNSUPPORTED
                           ? LineaMicraObservationQuality::UNSUPPORTED
                           : update.mode == LineaMicraObservedMode::NONE
                           ? LineaMicraObservationQuality::STALE
                           : LineaMicraObservationQuality::CURRENT;
      ++powerRevision;
    }
    // Boiler-widget data rides the temperature revision: the coffee target
    // and both boilers' status/estimate fields arrive in the same widget
    // outputs, and a stale poll must not clobber fresher pushed states.
    const bool boilerWidget = update.temperaturePresent ||
        update.coffeeBoilerPresent || update.steamBoilerPresent;
    if (boilerWidget &&
        (push || update.stamp.temperatureRevision == temperatureRevision)) {
      if (update.temperaturePresent) {
        status.targetValid = temperatureTrusted = update.temperatureValid;
        status.targetDeciC = update.targetDeciC;
      }
      if (update.coffeeBoilerPresent) {
        status.coffeeBoiler = update.coffeeBoiler;
        status.coffeeReadyAtUtcSec = update.coffeeReadyAtUtcSec;
      }
      // temperatureAtMs keeps its REST-facing name but now timestamps the
      // last boiler-widget frame, which is what the boiler age renders.
      status.temperatureAtMs = update.receivedAtMs;
      if (update.steamBoilerPresent) {
        status.steamBoiler = update.steamBoiler;
        status.steamReadyAtUtcSec = update.steamReadyAtUtcSec;
        status.steamLevel = update.steamLevel;
      }
      ++temperatureRevision;
    }
    return true;
  }
};

static_assert(std::is_trivially_copyable<MicraObservation>::value);
// Boiler readiness fields (states, steam level, readyAt timestamps) widened
// the observation beyond the historic 80-byte budget; 96 keeps it compact
// for the per-frame decode path.
static_assert(sizeof(MicraObservation) <= 96);

}  // namespace openbrewbyweight

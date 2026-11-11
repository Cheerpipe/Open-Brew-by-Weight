#pragma once

#include <cstring>

#include "OpenBrewByWeightLaMarzoccoPowerState.h"

struct cJSON;

namespace openbrewbyweight {

enum class LaMarzoccoCleaningState : uint8_t { UNKNOWN, OFF, REQUESTED, CLEANING };

struct LaMarzoccoObservationStamp {
  uint32_t identity = 0;
  uint32_t epoch = 0;
  uint32_t intent = 0;
  uint32_t powerRevision = 0;
  uint32_t temperatureRevision = 0;
  uint32_t connectionRevision = 0;
  uint32_t onlineRevision = 0;
};

struct LaMarzoccoObservation {
  LaMarzoccoObservationStamp stamp;
  uint32_t receivedAtMs = 0;
  uint32_t coffeeReadyAtUtcSec = 0;
  uint32_t steamReadyAtUtcSec = 0;
  uint16_t targetDeciC = 0;
  uint16_t targetMinDeciC = 0;  // The machine's brew range; 0 when the frame names none.
  uint16_t targetMaxDeciC = 0;
  LaMarzoccoObservationSource source = LaMarzoccoObservationSource::HTTP;
  LaMarzoccoObservedMode mode = LaMarzoccoObservedMode::NONE;
  LaMarzoccoBoilerState coffeeBoiler = LaMarzoccoBoilerState::UNKNOWN;
  LaMarzoccoBoilerState steamBoiler = LaMarzoccoBoilerState::UNKNOWN;
  LaMarzoccoSteamLevel steamLevel = LaMarzoccoSteamLevel::UNKNOWN;
  LaMarzoccoCleaningState cleaning = LaMarzoccoCleaningState::UNKNOWN;
  char cleaningLabel[33] = {};
  bool powerPresent = false;
  bool temperaturePresent = false;
  bool temperatureValid = false;
  bool coffeeBoilerPresent = false;
  bool steamBoilerPresent = false;
  // The steam fields come from the temperature widget, not the level widget.
  bool steamByTemperature = false;
  bool cleaningPresent = false;
  bool cleaningAvailable = false;
  bool connectedPresent = false;
  bool connected = false;
};

bool decodeLaMarzoccoDashboard(const cJSON *root, LaMarzoccoObservation &update);

// Hears each account coffee machine the listing leaves out: its La Marzocco
// modelCode, or nullptr when the model matched and the serial cannot be kept.
using LaMarzoccoThingSkipped = void (*)(const char *model);
// The account's coffee machines whose modelCode is the machine profile's, from
// the cloud's things array; false when the payload is not that array.
bool decodeLaMarzoccoThings(const cJSON *root, const char *profileModel,
                       LaMarzoccoDiscoverySnapshot &result,
                       LaMarzoccoThingSkipped skipped);

// Copies vendor text, truncating an over-long value at a UTF-8 character
// boundary instead of rejecting it.
inline void copyLaMarzoccoText(char *destination, size_t capacity, const char *source) {
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
struct LaMarzoccoObservationFence {
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

  LaMarzoccoObservationStamp stamp(uint32_t identity, uint32_t intent) const {
    return {identity, epoch, intent, powerRevision, temperatureRevision, connectionRevision, onlineRevision};
  }

  bool merge(LaMarzoccoStatus &status, LaMarzoccoPowerStateTracker &power,
             const LaMarzoccoObservation &update, bool observing,
             bool reconcile = false) {
    if (update.stamp.identity != status.identityGeneration ||
        update.stamp.epoch != epoch) return false;
    const bool push = update.source == LaMarzoccoObservationSource::WEBSOCKET;
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
      status.quality = LaMarzoccoObservationQuality::STALE;
      return true;
    }
    if (update.powerPresent && observing &&
        (push || update.stamp.powerRevision == powerRevision) &&
        power.noteEvidence(update.stamp.intent,
                           laMarzoccoPowerStateForMode(update.mode),
                           update.receivedAtMs, reconcile)) {
      status.observedMode = update.mode;
      status.powerState = laMarzoccoPowerStateForMode(update.mode);
      status.effectiveOn = status.powerState != LaMarzoccoPowerState::OFF;
      status.sampleAtMs = update.receivedAtMs;
      status.powerSource = update.source;
      synchronized = powerTrusted = status.powerState != LaMarzoccoPowerState::UNKNOWN;
      status.quality = update.mode == LaMarzoccoObservedMode::UNSUPPORTED
                           ? LaMarzoccoObservationQuality::UNSUPPORTED
                           : update.mode == LaMarzoccoObservedMode::NONE
                           ? LaMarzoccoObservationQuality::STALE
                           : LaMarzoccoObservationQuality::CURRENT;
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
      // The brew range is the machine's own. A frame that names none keeps
      // the last one, and a machine that never names one gets the default.
      if (update.targetMinDeciC != 0) {
        status.targetMinDeciC = update.targetMinDeciC;
        status.targetMaxDeciC = update.targetMaxDeciC;
      } else if (status.targetMinDeciC == 0 &&
                 (update.temperaturePresent || update.coffeeBoilerPresent)) {
        status.targetMinDeciC = LA_MARZOCCO_BREW_RANGE_DEFAULT_MIN_DECI_C;
        status.targetMaxDeciC = LA_MARZOCCO_BREW_RANGE_DEFAULT_MAX_DECI_C;
      }
      if (update.coffeeBoilerPresent) {
        status.coffeeBoiler = update.coffeeBoiler;
        status.coffeeReadyAtUtcSec = update.coffeeReadyAtUtcSec;
      }
      // temperatureAtMs keeps its REST-facing name but now timestamps the
      // last boiler-widget frame, which is what the boiler age renders.
      status.temperatureAtMs = update.receivedAtMs;
      // A machine that reports a steam level keeps the level widget as its
      // steam boiler: the temperature widget never replaces it.
      if (update.steamBoilerPresent &&
          !(update.steamByTemperature &&
            status.steamLevel != LaMarzoccoSteamLevel::UNKNOWN)) {
        status.steamBoiler = update.steamBoiler;
        status.steamReadyAtUtcSec = update.steamReadyAtUtcSec;
        status.steamLevel = update.steamLevel;
      }
      ++temperatureRevision;
    }
    return true;
  }
};

static_assert(std::is_trivially_copyable<LaMarzoccoObservation>::value);
// Boiler readiness fields (states, steam level, readyAt timestamps) widened
// the observation beyond the historic 80-byte budget; 96 keeps it compact
// for the per-frame decode path.
static_assert(sizeof(LaMarzoccoObservation) <= 96);

}  // namespace openbrewbyweight

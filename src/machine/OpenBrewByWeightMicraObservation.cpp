#include "OpenBrewByWeightMicraObservation.h"

#include <cJSON.h>
#include <cmath>
#include <cstring>

namespace openbrewbyweight {
namespace {
const cJSON *find(const cJSON *object, const char *key) {
  return cJSON_GetObjectItemCaseSensitive(object, key);
}

// cJSON keeps duplicate JSON keys as siblings after the first value; a repeat
// of a key this decoder consumes is a protocol error, not a silent override.
bool duplicated(const cJSON *value, const char *key) {
  for (const cJSON *field = value ? value->next : nullptr; field; field = field->next)
    if (field->string && strcmp(field->string, key) == 0) return true;
  return false;
}

const char *text(const cJSON *object, const char *key) {
  const cJSON *value = find(object, key);
  if (duplicated(value, key) || !cJSON_IsString(value)) return nullptr;
  return value->valuestring;
}

LineaMicraObservedMode mode(const char *value) {
  if (strcmp(value, "StandBy") == 0) return LineaMicraObservedMode::STANDBY;
  if (strcmp(value, "BrewingMode") == 0) return LineaMicraObservedMode::BREWING;
  if (strcmp(value, "EcoMode") == 0) return LineaMicraObservedMode::ECO;
  return LineaMicraObservedMode::UNSUPPORTED;
}

LineaMicraBoilerState boilerState(const char *value) {
  if (strcmp(value, "StandBy") == 0) return LineaMicraBoilerState::STANDBY;
  if (strcmp(value, "HeatingUp") == 0) return LineaMicraBoilerState::HEATING_UP;
  if (strcmp(value, "Ready") == 0) return LineaMicraBoilerState::READY;
  if (strcmp(value, "NoWater") == 0) return LineaMicraBoilerState::NO_WATER;
  if (strcmp(value, "EcoMode") == 0) return LineaMicraBoilerState::ECO;
  if (strcmp(value, "Off") == 0) return LineaMicraBoilerState::OFF;
  return LineaMicraBoilerState::UNSUPPORTED;
}

LineaMicraSteamLevel steamLevel(const char *value) {
  if (strcmp(value, "Level1") == 0) return LineaMicraSteamLevel::LEVEL_1;
  if (strcmp(value, "Level2") == 0) return LineaMicraSteamLevel::LEVEL_2;
  if (strcmp(value, "Level3") == 0) return LineaMicraSteamLevel::LEVEL_3;
  return LineaMicraSteamLevel::UNSUPPORTED;
}

// True when a widgets or removedWidgets array names this widget code.
bool names(const cJSON *list, const char *code) {
  const cJSON *entry = nullptr;
  cJSON_ArrayForEach(entry, list) {
    const char *name = cJSON_IsString(entry) ? entry->valuestring : text(entry, "code");
    if (name != nullptr && strcmp(name, code) == 0) return true;
  }
  return false;
}

// readyStartTime is cloud epoch time: milliseconds in the captured payloads,
// but live machines also report zero or seconds-scale values for the same
// field. An estimate that cannot be interpreted is absent; this display-only
// field must never reject the dashboard frame it rides on.
void readyAtSeconds(const cJSON *value, uint32_t &seconds) {
  seconds = 0;
  const double epoch = cJSON_IsNumber(value) && std::isfinite(value->valuedouble)
                           ? value->valuedouble
                           : 0.0;
  if (epoch >= 1.5e12 && epoch <= 4.2e12)
    seconds = static_cast<uint32_t>(epoch / 1000.0);
  else if (epoch >= 1.5e9 && epoch < 1.5e12)
    seconds = static_cast<uint32_t>(epoch);
}
}  // namespace

bool decodeMicraDashboard(const cJSON *root, MicraObservation &update,
                          const char **unsupported) {
  if (!cJSON_IsObject(root)) return false;
  unsigned seen = 0;
  for (const cJSON *field = root->child; field; field = field->next) {
    if (!field->string) return false;
    const unsigned bit = strcmp(field->string, "connected") == 0 ? 1U
        : strcmp(field->string, "widgets") == 0 ? 2U
        : strcmp(field->string, "removedWidgets") == 0 ? 4U : 0U;
    if (seen & bit) return false;
    seen |= bit;
  }
  MicraObservation next = update;
  const cJSON *connected = find(root, "connected");
  if (connected != nullptr) {
    if (!cJSON_IsBool(connected)) return false;
    next.connectedPresent = true;
    next.connected = cJSON_IsTrue(connected);
  }
  const cJSON *widgets = find(root, "widgets");
  if (widgets != nullptr && !cJSON_IsArray(widgets)) return false;
  // A machine reports its steam boiler through one widget: a level (Linea
  // Micra, Mini R) or a temperature (Linea Mini). A frame that carries the
  // level widget is read exactly as before the temperature widget existed
  // here: the temperature code is ignored in it, removal included.
  const bool levelFrame = names(widgets, "CMSteamBoilerLevel");
  const char *steamCode = levelFrame ? "CMSteamBoilerLevel" : "CMSteamBoilerTemperature";
  const cJSON *widget = nullptr;
  cJSON_ArrayForEach(widget, widgets) {
    // A widget without a string code cannot be one this decoder relies on;
    // skip it so one new vendor widget shape cannot stall observation.
    if (!cJSON_IsObject(widget)) continue;
    // A repeated code is ambiguous (it may name a control widget): reject.
    if (duplicated(find(widget, "code"), "code")) return false;
    const char *code = text(widget, "code");
    if (code == nullptr) continue;
    const bool power = strcmp(code, "CMMachineStatus") == 0;
    const bool boiler = strcmp(code, "CMCoffeeBoiler") == 0;
    const bool steam = strcmp(code, steamCode) == 0;
    const bool cleaning = strcmp(code, "CMBackFlush") == 0;
    if (!power && !boiler && !steam && !cleaning) continue;
    const cJSON *output = find(widget, "output");
    if (duplicated(output, "output") || !cJSON_IsObject(output)) return false;
    if (power) {
      const cJSON *value = find(output, "mode");
      if (duplicated(value, "mode")) return false;
      if (value == nullptr) continue;
      if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
      if (next.powerPresent) return false;
      next.powerPresent = true;
      next.mode = mode(value->valuestring);
    } else if (boiler || steam) {
      // Occurrence guards are field-keyed (status/level/target): the cloud
      // ships complete widget outputs, and a fragment would simply update
      // only the fields it carries. These readiness fields are display-only:
      // a value with an unexpected type degrades to absent rather than
      // rejecting the observation frame, unlike the control-relevant target
      // temperature below, which keeps its historical strictness.
      const cJSON *status = find(output, "status");
      if (duplicated(status, "status")) return false;
      if (status != nullptr && cJSON_IsString(status) &&
          status->valuestring != nullptr) {
        if (boiler ? next.coffeeBoilerPresent : next.steamBoilerPresent) return false;
        if (boiler) {
          next.coffeeBoilerPresent = true;
          next.coffeeBoiler = boilerState(status->valuestring);
        } else {
          next.steamBoilerPresent = true;
          next.steamBoiler = boilerState(status->valuestring);
        }
      }
      const cJSON *ready = find(output, "readyStartTime");
      if (duplicated(ready, "readyStartTime")) return false;
      uint32_t readyAt = 0;
      if (ready != nullptr) readyAtSeconds(ready, readyAt);
      const cJSON *target = find(output, boiler ? "targetTemperature" : "targetLevel");
      if (duplicated(target, boiler ? "targetTemperature" : "targetLevel")) return false;
      if (boiler) {
        if (ready != nullptr) next.coffeeReadyAtUtcSec = readyAt;
        if (target == nullptr) continue;
        if (!cJSON_IsNumber(target) || !std::isfinite(target->valuedouble) ||
            next.temperaturePresent) return false;
        if (target->valuedouble < LINEA_MICRA_BREW_TARGET_MIN_DECI_C / 10.0 ||
            target->valuedouble > LINEA_MICRA_BREW_TARGET_MAX_DECI_C / 10.0) {
          if (unsupported != nullptr) *unsupported = "brew target outside 80.0-100.0 C";
          return false;
        }
        next.temperaturePresent = next.temperatureValid = true;
        next.targetDeciC = static_cast<uint16_t>(std::lround(target->valuedouble * 10.0));
      } else {
        next.steamByTemperature = !levelFrame;
        if (ready != nullptr) next.steamReadyAtUtcSec = readyAt;
        if (levelFrame && target != nullptr && cJSON_IsString(target) &&
            target->valuestring != nullptr)
          next.steamLevel = steamLevel(target->valuestring);
      }
    } else if (next.source == MicraObservationSource::WEBSOCKET) {
      const cJSON *value = find(output, "status");
      if (duplicated(value, "status")) return false;
      if (value == nullptr) continue;
      if (!cJSON_IsString(value) || value->valuestring == nullptr ||
          next.cleaningPresent) return false;
      next.cleaningPresent = next.cleaningAvailable = true;
      const char *label = value->valuestring;
      next.cleaning = strcmp(label, "Off") == 0 ? MicraCleaningState::OFF
          : strcmp(label, "Requested") == 0 ? MicraCleaningState::REQUESTED
          : strcmp(label, "Cleaning") == 0 ? MicraCleaningState::CLEANING
          : MicraCleaningState::UNKNOWN;
      copyMicraText(next.cleaningLabel, sizeof(next.cleaningLabel), label);
    }
  }
  const cJSON *removed = find(root, "removedWidgets");
  if (removed != nullptr && !cJSON_IsArray(removed)) return false;
  cJSON_ArrayForEach(widget, removed) {
    const char *code = cJSON_IsString(widget) ? widget->valuestring : text(widget, "code");
    if (code == nullptr) continue;
    if (strcmp(code, "CMMachineStatus") == 0) {
      next.powerPresent = true;
      next.mode = LineaMicraObservedMode::NONE;
    } else if (strcmp(code, "CMCoffeeBoiler") == 0) {
      next.temperaturePresent = true;
      next.temperatureValid = false;
      next.targetDeciC = 0;
      next.coffeeBoilerPresent = true;
      next.coffeeBoiler = LineaMicraBoilerState::UNSUPPORTED;
      next.coffeeReadyAtUtcSec = 0;
    } else if (const bool byTemperature = strcmp(code, "CMSteamBoilerTemperature") == 0;
               byTemperature
                   ? !levelFrame
                   // A level removal never undoes the temperature widget this
                   // frame just read.
                   : strcmp(code, "CMSteamBoilerLevel") == 0 &&
                         !(next.steamBoilerPresent && next.steamByTemperature)) {
      next.steamByTemperature = byTemperature;
      next.steamBoilerPresent = true;
      next.steamBoiler = LineaMicraBoilerState::UNSUPPORTED;
      next.steamReadyAtUtcSec = 0;
      next.steamLevel = LineaMicraSteamLevel::UNKNOWN;
    } else if (strcmp(code, "CMBackFlush") == 0 &&
               next.source == MicraObservationSource::WEBSOCKET) {
      if (next.cleaningPresent) return false;
      next.cleaningPresent = true;
      next.cleaningAvailable = false;
      next.cleaning = MicraCleaningState::UNKNOWN;
      next.cleaningLabel[0] = '\0';
    }
  }
  update = next;
  return widgets != nullptr || removed != nullptr || connected != nullptr;
}

bool decodeMicraThings(const cJSON *root, const char *profileModel,
                       LineaMicraDiscoverySnapshot &result,
                       MicraThingSkipped skipped) {
  if (!cJSON_IsArray(root)) return false;
  const cJSON *thing = nullptr;
  cJSON_ArrayForEach(thing, root) {
    if (result.count >= LINEA_MICRA_MAX_ACCOUNT_MACHINES) break;
    const char *type = text(thing, "type");
    if (type == nullptr || strcmp(type, "CoffeeMachine") != 0) continue;
    const char *model = text(thing, "modelCode");
    const char *serial = text(thing, "serialNumber");
    const bool wanted = model != nullptr && strcmp(model, profileModel) == 0;
    if (!wanted || serial == nullptr || !validLineaMicraSerial(serial)) {
      skipped(wanted ? nullptr : model == nullptr ? "unknown" : model);
      continue;
    }
    LineaMicraMachineSummary &machine = result.machines[result.count++];
    std::memset(machine.serial, 0, sizeof(machine.serial));
    std::memcpy(machine.serial, serial, std::strlen(serial));  // Validated to fit.
    const char *name = text(thing, "name");
    copyMicraText(machine.name, sizeof(machine.name),
                  name == nullptr || name[0] == '\0' ? serial : name);
  }
  return true;
}
}  // namespace openbrewbyweight

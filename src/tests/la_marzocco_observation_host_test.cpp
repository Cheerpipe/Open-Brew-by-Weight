#include "machine/OpenBrewByWeightLaMarzoccoObservation.h"
#include "la_marzocco_power_test_helpers.h"
#include <cJSON.h>
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace openbrewbyweight;

static bool decode(const char *json, LaMarzoccoObservation &update) {
  cJSON *root = cJSON_Parse(json);
  const bool valid = decodeLaMarzoccoDashboard(root, update);
  cJSON_Delete(root);
  return valid;
}

static void initialStateContinuity() {
  for (bool pushFirst : {false, true}) {
    LaMarzoccoStatus state;
    state.identityGeneration = 1;
    state.connectionFreshness = true;
    LaMarzoccoPowerStateTracker power;
    LaMarzoccoObservationFence fence;
    LaMarzoccoObservation initial;
    initial.stamp = fence.stamp(1, power.generation());
    initial.source = LaMarzoccoObservationSource::HTTP_INITIAL;
    initial.powerPresent = initial.temperaturePresent = initial.temperatureValid = true;
    initial.mode = LaMarzoccoObservedMode::STANDBY;
    initial.targetDeciC = 930;
    initial.receivedAtMs = 100;
    LaMarzoccoObservation push = initial;
    push.source = LaMarzoccoObservationSource::WEBSOCKET;
    push.temperaturePresent = false;
    push.mode = LaMarzoccoObservedMode::BREWING;
    push.receivedAtMs = 101;
    if (pushFirst) assert(fence.merge(state, power, push, true));
    assert(fence.merge(state, power, initial, true, true));
    assert(state.targetDeciC == 930);
    assert(state.powerSource == (pushFirst ? LaMarzoccoObservationSource::WEBSOCKET :
                                            LaMarzoccoObservationSource::HTTP_INITIAL));
    assert(state.powerState == (pushFirst ? LaMarzoccoPowerState::ON : LaMarzoccoPowerState::OFF));
    assert(fence.synchronized);
    for (uint32_t now : {30100U, 60100U, 600100U, 3600100U})
      assert(power.effectiveStatus(state, true, now).quality == LaMarzoccoObservationQuality::CURRENT);
    assert(fence.merge(state, power, push, true));
    assert(state.powerSource == LaMarzoccoObservationSource::WEBSOCKET);
    auto staleOffline = initial;
    staleOffline.connectedPresent = true;
    staleOffline.connected = false;
    assert(fence.merge(state, power, staleOffline, true, true));
    assert(fence.synchronized && !fence.offline && !retained(power));
    initial.stamp = fence.stamp(1, power.generation());
    initial.source = LaMarzoccoObservationSource::HTTP;
    initial.receivedAtMs = 200;
    assert(fence.merge(state, power, initial, true, true));  // Later explicit refresh.
    assert(state.powerSource == LaMarzoccoObservationSource::HTTP && state.sampleAtMs == 200);

    LaMarzoccoObservation offline;
    offline.source = LaMarzoccoObservationSource::WEBSOCKET;
    offline.stamp = fence.stamp(1, power.generation());
    offline.connectedPresent = true;
    offline.receivedAtMs = 300;
    auto late = initial;
    late.stamp = offline.stamp;
    assert(fence.merge(state, power, offline, true));
    assert(fence.offline && !fence.synchronized && retained(power));
    late.connectedPresent = late.connected = true;
    assert(fence.merge(state, power, late, true, true));
    assert(fence.offline && retained(power) && state.sampleAtMs == 200);
    offline.connected = true;
    assert(fence.merge(state, power, offline, true));
    assert(!fence.offline && !fence.synchronized && retained(power));
    push.powerPresent = false;
    assert(fence.merge(state, power, push, true));
    assert(power.effectiveStatus(state, true, 400).quality == LaMarzoccoObservationQuality::STALE);
    push.powerPresent = true;
    push.receivedAtMs = 500;
    assert(fence.merge(state, power, push, true));
    assert(fence.synchronized && !retained(power));
    late.connected = false;  // An older HTTP offline result cannot undo recovery.
    assert(fence.merge(state, power, late, true, true));
    assert(fence.synchronized && !fence.offline && state.sampleAtMs == 500);
    ++fence.epoch;
    assert(!fence.merge(state, power, late, true, true));

    state.connectionFreshness = false;
    assert(power.effectiveStatus(state, true, 30500).quality == LaMarzoccoObservationQuality::STALE);
  }
}

static void connectionOnlyOrdering() {
  LaMarzoccoStatus state;
  LaMarzoccoPowerStateTracker power;
  LaMarzoccoObservationFence fence;
  LaMarzoccoObservation initial;
  initial.source = LaMarzoccoObservationSource::HTTP_INITIAL;
  initial.stamp = fence.stamp(0, power.generation());
  initial.connectedPresent = true;
  initial.receivedAtMs = 100;
  LaMarzoccoObservation online = initial;
  online.source = LaMarzoccoObservationSource::WEBSOCKET;
  online.connected = true;
  assert(fence.merge(state, power, online, true));
  assert(fence.merge(state, power, initial, true));
  assert(!fence.offline && !retained(power));  // Older API offline cannot win.
  // Online-only push must not prevent independent power initialization.
  initial.connected = initial.powerPresent = true;
  initial.mode = LaMarzoccoObservedMode::STANDBY;
  assert(fence.merge(state, power, initial, true));
  assert(state.powerState == LaMarzoccoPowerState::OFF && fence.synchronized);
  assert(state.powerSource == LaMarzoccoObservationSource::HTTP_INITIAL);
  initial.stamp = fence.stamp(0, power.generation());
  initial.connected = false;
  assert(fence.merge(state, power, initial, true));
  assert(fence.offline && !fence.synchronized && retained(power));
}

int main() {
  initialStateContinuity();
  connectionOnlyOrdering();
  // Synthetic dashboard fixtures; no cloud or hardware is contacted.
  LaMarzoccoObservation update;
  assert(decode(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy","status":"Brewing"}},{"code":"CMCoffeeBoiler","output":{"targetTemperature":93.5}}]})", update));
  assert(update.mode == LaMarzoccoObservedMode::STANDBY);
  assert(update.targetDeciC == 935 && update.temperatureValid);
  update = {};
  assert(!decode(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMCoffeeBoiler","output":{"targetTemperature":"93"}}]})", update));
  assert(!update.powerPresent);  // No partial publication from invalid data.
  assert(decode(R"({"connected":false,"widgets":[{"code":"FutureWidget"}]})", update));
  // Malformed widgets the decoder does not use are skipped, not fatal.
  {
    LaMarzoccoObservation tolerant;
    assert(decode(R"({"widgets":[{"code":7},"note",{"output":{}},{"code":"CMMachineStatus","output":{"mode":"StandBy"}}]})", tolerant));
    assert(tolerant.powerPresent);
    assert(decode(R"({"removedWidgets":[{"name":"Future"},"CMBackFlush"]})", tolerant));
  }
  assert(update.connectedPresent && !update.connected && !update.powerPresent);
  update = {};
  assert(!decode(R"({"connected":true,"connected":false})", update));
  assert(!decode(R"({"widgets":[],"widgets":[]})", update));
  assert(!decode(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy"}},{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}}]})", update));
  assert(!update.powerPresent);
  assert(!decode(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode","mode":null}}]})", update));
  assert(!decode(R"({"widgets":[{"code":"CMMachineStatus","code":"Future","output":{"mode":"BrewingMode"}}]})", update));
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[{"code":"CMBackFlush","output":{"status":"123456789012345678901234567890éé"}}]})", update));
  assert(std::strcmp(update.cleaningLabel, "123456789012345678901234567890é") == 0);
  assert(update.cleaning == LaMarzoccoCleaningState::UNKNOWN);

  for (const char *label : {"Requested", "Cleaning", "Off", "Future"}) {
    char json[120];
    std::snprintf(json, sizeof(json), "{\"widgets\":[{\"code\":\"CMBackFlush\",\"output\":{\"status\":\"%s\"}}]}", label);
    update = {};
    assert(decode(json, update) && !update.cleaningPresent);
    update.source = LaMarzoccoObservationSource::WEBSOCKET;
    assert(decode(json, update) && update.cleaningPresent && update.cleaningAvailable);
    assert(std::strcmp(update.cleaningLabel, label) == 0);
  }
  update = {};
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  assert(decode(R"({"removedWidgets":["CMBackFlush","CMMachineStatus","CMCoffeeBoiler"]})", update));
  assert(update.cleaningPresent && !update.cleaningAvailable);
  assert(update.powerPresent && update.mode == LaMarzoccoObservedMode::NONE);
  assert(update.temperaturePresent && !update.temperatureValid);
  update = {};
  assert(decode(R"({"removedWidgets":[{"code":"CMMachineStatus"},{"code":"CMCoffeeBoiler"}]})", update));
  assert(update.powerPresent && update.mode == LaMarzoccoObservedMode::NONE);
  assert(update.temperaturePresent && !update.temperatureValid);

  // Boiler readiness widgets: status, estimate, and steam level.
  update = {};
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[)"
      R"({"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},)"
      R"({"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","targetTemperature":93.0,"readyStartTime":1760000460000}},)"
      R"({"code":"CMSteamBoilerLevel","output":{"status":"HeatingUp","targetLevel":"Level3","readyStartTime":1760000820500}}]})", update));
  assert(update.powerPresent && update.mode == LaMarzoccoObservedMode::BREWING);
  assert(update.temperaturePresent && update.temperatureValid && update.targetDeciC == 930);
  assert(update.coffeeBoilerPresent && update.coffeeBoiler == LaMarzoccoBoilerState::HEATING_UP);
  assert(update.coffeeReadyAtUtcSec == 1760000460U);
  assert(update.steamBoilerPresent && update.steamBoiler == LaMarzoccoBoilerState::HEATING_UP);
  assert(update.steamLevel == LaMarzoccoSteamLevel::LEVEL_3);
  assert(update.steamReadyAtUtcSec == 1760000820U);
  // Unknown status strings degrade to UNSUPPORTED instead of failing.
  update = {};
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"FutureMode","targetLevel":"Level4"}}]})", update));
  assert(update.steamBoiler == LaMarzoccoBoilerState::UNSUPPORTED);
  assert(update.steamLevel == LaMarzoccoSteamLevel::UNSUPPORTED);
  // Null estimate means no estimate; malformed values degrade to absent
  // instead of failing the frame (display-only field), and both epoch
  // milliseconds and seconds-scale values are interpreted.
  const auto fresh = [&update] {
    update = {};
    update.source = LaMarzoccoObservationSource::WEBSOCKET;
  };
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready","targetTemperature":94,"readyStartTime":null}}]})", update));
  assert(update.coffeeBoiler == LaMarzoccoBoilerState::READY && update.coffeeReadyAtUtcSec == 0);
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":"soon"}}]})", update));
  assert(update.coffeeBoiler == LaMarzoccoBoilerState::HEATING_UP && update.coffeeReadyAtUtcSec == 0);
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":1000}}]})", update));
  assert(update.coffeeReadyAtUtcSec == 0);  // Sub-epoch garbage: absent.
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":0}}]})", update));
  assert(update.coffeeReadyAtUtcSec == 0);
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":-1}}]})", update));
  assert(update.coffeeReadyAtUtcSec == 0);
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":1760000460}}]})", update));
  assert(update.coffeeReadyAtUtcSec == 1760000460U);  // Seconds-scale epoch.
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":1.76e12}}]})", update));
  assert(update.coffeeReadyAtUtcSec == 1760000000U);  // Millisecond epoch.
  fresh();
  // A non-string boiler status degrades to absent, not rejection.
  assert(decode(R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":7,"targetLevel":"Level2"}}]})", update));
  assert(!update.steamBoilerPresent && update.steamLevel == LaMarzoccoSteamLevel::LEVEL_2);
  fresh();
  assert(!decode(R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready","status":"Ready"}}]})", update));
  assert(!decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready"}},{"code":"CMCoffeeBoiler","output":{"status":"Ready"}}]})", update));
  update = {};
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  assert(decode(R"({"removedWidgets":["CMSteamBoilerLevel"]})", update));
  assert(update.steamBoilerPresent && update.steamBoiler == LaMarzoccoBoilerState::UNSUPPORTED);
  assert(update.steamLevel == LaMarzoccoSteamLevel::UNKNOWN && update.steamReadyAtUtcSec == 0);

  // The Linea Mini reports its steam boiler as a temperature widget: it feeds
  // the same state and estimate, and no level exists to read.
  fresh();
  assert(decode(R"({"widgets":[)"
      R"({"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},)"
      R"({"code":"CMCoffeeBoiler","output":{"status":"Ready","targetTemperature":93.0}},)"
      R"({"code":"CMSteamBoilerTemperature","output":{"status":"HeatingUp","targetTemperature":128.0,"targetLevel":"Level2","readyStartTime":1760000820500}}]})", update));
  assert(update.steamBoilerPresent && update.steamByTemperature);
  assert(update.steamBoiler == LaMarzoccoBoilerState::HEATING_UP);
  assert(update.steamReadyAtUtcSec == 1760000820U);
  // A level is read from the level widget only, as the companion forwards it.
  assert(update.steamLevel == LaMarzoccoSteamLevel::UNKNOWN);
  // Its steam target is not the brew target: the coffee boiler alone sets it.
  assert(update.temperaturePresent && update.targetDeciC == 930);
  assert(update.coffeeBoiler == LaMarzoccoBoilerState::READY);
  {
    LaMarzoccoStatus mini;
    mini.powerState = LaMarzoccoPowerState::ON;
    mini.coffeeBoiler = update.coffeeBoiler;
    mini.steamBoiler = update.steamBoiler;
    mini.assumedPower = laMarzoccoAssumedPower(mini);
    assert(laMarzoccoReadiness(mini) == LaMarzoccoReadiness::WAITING_FOR_STEAM);
  }
  // A steam boiler switched off never blocks readiness, whichever widget says so.
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"status":"Off"}}]})", update));
  assert(update.steamBoilerPresent && update.steamBoiler == LaMarzoccoBoilerState::OFF);
  // Without a status the widget carries no steam state, like the level widget.
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"readyStartTime":1760000820500}}]})", update));
  assert(!update.steamBoilerPresent && update.steamReadyAtUtcSec == 1760000820U);
  fresh();
  assert(decode(R"({"removedWidgets":["CMSteamBoilerTemperature"]})", update));
  assert(update.steamBoilerPresent && update.steamByTemperature);
  assert(update.steamBoiler == LaMarzoccoBoilerState::UNSUPPORTED);
  assert(update.steamLevel == LaMarzoccoSteamLevel::UNKNOWN && update.steamReadyAtUtcSec == 0);
  assert(!decode(R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"status":"Ready"}},{"code":"CMSteamBoilerTemperature","output":{"status":"Ready"}}]})", update));

  // A frame that carries the level widget decodes as it did before the
  // temperature widget was read: that code is ignored in it, whatever its
  // position, content, or removal.
  const char *levelOnly =
      R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready","targetLevel":"Level2","readyStartTime":1760000820500}}]})";
  fresh();
  assert(decode(levelOnly, update));
  const LaMarzoccoObservation level = update;
  assert(level.steamBoilerPresent && !level.steamByTemperature);
  assert(level.steamBoiler == LaMarzoccoBoilerState::READY);
  assert(level.steamLevel == LaMarzoccoSteamLevel::LEVEL_2);
  for (const char *frame : {
      R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready","targetLevel":"Level2","readyStartTime":1760000820500}}],"removedWidgets":["CMSteamBoilerTemperature"]})",
      R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready","targetLevel":"Level2","readyStartTime":1760000820500}},{"code":"CMSteamBoilerTemperature","output":{"status":"HeatingUp","readyStartTime":1760000999000}}]})",
      R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"status":"HeatingUp","readyStartTime":1760000999000}},{"code":"CMSteamBoilerLevel","output":{"status":"Ready","targetLevel":"Level2","readyStartTime":1760000820500}}]})",
      R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready","targetLevel":"Level2","readyStartTime":1760000820500}},{"code":"CMSteamBoilerTemperature","output":{"status":null,"readyStartTime":null}}]})"}) {
    fresh();
    assert(decode(frame, update));
    assert(std::memcmp(&update, &level, sizeof(update)) == 0);
  }
  // A temperature frame keeps its state when the same frame removes the level
  // widget the machine never had.
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"status":"Ready"}}],"removedWidgets":["CMSteamBoilerLevel"]})", update));
  assert(update.steamByTemperature && update.steamBoiler == LaMarzoccoBoilerState::READY);
  // Across frames, a machine that reports a steam level keeps the level
  // widget as its steam boiler; one that never did follows the temperature.
  {
    LaMarzoccoStatus machine;
    machine.identityGeneration = 1;
    LaMarzoccoPowerStateTracker tracker;
    LaMarzoccoObservationFence fence;
    const auto push = [&](const char *json) {
      LaMarzoccoObservation frame;
      frame.source = LaMarzoccoObservationSource::WEBSOCKET;
      frame.stamp = fence.stamp(1, tracker.generation());
      assert(decode(json, frame));
      assert(fence.merge(machine, tracker, frame, true));
    };
    push(R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"status":"HeatingUp","readyStartTime":1760000999000}}]})");
    assert(machine.steamBoiler == LaMarzoccoBoilerState::HEATING_UP);
    assert(machine.steamReadyAtUtcSec == 1760000999U);
    push(R"({"removedWidgets":["CMSteamBoilerTemperature"]})");
    assert(machine.steamBoiler == LaMarzoccoBoilerState::UNSUPPORTED);
    push(levelOnly);
    assert(machine.steamBoiler == LaMarzoccoBoilerState::READY);
    assert(machine.steamLevel == LaMarzoccoSteamLevel::LEVEL_2);
    push(R"({"widgets":[{"code":"CMSteamBoilerTemperature","output":{"status":"HeatingUp","readyStartTime":1760000999000}}]})");
    push(R"({"removedWidgets":["CMSteamBoilerTemperature"]})");
    assert(machine.steamBoiler == LaMarzoccoBoilerState::READY);
    assert(machine.steamLevel == LaMarzoccoSteamLevel::LEVEL_2);
    assert(machine.steamReadyAtUtcSec == 1760000820U);
  }

  // The brew range is the machine's own, read from the coffee boiler widget
  // and kept inside the firmware's absolute bounds.
  const auto coffee = [&](const char *output) {
    fresh();
    return decode((std::string(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},)") +
                   R"({"code":"CMCoffeeBoiler","output":{"status":"Ready",)" + output + "}}]}").c_str(),
                  update);
  };
  assert(coffee(R"("targetTemperature":102.5,"targetTemperatureMin":85,"targetTemperatureMax":104)"));
  assert(update.targetMinDeciC == 850 && update.targetMaxDeciC == 1040);
  assert(update.temperaturePresent && update.temperatureValid && update.targetDeciC == 1025);
  // A target outside the machine's range is no target; the frame still
  // carries the machine's power and boiler state.
  for (const char *target : {"79.9", "100.1"}) {
    assert(coffee((std::string(R"("targetTemperatureMin":80,"targetTemperatureMax":100,"targetTemperature":)") +
                   target).c_str()));
    assert(update.powerPresent && update.coffeeBoiler == LaMarzoccoBoilerState::READY);
    assert(update.temperaturePresent && !update.temperatureValid && update.targetDeciC == 0);
    assert(update.targetMinDeciC == 800 && update.targetMaxDeciC == 1000);
  }
  // Without a reported range only the absolute bounds apply.
  assert(coffee(R"("targetTemperature":104.0)"));
  assert(update.targetMinDeciC == 0 && update.temperatureValid && update.targetDeciC == 1040);
  for (const char *target : {"59.9", "110.1", "1e9"}) {
    assert(coffee((std::string(R"("targetTemperature":)") + target).c_str()));
    assert(update.powerPresent && update.temperaturePresent && !update.temperatureValid);
  }
  // A range that is malformed, inverted, or outside the absolute bounds
  // counts as not reported: it never widens what the firmware accepts.
  for (const char *range : {
      R"("targetTemperatureMin":"80","targetTemperatureMax":100)",
      R"("targetTemperatureMin":100,"targetTemperatureMax":80)",
      R"("targetTemperatureMin":93,"targetTemperatureMax":93)",
      R"("targetTemperatureMin":50,"targetTemperatureMax":100)",
      R"("targetTemperatureMin":80,"targetTemperatureMax":130)",
      R"("targetTemperatureMin":80)"}) {
    assert(coffee((std::string(range) + R"(,"targetTemperature":93.0)").c_str()));
    assert(update.targetMinDeciC == 0 && update.targetMaxDeciC == 0);
    assert(update.temperatureValid && update.targetDeciC == 930);
  }
  assert(coffee(R"("targetTemperatureMin":60,"targetTemperatureMax":110,"targetTemperature":110.0)"));
  assert(update.targetMinDeciC == 600 && update.targetMaxDeciC == 1100 && update.targetDeciC == 1100);
  // A target that is not a number is still a malformed frame.
  assert(!coffee(R"("targetTemperature":"93")"));
  // The status keeps the machine's range across frames that name none, and a
  // machine that never names one gets the default; steam alone sets nothing.
  {
    LaMarzoccoStatus machine;
    machine.identityGeneration = 1;
    LaMarzoccoPowerStateTracker tracker;
    LaMarzoccoObservationFence fence;
    const auto push = [&](const char *json) {
      LaMarzoccoObservation frame;
      frame.source = LaMarzoccoObservationSource::WEBSOCKET;
      frame.stamp = fence.stamp(1, tracker.generation());
      assert(decode(json, frame));
      assert(fence.merge(machine, tracker, frame, true));
    };
    push(R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready"}}]})");
    assert(machine.targetMinDeciC == 0 && machine.targetMaxDeciC == 0);
    push(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready","targetTemperature":93.0}}]})");
    assert(machine.targetMinDeciC == 800 && machine.targetMaxDeciC == 1000);
    push(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"targetTemperature":95.0,"targetTemperatureMin":85,"targetTemperatureMax":104}}]})");
    assert(machine.targetMinDeciC == 850 && machine.targetMaxDeciC == 1040);
    assert(machine.targetValid && machine.targetDeciC == 950);
    push(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"targetTemperature":96.0}}]})");
    assert(machine.targetMinDeciC == 850 && machine.targetMaxDeciC == 1040);
    assert(machine.targetDeciC == 960);
  }

  // The account listing keeps only coffee machines of the model the machine
  // profile names, and reports what it leaves out without its serial.
  {
    static std::string skippedModels;
    const auto list = [](const char *json, const char *profileModel,
                         LaMarzoccoDiscoverySnapshot &found) {
      skippedModels.clear();
      cJSON *root = cJSON_Parse(json);
      const bool listed = decodeLaMarzoccoThings(root, profileModel, found, [](const char *model) {
        skippedModels += model == nullptr ? "<serial>" : model;
        skippedModels += ';';
      });
      cJSON_Delete(root);
      return listed;
    };
    const char *account = R"([)"
        R"({"type":"CoffeeMachine","modelCode":"LINEAMICRA","serialNumber":"MR000001","name":"Kitchen"},)"
        R"({"type":"CoffeeMachine","modelCode":"LINEAMINIR","serialNumber":"MI000002","name":""},)"
        R"({"type":"CoffeeMachine","modelCode":"LINEAMINI","serialNumber":"LM000003"},)"
        R"({"type":"Grinder","modelCode":"PICOGRINDER","serialNumber":"PG000004"},)"
        R"({"type":"CoffeeMachine","serialNumber":"XX000005"},)"
        R"({"type":"CoffeeMachine","modelCode":7,"serialNumber":"XX000006"},)"
        R"("note"])";
    struct Case { const char *model, *serial, *name, *skipped; };
    for (const Case &expected : {
        Case{"LINEAMICRA", "MR000001", "Kitchen", "LINEAMINIR;LINEAMINI;unknown;unknown;"},
        Case{"LINEAMINIR", "MI000002", "MI000002", "LINEAMICRA;LINEAMINI;unknown;unknown;"},
        Case{"LINEAMINI", "LM000003", "LM000003", "LINEAMICRA;LINEAMINIR;unknown;unknown;"}}) {
      LaMarzoccoDiscoverySnapshot found;
      assert(list(account, expected.model, found));
      assert(found.count == 1);
      assert(std::strcmp(found.machines[0].serial, expected.serial) == 0);
      assert(std::strcmp(found.machines[0].name, expected.name) == 0);
      assert(skippedModels == expected.skipped);
    }
    // A matching machine whose serial cannot be stored is skipped, unnamed.
    LaMarzoccoDiscoverySnapshot found;
    assert(list(R"([{"type":"CoffeeMachine","modelCode":"LINEAMICRA","serialNumber":"bad serial!"},)"
                R"({"type":"CoffeeMachine","modelCode":"LINEAMICRA"}])", "LINEAMICRA", found));
    assert(found.count == 0 && skippedModels == "<serial>;<serial>;");
    assert(skippedModels.find("bad serial") == std::string::npos);
    // The listing stops at the machines it can hold and reports none beyond.
    std::string many = "[";
    for (int i = 0; i < 10; ++i)
      many += std::string(i ? "," : "") +
          R"({"type":"CoffeeMachine","modelCode":"LINEAMICRA","serialNumber":"MR00000)" +
          std::to_string(i) + R"("})";
    many += R"(,{"type":"CoffeeMachine","modelCode":"GS3AV","serialNumber":"GS000001"}])";
    found = {};
    assert(list(many.c_str(), "LINEAMICRA", found));
    assert(found.count == LA_MARZOCCO_MAX_ACCOUNT_MACHINES && skippedModels.empty());
    assert(std::strcmp(found.machines[7].serial, "MR000007") == 0);
    // Anything but the things array is a failed listing.
    found = {};
    assert(!list(R"({"message":"Unauthorized"})", "LINEAMICRA", found) && found.count == 0);
    assert(!list("", "LINEAMICRA", found));
  }

  // Boiler fields ride the temperature revision: partial pushes apply, and a
  // stale poll cannot clobber fresher pushed boiler states.
  LaMarzoccoStatus bstate;
  bstate.identityGeneration = 1;
  LaMarzoccoPowerStateTracker bpower;
  LaMarzoccoObservationFence bfence;
  LaMarzoccoObservation poll;
  poll.source = LaMarzoccoObservationSource::HTTP_INITIAL;
  poll.stamp = bfence.stamp(1, bpower.generation());
  poll.powerPresent = poll.temperaturePresent = poll.temperatureValid = true;
  poll.mode = LaMarzoccoObservedMode::BREWING;
  poll.targetDeciC = 930;
  poll.coffeeBoilerPresent = poll.steamBoilerPresent = true;
  poll.coffeeBoiler = poll.steamBoiler = LaMarzoccoBoilerState::HEATING_UP;
  poll.coffeeReadyAtUtcSec = 1760000460U;
  poll.steamReadyAtUtcSec = 1760000820U;
  poll.steamLevel = LaMarzoccoSteamLevel::LEVEL_3;
  poll.receivedAtMs = 100;
  assert(bfence.merge(bstate, bpower, poll, true, true));
  assert(bstate.coffeeBoiler == LaMarzoccoBoilerState::HEATING_UP);
  assert(bstate.steamLevel == LaMarzoccoSteamLevel::LEVEL_3);
  assert(bstate.temperatureAtMs == 100);
  LaMarzoccoObservation steamOnly;
  steamOnly.source = LaMarzoccoObservationSource::WEBSOCKET;
  steamOnly.stamp = bfence.stamp(1, bpower.generation());
  steamOnly.steamBoilerPresent = true;
  steamOnly.steamBoiler = LaMarzoccoBoilerState::READY;
  steamOnly.receivedAtMs = 150;
  assert(bfence.merge(bstate, bpower, steamOnly, true));
  assert(bstate.steamBoiler == LaMarzoccoBoilerState::READY);
  assert(bstate.coffeeBoiler == LaMarzoccoBoilerState::HEATING_UP);  // Untouched.
  assert(bstate.temperatureAtMs == 150);
  assert(bfence.merge(bstate, bpower, poll, true, true));  // Stale poll loses.
  assert(bstate.steamBoiler == LaMarzoccoBoilerState::READY);
  assert(bstate.coffeeBoiler == LaMarzoccoBoilerState::HEATING_UP);
  // A coffee widget carrying only a status (no targetTemperature) updates the
  // boiler state and keeps the last known target.
  update = {};
  update.source = LaMarzoccoObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready"}}]})", update));
  assert(update.coffeeBoilerPresent && update.coffeeBoiler == LaMarzoccoBoilerState::READY);
  assert(!update.temperaturePresent && !update.steamBoilerPresent);
  update.stamp = bfence.stamp(1, bpower.generation());
  update.receivedAtMs = 200;
  assert(bfence.merge(bstate, bpower, update, true));
  assert(bstate.coffeeBoiler == LaMarzoccoBoilerState::READY);
  assert(bstate.targetDeciC == 930 && bstate.targetValid);  // Retained target.
  assert(bstate.steamBoiler == LaMarzoccoBoilerState::READY);  // Untouched.
  // A steam-only NoWater report drives the rollup once power is ON.
  LaMarzoccoObservation drySteam;
  drySteam.source = LaMarzoccoObservationSource::WEBSOCKET;
  drySteam.stamp = bfence.stamp(1, bpower.generation());
  drySteam.steamBoilerPresent = true;
  drySteam.steamBoiler = LaMarzoccoBoilerState::NO_WATER;
  drySteam.receivedAtMs = 250;
  assert(bfence.merge(bstate, bpower, drySteam, true));
  bstate.powerState = LaMarzoccoPowerState::ON;
  bstate.assumedPower = laMarzoccoAssumedPower(bstate);  // As statusLocked() publishes it.
  assert(laMarzoccoReadiness(bstate) == LaMarzoccoReadiness::NEEDS_WATER);

  // Readiness rollup truth table over the effective status, resolved as
  // statusLocked() publishes it.
  const auto resolved = [](LaMarzoccoStatus s) {
    s.assumedPower = laMarzoccoAssumedPower(s);
    return s;
  };
  LaMarzoccoStatus rs;
  rs.powerState = LaMarzoccoPowerState::ON;
  rs.coffeeBoiler = rs.steamBoiler = LaMarzoccoBoilerState::HEATING_UP;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::WARMING_UP);
  rs.coffeeBoiler = LaMarzoccoBoilerState::READY;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::WAITING_FOR_STEAM);
  rs.steamBoiler = LaMarzoccoBoilerState::READY;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::READY);
  rs.steamBoiler = LaMarzoccoBoilerState::OFF;  // Steam off never blocks.
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::READY);
  rs.coffeeBoiler = LaMarzoccoBoilerState::NO_WATER;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::NEEDS_WATER);
  rs.powerState = LaMarzoccoPowerState::OFF;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::OFF);
  rs.optimisticOff = true;  // Standby accepted, cloud not yet confirming.
  rs.quality = LaMarzoccoObservationQuality::OPTIMISTIC;
  rs.powerState = LaMarzoccoPowerState::ON;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::OFF);
  rs.optimisticOff = false;
  rs.quality = LaMarzoccoObservationQuality::CURRENT;
  rs.powerState = LaMarzoccoPowerState::UNKNOWN;
  rs.coffeeBoiler = rs.steamBoiler = LaMarzoccoBoilerState::READY;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::UNKNOWN);
  rs.powerState = LaMarzoccoPowerState::OFF;
  rs.optimisticOn = true;  // Paddle wake: confirmed OFF, optimistic ON.
  rs.quality = LaMarzoccoObservationQuality::OPTIMISTIC;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::UNKNOWN);  // Retained boilers predate standby.
  rs.coffeeBoiler = rs.steamBoiler = LaMarzoccoBoilerState::UNKNOWN;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::UNKNOWN);
  rs.optimisticOn = false;  // Confirmed ON, no overlay.
  rs.quality = LaMarzoccoObservationQuality::CURRENT;
  rs.powerState = LaMarzoccoPowerState::ON;
  rs.coffeeBoiler = LaMarzoccoBoilerState::STANDBY;  // Contradictory: no claim.
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::UNKNOWN);
  rs.coffeeBoiler = LaMarzoccoBoilerState::ECO;  // Below temperature.
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::WARMING_UP);
  rs.coffeeBoiler = LaMarzoccoBoilerState::READY;
  rs.steamBoiler = LaMarzoccoBoilerState::ECO;
  assert(laMarzoccoReadiness(resolved(rs)) == LaMarzoccoReadiness::WAITING_FOR_STEAM);
  assert(std::strcmp(laMarzoccoReadinessName(LaMarzoccoReadiness::WAITING_FOR_STEAM), "waiting_for_steam") == 0);
  assert(std::strcmp(laMarzoccoBoilerStateName(LaMarzoccoBoilerState::HEATING_UP), "heating_up") == 0);
  assert(std::strcmp(laMarzoccoSteamLevelName(LaMarzoccoSteamLevel::LEVEL_3), "level_3") == 0);

  LaMarzoccoStatus state;
  state.identityGeneration = 1;
  state.sampleAtMs = 100;
  state.powerState = LaMarzoccoPowerState::ON;
  state.quality = LaMarzoccoObservationQuality::CURRENT;
  LaMarzoccoPowerStateTracker power;
  LaMarzoccoObservationFence fence;
  LaMarzoccoObservation old;
  old.stamp = fence.stamp(1, power.generation());
  old.powerPresent = old.temperaturePresent = old.temperatureValid = true;
  old.mode = LaMarzoccoObservedMode::BREWING;
  old.targetDeciC = 930;
  old.receivedAtMs = 200;
  LaMarzoccoObservation push = old;
  push.source = LaMarzoccoObservationSource::WEBSOCKET;
  push.temperaturePresent = false;
  push.mode = LaMarzoccoObservedMode::STANDBY;
  push.receivedAtMs = 150;
  assert(fence.merge(state, power, push, true));
  assert(fence.merge(state, power, old, true));
  assert(state.powerState == LaMarzoccoPowerState::OFF && state.sampleAtMs == 150);
  assert(state.targetDeciC == 930);  // Independent field revisions.

  assert(power.notePhysicalStart(state, true, true, 300));
  const auto intent = power.generation();
  assert(!power.notePhysicalStart(state, true, true, 301));
  assert(fence.merge(state, power, old, true));
  assert(power.effectiveStatus(state, true, 302).optimisticOn);
  push.stamp = fence.stamp(1, intent);
  push.receivedAtMs = 400;
  assert(fence.merge(state, power, push, true));  // Contradictory WS keeps optimism.
  assert(power.effectiveStatus(state, true, 401).optimisticOn);
  power.hold(state, true, 402);
  ++fence.epoch;
  assert(!fence.merge(state, power, push, true));
  assert(power.effectiveStatus(state, true, 100000).effectiveOn);
  assert(power.effectiveStatus(state, true, 100000).quality == LaMarzoccoObservationQuality::STALE);
  push.stamp = fence.stamp(1, intent);
  push.powerPresent = false;
  push.temperaturePresent = push.temperatureValid = true;
  assert(fence.merge(state, power, push, true) && retained(power));
  push.powerPresent = true;
  push.mode = LaMarzoccoObservedMode::BREWING;
  push.receivedAtMs = 100001;
  assert(fence.merge(state, power, push, true) && !retained(power));
  assert(power.effectiveStatus(state, true, 100002).effectiveOn);

  // Retention is one clock: a hold stands in for kStaleRetentionMs, then the
  // state is unknown; the paddle restarts it; nothing offline ever turns an
  // assumption into OFF; fresh evidence clears it at any time.
  {
    using la_marzocco_timing::kStaleRetentionMs;
    LaMarzoccoStatus on;
    on.powerState = LaMarzoccoPowerState::ON;
    on.quality = LaMarzoccoObservationQuality::CURRENT;
    on.sampleAtMs = 1000;
    LaMarzoccoPowerStateTracker held;
    held.hold(on, true, 2000);
    auto e = held.effectiveStatus(on, true, 2000 + kStaleRetentionMs - 1);
    assert(e.powerState == LaMarzoccoPowerState::ON && e.quality == LaMarzoccoObservationQuality::STALE);
    e = held.effectiveStatus(on, true, 2000 + kStaleRetentionMs);
    assert(e.powerState == LaMarzoccoPowerState::UNKNOWN && e.effectiveOn &&
           e.quality == LaMarzoccoObservationQuality::STALE && retained(held));
    assert(!held.notePhysicalStart(on, true, true, 2000 + kStaleRetentionMs));  // Unknown: normal shot...
    held.notePhysicalEvidence(on, true, 2000 + kStaleRetentionMs);  // ...and evidence of ON.
    e = held.effectiveStatus(on, true, 2000 + 2 * kStaleRetentionMs - 1);
    assert(e.powerState == LaMarzoccoPowerState::ON && !e.optimisticOn);
    assert(held.effectiveStatus(on, true, 2000 + 2 * kStaleRetentionMs).powerState ==
           LaMarzoccoPowerState::UNKNOWN);
    held.noteEvidence(held.generation(), LaMarzoccoPowerState::OFF, 3000 + 2 * kStaleRetentionMs, false);
    assert(!retained(held));  // Cloud evidence always wins.
    held.hold(on, true, 5000);
    held.reset();
    assert(!retained(held));

    LaMarzoccoStatus off = on;  // Held OFF: the wake gesture flips it to ON and restarts.
    off.powerState = LaMarzoccoPowerState::OFF;
    off.effectiveOn = false;
    LaMarzoccoPowerStateTracker fromOff;
    fromOff.hold(off, true, 10);
    assert(fromOff.effectiveStatus(off, true, 10 + kStaleRetentionMs - 1).powerState ==
           LaMarzoccoPowerState::OFF);
    assert(fromOff.notePhysicalStart(off, true, true, 30));
    fromOff.notePhysicalEvidence(off, true, 30);  // No-op over the live overlay.
    e = fromOff.effectiveStatus(off, true, 31);
    assert(e.powerState == LaMarzoccoPowerState::ON && !e.optimisticOn &&
           e.quality == LaMarzoccoObservationQuality::STALE);
    assert(fromOff.effectiveStatus(off, true, 30 + kStaleRetentionMs - 1).powerState ==
           LaMarzoccoPowerState::ON);
    assert(fromOff.effectiveStatus(off, true, 30 + kStaleRetentionMs).powerState ==
           LaMarzoccoPowerState::UNKNOWN);

    constexpr uint32_t kMinute = 60U * 1000U, kWrap = 0xFFFFFF00U;
    LaMarzoccoPowerStateTracker restart;  // A press before expiry restarts the clock...
    restart.hold(on, true, kWrap);        // ...across the millis() wrap.
    restart.notePhysicalEvidence(on, true, kWrap + 10 * kMinute);
    assert(restart.effectiveStatus(on, true, kWrap + 25 * kMinute - 1).powerState ==
           LaMarzoccoPowerState::ON);
    assert(restart.effectiveStatus(on, true, kWrap + 25 * kMinute).powerState ==
           LaMarzoccoPowerState::UNKNOWN);
    LaMarzoccoPowerStateTracker inflight;  // Our own standby outranks the paddle while live...
    assert(inflight.noteStandbyCommandAccepted(on, true, 1100));
    inflight.hold(on, true, 1101);
    inflight.notePhysicalEvidence(on, true, 1102);
    assert(inflight.effectiveStatus(on, true, 1103).powerState == LaMarzoccoPowerState::OFF);
    inflight.notePhysicalEvidence(on, true, 1100 + la_marzocco_timing::kOptimisticOverlayMs);  // ...then counts.
    assert(inflight.effectiveStatus(on, true, 1101 + la_marzocco_timing::kOptimisticOverlayMs).powerState ==
           LaMarzoccoPowerState::ON);

    LaMarzoccoPowerStateTracker fresh;  // A fresh reading needs no evidence...
    fresh.notePhysicalEvidence(on, true, 5);
    assert(!retained(fresh));
    fresh.notePhysicalEvidence(on, false, 5);  // ...nor does a machine that is not observed.
    assert(!retained(fresh));
    LaMarzoccoStatus none;  // Boot before the first read: the paddle says ON until the read lands.
    LaMarzoccoPowerStateTracker boot;
    boot.notePhysicalEvidence(none, true, 5);
    assert(boot.effectiveStatus(none, true, 6).powerState == LaMarzoccoPowerState::ON);
    boot.noteEvidence(boot.generation(), LaMarzoccoPowerState::OFF, 7, false);
    assert(!retained(boot));
    LaMarzoccoStatus eco = on;  // Eco: the machine reports no power; the paddle cannot invent one.
    eco.powerState = LaMarzoccoPowerState::UNKNOWN;
    eco.quality = LaMarzoccoObservationQuality::UNSUPPORTED;
    LaMarzoccoPowerStateTracker unsupported;
    unsupported.hold(eco, true, 5);
    unsupported.notePhysicalEvidence(eco, true, 6);
    assert(unsupported.effectiveStatus(eco, true, 7).powerState == LaMarzoccoPowerState::UNKNOWN);
  }

  // Same production reducer, concurrent facade-style publication/snapshots.
  std::mutex mutex;
  std::thread receiver([&] {
    for (unsigned i = 0; i < 1000; ++i) {
      std::lock_guard<std::mutex> lock(mutex);
      push.stamp = fence.stamp(1, power.generation());
      push.receivedAtMs = 110000 + i;
      assert(fence.merge(state, power, push, true));
    }
  });
  for (unsigned i = 0; i < 1000; ++i) {
    std::lock_guard<std::mutex> lock(mutex);
    assert(power.effectiveStatus(state, true, 112000).effectiveOn);
  }
  receiver.join();

  // Discovery keeps an over-long UTF-8 machine name, cut at a character edge.
  std::string name;
  for (unsigned i = 0; i < 30; ++i) name += "\xc3\xa9";  // 60 bytes of "é".
  char machine[LA_MARZOCCO_NAME_CAPACITY];
  copyLaMarzoccoText(machine, sizeof(machine), name.c_str());
  assert(std::strlen(machine) == (LA_MARZOCCO_NAME_CAPACITY - 1) / 2 * 2);
  assert(std::strncmp(machine, name.c_str(), std::strlen(machine)) == 0);
}

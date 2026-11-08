#include "machine/OpenBrewByWeightMicraObservation.h"
#include <cJSON.h>
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

using namespace openbrewbyweight;

static bool decode(const char *json, MicraObservation &update) {
  cJSON *root = cJSON_Parse(json);
  const bool valid = decodeMicraDashboard(root, update);
  cJSON_Delete(root);
  return valid;
}

static void initialStateContinuity() {
  for (bool pushFirst : {false, true}) {
    LineaMicraStatus state;
    state.identityGeneration = 1;
    state.connectionFreshness = true;
    LineaMicraPowerStateTracker power;
    MicraObservationFence fence;
    MicraObservation initial;
    initial.stamp = fence.stamp(1, power.generation());
    initial.source = MicraObservationSource::HTTP_INITIAL;
    initial.powerPresent = initial.temperaturePresent = initial.temperatureValid = true;
    initial.mode = LineaMicraObservedMode::STANDBY;
    initial.targetDeciC = 930;
    initial.receivedAtMs = 100;
    MicraObservation push = initial;
    push.source = MicraObservationSource::WEBSOCKET;
    push.temperaturePresent = false;
    push.mode = LineaMicraObservedMode::BREWING;
    push.receivedAtMs = 101;
    if (pushFirst) assert(fence.merge(state, power, push, true));
    assert(fence.merge(state, power, initial, true, true));
    assert(state.targetDeciC == 930);
    assert(state.powerSource == (pushFirst ? MicraObservationSource::WEBSOCKET :
                                            MicraObservationSource::HTTP_INITIAL));
    assert(state.powerState == (pushFirst ? LineaMicraPowerState::ON : LineaMicraPowerState::OFF));
    assert(fence.synchronized);
    for (uint32_t now : {30100U, 60100U, 600100U, 3600100U})
      assert(power.effectiveStatus(state, true, now).quality == LineaMicraObservationQuality::CURRENT);
    assert(fence.merge(state, power, push, true));
    assert(state.powerSource == MicraObservationSource::WEBSOCKET);
    auto staleOffline = initial;
    staleOffline.connectedPresent = true;
    staleOffline.connected = false;
    assert(fence.merge(state, power, staleOffline, true, true));
    assert(fence.synchronized && !fence.offline && !power.retained());
    initial.stamp = fence.stamp(1, power.generation());
    initial.source = MicraObservationSource::HTTP;
    initial.receivedAtMs = 200;
    assert(fence.merge(state, power, initial, true, true));  // Later explicit refresh.
    assert(state.powerSource == MicraObservationSource::HTTP && state.sampleAtMs == 200);

    MicraObservation offline;
    offline.source = MicraObservationSource::WEBSOCKET;
    offline.stamp = fence.stamp(1, power.generation());
    offline.connectedPresent = true;
    offline.receivedAtMs = 300;
    auto late = initial;
    late.stamp = offline.stamp;
    assert(fence.merge(state, power, offline, true));
    assert(fence.offline && !fence.synchronized && power.retained());
    late.connectedPresent = late.connected = true;
    assert(fence.merge(state, power, late, true, true));
    assert(fence.offline && power.retained() && state.sampleAtMs == 200);
    offline.connected = true;
    assert(fence.merge(state, power, offline, true));
    assert(!fence.offline && !fence.synchronized && power.retained());
    push.powerPresent = false;
    assert(fence.merge(state, power, push, true));
    assert(power.effectiveStatus(state, true, 400).quality == LineaMicraObservationQuality::STALE);
    push.powerPresent = true;
    push.receivedAtMs = 500;
    assert(fence.merge(state, power, push, true));
    assert(fence.synchronized && !power.retained());
    late.connected = false;  // An older HTTP offline result cannot undo recovery.
    assert(fence.merge(state, power, late, true, true));
    assert(fence.synchronized && !fence.offline && state.sampleAtMs == 500);
    ++fence.epoch;
    assert(!fence.merge(state, power, late, true, true));

    state.connectionFreshness = false;
    assert(power.effectiveStatus(state, true, 30500).quality == LineaMicraObservationQuality::STALE);
  }
}

static void connectionOnlyOrdering() {
  LineaMicraStatus state;
  LineaMicraPowerStateTracker power;
  MicraObservationFence fence;
  MicraObservation initial;
  initial.source = MicraObservationSource::HTTP_INITIAL;
  initial.stamp = fence.stamp(0, power.generation());
  initial.connectedPresent = true;
  initial.receivedAtMs = 100;
  MicraObservation online = initial;
  online.source = MicraObservationSource::WEBSOCKET;
  online.connected = true;
  assert(fence.merge(state, power, online, true));
  assert(fence.merge(state, power, initial, true));
  assert(!fence.offline && !power.retained());  // Older API offline cannot win.
  // Online-only push must not prevent independent power initialization.
  initial.connected = initial.powerPresent = true;
  initial.mode = LineaMicraObservedMode::STANDBY;
  assert(fence.merge(state, power, initial, true));
  assert(state.powerState == LineaMicraPowerState::OFF && fence.synchronized);
  assert(state.powerSource == MicraObservationSource::HTTP_INITIAL);
  initial.stamp = fence.stamp(0, power.generation());
  initial.connected = false;
  assert(fence.merge(state, power, initial, true));
  assert(fence.offline && !fence.synchronized && power.retained());
}

int main() {
  initialStateContinuity();
  connectionOnlyOrdering();
  // Synthetic dashboard fixtures; no cloud or hardware is contacted.
  MicraObservation update;
  assert(decode(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"StandBy","status":"Brewing"}},{"code":"CMCoffeeBoiler","output":{"targetTemperature":93.5}}]})", update));
  assert(update.mode == LineaMicraObservedMode::STANDBY);
  assert(update.targetDeciC == 935 && update.temperatureValid);
  update = {};
  assert(!decode(R"({"widgets":[{"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},{"code":"CMCoffeeBoiler","output":{"targetTemperature":"93"}}]})", update));
  assert(!update.powerPresent);  // No partial publication from invalid data.
  assert(!decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"targetTemperature":100.1}}]})", update));
  assert(decode(R"({"connected":false,"widgets":[{"code":"FutureWidget"}]})", update));
  // Malformed widgets the decoder does not use are skipped, not fatal.
  {
    MicraObservation tolerant;
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
  update.source = MicraObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[{"code":"CMBackFlush","output":{"status":"123456789012345678901234567890éé"}}]})", update));
  assert(std::strcmp(update.cleaningLabel, "123456789012345678901234567890é") == 0);
  assert(update.cleaning == MicraCleaningState::UNKNOWN);

  for (const char *label : {"Requested", "Cleaning", "Off", "Future"}) {
    char json[120];
    std::snprintf(json, sizeof(json), "{\"widgets\":[{\"code\":\"CMBackFlush\",\"output\":{\"status\":\"%s\"}}]}", label);
    update = {};
    assert(decode(json, update) && !update.cleaningPresent);
    update.source = MicraObservationSource::WEBSOCKET;
    assert(decode(json, update) && update.cleaningPresent && update.cleaningAvailable);
    assert(std::strcmp(update.cleaningLabel, label) == 0);
  }
  update = {};
  update.source = MicraObservationSource::WEBSOCKET;
  assert(decode(R"({"removedWidgets":["CMBackFlush","CMMachineStatus","CMCoffeeBoiler"]})", update));
  assert(update.cleaningPresent && !update.cleaningAvailable);
  assert(update.powerPresent && update.mode == LineaMicraObservedMode::NONE);
  assert(update.temperaturePresent && !update.temperatureValid);
  update = {};
  assert(decode(R"({"removedWidgets":[{"code":"CMMachineStatus"},{"code":"CMCoffeeBoiler"}]})", update));
  assert(update.powerPresent && update.mode == LineaMicraObservedMode::NONE);
  assert(update.temperaturePresent && !update.temperatureValid);

  // Boiler readiness widgets: status, estimate, and steam level.
  update = {};
  update.source = MicraObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[)"
      R"({"code":"CMMachineStatus","output":{"mode":"BrewingMode"}},)"
      R"({"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","targetTemperature":93.0,"readyStartTime":1760000460000}},)"
      R"({"code":"CMSteamBoilerLevel","output":{"status":"HeatingUp","targetLevel":"Level3","readyStartTime":1760000820500}}]})", update));
  assert(update.powerPresent && update.mode == LineaMicraObservedMode::BREWING);
  assert(update.temperaturePresent && update.temperatureValid && update.targetDeciC == 930);
  assert(update.coffeeBoilerPresent && update.coffeeBoiler == LineaMicraBoilerState::HEATING_UP);
  assert(update.coffeeReadyAtUtcSec == 1760000460U);
  assert(update.steamBoilerPresent && update.steamBoiler == LineaMicraBoilerState::HEATING_UP);
  assert(update.steamLevel == LineaMicraSteamLevel::LEVEL_3);
  assert(update.steamReadyAtUtcSec == 1760000820U);
  // Unknown status strings degrade to UNSUPPORTED instead of failing.
  update = {};
  update.source = MicraObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"FutureMode","targetLevel":"Level4"}}]})", update));
  assert(update.steamBoiler == LineaMicraBoilerState::UNSUPPORTED);
  assert(update.steamLevel == LineaMicraSteamLevel::UNSUPPORTED);
  // Null estimate means no estimate; malformed values degrade to absent
  // instead of failing the frame (display-only field), and both epoch
  // milliseconds and seconds-scale values are interpreted.
  const auto fresh = [&update] {
    update = {};
    update.source = MicraObservationSource::WEBSOCKET;
  };
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready","targetTemperature":94,"readyStartTime":null}}]})", update));
  assert(update.coffeeBoiler == LineaMicraBoilerState::READY && update.coffeeReadyAtUtcSec == 0);
  fresh();
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"HeatingUp","readyStartTime":"soon"}}]})", update));
  assert(update.coffeeBoiler == LineaMicraBoilerState::HEATING_UP && update.coffeeReadyAtUtcSec == 0);
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
  assert(!update.steamBoilerPresent && update.steamLevel == LineaMicraSteamLevel::LEVEL_2);
  fresh();
  assert(!decode(R"({"widgets":[{"code":"CMSteamBoilerLevel","output":{"status":"Ready","status":"Ready"}}]})", update));
  assert(!decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready"}},{"code":"CMCoffeeBoiler","output":{"status":"Ready"}}]})", update));
  update = {};
  update.source = MicraObservationSource::WEBSOCKET;
  assert(decode(R"({"removedWidgets":["CMSteamBoilerLevel"]})", update));
  assert(update.steamBoilerPresent && update.steamBoiler == LineaMicraBoilerState::UNSUPPORTED);
  assert(update.steamLevel == LineaMicraSteamLevel::UNKNOWN && update.steamReadyAtUtcSec == 0);

  // Boiler fields ride the temperature revision: partial pushes apply, and a
  // stale poll cannot clobber fresher pushed boiler states.
  LineaMicraStatus bstate;
  bstate.identityGeneration = 1;
  LineaMicraPowerStateTracker bpower;
  MicraObservationFence bfence;
  MicraObservation poll;
  poll.source = MicraObservationSource::HTTP_INITIAL;
  poll.stamp = bfence.stamp(1, bpower.generation());
  poll.powerPresent = poll.temperaturePresent = poll.temperatureValid = true;
  poll.mode = LineaMicraObservedMode::BREWING;
  poll.targetDeciC = 930;
  poll.coffeeBoilerPresent = poll.steamBoilerPresent = true;
  poll.coffeeBoiler = poll.steamBoiler = LineaMicraBoilerState::HEATING_UP;
  poll.coffeeReadyAtUtcSec = 1760000460U;
  poll.steamReadyAtUtcSec = 1760000820U;
  poll.steamLevel = LineaMicraSteamLevel::LEVEL_3;
  poll.receivedAtMs = 100;
  assert(bfence.merge(bstate, bpower, poll, true, true));
  assert(bstate.coffeeBoiler == LineaMicraBoilerState::HEATING_UP);
  assert(bstate.steamLevel == LineaMicraSteamLevel::LEVEL_3);
  assert(bstate.temperatureAtMs == 100);
  MicraObservation steamOnly;
  steamOnly.source = MicraObservationSource::WEBSOCKET;
  steamOnly.stamp = bfence.stamp(1, bpower.generation());
  steamOnly.steamBoilerPresent = true;
  steamOnly.steamBoiler = LineaMicraBoilerState::READY;
  steamOnly.receivedAtMs = 150;
  assert(bfence.merge(bstate, bpower, steamOnly, true));
  assert(bstate.steamBoiler == LineaMicraBoilerState::READY);
  assert(bstate.coffeeBoiler == LineaMicraBoilerState::HEATING_UP);  // Untouched.
  assert(bstate.temperatureAtMs == 150);
  assert(bfence.merge(bstate, bpower, poll, true, true));  // Stale poll loses.
  assert(bstate.steamBoiler == LineaMicraBoilerState::READY);
  assert(bstate.coffeeBoiler == LineaMicraBoilerState::HEATING_UP);
  // A coffee widget carrying only a status (no targetTemperature) updates the
  // boiler state and keeps the last known target.
  update = {};
  update.source = MicraObservationSource::WEBSOCKET;
  assert(decode(R"({"widgets":[{"code":"CMCoffeeBoiler","output":{"status":"Ready"}}]})", update));
  assert(update.coffeeBoilerPresent && update.coffeeBoiler == LineaMicraBoilerState::READY);
  assert(!update.temperaturePresent && !update.steamBoilerPresent);
  update.stamp = bfence.stamp(1, bpower.generation());
  update.receivedAtMs = 200;
  assert(bfence.merge(bstate, bpower, update, true));
  assert(bstate.coffeeBoiler == LineaMicraBoilerState::READY);
  assert(bstate.targetDeciC == 930 && bstate.targetValid);  // Retained target.
  assert(bstate.steamBoiler == LineaMicraBoilerState::READY);  // Untouched.
  // A steam-only NoWater report drives the rollup once power is ON.
  MicraObservation drySteam;
  drySteam.source = MicraObservationSource::WEBSOCKET;
  drySteam.stamp = bfence.stamp(1, bpower.generation());
  drySteam.steamBoilerPresent = true;
  drySteam.steamBoiler = LineaMicraBoilerState::NO_WATER;
  drySteam.receivedAtMs = 250;
  assert(bfence.merge(bstate, bpower, drySteam, true));
  bstate.powerState = LineaMicraPowerState::ON;
  bstate.assumedPower = lineaMicraAssumedPower(bstate);  // As statusLocked() publishes it.
  assert(lineaMicraReadiness(bstate) == LineaMicraReadiness::NEEDS_WATER);

  // Readiness rollup truth table over the effective status, resolved as
  // statusLocked() publishes it.
  const auto resolved = [](LineaMicraStatus s) {
    s.assumedPower = lineaMicraAssumedPower(s);
    return s;
  };
  LineaMicraStatus rs;
  rs.powerState = LineaMicraPowerState::ON;
  rs.coffeeBoiler = rs.steamBoiler = LineaMicraBoilerState::HEATING_UP;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::WARMING_UP);
  rs.coffeeBoiler = LineaMicraBoilerState::READY;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::WAITING_FOR_STEAM);
  rs.steamBoiler = LineaMicraBoilerState::READY;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::READY);
  rs.steamBoiler = LineaMicraBoilerState::OFF;  // Steam off never blocks.
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::READY);
  rs.coffeeBoiler = LineaMicraBoilerState::NO_WATER;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::NEEDS_WATER);
  rs.powerState = LineaMicraPowerState::OFF;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::OFF);
  rs.optimisticOff = true;  // Standby accepted, cloud not yet confirming.
  rs.quality = LineaMicraObservationQuality::OPTIMISTIC;
  rs.powerState = LineaMicraPowerState::ON;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::OFF);
  rs.optimisticOff = false;
  rs.quality = LineaMicraObservationQuality::CURRENT;
  rs.powerState = LineaMicraPowerState::UNKNOWN;
  rs.coffeeBoiler = rs.steamBoiler = LineaMicraBoilerState::READY;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::UNKNOWN);
  rs.powerState = LineaMicraPowerState::OFF;
  rs.optimisticOn = true;  // Paddle wake: confirmed OFF, optimistic ON.
  rs.quality = LineaMicraObservationQuality::OPTIMISTIC;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::UNKNOWN);  // Retained boilers predate standby.
  rs.coffeeBoiler = rs.steamBoiler = LineaMicraBoilerState::UNKNOWN;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::UNKNOWN);
  rs.optimisticOn = false;  // Confirmed ON, no overlay.
  rs.quality = LineaMicraObservationQuality::CURRENT;
  rs.powerState = LineaMicraPowerState::ON;
  rs.coffeeBoiler = LineaMicraBoilerState::STANDBY;  // Contradictory: no claim.
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::UNKNOWN);
  rs.coffeeBoiler = LineaMicraBoilerState::ECO;  // Below temperature.
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::WARMING_UP);
  rs.coffeeBoiler = LineaMicraBoilerState::READY;
  rs.steamBoiler = LineaMicraBoilerState::ECO;
  assert(lineaMicraReadiness(resolved(rs)) == LineaMicraReadiness::WAITING_FOR_STEAM);
  assert(std::strcmp(lineaMicraReadinessName(LineaMicraReadiness::WAITING_FOR_STEAM), "waiting_for_steam") == 0);
  assert(std::strcmp(lineaMicraBoilerStateName(LineaMicraBoilerState::HEATING_UP), "heating_up") == 0);
  assert(std::strcmp(lineaMicraSteamLevelName(LineaMicraSteamLevel::LEVEL_3), "level_3") == 0);

  LineaMicraStatus state;
  state.identityGeneration = 1;
  state.sampleAtMs = 100;
  state.powerState = LineaMicraPowerState::ON;
  state.quality = LineaMicraObservationQuality::CURRENT;
  LineaMicraPowerStateTracker power;
  MicraObservationFence fence;
  MicraObservation old;
  old.stamp = fence.stamp(1, power.generation());
  old.powerPresent = old.temperaturePresent = old.temperatureValid = true;
  old.mode = LineaMicraObservedMode::BREWING;
  old.targetDeciC = 930;
  old.receivedAtMs = 200;
  MicraObservation push = old;
  push.source = MicraObservationSource::WEBSOCKET;
  push.temperaturePresent = false;
  push.mode = LineaMicraObservedMode::STANDBY;
  push.receivedAtMs = 150;
  assert(fence.merge(state, power, push, true));
  assert(fence.merge(state, power, old, true));
  assert(state.powerState == LineaMicraPowerState::OFF && state.sampleAtMs == 150);
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
  assert(power.effectiveStatus(state, true, 100000).quality == LineaMicraObservationQuality::STALE);
  push.stamp = fence.stamp(1, intent);
  push.powerPresent = false;
  push.temperaturePresent = push.temperatureValid = true;
  assert(fence.merge(state, power, push, true) && power.retained());
  push.powerPresent = true;
  push.mode = LineaMicraObservedMode::BREWING;
  push.receivedAtMs = 100001;
  assert(fence.merge(state, power, push, true) && !power.retained());
  assert(power.effectiveStatus(state, true, 100002).effectiveOn);

  // Retention is one clock: a hold stands in for kStaleRetentionMs, then the
  // state is unknown; the paddle restarts it; nothing offline ever turns an
  // assumption into OFF; fresh evidence clears it at any time.
  {
    using micra_timing::kStaleRetentionMs;
    LineaMicraStatus on;
    on.powerState = LineaMicraPowerState::ON;
    on.quality = LineaMicraObservationQuality::CURRENT;
    on.sampleAtMs = 1000;
    LineaMicraPowerStateTracker held;
    held.hold(on, true, 2000);
    auto e = held.effectiveStatus(on, true, 2000 + kStaleRetentionMs - 1);
    assert(e.powerState == LineaMicraPowerState::ON && e.quality == LineaMicraObservationQuality::STALE);
    e = held.effectiveStatus(on, true, 2000 + kStaleRetentionMs);
    assert(e.powerState == LineaMicraPowerState::UNKNOWN && e.effectiveOn &&
           e.quality == LineaMicraObservationQuality::STALE && held.retained());
    assert(!held.notePhysicalStart(on, true, true, 2000 + kStaleRetentionMs));  // Unknown: normal shot...
    held.notePhysicalEvidence(on, true, 2000 + kStaleRetentionMs);  // ...and evidence of ON.
    e = held.effectiveStatus(on, true, 2000 + 2 * kStaleRetentionMs - 1);
    assert(e.powerState == LineaMicraPowerState::ON && !e.optimisticOn);
    assert(held.effectiveStatus(on, true, 2000 + 2 * kStaleRetentionMs).powerState ==
           LineaMicraPowerState::UNKNOWN);
    held.noteEvidence(held.generation(), LineaMicraPowerState::OFF, 3000 + 2 * kStaleRetentionMs, false);
    assert(!held.retained());  // Cloud evidence always wins.
    held.hold(on, true, 5000);
    held.reset();
    assert(!held.retained());

    LineaMicraStatus off = on;  // Held OFF: the wake gesture flips it to ON and restarts.
    off.powerState = LineaMicraPowerState::OFF;
    off.effectiveOn = false;
    LineaMicraPowerStateTracker fromOff;
    fromOff.hold(off, true, 10);
    assert(fromOff.effectiveStatus(off, true, 10 + kStaleRetentionMs - 1).powerState ==
           LineaMicraPowerState::OFF);
    assert(fromOff.notePhysicalStart(off, true, true, 30));
    fromOff.notePhysicalEvidence(off, true, 30);  // No-op over the live overlay.
    e = fromOff.effectiveStatus(off, true, 31);
    assert(e.powerState == LineaMicraPowerState::ON && !e.optimisticOn &&
           e.quality == LineaMicraObservationQuality::STALE);
    assert(fromOff.effectiveStatus(off, true, 30 + kStaleRetentionMs - 1).powerState ==
           LineaMicraPowerState::ON);
    assert(fromOff.effectiveStatus(off, true, 30 + kStaleRetentionMs).powerState ==
           LineaMicraPowerState::UNKNOWN);

    constexpr uint32_t kMinute = 60U * 1000U, kWrap = 0xFFFFFF00U;
    LineaMicraPowerStateTracker restart;  // A press before expiry restarts the clock...
    restart.hold(on, true, kWrap);        // ...across the millis() wrap.
    restart.notePhysicalEvidence(on, true, kWrap + 10 * kMinute);
    assert(restart.effectiveStatus(on, true, kWrap + 25 * kMinute - 1).powerState ==
           LineaMicraPowerState::ON);
    assert(restart.effectiveStatus(on, true, kWrap + 25 * kMinute).powerState ==
           LineaMicraPowerState::UNKNOWN);
    LineaMicraPowerStateTracker inflight;  // Our own standby outranks the paddle while live...
    assert(inflight.noteStandbyCommandAccepted(on, true, 1100));
    inflight.hold(on, true, 1101);
    inflight.notePhysicalEvidence(on, true, 1102);
    assert(inflight.effectiveStatus(on, true, 1103).powerState == LineaMicraPowerState::OFF);
    inflight.notePhysicalEvidence(on, true, 1100 + micra_timing::kOptimisticOverlayMs);  // ...then counts.
    assert(inflight.effectiveStatus(on, true, 1101 + micra_timing::kOptimisticOverlayMs).powerState ==
           LineaMicraPowerState::ON);

    LineaMicraPowerStateTracker fresh;  // A fresh reading needs no evidence...
    fresh.notePhysicalEvidence(on, true, 5);
    assert(!fresh.retained());
    fresh.notePhysicalEvidence(on, false, 5);  // ...nor does a machine that is not observed.
    assert(!fresh.retained());
    LineaMicraStatus none;  // Boot before the first read: the paddle says ON until the read lands.
    LineaMicraPowerStateTracker boot;
    boot.notePhysicalEvidence(none, true, 5);
    assert(boot.effectiveStatus(none, true, 6).powerState == LineaMicraPowerState::ON);
    boot.noteEvidence(boot.generation(), LineaMicraPowerState::OFF, 7, false);
    assert(!boot.retained());
    LineaMicraStatus eco = on;  // Eco: the machine reports no power; the paddle cannot invent one.
    eco.powerState = LineaMicraPowerState::UNKNOWN;
    eco.quality = LineaMicraObservationQuality::UNSUPPORTED;
    LineaMicraPowerStateTracker unsupported;
    unsupported.hold(eco, true, 5);
    unsupported.notePhysicalEvidence(eco, true, 6);
    assert(unsupported.effectiveStatus(eco, true, 7).powerState == LineaMicraPowerState::UNKNOWN);
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
  char machine[LINEA_MICRA_NAME_CAPACITY];
  copyMicraText(machine, sizeof(machine), name.c_str());
  assert(std::strlen(machine) == (LINEA_MICRA_NAME_CAPACITY - 1) / 2 * 2);
  assert(std::strncmp(machine, name.c_str(), std::strlen(machine)) == 0);
}

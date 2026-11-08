#include "../OpenBrewByWeightPowerManagement.h"
#include <cassert>
#include <thread>

using namespace openbrewbyweight;

int main() {
  PowerPolicy policy;
  PowerInputs in;
  assert(policy.update(0, in) == PowerProfile::OFF);
  in.idleSaving = in.performanceBoost = true;
  assert(policy.update(1, in) == PowerProfile::SETTLING);
  assert(policy.update(1001, in) == PowerProfile::IDLE);
  in.machineBusy = true;
  in.physicalEdge = true;
  assert(policy.update(2000, in) == PowerProfile::MANUAL);
  in.physicalEdge = false;
  // A timer started at the initial press must not lower an ongoing operation.
  assert(policy.update(602000, in) == PowerProfile::MANUAL);
  in.machineBusy = false;
  assert(policy.update(602001, in) == PowerProfile::COOLDOWN);
  assert(policy.cooldownRemaining(602001) == 300000);
  in.webActive = true;
  assert(policy.update(603000, in) == PowerProfile::WEB);
  in.webActive = false;
  assert(policy.update(783000, in) == PowerProfile::COOLDOWN);
  in.physicalEdge = true;
  assert(policy.update(902000, in) == PowerProfile::COOLDOWN);
  in.physicalEdge = false;
  assert(policy.update(1201999, in) == PowerProfile::COOLDOWN);
  assert(policy.update(1202000, in) == PowerProfile::SETTLING);
  assert(policy.update(1203000, in) == PowerProfile::IDLE);
  in.scaleBusy = true;
  assert(policy.update(1203001, in) == PowerProfile::WAITING);
  assert(powerMinMhz(PowerProfile::WAITING, true) == 160);
  assert(powerMaxMhz(PowerProfile::WAITING, true) == 160);
  in.machineBusy = true;
  assert(policy.update(1203002, in) == PowerProfile::WORKING);
  in.scaleBusy = false;
  in.webActive = true;
  assert(policy.update(1600000, in) == PowerProfile::WORKING);
  assert(policy.update(1609999, in) == PowerProfile::WORKING);
  in.scaleBusy = true;
  assert(policy.update(1610000, in) == PowerProfile::WORKING);
  in.scaleBusy = false;
  assert(policy.update(1610001, in) == PowerProfile::WORKING);
  assert(policy.update(1640000, in) == PowerProfile::WORKING);
  assert(policy.update(1640001, in) == PowerProfile::MANUAL);
  in.scaleBusy = true;
  assert(policy.update(1640002, in) == PowerProfile::WORKING);
  in.scaleBusy = false;
  assert(policy.update(1640003, in) == PowerProfile::WORKING);
  in.machineBusy = false;
  in.webActive = false;
  assert(policy.update(1640004, in) == PowerProfile::WAITING);
  assert(policy.update(1670003, in) == PowerProfile::COOLDOWN);
  in.idleSaving = in.performanceBoost = false;
  in.scaleBusy = true;
  assert(policy.update(1670004, in) == PowerProfile::OFF);

  // Wraparound, connection flaps and maintenance hold must not admit idle.
  policy = PowerPolicy{};
  in = PowerInputs{};
  in.idleSaving = in.performanceBoost = true;
  in.physicalEdge = true;
  assert(policy.update(UINT32_MAX - 1000, in) == PowerProfile::COOLDOWN);
  in.physicalEdge = false;
  assert(policy.update(1000, in) == PowerProfile::COOLDOWN);
  assert(policy.cooldownRemaining(1000) == 297999);
  in.maintenance = true;
  assert(policy.update(400000, in) == PowerProfile::MAINTENANCE);
  in.maintenance = false;
  assert(policy.update(400001, in) == PowerProfile::SETTLING);
  in.scaleBusy = true;
  assert(policy.update(400500, in) == PowerProfile::WAITING);
  in.scaleBusy = false;
  assert(policy.update(401001, in) == PowerProfile::WAITING);
  assert(policy.update(431000, in) == PowerProfile::WAITING);
  assert(policy.update(431001, in) == PowerProfile::SETTLING);
  assert(policy.update(432001, in) == PowerProfile::IDLE);
  policy = PowerPolicy{};
  in.scaleBusy = true;
  assert(policy.update(UINT32_MAX - 1001, in) == PowerProfile::WAITING);
  in.scaleBusy = false;
  assert(policy.update(UINT32_MAX - 1000, in) == PowerProfile::WAITING);
  assert(policy.update(28998, in) == PowerProfile::WAITING);
  assert(policy.update(28999, in) == PowerProfile::SETTLING);

  // The two settings are independent: boost owns the scale window, idle
  // saving owns idle. Both off is the fixed 80-MHz baseline.
  assert(powerMinMhz(PowerProfile::IDLE, false) == 40);
  assert(powerMaxMhz(PowerProfile::IDLE, false) == 80);
  assert(powerMinMhz(PowerProfile::WAITING, false) == 80);
  assert(powerMaxMhz(PowerProfile::WORKING, false) == 80);
  assert(powerMaxMhz(PowerProfile::MACHINE_ON, true) == 80);
  policy = PowerPolicy{};
  in = PowerInputs{};
  in.idleSaving = true;  // Idle saving alone: 40 MHz idle, 80 MHz scale window.
  assert(policy.update(0, in) == PowerProfile::SETTLING);
  assert(policy.update(1000, in) == PowerProfile::IDLE);
  in.scaleBusy = true;
  assert(policy.update(1001, in) == PowerProfile::WAITING);
  in.scaleBusy = false;
  assert(policy.update(1002, in) == PowerProfile::WAITING);  // 30-s grace.
  in.idleSaving = false;
  in.performanceBoost = true;  // Boost alone: idle holds the 80-MHz baseline.
  assert(policy.update(31002, in) == PowerProfile::OFF);
  assert(policy.update(40000, in) == PowerProfile::OFF);
  in.scaleBusy = true;
  assert(policy.update(40001, in) == PowerProfile::WAITING);
  in.scaleBusy = false;
  in.performanceBoost = false;
  assert(policy.update(40002, in) == PowerProfile::OFF);

  // Machine power: ON holds 80 MHz, the OFF edge ends the cooldown, UNKNOWN
  // keeps the 5-minute rule, and use after OFF restarts the cooldown.
  policy = PowerPolicy{};
  in = PowerInputs{};
  in.idleSaving = in.performanceBoost = true;
  in.machinePower = MachinePower::ON;
  assert(policy.update(0, in) == PowerProfile::MACHINE_ON);
  assert(policy.update(600000, in) == PowerProfile::MACHINE_ON);
  in.webActive = true;
  assert(policy.update(600001, in) == PowerProfile::WEB);
  in.webActive = false;
  in.physicalEdge = true;
  assert(policy.update(600002, in) == PowerProfile::MACHINE_ON);
  in.physicalEdge = false;
  in.machinePower = MachinePower::OFF;
  assert(policy.update(600003, in) == PowerProfile::SETTLING);
  assert(policy.cooldownRemaining(600003) == 0);
  assert(policy.update(601003, in) == PowerProfile::IDLE);
  in.physicalEdge = true;
  assert(policy.update(602000, in) == PowerProfile::COOLDOWN);
  in.physicalEdge = false;
  assert(policy.update(901999, in) == PowerProfile::COOLDOWN);
  in.machinePower = MachinePower::UNKNOWN;
  in.physicalEdge = true;
  assert(policy.update(902000, in) == PowerProfile::COOLDOWN);
  in.physicalEdge = false;
  in.machinePower = MachinePower::OFF;  // A fresh OFF edge ends it again.
  assert(policy.update(903000, in) == PowerProfile::SETTLING);
  in.machinePower = MachinePower::UNKNOWN;
  in.physicalEdge = true;
  assert(policy.update(904000, in) == PowerProfile::COOLDOWN);
  in.physicalEdge = false;
  assert(policy.update(1203999, in) == PowerProfile::COOLDOWN);
  assert(policy.update(1204000, in) == PowerProfile::SETTLING);
  in.machinePower = MachinePower::ON;
  in.idleSaving = false;
  assert(policy.update(1204001, in) == PowerProfile::MACHINE_ON);

  notePowerWebActivity(UINT32_MAX - 1000, 30);
  assert(powerWebActive(1000));
  assert(!powerWebActive(29000));
  assert(!powerWebActive(UINT32_MAX - 1000)); // Expiry cannot resurrect.
  notePowerWebActivity(100, 31);
  assert(!powerWebActive(101));
  notePowerWebActivity(100, 2);
  assert(powerWebActive(2099));
  assert(!powerWebActive(2100));
  std::thread http([] {
    for (uint32_t now = 1; now < 10000; ++now) notePowerWebActivity(now, 30);
  });
  for (uint32_t now = 1; now < 10000; ++now) (void)powerWebActive(now);
  http.join();
  assert(powerWebActive(10000));

  PowerClock clock;
  int calls = 0;
  const auto configure = [&](int minimum, int maximum) {
    ++calls;
    assert(minimum <= maximum);
    return maximum == 160 ? -1 : 0;
  };
  assert(clock.apply(PowerProfile::OFF, true, configure));
  assert(clock.apply(PowerProfile::IDLE, true, configure));
  assert(clock.apply(PowerProfile::WAITING, false, configure));
  assert(calls == 3);  // Without boost the scale window is a fixed 80 MHz.
  assert(clock.apply(PowerProfile::MANUAL, false, configure));
  assert(calls == 3);  // Same range: no reconfiguration.
  assert(clock.apply(PowerProfile::WAITING, true, configure));
  assert(clock.applied() == PowerProfile::OFF && clock.error() == -1);
  assert(calls == 5);
  assert(clock.apply(PowerProfile::WORKING, true, configure));
  assert(calls == 5);  // Latched fault, no reconfiguration storm.
  PowerClock fatal;
  assert(!fatal.apply(PowerProfile::WORKING, true, [](int, int) { return -2; }));
  assert(!fatal.apply(PowerProfile::OFF, true, [](int, int) { return 0; }));
}

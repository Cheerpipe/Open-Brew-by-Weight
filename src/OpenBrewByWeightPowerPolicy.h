#pragma once

#include <stdint.h>

namespace openbrewbyweight {

enum class PowerProfile : uint8_t {
  OFF, IDLE, WAITING, WORKING, MANUAL, COOLDOWN, WEB, MAINTENANCE, SETTLING,
  MACHINE_ON
};

inline const char *powerProfileName(PowerProfile profile) {
  static const char *const names[] = {
      "off", "idle", "waiting", "working", "manual", "cooldown", "web",
      "maintenance", "settling", "machine-on"};
  return names[static_cast<uint8_t>(profile)];
}

// The scale-window profiles reach 160 MHz only with Performance boost on.
inline int powerMaxMhz(PowerProfile profile, bool boost) {
  return boost && (profile == PowerProfile::WORKING || profile == PowerProfile::WAITING)
             ? 160 : 80;
}
inline int powerMinMhz(PowerProfile profile, bool boost) {
  return profile == PowerProfile::IDLE ? 40 : powerMaxMhz(profile, boost);
}

// The machine integration's assumed power state; UNKNOWN without one.
enum class MachinePower : uint8_t { UNKNOWN, ON, OFF };

struct PowerInputs {
  bool idleSaving = false;
  bool performanceBoost = false;
  MachinePower machinePower = MachinePower::UNKNOWN;
  bool machineBusy = false;  // Includes uncertain stop and start/stop pulses.
  bool scaleBusy = false;    // Includes GAP/GATT setup, before usable weight.
  bool physicalEdge = false;
  bool webActive = false;
  bool maintenance = false;
};

// Control owner only. Timers govern energy, never actuation or shot duration.
class PowerPolicy {
 public:
  static constexpr uint32_t cooldownMs = 300000;
  static constexpr uint32_t scaleGraceMs = 30000;
  PowerProfile update(uint32_t now, const PowerInputs &in) {
    // A machine known to switch off ends the cooldown; later use restarts it.
    if (in.machinePower == MachinePower::OFF && machinePower_ != MachinePower::OFF)
      cooldown_ = false;
    machinePower_ = in.machinePower;
    if (in.physicalEdge || (wasBusy_ && !in.machineBusy)) {
      lastUseMs_ = now;
      cooldown_ = true;
    }
    wasBusy_ = in.machineBusy;
    if (wasScaleBusy_ && !in.scaleBusy) {
      scaleDisconnectedMs_ = now;
      scaleGrace_ = true;
    }
    wasScaleBusy_ = in.scaleBusy;
    if (scaleGrace_ && uint32_t(now - scaleDisconnectedMs_) >= scaleGraceMs)
      scaleGrace_ = false;
    const bool scaleBoost = in.scaleBusy || scaleGrace_;
    if (cooldown_ && uint32_t(now - lastUseMs_) >= cooldownMs)
      cooldown_ = false;
    PowerProfile desired = PowerProfile::IDLE;
    if (!in.idleSaving && !in.performanceBoost) desired = PowerProfile::OFF;
    else if (in.machineBusy)
      desired = scaleBoost ? PowerProfile::WORKING : PowerProfile::MANUAL;
    else if (scaleBoost) desired = PowerProfile::WAITING;
    else if (in.maintenance) desired = PowerProfile::MAINTENANCE;
    else if (in.webActive) desired = PowerProfile::WEB;
    else if (in.machinePower == MachinePower::ON) desired = PowerProfile::MACHINE_ON;
    else if (cooldown_) desired = PowerProfile::COOLDOWN;
    else if (!in.idleSaving) desired = PowerProfile::OFF;
    if (desired != PowerProfile::IDLE) idlePending_ = false;
    else {
      if (!idlePending_) {
        idlePending_ = true;
        idleSinceMs_ = now;
      }
      if (uint32_t(now - idleSinceMs_) < 1000)
        desired = PowerProfile::SETTLING;
    }
    return desired;
  }
  uint32_t cooldownRemaining(uint32_t now) const {
    const uint32_t elapsed = now - lastUseMs_;
    return cooldown_ && elapsed < cooldownMs ? cooldownMs - elapsed : 0;
  }

 private:
  uint32_t lastUseMs_ = 0;
  uint32_t scaleDisconnectedMs_ = 0;
  uint32_t idleSinceMs_ = 0;
  bool cooldown_ = false;
  bool wasBusy_ = false;
  bool wasScaleBusy_ = false;
  bool scaleGrace_ = false;
  bool idlePending_ = false;
  MachinePower machinePower_ = MachinePower::UNKNOWN;
};

// A failed boost degrades to legacy 80 MHz. Never retry a fault every tick.
class PowerClock {
 public:
  template <typename Configure>
  bool apply(PowerProfile requested, bool boost, Configure configure) {
    if (failed_) requested = PowerProfile::OFF;
    const int minimum = powerMinMhz(requested, boost);
    const int maximum = powerMaxMhz(requested, boost);
    if (initialized_ && minimum == minMhz_ && maximum == maxMhz_) {
      applied_ = requested;
      return safe_;
    }
    error_ = configure(minimum, maximum);
    safe_ = error_ == 0;
    minMhz_ = minimum;
    maxMhz_ = maximum;
    if (!safe_) {
      failed_ = true;
      requested = PowerProfile::OFF;
      minMhz_ = maxMhz_ = 80;
      safe_ = configure(80, 80) == 0;
    }
    initialized_ = true;
    applied_ = requested;
    return safe_;
  }
  PowerProfile applied() const { return applied_; }
  int error() const { return error_; }

 private:
  PowerProfile applied_ = PowerProfile::OFF;
  int minMhz_ = 0;
  int maxMhz_ = 0;
  int error_ = 0;
  bool initialized_ = false;
  bool failed_ = false;
  bool safe_ = false;
};

}  // namespace openbrewbyweight

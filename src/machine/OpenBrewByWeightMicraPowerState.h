#pragma once

#include "OpenBrewByWeightLineaMicraTypes.h"
#include "OpenBrewByWeightMicraTiming.h"

namespace openbrewbyweight {

// One optimistic overlay at a time, in either direction: the paddle wake
// gesture asserts ON before the cloud agrees, and an accepted scale-shutdown
// command asserts OFF before the next dashboard read confirms it. The overlay
// changes only the effective state and observation quality; the confirmed
// classification and its sample stay untouched until an authoritative read
// started after the overlay accepts, matched by generation.
// Qualification depends only on the power state: a stale sample still reports
// its last confirmed ON or OFF and qualifies; UNKNOWN and a live overlay do
// not.
enum class OptimisticDirection : uint8_t { NONE, ON, OFF };

class LineaMicraPowerStateTracker {
 public:
  LineaMicraStatus effectiveStatus(const LineaMicraStatus &authoritative,
                                   bool observing, uint32_t now) const {
    LineaMicraStatus result = authoritative;
    if (!observing || result.sampleAtMs == 0) {
      result.powerState = LineaMicraPowerState::UNKNOWN;
      result.effectiveOn = true;
    } else if (direction_ != OptimisticDirection::NONE &&
        static_cast<uint32_t>(now - optimisticAtMs_) <
            micra_timing::kOptimisticOverlayMs) {
      result.quality = LineaMicraObservationQuality::OPTIMISTIC;
      result.effectiveOn = direction_ == OptimisticDirection::ON;
      result.optimisticOn = direction_ == OptimisticDirection::ON;
      result.optimisticOff = direction_ == OptimisticDirection::OFF;
    } else if (!result.connectionFreshness &&
               result.powerState != LineaMicraPowerState::UNKNOWN &&
               static_cast<uint32_t>(now - result.sampleAtMs) >=
                   micra_timing::kStateFreshnessMs) {
      result.quality = LineaMicraObservationQuality::STALE;
    }
    if (observing && held_) {
      result.powerState = heldKnown_
          ? (heldOn_ ? LineaMicraPowerState::ON : LineaMicraPowerState::OFF)
          : LineaMicraPowerState::UNKNOWN;
      result.effectiveOn = heldOn_;
      result.quality = LineaMicraObservationQuality::STALE;
      result.optimisticOn = result.optimisticOff = false;
    }
    return result;
  }

  bool notePhysicalStart(const LineaMicraStatus &authoritative, bool observing,
                         bool recognizeWake, uint32_t now) {
    if (!recognizeWake) return false;
    return notePowerOnCommandAccepted(authoritative, observing, now);
  }

  // Optimistic ON arms for any cloud-accepted power-on command — the paddle
  // wake gesture or a scale-triggered turn-on — once the confirmed state is
  // OFF, current or stale: the machine is certainly waking, but the dashboard
  // has not said so yet.
  bool notePowerOnCommandAccepted(const LineaMicraStatus &authoritative,
                                  bool observing, uint32_t now) {
    return arm(authoritative, observing, OptimisticDirection::ON, now);
  }

  // A scale shutdown only asserts optimistic OFF once the cloud command was
  // accepted and the confirmed state is ON, current or stale: the machine is
  // certainly heading to standby, but the dashboard has not said so yet.
  bool noteStandbyCommandAccepted(const LineaMicraStatus &authoritative,
                                  bool observing, uint32_t now) {
    return arm(authoritative, observing, OptimisticDirection::OFF, now);
  }

  uint32_t generation() const { return generation_; }

  bool retained() const { return held_; }

  void hold(const LineaMicraStatus &authoritative, bool observing, uint32_t now) {
    if (!observing || held_) return;
    const auto effective = effectiveStatus(authoritative, observing, now);
    heldOn_ = effective.effectiveOn;
    heldKnown_ = effective.powerState != LineaMicraPowerState::UNKNOWN;
    held_ = true;
  }

  bool noteEvidence(uint32_t intent, LineaMicraPowerState state,
                    uint32_t now, bool reconcile) {
    if (intent != generation_) return false;
    const bool live = direction_ != OptimisticDirection::NONE &&
        static_cast<uint32_t>(now - optimisticAtMs_) < micra_timing::kOptimisticOverlayMs;
    const bool matches = state != LineaMicraPowerState::UNKNOWN &&
        (state == LineaMicraPowerState::ON) == (direction_ == OptimisticDirection::ON);
    if (!live || matches || (reconcile &&
        static_cast<uint32_t>(now - optimisticAtMs_) >= micra_timing::kPostWakeObservationDelayMs)) {
      direction_ = OptimisticDirection::NONE;
      if (state != LineaMicraPowerState::UNKNOWN) held_ = false;
    }
    return true;
  }

  void reset() {
    direction_ = OptimisticDirection::NONE;
    held_ = false;
    ++generation_;
  }

 private:
  bool arm(const LineaMicraStatus &authoritative, bool observing,
           OptimisticDirection direction, uint32_t now) {
    const LineaMicraStatus before =
        effectiveStatus(authoritative, observing, now);
    const LineaMicraPowerState required =
        direction == OptimisticDirection::ON
            ? LineaMicraPowerState::OFF
            : LineaMicraPowerState::ON;
    if (before.powerState != required ||
        (before.quality != LineaMicraObservationQuality::CURRENT &&
         before.quality != LineaMicraObservationQuality::STALE))
      return false;
    direction_ = direction;
    optimisticAtMs_ = now;
    if (held_) {
      heldOn_ = direction == OptimisticDirection::ON;
      heldKnown_ = true;
    }
    ++generation_;
    return true;
  }

  uint32_t generation_ = 0;
  uint32_t optimisticAtMs_ = 0;
  OptimisticDirection direction_ = OptimisticDirection::NONE;
  bool held_ = false;
  bool heldOn_ = true;
  bool heldKnown_ = false;
};

}  // namespace openbrewbyweight

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <new>

#ifndef OPEN_BREW_BY_WEIGHT_HOST_TEST
#include <esp_timer.h>
#endif

#include "OpenBrewByWeightAlertTone.h"
#include "OpenBrewByWeightBullseye.h"
#include "OpenBrewByWeightBuzzerPatterns.h"
#include "OpenBrewByWeightBuzzerPassive.h"
#include "OpenBrewByWeightDomain.h"
#include "OpenBrewByWeightPsram.h"
#include "OpenBrewByWeightTaskMutex.h"

namespace openbrewbyweight {

inline bool buzzerPatternIsHighPriority(BuzzerPattern pattern) {
  return pattern == BuzzerPattern::TRIPLE || pattern == BuzzerPattern::ECHO ||
         pattern == BuzzerPattern::ECHO_INVERTED;
}

inline bool buzzerRequestIsHighPriority(BuzzerPattern pattern, BuzzerCue cue) {
  if (cue != BuzzerCue::NONE) {
    return buzzerCueIsHighPriority(cue);
  }
  return buzzerPatternIsHighPriority(pattern);
}

#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER == 1
struct RtttlCatalog {
  static constexpr uint8_t kCueCount =
      static_cast<uint8_t>(BuzzerCue::RECOVERY_ERROR) + 1;
  static constexpr uint8_t kPulseRateCount =
      static_cast<uint8_t>(ExtendedPulseRate::RAPID) + 1;
  // Built-in cues use at most five notes; reserve eight and reject growth
  // before writing. Custom bullseye playback retains its full 250-note limit.
  static constexpr uint8_t kFixedNoteCapacity = 8;
  RtttlNote bullseyeNotes[BULLSEYE_RTTTL_MAX_NOTES] = {};
  RtttlNote cueNotes[kCueCount][kFixedNoteCapacity] = {};
  RtttlNote pulseNotes[kPulseRateCount][kFixedNoteCapacity] = {};
};
#endif

// Non-blocking local-buzzer driver. Passive piezo via hardware PWM (ledc)
// and RTTTL for operational cues. Host stubs map PWM to a digital level.
// Phase edges are armed on esp_timer so Serial/loop stalls cannot stretch
// beeps. Debug/BUZZER_TEST still plays timed BuzzerPattern motifs at
// BUZZER_TONE_HZ.
struct LocalBuzzer {
  uint8_t pin = 0;
  bool ready = false;
  BuzzerPattern active = BuzzerPattern::NONE;
  BuzzerPattern pending = BuzzerPattern::NONE;
  BuzzerCue activeCue = BuzzerCue::NONE;
  BuzzerCue pendingCue = BuzzerCue::NONE;
  uint8_t activePulseRate = 0;
  uint8_t pendingPulseRate = 0;
  uint8_t beepIndex = 0;
  uint8_t beepCount = 0;
  bool toneOn = false;
  bool looping = false;
  uint32_t phaseStartedAtMs = 0;
  uint32_t onMs = 0;
  uint32_t gapMs = 0;
  uint32_t deadlineAtMs = 0;
  uint32_t pendingDurationMs = 0;
  uint32_t acceptedRequests = 0;
  // These remain observable in the control diagnostic snapshot through the
  // buzzer owner; an audio scheduling fault must not be silently mistaken for
  // an ordinary completed cue.
  uint32_t phaseTimerFailures = 0;
  int32_t lastPhaseTimerError = ESP_OK;
  uint32_t toneHz = BUZZER_TONE_HZ;
  const BuzzerNote *sequenceNotes = nullptr;
  // The catalog and the active playback buffer both stay in internal RAM:
  // playback starts (and pending slots resolve) from the esp_timer task, with
  // task-context access serialized by the buzzer mutex. Reading PSRAM there
  // during a flash path that disables cache can panic despite XIP. The
  // catalog is only ~2 KiB and written once at begin().
  RtttlNote rtttlBuf[BULLSEYE_RTTTL_MAX_NOTES] = {};
  uint8_t rtttlCount = 0;
  uint8_t bullseyeNoteCount = 0;
  bool rtttlPlayback = false;
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER == 1
  static constexpr uint8_t kRtttlCueCount = RtttlCatalog::kCueCount;
  static constexpr uint8_t kPulseRateCount = RtttlCatalog::kPulseRateCount;
  RtttlCatalog *rtttlCatalog = nullptr;
  uint8_t cueNoteCount[kRtttlCueCount] = {};
  uint8_t pulseNoteCount[kPulseRateCount] = {};
#endif
  esp_timer_handle_t phaseTimer = nullptr;
  mutable TaskMutex mutex;

  void begin(uint8_t gpioPin);
  // Starts immediately when idle, otherwise keeps one pending slot. High
  // priority patterns (TRIPLE, ECHO, ECHO_INVERTED) and cues (NO_SCALE,
  // GUARD_STOP, SCALE_CONNECTED, SCALE_LOST) upgrade any weaker pending
  // slot; a weaker request is rejected while a high-priority slot is
  // pending. A finite pattern interrupts an active looping pulse so a
  // looping cue cannot block alarms. durationMs is used only for pulse
  // trains (0 = until stopped). Returns false if unsupported, not ready,
  // or not accepted.
  bool request(BuzzerPattern pattern, uint32_t durationMs = 0);
  bool requestTone(const BuzzerToneCommand &cmd);
  bool configureBullseyeRtttl(const char *rtttl);
  // Tests must start immediately so restoring the saved custom tune cannot
  // change a queued request. Normal shot completion may use the pending slot.
  bool requestBullseye(bool allowQueue = true);
  // True when a request would start now instead of waiting in the pending slot.
  bool canStartNow() const;
  void stopIfCue(BuzzerCue cue);
  void stopAll();
  // Drop a queued extended pulse and an infinite operational/debug pulse.
  // Finite Debug Slow/Medium/Fast/Rapid (deadline set) keep playing.
  void stopExtendedPulse();
  // End-of-cycle: drop queued and active pulse trains / looping cues.
  void stopPulseTrains();
  void service();
  bool busy() const {
    TaskLockGuard lock(mutex);
    return active != BuzzerPattern::NONE || pending != BuzzerPattern::NONE ||
           activeCue != BuzzerCue::NONE || pendingCue != BuzzerCue::NONE;
  }
  bool playingExtendedPulse(const BuzzerToneCommand &cmd) const {
    TaskLockGuard lock(mutex);
    return cmd.valid &&
           ((activeCue == cmd.cue && activePulseRate == cmd.pulseRate) ||
            (pendingCue == cmd.cue && pendingPulseRate == cmd.pulseRate));
  }

 private:
  bool canStartNowLocked() const {
    return (active == BuzzerPattern::NONE && activeCue == BuzzerCue::NONE) ||
           buzzerPatternIsPulseTrain(active) || (looping && rtttlPlayback);
  }
  void startTone();
  void stopTone();
  void applyDeadline(uint32_t durationMs, uint32_t nowMs);
  bool deadlineReached(uint32_t nowMs) const;
  bool cancelPhaseTimer();
  bool armPhaseTimer(uint32_t delayMs);
  void finish(uint32_t nowMs);
  bool startPattern(BuzzerPattern pattern, uint32_t nowMs);
  bool startRtttl(const BuzzerToneCommand &cmd, uint32_t nowMs);
  void applySequenceNote(uint8_t index);
  void applyRtttlNote(uint8_t index);
  void clearPlayback();
  bool acceptLocked(BuzzerPattern pattern, BuzzerCue cue, uint32_t durationMs,
                    bool rtttl, const BuzzerToneCommand *cmd);
  void startPendingDrainedLocked(uint32_t nowMs);
  static void phaseTimerCallback(void *arg);
};

inline bool LocalBuzzer::cancelPhaseTimer() {
  if (phaseTimer == nullptr) return true;
  const int status = esp_timer_stop(phaseTimer);
#ifdef OPEN_BREW_BY_WEIGHT_HOST_TEST
  const bool benign = status == ESP_OK;
#else
  // Stopping an already-fired/already-stopped timer is expected during a
  // phase callback; all other failures leave the cue fail-silent.
  const bool benign = status == ESP_OK || status == ESP_ERR_INVALID_STATE;
#endif
  if (benign) return true;
  ++phaseTimerFailures;
  lastPhaseTimerError = status;
  stopTone();
  clearPlayback();
  pending = BuzzerPattern::NONE;
  pendingCue = BuzzerCue::NONE;
  pendingPulseRate = 0;
  pendingDurationMs = 0;
  return false;
}

inline bool LocalBuzzer::armPhaseTimer(uint32_t delayMs) {
  if (!cancelPhaseTimer()) return false;
  if (phaseTimer == nullptr || delayMs == 0) {
    if (delayMs == 0) return true;
    ++phaseTimerFailures;
    lastPhaseTimerError = ESP_ERR_INVALID_STATE;
    stopTone();
    clearPlayback();
    return false;
  }
  const int status = esp_timer_start_once(
      phaseTimer, static_cast<uint64_t>(delayMs) * 1000ULL);
  if (status == ESP_OK) return true;
  ++phaseTimerFailures;
  lastPhaseTimerError = status;
  stopTone();
  clearPlayback();
  pending = BuzzerPattern::NONE;
  pendingCue = BuzzerCue::NONE;
  pendingPulseRate = 0;
  pendingDurationMs = 0;
  return false;
}

inline void LocalBuzzer::phaseTimerCallback(void *arg) {
  if (arg == nullptr) {
    return;
  }
  static_cast<LocalBuzzer *>(arg)->service();
}

inline void LocalBuzzer::clearPlayback() {
  active = BuzzerPattern::NONE;
  activeCue = BuzzerCue::NONE;
  activePulseRate = 0;
  beepIndex = 0;
  beepCount = 0;
  looping = false;
  deadlineAtMs = 0;
  sequenceNotes = nullptr;
  rtttlCount = 0;
  rtttlPlayback = false;
  toneHz = BUZZER_TONE_HZ;
}

inline void LocalBuzzer::begin(uint8_t gpioPin) {
  pin = gpioPin;
  ready = false;
  pending = BuzzerPattern::NONE;
  pendingCue = BuzzerCue::NONE;
  pendingPulseRate = 0;
  pendingDurationMs = 0;
  acceptedRequests = 0;
  phaseTimerFailures = 0;
  lastPhaseTimerError = ESP_OK;
  toneOn = false;
  clearPlayback();
  cancelPhaseTimer();
  if (!BUZZER_SUPPORT_ENABLED || pin == 0) {
    return;
  }
  if (phaseTimer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = &LocalBuzzer::phaseTimerCallback;
    args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "buzzer_phase";
    if (esp_timer_create(&args, &phaseTimer) != ESP_OK) {
      phaseTimer = nullptr;
      // Creation has no timer-specific benign status. Preserve the failure
      // for diagnostics without keeping a partially initialized buzzer live.
      lastPhaseTimerError = -1;
      ++phaseTimerFailures;
      return;
    }
  }
  if (!buzzerPassiveBegin(pin)) {
    return;
  }
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER == 1
  if (rtttlCatalog == nullptr) {
    rtttlCatalog =
        static_cast<RtttlCatalog *>(allocInternal(sizeof(RtttlCatalog)));
  }
  if (rtttlCatalog == nullptr) {
    return;
  }
  ::new (static_cast<void *>(rtttlCatalog)) RtttlCatalog{};
  memset(cueNoteCount, 0, sizeof(cueNoteCount));
  memset(pulseNoteCount, 0, sizeof(pulseNoteCount));
  bullseyeNoteCount = 0;
  for (uint8_t i = 1; i < kRtttlCueCount; ++i) {
    const BuzzerCue cue = static_cast<BuzzerCue>(i);
    if (buzzerCueIsLooping(cue) || cue == BuzzerCue::BULLSEYE) {
      continue;
    }
    uint8_t count = 0;
    if (!parseRtttlBounded(rtttlForCue(cue), rtttlCatalog->cueNotes[i],
                           RtttlCatalog::kFixedNoteCapacity, count) ||
        count == 0) {
      return;
    }
    cueNoteCount[i] = count;
  }
  for (uint8_t rate = 1; rate < kPulseRateCount; ++rate) {
    uint8_t count = 0;
    if (!parseRtttlBounded(rtttlForExtendedPulseRate(rate),
                    rtttlCatalog->pulseNotes[rate],
                    RtttlCatalog::kFixedNoteCapacity, count) || count == 0) {
      return;
    }
    pulseNoteCount[rate] = count;
  }
#endif
  ready = true;
}

inline void LocalBuzzer::startTone() {
  if (!ready || toneOn || toneHz == 0) {
    return;
  }
  buzzerPassiveSetTone(pin, toneHz);
  toneOn = true;
}

inline void LocalBuzzer::stopTone() {
  if (!ready || !toneOn) {
    toneOn = false;
    return;
  }
  buzzerPassiveSetTone(pin, 0);
  toneOn = false;
}

inline void LocalBuzzer::applyDeadline(uint32_t durationMs, uint32_t nowMs) {
  deadlineAtMs = durationMs == 0 ? 0 : nowMs + durationMs;
}

inline bool LocalBuzzer::deadlineReached(uint32_t nowMs) const {
  return deadlineAtMs != 0 &&
         static_cast<int32_t>(nowMs - deadlineAtMs) >= 0;
}

// Starts the queued pending tone (if any) after the active one stops.
// Callers hold the mutex; a no-op cancels the phase timer.
inline void LocalBuzzer::startPendingDrainedLocked(uint32_t nowMs) {
  if (pending == BuzzerPattern::NONE && pendingCue == BuzzerCue::NONE) {
    cancelPhaseTimer();
    return;
  }
  const BuzzerPattern next = pending;
  BuzzerToneCommand cmd;
  cmd.cue = pendingCue;
  cmd.pulseRate = pendingPulseRate;
  cmd.looping = buzzerCueIsLooping(cmd.cue);
  cmd.durationMs = pendingDurationMs;
  cmd.valid = true;
  pending = BuzzerPattern::NONE;
  pendingCue = BuzzerCue::NONE;
  pendingPulseRate = 0;
  pendingDurationMs = 0;
  const bool started = cmd.cue != BuzzerCue::NONE ? startRtttl(cmd, nowMs)
                                                  : startPattern(next, nowMs);
  if (started) {
    applyDeadline(cmd.durationMs, nowMs);
  } else {
    cancelPhaseTimer();
  }
}

inline void LocalBuzzer::finish(uint32_t nowMs) {
  stopTone();
  clearPlayback();
  startPendingDrainedLocked(nowMs);
}

inline void LocalBuzzer::applySequenceNote(uint8_t index) {
  if (sequenceNotes == nullptr) {
    return;
  }
  onMs = sequenceNotes[index].onMs;
  gapMs = sequenceNotes[index].gapMs;
  toneHz = BUZZER_TONE_HZ;
}

inline void LocalBuzzer::applyRtttlNote(uint8_t index) {
  if (index >= rtttlCount) {
    return;
  }
  toneHz = rtttlBuf[index].freqHz;
  if (toneHz == 0) {
    onMs = 0;
    gapMs = rtttlBuf[index].durationMs;
  } else {
    // A repeated pitch needs a short break, or the two notes merge into one.
    const uint32_t noteMs = rtttlBuf[index].durationMs;
    gapMs = index + 1U < rtttlCount && rtttlBuf[index + 1U].freqHz == toneHz
                ? noteMs / 10U
                : 0;
    onMs = noteMs - gapMs;
  }
}

inline bool LocalBuzzer::startRtttl(const BuzzerToneCommand &cmd,
                                    uint32_t nowMs) {
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER != 1
  (void)cmd;
  (void)nowMs;
  return false;
#else
  const uint8_t id = static_cast<uint8_t>(cmd.cue);
  const bool pulse = buzzerCueIsLooping(cmd.cue);
  if (cmd.cue == BuzzerCue::BULLSEYE) {
    if (bullseyeNoteCount == 0) {
      return false;
    }
    rtttlCount = bullseyeNoteCount;
    memcpy(rtttlBuf, rtttlCatalog->bullseyeNotes,
           sizeof(RtttlNote) * rtttlCount);
    activePulseRate = 0;
  } else if (pulse) {
    const uint8_t rate = cmd.pulseRate;
    if (rate == 0 || rate >= kPulseRateCount || pulseNoteCount[rate] == 0) {
      return false;
    }
    rtttlCount = pulseNoteCount[rate];
    memcpy(rtttlBuf, rtttlCatalog->pulseNotes[rate],
           sizeof(RtttlNote) * rtttlCount);
    activePulseRate = rate;
  } else {
    if (id == 0 || id >= kRtttlCueCount || cueNoteCount[id] == 0) {
      return false;
    }
    rtttlCount = cueNoteCount[id];
    memcpy(rtttlBuf, rtttlCatalog->cueNotes[id],
           sizeof(RtttlNote) * rtttlCount);
    activePulseRate = 0;
  }
  rtttlPlayback = true;
  sequenceNotes = nullptr;
  looping = cmd.looping;
  deadlineAtMs = 0;
  active = BuzzerPattern::NONE;
  activeCue = cmd.cue;
  beepCount = rtttlCount;
  beepIndex = 0;
  applyRtttlNote(0);
  phaseStartedAtMs = nowMs;
  if (toneHz > 0) {
    startTone();
    return armPhaseTimer(onMs);
  } else {
    return armPhaseTimer(gapMs);
  }
#endif
}

inline bool LocalBuzzer::startPattern(BuzzerPattern pattern, uint32_t nowMs) {
  looping = false;
  deadlineAtMs = 0;
  sequenceNotes = nullptr;
  rtttlPlayback = false;
  rtttlCount = 0;
  activeCue = BuzzerCue::NONE;
  activePulseRate = 0;
  toneHz = BUZZER_TONE_HZ;
  if (pattern == BuzzerPattern::SINGLE) {
    beepCount = 1;
    onMs = BUZZER_SINGLE_ON_MS;
    gapMs = 0;
  } else if (pattern == BuzzerPattern::LONG) {
    beepCount = 1;
    onMs = BUZZER_LONG_ON_MS;
    gapMs = 0;
  } else if (pattern == BuzzerPattern::RECOVERY_LONG) {
    beepCount = 1;
    onMs = BUZZER_RECOVERY_LONG_ON_MS;
    gapMs = 0;
  } else if (pattern == BuzzerPattern::DOUBLE) {
    beepCount = 2;
    onMs = BUZZER_BEEP_ON_MS;
    gapMs = BUZZER_BEEP_GAP_MS;
  } else if (pattern == BuzzerPattern::TRIPLE) {
    beepCount = 3;
    onMs = BUZZER_BEEP_ON_MS;
    gapMs = BUZZER_BEEP_GAP_MS;
  } else if (buzzerPatternIsPulseTrain(pattern)) {
    beepCount = 0;
    looping = true;
    onMs = BUZZER_PULSE_TRAIN_ON_MS;
    if (pattern == BuzzerPattern::PULSE_3HZ) {
      gapMs = BUZZER_PULSE_TRAIN_3HZ_GAP_MS;
    } else if (pattern == BuzzerPattern::PULSE_4HZ) {
      gapMs = BUZZER_PULSE_TRAIN_4HZ_GAP_MS;
    } else if (pattern == BuzzerPattern::PULSE_5HZ) {
      gapMs = BUZZER_PULSE_TRAIN_5HZ_GAP_MS;
    } else {
      gapMs = BUZZER_PULSE_TRAIN_GAP_MS;
    }
  } else if (buzzerPatternIsSequence(pattern)) {
    uint8_t count = 0;
    sequenceNotes = buzzerSequenceNotes(pattern, count);
    if (sequenceNotes == nullptr || count == 0) {
      return false;
    }
    beepCount = count;
    applySequenceNote(0);
  } else {
    return false;
  }
  active = pattern;
  beepIndex = 0;
  phaseStartedAtMs = nowMs;
  startTone();
  return armPhaseTimer(onMs);
}

inline bool LocalBuzzer::acceptLocked(BuzzerPattern pattern, BuzzerCue cue,
                                      uint32_t durationMs, bool rtttl,
                                      const BuzzerToneCommand *cmd) {
  const uint32_t nowMs = millis();
  const uint8_t pulseRate = cmd != nullptr ? cmd->pulseRate : 0;
  // A long custom melody must not hold back a high-priority alert.
  if (activeCue == BuzzerCue::BULLSEYE && cue != BuzzerCue::BULLSEYE &&
      buzzerRequestIsHighPriority(pattern, cue)) {
    stopTone();
    clearPlayback();
  }
  bool started = false;
  bool accepted = false;
  if (buzzerPatternIsPulseTrain(active) || (looping && rtttlPlayback)) {
    if (pattern == active && cue == activeCue &&
        pulseRate == activePulseRate) {
      applyDeadline(durationMs, nowMs);
      ++acceptedRequests;
      accepted = true;
    } else {
      pending = BuzzerPattern::NONE;
      pendingCue = BuzzerCue::NONE;
      pendingPulseRate = 0;
      pendingDurationMs = 0;
      stopTone();
      if (rtttl && cmd != nullptr) {
        started = startRtttl(*cmd, nowMs);
      } else {
        started = startPattern(pattern, nowMs);
      }
      if (started) {
        applyDeadline(durationMs, nowMs);
        ++acceptedRequests;
        accepted = true;
      }
    }
  } else if (active == BuzzerPattern::NONE && activeCue == BuzzerCue::NONE) {
    if (rtttl && cmd != nullptr) {
      started = startRtttl(*cmd, nowMs);
    } else {
      started = startPattern(pattern, nowMs);
    }
    if (started) {
      applyDeadline(durationMs, nowMs);
      ++acceptedRequests;
      accepted = true;
    }
  } else if (pending == BuzzerPattern::NONE && pendingCue == BuzzerCue::NONE) {
    pending = pattern;
    pendingCue = cue;
    pendingPulseRate = pulseRate;
    pendingDurationMs = durationMs;
    ++acceptedRequests;
    accepted = true;
  } else if (buzzerRequestIsHighPriority(pattern, cue) &&
             !buzzerRequestIsHighPriority(pending, pendingCue)) {
    pending = pattern;
    pendingCue = cue;
    pendingPulseRate = pulseRate;
    pendingDurationMs =
        buzzerPatternIsPulseTrain(pattern) || buzzerCueIsLooping(cue)
            ? durationMs
            : 0;
    accepted = true;
  } else {
    accepted = pattern == pending && cue == pendingCue &&
               pulseRate == pendingPulseRate;
  }
  return accepted;
}

inline bool LocalBuzzer::request(BuzzerPattern pattern, uint32_t durationMs) {
  if (!BUZZER_SUPPORT_ENABLED || !ready || pattern == BuzzerPattern::NONE) {
    return false;
  }
  TaskLockGuard lock(mutex);
  return acceptLocked(pattern, BuzzerCue::NONE, durationMs, false, nullptr);
}

inline bool LocalBuzzer::requestTone(const BuzzerToneCommand &cmd) {
  if (!BUZZER_SUPPORT_ENABLED || !ready || !cmd.valid ||
      cmd.cue == BuzzerCue::NONE) {
    return false;
  }
  TaskLockGuard lock(mutex);
  return acceptLocked(BuzzerPattern::NONE, cmd.cue, cmd.durationMs, true,
                      &cmd);
}

inline bool LocalBuzzer::configureBullseyeRtttl(const char *rtttl) {
#if OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER != 1
  (void)rtttl;
  return false;
#else
  if (rtttl == nullptr || rtttl[0] == '\0') {
    TaskLockGuard lock(mutex);
    bullseyeNoteCount = 0;
    return true;
  }
  // Validate before publishing a zero count. Parse directly into the inactive
  // custom-note storage so this setup-time path does not add a 1 KiB automatic
  // buffer to the Arduino loop task.
  if (!validBullseyeRtttl(rtttl)) {
    return false;
  }
  {
    TaskLockGuard lock(mutex);
    bullseyeNoteCount = 0;
  }
  uint8_t count = 0;
  if (rtttlCatalog == nullptr ||
      !parseRtttlBounded(rtttl, rtttlCatalog->bullseyeNotes,
                         BULLSEYE_RTTTL_MAX_NOTES, count) || count == 0) {
    return false;
  }
  TaskLockGuard lock(mutex);
  bullseyeNoteCount = count;
  return true;
#endif
}

inline bool LocalBuzzer::requestBullseye(bool allowQueue) {
  BuzzerToneCommand cmd;
  cmd.cue = BuzzerCue::BULLSEYE;
  cmd.valid = true;
  if (allowQueue) {
    return requestTone(cmd);
  }
  if (!BUZZER_SUPPORT_ENABLED || !ready) {
    return false;
  }
  TaskLockGuard lock(mutex);
  return canStartNowLocked() &&
         acceptLocked(BuzzerPattern::NONE, cmd.cue, 0, true, &cmd);
}

inline bool LocalBuzzer::canStartNow() const {
  if (!BUZZER_SUPPORT_ENABLED || !ready) {
    return false;
  }
  TaskLockGuard lock(mutex);
  return canStartNowLocked();
}

inline void LocalBuzzer::stopAll() {
  if (!BUZZER_SUPPORT_ENABLED || !ready) {
    return;
  }
  TaskLockGuard lock(mutex);
  pending = BuzzerPattern::NONE;
  pendingCue = BuzzerCue::NONE;
  pendingPulseRate = 0;
  pendingDurationMs = 0;
  stopTone();
  clearPlayback();
  cancelPhaseTimer();
}

inline void LocalBuzzer::stopIfCue(BuzzerCue cue) {
  if (!BUZZER_SUPPORT_ENABLED || !ready || cue == BuzzerCue::NONE) {
    return;
  }
  TaskLockGuard lock(mutex);
  if (pendingCue == cue) {
    pending = BuzzerPattern::NONE;
    pendingCue = BuzzerCue::NONE;
    pendingPulseRate = 0;
    pendingDurationMs = 0;
  }
  if (activeCue != cue) {
    return;
  }
  const uint32_t nowMs = millis();
  stopTone();
  clearPlayback();
  startPendingDrainedLocked(nowMs);
}

inline void LocalBuzzer::stopExtendedPulse() {
  if (!BUZZER_SUPPORT_ENABLED || !ready) {
    return;
  }
  TaskLockGuard lock(mutex);
  if (buzzerCueIsLooping(pendingCue) || buzzerPatternIsPulseTrain(pending)) {
    pending = BuzzerPattern::NONE;
    pendingCue = BuzzerCue::NONE;
    pendingPulseRate = 0;
    pendingDurationMs = 0;
  }
  const bool infinitePulse =
      looping && deadlineAtMs == 0 &&
      (buzzerCueIsLooping(activeCue) || buzzerPatternIsPulseTrain(active) ||
       rtttlPlayback);
  if (!infinitePulse) {
    return;
  }
  finish(millis());
}

inline void LocalBuzzer::stopPulseTrains() {
  if (!BUZZER_SUPPORT_ENABLED || !ready) {
    return;
  }
  TaskLockGuard lock(mutex);
  if (buzzerCueIsLooping(pendingCue) || buzzerPatternIsPulseTrain(pending)) {
    pending = BuzzerPattern::NONE;
    pendingCue = BuzzerCue::NONE;
    pendingPulseRate = 0;
    pendingDurationMs = 0;
  }
  if (!(buzzerCueIsLooping(activeCue) || buzzerPatternIsPulseTrain(active) ||
        looping)) {
    return;
  }
  finish(millis());
}

inline void LocalBuzzer::service() {
  if (!BUZZER_SUPPORT_ENABLED || !ready) {
    return;
  }
  TaskLockGuard lock(mutex);
  const uint32_t nowMs = millis();
  if (active == BuzzerPattern::NONE && activeCue == BuzzerCue::NONE) {
    return;
  }
  if (looping && deadlineReached(nowMs)) {
    finish(nowMs);
    return;
  }
  const uint32_t elapsed =
      static_cast<uint32_t>(nowMs - phaseStartedAtMs);
  if (toneOn) {
    if (elapsed < onMs) {
      return;
    }
    stopTone();
    phaseStartedAtMs = nowMs;
    if (looping && !rtttlPlayback) {
      if (deadlineReached(nowMs)) {
        finish(nowMs);
      } else {
        armPhaseTimer(gapMs);
      }
      return;
    }
    if (beepIndex + 1U >= beepCount) {
      if (looping && rtttlPlayback && !deadlineReached(nowMs)) {
        beepIndex = 0;
        applyRtttlNote(0);
        if (gapMs == 0 && toneHz > 0) {
          startTone();
          armPhaseTimer(onMs);
        } else {
          armPhaseTimer(gapMs == 0 ? 1 : gapMs);
        }
      } else {
        finish(nowMs);
      }
    } else if (gapMs == 0 && rtttlPlayback) {
      ++beepIndex;
      applyRtttlNote(beepIndex);
      if (toneHz > 0) {
        startTone();
        armPhaseTimer(onMs);
      } else {
        armPhaseTimer(gapMs == 0 ? 1 : gapMs);
      }
    } else {
      armPhaseTimer(gapMs);
    }
    return;
  }
  if (elapsed < gapMs) {
    return;
  }
  if (looping && !rtttlPlayback) {
    if (deadlineReached(nowMs)) {
      finish(nowMs);
      return;
    }
    phaseStartedAtMs = nowMs;
    startTone();
    armPhaseTimer(onMs);
    return;
  }
  if (rtttlPlayback && toneHz == 0 && beepIndex + 1U >= beepCount) {
    if (looping && !deadlineReached(nowMs)) {
      beepIndex = 0;
      applyRtttlNote(0);
      phaseStartedAtMs = nowMs;
      if (toneHz > 0) {
        startTone();
        armPhaseTimer(onMs);
      } else {
        armPhaseTimer(gapMs == 0 ? 1 : gapMs);
      }
      return;
    }
    finish(nowMs);
    return;
  }
  ++beepIndex;
  if (beepIndex >= beepCount) {
    finish(nowMs);
    return;
  }
  if (rtttlPlayback) {
    applyRtttlNote(beepIndex);
  } else {
    applySequenceNote(beepIndex);
  }
  phaseStartedAtMs = nowMs;
  if (toneHz > 0) {
    startTone();
    armPhaseTimer(onMs);
  } else {
    armPhaseTimer(gapMs == 0 ? 1 : gapMs);
  }
}

}  // namespace openbrewbyweight

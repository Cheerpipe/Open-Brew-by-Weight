#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "OpenBrewByWeightScaleTypes.h"
#include "OpenBrewByWeightTaskMutex.h"

#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
#include "OpenBrewByWeightPsram.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#endif

namespace openbrewbyweight {

constexpr size_t TASK_PROFILER_MAX_TRACKED = 48;
constexpr size_t TASK_PROFILER_MAX_ROWS = 20;
constexpr size_t TASK_PROFILER_NAME_CAPACITY = 24;
constexpr uint32_t TASK_PROFILER_SAMPLE_INTERVAL_MS = 1000;
constexpr uint32_t TASK_PROFILER_MAX_DURATION_MS = 5UL * 60UL * 1000UL;

enum class LoopPhase : uint8_t {
  SAFETY_HEALTH,
  INPUT_BACKFLUSH,
  INPUT_DRIVE_PERMISSION,
  INPUT_SCALE_EVENTS,
  INPUT_MACHINE_SENSE,
  INPUT_MACHINE_SERVICE,
  GUARDS_BREW_START,
  MACHINE_INTEGRATION,
  CONTROL_STATE_MACHINE,
  CONTROL_EFFECTS,
  ALERTS_AUDIO,
  ALERTS_TIMERS,
  SHOT_FINALIZE,
  COMMANDS_GATE_MAINTENANCE,
  COMMANDS_WEB,
  COMMANDS_POWER_RESTART,
  HOUSEKEEPING_SERIAL_LOG,
  HOUSEKEEPING_SERIAL_CLI,
  HOUSEKEEPING_RUNTIME_PERSISTENCE,
  HOUSEKEEPING_SHOT_PERSISTENCE,
  HOUSEKEEPING_SCALE_PERSISTENCE,
  HOUSEKEEPING_WEIGHT_TELEMETRY,
  HOUSEKEEPING_STATUS_GATHER,
  HOUSEKEEPING_STATUS_STORE_COPY,
  HOUSEKEEPING_STATUS_COMMIT,
  HOUSEKEEPING_LED,
  DIAGNOSTICS,
  FINAL_SCALE_DRAIN,
  COUNT
};

constexpr uint8_t LOOP_PHASE_COUNT = static_cast<uint8_t>(LoopPhase::COUNT);

inline const char *loopPhaseName(LoopPhase phase) {
  switch (phase) {
    case LoopPhase::SAFETY_HEALTH: return "safety/health";
    case LoopPhase::INPUT_BACKFLUSH: return "input/backflush poll";
    case LoopPhase::INPUT_DRIVE_PERMISSION: return "input/drive permission";
    case LoopPhase::INPUT_SCALE_EVENTS: return "input/scale events";
    case LoopPhase::INPUT_MACHINE_SENSE: return "input/machine sense";
    case LoopPhase::INPUT_MACHINE_SERVICE: return "input/machine service";
    case LoopPhase::GUARDS_BREW_START: return "guards/brew start";
    case LoopPhase::MACHINE_INTEGRATION: return "machine integration";
    case LoopPhase::CONTROL_STATE_MACHINE: return "control/state machine";
    case LoopPhase::CONTROL_EFFECTS: return "control/effects";
    case LoopPhase::ALERTS_AUDIO: return "alerts/audio";
    case LoopPhase::ALERTS_TIMERS: return "alerts/timers";
    case LoopPhase::SHOT_FINALIZE: return "shot finalize";
    case LoopPhase::COMMANDS_GATE_MAINTENANCE:
      return "commands/gate+maintenance";
    case LoopPhase::COMMANDS_WEB: return "commands/web";
    case LoopPhase::COMMANDS_POWER_RESTART: return "commands/power+restart";
    case LoopPhase::HOUSEKEEPING_SERIAL_LOG: return "housekeeping/serial log";
    case LoopPhase::HOUSEKEEPING_SERIAL_CLI: return "housekeeping/serial CLI";
    case LoopPhase::HOUSEKEEPING_RUNTIME_PERSISTENCE: return "housekeeping/settings";
    case LoopPhase::HOUSEKEEPING_SHOT_PERSISTENCE: return "housekeeping/history";
    case LoopPhase::HOUSEKEEPING_SCALE_PERSISTENCE: return "housekeeping/scale save";
    case LoopPhase::HOUSEKEEPING_WEIGHT_TELEMETRY: return "housekeeping/weight";
    case LoopPhase::HOUSEKEEPING_STATUS_GATHER: return "status/gather";
    case LoopPhase::HOUSEKEEPING_STATUS_STORE_COPY: return "status/store copy";
    case LoopPhase::HOUSEKEEPING_STATUS_COMMIT: return "status/lock+commit";
    case LoopPhase::HOUSEKEEPING_LED: return "housekeeping/LED";
    case LoopPhase::DIAGNOSTICS: return "diagnostics";
    case LoopPhase::FINAL_SCALE_DRAIN: return "final scale drain";
    case LoopPhase::COUNT: break;
  }
  return "unknown";
}

inline const char *loopPhaseNameFor(uint8_t index) {
  return index < LOOP_PHASE_COUNT
             ? loopPhaseName(static_cast<LoopPhase>(index))
             : "unknown";
}

// Component-level sections of the scale worker tick, profiled on the same
// wall/own-CPU timeline as the control-loop phases and surfaced as
// `scaleWorker/<section>` rows in the task profiler.
enum class ScaleWorkerPhase : uint8_t {
  ADMIN,
  LINK,
  DISCOVERY,
  COMMANDS,
  TELEMETRY,
  COUNT
};

constexpr uint8_t SCALE_WORKER_PHASE_COUNT =
    static_cast<uint8_t>(ScaleWorkerPhase::COUNT);

inline const char *scaleWorkerPhaseName(ScaleWorkerPhase phase) {
  switch (phase) {
    case ScaleWorkerPhase::ADMIN: return "admin/power+coex";
    case ScaleWorkerPhase::LINK: return "link service";
    case ScaleWorkerPhase::DISCOVERY: return "discovery/scan";
    case ScaleWorkerPhase::COMMANDS: return "commands";
    case ScaleWorkerPhase::TELEMETRY: return "watchdog+telemetry";
    case ScaleWorkerPhase::COUNT: break;
  }
  return "unknown";
}

inline const char *scaleWorkerPhaseNameFor(uint8_t index) {
  return index < SCALE_WORKER_PHASE_COUNT
             ? scaleWorkerPhaseName(static_cast<ScaleWorkerPhase>(index))
             : "unknown";
}

struct LoopPhaseProfilerRow {
  const char *name = nullptr;
  uint32_t sampleCount = 0;
  uint32_t averageExecutionUs = 0;
  uint32_t maxExecutionUs = 0;
  // Own-CPU time of the very iteration that set maxExecutionUs, so
  // preemption at the maximum is derivable by subtraction.
  uint32_t maxCpuUs = 0;
  // Uptime (ms) at the start of the iteration that set maxExecutionUs, so a
  // retained peak can be matched against log timestamps.
  uint32_t maxAtMs = 0;
  uint32_t lastExecutionUs = 0;
  uint32_t peakGapExecutionUs = 0;
  uint32_t recentGapExecutionUs = 0;
  float currentCpuPct = 0.0f;
  float averageCpuPct = 0.0f;
};

template <uint8_t PHASE_COUNT>
struct PhaseProfilerSnapshot {
  uint8_t rowCount = 0;
  uint32_t peakGapMs = 0;
  uint32_t recentGapMs = 0;
  uint32_t peakGapUs = 0;
  uint32_t recentGapUs = 0;
  // Loop-task CPU and loop-core idle time inside the peak gap.
  uint32_t peakGapCpuUs = 0;
  uint32_t peakGapIdleUs = 0;
  uint32_t peakGapAtMs = 0;  // Uptime at the loop start that opened the peak gap.
  uint32_t peakDelayUs = 0;
  uint32_t recentDelayUs = 0;
  uint32_t peakDispatchUs = 0;
  uint32_t recentDispatchUs = 0;
  LoopPhaseProfilerRow rows[PHASE_COUNT] = {};
};

using LoopPhaseProfilerSnapshot = PhaseProfilerSnapshot<LOOP_PHASE_COUNT>;

template <uint8_t PHASE_COUNT>
class PhaseProfiler {
 public:
  explicit PhaseProfiler(const char *(*nameFor)(uint8_t) = loopPhaseNameFor)
      : nameFor_(nameFor) {}
  void beginIteration(bool enabled, uint32_t nowUs, uint32_t nowMs = 0) {
    memset(iterationUs_, 0, sizeof(iterationUs_));
    iterationAtMs_ = nowMs;
    if (enabled && !active_) {
      reset_(nowUs);
    } else if (!enabled && active_) {
      publish_(nowUs);
      active_ = false;
    }
  }

  bool active() const { return active_; }

  template <typename Phase>
  void record(Phase phase, uint32_t durationUs, uint32_t nowUs) {
    record(phase, durationUs, nowUs, durationUs);
  }

  // cpuUs is the acquiring task's own-CPU delta over the same interval
  // (runtime-stats clock); it defaults to the wall duration for callers
  // without a counter read. A new wall maximum stores the paired cpuUs.
  template <typename Phase>
  void record(Phase phase, uint32_t durationUs, uint32_t nowUs,
              uint32_t cpuUs) {
    const uint8_t index = static_cast<uint8_t>(phase);
    if (index >= PHASE_COUNT) return;
    iterationUs_[index] = durationUs;
    lastUs_[index] = durationUs;
    if (durationUs > maxUs_[index]) {
      maxUs_[index] = durationUs;
      // The kernel folds own-CPU at switch-out, so a delta can carry time
      // from earlier phases (cpuUs > wall); clamp so wall − cpu stays a
      // conservative lower bound of preemption.
      maxCpuUs_[index] = cpuUs > durationUs ? durationUs : cpuUs;
      maxAtMs_[index] = iterationAtMs_;
    }
    if (active_) {
      totalsUs_[index] += durationUs;
      windowUs_[index] += durationUs;
      ++sampleCounts_[index];
    }
    if (static_cast<uint32_t>(nowUs - windowStartedAtUs_) >= 1000000U) {
      publish_(nowUs);
    }
  }

  void requestReset() { resetRequested_.store(true, std::memory_order_release); }

  bool consumeReset() {
    if (!resetRequested_.exchange(false, std::memory_order_acq_rel)) return false;
    memset(maxUs_, 0, sizeof(maxUs_));
    memset(maxCpuUs_, 0, sizeof(maxCpuUs_));
    memset(maxAtMs_, 0, sizeof(maxAtMs_));
    memset(peakGapUs_, 0, sizeof(peakGapUs_));
    peakGapMs_ = 0;
    peakGapDurationUs_ = 0;
    peakGapCpuUs_ = peakGapIdleUs_ = peakGapAtMs_ = 0;
    peakDelayUs_ = 0;
    peakDispatchUs_ = 0;
    TaskLockGuard lock(snapshotMutex_);
    snapshot_.peakGapMs = 0;
    snapshot_.peakGapUs = 0;
    snapshot_.peakGapCpuUs = snapshot_.peakGapIdleUs = snapshot_.peakGapAtMs = 0;
    snapshot_.peakDelayUs = 0;
    snapshot_.peakDispatchUs = 0;
    for (LoopPhaseProfilerRow &row : snapshot_.rows) {
      row.maxExecutionUs = 0;
      row.maxCpuUs = 0;
      row.maxAtMs = 0;
      row.peakGapExecutionUs = 0;
    }
    return true;
  }

  void capturePeakGap(uint32_t gapMs, uint32_t gapUs, uint32_t delayUs,
                      uint32_t dispatchUs, uint32_t cpuUs = 0,
                      uint32_t idleUs = 0) {
    memcpy(peakGapUs_, iterationUs_, sizeof(peakGapUs_));
    peakGapMs_ = gapMs;
    peakGapDurationUs_ = gapUs;
    peakGapCpuUs_ = cpuUs;
    peakGapIdleUs_ = idleUs;
    peakGapAtMs_ = iterationAtMs_;  // The gap opened at the previous iteration's start.
    peakDelayUs_ = delayUs;
    peakDispatchUs_ = dispatchUs;
  }

  void captureIntervalGap(uint32_t gapUs, uint32_t delayUs,
                          uint32_t dispatchUs) {
    memcpy(intervalGapUs_, iterationUs_, sizeof(intervalGapUs_));
    intervalDelayUs_ = delayUs;
    intervalDispatchUs_ = dispatchUs;
    intervalGapDurationUs_ = gapUs;
  }

  void publishIntervalGap(uint32_t gapMs) {
    memcpy(recentGapUs_, intervalGapUs_, sizeof(recentGapUs_));
    memset(intervalGapUs_, 0, sizeof(intervalGapUs_));
    recentGapMs_ = gapMs;
    recentGapDurationUs_ = intervalGapDurationUs_;
    recentDelayUs_ = intervalDelayUs_;
    recentDispatchUs_ = intervalDispatchUs_;
    TaskLockGuard lock(snapshotMutex_);
    snapshot_.recentGapMs = gapMs;
    snapshot_.recentGapUs = recentGapDurationUs_;
    snapshot_.recentDelayUs = recentDelayUs_;
    snapshot_.recentDispatchUs = recentDispatchUs_;
    for (uint8_t i = 0; i < PHASE_COUNT; ++i) {
      snapshot_.rows[i].recentGapExecutionUs = recentGapUs_[i];
    }
  }

  void copySnapshot(PhaseProfilerSnapshot<PHASE_COUNT> &out) const {
    TaskLockGuard lock(snapshotMutex_);
    out = snapshot_;
  }

 private:
  void reset_(uint32_t nowUs) {
    memset(totalsUs_, 0, sizeof(totalsUs_));
    memset(windowUs_, 0, sizeof(windowUs_));
    memset(sampleCounts_, 0, sizeof(sampleCounts_));
    startedAtUs_ = nowUs;
    windowStartedAtUs_ = nowUs;
    active_ = true;
    publish_(nowUs);
  }

  void publish_(uint32_t nowUs) {
    const uint32_t elapsedUs = nowUs - startedAtUs_;
    const uint32_t windowElapsedUs = nowUs - windowStartedAtUs_;
    PhaseProfilerSnapshot<PHASE_COUNT> next;
    if (!active_) copySnapshot(next);
    next.rowCount = PHASE_COUNT;
    next.peakGapMs = peakGapMs_;
    next.recentGapMs = recentGapMs_;
    next.peakGapUs = peakGapDurationUs_;
    next.peakGapCpuUs = peakGapCpuUs_;
    next.peakGapIdleUs = peakGapIdleUs_;
    next.peakGapAtMs = peakGapAtMs_;
    next.recentGapUs = recentGapDurationUs_;
    next.peakDelayUs = peakDelayUs_;
    next.recentDelayUs = recentDelayUs_;
    next.peakDispatchUs = peakDispatchUs_;
    next.recentDispatchUs = recentDispatchUs_;
    for (uint8_t i = 0; i < PHASE_COUNT; ++i) {
      LoopPhaseProfilerRow &row = next.rows[i];
      row.name = nameFor_(i);
      row.lastExecutionUs = lastUs_[i];
      row.peakGapExecutionUs = peakGapUs_[i];
      row.recentGapExecutionUs = recentGapUs_[i];
      row.maxExecutionUs = maxUs_[i];
      row.maxCpuUs = maxCpuUs_[i];
      row.maxAtMs = maxAtMs_[i];
      if (!active_) continue;
      row.sampleCount = sampleCounts_[i];
      row.averageExecutionUs = sampleCounts_[i] == 0
                                   ? 0
                                   : static_cast<uint32_t>(totalsUs_[i] /
                                                           sampleCounts_[i]);
      row.currentCpuPct = windowElapsedUs == 0
                              ? 0.0f
                              : static_cast<float>(windowUs_[i]) * 100.0f /
                                    static_cast<float>(windowElapsedUs);
      row.averageCpuPct = elapsedUs == 0
                              ? 0.0f
                              : static_cast<float>(totalsUs_[i]) * 100.0f /
                                    static_cast<float>(elapsedUs);
      windowUs_[i] = 0;
    }
    windowStartedAtUs_ = nowUs;
    TaskLockGuard lock(snapshotMutex_);
    snapshot_ = next;
  }

  bool active_ = false;
  uint32_t startedAtUs_ = 0;
  uint32_t windowStartedAtUs_ = 0;
  uint64_t totalsUs_[PHASE_COUNT] = {};
  uint32_t windowUs_[PHASE_COUNT] = {};
  uint32_t sampleCounts_[PHASE_COUNT] = {};
  uint32_t maxUs_[PHASE_COUNT] = {};
  uint32_t maxCpuUs_[PHASE_COUNT] = {};
  uint32_t maxAtMs_[PHASE_COUNT] = {};
  uint32_t iterationAtMs_ = 0;
  uint32_t peakGapAtMs_ = 0;
  uint32_t lastUs_[PHASE_COUNT] = {};
  uint32_t iterationUs_[PHASE_COUNT] = {};
  uint32_t peakGapUs_[PHASE_COUNT] = {};
  uint32_t intervalGapUs_[PHASE_COUNT] = {};
  uint32_t recentGapUs_[PHASE_COUNT] = {};
  uint32_t peakGapMs_ = 0;
  uint32_t recentGapMs_ = 0;
  uint32_t peakGapDurationUs_ = 0;
  uint32_t peakGapCpuUs_ = 0;
  uint32_t peakGapIdleUs_ = 0;
  uint32_t recentGapDurationUs_ = 0;
  uint32_t intervalGapDurationUs_ = 0;
  uint32_t peakDelayUs_ = 0;
  uint32_t recentDelayUs_ = 0;
  uint32_t peakDispatchUs_ = 0;
  uint32_t recentDispatchUs_ = 0;
  uint32_t intervalDelayUs_ = 0;
  uint32_t intervalDispatchUs_ = 0;
  std::atomic<bool> resetRequested_{false};
  const char *(*nameFor_)(uint8_t);
  mutable TaskMutex snapshotMutex_;
  PhaseProfilerSnapshot<PHASE_COUNT> snapshot_ = {};
};

using LoopPhaseProfiler = PhaseProfiler<LOOP_PHASE_COUNT>;

// Last/max acquisition wait for a blocking mutex, updated from the acquiring
// task; read by the task-profiler serializer via the snapshot fields below.
inline void noteMutexWaitUs(std::atomic<uint32_t> &lastUs,
                            std::atomic<uint32_t> &maxUs, uint32_t waitedUs) {
  lastUs.store(waitedUs, std::memory_order_relaxed);
  uint32_t known = maxUs.load(std::memory_order_relaxed);
  while (waitedUs > known &&
         !maxUs.compare_exchange_weak(known, waitedUs,
                                      std::memory_order_relaxed)) {
  }
}

enum class TaskProfilerState : uint8_t { NEVER, RUNNING, STOPPED, FAILED };
enum class TaskProfilerStopReason : uint8_t {
  NONE,
  MANUAL,
  TIMEOUT,
  ALLOCATION_FAILED,
  CAPTURE_FAILED
};

inline const char *taskProfilerStateName(TaskProfilerState state) {
  switch (state) {
    case TaskProfilerState::NEVER: return "never";
    case TaskProfilerState::RUNNING: return "running";
    case TaskProfilerState::STOPPED: return "stopped";
    case TaskProfilerState::FAILED: return "failed";
  }
  return "failed";
}

inline const char *taskProfilerStopReasonName(TaskProfilerStopReason reason) {
  switch (reason) {
    case TaskProfilerStopReason::NONE: return "none";
    case TaskProfilerStopReason::MANUAL: return "manual";
    case TaskProfilerStopReason::TIMEOUT: return "timeout";
    case TaskProfilerStopReason::ALLOCATION_FAILED: return "allocation_failed";
    case TaskProfilerStopReason::CAPTURE_FAILED: return "capture_failed";
  }
  return "capture_failed";
}

struct TaskProfilerRow {
  char name[TASK_PROFILER_NAME_CAPACITY] = {};
  uint32_t taskNumber = 0;
  int8_t core = -1;
  uint32_t stackMinBytes = UINT32_MAX;
  float currentCpuPct = 0.0f;
  float averageCpuPct = 0.0f;
};

struct TaskProfilerSnapshot {
  TaskProfilerState state = TaskProfilerState::NEVER;
  TaskProfilerStopReason stopReason = TaskProfilerStopReason::NONE;
  uint32_t elapsedMs = 0;
  uint32_t remainingMs = 0;
  uint32_t sampleCount = 0;
  uint32_t lastCaptureUs = 0;
  uint32_t maxCaptureUs = 0;
  float currentTotalCpuPct = 0.0f;
  float averageTotalCpuPct = 0.0f;
  float unreportedCurrentCpuPct = 0.0f;
  float unreportedAverageCpuPct = 0.0f;
  bool truncated = false;
  uint8_t rowCount = 0;
  TaskProfilerRow rows[TASK_PROFILER_MAX_ROWS] = {};
  LoopPhaseProfilerSnapshot loopPhases = {};
  // Component sections of the scale worker tick, recorded by the worker on
  // the same profiler session as the loop phases.
  PhaseProfilerSnapshot<SCALE_WORKER_PHASE_COUNT> scaleWorkerPhases = {};
  // Mutex-acquisition waits observed on the control loop (Micra backflush
  // mux_, scale critical-event mutex, and the status publish's shot-store and
  // control-status acquisitions) across every acquiring phase, not just the
  // input sub-phases: last and observed-maximum wait time.
  uint32_t micraBackflushWaitLastUs = 0;
  uint32_t micraBackflushWaitMaxUs = 0;
  // How long the control task itself owned the Micra mutex inside the
  // backflush poll: the window the wait telemetry cannot see.
  uint32_t micraBackflushHoldLastUs = 0;
  uint32_t micraBackflushHoldMaxUs = 0;
  uint32_t scaleEventsWaitLastUs = 0;
  uint32_t scaleEventsWaitMaxUs = 0;
  uint32_t shotStoreWaitLastUs = 0;
  uint32_t shotStoreWaitMaxUs = 0;
  uint32_t controlStatusWaitLastUs = 0;
  uint32_t controlStatusWaitMaxUs = 0;
};

class TaskProfiler {
 public:
  // The published report (~2.5 KiB) is copied under reportMutex_ by tasks
  // only, so its storage may live in PSRAM BSS (zeroed at boot: zero state is
  // NEVER). The mutex, the loop-phase flag and the scalars stay in this
  // internal object.
  explicit TaskProfiler(TaskProfilerSnapshot &report) : report_(report) {}
  ~TaskProfiler() { releaseWorkspace_(); }

  bool start(uint32_t nowMs) {
    if (report_.state == TaskProfilerState::RUNNING) {
      return false;
    }
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    ActiveWorkspace *next = static_cast<ActiveWorkspace *>(
        allocExternal(sizeof(ActiveWorkspace)));
    if (next == nullptr) {
      noteStartFailure_(TaskProfilerStopReason::ALLOCATION_FAILED);
      return false;
    }
    memset(next, 0, sizeof(*next));
    // uxTaskGetSystemState writes its output while holding the kernel lock.
    next->capture = static_cast<TaskStatus_t *>(
        allocInternal(sizeof(TaskStatus_t) * TASK_PROFILER_MAX_TRACKED));
    if (next->capture == nullptr) {
      heapCapsFree(next);
      noteStartFailure_(TaskProfilerStopReason::ALLOCATION_FAILED);
      return false;
    }
    uint32_t ignoredTotal = 0;
    const int64_t captureStartedUs = esp_timer_get_time();
    const UBaseType_t count = uxTaskGetSystemState(
        next->capture, TASK_PROFILER_MAX_TRACKED, &ignoredTotal);
    const int64_t captureEndedUs = esp_timer_get_time();
    if (count == 0 || uxTaskGetNumberOfTasks() > TASK_PROFILER_MAX_TRACKED) {
      heapCapsFree(next->capture);
      heapCapsFree(next);
      noteStartFailure_(TaskProfilerStopReason::CAPTURE_FAILED);
      return false;
    }
    next->trackedCount = static_cast<uint8_t>(count);
    for (UBaseType_t index = 0; index < count; ++index) {
      seedTracked_(next->tracked[index], next->capture[index]);
    }
    releaseWorkspace_();
    workspace_ = next;
    beginSnapshotWrite_();
    report_ = TaskProfilerSnapshot{};
    report_.state = TaskProfilerState::RUNNING;
    startedAtMs_ = nowMs;
    startedAtUs_ = captureEndedUs;
    lastCaptureAtMs_ = nowMs;
    lastCaptureAtUs_ = captureEndedUs;
    intervalUs_ = 0;
    const uint64_t initialCost =
        captureEndedUs > captureStartedUs ? captureEndedUs - captureStartedUs : 0;
    report_.lastCaptureUs = clampU32_(initialCost);
    report_.maxCaptureUs = report_.lastCaptureUs;
    refreshReport_(nowMs);
    endSnapshotWrite_();
    loopPhaseEnabled_.store(true, std::memory_order_release);
    return true;
#else
    (void)nowMs;
    beginSnapshotWrite_();
    report_.state = TaskProfilerState::FAILED;
    report_.stopReason = TaskProfilerStopReason::CAPTURE_FAILED;
    endSnapshotWrite_();
    return false;
#endif
  }

  bool stop(uint32_t nowMs) {
    if (report_.state != TaskProfilerState::RUNNING) {
      return false;
    }
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    if (static_cast<uint32_t>(nowMs - lastCaptureAtMs_) >= 100U) {
      sample_(nowMs);
    }
    if (report_.state == TaskProfilerState::RUNNING) {
      finish_(TaskProfilerState::STOPPED, TaskProfilerStopReason::MANUAL,
              nowMs);
    }
#else
    (void)nowMs;
#endif
    return true;
  }

  void service(uint32_t nowMs) {
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
    if (report_.state != TaskProfilerState::RUNNING) {
      return;
    }
    if (static_cast<uint32_t>(nowMs - startedAtMs_) >=
        TASK_PROFILER_MAX_DURATION_MS) {
      sample_(nowMs);
      if (report_.state == TaskProfilerState::RUNNING) {
        finish_(TaskProfilerState::STOPPED, TaskProfilerStopReason::TIMEOUT,
                nowMs);
      }
      return;
    }
    if (static_cast<uint32_t>(nowMs - lastCaptureAtMs_) >=
        TASK_PROFILER_SAMPLE_INTERVAL_MS) {
      sample_(nowMs);
    }
#else
    (void)nowMs;
#endif
  }

  bool running() const {
    TaskLockGuard lock(reportMutex_);
    return report_.state == TaskProfilerState::RUNNING;
  }
  bool loopPhaseProfilingEnabled() const {
    return loopPhaseEnabled_.load(std::memory_order_acquire);
  }
  TaskProfilerSnapshot snapshot() const {
    TaskProfilerSnapshot out;
    copySnapshot(out);
    return out;
  }

  void copySnapshot(TaskProfilerSnapshot &out) const {
    TaskLockGuard lock(reportMutex_);
    out = report_;
  }

#if defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  void resetForHost() {
    TaskLockGuard lock(reportMutex_);
    report_ = TaskProfilerSnapshot{};
  }

  void publishForHost(const TaskProfilerSnapshot &next) {
    TaskLockGuard lock(reportMutex_);
    report_ = next;
  }
#endif

 private:
  void beginSnapshotWrite_() {
    reportMutex_.lock();
  }
  void endSnapshotWrite_() {
    reportMutex_.unlock();
  }
#if defined(ARDUINO) && !defined(OPEN_BREW_BY_WEIGHT_HOST_TEST)
  struct TrackedTask {
    char name[TASK_PROFILER_NAME_CAPACITY] = {};
    uint32_t taskNumber = 0;
    uint32_t previousCounter = 0;
    uint64_t accumulatedCounter = 0;
    uint32_t currentDelta = 0;
    uint32_t stackMinBytes = UINT32_MAX;
    int8_t core = -1;
    bool idle = false;
  };

  struct ActiveWorkspace {
    TaskStatus_t *capture = nullptr;
    TrackedTask tracked[TASK_PROFILER_MAX_TRACKED] = {};
    uint8_t trackedCount = 0;
  };
  static_assert(sizeof(ActiveWorkspace) <= 4096,
                "Profiler external workspace exceeds its budget");
  static_assert(sizeof(TaskStatus_t) * TASK_PROFILER_MAX_TRACKED <= 4096,
                "Profiler kernel capture exceeds its internal budget");

  static uint32_t clampU32_(uint64_t value) {
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
  }

  void noteStartFailure_(TaskProfilerStopReason reason) {
    loopPhaseEnabled_.store(false, std::memory_order_release);
    beginSnapshotWrite_();
    if (report_.state == TaskProfilerState::NEVER) {
      report_.state = TaskProfilerState::FAILED;
      report_.stopReason = reason;
    }
    endSnapshotWrite_();
  }

  static bool idleTaskName_(const char *name) {
    return name != nullptr &&
           (strcmp(name, "IDLE0") == 0 || strcmp(name, "IDLE1") == 0 ||
            strcmp(name, "IDLE") == 0);
  }

  static int8_t coreFor_(BaseType_t core) {
    return core == 0 || core == 1 ? static_cast<int8_t>(core) : int8_t{-1};
  }

  // TaskStatus_t only has a core field when FREERTOS_VTASKLIST_INCLUDE_COREID
  // is on. xTaskGetCoreID is always present on ESP-IDF. A deleted task's TCB
  // (handle and name) may be freed by IDLE after the snapshot: never read it.
  static int8_t coreOf_(const TaskStatus_t &task) {
    return task.eCurrentState == eDeleted
               ? int8_t{-1}
               : coreFor_(xTaskGetCoreID(task.xHandle));
  }

  static void seedTracked_(TrackedTask &tracked, const TaskStatus_t &task) {
    tracked = TrackedTask{};
    tracked.taskNumber = static_cast<uint32_t>(task.xTaskNumber);
    tracked.previousCounter = static_cast<uint32_t>(task.ulRunTimeCounter);
    tracked.stackMinBytes = static_cast<uint32_t>(task.usStackHighWaterMark);
    tracked.core = coreOf_(task);
    if (task.pcTaskName != nullptr && task.eCurrentState != eDeleted) {
      copyCString(tracked.name, sizeof(tracked.name), task.pcTaskName);
    }
    tracked.idle = idleTaskName_(tracked.name);
  }

  TrackedTask *findTracked_(uint32_t taskNumber) {
    for (uint8_t index = 0; index < workspace_->trackedCount; ++index) {
      if (workspace_->tracked[index].taskNumber == taskNumber) {
        return &workspace_->tracked[index];
      }
    }
    return nullptr;
  }

  void sample_(uint32_t nowMs) {
    if (workspace_ == nullptr) {
      finish_(TaskProfilerState::FAILED,
              TaskProfilerStopReason::CAPTURE_FAILED, nowMs);
      return;
    }
    uint32_t ignoredTotal = 0;
    const int64_t captureStartedUs = esp_timer_get_time();
    const UBaseType_t count = uxTaskGetSystemState(
        workspace_->capture, TASK_PROFILER_MAX_TRACKED, &ignoredTotal);
    const int64_t captureEndedUs = esp_timer_get_time();
    if (count == 0) {
      finish_(TaskProfilerState::FAILED,
              TaskProfilerStopReason::CAPTURE_FAILED, nowMs);
      return;
    }
    const uint64_t intervalUs = captureEndedUs > lastCaptureAtUs_
                                    ? captureEndedUs - lastCaptureAtUs_
                                    : 0;
    if (intervalUs == 0) {
      return;
    }
    for (uint8_t index = 0; index < workspace_->trackedCount; ++index) {
      workspace_->tracked[index].currentDelta = 0;
    }
    bool truncated = uxTaskGetNumberOfTasks() > TASK_PROFILER_MAX_TRACKED;
    for (UBaseType_t index = 0; index < count; ++index) {
      const TaskStatus_t &task = workspace_->capture[index];
      const uint32_t taskNumber = static_cast<uint32_t>(task.xTaskNumber);
      TrackedTask *tracked = findTracked_(taskNumber);
      if (tracked == nullptr) {
        if (workspace_->trackedCount >= TASK_PROFILER_MAX_TRACKED) {
          truncated = true;
          continue;
        }
        tracked = &workspace_->tracked[workspace_->trackedCount++];
        seedTracked_(*tracked, task);
        continue;
      }
      const uint32_t counter = static_cast<uint32_t>(task.ulRunTimeCounter);
      const uint32_t delta = counter - tracked->previousCounter;
      tracked->previousCounter = counter;
      tracked->currentDelta = delta;
      tracked->accumulatedCounter += delta;
      const uint32_t stackMin =
          static_cast<uint32_t>(task.usStackHighWaterMark);
      if (stackMin < tracked->stackMinBytes) {
        tracked->stackMinBytes = stackMin;
      }
      tracked->core = coreOf_(task);
    }
    intervalUs_ = intervalUs;
    lastCaptureAtUs_ = captureEndedUs;
    lastCaptureAtMs_ = nowMs;
    beginSnapshotWrite_();
    ++report_.sampleCount;
    report_.truncated = report_.truncated || truncated;
    report_.lastCaptureUs = clampU32_(
        captureEndedUs > captureStartedUs ? captureEndedUs - captureStartedUs
                                          : 0);
    if (report_.lastCaptureUs > report_.maxCaptureUs) {
      report_.maxCaptureUs = report_.lastCaptureUs;
    }
    refreshReport_(nowMs);
    endSnapshotWrite_();
  }

  void refreshReport_(uint32_t nowMs) {
    if (workspace_ == nullptr) {
      return;
    }
    report_.elapsedMs = static_cast<uint32_t>(nowMs - startedAtMs_);
    report_.remainingMs =
        report_.elapsedMs >= TASK_PROFILER_MAX_DURATION_MS
            ? 0U
            : TASK_PROFILER_MAX_DURATION_MS - report_.elapsedMs;
    report_.currentTotalCpuPct = 0.0f;
    report_.averageTotalCpuPct = 0.0f;
    report_.unreportedCurrentCpuPct = 0.0f;
    report_.unreportedAverageCpuPct = 0.0f;
    report_.rowCount = 0;
    memset(report_.rows, 0, sizeof(report_.rows));

    const uint64_t elapsedUs = lastCaptureAtUs_ > startedAtUs_
                                   ? lastCaptureAtUs_ - startedAtUs_
                                   : 0;
    for (uint8_t index = 0; index < workspace_->trackedCount; ++index) {
      const TrackedTask &tracked = workspace_->tracked[index];
      if (tracked.idle) {
        continue;
      }
      TaskProfilerRow row;
      copyCString(row.name, sizeof(row.name), tracked.name);
      row.taskNumber = tracked.taskNumber;
      row.core = tracked.core;
      row.stackMinBytes = tracked.stackMinBytes;
      row.currentCpuPct = intervalUs_ == 0
                              ? 0.0f
                              : static_cast<float>(tracked.currentDelta) *
                                    100.0f / static_cast<float>(intervalUs_);
      row.averageCpuPct = elapsedUs == 0
                              ? 0.0f
                              : static_cast<float>(tracked.accumulatedCounter) *
                                    100.0f / static_cast<float>(elapsedUs);
      report_.currentTotalCpuPct += row.currentCpuPct;
      report_.averageTotalCpuPct += row.averageCpuPct;

      const float rank = report_.state == TaskProfilerState::RUNNING
                             ? row.currentCpuPct
                             : row.averageCpuPct;
      size_t insertAt = report_.rowCount;
      while (insertAt > 0) {
        const TaskProfilerRow &previous = report_.rows[insertAt - 1];
        const float previousRank =
            report_.state == TaskProfilerState::RUNNING
                ? previous.currentCpuPct
                : previous.averageCpuPct;
        if (previousRank >= rank) {
          break;
        }
        --insertAt;
      }
      if (insertAt >= TASK_PROFILER_MAX_ROWS) {
        continue;
      }
      const size_t moveEnd = report_.rowCount < TASK_PROFILER_MAX_ROWS
                                 ? report_.rowCount
                                 : TASK_PROFILER_MAX_ROWS - 1;
      for (size_t move = moveEnd; move > insertAt; --move) {
        report_.rows[move] = report_.rows[move - 1];
      }
      report_.rows[insertAt] = row;
      if (report_.rowCount < TASK_PROFILER_MAX_ROWS) {
        ++report_.rowCount;
      }
    }
    float shownCurrent = 0.0f;
    float shownAverage = 0.0f;
    for (uint8_t index = 0; index < report_.rowCount; ++index) {
      shownCurrent += report_.rows[index].currentCpuPct;
      shownAverage += report_.rows[index].averageCpuPct;
    }
    report_.unreportedCurrentCpuPct =
        report_.currentTotalCpuPct > shownCurrent
            ? report_.currentTotalCpuPct - shownCurrent
            : 0.0f;
    report_.unreportedAverageCpuPct =
        report_.averageTotalCpuPct > shownAverage
            ? report_.averageTotalCpuPct - shownAverage
            : 0.0f;
    if (workspace_->trackedCount > TASK_PROFILER_MAX_ROWS) {
      report_.truncated = true;
    }
  }

  void finish_(TaskProfilerState state, TaskProfilerStopReason reason,
               uint32_t nowMs) {
    loopPhaseEnabled_.store(false, std::memory_order_release);
    beginSnapshotWrite_();
    refreshReport_(nowMs);
    report_.state = state;
    report_.stopReason = reason;
    report_.remainingMs = 0;
    // Stopped reports are easier to read when ranked by the session average.
    if (workspace_ != nullptr) {
      refreshReport_(nowMs);
    }
    endSnapshotWrite_();
    releaseWorkspace_();
  }

  void releaseWorkspace_() {
    if (workspace_ != nullptr) {
      heapCapsFree(workspace_->capture);
      heapCapsFree(workspace_);
      workspace_ = nullptr;
    }
  }

  ActiveWorkspace *workspace_ = nullptr;
  uint64_t startedAtUs_ = 0;
  uint64_t lastCaptureAtUs_ = 0;
  uint64_t intervalUs_ = 0;
  uint32_t startedAtMs_ = 0;
  uint32_t lastCaptureAtMs_ = 0;
#else
  void releaseWorkspace_() {}
#endif

  TaskProfilerSnapshot &report_;
  std::atomic<bool> loopPhaseEnabled_{false};
  mutable TaskMutex reportMutex_;
};

}  // namespace openbrewbyweight

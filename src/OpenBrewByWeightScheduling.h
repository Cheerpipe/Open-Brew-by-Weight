#pragma once

#include <stddef.h>
#include <stdint.h>

namespace openbrewbyweight {

// Qualification contract for the two hard real-time-adjacent application
// loops. The nominal cadence is 1 ms while a scale is active; the 10 ms
// service deadline bounds lateness beyond each loop's own requested sleep,
// leaving radio/flash jitter headroom while remaining far below every control
// safety timeout. Deadline misses are monotonic and exported in the coherent
// ControlStatus snapshot.
constexpr uint32_t CONTROL_SERVICE_DEADLINE_MS = 10;
constexpr uint32_t SCALE_SERVICE_DEADLINE_MS = 10;
constexpr uint32_t CONTROL_HOUSEKEEPING_INTERVAL_MS = 10;
constexpr uint32_t CONTROL_EXECUTION_BUDGET_US = 9000;
constexpr uint32_t SCALE_EXECUTION_BUDGET_US = 9000;
// The 1 ms loops feed the 5 s Task Watchdog at this cadence instead of every
// activation: esp_task_wdt_reset takes the TWDT spinlock and walks the
// subscribed-task list, so per-millisecond feeding adds no protection. A
// failed feed is reported within this interval; the TWDT remains the backstop.
constexpr uint32_t TASK_WATCHDOG_FEED_INTERVAL_MS = 100;
constexpr uint32_t TASK_BLOCKING_UNBOUNDED_MS = UINT32_MAX;
// Priorities above idle on core 1: control preempts its own scale producer so
// relay/safety service never waits behind a BLE worker time slice.
constexpr uint8_t CONTROL_TASK_PRIORITY_OFFSET = 2;
constexpr uint8_t SCALE_WORKER_TASK_PRIORITY_OFFSET = 1;
// Telemetry shares the core 0 best-effort priority so CPU and heap sampling
// keeps running while httpd/network_manager saturate core 0.
constexpr uint8_t HEALTH_TASK_PRIORITY_OFFSET = 1;

enum class TaskActivation : uint8_t {
  PERIODIC,
  EVENT_DRIVEN,
  FRAMEWORK
};

struct TaskScheduleContract {
  const char *name;
  int8_t core;
  uint8_t priorityOffset;
  uint32_t configuredStackBytes;
  TaskActivation activation;
  uint32_t nominalPeriodMs;
  uint32_t serviceDeadlineMs;
  uint32_t executionBudgetUs;
  uint32_t maxBlockingMs;
  uint32_t watchdogDeadlineMs;
  bool watchdogSubscribed;
};

constexpr TaskScheduleContract TASK_SCHEDULE_CONTRACTS[] = {
    {"control", 1, CONTROL_TASK_PRIORITY_OFFSET, 8192,
     TaskActivation::PERIODIC, 1,
     CONTROL_SERVICE_DEADLINE_MS, CONTROL_EXECUTION_BUDGET_US, 50, 5000, true},
    {"scale_worker", 1, SCALE_WORKER_TASK_PRIORITY_OFFSET, 6656,
     TaskActivation::PERIODIC, 1,
     SCALE_SERVICE_DEADLINE_MS, SCALE_EXECUTION_BUDGET_US, 3000, 5000, true},
    {"settings_persist", 0, 0, 5120, TaskActivation::EVENT_DRIVEN, 0, 1000,
     0, 3000, 5000, true},
    {"health", 0, HEALTH_TASK_PRIORITY_OFFSET, 4096, TaskActivation::PERIODIC,
     100, 0, 0, 0, 0, false},
    {"network_manager", 0, 1, 10240, TaskActivation::PERIODIC, 50, 250,
     200000, 2500, 5000, true},
    {"httpd", 0, 1, 11264, TaskActivation::FRAMEWORK, 0, 0, 0, 30000, 0,
     false},
    {"webhook", 0, 0, 4608, TaskActivation::EVENT_DRIVEN, 0, 0, 0, 1800, 0,
     false},
    {"micra_cloud", 0, 0, 8192, TaskActivation::EVENT_DRIVEN, 0, 0, 0,
     10000, 0, false},
    {"serial_log", 0, 0, 3072, TaskActivation::EVENT_DRIVEN, 0, 0, 0,
     TASK_BLOCKING_UNBOUNDED_MS, 0, false},
    // Transient: exists only while OTA requests are queued.
    {"ota_worker", 0, 1, 11264, TaskActivation::EVENT_DRIVEN, 0, 0, 0, 30000,
     0, false},
};

constexpr size_t TASK_SCHEDULE_CONTRACT_COUNT =
    sizeof(TASK_SCHEDULE_CONTRACTS) / sizeof(TASK_SCHEDULE_CONTRACTS[0]);

static_assert(CONTROL_TASK_PRIORITY_OFFSET > SCALE_WORKER_TASK_PRIORITY_OFFSET,
              "control must preempt the scale worker on their shared core");
static_assert(CONTROL_SERVICE_DEADLINE_MS < 200,
              "control deadline must precede health warning threshold");
static_assert(CONTROL_HOUSEKEEPING_INTERVAL_MS <= CONTROL_SERVICE_DEADLINE_MS,
              "control housekeeping must fit the control deadline");
static_assert(SCALE_SERVICE_DEADLINE_MS < 250,
              "scale deadline must precede stale-link detection");
static_assert(CONTROL_EXECUTION_BUDGET_US <
                  CONTROL_SERVICE_DEADLINE_MS * 1000U,
              "control execution budget must fit service deadline");
static_assert(SCALE_EXECUTION_BUDGET_US < SCALE_SERVICE_DEADLINE_MS * 1000U,
              "scale execution budget must fit service deadline");
static_assert(TASK_WATCHDOG_FEED_INTERVAL_MS * 10 < 5000,
              "watchdog feed cadence must keep a 10x margin to the 5 s TWDT");

}  // namespace openbrewbyweight

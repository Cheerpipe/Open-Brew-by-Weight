# Scheduling qualification

The authoritative task contract is `OpenBrewByWeightScheduling.h`. It fixes core,
priority, nominal period, service deadline and Task Watchdog participation for
every application task. A task without a periodic deadline is event-driven or
diagnostic and must never execute on a safety path.

## Core topology

The network stack is pinned to core 0 by build configuration
(`idf/sdkconfig.defaults`): the Wi-Fi driver task, the lwIP `tcpip` task,
`esp_timer`, the FreeRTOS timer service task and Arduino's `arduino_events`
network event task all run on core 0, and the firmware build fails if the
effective configuration moves either of the last two elsewhere, so an RF-storm burst cannot preempt the
control and scale loops on core 1. Core 0 therefore concentrates
every best-effort consumer (httpd, cloud integration, persistence, logging)
and may degrade first under radio pressure — by design; core 1 stays
exclusive to control, scale_worker and their IPC. Measured motive: the
2026-10-12 audit records of ~0.9 s control-loop stalls during beacon-timeout
storms, where the Micra mutex wait (902 ms) ran with a 1.8 ms holder tenure —
the loop itself had been preempted, and `tcpip` was the only high-priority
task without affinity.

On core 1 the control task runs at idle+2 and `scale_worker` at idle+1, so
control preempts its own scale producer instead of sharing time slices with
it: relay and safety service never waits behind a BLE worker step. The worker
still runs whenever control sleeps, which it does on every activation, and a
worker holding a task mutex that control needs inherits control's priority.

## Application task contract

`max block` is the largest explicit API timeout reachable in one activation;
it is not a measured WCET. `unbounded` is intentional for the isolated serial
sink: USB backpressure may block it forever, so it is neither watchdog
subscribed nor part of control. Stack values are configured bytes in ESP-IDF.

| Task | Activation / period | Deadline | Execution budget | Priority | Max block | Stack | Core | TWDT |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| control | periodic / 1 ms active | 10 ms | 9000 us | idle+2 | 50 ms maintenance flash take | 8192 | 1 | 5 s |
| scale_worker | notification / 1 ms connecting or unlinked queued command, 10 ms linked/idle | 10 ms | 9000 us | idle+1 | 3000 ms GATT step | 6656 | 1 | 5 s |
| settings_persist | event-driven | 1000 ms service | n/a | idle | 3000 ms lock acquisition; one flash primitive/step | 5120 | 0 | 5 s |
| health | periodic / 100 ms | diagnostic | n/a | idle+1 | none | 4096 | 0 | no |
| network_manager | periodic / 50 ms | 250 ms | 200000 us | idle+1 | 2500 ms lifecycle/cancel | 10240 | 0 | 5 s |
| mdns | event-driven (action queue) | n/a | n/a | 1 | freed once in network stop | 4096 | 0 | no |
| httpd | framework event | n/a | n/a | idle+1 | 2500 ms response workspace; 30000 ms OTA receive only when the worker cannot start | 11264 | 0 | no |
| ota_worker | transient, while OTA requests are queued | n/a | n/a | idle+1 | 30000 ms OTA receive budget | 11264 | 0 | no |
| webhook | event-driven | n/a | n/a | idle | 1800 ms HTTP | 4608 | 0 | no |
| micra_cloud | event-driven | n/a | n/a | idle | 10000 ms per HTTPS request | 8192 | 0 | no |
| websocket_task | SDK transport events | n/a | n/a | 1 | 10000 ms network timeout | 6144 | 0 | no |
| serial_log | event-driven | n/a | n/a | idle | unbounded USB sink | 3072 | 0 | no |

The control task keeps safety, machine sampling and actuation, brew guards,
scale-event drains, maintenance cancellation, Web commands, power transitions,
alerts, the compact remote-control gate, and watchdog service on every
activation. Administrative polling for the serial CLI, persistence completion,
diagnostic weight-stream state, the full Web status snapshot, and the scale
status LED runs at boot and then every 10 ms. This cadence does not change the
1 ms active control period or its 10 ms service deadline.

The 10 ms service deadline does not apply while `scale_worker` is executing an
explicit connection/discovery operation whose bounded step is listed above;
those paths are separately bounded by the 5 s TWDT. HIL qualification must
report active-link and connect/discovery distributions separately.

The optional Micra cloud service owns a low-priority core-0 worker so HTTPS and
signature work never enters the scale or control task bodies. HTTPS uses the
ESP-IDF asynchronous client contract: incomplete progress yields for one tick,
rechecks the network/scale/shot gates, and retains one absolute 10-second
operation deadline. The network manager publishes the current gate and wakes
the cloud worker in the same activation. Only the cloud worker accesses or
destroys its HTTP/TLS client; cancellation is checked before and after each
asynchronous progress call, so teardown cannot race an in-flight TLS read.
The API-mode observer is due nominally every 30 seconds, and its four-attempt cycle
waits 3, 6, then 9
seconds. After exhaustion API observation resumes its 30-second cadence;
WebSocket reconnection and command retries use a 60-second cooldown.
WebSocket failure backoff starts after cleanup or the failed setup operation
returns, so transport delays do not consume the following retry interval.
WebSocket selection suppresses regular polling. Each admitted subscription
queues one initial dashboard cycle with the same bounded retries, coalescing
compatible queued observations. Explicit offline-to-online recovery can queue
one resynchronization read. The SDK receiver runs independently on core 0,
priority 1, with a 6144-byte internal stack and fixed 1024-byte I/O buffers.
CONNECT/SUBSCRIBE and incomplete-frame progress each have a 10-second deadline.
Payload processing decodes each header once, accumulates the body in linear
time, admits at most 32 complete frames per feed and yields after publication.
The facade records validated-frame→publication and pause→actual stop latency.
The publication metric covers JSON decoding and facade publication; transport,
fragment assembly and STOMP decoding require separate callback timing in target
traces. Target qualification must establish those bounds under HTTP load.
An established socket follows owner stop; DNS/connect already in flight may
finish before SDK stop completes. BLE never waits for that completion.

Backflush processing drains at most eight transitions per control turn. A
published supervision loss opens on the next turn; silent transport loss is
detected no later than the 30-second pong/connection liveness budget plus
control scheduling latency. Pongs prove transport reachability, not machine
state freshness. The independent relay deadline remains 60 seconds until
qualified promotion, then 180 seconds from the original close, regardless of
worker or cloud progress. Measure these bounds on target under concurrent load.

AP, STA-loss, unsynchronized clock, scale acquisition/setup/quiet, and shot
activity gate every API purpose, including authentication and retries, without
consuming attempts while awaiting admission. Every gate must clear before work
resumes. These are admission intervals, not a guaranteed cloud
detection latency; an individual TLS/crypto progress call still requires target
deadline qualification.
Shot→rinse classification clears the shot gate immediately. WSS does not
participate in scale acquisition/setup/quiet admission.

The scale worker blocks on a task notification with a 1 ms timeout while
connecting or while unlinked with queued commands, and 10 ms while linked or
idle. Commands, policy changes and sound mailboxes notify it immediately. The
timeout remains the compatibility path for NimBLE frames and GAP/GATT state until
the backend
publishes its asynchronous wake edge; no protocol timeout depends solely on a
notification.

Each NimBLE activation drains at most the physical capacity of its receive,
critical-event, and control-event rings (16, 6, and 12 respectively). New work
arriving during a drain is deferred to the next activation; no
event is discarded and BLE sampling/poll cadence is unchanged.

## Runtime evidence

Optional [Power management](settings/power-management.md) changes only CPU
limits and owner-local radio policy; task priorities, watchdogs and safety
deadlines remain unchanged. Control evaluates demand once per loop and promotes
before accepted starts. PM configuration runs outside critical sections; stop
handling does not wait for a PM acknowledgement. The BLE owner explicitly wakes
the controller and defers BLE work while it is still asleep, without publishing
false worker progress. Failed awake restoration or a 100-ms wake timeout enters
the existing critical-task safety fault path. Qualify PM
apply time, 40/80/160-MHz service distributions and timer/buzzer timing under
M94–M98 in the [manual plan](MANUAL_TEST_PLAN.md). CPU percentages are diagnostic,
not the energy objective; electrical savings require supply measurements.

The coherent control-status snapshot publishes a monotonic version/timestamp,
the lifetime maximum control and scale-worker service gaps, monotonic deadline
miss counts, maximum observed loop-body execution in microseconds, and stack
high-water marks. A 10 ms deadline applies to both control and scale worker.
It bounds lateness beyond each task's own requested sleep: a service gap counts
as a miss only when it exceeds the delay the task asked for (1 or 5 ms for
control, the selected cadence or explicit backoff for the scale worker) by more
than 10 ms, so the idle cadences are not reported as misses.
The generalized persistence owner runs at idle priority on core 0,
blocks on its bounded queue and yields around flash work. Control only stages
an immutable generation and attempts a zero-wait enqueue. The health worker
runs on core 0, samples heap/CPU and services the optional task profiler, then
publishes a one-element latest-wins mailbox. Network and scale metrics are
published under their owning snapshot or as monotonic atomics; the scale
worker alone updates its maximum gap and execution metrics.

The control loop continuously records the last and maximum execution time for
coarse safety/health, four chronological input sub-phases (backflush poll,
drive permission, scale event drain, machine service),
machine-guards, control, alerts/timers, commands, diagnostics
and final-scale-drain phases. It also retains the phases from the iterations
preceding the largest start-to-start loop gaps since reset and within the
current health window. The latter snapshot is frozen and published with the
recent Loop gap value. Both gaps include scheduling and waiting beyond phase
execution. The breakdown captures the delay call, the dispatch time from its
return to the next loop start, and the remaining unphased work. The delay call
includes scheduler time until the task runs again; it does not identify which
task ran during that interval. To narrow that down, every `health loop gap
high` alert and the exported peak gap (`peakGapCpuUs`, `peakGapIdleUs`) carry
the loop task's own CPU time and the loop core's idle-task time inside the
gap, and the alert adds `flash=1` when the application flash I/O lock was held
during it. Loop CPU near the gap means the loop itself ran; idle near the gap
means the loop was blocked or sleeping; neither means another task on that
core, or a stalled core, held it. Because both the peak gap and each phase's
maximum are retained until reset, the export also stamps them with the uptime
at which they were recorded (`peakGapAtMs` for the loop start that opened the
peak gap; `maxAtMs` per phase row for the start of the iteration that set its
maximum), so a peak seen hours later can be matched against the log ring and
audit timestamps. Housekeeping has ten separate, non-overlapping
rows: serial log, serial CLI, settings persistence, history persistence, scale
identity persistence, weight telemetry, and LED service, plus the status
publication's three contiguous sub-phases — owner snapshot gather, store
copy, and lock+commit — which the publish itself records on its own timeline,
so a housekeeping pass that publishes nothing contributes no status rows.
History persistence includes selective immutable image capture before dispatch
to the flash worker: clean ring payloads are skipped, and curves copy only the
uncommitted suffix, at most four records per image, rather than the retained
bank. The shot-store persistence
service additionally skips its locked dirty check entirely while the store
generation matches the last image the flash worker acknowledged. These rows
record only on iterations that run the 10 ms-gated
block; skipped iterations contribute zero to their loop-gap breakdown.
The loop owns the counters; ordinary
phase records avoid locking, while one-second and health-window publication
briefly lock the diagnostic snapshot. The optional task profiler uses the same timing
calls for session CPU percentages and averages. Reset requests are consumed by
the loop owner and clear the historical loop-gap and per-phase maxima without
changing the recent gap, deadline, or health alert accounting. Phase times
include their own measurement cost. The full task-profiler export pairs each
phase's wall-time maximum with the own-CPU time of the iteration that set it
(the loop task's kernel-accounted runtime counter), so preemption at a
maximum is derivable by subtraction,
and reports the last and maximum mutex-acquisition wait recorded at the
instrumented control-loop sites: the Micra backflush poll and the scale
critical-event drain (other acquisitions of the same mutexes, such as the
machine-guards backflush poll or the diagnostics tare capture, are not
counted). The Micra status and backflush polls never wait: they acquire the
facade mutex with a zero-timeout take, and on contention serve the control
task's cached snapshot — the last locked status resolution (a poll cache
older than 250 ms counts as unknown, never as machine-off) or the last
backflush resolution with a zeroed change count, so a stretched core-0
holder can no longer stall a loop iteration; a cleaning admission stamps
the post-admission resolution into that cache so a post-admission miss
stays provenance-current. Because of this, the backflush poll's wait figure
measures only the acquisition attempt (microseconds uncontended); its hold
figure (`holdLastUs`, `holdMaxUs`) still reports the locked section, and a
long hold means the loop was preempted or stalled while holding the mutex.
Every task mutex additionally remembers the name and tenure of
its longest holder since the waiter last reset it — written under the lock,
so the next acquirer reads it race-free, and a short re-lock cannot hide the
long section before it. The control loop's physical-start, cleaning-release
and temperature/power edges never wait on the facade (see
[concurrency](CONCURRENCY.md)); the remaining blocking Micra edge, queuing a
Web or integration request, reports an acquisition wait of
2 ms or more as it happens: a warning on the serial log names the
holder, and a `machine mux wait` debug-ring event stamps the moment with
the wait and tenure as its arguments, rate-limited to one report per 10 s,
with the report gathered under the lock and emitted after it so
logging never nests inside the facade mutex. The same event also enters
the curated audit latch: a small RAM ring
of classified rare events (every error plus a maintained allowlist of
warnings) that the common ring's rotation cannot evict, captured in the
same critical section and exported as the `audit` section of the debug
export. Own-CPU values are kernel-folded at switch-out: a phase that ran
uninterrupted can read zero until its time folds into a later boundary.

Partition-backed shot stores advance through one 4 KiB erase or one 1 KiB
program operation per worker step. The flash lock is released and the current
machine/scale gates are rechecked between steps; the body precedes the slot
header so a reset cannot expose a partially written generation. Runtime control
is explicitly forbidden from taking the flash lock after boot initialization.

Periodic reset-uptime checkpoints run only when coherent control/scale gates
permit flash I/O. Durable I/O failures back off exponentially from 500 ms to a
30 s ceiling; flash-lock contention retains the short retry, and a successful
write or newly dirtied data resets the I/O backoff.

The health worker's five-second CPU-load sample refreshes the other core through ESP-IDF's
existing IPC task before reading the two IDLE run-time counters. This bounds the
counter age to the sampling point; without that context switch, a core that
remains continuously in IDLE can appear busy because FreeRTOS commits task
run-time only when the task switches out. IPC failure invalidates the sample.
Control reports the worker sample's monotonic version and age; data older than
15 seconds is invalid rather than reported as fresh.

Stack high-water marks are bytes on the supported ESP32-S3 port. Diagnostic
JSON and `HEALTH` publish `stackUnit=bytes` and an unavailable sentinel of
`4294967295`; legacy field names ending in `Words` retain their historical
byte-valued numbers for compatibility. Zero is a measured exhausted margin,
not a missing sample. The low-stack alert enters below 1024 bytes and clears
at 1536 bytes. Reducing a configured stack needs target measurements under the
combined workload.
The current memory trial sets the separate WS SDK task to 6144 bytes. Its
previous 8192-byte configuration had 4180 bytes free in one supplied capture;
the reduced stack remains unqualified until cold TLS, large/invalid frames,
renewal, reconnection and cancellation paths are measured on target.
The HTTP server task is configured with 11264 bytes after the 2026-09-26
target capture (300-sample task profiler under sustained status polling)
left only 1540 bytes free on 10240 — 4 bytes above the 1536-byte gate —
matching the earlier 8192-byte run that left 452 bytes. The webhook worker
stack is 4608 bytes after the same capture measured 1400 bytes free on 4096
during a cold TLS send (client create plus handshake), below the gate; the
network_manager stack stays at 10240 because the documented FACTORY_RESET
path canaried 7168 and the capture did not exercise it. Repeat each workload
on target after any change and retain the new watermark as manual acceptance
evidence.

## Release test

Run the target at the qualified 80 MHz configuration for at least eight hours
with all of the following active together: BLE scan/connect/notifications,
Wi-Fi scan and reconnect, Web UI polling, webhook flood, repeated settings
writes, a full OTA with interruption at every checkpoint, and USB CDC attached
but not drained. Record the status snapshot once per second.

A build passes only when:

- there is no Task/Interrupt Watchdog reset;
- control and scale deadline-miss counters remain zero;
- lifetime service gaps stay at or below 10 ms;
- no application-task stack watermark falls below 1536 bytes, and every required task has evidence;
- safety timers still open the circuit at their configured deadline; and
- diagnostic queue drops are monotonic/accounted and never affect control.

Keep the raw trace, firmware build ID, board revision and RF environment with
the release evidence. Any task/core/priority/clock change invalidates the
measurement and requires the test again.

An observed maximum is not a proven worst-case execution time (WCET).
Qualification requires a combined target run demonstrating each periodic
execution budget with margin and a reviewed result tied to the immutable build
ID. Record evidence for the current build rather than inferring it from a
historical development milestone.

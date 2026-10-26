# Concurrency and lock order

Mutable state has one owner unless its publication is explicitly listed here.
Task-only compound snapshots use `TaskMutex` (a priority-inheriting FreeRTOS
mutex); ISR-shared scalar state uses a `portMUX`; monotonic counters and latches
use lock-free atomics. No code may perform Serial, network, flash, allocation,
formatting, a PSRAM walk, or an unbounded copy while holding a `portMUX`.

This also applies to kernel copies: FreeRTOS queue storage, queue source and
destination buffers, and the `uxTaskGetSystemState` capture array must be in
internal RAM. The persistence queue carries a small work discriminator. Its
settings mailbox and PSRAM-backed shot-store image are immutable from enqueue
until control consumes the worker's generation-tagged completion. A failed
enqueue releases that ownership immediately; the dirty generation remains
pending.

## Lock DAG

Locks are normally acquired one at a time. The only permitted nesting is:

1. lifecycle mutex → webhook state mutex;
2. network work-buffer mutex → JSON document lifetime;
3. OTA task mutex → flash-I/O recursive mutex, and OTA task mutex → OTA
   published-snapshot mutex (a leaf);
4. NimBLE advertisement mutex → NimBLE client-state mutex; and
5. native BLE host-task lifetime mutex → native BLE runtime state spinlock.

The Micra facade mutex nests nothing: every Micra log line, including the
mux-wait anomaly report, is emitted after releasing it, so a facade holder
waiting on the debug log mutex cannot strand the priority a control-loop
waiter would otherwise lend it.

The debug log mutex is a leaf: no holder logs or takes another lock. The
ESP-IDF log hook relies on that when it retains component lines in the Web UI
ring from whichever task logs; it skips contexts that cannot block and never
re-enters for application (`obbw.*`) lines, which are already retained.

No reverse edge is permitted. A callback that would require one must publish a
queue item or atomic latch for the owner instead.

Control status gathers machine/relay, scale telemetry, preferred-scale,
and persistence inputs before taking its publication mutex. The
machine facade supplies a small scalar sample; publication does not allocate a
second full status snapshot. Readers can use the previous committed version
while an input owner is busy. The status version is published with the completed
commit, and refresh acknowledgment follows it.
The scalar publication stays internal; curve arrays reside in PSRAM. The same
mutex covers both writes and the full reader copy, so their snapshot version,
cycle identity and observations cannot tear. The control gate remains a
separate internal snapshot with its existing owner and lock.

Stats obtains records, curves, aggregate and epoch in one acquisition of
`shotStoreMutex`, then releases it before sorting or transmission. Its retained
external capture is protected by the Network workspace mutex. Continuations
reuse the capture; epoch changes reload it and window/order changes rebuild
only the page projection. Debug export invalidates this cache before overwriting
its record array. No second full curve cache or store-lock-to-network-I/O edge
is introduced.

Scale results, fallback mailboxes and the weight FIFO share one task mutex.
The consumer checks results and selects a weight only if none is pending,
without releasing that mutex between the two decisions. Producers use the same
mutex, so a newly published result cannot slip between them. The consumer processes
the copied event after unlocking and retains both control-loop drain checkpoints.
A successful shot tare installs its
capture boundary once per request ID; older or duplicate results cannot re-arm
the baseline, and buffered samples at or before that boundary cannot enter cup,
flow, or trajectory evidence.
The existing retained successful late-tare result also installs that boundary
when its queue event is lost; control rechecks it before processing each selected
weight as well as at drain entry. Control keeps one near-zero sample while its
completion is pending so result/sample processing order does not lose valid
effect evidence. Confirmation requires the same request, cycle and connection,
a capture sequence after the boundary, and a fresh timestamp within the
completion-anchored grace and shot end. Clearing first-drop data and pending
finalization projections remains exclusively control-owned.
Confirmation advances the sample boundary through the observed zero, so older
buffered samples cannot seed a replacement first-flow candidate or machine sense.
Weight delivery uses a fixed 16-event FIFO under its existing task mutex;
overflow explicitly invalidates sample evidence instead of silently joining
nonconsecutive readings. No parsing or cup-state transition runs under that
mutex. Native NimBLE notification callbacks only copy bounded frames into the
client RX ring and then notify the scale owner's task, which drains and
publishes every queued frame on that wake (bounded by the ring depth); its
10 ms linked tick is only the fallback for a missed notification, and
ATT-yield paths do not drain the ring. The safety-critical final
pre-tare harvest runs after the library's command-spacing wait. The worker keeps its 1 ms
service cadence only while establishing a connection. Idle-tare
claim/cancel/approved-sequence updates use the separate request
mutex; neither mutex nests with the other or spans ATT. Unvalidated publication
defers claim using the existing worker tick and unchanged command expiry.
The post-spacing pre-write harvest precedes the idle claim. Its weight sample must
match the latest notification sequence and the control-approved published
packet; otherwise the still-queued command yields to control. The claim freezes
the capture boundary, which the library rechecks under its existing final radio
admission lock. A notification arriving before that admission defers the write;
the worker returns an idle claim to QUEUED for control to validate. Thus a zero
captured during command spacing cannot acknowledge a tare. No new mutex spans ATT.
The same request mutex protects a control-approved pre-tare sample copy. Control
publishes that copy before approving the corresponding idle claim. Immediately
before each tare, the worker copies it only if its packet/generation matches the
published stream and its capture time is fresh; no mutex spans the BLE write.
The frozen pre-write weight returns through the existing command event or durable
idle status. Control translates the anchor, or invalidates it when evidence is
unavailable; enqueue-time load changes cannot accumulate as reference error.
Control publishes the active scale-command cycle under the same request mutex
and revokes it before finalization or failed-start cleanup. Post-spacing admission
rejects queued start/reset/tare operations from another or ended cycle. An
already-admitted operation may finish; timer STOP remains exempt and prioritized.
Preferred-scale persistence acknowledges the copied MAC, name, and history in
one compare-and-clear critical section under its existing task mutex. A newer
identity remains dirty for the next persistence attempt.
Cup/idle-tare diagnostic scalars and worker outcome/drop snapshots are gathered
by control before taking its status publication mutex. Debug export reads the
committed copy, including uncertainty and the last terminal reason.
Calculated cup weight and validity travel in the same `CupTareDiagnostics` copy
inside `ControlStatusSnapshot`, with the placement identity. Control alone
qualifies the FSM's stable absent/present records, calculates their difference,
and invalidates the evidence. Qualified idle unloading can reuse its trusted
empty anchor without forging a new stable-absence record. The scale worker returns
an opaque request ID, pre-write weight, and tare outcome for uncertainty handling.
The Home stream publishes `cupPresence`
with `weightG` (finite grams or null), `weightValid`, and `placementId` from that
committed snapshot. `cupPresence.idleTare` projects readiness and
`cupPresence.accessory`/`accessoryG` the accessory state from those same
diagnostics; the Web layer never reads or changes the cup FSM directly.
The NimBLE advertisement mailbox copies a raw six-byte address and bounded name
under the existing nested spinlocks; the consumer formats its private address
copy after unlocking. A concurrent advertisement remains pending for the next
consumption.

Cancelled setup and established-link teardown retain one operation, handle, and
runtime epoch until GAP acknowledges closure or the controller reports no link.
A late success only publishes its handle; the scale owner performs termination
outside all spinlocks, deferring it through an active quiet interval. New attempts
remain blocked while cleanup is unresolved, preventing slot overwrite and handle
reuse. A matching late disconnect restarts the full quiet interval and publishes
its completion for the owner without reviving old data. Host reset clears that
ownership before further radio work. Power-off closes the library command
generation under the same client mux used by RX callbacks.

Application scale writes remain single-owner operations. Protocol metadata
sets their minimum interval (100 ms for Bookoo), and the NimBLE owner services
callbacks while waiting before rechecking ready state, disconnect evidence,
handle, and connection generation. A power-off request publishes a terminal
generation barrier under the existing scale mailbox spinlock before it can
compete with queued work. Producers then reject new commands, queued commands
complete through their stale-result path. Debug, STOP, beep and volume writes
also check that barrier after command spacing; STOP remains exempt from cycle
cancellation. A write already admitted before shutdown publication may finish.
Connection-transition cleanup preserves an accepted shutdown for the newly
published generation. No scale lock spans ATT or GAP. The GAP callback
immediately marks link loss and rejects new admissions. If one radio submission
was already admitted, it finishes before the full 3,000 ms quiet interval
starts; otherwise the interval starts in the callback. Every NimBLE procedure
shares that library-owned final admission point; firmware can observe the
remaining time but cannot clear or bypass it. Pre-disconnect weight events
carry the disconnect sequence and are rejected after the epoch changes. Every
actual application-command submission and terminal outcome is emitted at INFO with
its operation, generation, response mode, spacing, raw status, and elapsed
time; payload bytes and peer addresses are excluded.

The owner copies native BLE diagnostics under the callback's client-state mux
before taking the firmware link-publication spinlock. Native host stack sampling
retains the task through a static task mutex until the stack read finishes.
Health sampling never waits for that mutex and returns cached stack telemetry
during teardown or another sample. Shutdown acquires it within its existing
timeout budget before stopping/joining/deleting the host. Stack walks and host
teardown remain outside spinlocks; the final watermark survives deletion.

The relay and independent safety-timer `portMUX` sections are independent and
may never nest with another lock. The timer captures callback state under its
spinlock, invokes the relay callback after releasing it, and rejects stop/re-arm
while that callback is in flight. Relay sections contain only GPIO and bounded
DRAM scalar state. The local buzzer, debug ring, and other task-only compound
state use `TaskMutex`. Heap/CPU sampling and task-profiler capture belong to the
core-0 health worker; control consumes its one-slot mailbox without waiting and
ages stale samples explicitly. The same health worker owns the manual scale
profiler: the health worker maintains the capture clock and smoothed capacity
ETA on its service tick, independently of browser reads. Producers request
completion on filling the last ordinary slot; the health worker closes the
capture with the reserved terminal record. Producers (scale worker and control loop) append 32-byte records
through one leaf capture mutex they only take after releasing their own locks,
flash invalidate/save run as single cache-off steps on the settings_persist
worker between safe-write gates, and a download lease pins the frozen
generation so Start/Delete cannot replace a trace while it streams. USB application logs are emitted by the bounded
`serial_log` queue on core 0. Eight short log records remain internal while a
bounded 2.5 KiB PSRAM buffer carries the CLI reply published to that owner;
saturation increments dropped/truncated counters instead of waiting in control.

## P2 spinlock inventory

This is the retained-lock inventory for the resource/concurrency qualification
work; the filename/section label is preserved for existing references.

The task-only bullseye configuration,
settings-persistence handoff, and the shared scale result/weight handoff use static
FreeRTOS mutexes. These paths can be reached from lower-priority HTTP,
network, persistence, BLE or control tasks and therefore require priority
inheritance; disabling interrupts was not justified.

The remaining `portMUX_TYPE` groups are tracked explicitly:

- relay and independent hardware-timer state: shared with an ISR; must remain
  spinlocked and be measured on target;
- scale link/beep/debug snapshots: hot BLE/control publication, pending the
  event-driven worker conversion;
- time service, native BLE runtime, and EspressoScaleBLE
  callback registries/queues: callback-facing state whose call context must be
  proven before conversion.

No listed section may allocate, log, access flash/PSRAM-dependent data or call
a blocking API while locked. Qualification is incomplete until target tracing records
the maximum interrupts-disabled duration for every retained group.

## Snapshot contract

BBW candidate windows, learning generations and both per-preset offsets belong
to control. Network reads gain, alpha baseline, provenance, evidence count and both offset
previews from the same committed control snapshot as the active preset ID;
it never reads candidate RAM. Cycle/finalizer snapshots retain the actual applied
gain and generation. Deferred persistence still owns writes; no new task, queue,
mutex or lock-order edge is introduced.

Control, gate, recipe, profiler, scale-link, network, OTA and webhook readers
must use their snapshot APIs. A snapshot includes a publication timestamp or
age; control status also carries a monotonic `snapshotVersion`. Observational
monotonic metrics may be atomic. Metrics used to authorize control must be part
of the same coherent snapshot as the decision state.

The network task progress timestamp is a relaxed atomic because it is a
monotonic observational metric. Reset-history checkpoint and clear operations
hold the shared flash-I/O lock across the durable write and publication of the
live mirror/checkpoint timestamp, so failed writes cannot publish or resurrect
state.

## Micra push observation and outbound admission

The cloud worker is the sole owner of the HTTP session and WebSocket init,
start, stop, unregister and destroy. SDK stop joins actual transport shutdown
and is never called from an event or while holding the facade mutex. The SDK
callback can hold its own recursive lock: the permitted edge is SDK lock →
facade snapshot mutex. No application-mutex → SDK-call edge is permitted.
The bounded callback parser publishes complete field updates directly, so an
HTTP operation cannot delay push publication. Identity/epoch/intent and
independent power, temperature and online-evidence revisions prevent older
results overwriting newer evidence. A repeated online report advances its own
revision without blocking initial power from the API. Diagnostic readers only
copy under the facade mutex; socket generations are initialized under that same
mutex before startup. The control loop never blocks on the facade: every
loop-side entry takes the mutex with a zero-timeout acquire. On contention the
status poll and backflush poll serve the loop-owned cache — the last locked
status resolution (with a poll cache older than 250 ms counting as unknown, so
the effectively-off shortcut can only fail toward queueing a
server-revalidated StandBy) or the last backflush resolution with a zeroed
change count, which hides new ring edges for one poll instead of re-serving
drained ones; a cleaning admission stamps the post-admission resolution into
that cache. A contended physical-start edge decides from that same cached
backflush resolution: a busy cleaning episode refuses the edge (the barista
presses again), anything else is a normal start, and the atomic ingress fence
still retires bytes in flight; cleaning admission and wake recognition run
only under the lock. Contended temperature/power commands and cleaning
releases wait in loop-owned slots (latest wins per kind) and are applied first
at the loop's next successful acquire, which the backflush poll forces while
any is pending. Receive-path
subscription checks copy only the subscribed
flag under that mutex; traffic-rate aggregation belongs to diagnostic reads.
WS transport operations use the SDK's 10-second timeout, including initial TLS
and Upgrade. The connected SDK task normally polls input every second; a TLS
handshake or stalled frame already in progress can delay owner stop until its
transport operation returns. Failure callbacks publish authentication metadata
before signaling failure to the owner. Safe disconnect logs contain only numeric
HTTP/TLS/socket error codes; signed headers and event payloads are never logged.
A scale-worker-owned atomic connection flag qualifies the admitted-shot
transport gate. Unscaled shots do not abort API reads or fence WS ingress;
scale loss releases that gate independently of the shot's starting scale.
A lock-free disconnect generation also invalidates in-flight HTTP/WS evidence
when a scaled-shot pause starts and ends before the cloud owner runs. The next
owner turn UNSUBSCRIBEs and resubscribes on the same socket to resynchronize;
deferred HTTP observations retain held state. Broker MESSAGE frames that arrive
after UNSUBSCRIBE are ignored rather than treated as protocol failures, and
token renewal waits while the subscription is parked.
A separate shot generation cancels queued power commands even when a complete
cycle occurs between worker turns. Queue admission captures it before checking
the shot gate; commands from earlier cycles cannot resume afterward. Scale-only
inhibition still defers those commands. Backflush admission cancels active
machine commands without canceling parallel API observation or WSS.

A discovery-owned atomic acquisition latch is published under the existing
NimBLE discovery critical section before Candidate becomes consumable. Its
callback performs atomic stores and notifications only: no task mutex, heap,
network API or wait. HTTP/webhook/NTP owners close their transports independently;
An established Micra WSS connection stays open through scale acquisition but
parks its subscription unless a backflush is in progress.
New WSS setup and reconnection wait for acquisition, setup, BLE quiet and scale
connection gates to clear; admission is rechecked after session preparation and
before SDK init/start. A handshake already in flight can still finish.
Ready
clears acquisition; disappearance requires five seconds of actual active scan
opportunity. Setup, communication quiet, shot and maintenance reasons remain
independent; stale periodic connecting snapshots cannot release acquisition.
Acquisition closes admission before updating its fence; release publishes the
new fence before opening admission, so an old callback cannot cross the release.

The integration mutex also owns the eight-entry ordered backflush handoff.
Control copies/drains it and evaluates outside the lock. The callback samples
the atomic attempt counter before taking any mutex, so lock contention cannot
retag a pre-edge frame. Tokens persist across fragments; continuity latches loss across
rapid recovery. SDK callbacks publish loss immediately without waiting for
transport destruction. Control opens on its next service, with no reconnect
grace period. An invalidation after a promotion check is handled on the next
turn; this is not a cross-task atomic actuation transaction.
Explicit account, machine or transport replacement retires the old unresolved
episode; a live attempt remains busy until control observes invalidation and
opens. Ordinary connection loss retains unresolved cleaning for reconciliation.
Socket liveness compares callback timestamps against a clock sampled after
the snapshot, preventing concurrent publication from underflowing its age.
The relay driver keeps the old task timer while replacing the independent
deadline, then replaces the task deadline and commits under its ISR-shared
lock. A racing trip or failed rearm wins. No timer operation holds the network
mutex, and promotion never writes GPIO closed.

The cloud owner retains assumed power before every live socket stop. HTTP-only
cancellation and retry interruption preserve an admitted WSS connection; shot,
network and maintenance fences still stop it. Graceful SDK CLOSED events trigger
the same bounded recovery as transport errors. Authentication recovery counts survive pauses and workspace
parking; only manual retry, account/transport changes or stable streaming reset
them. The IDF transport_ws log tag is disabled before client startup because its
write-error path prints signed Upgrade headers; application diagnostics retain
safe status/reason fields.

Owner completion telemetry certifies transport teardown, not RF or DNS
silence. An SDK resolution already submitted can finish. No resolver
replacement or global DNS-cache mutation is introduced. Webhooks resolve the
receiver through lwIP's asynchronous resolver, polled under the admission gate
and the delivery deadline, so the transport's own lookup is answered from the
cache instead of blocking the worker. They progress plain HTTP asynchronously
on the pinned IDF 6.1 transport with one connection per POST, keep one
absolute 1800-ms deadline and do not replay an ambiguously dispatched POST.
NTP closes callback acceptance before owner stop. OTA publishes its own atomic
busy gate. OTA session creation, range writes, commit and discard run in FIFO
order on the transient `ota_worker` task through ESP-IDF async requests, so
the httpd task keeps serving the Web UI stream and API reads during erase and
write. Those operations hold the OTA mutex for seconds; status readers on
httpd use `snapshot(false)`, which takes the copy published at the last state
change or written sector instead of waiting. `stopHttpServer()` refuses new
OTA jobs and waits, feeding its watchdog, until every accepted job has been
answered before `httpd_stop` frees the sessions those jobs hold; a range
write aborts at its next sector once the stop is visible. Maintenance parks
and frees the Micra workspaces without delaying
control or BLE. Settings writes resume only after image confirmation, with
the original queued dirty generations intact.

# Firmware state machines

This page is a map of the **runtime finite-state machines** in Open Brew by
Weight. It is written for someone who already knows the product
(activator → machine circuit → scale) and wants to know **what each machine is for**,
**what each state means**, and **which events move it**.

Enums that are not machines (settings, log codes, buzzer patterns) are
omitted. Source names match the firmware.

Home's shot stream is a read-only presentation of these owners, not another
brew state machine. Control publishes active, pending-drip and resolved cycle
views at its existing snapshot boundary. Network selects the latest eligible
Stats record when idle, or a nonqualifying result for an observing page until
30 seconds after resolution. New cycles take precedence. Extraction duration
freezes at cutoff while accepted drip observations retain their own timestamps;
eligibility, guards, learning and durable storage keep their existing authority.

Related product docs: [Brew by weight](features/brew-by-weight.md),
[Cup protection](features/cup-protection.md),
[A→M time guard](features/auto-to-manual.md),
[OTA](features/ota.md),
[Emergency recovery](EMERGENCY_RECOVERY.md),
[Hardware](HARDWARE.md).

## Find a subsystem

| Topic | Sections |
| --- | --- |
| Brew and electrical safety | [Brew orchestrator](#1-brew-orchestrator-brewstate), [relay](#2-relay-safety-relaysafetystate) |
| Physical controls | [Machine state](#3-machine-run-state-machinerunstate), [user intent](#4-user-intent-userintent) |
| Optional machine integration | [Linea Micra observer](#linea-micra-power-observer-and-wake-qualification) |
| Weight and cup sensing | [Weight control](#5-weight-control-weightcontrolstate), [stream](#6-weight-stream-weightstreamstate), [cup](#7-cup-presence-cuppresencestate), [first flow](#8-first-flow-firstflowphase--firstflowclass), [touch](#9-accidental-touch-accidentaltouchphase--accidentaltouchclass) |
| Scale connection | [Link and commands](#10-scale-link-scalelinkstate), [no-scale guard](#11-no-scale-bbw-guard) |
| Access and updates | [Recovery](#12-recovery-gesture), [Wi-Fi](#13-station-wi-fi-stastate), [scan](#14-wi-fi-scan-wifiscanstate), [clock](#15-wall-clock-timesyncstate), [OTA](#16-ota-otastate), [Web commands](#17-web-command-pipeline-commandresultstate) |
| Cross-cutting | [End reasons](#end-reasons-brew-outcomes), [loop ordering](#control-loop-ordering) |

[Power management](settings/power-management.md) derives energy demand from
these existing states. It has no actuation authority: active or uncertain
machine operation holds the CPU requirement until confirmed stop, independent
of relay level. Control owns clock changes, ScaleService owns BLE policy and
NetworkService owns Wi-Fi policy. Temporary WebUI/physical leases never change
the functional machine state or its safety deadlines.

## How to read this

Each section has:

1. **Purpose** — why the machine exists (what would go wrong without it).
2. **States** — the current mode.
3. **Events** — inputs that can change the mode (GPIO, BLE packets,
   timers, Web UI commands). Not every enum is an event; some machines
   are derived (they recompute from another machine every loop).

The control task in `openBrewByWeight.cpp` is the orchestrator. Brew, scale sense,
and cup presence do not call each other: the brew orchestrator polls machine **once** per
loop (`machinePollIntention` → `machineLastIntention`), builds `GuardInputs` for
brew guards, pushes a `MachineSense` snapshot into machine, and applies scale
events (first-drop, post-tare cup hold). The **activator** reads user intention
on `ACTIVATOR_GPIO` (paddle, switch, or another compatible mechanism),
interprets that signal, and publishes `UserIntent` / `MachineIntention`. Brew
by weight / brew / cup / scale consume that contract; they do not read the GPIO
or know paddle vs switch. Machine specializations never read `session` or live
scale globals. One machine
is special: **relay safety can open the machine circuit without waiting for brew
policy**. Everything else *requests* close/open; safety *owns* the
contact.

## Layers

| Layer | Machines | Privilege |
| --- | --- | --- |
| Electrical | Relay safety | Can de-energize K1 from an ISR or trip path. Normal-mode 60 s cap; compatibility mirroring has no duration cap. |
| Machine view | Machine run state, user intent | Derived. Status/UI only; they do not drive GPIO themselves. |
| Brew orchestrator | Brew orchestrator | Orchestrates rinse vs shot vs idle from `UserIntent` (including `REQUEST_RINSE`) and *requests* circuit start/stop. Does not classify paddle ON→OFF or long-press. |
| In-shot sensing | Weight stream, weight control, cup presence, first flow, accidental touch | Consume scale samples. Only **weight control** can ask the brew orchestrator to cut. |
| Scale link | BLE link + scale command/result | Connects the pan; does not close the machine circuit. |
| Off-brew | STA, Wi-Fi scan, NTP, OTA, Web command results, recovery gesture | Must not leave machine circuit closed. OTA and recovery keep the relay open. |

```mermaid
flowchart TB
  gpio[ACTIVATOR_GPIO] --> activator[Activator]
  activator -->|UserIntent| intent[User intent]
  intent --> brew[Brew orchestrator]
  web[Web] --> brew
  brew -->|request start/stop| safety[Relay safety]
  safety -->|K1| circuit[machine circuit]
  safety --> run[Machine run state]
  ble[BLE scale] --> link[Scale link]
  link --> stream[Weight stream]
  stream --> cup[Cup presence]
  stream --> flow[First flow]
  stream --> touch[Accidental touch]
  stream --> wctl[Weight control]
  cup --> brew
  wctl -->|cut / suspend| brew
  safety -->|trip| brew
```

## How the machines interact

The relay-close/open sequence below describes paddle firmware. Momentary
firmware uses pulses and separate inferred/reed state; relay OPEN alone does
not mean the group is idle. See section 3 before applying this sequence to a
button machine.

**Idle.** The brew orchestrator is `READY`. Relay safety is `OPEN`. Machine run state
is `IDLE`. The scale link may be `CONNECTED` or
`DISCONNECTED`; that only matters when a shot starts.

**Start.** A debounced activator ON (or a Web start, if remote machine control is
compiled in) is `REQUEST_START`. The brew orchestrator may **block** that start
(no-scale BBW guard, cup-to-start guard) without closing the machine circuit. With Quick rinse enabled, a short
ON→OFF in its window becomes `RINSE`.

If the start is accepted, the brew orchestrator calls `machineRequestStart`.
On paddle, relay safety goes `OPEN → ARMING → CLOSED` (or refuses and the brew orchestrator
goes to `REQUIRES_OFF`). Machine run state follows: `RUNNING_UNCONFIRMED` while
arming, then `RUNNING`.

**During a shot.** The brew orchestrator is `BREW` or `MANUAL_NO_SCALE`. Weight
control is `ACTIVE` only if the cycle started with a usable scale and
brew-by-weight is on. Fresh BLE weights update the stream, cup
presence, first-flow, and accidental-touch detectors. BLE drop or
**silent notifications** (no parsed `WEIGHT` for 1 s) **suspends**
weight control; rejected brew samples (post-tare, slew) and a stable
accepted weight do not. The brew orchestrator stays in `BREW` and A→M may still
cut later. Cup `REMOVED` can cut if that option is on.

At cycle start, control snapshots the preset's smooth-cut parameters into the
control-owned corridor state: target, shot time window t1/t2, side switches and
margins, weight-cut delay, max shot time, each side's ramp length and curve
(D_fast, D_slow, 5 s smoothstep by default), the drip offset r
and τ (scale protocol sensor lag + 100 ms valve latency + the momentary stop
pulse where the machine has one). The cut line f(t) holds the ceiling (target +
fast margin) until t1 − D_fast, eases into the target at t1 along the Fast
curve, stays on the target through t2, eases down to the floor (target − slow
margin) by t2 + D_slow along the Slow curve and holds
it; a side that is off is the target line. All times are stop-command times
from the machine running start.

Two laws fire, both behind the existing BBW protection, freshness and `ACTIVE`
gates. After each accepted sample a time-window least-squares flow estimate q̂
(3 s, MAD-gated) predicts the earliest stop-command time t* where weight +
q̂·(t − t_k) + q̂·τ + r reaches f(t); the timed law fires when the machine time
reaches t*. Without a valid q̂ (or below 3 dg/s) only the direct backstop acts:
two consecutive fresh samples at or above f(t) − r. While samples are rejected
(`VALIDATING`) neither law fires. A weight cut reports the segment of f at the
stop: `FAST_EXTRACTION_MAX_WEIGHT` above the target, `SCALE_THRESHOLD` on it,
`SLOW_EXTRACTION_MIN_WEIGHT` below it; `FAST_EXTRACTION_MIN_TIME` and
`SLOW_EXTRACTION_MAX_TIME` are no longer emitted. Late retare and scale recovery
reset the estimator. Safety trips, hard/operational walls, physical stop, cup
removal and A→M keep their existing authority; nothing in the smooth cut can
suspend the wall or the circuit limit.

After drip delay, history records captured values and whether learning applied.
The drip offset learns from weight cuts on any segment, against the cut line at
the stop (observation = offset used + final − f(t_cut)); channeling, manual,
time/safety-limit, A→M and cup-removal outcomes, and cuts without a valid flow
estimate (no q·τ lead), cannot adjust it or its candidate evidence. The
channeling surge baseline must lie a full estimator window after the first
drop. A quick start (armed by Set drip, a new or restored preset, or a fresh
install; persisted in NVS key `bbwQs`, absent = off) records the raw
observations of the next three learning shots; when max − min ≤ 0.40 g the
third sets the offset to their median instead of the EWMA step.
EWMA reuses the accepted final observation and additionally requires fresh weight,
valid baseline and matching connection provenance. Control updates only the
originating preset/algorithm generation. Reset or deletion invalidates pending
learning while history can still finalize. A new shot cancels old pending
analysis through the existing last-known-weight path. Candidate scoring never
runs in the active cutoff or ISR path.

**Stop.** Brew policy (`SCALE_THRESHOLD`, paddle OFF, guards, Web Stop)
or a safety trip asks to open the machine circuit. `machineRequestStop` drives safety
back to `OPEN` unless it already tripped. If the paddle is still ON,
the brew orchestrator lands in `REQUIRES_OFF` until a stable OFF (`STABLE_IDLE`).
Otherwise it returns to `READY`.

**Independence.** In full firmware mode, a 60 s (or operational-wall) timer ISR can trip
safety while the control task is in BLE or flash I/O. The next loop
sees `TRIPPED` / `LOCKOUT` and finalizes the cycle with
`RELAY_SAFETY_FAILURE`. Until that loop consumes the trip, activator drive is
denied in both firmware modes, so a held paddle or button cannot re-close K1;
the mirror then stays suppressed for the rest of that hold. Consuming an ISR trip also publishes the RTC OPEN
marker and stops the deadline timers, even when no cycle is active (for
example a wake passthrough), so a later restart is not counted as a reset
during close. Network, OTA, and NTP never close the machine circuit. A
maintenance lease (Wi-Fi scan, some admin work) forces machine circuit open and
parks the brew orchestrator in `REQUIRES_OFF` if the paddle is ON.
Linea Micra settings saves take no lease: they change neither the network nor
the machine, so configuration stays editable and the cloud client keeps its
in-flight requests. The network task commits them to the PSRAM settings record
(only while no cycle is active and the machine circuit is open) and the
deferred settings persist writes them, as for every Settings-page change.

---

## 1. Brew orchestrator (`BrewState`)

**Purpose.** The brew *workflow*: idle, rinse, automatic shot, or
manual no-scale shot. It is the only machine that decides *when* to
request machine circuit close, and *why* a cycle ended (`EndReason`).

Source: `OpenBrewByWeightBrewTypes.h`, orchestrated in `openBrewByWeight.cpp`.

### States

| State | Meaning |
| --- | --- |
| `REQUIRES_OFF` | Cycle over (or start refused after a trip) while the activator is still ON. Machine circuit must stay open until a stable activator OFF. Prevents an immediate re-close. |
| `READY` | Idle. Waiting for a start gesture. Machine circuit is open. |
| `BREW` | Shot in progress with weight and/or timer policy. Machine circuit is closed (unless safety already opened it). Includes timer-only BBW-off shots that started with a scale (Max BBW time not armed; 60 s cap only). |
| `RINSE` | Timed group-head rinse. Activator edges are ignored until the rinse duration elapses. A rinse demoted from a running shot anchors the clock to the original activation, so the pre-classification time counts toward the duration and the reported duration equals the configured one; a rinse started from idle runs the full duration from its start. Not stored in shot history. |
| `MANUAL_NO_SCALE` | Shot in progress without weight stop (no usable scale at start, or BBW off without treating it as timer-only brew). Ends on activator OFF, rinse demotion, or the 60 s firmware cap — not Max BBW time. |

### Events (inputs)

| Event | From | Effect |
| --- | --- | --- |
| Activator ON (`REQUEST_START`) | User intent | From `READY`: `beginCycle`. May be held/blocked by no-scale or cup-start guards. |
| `REQUEST_RINSE` | Machine (paddle short ON→OFF, momentary idle long-press, or web) | From `READY`: `beginRinseCycle` + timed `RINSE`. From `BREW` / `MANUAL_NO_SCALE`: demote to `RINSE`. The clock anchors to the machine run's start (the acceptance time for a fresh rinse), never to a raw GPIO edge. |
| Activator OFF after rinse window | User intent | Natural mode: end shot (`EndReason::ACTIVATOR` → `READY`). Original/Auto BBW: may keep machine circuit closed (walk-away). `REQUEST_STOP` always ends a shot; it does not demote to rinse. |
| Paddle ON during Original BBW | User intent | Promotes the rest of that shot to Natural (paddle OFF will then end it). |
| Weight cut due | Weight control | `finalizeCycle` with `SCALE_THRESHOLD` or a guard `EndReason`. |
| Cup removed | Cup presence | Optional cut (`CUP_REMOVED`) if cup-stop is enabled. |
| A→M deadline | Weight control suspended | Cut (`AUTO_TO_MANUAL_GUARD`) if the guard is enforced. |
| Rinse duration elapsed | Timer | `RINSE_COMPLETE` → `READY` or `REQUIRES_OFF`. |
| Web Stop / Web start / rinse | Web command | Same as the activator, but `ControlSource::WEB`. Physical activator always wins (`PHYSICAL_OVERRIDE`). |
| Safety trip / arm failure | Relay safety | `RELAY_SAFETY_FAILURE` or wall/hard-limit reasons; next state `REQUIRES_OFF` if the activator is ON. |
| Physical activator while Web cycle | User intent | Immediate finalize, `PHYSICAL_OVERRIDE`. |

Typical path: `READY → BREW → READY` (or `REQUIRES_OFF` if the activator
was left ON). Rinse: `READY → RINSE → READY`.

---

## 2. Relay safety (`RelaySafetyState`)

**Purpose.** Own the brew contact. Policy can *ask* to close; this
machine arms independent timers **before** energizing K1, and can
de-energize from an ISR if the control task is stuck. This is the
last firmware line of defence short of a second hardware barrier
(K2). See [Hardware](HARDWARE.md).

Source: `OpenBrewByWeightSafety.h`, `OpenBrewByWeightMachine.h`.

### States

| State | Meaning |
| --- | --- |
| `BOOT_SAFE` | Power-on default before supervisors are judged. Machine circuit is commanded open. |
| `OPEN` | Relay de-energized. Close is allowed if watchdogs and timers are ready. |
| `ARMING` | Close requested: generation bumped, hard/operational timers started, GPIO not yet committed (or about to be). A timeout here still trips. |
| `CLOSED` | K1 energized. `commandedClosed` is latched in RTC so a reset mid-close is treated as unsafe. |
| `TRIPPED` | Opened by a limit or fault with **no lockout**. Control must still treat machine circuit as forbidden until the cycle is finalized. |
| `LOCKOUT` | Opened by a fault that must not silently retry (stuck feedback, watchdog missing, timer arm or initialization failure). New closes are refused until the fault is cleared by a safe boot path. An active session is finalized with `RELAY_SAFETY_FAILURE`. Unsafe resets and boot loops are counted in the reset history but never latch `LOCKOUT`. |

### Events / faults (`RelaySafetyFault`)

| Fault | Typical cause | Trip style |
| --- | --- | --- |
| `NONE` | Healthy. | — |
| `INITIALIZATION_FAILED` | Clock or software timers not ready at boot. | Lockout |
| `WATCHDOG_UNAVAILABLE` | Task WDT not subscribed. | Lockout |
| `INVALID_LIMIT` | Close requested with a bad operational limit. | Lockout |
| `TIMER_ARM_FAILED` | Could not start the deadline timers. | Lockout |
| `HARD_LIMIT` | 60 s normally; qualified backflush permits at most 180 s from the original close. ISR-safe. | Trip (not lockout) |
| `OPERATIONAL_LIMIT` | Configured Max BBW time (default 50 s) on automatic BBW, or Original-mode wall after paddle release. Not armed on timer-only or no-scale shots. | Trip |
| `FEEDBACK_STUCK_CLOSED` | Optional echo GPIO already closed before arm. | Lockout |
| `FEEDBACK_FAILED_TO_CLOSE` | Echo never matched a commanded close. | Lockout |
| `FEEDBACK_CHANGED_UNEXPECTEDLY` | Echo flipped while closed/open unexpectedly. | Lockout |
| `TASK_WATCHDOG_FAILURE` | Control task missed WDT. | Lockout |
| `GPIO_DESYNC` | Commanded CLOSED but the relay GPIO still reads OPEN after a rewrite. Best-effort (`digitalRead` of an output is not a contact sense). Periodic rewrite while commanded closed is the main defense. | Trip (not lockout) |

**Events that are not faults:** `machineRequestStart` (OPEN→ARMING→CLOSED),
`machineRequestStop` (CLOSED/ARMING→OPEN, unless already TRIPPED/LOCKOUT).

Machine run state is **derived** from this machine (next section). The
The brew orchestrator observes trips on the next control-loop pass.

---

## 3. Machine run state (`MachineRunState`)

**Purpose.** A coarse, UI-facing view of “is water running through the
group (shot or rinse)?”, without exposing ARMING vs CLOSED vs TRIPPED. It
does not describe machine power: a powered-on machine is normally `IDLE`.
It does not command GPIO.

Source: `OpenBrewByWeightMachineTypes.h`, `machineRunState()` in
`OpenBrewByWeightMachinePaddleState.h` (included from `OpenBrewByWeightMachine.h`).
Config lock uses `machineIsRunning()`, which for paddle equals machine circuit closed.

### States

| State | Meaning |
| --- | --- |
| `IDLE` | Relay not closed. Safety is OPEN, or tripped/lockout with contact open. |
| `RUNNING_UNCONFIRMED` | Safety is `ARMING`: close is in flight, echo may not yet match. Reed: configured start edge (press or release), reed still off, within confirm timeout. |
| `RUNNING` | Safety `CLOSED` or `relay.closed`. Reed: reed is on (outside an `IDLE_UNCONFIRMED` window). |
| `IDLE_UNCONFIRMED` | Reed: configured stop edge (press or release), reed still on, within confirm timeout. Switch-only: quiet START nack (no espresso-like flow before the shot-reaction timeout), or a logical STOP still settling (pan not yet quiet). Momentary-only: cup `REMOVED` while in this state settles to `IDLE` immediately (no quiet wait). |
| `UNKNOWN` | Paddle / momentary-only: safety TRIPPED/LOCKOUT but the contact still reads closed (should not last). Reed builds do not use this: K1 trip does not hide the reed. |

### Command vs pin vs paddle

Two different “desync” stories. Do not mix them:

| Build | Source of truth for machine run | Internal software/pin desync | Paddle ON / K1 OFF after an automatic cut |
| --- | --- | --- | --- |
| Paddle (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=0`) | Relay GPIO / commanded closed | **None expected.** Software CLOSED with pin OPEN is a bug; firmware rewrites the closed level every loop and logs `RELAY_GPIO_DESYNC`. If the rewrite does not stick, `GPIO_DESYNC` trips. | **Expected:** brew orchestrator `REQUIRES_OFF` + paddle-return reminder until a stable OFF. K1 must stay open; the ON-only mirror must not re-close. |
| Reed (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2`) | Reed, except Assumed after the configured edge | Only **reed confirm timeout** (about 1 s) | N/A (cut is a pulse) |
| Momentary-only (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=1`) | Inferred | Yes, outside reed | N/A |

On **momentary-only** builds (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=1`) these labels are
inferred from brew-accepted weight, not from K1. Boot is `IDLE`. A
START pulse goes to `RUNNING_UNCONFIRMED`. Espresso-like flow (about 0.60 g/s, below
the finger-jump rate, ≥1 g
above the shot baseline, after 800 ms of logical run) is the only path to
`RUNNING`, including recovery from `IDLE_UNCONFIRMED`. `RUNNING` expires if
flow stays below 0.20 g/s for 500 ms, so auto-cut cannot pulse a stopped
group. A START that stays on the shot baseline for the shot-reaction
timeout (default 12 s, setting 3–30 s) is nacked to `IDLE_UNCONFIRMED` — not
`IDLE` — so a late first drop can still confirm ON without a second
pulse. A START nack (or `RUNNING_UNCONFIRMED` that never left a 1 g band around the
shot baseline) that lasts until the compiled hard cap
(`HARD_MAX_CIRCUIT_CLOSED_MS`, 60 s) still settles to `IDLE` with
no relay pulse and no beep; the brew orchestrator abandons the cycle so the next
press is a new Start (tare + timer). A logical STOP (user or firmware) goes to `IDLE_UNCONFIRMED` while the
pan settles. A STOP ack needs a quiet pan; timeout without quiet does
**not** force Idle, but later quiet still settles to `IDLE`. A START
nack that stays `IDLE_UNCONFIRMED` without stop-settling does **not** idle on
quiet — late espresso-like flow still confirms ON with no extra pulse. Cup
presence `REMOVED` while `IDLE_UNCONFIRMED` settles immediately to `IDLE`
(no quiet wait); this does not apply from `RUNNING_UNCONFIRMED` / `RUNNING` and
has no effect on reed builds. If
espresso-like flow continues after an assumed user STOP, polarity is
wrong: go to `RUNNING` without an extra pulse. Firmware-cut stop
retries still pulse while the stop-ack window is open. Scale connect can
settle to `IDLE` when
**Assume idle when the scale connects** is on and no brew cycle is active
(no relay pulse). A successful BBW or extraction-guard cut
(`SCALE_THRESHOLD`, `FAST_EXTRACTION_*`, `SLOW_EXTRACTION_*`) plus a quiet
pan (live, or at drip delay with a plausible settled weight) settles to
`IDLE`. A short shot that never schedules drip-delay finalize still
settles on a quiet pan. Home
**Override idle** / **Override brewing** are the manual escape hatch when
the group is electrically ON with no coffee; they do not pulse. Override
brewing is rejected while a firmware cut or its stop retry is pending. An
operational wall without `RUNNING` and with a scale still leaves an
orphan run: settings stay locked and a later user Stop may pulse; firmware
auto-cut does not. Max BBW time is not armed on timer-only or no-scale
shots. At the 60 s hard cap without `RUNNING`, firmware settles to
`IDLE` with no stop pulse so the next press is Start.

On **momentary+reed** builds (`OPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2`) the reed is
canonical except for a short assumed window after the configured start/stop
edge (press or release per Start/stop on — not the 1:1 relay close; button
→ solenoid → reed lag). Reed is polled every control loop (`digitalRead`
plus 30 ms debounce); a sticky level is not missed. Outside assumed, reed
off is `IDLE` and reed on is `RUNNING`. If the reed reaches
the expected level during the window, confirm immediately. If the
reed-confirm timeout elapses (default 1 s, setting 0.2–5 s, reed-only),
confirm the **actual** reed: `RUNNING_UNCONFIRMED` + reed still off → `IDLE`
(clear logical run, one-cycle `REQUEST_STOP`, no extra stop pulse);
`IDLE_UNCONFIRMED` + reed still on → `RUNNING`. Boot with the reed already
on is `RUNNING`. K1 `TRIPPED`/`LOCKOUT` does not override the reed.
Firmware auto-cut still requires a stable reed off earlier this boot.

`machineIsRunning()` is logical run, stop-ack, orphan, `RUNNING`, or
`RUNNING_UNCONFIRMED`. On reed builds it is also true while the reed is on, so
`IDLE_UNCONFIRMED` stays running until the reed drops. Home shows these as Idle, Starting, Running, and Stopping;
Diagnostic shows the raw state name. Diagnostic JSON also reports
`machineStartAck`, `machineStopAck`, and `machineOrphan`.

### Events

Paddle state is derived from relay safety. Momentary state also responds to
button intentions, scale-flow evidence or reed changes, and confirmation
windows as described above; it is not derived from K1 alone.

---

## 4. User intent (`UserIntent`)

**Purpose.** Generic brew request after the **activator** reads `ACTIVATOR_GPIO`
(paddle, switch, or another compatible mechanism), interprets that signal, and
translates it to something the brew orchestrator understands. The brew orchestrator never
reads GPIO, paddle mode, or machine type.

Source: `OpenBrewByWeightMachineTypes.h`, `machinePollIntention()`. Latch TYPE=0
maps GPIO + snapshotted `PaddleMode` onto these intents (Original/Auto may
omit `REQUEST_STOP` after the rinse window). A short ON→OFF with rinse
enabled publishes `REQUEST_RINSE`. Momentary maps a hold no longer than
Single-press limit to start/stop (on press or on release). With rinse
enabled, an idle long-press at the rinse gesture publishes `REQUEST_RINSE`
instead (press mode demotes; release mode starts rinse directly). With
rinse off, a longer hold is mirror-only.

### States (snapshot, not latched)

| State | Meaning |
| --- | --- |
| `NONE` | No classified edge this sample (transient). |
| `REQUEST_START` | User asked to start. |
| `REQUEST_STOP` | User asked to stop. Always ends a shot; does not demote to rinse. |
| `REQUEST_RINSE` | Machine classified a rinse gesture. |
| `HOLD_ACTIVE` | User is still requesting brew. |
| `STABLE_IDLE` | User is idle long enough to leave `REQUIRES_OFF`. |

The machine owns debounce and bounce-safety. The brew orchestrator only sees
`HOLD_ACTIVE` vs `STABLE_IDLE`.

---

## 5. Weight control (`WeightControlState`)

**Purpose.** Whether **this cycle** may use the pan to stop. A dropped
link or silent notifications must not slam machine circuit open; they
**suspend** automation and let A→M / paddle / walls decide. Brew
rejecting a sample is not a lost scale.

Source: `OpenBrewByWeightScaleTypes.h`, `setWeightControlState()` in
`openBrewByWeight.cpp`.

### States

| State | Meaning |
| --- | --- |
| `INACTIVE` | This cycle will not stop on weight (no scale at start, or timer-only). |
| `VALIDATING` | Last sample was implausible (range, slew, reconnect). Need coherent recovery samples before trusting a cut. Recovery may land below the last accepted weight, so releasing a heavy touch returns control to the pre-anomaly trajectory. |
| `ACTIVE` | Samples are accepted into the trajectory; threshold/guards may cut. |
| `SUSPENDED` | Link dropped, connection generation changed, or **notifications went silent** mid-shot (`observedWeight` older than 1 s). Rejected brew samples and unchanged grams do not suspend. A→M becomes **enforced** if it was armed. Reconnect with three coherent samples can return to `ACTIVE`. |
| `FAULT_STOPPED` | Weight path gave up for this cycle (e.g. persistent overload policy). No further automatic cut from weight. |

### Events

| Event | Effect |
| --- | --- |
| Cycle start with usable scale + BBW | `INACTIVE`/`—` → `ACTIVE`. |
| Cycle start without scale or timer-only | Stays `INACTIVE`. |
| Notifications silent / disconnect / generation change | `ACTIVE`/`VALIDATING` → `SUSPENDED` (A→M armed → enforced). Parsed packets keep control off this path even if brew rejects them. |
| Three coherent recovery samples | `SUSPENDED`/`VALIDATING` → `ACTIVE` (A→M enforcement cleared). Only `SUSPENDED` also requires the samples not to fall more than 2 g below the last accepted weight, because a stream gap can hide a cup lift. |
| Out-of-range / overload sample | `ACTIVE` → `VALIDATING`, stream `OVERLOAD`. |
| Shot weight below −max(`minimumCupWeightG`, 10 g) | `ACTIVE` → `VALIDATING`, the sample rejected as range against that floor (as slew when it also broke the slew limit); a below-empty sample that completes recovery is rejected (`SCALE_SAMPLE_REJECTED_RANGE`, limit −max(minimum cup, 10 g)) and moves `VALIDATING` → `SUSPENDED` (A→M armed → enforced) instead of `ACTIVE`, so control never accepts such a weight. |
| Tare / post-tare grace | May promote `VALIDATING` → `ACTIVE` so the new zero is used. If the shot-start grace expires while the latest reading is still within half a minimum cup of the pre-tare load (at least the minimum cup), that load becomes a software zero subtracted from shot samples, first flow restarts on it, and the cup is restored as an untared presence (`TARE_BASELINE_TIMEOUT` carries the zero). While that software zero stands, within one more grace after the deadline, on the connection that acknowledged a successful shot-start tare write and outside `SUSPENDED`, two consecutive raw samples within half a minimum cup of zero that would have passed as the grace baseline, not claimed by a cup-removal stop, are the late zero: the first is held, the second replaces the software zero with minus the last accepted shot weight (never below 0 g), so control continues from the coffee already weighed (`SCALE_LATE_TARE_RECOGNIZED`). The dropped zero is remembered: a lift of a cup the detector never saw reads the same, and while the shot tracks weight on that connection two consecutive raw samples within half a minimum cup of that zero plus the last accepted shot weight (the cup and the coffee it held, a step a real scale zero never adds) are the cup coming back: the first is held, the second restores the dropped software zero, marks a curve break and makes the cycle calibration-ineligible (`SCALE_LATE_TARE_UNDONE`, WARNING); cup removal is not applied after the fact. A new tare (baseline window armed) forgets the dropped zero. Any other drop stays a lift for cup removal or the below-empty rule. |

---

## 6. Weight stream (`WeightStreamState`)

**Purpose.** Quality of the **latest parsed notification**, for
diagnostics and for whether A→M may treat the scale as lost. Not the
same as weight control (accepted grams for a cut): the stream can be
`FRESH` while control is `VALIDATING` on slew, or `STALE` while
control is still `ACTIVE` until the next loop notices. Control
`SUSPENDED` after a disconnect can also overlap a still-`FRESH`
observed sample until the 1 s timer elapses.

Source: `OpenBrewByWeightScaleTypes.h`.

### States

| State | Meaning |
| --- | --- |
| `NO_SAMPLE` | No usable weight this boot / this link. |
| `FRESH` | Last **parsed** notification (`observedWeight`) is within `MAX_AUTOMATION_WEIGHT_AGE_MS` (1 s) on the current connection generation. Brew accept/reject does not change this. |
| `STALE` | No parsed notification within 1 s, or the link dropped, during `BREW` while control was still tracking. Diagnostics **Recovered stales** / **Stale time** count only live stream → STALE → live recoveries on the same BLE link; stales that end in disconnect are discarded. |
| `ANOMALOUS` | Sample failed slew/plausibility vs the accepted trajectory. |
| `OVERLOAD` | Absolute weight outside the automation window (pan slammed or protocol glitch). |

### Events

New BLE `WEIGHT` samples, connection generation changes, and the
1 s **notification** freshness timer. Overload/anomaly are classified in
`recordWeightSampleWithProvenance` and do not by themselves mark the
stream `STALE`.

---

## 7. Cup presence (`CupPresenceState`)

**Purpose.** Infer cup on / cup off from weight **hysteresis**, so a
late cup can retare and a lifted cup can stop. A tare must **not**
flip presence (the zero moves; the cup did not leave).

Source: `OpenBrewByWeightScaleTypes.h`, `OpenBrewByWeightCupPresence.h`.

### States

| State | Meaning |
| --- | --- |
| `ABSENT` | No stable mass ≥ **Minimum cup weight** (default 10 g). |
| `PRESENT` | Stable increase above the qualified absent reference of at least that threshold; absolute load is used only when no initial absent reference exists. |

Transitions can be **held** during post-tare grace so the zeroing
transient is not a remove/place.

### Events

| Event | Meaning |
| --- | --- |
| `NONE` | Sample did not complete a transition (still counting stability or confirmations). |
| `PLACED` | ABSENT → PRESENT after the shared stable-sample/time requirements. Drives late retare inside its shot window, or independent idle tare on an eligible new placement. |
| `REMOVED` | PRESENT → ABSENT after consecutive samples below **Cup removed** (default −3 g). May set `cupRemovedPending` on the brew orchestrator. |

Placement retains an occupied reference and identity. For a negative occupied
plateau, the removal threshold applies to a further drop from that reference,
so repeated unchanged negative readings cannot emit another REMOVED event.
Stability uses the whole candidate's maximum-minus-minimum spread. Tare
notification rebases the reference to zero without changing PRESENT/ABSENT.
An unconfirmed effect can mark the reference uncertain; PRESENT alone then
does not satisfy the cup-start guard. Confirmed removal clears uncertainty.
Diagnostic tare results apply only to their connection generation: success
rebases presence to zero, failure marks it uncertain, and both discard mass and
sample evidence. An idle placement pending at that reference change is cancelled
and its presence discarded; it must qualify again from fresh samples.

Informational cup weight is independent of the mutable occupied reference.
The FSM keeps a qualified `ABSENT` reference and stable placement `PRESENT`
record with qualification times, plus their difference for the current placement. The
absent reading must qualify the shared stability window before placement starts.
After REMOVED, a new absent plateau or qualified idle unload with a trusted
anchor is required before replacement can qualify; the negative-hole minimum
is diagnostic only. Placement and queued tare
validation use absent plus minimum cup weight. An established absent record
survives transient intermediate weights while the next plateau qualifies.
An independent empty anchor preserves the coordinate through freshness loss and,
after a qualified placement, does not move to another stable plateau while ABSENT. A downward departure beyond
placement tolerance revokes absence qualification until samples settle back at
the anchor; upward intermediate placement readings do not redefine it. Without
an anchor or observed removal, initial acquisition is restricted to
±min(5 g, minimum cup weight / 2). Initial positive absolute presence remains available without
mass or idle tare; negative boot offsets cannot authorize relative placement.
Before a qualified placement, a transient negative departure from a qualified zero
may discard the current anchor while a stable negative reference qualifies.
A return inside the initial zero band restores the observed empty offset through
the unchanged absent stability window, without PLACED or tare. A positive
return outside that band can also restore the historical near-zero
anchor before the first qualified idle placement if its increase from that
still-valid anchor is strictly below minimum cup mass. It does not acquire a
new empty offset or emit PLACED/tare; the next load still requires normal cup
stability. Missing initial zero, lost sample evidence, and unknown negative
references cannot use this recovery. An unqualified
absolute startup placement does not close this idle recovery path; a qualified
placement, known occupied tare or uncertain tare does. Initial small shifts are
bounded against zero, not accumulated against each successive anchor. The fast
unload path cannot bypass initial recovery stability. In-shot recovery retains
its existing zero band and placement rules. Unknown initial negative offsets and unqualified
rebounds away from that zero remain ambiguous and require diagnostic empty tare.
For an unqualified rebound, readiness reports that recovery action instead of
waiting indefinitely for settling.
`PLACED` publishes
present minus absent, including when tare is disabled. Firmware tare and coffee
do not overwrite these placement records or the calculated mass. Pending tare
IDs track uncertainty; successful matching outcomes preserve known mass rather
than acquiring new mass from net weight. No baseline is learned across a pending
tare. Removal, reset, connection changes, stale/invalid samples, lost evidence,
and uncertain outcomes invalidate the measurement. Sample gaps beyond the
configured stability maximum clear qualification, not the numerical anchor.
A matching successful tracked tare translates the anchor by the latest
control-approved reading captured at the write boundary, with matching packet
sequence and connection. The enqueue reading is not a frozen tare offset;
missing/unvalidated pre-write evidence, overlapping, timed-out or uncertain tares
discard that authority.
Diagnostic reference changes also discard it. Original placement records and mass
are not rewritten by this translation. An unknown anchor may be reacquired from
fresh stable absence after an observed removal.
Raw reference acquisition
uses parsed sensing bounds, including valid negative readings below the automation
floor; placement automation and brew acceptance retain their existing bounds.
The calculated mass is informational; the shared absent record qualifies placement.
Freshness/order validation gates the cup FSM as well as mass acquisition in idle
and active cycles. Samples over 1000 ms old, future-dated, duplicated or out of
order, and non-finite/out-of-parsed-range weights, reset placement and removal
confirmation evidence and cannot emit PLACED/REMOVED or trigger retare.
Outside an active cycle, two consecutive fresh near-empty samples can qualify
unloading against a trusted anchor without a stable-empty pause. The band is
`min(placement tolerance, 0.5 g, minimum cup mass / 2)` above that anchor; samples
must meet both the configured gap and removal confirmation window. This evidence
is distinct from a stable absent record and is consumed by the next qualified
placement. It is reset by lost/invalid samples, config/reference/connection changes,
and tare/transition holds. The occupied stable window and relative minimum mass
remain required. No fast authorization crosses an active cycle or pending tare.
Idle tare accepts this path or stable absence, never one ABSENT sample or a
stable-to-stable load difference alone. In-shot confirmation remains unchanged.
See [Displayed cup weight](settings/cup.md#displayed-cup-weight).

For idle accessory retare, a positive addition qualifies only as a step: the
first reading at or above the minimum cup weight must arrive within one maximum
sample gap of the last reading at or below half that minimum. That decision is
made once per crossing; later stability-window resets do not repeat it. The
peak during the first gap after the crossing bounds the settled load (at most
peak + half the minimum), and the step expires 3 s after the crossing. A queued
accessory tare released as unstable before its write may be requested again.
A separately measured accessory mass can identify
its later negative removal. That matching partial unload leaves PRESENT in place
until the normal stability window qualifies a retare. Removing the cup or the
whole load still emits REMOVED. Similar component masses, an unknown component
of a combined placement, stale evidence, or a failed write do not authorize
negative retare; full unloading to the known empty reference re-arms placement.
The active-shot removal rule is unchanged.

The shot-start resync preserves known tared PRESENT at zero. Require-cup checks
consume that state, not a positive net-weight threshold. Untared lift-to-zero
still removes the cup. Idle eligibility/connection/config changes reset the
placement candidate, not PRESENT. Ending a shot never creates a placement.

Idle tare requires current-connection absence evidence followed by PLACED while
READY, or after a normal shot stop in REQUIRES_OFF awaiting paddle release,
with machine run state `IDLE` and no active cycle, safety trip, or maintenance.
A placement outside eligibility is consumed, not deferred. Observed removal or
placement during drip finalization commits captured last-known weight instead of
the replacement cup, independently of the idle-tare setting. One fresh credible
near-empty sample can discard optional post-drip substitution/learning without
emitting a cup event or authorizing tare. Entirely unsampled motion remains
unobservable from final weight alone.

---

## 8. First flow (`FirstFlowPhase` / `FirstFlowClass`)

**Purpose.** Distinguish likely first coffee from a finger or cup put-down
using weight evidence; sustained pressure can still resemble coffee. It does
**not** stop the shot.

Source: `OpenBrewByWeightScaleTypes.h` (`stepFirstFlow`).

### States (`FirstFlowPhase`)

| State | Meaning |
| --- | --- |
| `SEEKING` | Looking for a small step above tare zero (default 0.3 g) that is below cup mass and below a 2 g finger step. |
| `TOUCH` | A ≥2 g jump faster than 8 g/s (a slower step across a packet gap is coffee that arrived while the stream was silent) or cup-sized mass; wait for release or leftover coffee. Coffee that keeps rising at least 2 g above the jump fires while the reading stays below cup mass or less than 4 g above the jump, so a small minimum cup weight or a large onset step still reaches `FIRE`. |

### Events / classes (`FirstFlowClass`)

| Class | Meaning |
| --- | --- |
| `NONE` | Still at baseline. |
| `CANDIDATE` | First confirming coffee-sized sample. |
| `TOUCH` | Finger/cup jump; do not fire first drop. |
| `FIRE` | Confirmed first drops (two consecutive coffee-sized samples, or leftover after a touch that was not cup mass). |

`FIRE` is emitted when the qualifying sequence is confirmed, but the recorded
first-drop time and weight come from the first sample in that uninterrupted
sequence. A packet or timing gap starts a new sequence, and so does any
downward step: vibration alternates sign, flow does not. The confirmation
counter saturates, so a long sub-rate creep keeps its first sample as the
candidate.

A qualified late PLACED event remains eligible after FIRE within the original
retare window. The old timestamp/curve marker survives admission and write
failure. Matching successful completion plus a fresh near-zero sample captured
after the write boundary, within the completion-anchored baseline grace,
and, when the tared load was at least the minimum cup weight, at least half
that minimum below it (the same rule as the shot-start baseline),
invalidates that record and resets the same detector. Control retains one
near-zero sample while completion is pending, including one observed during
the write. Pre-write, wrong-link, expired, or post-end samples cannot confirm.
Pre-end evidence processed during pending finalization corrects its snapshot
and matching last shot; persisted history is written once with corrected data.
Without replacement FIRE, first drop and its dependent metrics remain missing.
The historical first-flow observation remains latched for machine sensing and
once-per-cycle webhook publication; correcting the statistic does not revoke
that observation or replay the beep. Tare time remains a separate successful
command-completion diagnostic.

---

## 9. Accidental touch (`AccidentalTouchPhase` / `AccidentalTouchClass`)

**Purpose.** Hold automatic **weight stop** if the pan is being poked,
without ending the shot. Complements BBW protection (time window) with
a live residual check against the accepted flow trend.

Source: `OpenBrewByWeightScaleTypes.h`. Only runs while weight control is
`ACTIVE` and the setting is on. A step large enough to trip the slew guard
first puts control in `VALIDATING`; when three coherent samples recover it,
the pre-anomaly anchor and trend are kept, so the recovered level is
classified here as a step (heavy and light touches share the hold/fallback
path) instead of being accepted as a new plateau. Recovery from `SUSPENDED`
re-anchors at the recovered level.

Each sample is judged by its **residual** against the accepted trend:
`residual = w − (w_ref + m · (t − t_ref))`. The reference is the newest
accepted point at least 0.5 s older than the anchor (last accepted sample),
so it moves only when the anchor does: held samples never drag it. The flow
`m` is the secant of the accepted points over the second ending at the
reference (with younger history, the secant up to the newest point; never
negative). Neither reaches behind the last accepted level shift or the
first drop, where the flat pre-flow trend ends. A sample is anomalous at
`|residual| ≥ 1.5 g`; once holding it stays anomalous down to 1.0 g
(hysteresis). An empty trajectory (pre-cycle anchor) is never judged.
When a hold begins, the anchor's own sample leaves the trend if it was already
1.0 g or more off it in the same direction: a contact's leading edge cannot
later become the reference for clean flow.

### States (`AccidentalTouchPhase`)

| State | Meaning |
| --- | --- |
| `STARTUP` | No accepted point is yet 0.5 s older than the anchor (start of shot, first drop, or right after a level shift): the anchor is the reference. |
| `TREND` | A lagged reference exists; it stays frozen while a hold lasts. |

### Classes

| Class | Meaning |
| --- | --- |
| `OK` | Sample looks like brew flow. Stop logic may run. |
| `TOUCH` | Anomalous sample; **hold** cuts (`accidentalTouchHolding`). It is not accepted into the trajectory. A plateau 5 g or more off the trend stays `TOUCH` (a resting load, not a spurt). |
| `SUSTAINED` | An accepted level shift: ten held residuals within 1 g of each other and under 5 g (one second of a flat offset riding on the flow), or any hold that reaches 2.5 s. The sample is accepted as weight, starts a new trend epoch and curve segment, and the cycle becomes ineligible for offset learning. Above the cut line, with the touch fallback effective, it is reclassified `TOUCH` and counts as fallback evidence instead of allowing a direct cut. |

While holding, a sample above the cut line also stays `TOUCH` when the touch
fallback is effective, so a hold never releases into a direct cut there; it
releases when the reading returns below the line. Below the line (or with the
fallback off) a hold releases when the reading returns within 1.0 g of the
trend, or as `SUSTAINED` after the bounded 2.5 s hold, so held mass that never
lifts is accepted as weight and can still end the shot instead of letting the
cup overflow.

The default-on, per-preset `touchStopFallbackEnabled` option is effective only
with `avoidAccidentalTouchEnabled`. It does not modify this classifier. A
`TOUCH` that blocks an otherwise eligible measured-weight stop starts bounded
session evidence: at least three consecutive packet IDs in one connection
generation, strictly increasing receive times, gaps no greater than 1000 ms,
and at least 1000 ms above the cut line minus the drip offset. Normal release
can stop sooner. Rejected non-touch data, reference/link discontinuity, expired
freshness, lost eligibility and a changed cut-line segment cannot carry
evidence forward; the line itself moves along a ramp, so only its segment is
compared and f is re-read when the fallback fires. Machine automation
permission still applies. Segment timing uses each sample's receive time, so a
delayed sample cannot count under a segment that began only after its arrival.
The fallback requests `TOUCH_WEIGHT_FALLBACK` through normal control arbitration,
does not accept the rejected sample into the trajectory, and disables offset
and A→M duration learning. Saved ON with parent OFF is dormant, not effective ON.

---

## 10. Scale link (`ScaleLinkState`)

**Purpose.** BLE session with the preferred / discovered scale.
Connects the weight path; **never** drives the machine circuit.

Source: `openBrewByWeight.cpp` (`ScaleLinkState`).

### States

| State | Meaning |
| --- | --- |
| `DISCONNECTED` | No GATT session. Discovery/reconnect may be running. |
| `CONNECTED` | Notifications flowing (or about to). Blue LED may follow this if enabled. |

A `connectionGeneration` and `disconnectSequence` ride on the snapshot
so a *new* connection cannot be mistaken for the one that started the
shot (that is what suspends weight control). The library also owns a 3,000 ms
quiet interval. Its GAP disconnect callback immediately rejects new procedures
and old-link weight events. The full interval starts in that callback, or as
soon as an already-admitted radio submission returns; discovery becomes
eligible only after the interval expires.

### Scale commands (`ScaleCommandType`) — outbound

| Command | Meaning |
| --- | --- |
| `START_TIMER_AND_TARE` | Shot start: tare (if enabled) and start the scale timer. |
| `TARE_ONLY` | Late-cup retare, or an idle placement request carrying its own nonzero request ID and expiry. Never starts a timer. |
| `STOP_TIMER` | After machine circuit opens, once the scale timer has reached the internal whole-second time (or the 2 s catch-up cap). Optional extra delay is a pad after that. Queued to the **front**. |

### Scale events (`ScaleEventType`) — inbound

| Event | Meaning |
| --- | --- |
| `WEIGHT` | Notification with grams (+ optional timer). Feeds stream/cup/flow/touch. |
| `TIMER_START_RESULT` | Write/feedback for start+tare. |
| `TARE_RESULT` | Tare write result. An idle request ID routes completion away from shot baseline/retare state. |
| `TIMER_STOP_RESULT` | Write/feedback for stop (`TimerStopResult` tracks pending/success/fail). |

The worker owns `IdleTareStatus` under a short task mutex: NONE → QUEUED →
WRITING → SUCCEEDED/FAILED. Cancellation and claiming a queued request are
mutually exclusive; cancellation cannot clear WRITING. No lock spans ATT.
Terminal status survives a dropped event. Control owns placement provenance
and gives an accepted physical shot start priority over this automation. A
queued request is canceled; if the worker already owns the ATT write, it keeps
ownership and may finish while the shot starts normally. Its idle request ID
routes terminal/effect cleanup away from the new `CycleSession`, whose own
`START_TIMER_AND_TARE` remains identified by its cycle ID. Activator forwarding
continues to obey start guards but does not wait for idle tare. STOP queue
priority remains unchanged.

Control revokes a cycle's start/reset/tare admission before finalizing it or
cleaning up a failed start. The worker checks that permission after BLE command
spacing, before each operation. Commands already admitted may finish; STOP is
still allowed for an ended cycle and retains its queue priority.

The control owner validates new samples while QUEUED and publishes their last
approved sequence under the request mutex. A worker seeing newer published
samples requeues once per existing tick until control validates or cancels;
the original expiry remains. The final harvest, expiry check, and claim occur
after BLE command spacing. The library rechecks the claimed notification boundary
at radio admission; if it changed, no write is sent and the idle request returns
to QUEUED. Claim records notification capture sequence/time.
Control separates transport completion from post-boundary zero evidence,
rolls back a provisional reference after failure, and preserves or invalidates
the prior reference at the existing settling deadline. A new placement cannot
inherit an old request's result. Disabling the feature after a successful write
still allows the pending reference to settle within that bound.

Weight events preserve BLE capture time/sequence and use a fixed 16-event FIFO.
Overflow clears the lost window and marks discontinuity on the retained newest
sample; cup confirmation/stability cannot span that gap. Negative removal
evidence is distinct from the narrower placement/automation weight range.
Both existing drain checkpoints and the 33-event per-call cap remain unchanged.
Command results precede buffered weights. Successful shot tares install their
capture boundary once per request ID; duplicate or older results do not reset
post-tare evidence. Samples at or before that boundary belong to the old
reference and are excluded from cup, first-flow, and shot-trajectory processing.

Idle tare uses the existing cup reference notification without freezing the
cup FSM. Negative removal evidence remains active during writing. A physical
lift entirely hidden by a simultaneous scale re-zero, or occurring entirely
between notifications, cannot be reconstructed from weight alone. Physical
button tare is not currently exposed as a verifiable protocol event.

Preferred-scale policy (`FIRST` / `PREFER` / `ONLY`) is a **setting**,
not a state machine; it only filters which peripheral may enter
`CONNECTED`.

---

## 11. No-scale BBW guard

**Purpose.** Select whether missing-scale BBW attempts are allowed, warned
once, or blocked until a scale is usable. In **Require a scale**, ordinary shot
and rinse gestures stay blocked; the three-cycle emergency gesture temporarily
allows manual use. See
[No-scale BBW](settings/no-scale-bbw.md).

The brew orchestrator, not the guard, pushes `machineSetActivatorDriveAllowed` so
momentary does not 1:1-forward while this guard (or cup-start) would
block. Paddle's ON-level refresh does not bypass start permission; `beginCycle`
still withholds `machineRequestStart` when blocked.

`NoScaleBbwMode` selects `OFF`, `WARN_ONCE`, or `REQUIRE_SCALE`; the runtime
still exposes the Armed/cooldown latch.

### States

| State (UI) | Meaning |
| --- | --- |
| Off | Setting disabled or BBW off. Guard does nothing. |
| Armed | Next long activator ON is blocked (machine circuit stays open, local buzzer cue). |
| Temporarily allowed | Warn-once guard consumed or Require-scale bypass accepted. Re-arms on scale connect, boot, or **Protection returns after**. |
| Scale required | Strict mode is armed and no usable scale exists; shot and rinse remain blocked. |
| Ready | Strict mode is configured and the scale is usable. |

### Events

| Event | Effect |
| --- | --- |
| Scale becomes usable | Arm immediately. |
| Activator while Warn once is Armed | Block and consume → Temporarily allowed. Circuit stays open. |
| Activator while Require scale is blocking | Block without consuming; release is required before a later valid start. |
| Three complete ON→OFF cycles within 2 s while Require scale is blocking | Move to Temporarily allowed, play the scale-connected melody, and consume the final release so it cannot start a shot. |
| Short ON→OFF / idle long-press (rinse) while Armed | Default: no `RINSE`, circuit stays open, consume → Temporarily allowed. If **Allow rinse while Armed** is on: rinse runs; consume → Idle. Quick rinse off: paddle short ON→OFF ends without `RINSE`; momentary long-press is not forwarded. |
| Shot (non-rinse) ends | Idle; cooldown then Arm. |
| Cooldown elapsed / boot | Arm. |

---

## 12. Recovery gesture

**Purpose.** Restore network or factory-reset when Wi-Fi, Web UI, BLE,
and USB are all unusable. **machine circuit stays open** for the whole window.

Source: `OpenBrewByWeightRecoveryGesture.h`. Entry: power-on reset **and**
paddle already stably ON **and** still held 30 s later (entry grace below).

### Entry grace (`RecoveryGraceWindow`)

A power-on hold is not by itself a recovery intent, and the gesture is
irreversible, so entry passes a 30 s grace first. The grace runs inside the
blocking boot-recovery region — persistence, BLE, Wi-Fi, scale worker, and
machine integration stay unloaded — with the stopper in `REQUIRES_OFF` and
the machine circuit open. Every tick services relay safety, the buzzer, the
task watchdog, and the safety heartbeat. Activator drive stays denied from
`machineInitialize()` until the first control-loop pass grants it, so a held
momentary button sampled here never closes K1 or trips an initialization
lockout.

| Result | Meaning |
| --- | --- |
| `HOLDING` | Grace running; nothing due this tick. |
| `BEEP_DUE` | One `PADDLE_REMINDER` double beep at seconds 0, 5, 10, 15, 20, 25. A late caller gets one cue, never a replay burst. |
| `ABORTED` | Activator released: recovery entry cancelled for this boot; normal startup continues. Release wins over expiry on a coincident tick. |
| `ELAPSED` | Held the full 30 s: `RECOVERY_START` announces the 60 s gesture window. Second 30 itself does not beep. |

### Internal flags (not published)

| Flag | Meaning |
| --- | --- |
| `active` | Inside the 60 s recovery window. |
| `attemptActive` | Counting OFF→ON cycles after the first OFF. |

### Results (`RecoveryGestureResult`)

| Result | Meaning |
| --- | --- |
| `NONE` | Still listening. |
| `NETWORK_ACCESS_RESET` | Three complete OFF→ON cycles, then 3 s confirmation with paddle ON. Restores AP / device-password access. |
| `FACTORY_RESET` | Five cycles + confirmation. Erases settings and history. |
| `TIMED_OUT` | 60 s elapsed with no confirmed gesture. |

Cycle counting must finish inside a 5 s movement window; confirmation
is a separate 3 s hold. See [Emergency recovery](EMERGENCY_RECOVERY.md).

---

## 13. Station Wi-Fi (`StaState`)

**Purpose.** Join the configured home network without fighting BLE
mid-shot. SoftAP auto-raise is **boot/bootstrap only**, before the first successful
STA join. Auto SoftAP also idle-stops after 3 minutes with zero SoftAP
stations (latched for the rest of the boot; USB `AP_START` still works).

Source: `OpenBrewByWeightNetwork.h`.

### States

| State | Meaning |
| --- | --- |
| `NOT_CONFIGURED` | No saved STA credentials. SoftAP at boot with idle shutdown. |
| `CONNECTING` | `WiFi.begin` in flight (25 s connect timeout). |
| `CONNECTED` | STA has a link. NTP may arm. SoftAP auto-raise is latched off for this boot. |
| `FAILED` | Connect timed out or was rejected. SoftAP may raise once if STA never joined this boot; then retry / recovery timers apply. |
| `DISCONNECTED` | Had a link (or was configured) and lost it. Reconnect interval 10 s; SoftAP does **not** come back automatically after `staEverConnected_`. |

### Events

Save/clear credentials, DHCP vs static apply, connect timeout, link
down, `AP_START` from USB CLI, maintenance holds that pause reconnect
during a shot or scan.

### Pending config (`StaConfigState`)

| State | Meaning |
| --- | --- |
| `CONFIRMED` | Current STA addressing is committed. |
| `PENDING` | A new static IP (or similar) is on probation (~3 min). Unreachable config **reverts**. |

---

## 14. Wi-Fi scan (`WifiScanState`)

**Purpose.** Fill the Web UI network list. Scanning is RF-heavy, so it
takes a **maintenance lease**: machine circuit is forced open for the scan.

| State | Meaning |
| --- | --- |
| `IDLE` | Nothing queued. |
| `QUEUED` | POST `/api/v1/network/scan` accepted; waiting for the network task. |
| `RUNNING` | IDF scan in progress (20 s timeout). |
| `READY` | Results captured (`WifiScanSnapshot`). |
| `FAILED` | Scan error. |
| `CANCELED` | Aborted (timeout, paddle during lease, restart). |

---

## 15. Wall clock (`TimeSyncState`)

**Purpose.** Timestamp shot history. Brewing does **not** wait on NTP.

Source: `OpenBrewByWeightTime.h`.

| State | Meaning |
| --- | --- |
| `OFF` | NTP not armed (no STA, or disabled). |
| `SYNCING` | SNTP request in flight. |
| `SYNCED` | Valid UTC anchor; local time = anchor + monotonic. |
| `FAILED` | Sync attempt failed; retry delay applies. |
| `STALE` | Was synced, but last success is older than `NTP_STALE_AFTER_MS` (derived in the snapshot, not a stored latch). |

NTP does **not** arm while brew RF is active (`activeCycle` or machine
circuit closed) or while the scale is **connecting**. An in-flight SNTP
attempt is aborted (`stopNtp` + `cancelSyncing`) without counting a
failure; `ntpRearmPending` retries when the gate clears.

Shot rows without a sync store `hasWallTime = 0` and show “no time” in
the UI.

---

## 16. OTA (`OtaState`)

**Purpose.** Dual-slot Wi-Fi update that cannot brick the only bootable
image. Machine circuit stays open. Transfer writes the **inactive** slot; boot
selection changes only on an explicit flash/commit.

Source: `OpenBrewByWeightOta.h`. Product notes: [OTA](features/ota.md).

### States

| State | Meaning |
| --- | --- |
| `UNAVAILABLE` | Partition table has no spare app slot (USB-only updates). |
| `IDLE` | Ready for an upload (after the running image has confirmed itself). |
| `RECEIVING` | Bytes streaming into the spare slot. |
| `STAGED` | Image passed header, identity, arch, and SHA-256. Waiting for the operator to flash. |
| `COMMITTED` | Boot partition switched; restart pending. Next boot is `PENDING_VERIFY` until HTTP proves the Web UI, or until the 180 s deadline (rollback if a previous slot is bootable; otherwise the running image is kept). |

`OtaResult` values (`BUSY`, `PENDING_VERIFY`, `DOWNGRADE`, …) are
**command outcomes**, not states. A second upload while the new image
has not confirmed HTTP is refused.

---

## 17. Web command pipeline (`CommandResultState`)

**Purpose.** The controller handles **one** mutating request at a time.
The Web UI shows an action-specific success or failure toast when the
HTTP handler accepts or rejects the command, not when NVS is done.
Configuration and preset saves are the exception: they wait for their own
request's `APPLIED` (presets: `PERSISTED`) and a new configuration revision,
read from the Home stream that every page keeps open. While that stream is
down, the Admin page's REST status stands in, backing off from 250 ms to 1 s;
Settings has no REST status and waits for the stream to return.

Source: `OpenBrewByWeightDomain.h`.

| State | Meaning |
| --- | --- |
| `NONE` | No command in flight for this id. |
| `QUEUED` | HTTP accepted; waiting for the control or network task. |
| `RESERVED` | Maintenance lease held (scan, some admin). Paddle cancels → `CANCELED`. |
| `APPLIED` | Runtime updated (RAM). Persist may still be pending. |
| `PERSISTED` | Durable store committed. |
| `FAILED` | Validation, queue, or persist error. |
| `CANCELED` | Lease dropped (e.g. paddle ON during maintenance). |

The development-only unsafe Web UI override is **not** a state machine: it is a flag on the
queued command (`unsafeWebUiOverride`) that bypasses the config-lock
gate. It never changes relay safety.

---

## Supervised backflush

The optional machine contract supplies normalized ordered observations, never
actuation commands. An observed inactive→awaiting episode and a new physical
paddle edge admit `BACKFLUSH_CANDIDATE` before brew/scale side effects. Initial
awaiting can admit a 60-second candidate but cannot authorize promotion. A live,
eligible post-edge active confirmation enters `BACKFLUSH_RUNNING`, with a single
ceiling of original close +180 seconds. There is no local app-window timer.

Physical OFF, supervision loss or a safety trip opens first. Inactive during
candidate opens and records Other; any non-active state after confirmation
opens and records Backflush. Both finish in `REQUIRES_OFF`, without a brew
session, tare/timer commands, shot statistics or automatic restart. The
electrical history duration and final classification are appended once.
An unresolved/consumed request blocks a new hold until observed inactive and a
new request; only control drains the bounded transition handoff. Loss and
first-ingress provenance survive batching and cannot be repaired by a pong.

## Linea Micra power observer and wake qualification

Micra builds run a separate cloud worker when **Monitor machine power state**
is enabled and an account machine is selected. WebSocket is the persisted default;
API selection queues HTTPS dashboard reads on a nominal 30-second cadence.
Commands and authentication always use HTTP. At startup it waits for an eligible STA
connection, a closed setup AP, no admitted shot with a connected scale, and a
synchronized wall clock, then opens a WebSocket subscription or starts the initial API read on
the next worker opportunity. Each admitted WebSocket subscription queues one
initial dashboard cycle while push reception continues. Explicit refresh retains its HTTP path. An accepted temperature or power
command is verified by a push on the live subscription within
`kPushConfirmMs` (10 s); only then does verification fall back to an HTTPS
dashboard read.
All API purposes, including sign-in and manual requests, remain deferred during
scale acquisition/setup/quiet and shots with a connected scale; active requests
cancel through the owner. Unscaled shots keep API observation and WSS active.
An established WSS subscription stays subscribed through connected-scale shots
and scale acquisition/setup/quiet: no UNSUBSCRIBE is sent and no dashboard read
follows, because pushes keep arriving. Rinse classification or scale loss
clears the shot transport pause for HTTP. Wake and
backflush gestures keep both transports available regardless of scale presence.
New WSS setup and reconnection wait for the shot and scale gates. A due access
token is renewed over HTTP while the subscription stays open; renewal never
reconnects the socket.
Power commands are canceled when a shot starts, including previously queued
commands; they require a new trigger after the shot. Temperature and power
commands are skipped without network I/O when a WebSocket-mode CURRENT status
already holds the requested value from evidence accepted on the live
subscription (`temperatureTrusted`/`powerTrusted`, cleared by every fence
invalidation, including rejected pushes). A cloud-offline machine defers a
temperature target locally and drops power commands. Exhausted command cycles
stop after `kMaxCommandCycles` (3) while the machine is online. When initialization coalesces
with post-wake reconciliation, reconciliation uses the request-time field
baseline even if wake preceded the subscription.
A current
`StandBy` response maps to OFF, `BrewingMode` to ON, and ECO or an unknown value
to UNKNOWN. API-mode evidence older than 30 seconds remains the last cloud
classification but its quality becomes stale. WebSocket-mode evidence stays
current without an age deadline while its subscription remains synchronized.
Power source distinguishes the initial API read, ordinary API reads and push;
it changes only when power is accepted. An unsupported response or an exhausted
API-mode communication cycle can replace power with UNKNOWN; the separate
`effectiveOn` policy treats UNKNOWN as ON without claiming that ON was measured.

WebSocket transitions through waiting for network/time, authentication, socket
connect, STOMP connect, waiting for data and streaming. Failure uses backoff;
invalid authentication requires manual retry/account correction. Network pauses
and maintenance do not clear the authentication failure latch. A shot with a
connected scale or maintenance moves it through STOPPING to PAUSED after actual
stop/destroy. Scale detection/setup/quiet only defers new setup and reconnection.
No live paused socket or automatic SDK reconnect is retained.

The La Marzocco route (`companion_.status.route`, owned by the Micra service,
[companion link](COMPANION_LINK.md)) is DIRECT, PENDING or COMPANION. With no
companion link it stays DIRECT and the cloud worker skips it without taking a
lock, so every path above runs unchanged. DIRECT → PENDING is taken only by the
cloud worker at a quiet point: link up, La Marzocco switch on, observation on,
credentials confirmed for the current generation, upstream subscribed and `up`,
not flapping, and no shot, scale connection or quiet interval, cleaning episode,
queued or running command, or queued request. While PENDING the worker leaves
the direct socket untouched (it keeps streaming). The companion's `full` frame
moves PENDING → COMPANION on the link task in one locked step: it ends the
socket's epoch, so later socket frames are dropped, then applies the frame as a
new subscription plus its initial read; the worker then stops the obsolete
socket and frees the HTTPS session. If that frame finds a shot, scale
connection, cleaning episode or command, the route goes back to DIRECT
(`busy`). On COMPANION the controller makes no La Marzocco request: `push`
frames run the socket's push body, `read` frames the HTTPS read's merge (dropped
during a scaled shot or scale connection), the post-wake and API schedules
settle from that state, commands and Refresh become `cmd` frames and an
accepted command is confirmed only by companion state within `kPushConfirmMs`.
An upstream epoch change or lost subscription drops the basis (the socket stop
steps) until the next `full` frame. COMPANION or PENDING → DIRECT on link loss,
no ping or `up` false for 30 s, no `full` frame within 30 s, maintenance, Wi-Fi
loss or AP entry, switch off, rejected credentials, or an account, machine,
transport or observation change. Leaving COMPANION applies the socket stop
steps to the feed (hold power, invalidate fence and cleaning, new epoch) and
the direct client reconnects under its usual admission rule, so never during a
shot with a connected scale, a scale connection or the BLE quiet interval.
More than three losses in ten minutes keep the route DIRECT for five minutes.
The route never changes on its own initiative during a cleaning episode or a
command; only a loss can end it then, and cleaning supervision opens as on a
socket loss. A command in flight when the link drops becomes a transport
failure and its retry runs on the direct route.

On resume, connection/subscription/pong or temperature-only updates do not clear
the separate stale assumed-power hold; only current-epoch known power does.
Machine `connected:false` is offline evidence, never power OFF. Cleaning is a
separate WS-only diagnostic state and cannot create a control transition.
Offline-to-online queues no HTTPS read: the broker's online push carries the
full dashboard, and power absent from it stays stale until a push supplies it.
Connection
revision fencing rejects HTTP results that predate offline, teardown or a new
subscription, including late offline dashboard responses. Online-evidence
revisions also prevent an older API offline result from overriding a repeated
online report. Initialization field
revisions are captured at subscription, so even push received before HTTP
dispatch wins for that field. No repeated reads are scheduled after exhausted
initialization; valid push remains usable even if that API cycle fails.
Matching fresh push evidence clears local optimism early; contrary push evidence
keeps it until the gated 15-second reconciliation or its original expiry. At
that deadline a live subscription with trusted power settles the overlay from
pushed evidence without HTTP; the HTTPS reconciliation read runs only when the
subscription is not live or power is untrusted.
During pause/resynchronization the separate hold retains the effective value
past expiry without modifying the last cloud sample timestamp.

On every physical paddle ON edge, the Micra state owner first evaluates the
pre-edge effective state. A confirmed OFF, current or stale, starts a
60-second optimistic ON overlay, defined as twice the normal poll interval. It makes `effectiveOn` true without
replacing the cloud-confirmed OFF. When a scale shutdown command is accepted
by the cloud over a confirmed ON, current or stale, the owner starts the same
overlay in
the opposite direction: it makes `effectiveOn` false without replacing the
confirmed ON. The owner delays automatic and requested reads for 15 seconds
after either event so cloud state can converge. The first authoritative
dashboard read initiated after the deadline replaces the overlay. Reads
initiated before the event cannot overwrite it or move the deadline. Events over other states (ON, UNKNOWN), a live
overlay, or unsupported samples do not change the tracked state, and repeated
events while an overlay is live never re-arm or extend it.

The observer uses four total attempts with 3/6/9-second waits and bounded
jitter. After exhaustion, the next observation waits 60 seconds. Missing STA,
an open setup AP, an unsynchronized clock, and local shots with a connected scale
are readiness gates rather than failed cloud attempts: requested work
remains pending and starts when the gate clears. Scale BLE runs independently
and never waits for network-owner teardown.

When wake recognition is enabled, the adapter reduces that pre-edge decision
to a generic normal/wake disposition. Open Brew by Weight owns the wake passthrough:
it mirrors only the physical paddle through the existing relay safety authority
and consumes the entire ON/OFF interaction before brew, rinse, guards, scale,
alerts, webhooks, or history. No guard or scale component knows Micra state.

---

## Compatibility mode (firmware master switch)

Compatibility mode is **not** a state machine: it is a boot-scoped mode flag
(`fwmode` NVS record, namespace `fwmode`; an absent, invalid, or unreadable
record means compatibility, so a fresh or factory-reset controller stays
transparent until the owner enables the firmware) read once
in `setup()` before any subsystem initializes. Saving the switch persists the
record and queues the same idle-safe `RESTART` command the Admin Restart
button uses, so a running pour always finishes before the mode flips. There
are no live transitions; every gate reads one immutable-per-boot value.

While active, `stateMachineTask` retains fault handling and the maintenance-lease
block, then `serviceCompatibilityMirror`
replaces the rest of the orchestrator:

- Activator→K1 drive permission bypasses brew guards only in `READY`, with no
  pending relay trip. `REQUIRES_OFF` keeps drive denied during input sampling,
  including momentary mirroring, until a stable OFF input rearms the controller.
- Paddle builds close on the held activator and open on release through
  `machineRequestStart(HARD_MAX_CIRCUIT_CLOSED_MS)` / `machineRequestStop()`;
  momentary builds keep the stock 1:1 switch mirror and simply never receive
  synthetic pulses.
- A safety trip or failed close parks the mirror in `REQUIRES_OFF` until
  the activator returns stably idle — the ON-only mirror never re-closes.

Timer authority in compatibility mode:

| Timer | Authority | In compatibility mode |
| --- | --- | --- |
| BBW operational wall (5–60 s, per preset/policy `machineCloseLimitMs`) | brew feature | suspended |
| Momentary logical-run walls | brew feature | suspended (no logical run is armed) |
| Hardware cap `HARD_MAX_CIRCUIT_CLOSED_MS` (pre-return check + independent ISR) | relay safety | disabled for compatibility mirroring |
| Watchdog, stuck feedback, reset guard, boot-open, LOCKOUT refusal | relay safety | enforced (unchanged) |

For compatibility mirroring, the relay snapshot publishes both duration limits
as zero, meaning disabled. GPTimer and both duration `esp_timer` deadlines stay
unarmed, and software timeout checks and obsolete queued callbacks ignore these
disabled limits. The electrical close timestamp is preserved; the relay is not
periodically reopened or rearmed. A control stall therefore has no duration-timer
cutoff in this mode; the existing watchdog and configured external heartbeat
remain separate protections. Re-enabling full firmware requires restart and
restores normal deadline arming.

Subsystem gates (all boot-scoped, all reusing existing choke points): BLE
scale worker held idle by `applyLiveBleEnabled(false)`; NTP held stopped by the
`serviceNtp` gate (manual sync rejected at the handler); the Micra cloud client
parked through the unconfigured-account path with a cleared session; alerts
muted in `soundAlertsEnabled()` with recovery cues deliberately exempt; webhook
dispatch rejected at `enqueueWebhook`. Presets and every other persisted
setting are untouched; only a factory reset erases them, and it also clears
`fwmode` back to full mode.

Web commands: `processWebCommand` rejects actuation and disabled-feature
commands (`REMOTE_ON`, `REMOTE_OFF`, `RINSE`, `STOP`, `STOP_HEARTBEAT`,
`FORCE_SWITCH_PULSE`, `STATE_OVERRIDE_ON/OFF`, `BLE_SCAN_INTENSITY`,
`RESET_WEIGHT_OFFSET`, `RESET_AUTO_TO_MANUAL_GUARD_SAMPLES`, `BUZZER_TEST`,
`BOOKOO_DEBUG`) with `FAILED` results. Configuration, persistence, network,
restart, OTA, and factory-reset commands keep flowing. The HTTP surface adds
`POST /api/v1/firmware-mode` (`{enabled: bool}`, admin unlock required;
`409 CONFIG_LOCKED_DURING_ACTIVE_CYCLE` while durable flash writes are
deferred, see `networkFlashWriteAllowed`) and
the status envelope carries `compatibilityMode` on every page.

---

## End reasons (brew outcomes)

These are not a machine. They are **why** the last `finalizeCycle`
ran, stored on the session, last-shot blob, and shot log.

| `EndReason` | Typical trigger |
| --- | --- |
| `ACTIVATOR` | Physical stop intention (Natural paddle OFF after any enabled rinse window, or a valid button stop). |
| `SCALE_THRESHOLD` | Weight control `ACTIVE`, target (minus drip offset) confirmed. |
| `WEIGHT_ANOMALY` | Direct-stop path on a pathological sample. |
| `TOUCH_WEIGHT_FALLBACK` | Touch protection still blocks an eligible stop after one second of fresh above-threshold evidence; no offset or duration learning. |
| `GLOBAL_LIMIT` | 60 s hard cap. |
| `CONFIGURED_WALL_LIMIT` | Max BBW time / operational wall on automatic BBW cycles. Not used for timer-only or no-scale shots. |
| `SHORT_SHOT` | Reserved/legacy short-shot path. |
| `RINSE_COMPLETE` | Rinse duration elapsed. |
| `WEB_STOP` | Authenticated Stop. |
| `PHYSICAL_OVERRIDE` | Paddle moved during a Web-owned cycle. |
| `WEB_HEARTBEAT_TIMEOUT` | Remote hold lost (when remote machine control is enabled). |
| `RELAY_SAFETY_FAILURE` | Arm failed or safety tripped. |
| `FAST_EXTRACTION_MAX_WEIGHT` | Fast guard: recovery weight. |
| `FAST_EXTRACTION_MIN_TIME` | Fast guard: min BBW brew time reached after an early target. |
| `SLOW_EXTRACTION_MAX_TIME` | Slow guard: max BBW brew time with enough mass. |
| `SLOW_EXTRACTION_MIN_WEIGHT` | Slow guard: floor weight after extend. |
| `AUTO_TO_MANUAL_GUARD` | Notifications silent or link lost; A→M deadline from shot start. |
| `CUP_REMOVED` | Cup presence `REMOVED` with stop-if-removed on. |
| `UNCONFIRMED_START` | Switch-only: live scale, no espresso-like flow, net mass still within 1 g of the shot baseline at the 60 s hard cap. Silent; no last-shot / history row. |

History `cut_type` / `stop_detail` are a **stable log encoding** of
the same facts ([Shot history](features/shot-history.md)), not another
FSM.

---

## Control loop (ordering)

One pass of the control task, simplified:

1. Feed watchdogs. The activator samples `ACTIVATOR_GPIO` and publishes `UserIntent`.
2. Service relay safety (honor ISR trips, echo GPIO if present).
3. Recovery gesture only in the boot window; it never closes the machine circuit.
4. Apply brew orchestrator `switch (brewState)` using intent, weight control,
   cup, A→M, and rinse timers.
5. Drain scale events; update link, stream, cup, first flow, touch,
   weight control.
6. Drain Web/BLE commands if no cycle forbids that mutation.
7. Network, NTP, OTA, and HTTP run on the **network task**. They take a
   maintenance lease when they need exclusive RF or flash.

That split is why a hung HTTP handler cannot keep machine circuit closed: the
deadline timers and panic/stack-overflow hooks write the relay GPIO
from IRAM. The stack-overflow hook then aborts, so the controller reboots
with the circuit open instead of running on a corrupted stack.

---

## What is *not* a state machine here

| Name | Why it is omitted |
| --- | --- |
| `PaddleMode` (Natural / Original / Auto) | Latch TYPE=0 translator setting. Lives in `OpenBrewByWeightMachinePaddleConfig.h`; the brew orchestrator never branches on it. |
| `AlertEvent` | Outputs (beeps), not a mode. |
| `TaskProfilerState` | Diagnostics only. |
| Shot-log / NVS dual-slot enums | Storage format, not live control. |

# Shot trace format

Developer reference for the per-shot decision record (`ShotTrace`,
`src/OpenBrewByWeightShotTrace.h`). The firmware captures one trace for every
brew cycle that started with a scale: the same cycles the shot log stores,
good or bad. It explains a shot after the fact: what the controller knew at the
start, what happened just before, every decision it took, and how healthy the
weight stream was.

The trace lives in PSRAM that survives panics, watchdog and software resets;
it is not yet written to flash. The newest completed trace downloads from
[`GET /api/v1/diagnostic/shot-trace`](INTEGRATION_API.md#get-apiv1diagnosticshot-trace);
a flash pool and a Web UI view are planned.

## Capture window

| Moment | Effect |
| --- | --- |
| Cycle start with a fresh scale reading | Opens the capture just before the relay acts, so the close is recorded; the anchor is the cycle start (`anchorMs`). Only the header and counters are reset at this point. |
| Machine started | Adds identity, the start snapshot and the frozen pre-shot ring, then empties the ring; from here the capture survives a crash. |
| Rinse classification, abandoned (unconfirmed) start, refused machine start | Discards the capture and leaves a `TRACE_DISCARDED` entry in the pre-shot ring, so the next trace shows it. |
| Drip finalize, or drip analysis cancelled (new cycle, rinse, New coffee, cup lifted, backflush) | Completes the trace with the stored summary's id (0 if the shot log refused it). |

Cycles without a scale never open a capture. While a capture is open the
download answers `409`; nothing is exported during a shot. Only the newest
completed trace is kept.

## Interrupted traces

The two capture buffers live in a PSRAM segment that no boot clears
(`.ext_ram_noinit`). PSRAM is reached through a write-back cache that a reset
discards, so the firmware writes it back: the whole segment from the panic
handler (which also covers the interrupt and task watchdogs) and from a
shutdown hook for planned restarts, and the header of a buffer each time its
magic changes (capture in flight, completed, discarded, recovered).

Recovery after a reset that skips the panic handler, an "other watchdog"
reset (reason 7) or a hardware watchdog that resets the chip directly, is
best-effort: the header magic is current, but events written since the last
magic change may not have reached PSRAM, and the checks below then drop the
torn tail or the whole trace.

At boot, before anything is captured, the firmware looks at the header of
each buffer:

- After a software restart, a panic, or an interrupt, task or other watchdog
  reset, a capture that was in flight and passes the header checks (magic,
  versions, section sizes, counts) becomes the completed trace, flagged
  **interrupted** (`flags` 0x4). Its `bootId` is the boot that crashed, so it
  matches the crash archive entry of that boot. `shotLogId`, `endedAtMs` and
  `endReason` are 0: the shot never finished. Only captures of the running
  firmware (same packed version and version string) are trusted, because the
  segment's address depends on the image. The Web UI log says
  "Shot trace of boot N recovered as interrupted, pending verification".
- After a power-on, a brownout or any other reset, PSRAM contents are
  undefined: nothing is recovered and every in-flight marker is cleared.
- A completed or discarded capture is never recovered, and a recovered one is
  recovered only once.

The heavier checks run on first use (the download), not at boot: the start
check covers the snapshot and the pre-shot entries, and the event checks cover
the events. If only the last append is inconsistent (it was cut by the reset)
it is dropped and `flags` 0x8 is set; any other inconsistency drops the whole
trace, and the Web UI log warns "Interrupted shot trace of boot N failed
verification; dropped". Counters in an interrupted trace are those of its last
recorded event.

## Record layout

Little-endian, fixed, pointer-free; sections follow each other with no
padding. One buffer holds a whole record and is `SHOT_TRACE_BUFFER_BYTES`
(16 KiB, decision D2).

| Offset | Section | Bytes |
| --- | --- | --- |
| 0 | Header | 88 |
| 88 | Start snapshot | `snapshotBytes` (108) |
| 196 | End counters | `countersBytes` (84) |
| 280 | Pre-shot entries | `preShotCapacity` × 16 (32 × 16) |
| 792 | Events | `eventCount` × 8, at most 1,949 |

A record is therefore 792 to 16,384 bytes. Decoders must take section sizes
from the header, not from this table.

### Header

| Field | Type | Meaning |
| --- | --- | --- |
| `magic` | u32 | `0x43525453` ("STRC") in a completed record; `0x49525453` ("STRI") marks a capture in flight in RAM |
| `formatVersion` | u16 | 1 |
| `vocabularyVersion` | u16 | 1; names of kinds, codes and signals |
| `totalBytes` | u32 | Header through the last event |
| `crc32` | u32 | CRC-32 (IEEE) of `totalBytes` with this field zero; 0 in RAM, filled on export |
| `shotLogId` | u32 | Stored summary id; 0 when not stored |
| `bootId` | u32 | Boot of the shot (the crashed boot for an interrupted trace) |
| `anchorMs` | u32 | `millis()` at cycle start; event times count from it |
| `endedAtMs` | u32 | Shot end (`millis()`); with `bootId` and `shotLogId` links the summary; 0 when interrupted |
| `firmwarePacked` | u32 | `FW_VERSION_PACKED` |
| `startCheck` | u32 | FNV-1a of the snapshot and the valid pre-shot entries |
| `firmware` | char[24] | `FW_VERSION`, NUL-terminated (version and commit); truncated to 23 characters, so `firmwarePacked` is authoritative |
| `eventsCheck`, `prevCheck` | u32 | FNV-1a of `events[0, eventCount)` and of `events[0, prevCount)` |
| `eventCount`, `prevCount` | u16 | Event records (continuations included), and the count before the last append |
| `flags` | u8 | 0x1 verbose events truncated, 0x2 decisions truncated, 0x4 interrupted, 0x8 torn last append dropped |
| `endReason` | u8 | Raw `EndReason`; 0 when interrupted |
| `preShotCount` | u16 | Valid pre-shot entries |
| `snapshotBytes`, `countersBytes`, `preShotCapacity` | u16 | Section sizes and pre-shot slots |
| `reserved` | u16 | 0 |

The 16 bytes from `eventsCheck` to `preShotCount` (offsets 64–79) share one
cache line; each append writes its records first and then the 14 bytes from
`eventsCheck` to `endReason`.

### Start snapshot

Only what the shot log summary lacks, taken once the machine started. Weights
are signed centigrams; `INT32_MIN` means unknown.

| Field | Meaning |
| --- | --- |
| `freeHeapBytes`, `freePsramBytes` | The latest health sample |
| `postTareGraceMs`, `retareWindowMs` | The shot's post-tare grace and retare window |
| `stabilityDurationMs`, `stabilityGapMs`, `stabilitySamples` | Stability qualification |
| `minimumCupCg`, `cupRemovedCg`, `stabilityToleranceCg` | Cup thresholds and stability tolerance |
| `emptyReferenceCg`, `occupiedReferenceCg` | Empty-pan reference, occupied reference (cup present only) |
| `preTareWeightCg` | Weight at cycle start, before the shot tare |
| `flags` | Bits 0–14: auto tare, auto retare, idle tare, accessory retare, touch avoidance, require cup, stop if removed, cup protection, cup present, cup tared, empty pan qualified, idle tare eligible, scale supports tare, Linea Micra WebSocket subscribed, `wifiRssi` valid |
| `paddleMode`, `noScaleBbwMode` | Raw settings (accessory bits included) |
| `idleEligibility`, `idleLastReason` | `IdleTareReason` values |
| `wifiRssi` | dBm, valid with flag bit 14 |
| `scaleName`, `scaleProtocol` | NUL-terminated |

### End counters

Exact even when records were dropped.

| Field | Meaning |
| --- | --- |
| `readingsReceived` | Weight readings control processed in the window |
| `readingsAccepted` | Readings accepted (never recorded as events) |
| `readingsHeld` | Readings consumed without a decision: touch hold, recovery confirmation, pending retare effect |
| `readingsRejected[6]` | By reason: invalid, range, slew, recovery, pre-cycle, other |
| `readingGapMinMs`, `readingGapMeanMs`, `readingGapMaxMs` | Gaps between received readings |
| `packetGaps` | Scale packet gaps reported by the scale worker |
| `staleFrames`, `undecodableFrames` | Scale frames dropped by the scale worker (stale or from an old connection; not decodable). Counted only, never recorded |
| `confirmationResets` | Over-the-line confirmation chains that broke before a weight stop (a reading back under the line, or too late to be consecutive) |
| `loopGapMaxMs` | Longest control-loop gap |
| `micraRxBytes` | Linea Micra WebSocket bytes received |
| `uiStreamDeferrals` | Web UI stream batches deferred by a full socket |
| `verboseDropped`, `decisionsDropped` | Records that did not fit |

### Pre-shot entries

An always-on ring outside the window keeps the latest occurrence of each kind
(of each signal for state changes) with a repeat count. Kinds: scale
connect/disconnect, tare request/write/phase/confirmation/timeout/release/
rejection (an ineligible idle tare is a rejection with its eligibility reason),
cup placed/removed/reset, empty-pan anchor and qualification resets,
reference changes, drip analysis cancelled, a discarded trace
(`TRACE_DISCARDED`), and the `CUP_PRESENT`, `EMPTY_READY` and
`IDLE_ELIGIBILITY` signals.

| Field | Type | Meaning |
| --- | --- | --- |
| `head` | u32 | As an event head, but bits 0–22 are ms **before** the anchor (saturating; an entry stamped after the anchor reads 0) |
| `a`, `b` | i16, u16 | As the event's first record (`b` holds the low half of `arg1`) |
| `arg2` | u32 | The profile event's second argument |
| `arg1High` | u16 | High half of the profile event's first argument |
| `repeats` | u16 | Earlier occurrences folded into this one, saturating |

## Event records

```text
u32 head   bits 0-22  ms after the anchor (saturates at 8,388,607 ≈ 2.3 h)
           bit  23    namespace: 0 trace/profile kinds, 1 DebugCode
           bits 24-31 kind
i16 a
u16 b
```

`DebugCode` already has more than 128 values, so the namespace bit sits in the
time word and the kind keeps all eight bits.

Event times are **not monotonic**: producers on different tasks stamp their own
time before taking the lock, and some events carry an earlier observation time.
Times before the anchor clamp to 0. Order records by time when presenting them.

**Continuation.** A namespace-0 record of kind `0xFF` extends the record before
it and has that record's time. Its head bits 0–15 carry one more 16-bit field
and bits 16–23 are zero. It never follows another continuation and is never
split from its first record (both fit or both are dropped).

| Namespace / kind | `a` | `b` | Continuation (head bits 0–15 / `a` / `b`) |
| --- | --- | --- | --- |
| 0, `ScaleProfileEvent` (0–223) | weight in 0.1 g, `INT16_MIN` none | `arg1` low half | `arg1` high half / `arg2` low half / `arg2` high half; present when `arg1 > 0xFFFF` or `arg2 ≠ 0` |
| 0, `STATE_CHANGED` (31) | new value (saturated) | `ScaleProfileSignal`, `0x8000` first observation in the window | — |
| 1, `DebugCode` | `argument1` low half | `argument2` low half | 0 / `argument1` high half / `argument2` high half; present when `argument1` does not fit i16 or `argument2` does not fit u16 |
| 0, `0xE0` `STOP_REQUESTED` | `EndReason` | 0x1 web stop, 0x2 the machine refused the stop | — |
| 0, `0xE1` `STOP_BLOCKED` | 1 blocked, 0 released | cause (1 paddle original mode, 2 protection window), 0x100 a weight stop was pending | — |
| 0, `0xE2` `MACHINE_RUNNING` | 1 running, 0 stopped | — | — |
| 0, `0xE3` `SLOW_WINDOW_END` | weight dg | cut line dg | 0 / offset dg / flags: 0x1 target reached, 0x2 slow extension, 0x4 fast extension, 0x8 over the line |
| 0, `0xE4` `OVER_LINE` | weight dg | cut line dg | 0 / offset dg / confirmations so far |
| 0, `0xE5` `FINAL_WEIGHT` | weight dg, `INT16_MIN` none | `ActualWeightSource` | — |
| 0, `0xE6` `TRACE_DISCARDED` (pre-shot only) | `EndReason` (`RINSE_COMPLETE` for a rinse classification) | ms from that capture's anchor, saturating | — |

Without a continuation, a debug event's `argument1` is `a` sign-extended and
`argument2` is `b`; a profile event's `arg1` is `b` and `arg2` is 0.

### What is recorded

- Every scale profile event passed to `scaleProfileNoteEvent` except
  `WEIGHT_ACCEPTED`, which only counts. `WEIGHT_REJECTED` keeps the weight and
  the reason code, not the reference. Raw weights and dropped frames
  (`STALE_FRAME`, `FRAME_UNDECODABLE`) are not recorded; dropped frames count.
- Every debug event (`addDebugEvent`) during the window, except the twins of
  profile events already recorded: the five `SCALE_SAMPLE_REJECTED_*` codes,
  `ACCIDENTAL_TOUCH_HOLD/RELEASE/SUSTAINED`, `SCALE_PACKET_GAP` and
  `SCALE_EVENT_DROPPED`.
- State changes of `IDLE_ELIGIBILITY`, `WEIGHT_CONTROL`, `BASELINE_WAITING`,
  `BBW_PROTECTION` and `SCALE_LOSS_GUARD`, each first value marked initial.
- The trace-only kinds above. `STOP_REQUESTED` is written after the stop
  command returned, so it follows the relay opening. `SLOW_WINDOW_END` is the
  first timed reading at or after the slow window start (`slowAfterMs`);
  `OVER_LINE` is the first reading at or above the cut line minus the offset,
  whether or not it ever confirms a stop. The protection `STOP_BLOCKED` is the
  first reading the protection window keeps at or above the cut line and has no
  release record. These three are recorded once per shot; the paddle block is
  recorded on each change, blocked and released.

### Truncation

Verbose records (each rejected reading, touch event, packet gap and touch
handoff) stop at 75 % of the event capacity; decisions use the rest. A record
that does not fit increments `verboseDropped` or `decisionsDropped` and sets the
matching header flag; counters stay exact.

## Ownership and memory

Two 16 KiB buffers and the pre-shot ring are static PSRAM no-init memory
(33,408 bytes, `.ext_ram_noinit`); nothing in it is trusted before the boot
checks;
one buffer captures while the other keeps the newest completed trace. The
control loop owns the window, the state-change cache and the reading
statistics; producers on other tasks append under the trace's leaf mutex or
through relaxed atomics. The export copies 4 KiB chunks through a static PSRAM
buffer. See [concurrency](CONCURRENCY.md) and
[resource ownership](RESOURCE_OWNERSHIP.md).

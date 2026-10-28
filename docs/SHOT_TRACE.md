# Shot trace format

Developer reference for the per-shot decision record (`ShotTrace`,
`src/OpenBrewByWeightShotTrace.h`). The firmware captures one trace for every
brew cycle that started with a scale: the same cycles the shot log stores,
good or bad. It explains a shot after the fact: what the controller knew at the
start, what happened just before, every decision it took, and how healthy the
weight stream was.

The trace is captured in PSRAM that survives panics, watchdog and software
resets, and each completed trace is then saved to the [flash pool](#flash-pool)
once the shot is over, in the same record as the shot's curve
([saving](#saving-completed-traces), [curves](#curves-in-the-pool)).
[`GET /api/v1/diagnostic/shot-trace`](INTEGRATION_API.md#get-apiv1diagnosticshot-trace)
downloads the newest completed trace still in RAM, or any stored shot's
trace from the pool; the Web UI [decodes and shows it](#decoding-and-the-web-ui).

## Capture window

| Moment | Effect |
| --- | --- |
| Cycle start with a fresh scale reading | Opens the capture just before the relay acts, so the close is recorded; the anchor is the cycle start (`anchorMs`). Only the header and counters are reset at this point. |
| Machine started | Adds identity, the start snapshot and the frozen pre-shot ring, then empties the ring; from here the capture survives a crash. |
| Rinse classification, abandoned (unconfirmed) start, refused machine start | Discards the capture and leaves a `TRACE_DISCARDED` entry in the pre-shot ring, so the next trace shows it. |
| Drip finalize, or drip analysis cancelled (new cycle, rinse, New coffee, cup lifted, backflush) | Completes the trace with the stored summary's id (0 if the shot log refused it). |

Cycles without a scale never open a capture. While a capture is open every
trace download and the Shot storage statistics answer `409`; nothing is
exported during a shot. RAM keeps at most the last two completed traces until
the flash pool takes them; the pool keeps the history.

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
between shots both buffers can hold a completed trace waiting for the flash
pool, and a new capture takes one of them (see
[saving](#saving-completed-traces)). The control loop owns the window, the state-change cache and the reading
statistics; producers on other tasks append under the trace's leaf mutex or
through relaxed atomics. The export copies 4 KiB chunks through a static PSRAM
buffer. See [concurrency](CONCURRENCY.md) and
[resource ownership](RESOURCE_OWNERSHIP.md).

## Flash pool

`ShotTracePool` (`src/OpenBrewByWeightShotTracePool.h`) keeps one immutable
record per shot in a log-structured FIFO ring: no filesystem, garbage
collection, copy-forward or in-place update. Records are written back to back;
before the ring uses a 4 KiB sector it erases it, and the records that start
there are gone. Deleting a shot only marks its summary; its bytes are reclaimed
when the ring comes back to them. All fields are little-endian.

### Logical space

| Logical sectors | Partition | Offset |
| --- | --- | --- |
| 0–334 | `reserved` (0xCB1000, 1340 KiB) | sector × 4 KiB |
| 335–1336 | `shotcurve` (0x8C5000, 4016 KiB) | (sector − 333) × 4 KiB |

`reserved` comes first although it is physically after `shotcurve`.
`shotcurve` sectors 0 and 1 keep the retired legacy curve store's epoch and are
never part of the ring. Both partitions are found by label, subtype 0x40 and
exact size. The firmware's ring covers the whole space from boot (1,337
sectors, about 5.2 MB of records); the engine also supports a ring of
`reserved` alone whose end only grows (`setLogicalEnd`, followed by a
recovery). A logical address is
sector × 4096 + offset; every flash call stays inside one sector, so none
spans the partition boundary.

### Sector header

The first 24 bytes of every ring sector; data follows from offset 24.

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `magic` u32 | `0x53505453` ("STPS") |
| 4 | `version` u16 | 1 |
| 6 | `headerBytes` u16 | 24 |
| 8 | `sequence` u32 | +1 for every sector the ring starts, never reused (a failed header program consumes its number) |
| 12 | `floor` u32 | Sectors with a lower sequence are cleared |
| 16 | `crc` u32 | CRC-32 (IEEE) of bytes 0–15 |
| 20 | `firstRecord` u32 | Offset of the first record that starts in this sector, stored as `offset \| ~offset << 16`; `0xFFFFFFFF` while none has |

The first 20 bytes are programmed right after the erase. `firstRecord` is
programmed with the header when a record starts at offset 24, and otherwise
on its own, before the first byte of the first record that starts later in
the sector (NOR programming only clears bits). A sector whose header is blank,
torn or fails its CRC is a **hole**, not the end of the ring.

### Record

A record is 32 to 24,576 bytes, a multiple of 16: a 32-byte header, tagged
sections, and zero padding to the next 16 bytes. Its header never crosses a
sector boundary (with fewer than 32 bytes left, the record starts in the next
sector); the rest may span sectors and the partition boundary, skipping each
sector header.

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `magic` u32 | `0x52505453` ("STPR") |
| 4 | `version` u8 | 1 |
| 5 | `endReason` u8 | Raw `EndReason` |
| 6 | `totalBytes` u16 | Header, sections and padding; a multiple of 16 |
| 8 | `shotId` u32 | Shot log summary id |
| 12 | `bootId` u32 | Boot of the shot |
| 16 | `endedAtMs` u32 | Shot end (`millis()`) |
| 20 | `firmwarePacked` u32 | `FW_VERSION_PACKED` that wrote it |
| 24 | `crc` u32 | CRC-32 of bytes 0–23 and of bytes 32 to `totalBytes` |
| 28 | `commit` u32 | `0x54494D43` ("CMIT"), programmed last; anything else is uncommitted and ignored |

Each section is a 4-byte header, `tag` u8, `flags` u8 (0), `bytes` u16,
followed by its payload and zero padding to 4 bytes. Tags: 1 curve (below), 2
trace (a record of the [layout above](#record-layout)); tag 0 ends the list
and only padding follows. Readers skip unknown tags. A record holds the curve
first, then the trace; either may be missing.

The curve section is the PSRAM curve record's own bytes, so the delta stream
travels unchanged from capture to the browser:

| Offset | Field | Meaning |
| --- | --- | --- |
| 0 | `count` u16 | Points, at most 1,201 |
| 2 | `flags` u8 | 0x1 truncated (points past the capacity were dropped) |
| 3 | `reserved` u8 | 0 |
| 4 | `atmClearedMs` u16, then `firstDrop`, `extended`, `atm`, `ended` | 18 bytes; each event is `atMs` u16 and `weightCg` i16, `0xFFFF` / `INT16_MIN` when missing |
| 22 | stream | The schema-3 delta stream (see [architecture](ARCHITECTURE.md)); its length is the section length minus 22 and the two bitmaps |
| 22 + stream | `breakBefore`, `held` | `count` bits each, LSB first, zero padded to whole bytes |

A curve section is 22 to 5,598 bytes; with a full 16 KiB trace a record stays
under 22 KB. Decoders reject a count over 1,201, unknown flags, a stream that
does not hold exactly `count` points, and times that go backwards or past
60 s.

### Saving completed traces

The settings_persist worker owns the pool:

1. Its first idle pass with the durable write gate open and no capture open
   recovers the pool and logs "Shot trace pool ready" with the record count,
   unused sectors and duration. Until then the pool answers not ready and
   nothing reads or writes it; boot readiness and the relay never wait for it.
2. Each idle pass (about once a second) appends one record per stored shot,
   oldest first. Its identity is the summary's shot id, boot id and end time,
   with the raw end reason and the firmware that wrote it. It holds the shot's
   curve and, while that shot's trace still waits in one of the two RAM
   buffers, the trace (its export bytes, with the export CRC). A trace whose
   shot the log refused, and an interrupted one, go alone, after the pending
   curves, oldest first; a shot whose trace was overwritten, or that had none,
   gets a record with its curve alone (end reason 0). A
   curve is stored at finalize while the shot's capture is still open and the
   trace completes right after, so the worker only takes curves while no
   capture is open: a curve never leaves without the trace that belongs with
   it.
3. Steps run only while the gate stays open, which excludes the whole capture
   window (cycle start to drip finalize), an active machine, a scale being
   connected and a maintenance lease. When it closes the append pauses and
   the next pass resumes it; a flash lock timeout leaves the staged append in
   place, and the next step resumes it at the same address.
4. Both buffers' completed traces wait until the pool takes them. A capture
   opens over a buffer whose trace is saved (or empty); with two unsaved
   traces it overwrites the older one, unless only that one is interrupted,
   whose trace it keeps and overwrites the newer instead. Each overwrite is
   counted (`tracesDropped`, "Traces not saved since restart") and logged
   ("Shot trace of an earlier shot not saved: this shot started first"). The
   trace is copied by generation, so a paused append whose buffer a capture
   overwrote is abandoned instead of mixing two shots; one paused under a
   shot that took the other buffer resumes after it. In practice only a third
   shot started with no idle pass since the first ended loses a trace (a
   drip analysis cancelled by the next shot completes one trace and opens the
   next capture in the same loop). Power loss loses the traces still in RAM.
5. A failed write is retried with the whole record up to three times, with one
   warning ("Shot record save failed"); a record that is already on flash (a
   commit reported as failed that landed) is never written again. A record
   given up after three attempts is logged as "Shot record of shot N not saved
   after 3 attempts", and the next shot goes on.

An interrupted trace recovered after a crash is saved the same way. It has no
summary: its record is shot 0, end time 0, under the boot that crashed. A
completed trace whose summary the shot log refused is shot 0 with its boot and
end time.

Clear all marks the pool for clearing and drops the unsaved traces and curves
of the cleared shots; the worker tries the pool clear before it persists the
cleared shot log, so a power loss normally leaves both cleared, neither, or
only the pool. A clear requested while a capture is open (a drip delay) waits
for it to close: the shot log is persisted meanwhile, and the record identity
keeps the old records apart. Factory reset drops the cached curves and the unsaved traces
whatever else the reset reached (the shot log may already restart its ids),
then clears the pool and verifies it with a fresh recovery under the same
writer lock. A pool that cannot be recovered or cleared never holds the shot
log or the factory reset back: the clear stays requested ("Shot trace pool
not cleared … retrying", logged once), appends wait for it, and the record
identity (shot id, boot id, end time) keeps every older record apart from a
reused id meanwhile. A failed recovery or clear is retried after 1 s, then
after twice the previous wait, up to 10 minutes.

The legacy curve store (fixed 8 KiB blocks in `shotcurve`) is retired without
migrating its curves; an installation over USB with `--erase-all` starts
empty. Its blocks, if any remain, are never read as records: they fail the
sector magic, version or CRC and are holes the ring overwrites.

How many shots the pool keeps depends on how much each shot records, which
is not measured yet (plan B1). A record holds a curve of up to 5.6 KB (about
2–3 KB for a typical 30 s shot) and a trace of 0.8 to 16 KB, so the pool's
5.2 MB keep somewhere between about 240 shots of the largest records and
more than the shot log's 1,000 summaries of small ones. FIFO recycling
decides, never the shot's quality; the newest saved records are always kept.

### Curves in the pool

The newest 500 curves stay in a PSRAM cache (`ShotCurveCache`,
`src/OpenBrewByWeightShotCurve.h`, about 2.8 MB, under `shotStoreMutex`), so
Home, the Stats charts and the last 10, 50 or 100 shots never touch flash. The
curves not in the pool yet are always the newest ones in the cache; the worker
saves them oldest first.

- **After boot** the cache is empty. Once the pool is recovered, the worker
  fills the cache's older end from the pool, newest summary first, eight
  summaries per idle pass (its queue and new shots' records are served in
  between), until it holds 500 curves or every summary was tried, and logs
  "Shot curves loaded: N in RAM". It reads with no store lock, never
  displaces a curve stored since boot, and moves past a summary only once
  its read was answered: a busy read is retried on the next pass. The newest
  shot's curve is back on the first pass; until then Home shows that shot
  without a curve.
- **Older curves** (the shot list past the newest 500, its pages and exports)
  are read from the shot's record on demand: the summary's identity is looked
  up under `shotStoreMutex`, the lock is released, and the record is opened
  and CRC-checked under the flash lock.
- **Busy.** While a shot runs (the write gate is closed or a capture is open),
  before the pool's first recovery and when a flash read or the flash lock
  fails, an older curve answers busy: the Stats stream holds that row and
  resumes once flash may be read, sending nothing meanwhile. A pool that
  failed to recover answers no curve. The Stats page fingerprint never reads
  flash: a curve past the cache is hashed by its shot id (curves are
  immutable), and only the row being framed is read.
- **Gone.** A record whose sector the ring recycled, or that an interrupted
  erase tore, reads as a shot without a curve, never as damage.

Deleting a shot drops its summary and its cached curve; its record stays on
flash, unreachable, until the ring recycles its sector.

### Writing

An append performs one flash operation per step, so its caller can pause it
between steps: erase the next sector (after dropping the records that start
in it from the index), program its header, program `firstRecord` when needed,
write at most 1 KiB, and finally program the commit word. Writes are strictly
sequential, one erase per sector the ring advances. A failed step abandons the
record; the next one starts in a new sector. A flash lock timeout is not a
failure: nothing was written, so the append stays staged.

A record being written when the power goes is lost, by design: the machine is
often switched off right after a shot. Recovery never returns it as data,
always completes, keeps every older committed record outside the sector being
recycled, and appends continue. A commit word whose program reports a failure
is read back: if it landed, the record is committed and indexed at once, and
the writer's `contains()` check keeps a retry from writing the shot twice;
only a commit that did not land leaves the record uncommitted, and the whole
record is retried as a new one in the next sector.

Clearing (clear all, factory reset) empties the RAM index, erases the next
sector and writes a header whose `floor` is its own sequence: two operations,
after which every older sector reads as cleared. If power is lost before the
header lands, the RAM index was already empty but the older records reappear
after the reboot, so the caller re-runs and verifies the clear after boot.

### Recovery

Recovery runs on the persistence worker, never on the control loop:

1. Read every sector header of the ring. The valid one with the highest
   sequence is the head; its `floor` applies.
2. Walk the ring from the sector after the head, oldest first. A sector joins
   only if it is valid, at least the floor, and above every sequence before
   it; otherwise it is a hole.
3. Records chain by length through sectors with consecutive sequences. After
   a hole, a sequence gap or an uncommitted record, the walk resumes at the
   next sector's `firstRecord`. A record whose span crosses a hole is dropped.
   Only record headers are read; payload CRCs are checked when a record is
   opened.
   A sector's valid `firstRecord` outranks the length read from the record
   before it: on disagreement (a stale header in a sector whose erase was
   interrupted can change its length bits) that record is dropped and the
   walk resumes at the pointer. A torn `firstRecord` never reads as another
   valid offset: partial programming only leaves extra 1 bits, which break
   its complement check.
4. The next record goes right after the newest one if that is a committed
   record in the head sector, the rest of the head sector is blank and its
   `firstRecord` is blank or valid; otherwise (a torn append, an interrupted
   erase, a head with no committed record) it starts in the next sector.

With `shotcurve` open that is about 2,674 sector-header reads (each sector is
read in both passes), one record-header read per record, and a blank check of
at most 4 KiB; the walk yields and feeds the watchdog every 64 sectors.
Records in a sector whose erase was interrupted may open as `CORRUPT`: that
is the oldest data being recycled, not damage, and the Web UI must not
present it as such.

Opening `shotcurve` (`setLogicalEnd`) after the ring first wrapped inside
`reserved` drops the record that wrapped from the last `reserved` sector to
sector 0, and the ring recycles the oldest `reserved` sectors before it
extends.

### Index, linkage and reads

The RAM index holds one 8-byte entry per record, oldest first: bits 0–23 of
the shot id with an 8-bit tag of its boot id and end time, the logical address
in words (21 bits), and the length in 16-byte units (11 bits). It keeps the
newest 2,048 records, twice the shot log's summary capacity, since a record
is only reachable through its summary: 16,552 bytes of static PSRAM with a
one-bit-per-sector map of unused space. When it is full, recovery and appends
drop the oldest entry; that record stays on flash, unreachable, until the ring
recycles its sector.

A summary matches a record by shot id, boot id and end time, so an id reused
after a clear or factory reset never reaches an older record. Opening a record
finds the newest matching entry, checks the full identity in its flash header
(trying older entries when an 8-bit tag collides) and verifies the CRC;
`contains()` answers with the same identity check, so it never reports a
record of another boot or end time. A section whose length runs past its
record reads as corrupt, never as bytes beyond the record. Reads then take any chunk size; the index is looked up under
the pool's mutex, flash is read without it, and the record is checked again
after every chunk, so one the writer recycled meanwhile reads as gone rather
than as other data.

The storage statistics divide the pool into live and reclaimable indexed
records (the caller decides which are live: a deleted or evicted summary makes
its record reclaimable), unindexed space (records dropped from a full index,
sector headers, torn data and the head sector's unwritten tail) and unused
sectors. They also report the indexed record count and mean size, how many
records the full index dropped since the last recovery or clear, and the last
error. They read RAM only and may be taken under the shot store lock; a
record's header (`headerAt(n)`, the n-th oldest entry, its CRC unchecked) is a
flash read made without that lock, as are `open()` and `contains()`.

### Shot storage statistics

`GET /api/v1/diagnostic/shot-storage` (the Diagnostic **Shot storage**
section, read on demand, never during a shot) takes them in two steps:

1. Under `shotStoreMutex`, one walk of the summaries writes their index
   tags into the route's 4 KiB static PSRAM download chunk, the good ones
   from the front and the bad ones from the back (no memory of their own).
   The lock is released; each part is heap-sorted (no recursion) and one
   statistics pass runs with a live predicate that binary-searches both
   parts (2,048 lookups of at most 10 comparisons each instead of a scan of
   1,000 summaries per entry), counting good and bad shots. A record of shot
   0 (a trace without a summary) is live too. A second short hold finds the
   summary of the first live entry, the oldest stored shot ("history
   since"). Control takes `shotStoreMutex` blocking when a shot starts, so
   the holds stay at one walk each.
2. Without any store lock: for each indexed record, newest first, its header
   (a deleted or evicted shot's record, found absent from the sorted tags, is
   skipped) and the trace's `flags` byte (about four small reads) count the
   traces, the truncated ones, the traces of refused shots and the
   interrupted ones; the newest 16 of these traces without a shot are listed
   by boot and end time (end 0: interrupted), and the section offers each
   for download. The walk yields every 64 records and stops with `409` if the
   write gate closes. A record appended or recycled meanwhile can shift a
   count by one; the figures are diagnostic, not accounting.

A tag collision (24-bit shot id plus an 8-bit boot and end-time tag) can only
count a record live that is not; it never affects reads.

## Decoding and the Web UI

The Web UI decodes a trace once, in `decodeShotTrace` (`src/web/js/stats.js`),
for both the trace view and the **JSON + diagnostics** export, so both show
the same fields: the header (identity, firmware, end reason, flags), the start
snapshot and end counters by name, the pre-shot entries and the events in
record order with each continuation merged into `ext` as
`[head bits 0-15, a, b]`. `a` and `b` stay raw; their meaning per kind is the
[event table](#event-records). Section sizes come from the header, and a
record whose magic, format version, length or sections do not add up, or
whose pre-shot count exceeds its capacity, is refused. Its output is the export schema's
[`trace`](schemas/README.md#shot-export-v1).

Names come from `GET /api/v1/diagnostic/shot-trace/vocabulary`: the firmware's
`ScaleProfileEvent`, `DebugCode`, `ScaleProfileSignal` and end-reason names
and the yield sources by number, with `SHOT_TRACE_VOCABULARY_VERSION`; each
list holds exactly `SCALE_PROFILE_EVENT_COUNT`, `DEBUG_CODE_COUNT`,
`ScaleProfileSignal::COUNT`, `END_REASON_COUNT` and
`ACTUAL_WEIGHT_SOURCE_COUNT` names. Each count is `static_assert`ed next to
its name table: the build fails when the number past a count has a name or
the last one counted has none. The Web UI asks with the firmware identity in
the query (`?fw=` the version string), so a firmware update, even one that
leaves the Web UI unchanged, fetches the new names; the answer is cached
immutable for a unique identity (`x.y.z+sha`) and revalidated (`no-cache`)
for a build with uncommitted changes (`-dirty`) or without Git (`+unknown`). A trace written under another vocabulary
version gets generic names (`profile_21`, `debug_7`), no signal names and no
end-reason or source names; the trace-only kinds (`0xE0`–`0xE6`) are named
by the Web UI, which also formats their arguments (the slow-window flags,
the over-line confirmations, the yield source, a discarded trace's age).
Bump the vocabulary version only when an existing number changes meaning or
argument encoding: a code, kind, signal, end reason or yield source
renumbered or reused, or a trace-only kind's arguments or continuation layout
changed. Appending one keeps the version: older traces never use the new
number, and the firmware-keyed vocabulary URL brings its name.

The view (**⋯ → View trace** on a shot card) fetches the record by the
shot's identity, retrying a record that is still being saved, and shows the
shot's curve with one mark per event shown (decisions in the accent color),
the events sorted by time with a type filter and a text search that narrow
both, then the pre-shot entries, the start snapshot and the end counters.
The list carries all the information; the chart is decorative. **Download**
saves the raw record. `src/tests/fixtures/shot_trace_v1_hex.txt` is a real
export the host case TRC25 checks with the firmware layout and the Web UI
contract decodes, so both decoders read the same bytes.


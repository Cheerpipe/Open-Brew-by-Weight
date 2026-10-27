# Shot trace format

Developer reference for the per-shot decision record (`ShotTrace`,
`src/OpenBrewByWeightShotTrace.h`). The firmware captures one trace for every
brew cycle that started with a scale: the same cycles the shot log stores,
good or bad. It explains a shot after the fact: what the controller knew at the
start, what happened just before, every decision it took, and how healthy the
weight stream was.

The trace is captured in PSRAM that survives panics, watchdog and software
resets, and each completed trace is then saved to the [flash pool](#flash-pool)
once the shot is over ([saving](#saving-completed-traces)). The newest
completed trace downloads from RAM through
[`GET /api/v1/diagnostic/shot-trace`](INTEGRATION_API.md#get-apiv1diagnosticshot-trace);
a per-shot download from the pool and a Web UI view are planned.

## Capture window

| Moment | Effect |
| --- | --- |
| Cycle start with a fresh scale reading | Opens the capture just before the relay acts, so the close is recorded; the anchor is the cycle start (`anchorMs`). Only the header and counters are reset at this point. |
| Machine started | Adds identity, the start snapshot and the frozen pre-shot ring, then empties the ring; from here the capture survives a crash. |
| Rinse classification, abandoned (unconfirmed) start, refused machine start | Discards the capture and leaves a `TRACE_DISCARDED` entry in the pre-shot ring, so the next trace shows it. |
| Drip finalize, or drip analysis cancelled (new cycle, rinse, New coffee, cup lifted, backflush) | Completes the trace with the stored summary's id (0 if the shot log refused it). |

Cycles without a scale never open a capture. While a capture is open the
download answers `409`; nothing is exported during a shot. RAM keeps only the
newest completed trace; the flash pool keeps the history.

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
`shotcurve` sectors 0 and 1 keep the legacy curve epoch and are never part of
the ring. Both partitions are found by label, subtype 0x40 and exact size. The
ring ends at sector 335 (`reserved` only) while the legacy curve store still
owns `shotcurve`; retiring that store opens the rest. The end only grows, and
a recovery follows. A logical address is
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
followed by its payload and zero padding to 4 bytes. Tags: 1 curve, 2 trace (a
record of the [layout above](#record-layout)); tag 0 ends the list and only
padding follows. Readers skip unknown tags.

### Saving completed traces

The settings_persist worker owns the pool:

1. Its first idle pass with the durable write gate open and no capture open
   recovers the pool and logs "Shot trace pool ready" with the record count,
   unused sectors and duration. Until then the pool answers not ready and
   nothing reads or writes it; boot readiness and the relay never wait for it.
2. Each idle pass (about once a second) takes the newest completed trace that
   was not saved yet, with its export CRC, and appends it as one record whose
   identity is the summary's shot id, boot id and end time, its raw end reason
   and the firmware that wrote it. The trace section holds the export bytes;
   the curve section is added with curves from the pool (planned).
3. Steps run only while the gate stays open, which excludes the whole capture
   window (cycle start to drip finalize), an active machine, a scale being
   connected and a maintenance lease. When it closes the append pauses and
   the next pass resumes it.
4. The trace is copied by generation, so a shot that ends before the previous
   trace was saved replaces it (newest wins): the paused append is abandoned,
   and the log counts the traces that were never saved.
5. A failed write is retried with the whole record up to three times, with one
   warning; a record that is already on flash (a commit reported as failed
   that landed) is never written again.

An interrupted trace recovered after a crash is saved the same way. It has no
summary: its record is shot 0, end time 0, under the boot that crashed. A
completed trace whose summary the shot log refused is shot 0 with its boot and
end time.

Clear all marks the pool for clearing and drops the unsaved trace of a cleared
shot; the worker clears the pool before it persists the cleared shot log, so a
power loss leaves both cleared, neither, or only the pool, and an id reused
after a clear never reaches an older record. Factory reset clears the pool and
verifies it with a fresh recovery under the same writer lock. A pool that
cannot be recovered never holds the shot log back.

The pool starts as the `reserved` partition alone, about 1.36 MB of record
space: roughly 700 shots of 2 KB traces, or 350 of 4 KB. When curves move to
the pool, the legacy curve store is retired without migrating its curves and
the ring opens `shotcurve`. Its legacy blocks are never read as records: they
fail the sector magic, version or CRC and are holes the ring overwrites.

### Writing

An append performs one flash operation per step, so its caller can pause it
between steps: erase the next sector (after dropping the records that start
in it from the index), program its header, program `firstRecord` when needed,
write at most 1 KiB, and finally program the commit word. Writes are strictly
sequential, one erase per sector the ring advances. A failed step abandons the
record; the next one starts in a new sector.

A record being written when the power goes is lost, by design: the machine is
often switched off right after a shot. Recovery never returns it as data,
always completes, keeps every older committed record outside the sector being
recycled, and appends continue. A commit that reported a failure but landed
does reappear after a reboot, so a writer checks `contains()` before writing
the same shot again; until then the statistics may count one duplicate.

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
record of another boot or end time. Reads then take any chunk size; the index is looked up under
the pool's mutex, flash is read without it, and the record is checked again
after every chunk, so one the writer recycled meanwhile reads as gone rather
than as other data.

The storage statistics divide the pool into live and reclaimable indexed
records (the caller decides which are live: a deleted or evicted summary makes
its record reclaimable), unindexed space (records dropped from a full index,
sector headers, torn data and the head sector's unwritten tail) and unused
sectors. They also report the indexed record count and mean size, how many
records the full index dropped since the last recovery or clear, and the last
error. They read RAM only and may be taken under the shot store lock; the
oldest reachable record's boot and end time ("history since") is a separate
flash read (`oldest()`) made without that lock, as are `open()` and
`contains()`.


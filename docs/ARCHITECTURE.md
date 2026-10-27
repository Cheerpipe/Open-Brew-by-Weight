# Firmware architecture and ownership

Use this reference when changing module boundaries. For the runtime sequence,
read [State machines](STATE_MACHINES.md); for a first code change, start with
[Contributing](../CONTRIBUTING.md).

The integration roots `src/openBrewByWeight.cpp` and `src/OpenBrewByWeightNetwork.cpp`
assemble implementation fragments in `src/control/`, `scale/`, `network/`,
`persistence/`, `diagnostics/`, and `platform/`. Fragments compile in their
owning translation unit. Shared headers carry fixed-size values, pure policy,
or explicit callback tables; each service owns its mutable state.

| Service | Owns mutable state | Accepts | Publishes |
|---|---|---|---|
| SafetyKernel | relay, safety timers, watchdog and reset history | bounded control intention | `RelaySafetySnapshot`, safety events |
| ControlOrchestrator | active session, cup/recipe policy and command arbitration | scale/machine/network messages | fixed-size control snapshots and commands |
| ScaleService | BLE central link, scale queues and radio policy | `ScaleCommand`, immutable worker policy | `ScaleEvent`, `ScaleLinkSnapshot`, RF state callback |
| SelectedMachineIntegration | concrete machine integration state; absent integrations use a no-op adapter | common lifecycle/config, physical-start disposition and optional backflush contract | normalized bounded backflush observations plus private feature status; no universal feature model |
| NetworkService | Wi-Fi/httpd/mDNS, request parsing and webhook transport | `NetworkBridgeCallbacks`, control snapshots | `WebCommand`, transport diagnostics |
| PersistenceService | NVS/EEPROM/partition serialization and write schedule | fixed-size records | success/failure result messages |
| DiagnosticsService | logs, counters, task/heap snapshots and exports | observational snapshots | JSON/serial evidence only |

## Dependency rules

1. SafetyKernel never includes or calls Wi-Fi, HTTP, JSON, webhook or network
   code. Its ISR path remains fixed-size, internal-memory-only and nonblocking.
2. ScaleService never references the `networkManager` singleton. Radio state
   crosses `ScaleWorkerBridgeCallbacks`, configured once before its task starts.
3. NetworkService requests control effects only through
   `NetworkBridgeCallbacks`; it cannot call machine/relay implementation APIs.
4. Persistence owns durable writes. Other services publish fixed-size requests
   and do not hold task/spin locks across flash I/O.
5. Diagnostics are observational. No diagnostic result authorizes the relay or
   mutates session state.
6. C task handles are borrowed lifecycle tokens: their owner must execute
   stop/ack/join. Other ESP/FreeRTOS handles use a unique non-allocating owner
   or an explicitly documented static lifetime.
7. Common machine-integration code exposes lifecycle hooks, physical-start
   disposition and an optional normalized backflush observation contract.
   Native protocol, authentication and provider policy remain in the selected
   module. Unsupported integrations return no permission. CMake compiles exactly
   one concrete adapter and only its private component dependencies.

`scripts/check_architecture.py` gates these rules and caps growth of the three
legacy concentration points. The caps are not quality targets: when a feature
would exceed one, extract it into its owning service rather than raising the
limit. Current independent host harnesses exercise SafetyKernel, OTA,
persistence, webhook policy, BLE protocol/runtime and shared resource owners
without including either monolithic integration root.
The same script rejects the project's former name anywhere in the repository;
only the README credit to the original kit keeps it.

## Wi-Fi traffic economy

Every radio exchange wakes the modem from sleep and takes airtime shared with
the BLE scale link. Any feature, endpoint, client, or protocol change that
sends or receives Wi-Fi traffic must be designed, in this order, to:

1. Minimize the number of transmissions and receptions: prefer events over
   polling, coalesce updates into one message, batch requests, and avoid
   keepalives, retries, or periodic refreshes that the requirement does not
   need.
2. Minimize the bytes per exchange: send only changed or required fields,
   keep payloads compact, and serve cacheable or compressed assets.

Apply both whenever the requirement can still be met without a functional or
technical tradeoff, such as added latency in safety- or brew-relevant
feedback, lost diagnostics, or weaker reliability. When an exception is
necessary, document the reason with the change. Modem sleep stays enabled;
these rules reduce wakeups rather than disabling power saving.

Documented exception: the Admin page reads its own status by REST every 10
seconds while visible and owned. Live and shared state (machine, scale,
presets, command results, configuration revision) reaches every page on the
Home stream, Settings has its own standing stream, and command confirmation
waits on the Home stream; the slow poll only catches device-side changes the
Admin forms show that have no stream projection (network, OTA, webhooks),
which would otherwise need a stream of their own.

## PSRAM cache before flash

Everything a person can change from the Settings and Home pages, and the shot
history with its records and curves, is read from and written to its PSRAM
cache only. Request handlers and the tasks that apply a change never read or
write flash for these values: they update the cache, and the cache's own
deferred persistence (the settings persist worker, the shot-store image
writer) decides when and what reaches flash, coalescing bursts into one write.
Reads always come from the cache, never from flash; flash is read only at boot
to fill it. This covers presets, shared settings, Bullseye, scale choice and
names, Linea Micra options and machine selection, quick settings, drip and
learning-speed sets, and every history or shot mutation.

Only some Admin operations may read or write flash directly, because they must
be durable before they take effect: Wi-Fi and access point, device password and
Web UI access, webhooks, firmware mode, factory reset, restart, and OTA. The
onboarding brewing choice (`saveSetupBrew`) is the one non-Admin exception: it
writes the setup intent directly because the next boot applies it, with or
without a saved Wi-Fi network. A new setting belongs to the cached group
unless it has the same durability need; an exception must be documented with
the change.

## Machine integration modules

The machine profile declares an allow-listed `integration` value explicitly;
brand, model, ID and display text never select executable code. Profiles with
`integration: none` compile the no-op adapter and do not link a concrete machine
protocol component. Linea Micra builds compile its adapter, service, bounded
feature types, HTTPS cloud client and the pinned WSS/STOMP observer. Future machines may expose entirely
different feature APIs while retaining only the small lifecycle boundary needed
by boot, network-state publication, and generic physical-start disposition.

The Micra adapter owns a dedicated low-priority worker, bounded PSRAM response
workspace, and a PSRAM-only mbedTLS allocation profile so transient handshakes
do not fragment internal DRAM. It registers an installation key, signs La
Marzocco cloud requests, keeps short-lived access and refresh tokens in RAM,
lists account machines, and reads only the selected serial's dashboard. The
network service publishes STA, AP, and shot state through the common lifecycle
boundary. The worker never starts cloud work without STA, cancels it when AP
starts or STA is lost, and pauses state observations during shots. It has no
NimBLE dependency and cannot delay scale discovery, scale commands, or weight
delivery.

The cloud client deliberately does full TLS 1.2 handshakes. Session-ticket
resumption was shipped and reverted twice: ESP-IDF 6.1 re-applies the saved
session on every step of an asynchronous handshake, so each resumed reconnect
fails with `MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE`; a Web contract rejects
`save_client_session` until ESP-IDF fixes asynchronous resumption. TLS 1.3 stays
off: the cloud server repeats the key exchange when resuming TLS 1.3 sessions,
so it saves one round trip but almost no CPU, at a flash cost.

The Micra service also owns power-state freshness and the optimistic ON/OFF
overlay lifetime.
Its adapter returns normal, wake, backflush-candidate or cleaning-blocked
physical-start dispositions. Control consumes them before brew, rinse, guards
and scale effects; those subsystems never depend on Micra types.

Backflush uses an optional common bounded handoff of eight ordered transitions,
copied under the integration mutex and drained only by control. Opaque
continuity, episode and first-ingress attempt tokens fence discontinuities and
pre-edge frames. Micra owns native state, transport liveness and qualification;
control owns candidate/running activity without a brew session. The existing
relay driver alone promotes a live 60-second candidate to the original close
time plus 180 seconds, transactionally replacing its independent and task
deadlines. Every non-active state after confirmation, observation loss, physical
release or safety failure opens the relay. The end path appends one electrical
duration record without scale commands, shot statistics or brew events.

The settings blob carries the `LineaMicraPersistedSettings` record in every
profile; its connection type is explicitly WebSocket=0 and API=1. Settings
saves and resets wait for successful OTA trial confirmation, preserving the old
durable records for rollback. This admission does not gate boot readiness or
other stores.

One facade reduces HTTP and WebSocket field updates. Each admitted subscription
queues one initial dashboard cycle in the existing HTTP worker, concurrent with
push reception. Its identity, epoch, connection revision and independent field
revisions are captured at subscription; ordinary reads capture them at request
start. Both capture intent at request start; callbacks capture intent when a
fragmented message starts. Old results cannot overwrite newer push evidence or
offline/reconnect transitions. Compatible queued reads coalesce; post-command
reconciliation retains its request-time field baseline. Accepted power alone
changes its source (`api_initial`, `api`, `websocket`) and synchronization.
WebSocket power stays current during healthy silence; API mode retains its
30-second age limit. Accepted commands do not fabricate power/temperature samples.
A 60-second optimistic overlay and a separate stale suspension hold preserve
the effective power without changing the last cloud evidence timestamp.
The SDK callback publishes decoded updates under the facade mutex, independently
of the HTTP worker. Only the worker stops/destroys the WebSocket.
Observation pauses require both an admitted shot and a currently connected
scale. Scale loss or rinse classification releases the pause. Unscaled shots,
wake, backflush and rejected starts keep observation available. Actual-shot
command safety and relay-critical webhook/NTP admission remain independent
of the scale-qualified transport pause.

Scale discovery publishes a generic atomic inhibit before exposing an eligible
Candidate mailbox. Micra HTTP, webhooks and NTP consume it, regardless of
machine profile. Discovery releases acquisition at Ready, or after five seconds
of actual scan opportunity without a candidate. Setup, shot and the library's
three-second quiet interval remain separate gates. Local access remains active;
this is transport quiescence, not a physical RF-silence guarantee. Micra WSS is
excluded from the scale inhibit so backflush supervision survives BLE acquisition.

## BBW policy and storage

Brew-by-weight uses a single cutoff policy, the smooth cut (`corridor`): a
pure integer kernel (`OpenBrewByWeightCorridor.h`: cut line f(t), time-window
flow estimator with a MAD gate, crossing solver, verdict, channeling predicate,
arrival statistics and the scale-loss deadline) with adaptive EWMA learning of
the drip offset. The kernel holds no globals and allocates nothing; the control
task owns the per-shot `CorridorState` (≤ 512 B) and is its only writer. Shared
scale qualification, direct confirmation, control arbitration and
machine/safety authority remain outside the kernel; it never actuates hardware
or writes flash. The trend fit remains only for accidental-touch sensing. The
`replay` host test fixed the timed law, D = 5 s and the channeling floor from a
synthetic uncensored corpus, and a D = 0 oracle reproduces the captured legacy
Fast/Slow decisions. Each preset sets its Fast and Slow ramps on their own:
length D (0–10 s, 5 s by default) and a curve from `RAMP_CURVES`, the
regularized incomplete beta I(u; a, b) with integer a, b ≥ 2 (monotone and
flat at both ends, as the crossing bisection requires). The kernel evaluates
it exactly as floor(margin · Bernstein sum / d^(a+b−1)) in a 128-bit
multi-limb integer; the default Beta(2,2) is the original smoothstep bit for
bit, and the Web UI repeats the same law with `BigInt`.

Control owns `BbwLearningBank`: up to eight identity-keyed states, under 3,000
bytes total, with 20 observations and five trajectory anchors per preset.
Four fixed gains compete against the current gain, including a custom incumbent.
Scoring replays the retained window from its anchors and is bounded
post-finalization work, with no allocation or per-sample persistence. The cycle
and pending finalizer capture preset, algorithm/profile, full-precision offset,
actual alpha, the rate at the stop and a per-learner generation. Learning checks
the originating state; reset/delete/recreation or a conflicting update
invalidates it. Recipe changes clear evidence.
Editing reset bases alone preserves current learning and evidence.
The Settings stream publishes the active-preset identity, the drip in use and
the gain together from the existing coherent control snapshot.

Settings schema 2 stores `PersistedSettings` as a tagged encoding (see
[Settings encoding and evolution](#settings-encoding-and-evolution)). A preset
stores the recipe: target, dose (reference
only), shot time window (`fastBeforeMs`/`slowAfterMs`), fast and slow side
switches with margins relative to the target (decigrams), weight-cut delay, max
shot time, the optional fixed scale-loss stop time, drip compensation, grind
hints and its statistics epoch (`learningEpoch`, a shot-log record id, moved
only by Relearn (the `new_coffee` command), create, duplicate, restore and the
reset-guard-samples command). Arrival statistics also require each shot's stored window and a
target within 1 g of the preset's; the first-drop median ignores both and
falls back to the other presets' shots while the preset has none. The grind
hint stores nothing: each status snapshot replays the preset's shot-log rows
for its current window and target (`corridor::grindHintOf`, under
`shotStoreMutex`, on the HTTP task), deriving segments, spread, phase and the
hint lifecycle from record ids, stop details, arrival, duration and final
weight. Rows before `learningEpoch` feed only the spread estimates. A reboot or an OTA
update therefore yields the same hint, and no settings or shot-log schema
field exists for it. Cup
protection, accidental-touch, touch-stop fallback and the scale-loss backup
switch are device-level `RuntimeConfig` fields and are never overlaid from a
preset. A fresh or factory-reset bank holds four permanent factory presets (ids
1–4: Double · Adaptive, Single · Adaptive, Double · Fixed, Single · Fixed);
they can be restored but not deleted. A golden schema-2 blob in the host tests
proves later firmware keeps settings across OTA. Candidate
anchors/observations/generations are RAM only; deferred persistence retains
offsets, gain/provenance, and profile through the existing dual-slot owner.
`powerManagementEnabled`, webhook preset delivery, and Rinse without a scale
are current explicit fields. Presets never copy the global power setting.
`touchStopFallbackEnabled` is a device-level runtime field, defaulting to true. Revisions and
dual-slot ownership remain unchanged.
The shot log is the authority for a completed shot: confirmed, non-rinse,
strictly more than 12,000 ms of brewing and a finite settled yield strictly
above 2 g. The fixed recording threshold does not alter the configurable BBW
protection window or actuation. Home and the integration snapshot resolve the
newest eligible log record by ID under `shotStoreMutex`; the curve sidecar and
rating use that same ID. Deleting the newest record advances Home to the next
eligible row, and clearing the log empties the idle card. The in-progress card
continues to use the live control snapshot. The legacy `LastShotStore` blob
remains in the binary layout for internal completed-cycle diagnostics, but its
separate good-shot field is not a public shot authority. An erase-all install
starts both histories empty; no upgrade migration is required.

History schema 0 keeps the current fixed records and entries. In shot-log
records the side-flags byte (bits 0–1 fast/slow side on, 2–4 rating) uses bits 5–7 to encode
profile (0 unknown, 1 pre-selector regression with unknown version, 2 regression v1,
3 adaptive EWMA v1, 4 EWMA v2, 5 smooth cut v1). Alpha is 0 unknown or 1–100 hundredths;
extension bits 2–4 hold its low three bits, cut-type bits 4–7 its high four.
Extension bits 5–6 encode learning application (0 unknown,
1 skipped, 2 applied). Guard, rating, extension and weight-source meanings are
preserved in the current layout. Shot-type bits 2–7 and cut-type
bits 2–3 hold the captured preset ID (low six/high two bits); type/cut readers
mask the low two bits. Preset/BBW writers preserve each other's bit fields.
Any non-v1 history store is discarded instead of decoded. The ID follows
existing preset allocation, while the stored name is historical data rather
than a lookup through the current preset bank.
Stats values are derived under the store mutex from shot records in RAM;
no aggregate is persisted. The read visits at most 100 ring entries and copies
at most ten records for general metrics. BBW error has its own ten-cut window
across the same ring, counting every weight cut (target, raised or lowered
segment) and measuring each against its stored cut line.
Available flow is counted separately within the general window.

The stats shot log, its curve sidecar, and the independent activation history
are owned by one RAM data layer (`ActivationStores`) whose every access runs
under the single `shotStoreMutex`. Control and HTTP mutate only RAM and advance
a generation. Control housekeeping captures an immutable image under that
mutex: clean shot/activation stores skip their record payloads, and the curve
image carries only the uncommitted suffix plus block, deletion and epoch metadata.
Oldest-first curve commits preserve this suffix across retries and ring eviction.
The image is dispatched to the core-0 persistence worker. The worker performs
flash I/O without the store mutex. Control acknowledges completion and clears
live dirtiness only when the completion generation still matches.
Acknowledgement carries the image's
flash progress back to each live store even when newer RAM edits remain dirty,
so the next snapshot advances from the committed physical state. ShotLog and
History keep their two-slot generations; curves use block sequences, retention
floors and clear epochs. The manual scale
profiler follows the same deferred pattern: the health worker owns one
bounded PSRAM capture of raw decoded weights plus correlated firmware
decisions, and the persistence worker commits the completed trace to a
dedicated flash partition with the validity-bearing header written last.
Capture ends at record capacity or on request, with a reserved terminal slot;
there is no elapsed-time cutoff. Its 256 KiB PSRAM payload and 256 KiB flash
payload hold the same trace, so they are not additive capacity. The health
worker maintains a constant-memory, time-based EWMA of record growth for the
advisory remaining-time estimate. Persistence state remains separate from
capture occupancy. Existing record/header fields extend timestamps without
changing the 32-byte record size or partition layout; see the
[diagnostic API](INTEGRATION_API.md#post-apiv1diagnosticscale-profile). Independently
of that manual capture, every brew cycle started with a scale records a
compact [shot trace](SHOT_TRACE.md) of the controller's decisions in PSRAM;
it is not yet persisted.
Only the sectors the store occupies in the
inactive slot (16 of the shot log's 126, all four of the history slot) or the
used curve sector are erased, one 4 KiB sector at a time; each is programmed
one 1 KiB staged chunk at a time and receives its validity-bearing header last; the worker rechecks the current
machine and scale gates between steps. The shot log's whole 62,024-byte store and the
16,024-byte activation-history store live in PSRAM and move to and from their
slots in 1 KiB chunks staged through the small internal flash-I/O scratch only
while that owner holds the flash lock. Each keeps two slots in its own data
partition — 2×504 KiB for `shotlog` (reserving room for 1 KiB records), 2×16 KiB for `history` — with generation
and checksum selection preserving the atomic whole-store update; a failed
write never erases the last-good slot. Writes remain deferred until the shot
has ended. The shot log uses schema 2 in its dedicated partition (128-byte records, each
a self-contained snapshot of the cut parameters in force: target, window,
side switches, margins, delay, max shot time, each side's ramp length and
curve (a 124-byte record widens to one ramp length for both sides and
smoothstep), plus the cut
line at the stop, arrival time, packed verdict byte, the machine integration's
target temperature, the preset's dose and the shot's brew ratio); any other
schema is discarded and the shot history starts empty. Within schema 2,
records grow only by appending fields: the header stores the record size, and
an image with longer records widens a store of shorter ones on load (appended
fields read as zero, so zero must mean "missing" for them unless the widening
sets an explicit default). History and the statistics replayed from it
therefore survive an OTA update that grows the record. The reverse is not
supported: an earlier image discards a store written with longer records.
Each slot leaves room for 1,032-byte records, but every save rewrites the
whole store, so the per-shot write cost grows with the record size. The boot
ID lives in its own small NVS key, so a boot never rewrites the shot log.

On n16r8, ESP-IDF writes one ELF core dump to a 640 KiB capture partition
after a panic. Once the relay is open and the durable boot ID has been saved,
boot code validates that image and promotes it into one of two 704 KiB slots
in the crash-history partition. Each slot commits a checksum-protected metadata
sector last, after the dump copy and SHA-256 verification. Later boots list
records by that metadata checksum alone; the dump digest is re-verified, chunk
by chunk under the flash lock, when a download is requested, and a record whose
dump no longer matches is dropped from the list then. The copy replaces
the oldest valid slot only when both slots are occupied; interruption before
commit leaves the capture available for retry. The capture's first sector is
erased only after commit, so a reboot between those operations is recognized
by the boot ID and dump digest. HTTP download reads bounded chunks under the
shared flash lock and releases the lock before sending each chunk. The raw
archive requires Admin unlock because task stacks may contain secrets.

The shot-curve sidecar writes schema 3: delta-encoded observations (its
extension event marks the cut line leaving the target) plus per-observation
break and held bitmaps. Each point is one time byte and one weight byte
relative to the previous point; a step outside one byte stores a marker
(`0xFF` time, `0x80` weight) and the little-endian absolute value. The
observations stay in this one stream end to end: capture appends to it,
the PSRAM cache, flash blocks and the Web UI curve JSON carry it unchanged,
and only the browser decodes it. Earlier absolute schemas (1 and 2) no longer
load. Each accepted extraction or drip weight, and each reading the accidental-touch
classifier held, is paired with its relative reception time in milliseconds;
repeated weights and equal timestamps remain observations. Held readings are
flagged, never control weight, and never start a segment. The 1201-observation capacity covers a 60 s
shot with observations at least 50 ms apart. That spacing is a capacity
assumption, not a grid or rate limiter. Known rejected observations, reference
changes and link discontinuities mark the next accepted observation as a new
segment. Overflow preserves the prefix, marks it incomplete and leaves control
and scalar history running.

The 5608-byte RAM record (header, the stream reserved for the valid-record
worst case of 5274 bytes, and both bitmaps) and the 2,804,004-byte 500-record
cache live in PSRAM; copies move only the used stream and bitmap bytes.
Curve events also preserve elapsed milliseconds in 16-bit fields, with
`UINT16_MAX` reserved for unavailable values. Sidecar schema 0 distinguishes
these fields from previous decisecond events; older curve records are discarded
without changing scalar history, settings, partition addresses or OTA slots.
Curve JSON keeps the seconds-based `dropS`, `extendedS`, `atmS`, `atmClearedS`
and `endS` fields with three decimal places; scalar shot metrics retain their
existing precision and meaning.
A 4,016 KiB dedicated partition holds 501 reserved 8 KiB data blocks and two
4 KiB clear-epoch sectors. A maximum-size record occupies 5632 bytes. Writes
erase only its one or two used physical sectors, transfer only the used record
through the existing internal 1 KiB scratch, and commit the checksum-bearing
header last. A replacement uses the spare block, retaining the previous 500
records until commit. Startup scans only block starts, checks schema, lengths,
counts, times and checksums, and reconstructs the newest retained records.
Deletion clears a validity bit; the committed retention floor prevents old
evicted curves from returning after deletion. Alternating clear epochs prevent
curves from returning when shot IDs are reused. Immutable worker acknowledgement
carries physical progress forward while newer RAM mutations stay dirty.

Capture performs no flash I/O. After cutoff, control appends valid same-connection
drip observations to the pending curve until finalization or cup discontinuity.
The existing 1201-reading/60-second bounds include this tail; overflow retains
the captured prefix and sets `wTruncated`. Rejected readings, reference changes
and link discontinuities retain segment breaks. The cutoff event keeps its
original time; accepted post-drip yield updates its separate weight annotation.
Curve JSON exposes the points as the base64 delta stream `wD` (a fragment
also names the point it continues in `wRef`), segment-start indices in
`wBreakBefore`, held-reading indices in `wHeld`, and `wTruncated`. Weight
and flow consumers use actual
times; event annotations never become observed samples. The Web weight chart
draws post-cutoff observations dashed without fill and uses a fixed-size hollow
marker only when the complete tail's last observation matches settled yield.
Flow applies the same supported-window estimator to extraction and drip readings;
CSV includes both. Its faint, unfilled dashed continuation starts at cutoff,
with a visual-only interpolated boundary when continuous support exists.
Post-cutoff endpoint estimates with midpoints before cutoff do not alter the
earlier solid trace. Extraction Max flow excludes post-cutoff endpoints, while
the plot range includes drip rates. Scalar shot metrics,
independent Stats eligibility windows, sorting and exact-ID joins remain owned
by ShotLog. Other curve schemas start empty. OTA remains supported after the
one-time preserving USB layout transition.

Decoding checks the supplied length before reading record CRCs and copies only
that validated length; compact inputs do not require a full-store allocation.
Profile rules are immutable: changing prediction, gain candidates or eligibility
requires versioned compatibility, not relabeling historical data. Numeric and
user contracts are in [BBW](features/brew-by-weight.md#cutoff-behavior-and-learning)
and [shot history](features/shot-history.md).

## Live settings notifications

The global `timezoneId` is an IANA region/city string in the settings
blob. Network validates it against the firmware's generated tzdata2026d
catalog; control owns the effective setting and first-auto provenance; the
existing persistence worker saves both in the same settings generation.
The `timezoneAutomatic` preference occupies former runtime padding at byte 6,
preserving the remaining layout. Saved zones and their provenance survive
same-schema OTA updates.
Automatic time zone defaults to on for new records.
Flash persistence publishes `timezoneInitialized` only after loading a valid
saved zone or verifying a settings write containing one; factory reset clears
it. Until then, a zone-only browser initialization is permitted without Admin.
Once initialized, zone-only browser updates require saved automatic mode.
Other date/time patches, including mode changes, still require Admin. Control
rechecks the revision and automatic-update eligibility before applying commands;
the existing worker retries persistence failures without repeated UI writes.
`src/OpenBrewByWeightTimeZoneData.h` is an immutable flash table of 597 supported
IDs and 4,341 transitions, deduplicated into 67 schedules for UTC instants in
2025–2099. `scripts/generate_timezones.py` regenerates it from the pinned
upstream archive without runtime allocation or a global C library `TZ` state.
There is no mutable catalog copy or per-request allocation. On n16r8, the
existing PSRAM XIP profile places this read-only data in PSRAM at startup;
other profiles keep their configured read-only mapping. The API exposes the current
zone and the offset resolved at the current UTC instant separately. Without a
configured/resolvable zone, UTC offset zero is the explicit fallback.

The control task captures UTC and its resolved offset when a cycle ends.
Pending shot finalization keeps that pair in RAM through drip delay; the shot
and activation stores retain their existing UTC/local/offset representation
without a storage migration. NTP continues to own UTC synchronization and
does not change the monotonic control timers. The Web preview resolves a draft
zone without saving it; a browser UTC estimate is used only for preview when
the controller clock is unavailable.

First-auto provenance reads UTC once under the wall-clock owner's lock so
its timestamp and quality cannot disagree during an NTP state transition.
Configuration and preset saves are patches: the Web UI sends only the fields
that changed, and the firmware merges them into the saved configuration or
the stored preset recipe, validates the merged result as a whole, and
persists it. A preset save that omits a recipe field keeps its stored value;
a full recipe is still accepted.
Configuration and preset saves are acknowledged by their request ID and
result, together with the resulting revision; unrelated revision changes do
not acknowledge a pending command. Applied settings may still await the
existing asynchronous persistence worker.

Live settings commit publishes the new runtime snapshot, then dispatches a
fixed, allocation-free table of subscriptions on the control task. Each owner
provides its own old/new value predicate and callback; the settings dispatcher
knows neither the effect nor its destination. One callback runs at most once per
commit even when several of its input fields change. The NTP subscription
publishes the new network snapshot and its change generation under the same
data lock; the network task performs the rearm. Other settings commits still
publish the network snapshot without rearming NTP. Cup-placement and idle-tare
evidence likewise use separate RAM generations. Boot initialization and a
scale's initial connection retain their
own policy application paths. Persistence and status publication are independent
of these operational triggers. No callbacks run from an ISR or across a flash
write, and the OTA update path remains available.

## Settings encoding and evolution

Settings schema 2 (`OpenBrewByWeightSettingsCodec.h`) stores the settings as
groups of tagged fields: a header (magic, schema, body length, storage
revision), the body and a CRC-32. Each group (runtime, bullseye, device and
network, one per preset, preset bank, one per scale-history entry, webhook,
Linea Micra) holds `tag, length, value` fields with LEB128 tags and lengths.
Every persisted struct member has one table row; `static_assert` checks that
each table covers its struct apart from padding, so a member added without a
row fails the build. Encoding is canonical (table order, every field, text at
its real length), and a save verifies the read-back length, revision and CRC.

Loading starts from the struct defaults. Unknown groups and fields are
skipped, so earlier firmware reads a later blob; a missing field keeps its
default, so later firmware reads an earlier blob. Integers convert between
widths while the value fits. A malformed value (bad length, out of range,
unterminated text) keeps its default and makes the load a repair, logged as
`CONFIG_REJECTED` in the Web UI log like a section repair. A blob that does not
frame, fails its CRC, or belongs to another schema epoch never loads.

Rules for every later change:

- a released tag keeps its meaning forever and is never reused; a retired tag
  is listed in the codec header;
- a member whose type or meaning changes gets a new tag;
- the encoded worst case stays within `PERSISTED_SETTINGS_NVS_BUDGET`
  (4,096 B, never reduced), so earlier firmware can always read a later blob;
- `CONFIG_SCHEMA_VERSION` changes only for a change the tags cannot express,
  together with a load path that converts the previous epoch.

The other persisted records (flash stores and the remaining NVS records) keep
fixed layouts guarded by `static_assert` size checks; a layout change there
needs a versioned load path or discards that record.

## Residual qualification

`autoTareOutsideBrew` remains a global machine setting rather than part of the
per-shot/preset recipe snapshot. No records from earlier schemas are converted
at boot. The optional idle accessory retare uses spare bit 6 of
the already-packed `noScaleBbwMode` byte. New and factory-reset records default
on; existing saved records retain their stored bit, including OFF. The bit is
preserved when the no-scale mode changes.

Idle tare arbitration lives in `control/OpenBrewByWeightCycleRuntime.inc`, reusing
the cup FSM's PLACED event and ScaleService's TARE_ONLY transport. Worker
request lifetime and control-owned cup provenance remain separate; see
[scale commands](STATE_MACHINES.md#scale-commands-scalecommandtype--outbound).

Source boundaries do not prove real FreeRTOS interleavings or interrupt
latency. Release evidence must still include the target trace and HIL runs in
`SCHEDULABILITY.md` and `P2_RESOURCE_BUDGETS.md`.

# Resource budgets

This file versions the firmware's quantitative resource limits. The P2 filename
is retained for existing tool and evidence references. It is
an implementation contract, not a substitute for target/HIL evidence.

## Firmware image and static regions

`config/resource-baselines.json` records two kinds of `-O2` baseline.
`targets.n16r8` is the shared development baseline: every measurement compared
with it must compile with the `--development` profile, which enables the admin
unlock and the USB Serial/JTAG console and gives a reproducible comparison
profile. `releaseTargets` holds one baseline per supported profile pair for the
image that ships: a flag-free release build (no `--development`, `--jtag`,
`--no-auth-admin`, or `--flags`). This is enforced, not just prose: the R2/R3
validation gate compiles every supported profile pair with `--development`
(FIRMWARE_BUDGET_OPTIONS in scripts/dev) and again as a flag-free release
image in its own `build-idf/release/` tree, and a tooling test pins that
composition for both risk levels and requires a release baseline for every
pair. Any other build profile makes the resource verifier print "no versioned
baseline for this build profile" instead of comparing baselines. Comparisons
must use the
same hardware and machine profiles and the same profile on both sides. Every
supported build emits `size.json` from the linker map and
records image bytes, total linked bytes, DIRAM, flash code, and flash rodata.
The verifier requires valid measurements for all five metrics at every
optimization level.
Builds without a matching baseline (other optimization levels, extra flags,
individual `--jtag`/`--no-auth-admin`) retain the OTA-slot, external-BSS,
and internal-placement checks. The development and release Web UIs differ, so
the development image is not always the larger one.
Both kinds use the same growth allowances.

The release baselines were first recorded on 2026-10-23 with ESP-IDF 6.1,
because until then no gate built or measured the shipped image. Release images
carry the password-unlock endpoints and lock panel that development builds
omit, so the release Linea Micra image (2,477,840 bytes) was 9,216 bytes over
the development baseline plus allowance (2,468,624) and its flash code
(1,670,372 bytes) 5,916 bytes over that allowance, while still filling only
78.8% of the 3 MiB OTA slot (667,888 bytes free). Nothing was removed to
fit; each pair now compares against its own measured release image:

| Pair | Image | Linked | DIRAM | Flash code | Rodata |
| --- | --- | --- | --- | --- | --- |
| `esp32-s3-relay-x1-speaker` + `la-marzocco-linea-micra` | 2,477,840 | 2,477,727 | 193,074 | 1,670,372 | 647,028 |
| `esp32-s3-relay-x1-speaker` + `rancilio-silvia-pro-x` | 2,309,696 | 2,309,575 | 189,682 | 1,596,204 | 553,028 |
| `esp32-s3-relay-x1-speaker-reed` + `rancilio-silvia-pro-x-reed` | 2,305,408 | 2,305,291 | 189,634 | 1,591,840 | 553,108 |

Small reviewed growth allowances catch regressions without coupling unrelated
toolchain padding to an exact byte count; raising a baseline or allowance
requires explicit architecture and resource review. The baselines and
allowances themselves live only in `config/resource-baselines.json`; this
guide does not repeat them, so a change that moves them edits one file.
The current baselines were measured with ESP-IDF 6.1, its GCC 15.2 toolchain,
and the qualified `CONFIG_FREERTOS_IN_IRAM=y` build profile. The shared
development baseline represents the largest reviewed supported profile: the
Linea Micra cloud build with WebSocket observation on
`esp32-s3-relay-x1-speaker`. The 3 MiB OTA slot remains the hard image limit
for every build, and static measurements do not qualify runtime heap or stack
behavior.

## Web UI budgets

`config/web-asset-budgets.json` holds every Web UI size limit, and
`src/tests/check_web_budgets.js` measures all of them in one pass of a few
seconds, listing every overrun together:

- `source.*` count the compact, English-localized authoring sources the Web
  contracts read (HTML with every partial injected, all JS modules, and both
  together). Complete, human-readable settings help is part of the UI
  contract: adding a setting raises the HTML budget when needed, and hints
  are never cut to fit.
- `gzip.*` count the shipped compressed assets: the shell HTML and `app.js`,
  the stylesheet, the runtime, the lazy OTA image module, each lazy view
  module, the manifest, the cold Home download (shell, stylesheet, `app.js`
  and runtime: what a first visit fetches before Home can paint, so view-only
  code belongs in the lazy view modules), and the combined embedded assets.
  The runtime is measured with a fixed sentinel build id, because the real id
  changes with every commit; the combined budget measures the real assets.
- `raw.*` count the two icons, which ship uncompressed.

## Changing a budget

Gates never fail one budget at a time. Every size check inside
`./scripts/dev test web`, `test budgets`, and the R2/R3 `validate` gates only
records what it measured; one final `budget-verdict` step judges all the Web
UI budgets and every development and release image together. When a reviewed
change legitimately grows past a limit, accept the measurement of that same
run with `./scripts/dev budget update --from <run-id>`: it raises only the
exceeded limits, to the measured value plus the file's `updateHeadroomBytes`
(64 bytes for the firmware allowances, absorbing padding and asset-tag
wobble; 32 bytes for compressed Web assets; none for the deterministic source
and icon counts), prints every change, and confirms the run now fits, with no
rebuild. A development and a release image share one allowance per metric,
sized for the larger growth. Record why each budget moved in the commit body,
not in this guide or in code comments. See
[Validation](../VALIDATION.md#size-budgets-and-parallel-work) for which runs it
accepts.

## Static memory placement

Moving the unused RMT and GDMA driver code and the task-profiler report out
of internal RAM lowered linked DIRAM from 187,366 to 177,434 bytes.
On the reference board, after a 75 s settle and 200 s of three concurrent
Web UI clients, the same session on the earlier and the reduced image
measured:

| Measurement | Before (187,366 B DIRAM) | After (177,434 B DIRAM) |
|---|---:|---:|
| Internal free heap after load | 85,803 | 96,035 |
| Largest internal block after load | 43,008 | 43,008 |
| Lifetime minimum internal heap | 38,160 | 39,828 |
| Boot reservation minimum free | 38,991 | 50,091 |
| Control-loop maximum gap | 11 ms | 11 ms |
| Lowest application stack (webhook) | not measured | 1,588 |

The extra free memory lies outside the largest block, so the fragmentation
ratio rises from 498 to 552 per mille although no free extent shrank. Shot,
webhook delivery, settings writes, OTA and BLE scale workloads, and the 8-hour
soak, are not covered by this measurement.

The n16r8 PSRAM XIP profile moves flash instructions and read-only data to
PSRAM at startup and prefers PSRAM for the NVS page cache and key hash list,
with internal fallback.
The n16r8 Micra development+JTAG build (`esp32-s3-relay-x1-speaker-reed` /
`la-marzocco-linea-micra`, ESP-IDF 6.1, validation at `79c4788-dirty`)
measured 2,123,936 image bytes, 2,123,811 linked bytes, 1,444,516 flash-code
bytes, 517,148 `.rodata` bytes, 177,438 DIRAM bytes (32,016 internal BSS), and
106,992 external-BSS bytes. The code and rodata require roughly 1.9 MiB of
PSRAM before mapping overhead. Linker figures do not measure runtime heap;
matched target memory, settings latency and loop-gap measurements remain
required before qualification. PSRAM access may slow NVS integer operations.

The 2026-10-22 instruction-cache change re-baselines DIRAM to 193,142 bytes
(owner-requested). All code runs through the cache from PSRAM, and a 32 KB
I-cache (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`, with the measured 64-byte
data line) removes the 16 KB IRAM-only heap region that IDF adds only with a
16 KB I-cache; its 32-bit users move into DRAM, so DIRAM grows by that amount.
Target profiles (Home page open, five-minute task profiler, matched builds)
measured the control loop at 14.8 % of a core without the scale linked and
11.9 % with the 32 KB cache, its phase average from 1,438 to 1,254 µs, and the
scale worker from 4.9 % to 4.4 %; the 64-byte data line alone changed nothing
measurable. The steady largest internal block is unchanged; the cost lands in
small side holes, and it is affordable only because the startup heap hold is
gone (see below).

Reviewed, measured and accepted lever, not applied: disabling
`CONFIG_ESP_WIFI_IRAM_OPT` and `CONFIG_ESP_WIFI_RX_IRAM_OPT` returns about
18 KB of IRAM to the internal heap (7.8 KB hot TX/RX path, 3.6 KB RX path and
6.7 KB of the shared RX/sleep path, which stays in IRAM only for the RX
option), enough to bring DIRAM back under the previous baseline. Matched
target runs with the scale linked and the machine on cost +0.3 % of a core in
the Wi-Fi task, no measurable change in the control loop, web server, NimBLE
or 300-packet ping latency (dominated by modem sleep), and no packet loss.
Bulk receive throughput (Wi-Fi OTA) was not measured and is not critical.
Coexistence arbitration is radio-time based, so it does not give BLE more air
time. Apply it when internal RAM becomes the constraint.

Both linker maps must also keep external BSS at or below `maximumExternalBssBytes`
in `config/resource-baselines.json` (currently 3,186,688 bytes) and retain
`localBuzzer` and `taskProfiler` in internal DRAM: both hold synchronization
state (mutexes, atomics) that must not live in PSRAM. The profiler's published
report (`taskProfilerReport`) must instead reside in external BSS.
The same map check now requires the published control scalars, control gate and
their mutexes to stay internal, and the published curve payload to reside in
external BSS. Runtime session fingerprints remain part of the explicitly
external Network allocation.
The timestamped-curve Micra development profile measured 756,272 external-BSS bytes,
183,510 DIRAM bytes and a 2,188,640-byte image with the task-only pending finalizer
in PSRAM. This ceiling detects
static-placement regressions; it is not the physical PSRAM limit or a
runtime-heap measurement. The earlier 96→104 KiB increase covered the V3
half-second shot-curve store, and the 112→240 KiB increase covers the
96→512-event diagnostic log ring, whose static PSRAM ring and serial dump
snapshot grew by about 123 KiB. The current 240→800 KiB increase covers the
498,420-byte accepted-observation cache, its bounded disk workspaces, the
1201-observation sampler and task-owned finalization/serialization staging.
The 800 KiB→2,880 KiB increase covers the 500-shot capacity's 2,566,020-byte
curve-record bank with its block-header index and deletion ring; the matched
development profile measured 2,916,120 external-BSS bytes, 177,946 DIRAM bytes
and a 2,369,456-byte image, re-baselining
`config/resource-baselines.json`.
The 2,880 KiB→3,112 KiB increase keeps each curve as one delta stream from
capture to the browser: a record reserves the stream's valid-record worst case
(5,274 bytes, 476 bytes over the former absolute arrays) so no shot is ever
cut short or evicted by curve size. Across the 500-record bank, the worker
image, sampler, published and finalization records it adds 237,504 bytes (about 232 KiB); the
matched development profile measured 3,156,312 external-BSS bytes. The
runtime PSRAM heap had 2.6 MB free before the change.
The immutable persistence image is separately allocated in external heap.
It mirrors the shot and activation logs but carries at most four unpersisted
curves (about 22 KiB) instead of a second curve bank; a larger backlog is
committed over successive images, so the 500-shot bank is not duplicated.

## Home shot stream target measurements

The 2026-10-05 comparison used the same ESP32-S3 N16R8, speaker/Micra
development pair, ESP-IDF 6.1, retained settings/history, disconnected scale,
disabled scale recording and existing cloud/webhook connections. Baseline
`0501da90` and corrected stream firmware `93dc21a4` were USB-installed and
sampled with the same procedure through 64 seconds after restart. These are
measured reference workloads, not new limits or full hardware qualification.

| Measurement, bytes unless stated | Baseline | Home stream | Difference |
| --- | ---: | ---: | ---: |
| Settled internal free heap | 80,971 | 80,063 | -908 |
| Lifetime minimum internal heap | 31,232 | 30,240 | -992 |
| Settled largest internal block | 40,960 | 40,960 | 0 |
| Reported PSRAM heap capacity | 5,467,008 | 5,462,016 | -4,992 |
| Settled PSRAM free heap | 4,187,424 | 4,182,512 | -4,912 |
| Lifetime minimum PSRAM heap | 4,132,076 | 4,135,112 | +3,036 |
| Settled largest PSRAM block | 4,128,768 | 4,128,768 | 0 |
| Linked DIRAM | 187,158 | 187,494 | +336 |
| External BSS | 756,976 | 757,144 | +168 |
| Firmware image | 2,292,288 | 2,310,512 | +18,224 |

Reported PSRAM capacity changed with the image; allocated PSRAM at the settled
comparison point was 80 bytes lower. Free-heap differences alone therefore do
not describe dynamic allocation growth. Separate boots measured internal minima
of 31,232–37,088 bytes for the baseline and 30,184–36,196 bytes for the corrected
firmware. Startup traces placed the transient dips around existing outbound
connections and the startup reservation; compare matched workloads rather
than attributing a lifetime minimum to the Home stream alone.

Two 100-reconnection runs recovered their initial internal free/minimum/largest
figures. A bound/overlapping connection temporarily reduced the largest internal
block by 2 KiB; it recovered after cleanup. Idle stream CPU samples were
0.35–0.46%, versus 0.25–0.30% settled without a stream. A 200-request resync burst
produced 25 snapshots over the burst and drain interval. A stalled reader closed
and a fresh stream recovered; concurrent Home/Stats/History/OTA-status requests
completed, with a worst observed response of 744 ms under three HTTP workers.
No additional external-allocation fallback or HCI drops were observed. A full
2,310,512-byte same-image OTA transfer also completed with the stream occupied;
the stream recovered after socket recycling during upload and after reboot.
The new boot's image digest matched and automatic confirmation succeeded.

The target's 396-point canonical snapshot was 4,730 bytes and matched Stats
weights, timestamps and breaks exactly. An offline maximum-width 1201-point
curve with every possible break and all event markers serialized to 20,703
bytes inside the existing 22,016-byte curve workspace. Active scale/drip timing,
maximum-curve target sends, HTTP-task stack margin and server-stop overlap remain
manual qualification requirements. OTA power-cut, safety-abort and rollback
cases also remain pending; the successful full transfer does not qualify them.

## Runtime placement and allocation

| Resource | Placement and bound |
|---|---|
| Network work buffer | external, bounded at 640 KiB; includes the Stats capture's full-capacity shot-record snapshot (500 × 124 bytes) with one-curve fetch scratch (also the empty-curve template), 19,554-byte curve JSON, 23,650-byte row JSON, 40,960-byte status JSON, the Home shot's curve copy, 3,792 bytes of session field fingerprints and a bounded page header/cache key; handlers share the work-buffer mutex. The earlier 629,376-byte measurement predates this layout; verify the current size in the matched firmware build |
| HTTP response send | complete assets and JSON use HTTPD Content-Length responses; streamed bodies retain chunked transfer. Source buffers pass directly to HTTPD's default socket send, which copies into lwIP; no application bounce buffer or extra copy |
| NVS metadata cache | PSRAM preferred with internal fallback; flash I/O still uses the internal scratch below |
| Shot-curve store | external, 2,804,004-byte cache for 500 delta-stream records of 5,608 bytes (stream reserved for the valid-record worst case); bounded block-header index and two 5,632-byte disk/verification workspaces belong to the same owner; copies move only used stream and bitmap bytes; immutable worker image is separately external and holds at most four unpersisted curves |
| Curve capture/finalization | external task-owned sampler (one delta-stream record) and pending/finalization snapshots; the published scalar status stays internal, bounded at 2,048 bytes; the live curve record (5,608 bytes, empty while idle) is published in PSRAM under the same mutex/version, copying only its used bytes. The full reader snapshot stays bounded at 7,432 bytes in the external HTTP workspace; no control-path allocation or large stack local |
| Home live stream | two fixed internal session contexts and one coalesced dispatch on the existing priority-1 tasks; 128 Home, 224 Diagnostic, 42 shot-card field and curve-scalar, and 80 Settings fingerprints per session occupy 3,792 bytes in the external workspace, plus one 1,436-byte external coalescing buffer owned by httpd and a 1,000-byte external batch rollback copy; boot/clock anchors stay internal. Shared 23,650-byte external row workspace and 19,554-byte curve scratch (base64 stream plus break/held indices), with no extra TX allocation; 1201 curve points maximum, progress after every second scale sample (150 ms floor, 300 ms without samples) and no unchanged data; four HTTP sockets retained |
| Shared flash-I/O scratch | internal heap, 3,328-byte capacity for one 3,304-byte PersistedSettings record; slots are read, written, and verified sequentially under the flash-I/O lock, with no PSRAM fallback; the larger partition stores transfer in 1 KiB chunks staged through the same scratch |
| USB serial output | internal heap, 2,064 bytes for the eight-record ESP log queue; one external 2,560-byte CLI reply buffer; startup failures free both allocations, and successful startup retains one boot-lifetime owner |
| Micra cloud workspace | external and lazy; a 6,344-byte work buffer on ESP32-S3 holds identity, tokens, authorization header, and client state while cloud observation is active, plus one request-scoped 16 KiB buffer whose mutually exclusive request-body and response phases share storage (22,728 bytes combined, excluding HTTP/TLS library allocations); Disconnect, disabled observation, STA loss, and AP entry destroy the client and free both blocks; WebSocket mode also destroys the client between operations, while API polling reuses it for up to 180 s idle |
| Micra WS/STOMP scratch | PSRAM-only reusable block, capped at 24 KiB; includes 17410 B accumulator, two 1025 B header scratch arrays, signed-header/CONNECT storage, and 60 × 12 B rate bins; retained across shot/acquisition pauses, freed after callback quiescence for API/disable/identity/network/maintenance changes |
| Micra SDK transport buffers | HTTP RX/TX 4096 B each; WebSocket RX/TX 4096 B each plus a separate 4096 B upgrade buffer. Ordinary malloc prefers PSRAM above ALWAYSINTERNAL=2048, with possible internal fallback. The 6144 B internal WebSocket task stack remains on core 0 priority 1; event/transport objects and WSS TLS are separate; dynamic SDK buffers and auto-reconnect disabled |
| Micra TLS allocations | external through the Micra profile's mbedTLS allocator (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` from `sdkconfig.defaults.micra`); dynamic record, certificate, handshake, and session objects use PSRAM without internal fallback. Generic webhooks accept HTTP only and do not create TLS handshakes |
| Profiler processing workspace | external, at most 4 KiB, only while running |
| Profiler published report | external BSS, 2,512 bytes; written and copied under the profiler's internal mutex by tasks only |
| OTA state singleton | external BSS, about 1.6 KiB (identity, tags, journal bookkeeping, tag scanner); its mutex and published flags are internal statics, and chunk buffers, SHA context and tag re-reads use internal memory |
| Scale profiler workspace | external, only while a manual capture exists: 256 KiB record buffer, header/context and a bounded per-signal observation cache (32 bytes per signal), and a 32 KiB ordering index after stop (combined ceiling 320 KiB, enforced including the index); freed after the trace is saved and no download is streaming it; capture storage is a dedicated 260 KiB flash partition with a header-last commit |
| Profiler kernel capture | internal, at most 4 KiB, only while running |
| Settings handoff | one external mailbox for the 3,312-byte settings blob and revision, and one internal byte queued; no full settings copy in the queue or receiver |
| Web command | trivially copyable, at most 416 bytes; configuration and network payloads share a discriminated union |
| Radio settings snapshot | at most 224 bytes; full 3,312-byte settings remain for durable mutations |
| Wi-Fi static pools | eight RX and sixteen TX internal DMA buffers reserved while Wi-Fi is initialized, approximately 1.6 KB each; about 19.2 KB more internal RAM than the former six RX/six TX pools |
| Wi-Fi packet queues | dynamic RX and cache TX each bounded at 32 packets; RX block-ack window 16. Cache TX holds overflow packets when static TX buffers are busy; it is not an equivalent preallocated DMA pool |
| TCP capacity | 16 KiB send/receive limits and a 16-entry receive mailbox per connection; payload memory grows under load, with PSRAM preferred where supported. Fully loaded bidirectional payload capacity is 27,008 bytes higher per connection than the former 2,880-byte limits, before metadata |
| mDNS responder | NetworkService-owned; mDNS 1.13.1 places its 4096-byte priority-1 task stack on core 0 in internal RAM for core dumps; dynamic responder allocations remain in PSRAM; one persistent UDP socket; freed once in `OpenBrewByWeightNetwork::stop()` |
| Fixed buzzer melodies | at most 8 notes each; custom tune capacity remains 250 notes |
| JSON parser | PSRAM only; Web input remains at most 2047 bytes / 128 values; the Micra worker explicitly admits at most 16 KiB / 1024 values for bounded cloud responses; nesting remains 32 |
| BBW adaptive candidates | control-owned fixed RAM, at most 3,000 bytes for eight presets; 20 observations and five trajectory anchors each |

The timestamped-curve Micra development image has compiled entry frames of
3,216 bytes for control-status publication, 480 bytes for finalization scheduling,
6,944 bytes for the status handler and 4,720 bytes for integration. The control
loop has an 8,192-byte stack and HTTPD has 11,264 bytes. These frames exclude
nested calls and interrupt overhead; target stack-watermark qualification is
still required. Curve helpers fill their owned destinations directly, and
control-status publication initializes its existing object in place, avoiding
large return-value and default-aggregate temporaries.

The destructive paths have a whole-call-chain bound instead: the stack-depth
gate in [Validation](../VALIDATION.md) covers boot recovery on the loopTask,
the Web UI factory reset on `network_manager`, and the OTA transfer and commit
on `ota_worker` and its inline httpd fallback. No `PersistedSettings` copy
lives on those stacks: the network task edits a PSRAM working copy and NVS
stages through the internal scratch. After every boot recovery the next boot
writes `factory reset left N B of loopTask stack unused` (or `network access
reset ...`) to the Web UI log, a warning below 1,024 bytes, so field units
qualify the static bound against the real watermark.

Network command builders must activate their union member with
`setNetworkType()` before writing credentials. Preset metadata remains outside
the union because a preset operation also carries configuration. Settings
schema 0 uses a 3,344-byte blob for the bounded Micra cloud account, selected
machine, and per-scale friendly names. Blobs from earlier schema epochs are
rejected. The connection byte uses prior padding; the checksum stays at offset
3336. Trial-boot writes wait for OTA confirmation to retain rollback settings.

History V5 retains an exact bounded preset-name snapshot and transfers through
the shared chunked flash-I/O path. The separate last-shot V4 record retains the
same provenance. The Web UI size caps live in the Web contracts
(`src/tests/web_contracts/10-assets-security.inc.js` and
`60-generated-ota.inc.js`) and are the authority; as of the frontend audit
(2026-10-16) the English authoring caps are 93,300 HTML bytes, 444,698
JavaScript bytes and 530,162 combined source bytes. Compressed, the always
loaded runtime is capped at 48,300 bytes and the cold Home download (shell,
CSS, app and runtime) at 74,000 bytes. Each lazy view module carries its own
cap: Stats and History 4,500, Diagnostic 12,450, Admin 14,200, Settings
14,150 and Setup 3,600 bytes. All embedded assets together are capped at
147,350 bytes; the optional Web UI login (sign-in form, Access panel, Home
notice, Home Assistant pairing prompt; owner-requested 2026-10-22) measures
151,355 bytes (with the Admin deep link from its Home note, one password form
at a time, the factory-password reminder, Sign out in the footer and the
signed-in device list with inline renaming) and raises that cap to 151,380 bytes. The Settings group rows
(an icon and a live summary per group; owner-requested 2026-10-22) measure 153,049 bytes and raise it to
153,080 bytes; Admin's sections as the same rows measure 154,072 bytes and raise it to 154,100 bytes;
Diagnostics' sections as rows with icons measure 154,901 bytes and raise it to 154,930 bytes,
with the per-asset caps raised to their measured values in the Web contracts. Its firmware state (~400 bytes) lives in external BSS and
one 388-byte NVS blob (196 bytes before device labels; migrated on load). The selectable
WS implementation and its correctness fixes are included in the reviewed flash
baseline above. The OTA-slot, DIRAM, external-BSS and other asset limits remain
unchanged. The reference clean development build with WS measures 2,273,760 image
bytes, 2,273,647 linked bytes, 1,515,100 flash-code bytes and 587,352 rodata bytes.
The HTTP-only dependency-preparation image was 2,228,768 bytes; WS and its
correctness fixes add about 44 KiB, including differences in version metadata.
Linked DIRAM rises by 448 bytes to 185,382; external BSS stays at 756,272.
The clean image retains 871,968 bytes in its OTA slot.
These are linked measurements; handshake/streaming peaks still require target
qualification. Fixed-version English assets measure 115,211 combined gzip
bytes and 9,685 secondary-module gzip bytes. The rounded compressed caps retain
789 and 115 bytes respectively; other per-asset and source caps stay unchanged.

Every new setting must include concise, natural help that explains its effect on
the barista's workflow, including what changes when an option is enabled or
disabled. Adding a setting is expected to increase the Web UI budget, and the
applicable source, compressed-asset, and firmware limits must be raised through
the normal measured review when necessary. Removing, shortening, or making help
less useful merely to fit an earlier budget is not acceptable: a clear,
friendly, well-constructed UI takes priority over preserving the previous Web UI
byte allowance. Per-asset caps remain independently enforced by Web contract
tests, so one asset cannot consume all combined headroom.

Both supported partition tables reserve `shotcurve` at custom subtype
`0x40`, exactly `0x3EC000` (4,016 KiB): 501 × 8192-byte record blocks
plus two separate 4096-byte epoch sectors. The maximum used record is 5632
bytes, leaving 2560 reserved bytes per block. A shorter curve erases/programs
only its used length and ignores stale bytes beyond it. The partition occupies
`0x775000`–`0xB60FFF`. The installer accepts only the exact current table;
any other layout needs a USB `--erase-all` installation.

Capability samples use `INTERNAL|8BIT` and `SPIRAM|8BIT`, including the PSRAM
minimum-free watermark. The retained boot capture uses at most 768 bytes of
static PSRAM plus one internal static task mutex and an atomic sampling flag.
The startup heap hold requests 69,632 bytes (68 KiB). A 2026-10-18 sweep on the
release image (central run 102,400 bytes at network start, two boots per size)
measured steady largest block / lifetime minimum of 38,912 / ~67,000 bytes with
no hold, 45,056 / ~41,000 at 52 KiB, 59,392 / 39,828 at 68 KiB, 63,488 /
36,492–39,828 at 72 KiB and 65,536 / 28,152–29,820 at 76 KiB; larger holds also
shrink the side blocks while held. 68 KiB is the largest hold that keeps the
lifetime minimum on its plateau. The lifetime minimum includes the held block,
so the no-hold figure is not lost capacity. For that reason the published heap
minimum restarts at the release (ESP-IDF local minimum monitoring): health,
the Home stream and the Diagnostic page report `UINT32_MAX` (shown as waiting
for startup to settle) while the block is held, and the sweep figures above are
startup watermarks, not comparable to post-release readings. The debug export
and the Home stream also keep the minimum reached before the release as
`minimumFreeHeapBeforeReleaseBytes`, and the Diagnostic page shows it beside
the post-release figure in the Heap min row.
Zero skips allocation and records `disabled`,
then starts the 60-second post-release window immediately. The reduced WS task
stack still requires target qualification.

The startup hold was removed on 2026-10-22. Target heap maps show what it did:
the IDF heap serves internal requests from the main DRAM first, so the ~33 KB
Wi-Fi bring-up wave lands there by design, while the 32 KB internal reserve
pool (`SPIRAM_MALLOC_RESERVE_INTERNAL`) and the 32 KB DRAM shared with the data
cache are lower-priority fallbacks. Holding the central run pushed the wave
into those fallbacks, consuming the emergency reserve for a larger steady
largest block nobody needs; with a 32 KB instruction cache it also left only
17,195 free internal bytes (8,704 largest, 9,535 DMA-capable) during the hold.
Without it, the wave splits the central run (largest block about 31 KB, above
the 24 KiB health clear level), both fallbacks stay intact, and the first
minute after network start keeps about 73-81 KB free. Health, the Home stream,
the Diagnostic page and the debug export report one heap minimum, measured from
network start (ESP-IDF local minimum monitoring): the ESP-IDF v6.1 lifetime
minimum under-reports by the 32 KB reserve pool (esp-idf#19261). `BOOT_HEAP`
and the export's `bootHeap` section start at network start and drop the held
phase.
It also samples `INTERNAL|DMA`; the overlapping capability pools are not additive.
First-stage samples and approximately 100 ms health-task samples maintain separate
hold/post-release minima plus an independent 60-second minimum from the first
WS outcome, without resetting lifetime watermarks. Client-init and task-start
stages expose the setup allocation phases. Sampling stops when the release
and WS windows finish; a WS start without an outcome is bounded to 60 seconds
from that first start. Records remain available through
[`BOOT_HEAP`](SERIAL_CLI.md#capture-startup-memory) until reset. These sampled
minima can miss shorter dips; missing phases/stages are explicit in the report.
Diagnostic `memoryAllocations` reports cumulative
successes, failures, largest requested size, and last failed size by owner for
the application's capability-allocation wrappers. These counters are not live
allocation counts and do not include allocations made directly by SDK code.
The retained legacy external-fallback counter stays zero: the application's
explicit external allocator has no fallback. It does not measure ordinary
SDK malloc fallback or prove that all SDK allocations landed in PSRAM.
JSON and Micra-profile mbedTLS still allocate individual objects, but those
allocations no longer churn the internal heap; an arena would require separate
lifetime/concurrency evidence.
The Micra worker deletes each bounded cloud document before releasing its
request buffer, so those temporary PSRAM blocks can coalesce after each poll.
The compatibility field `jsonArenaExternal=false` means no arena is installed;
it does not describe the placement of the independently allocated documents.
The 4096-byte OTA transfer chunk remains request-scoped; retain it across
requests only if target traces justify the extra resident memory.

Internal-heap diagnostics also publish allocated, free, and total block counts.
Heap alerts use raw internal free bytes and the largest block. A valid zero
is exhaustion, including a zero largest block. Invalid or stale samples cannot
clear the heap latch or request a restart; a fresh valid sample is needed.
The existing hysteresis, five-minute sustained-low window and configuration-safe,
open-circuit restart admission remain unchanged.
`internalHeapFragmentationPermille` is
`1000 × (total free - largest free block) / total free`, guarded to zero when
total free is zero or the reported largest block covers it. ESP32-S3 internal
memory contains multiple allocator regions, so this ratio is a trend indicator,
not a claim that all free bytes can form one allocation. HTTP, Wi-Fi, OTA,
Micra TLS, and webhook owners retain only a bounded last before/after sample,
signed deltas, cycle count, stale-start count, worst free/largest loss, and
maximum free-block increase. Sampling occurs outside owner locks and outside
OTA chunk/cache-off work; only the fixed result is copied under the existing
owner mutex.

The current n16r8 Micra development+JTAG build measures 177,434 linked DIRAM bytes.
The one-record scratch reserves 3,328 bytes in its lazy runtime allocation,
while the disabled-USB path avoids the 2,064-byte serial payload. Their actual
free/largest-block effects remain target measurements, not linked-memory claims.

## OTA NVS endurance

The resumable OTA journal alternates two NVS keys (`j0` and `j1`). SHA-256 is
updated incrementally for every received byte. A journal record snapshots that
hash state only after each 512 KiB of new data, so ordinary 64 KiB HTTP ranges
do not each cause an NVS write or a full-prefix rehash.

| Board | OTA slot | Maximum journal `putBytes` per attempt | Key removals on close |
|---|---:|---:|---:|
| n16r8 | 0x300000 (3 MiB) | 6 | up to 2 |

The write count is `1 + floor((imageBytes - 1) / 512 KiB)`: one empty-session
record and checkpoints strictly before completion. `esp_ota_end()` validates
the completed image; completion is not journaled. Both keys are removed when a
session completes, expires, is discarded, or fails.

`OTA_SUPPORTED_MAX_SLOT_BYTES` and `OTA_JOURNAL_MAX_WRITES_PER_SESSION` encode
the upper bound. Host tests fail if a supported partition grows past the
versioned budget without an explicit review.

This is a per-attempt bound. Repeated operator-initiated failed attempts remain
unbounded over product life, so release qualification must retain NVS wear
levelling, capture the attempt count externally, and include interrupted OTA
in the combined soak. The firmware does not claim a service-life figure until
the flash vendor endurance, NVS partition geometry, and expected field update
rate are fixed for production hardware.

## NVS capacity

The supported partition table reserves `0x65000` bytes (404.5 KiB) for NVS.
With 4 KiB pages this gives 101 pages. The capacity contract conservatively
reserves two whole pages for NVS housekeeping/compaction, leaving 12,474
32-byte entries for application records.

The worst-case application set is budgeted by the NVS blob rule
`2 + ceil(bytes / 32)` per blob, plus namespace and scalar-key entries:

| Records retained together | Maximum entries |
|---|---:|
| Settings A/B (`2 × 3,056 B`) | 196 |
| Last Shot (`2 + ceil(356 / 32)`) | 14 |
| BLE settings A/B | 6 |
| Recovery intent | 3 |
| OTA journal A/B | 24 |
| Reset history, active pointers, and namespaces | 32 |
| **Application total** | **275** |

Shot and activation history use their dedicated flash partitions rather than
NVS entries. The resulting conservative compaction margin is 12,199 entries
(97.8%). Host
tests bind the large record sizes and this arithmetic to the installed
partition layout.
Diagnostic status and debug exports publish the installed partition size,
layout match, NVS used/free/available/total entries, namespace count, failure
count, last failing subsystem/operation/error, and flash-I/O lock timeouts.

## Combined heap/timing soak

`scripts/p2_soak.py` captures one status JSON object per interval as JSONL and
emits a machine-readable `.summary.json`. Headers, including an optional WebUI
claim, are read only from `OPENBREWBYWEIGHT_SOAK_HEADERS`; they are never copied into
the evidence. Prefer the public debug export (it carries the same health and
heap telemetry the retired status page served) when the Diagnostic page is
enabled:

```sh
python3 scripts/p2_soak.py \
  --url http://192.168.1.50/api/v1/debug/export \
  --output artifacts/p2/combined-8h.jsonl \
  --scenario combined-ble-wifi-webhook-ota-nvs \
  --duration 28800
```

During the window, exercise BLE scanning/link/notifications, Wi-Fi scan and
reconnection, Web UI polling, webhook delivery/failure, settings and shot-log
writes, and interrupted/resumed OTA at every checkpoint. The runner fails on
fetch errors, reboot/uptime regression, stale snapshots, deadline misses,
increased BLE allocation fallback/HCI drops, heap below the versioned limits,
stack below 1536 bytes, PSRAM free below 128 KiB or largest block below 76 KiB,
or sustained internal free/largest-block loss over 16 KiB, free-block growth
over 8, or fragmentation growth over 50 permille. Zero values are retained
and fail the limits; missing, invalid or unavailable required samples fail.
Control, scale-worker and BLE-host stack samples are required in every snapshot.
Use repeated `--require-task NAME` options for additional profiler tasks; each
requested task must appear in a running-profiler sample at every interval.
The free-margin threshold applies to those required tasks and the three
continuous health tasks. Other SDK tasks may have smaller configured stacks
(IDLE has only 1536 bytes total); their observed minimum is reported separately
as `profiledStackMinimumBytes`. Zero fails for every observed task. Qualifying
additional SDK tasks requires selecting them and reviewing their own margin.
The profiler stops after five minutes, so qualify those tasks in separate
bounded captures and retain their evidence with the long soak. A long capture
of the three always-exported stacks alone does not qualify every task.
`--min-stack-bytes` controls the stack gate; the deprecated `--min-stack-words`
alias also takes bytes for compatibility. Capture schema 2 and summary schema 3
use explicit byte names and add block topology plus per-lifecycle recovery
metrics. Review allocation-failure counter deltas for every exercised owner.

The diagnostic snapshot also carries webhook worker/client lifecycle and
bounded before/after heap samples for HTTP, Wi-Fi, OTA, webhook, and Micra
TLS. Repeated `--require-lifecycle FAMILY` options require the selected family
to advance its cycle count and recover its largest block within the configured
limit; the summary reports final/worst deltas, block-count trends, and the
largest/free ratio trend per family. The analyzer also rejects
more than one worker creation during a capture, missing lifecycle counters, or
more than one live HTTP client. A disabled webhook keeps an already-created
worker idle; only service shutdown performs stop/ack/join and releases it.

Required release artifacts are the JSONL, summary, firmware build ID, board
revision, board architecture, power/RF setup and an operator timeline marking
each injected event. Run 8 h for scheduling qualification and 72 h for the
long-term memory gate. A passing host self-test only validates the analyzer:

```sh
python3 scripts/p2_soak.py --self-test
```

## WebSocket memory qualification

Diagnostic `lineaMicra.websocket` reports retained application bytes/placement,
allocation failures, internal free/largest-block deltas for connect and stop,
and internal/PSRAM minima plus PSRAM free/largest values sampled at lifecycle
boundaries. `heapLifecycle.micraTls` remains HTTP-only; `micraWebsocket` records
WS connection history. Counts exclude TLS, SDK stack and allocator overhead.
These are bounded samples, not continuously observed peaks or proof of placement.
The internal stack and SDK buffers remain independent reservations even when TLS
uses PSRAM. Target comparison must include simultaneous HTTP/WSS handshakes,
1000 pause/reconnect cycles, full disable and OTA maintenance. Firmware/linker
sizes and XIP/static PSRAM reservations do not measure runtime heap pressure.

# ESP and FreeRTOS resource ownership

BBW prediction/learning has no resource authority. Control owns the fixed
per-preset candidate bank and immutable shot/finalizer snapshots; persistence
owns deferred durable writes. Network consumes the published Home snapshot;
bounded history reads and explicit user mutations share the one static
`shotStoreMutex` with control finalization and persistence-image capture. The
`ActivationStores` component is the single explicit owner of the two
activation ring stores (stats shot log, activation history); the PSRAM cache
of the newest 500 shot curves sits beside it under the same mutex. The curve
sampler, retained records and task-owned finalization/JSON workspaces use
PSRAM; flash transfers use the shared internal scratch. Curves reach flash
only as their shots' records in the shot trace flash pool (below), never
through the shot-store image.
Every read, append, page query, mutation, and immutable image capture runs under
that mutex. Control skips clean record payloads. The generalized persistence worker
writes the captured image without the mutex; control clears dirtiness only when
the completed generation is still current. Control never acquires the flash lock. See
[BBW policy and storage](ARCHITECTURE.md#bbw-policy-and-storage).

The v1 Home shot stream carries the existing card's scalar values and accepted
curve observations. Sequence/base, boot/cycle/shot identity and curve cursors
govern replacement and append updates; bind and resync share one coalesced
dispatch. Unavailable weight, average flow, first drop and tare are explicit
`null` values. Active average flow uses the control owner's fresh accepted
weight, known scale baseline and confirmed first drop. Qualifying results use
the exact Stats record; nonqualifying results preserve their firmware cutoff
time. This presentation contract adds no guard decisions or actuation messages.

Every changed control-gate publication requests an urgent stream update through
the existing atomic RF notification, including physical activator ON/OFF edges
that do not change the active-cycle or relay state. Network bypasses the
sample-paced periodic stream cadence for that request; a coalesced HTTP
dispatch still owns all serialization. Queue or workspace contention preserves
the request for a later Network service pass.
Control never performs serialization or waits for this presentation update.

The published control scalars and synchronization stay in internal RAM, with a
2 KiB scalar budget. The 1201-point curve payload lives in PSRAM. The same
publication mutex commits and copies both parts under one snapshot version;
readers own their complete copy and never retain a mutable publisher pointer.

The same socket carries independent Home field patches, including Equipment,
quick settings, presets, shared navigation and command readback. Each fixed
session owns a fixed PSRAM slot with 128 Home, 224 Diagnostic and 32 shot-card
CRC32 field fingerprints, plus an internal clock anchor; fields have stable
ordinals and formatting at display precision. Initial/resync frames replace the
browser cache, then unchanged values produce no data frame. Disconnected scale
weight/timer are never inspected; a single null transition clears them. Serialization
and transmit reuse the existing HTTP workspace and bounded dispatch. The
periodic dispatch is paced by scale weight samples: the scale worker only counts
each sample and, while a stream is open and a dispatch is due, notifies the
Network task; it never serializes, locks or waits for the stream.
The browser uses socket activity/alive control messages for liveness and power
activity, rather than a periodic Home REST request.
The bounded Home presentation projection uses size optimization and shared
typed formatting calls to keep the existing firmware growth budget; control
and safety retain the build profile's performance optimization.
Fingerprint slots belong to the existing external Network workspace and are
reset in place on handshake and session release. HTTP server shutdown releases
its contexts before the workspace is freed; no task, allocation, or external
mutex storage is added.

ScaleService publishes the bounded friendly/raw BLE name in its link
snapshot and retains the latest successful shot-tare result under the critical
event mutex. Control copies the name into the shot and correlates tare completion
by cycle and request before capture/finalization; event loss or a new link epoch
cannot erase that diagnostic fact or rearm control on a stale connection.

Every fallible resource acquisition needs one owner and a defined rollback
path. Read this before adding queues, tasks, clients, or persistent handles. `UniqueResource<Handle, Deleter>` is a one-handle-wide,
non-allocating owner with move, `release()` and `reset()`. It is used only for
C handles whose cleanup is safe in the owner's context. FreeRTOS tasks are
never deleted by this wrapper: their owners retain explicit stop/ack/join.

| Resource | Owner | Teardown/rollback |
|---|---|---|
| Dynamic CPU limits | ControlOrchestrator via `applyPowerProfile` | boot-lifetime `PowerClock`; configure only changed limits outside locks, fixed-80-MHz fallback on failure; Arduino clock setter only during boot |
| Energy activity mailbox | HTTP publishes bounded presence; control expires it and publishes applied profile/cooldown; ScaleService and NetworkService publish busy/error state | static atomics, no new tasks or flash writes; requests never authorize actuation |
| BLE modem sleep / scan and Wi-Fi sleep | ScaleService / NetworkService respectively | owner-local application, live link/connecting gates, saved preferences restored when PM is off; see [Power management](settings/power-management.md) |
| scale NimBLE peer procedure | EspressoScaleBLE client, called only by ScaleService | owns final admission and retains pending link cleanup until closure is confirmed; scanning/reconnection wait for cleanup and the full 3,000 ms quiet interval from GAP completion (or completion of an already-admitted submission); callbacks publish evidence, while only the owner submits deferred termination |
| Micra cloud HTTPS client | build-selected `OpenBrewByWeightMicraService` worker | boot-lifetime 8 KiB task; a lazy 6,344-byte external work buffer holds identity, tokens, authorization header, and client state, while request E/S uses one transient 16 KiB external union; HTTP/TLS library allocations are separate; connected-scale shot transitions cancel active I/O, while Disconnect, disabled observation, STA loss, and AP entry wipe/free both workspaces and destroy the client handle; WebSocket mode also destroys the client between operations, while API polling reuses it for up to 180 s idle |
| webhook `esp_http_client` | `WebhookDispatcher::httpClient_` (`UniqueResource`) | normal cleanup after worker join, or on the worker's next pass after webhooks are disabled or the URL changes; destructor is final rollback |
| OTA write handle | `OpenBrewByWeightOta::otaHandle_` (`UniqueResource`) | abort under `FlashIoGuard`; `release()` transfers it exactly once to `esp_ota_end` |
| OTA SHA context | `OpenBrewByWeightOta::sessionSha256_` (`UniqueResource`) | `psa_hash_abort` then capability-aware heap free |
| OTA state singleton | `OpenBrewByWeightOta::instance()` | PSRAM BSS object constructed on first use; its mutex, published-snapshot mutex and lock-free published flags are internal statics; the published status copy lives in the PSRAM object; flash chunks, the SHA context and tag re-reads use internal memory only |
| Network command queue and lifecycle semaphores | `OpenBrewByWeightNetwork` (`UniqueResource`) | acquired before task creation; reset in reverse order after manager join |
| boot heap capture | `BootHeapCapture`, task-only producers under one internal static `TaskMutex` | fixed payload of at most 768 bytes in PSRAM; first stage samples retained until reset, health-task sampling stops 60 s after network start (and after the first WebSocket outcome); `BOOT_HEAP` copies each immutable record under the mutex and prints outside it; no task, dynamic allocation, flash write or credential capture |
| internal-heap block maps | `HeapMapSnapshot` array, producers and readers under one internal static `TaskMutex` | one PSRAM-only allocation (about 26 KB, no internal fallback) at the first capture, never freed; boot stages written once, the live view replaced per Diagnostic download and skipped while a shot runs; the download reads one row at a time under the mutex |
| relay `esp_timer` constructor temporaries | local `TimerRollbackOwner` | automatic reverse rollback until both timers and the independent timer are ready |
| network/webhook tasks | owning service, borrowed `TaskHandle_t` | stop request, task acknowledgement, join, then queues/buffers/clients |
| scale and persistence tasks/queues | boot-lifetime owning service | BLE startup failure remains TWDT-covered through a bounded 1 s native-host stop, then clears the task handle and releases callback queues only after quiescence; resources remain stable after successful boot |
| native NimBLE host task and live stack telemetry | native BLE runtime lifecycle owner | health readers retain task lifetime under a static mutex; stop joins and deletes under that mutex within its timeout budget; competing health reads use cached telemetry |
| idle scale tare status and approved pre-write sample | ScaleService; control publishes validated sample copies and accesses its request API | task mutex serializes sample copies/claim/cancel/status; never held during BLE writes; WRITING cannot be cancelled; control releases terminal/expired requests and owns anchor translation |
| ordered scale result/weight handoff | ScaleService producer, control consumer | result queues, fallback mailboxes and static 16-weight FIFO share one task mutex for publication and selection; overflow drops the incomplete weight window and marks discontinuity; no allocation or dynamic teardown |
| static task mutex/event storage | containing static object | no heap allocation and no dynamic teardown |
| HTTP server | NetworkService | manager-task-only stop/restart; handle cleared immediately after `httpd_stop` |
| Home WebSocket | existing priority-1 HTTP server; NetworkService queues at most one dispatch | two fixed session contexts allow one bound owner and one handshake; binding expires after 5 s, takeover/close/server stop release contexts; 128 Home, 32 shot-card and 80 Settings field fingerprints per session deduplicate each frame's fields, and the curve streams from a per-session cursor; serialization reuses the shared PSRAM workspace; one httpd-owned 1,436-byte PSRAM buffer sends a frame's header and payload, and a dispatch's Home, shot-card and Settings frames, in one write; a write with nothing sent defers after 100 ms (a deferred batch rolls every frame's fingerprints back from a workspace copy and resends the same deltas), a started write allows 500 ms for modem-sleep ACK gaps and incomplete frames close; each session gets 800 ms of one dispatch, a deadline that also caps every one of its waits, and a session that has spent it is closed before its next stream stage, so a stalled peer cannot hold the httpd task past the budget or starve REST or OTA; Stats targets 8 KiB row batches (a larger single curve stays intact), with at least 100 ms between successful continuation sends; no new task, application TX allocation or event queue |
| mDNS responder and its service task | NetworkService | network-task-only `mdns_init`/`mdns_hostname_set`/`mdns_free`; mDNS 1.13.1 places its 4096-byte task stack in internal RAM for core dumps; dynamic responder memory stays in PSRAM; always-on passive responder; `mdns_free` only in `stop()` after the task join |
| crash capture and two-slot archive | ESP-IDF panic writer owns the capture during panic; boot promotion and NetworkService own normal access | the relay opens before the RTC address snapshot; normal access holds the shared flash lock, commits a historical slot only after verification, and requires Admin unlock for raw download or confirmed deletion |
| session Quick Settings overrides | control task (`sessionQuickSettings`) | RAM only and never persisted; published with the recipe snapshot under `recipeMutex`, so integrations read effective values while Network persistence reads the saved config; the control status snapshot also carries the saved value each override hides, which the Settings stream sends as `quickRevert.saved` so the Settings form shows saved values while Home and the header panel show effective ones; cleared by an active-preset change or reboot |
| persistence mailbox | control producer, then persistence worker | fixed-capacity internal work queue plus one external settings request and one PSRAM shot-store image; generation-tagged completion prevents clearing newer dirtiness |
| drip quick start record | control task (`bbwQuickStart`); settings_persist worker writes the staged copy | `bbwQuickStartMux` guards only the staged copy and its pending flag; the worker saves NVS key `bbwQs` on its idle cadence and retries on failure; factory reset drops the stage and erases the key, so the next boot re-arms the factory presets |
| Web UI login state (`g_webAccess`) | NetworkService; HTTP handlers borrow it under `dataMux_` | PSRAM BSS, no task or allocation; handlers change RAM only and mark it dirty; the network task copies it to its stack and saves NVS key `webAccess` through the shared flash lock while configuration is allowed, retrying after 30 s on failure; boot with the factory password and every factory-password restore turn the login off |
| reset-history durable state | existing maintenance lease and NetworkService persistence owner | control holds clear requests until the machine is configuration-safe; NetworkService writes through the shared flash lock, and control publishes completion only after success |
| shot history, curves, activation history and last-shot aggregate | `ActivationStores` RAM data layer and the `ShotCurveCache` of the newest 500 curves, plus the core-0 persistence worker; Network borrows only through mutex-guarded callbacks | `shotStoreMutex` covers RAM operations and immutable image capture only; no network I/O spans it and no flash I/O except the factory reset's durable reset (see [concurrency](CONCURRENCY.md)): an older curve's identity is looked up under it, the lock is released, and the pool record is read under the flash lock (busy while a shot runs); Home reads the cache only |
| webhook queue / payload | `WebhookDispatcher` | internal queue storage and external HTTP payload; release after worker join, or startup rollback |
| Micra WebSocket/STOMP | cloud worker owns lifecycle; SDK receive task owns framing between callbacks | one reusable PSRAM workspace capped at 24 KiB (header 1024 B decoded in place, body 16384 B, retained frame descriptor and 60 fixed rate bins); Home copies cleaning state under the existing mutex without aggregating traffic bins, while Diagnostic requests full traffic/heap enrichment; 4096 B RX and 4096 B TX library buffers and a separate 4096 B upgrade buffer use ordinary malloc with PSRAM preference; separate 6144 B internal SDK stack on core 0 priority 1; no SDK auto-reconnect; stop joins callbacks before destroy/free |
| Micra-profile TLS state | mbedTLS / owning HTTPS/WSS client | certificate, handshake, record, and session allocations use PSRAM only and are released by mbedTLS; no internal fallback |
| profiler workspace / capture | core-0 health worker via `TaskProfiler` | control/HTTP publish requests only; external processing workspace and separate internal kernel capture are freed on stop or failed start; the published report lives in PSRAM BSS and is written and copied only under the profiler's internal mutex |
| scale profile capture/store | core-0 health worker via `ScaleProfiler` | owns the extended capture clock, constant-memory capacity ETA and full-buffer completion; status readers only copy estimates; scale worker and control loop append fixed-size records through one leaf capture mutex after releasing their own locks; the settings_persist worker performs invalidate/save steps against the frozen immutable generation; a download lease pins it against Start/Delete |
| shot trace capture | control loop via `ShotTrace` | two 16 KiB capture buffers and the pre-shot ring are static PSRAM no-init memory (33,408 B) with a fixed, pointer-free layout; boot recovery (setup, before producers) owns them first and adopts an in-flight capture only after a reset that keeps PSRAM; control opens, discards and completes the window and alone owns the state-change cache and reading statistics; other producers append under the trace's leaf mutex or through relaxed atomics; the completed buffer is immutable until the next completion replaces it; httpd only copies it outside a capture; loopTask never touches flash for it |
| shot trace flash pool | settings_persist worker via `ShotTracePool` (`persistence/OpenBrewByWeightShotTracePersistence.inc`) | recovery and the curve cache load on the first idle pass, appends of one record per stored shot (its curve and its completed trace; one flash operation per step, only while the durable write gate is open and no trace capture is open) and clear-all clears run on that worker only, never on loopTask; the worker takes `shotStoreMutex` only while not holding the writer mutex, stages each curve in a 5,608-byte static PSRAM buffer, loads the cache back eight summaries per idle pass, and backs a failed recovery or clear off from 1 s up to 10 min; factory reset clears and verifies it on network_manager under the maintenance lease, serialized by `shotTracePoolWriterMutex`, and leaves the clear requested for the worker when the pool cannot be cleared (never failing the reset); the pool object, writer mutex and save state are internal statics; the 16,552-byte index of the newest 2,048 records (drop-oldest when full) lives in static PSRAM, its leaf mutex guards only index updates and lookups and is never held across flash I/O or taken by control; readers (httpd) read flash under the shared flash lock, holding no store lock, and revalidate the record after every chunk; statistics read RAM only; the `reserved` and `shotcurve` partitions are found by label and exact size, the ring covers both from boot, and shotcurve sectors 0–1 (the retired legacy curve store's epoch) are never written |
| USB application output | core-0 `serial_log` task | the eight-record ESP-log queue stays internal for cache-off logging; the bounded 2.5 KiB CLI reply lives in PSRAM and transfers without copying or waiting |
| cJSON document | parsing caller | PSRAM allocations through process-wide hooks installed once before BLE workers and HTTP start; `cJSON_Delete` releases each independent document |

Micra cancellation stays latched throughout the active cloud operation, including
session renewal. Network recovery and session cleanup cannot clear it. Only the
idle cloud worker consumes it before selecting the next operation; once observed,
cancellation ends HTTP progress immediately, including an `EAGAIN` result. Client
cleanup remains on that worker after HTTP progress returns. Connected-scale
shots and scale acquisition/setup/quiet keep the WS subscription; the client,
TLS state, scratch and session tokens stay allocated. API selection, observation disablement, account
removal/change, STA loss/AP entry and maintenance release WS scratch after
callback quiescence. Maintenance also releases the HTTP workspace. Each
client records actual pause completion; residual SDK DNS resolution is excluded.
Control owns the admitted-shot signal and clears it immediately on rinse
classification. The scale worker owns live connection publication; Micra
transport pauses require both signals, independently of relay-critical
webhook/NTP admission. Unscaled shots, wake, backflush and rejected starts do
not cancel Micra observation. Scale acquisition does not cancel existing WSS.
The integration owns the fixed eight-transition backflush handoff under its
mutex; control is its only consumer. Diagnostics copies without draining it.
The relay owner retains both deadlines and the original close timestamp;
neither the network worker nor an SDK callback can actuate GPIO.

`initJsonParser()` installs the cJSON allocator once, before concurrent users
start. No caller may replace the process-wide hooks afterward. This is not a
resettable arena: simultaneous documents never share storage, and allocation
failure rejects the current parse without invalidating another document.
External allocations fail closed; they never fall back to internal RAM.
UI WebSocket control frames have a 640-byte wire limit, held on the httpd
task stack for one request and sized for a bind that carries the page's usual
subscriptions or for a Stats subscribe with the largest list filter, and use
this same parser's depth and value limits. Micra HTTP RX/TX buffers are 4096 bytes each.
The SDK's ordinary malloc prefers PSRAM above the 2048-byte threshold, but can
fall back to internal RAM; this differs from the application's fail-closed
external allocator and the Micra mbedTLS allocator.

The remaining raw task and server handles are deliberate lifecycle tokens, not
unowned allocations. Converting a task handle to a destructor that invokes
`vTaskDelete` would violate the join rule and may free state while code is still
executing. The HTTP server similarly owns internal LwIP callbacks and must be
stopped on the manager task before its token is cleared.

The claimed Web UI shares one session-owned socket across Home, Stats, History,
and Diagnostic. Only the active view subscribes to records, diagnostic sections,
and logs; shared Home state keeps navigation and command readback current.
Diagnostic readiness includes its base, sampled metrics, and both profiler
sections. Record request IDs distinguish standing windows, scroll fetches, and
exports; Stats continuations also retain their store epoch and sequence.
Stats finishes a standing page before serving a queued fetch; a new subscription
or explicit resync cancels earlier fetches when that operation is accepted.
Stats captures records, aggregate and epoch under one store lock into the
existing external workspace (curves are fetched per row, the older ones from
flash with no store lock held). Continuations reuse that immutable capture
and the sorted page/header/fingerprint until its epoch or window/order changes.
Sessions retain their own request identity, sequence and cursor. Debug export
invalidates the cache before borrowing the record array. A capture that sees a
newer epoch than the initial probe starts a new page sequence. Store locks are
released before sorting, hashing, JSON formatting or sending.
Other API requests remain exclusive, including commands. Seven HTTP sockets
and a four-connection backlog cover the claimed stream and a takeover
handshake, lazy HTML and JavaScript or Stats reads, a Home Assistant request,
and an OTA range plus its status poll. `CONFIG_LWIP_MAX_SOCKETS=18` covers
httpd's `max_open_sockets + 3`, mDNS, SNTP, DNS, the webhook, Micra WSS and
HTTPS, and two spare sockets; slots are allocated per open socket.
Handlers execute serially on the existing HTTP task, retaining one owner of
the shared response workspace. The one exception is OTA work: session
creation, range writes, commit and discard move to the transient
`ota_worker` task (httpd-sized internal stack, core 0, idle+1, not watchdog
subscribed, like httpd) through ESP-IDF async requests, queued in FIFO order
up to four. The worker borrows the response workspace only to answer, under
the same mutex. If the queue, the async copy or the task cannot be allocated,
the request runs on httpd as before and the Web UI log says so. A handler
that holds httpd for 500 ms or more, or a request answered `STATUS_BUSY`, is
reported in the Web UI log with its path only.

Host evidence consists of allocation/task fault injection already in the main
harness, OTA concurrent TSAN, ASan/UBSan tests and
`resource_owner_host_test.cpp`, which checks move, release, replacement,
idempotent reset and scope rollback. Target resource counts remain part of the
combined soak gate.

The webhook worker keeps a dequeued event pending while the configured shot
gate is closed; stop-after-drain includes that locally held event. The worker
reads its queue and state under the state mutex and takes the lifecycle mutex
only to end a disabled test drain, so a control enqueue never fails on worker
contention. An empty 50 ms queue wait is followed by the drain check without
an additional 25 ms delay. The 25 ms gated wait and the gate check before starting delivery remain;
delivery checks admission between asynchronous progress calls and after the
final call. Each call blocks at most 25 ms, except before the request headers
are sent: ESP-IDF 6.1 reports a plain-HTTP asynchronous connect as established
while the TCP handshake is still pending, so the header write waits up to
500 ms for it (longer under Wi-Fi modem sleep) and returns as soon as the
socket connects; the `HEADERS_SENT` event restores the 25 ms interval. The
absolute 1800 ms deadline also rejects a late successful return.
Unsent canceled events remain pending without consuming an attempt. Any other
failure, including an interrupted possibly dispatched POST, is resent after
3, 6, and 9 s while it holds the queue head (receivers deduplicate identical
payloads); `test` events, invalid requests, and HTTP 4xx other than 408/429
are dropped at once. A retrying full-state head (brew state, presets, Quick
Settings, IP) is discarded without counting a drop when a newer event of the
same kind is queued; per-kind counters owned by `mux_` track that without
scanning the queue. `webhook_worker_host_test.cpp`
executes this worker with deterministic queue, transport and heap injection,
including stale configuration and an active shot beginning during delivery.
These tests establish ordering and accounting, not target CPU or RF latency.

# Integration API

This is the public LAN contract used by the Open Brew by Weight Home
Assistant integration. It is available in firmware `0.1.0` and later at
`http://<controller>/api/v1/integration`. Like the Web UI API, it is open by
default and must be used only on a trusted local network; while the optional
[Web UI login](settings/web-access.md) is on, it requires a token (see
[Access](#access)). It observes
brewing and manages the single webhook destination, active preset, Home Quick
Settings, and a safe queued controller restart. It cannot start, stop, rinse,
or directly actuate the machine.

## Common rules

- Requests and responses use `application/json`; request bodies are limited to
  2,048 bytes and response collections to `MAX_SHOT_PRESETS` items.
- Success responses contain `apiVersion: 1`, except the integration snapshot,
  which uses `apiVersion: 2` and `minimumClientApiVersion: 2` for its unified
  `lastShot` contract. Errors use
  `{"error":"STABLE_CODE","message":"English diagnostic"}`.
- While the Web UI login is off, every route is open on the trusted LAN and
  needs no token. That includes reading and changing the webhook destination
  (with its Home Assistant webhook ID) and restarting the controller, so turn
  the login on when other devices on the network are not trusted. While it is
  on, every route except the [access routes](#access) needs
  `Authorization: Bearer <token>`, and any signed-in Web UI device can
  approve a pairing request.
- Responses carry no cross-origin (CORS) headers, so a web page served from
  another site cannot read the API through a visitor's browser. Call it from a
  LAN client such as Home Assistant, a script, or the controller's own Web UI.
- Every route, like the Web UI, answers only when the request's `Host` names
  the controller: an IP address (IPv4 or bracketed IPv6), its mDNS name
  `<name>.local` (including a `<name>-2.local` conflict rename), the bare name,
  or the name under `.lan`, `.home.arpa` or `.internal`, with or without a
  port. Any other host name gets `403 HOST_NOT_ALLOWED`, which keeps a website
  from reaching the controller through DNS rebinding. Point clients at the IP
  address or the `.local` name, and make a reverse proxy forward one of them.
- Durable mutations return `202` and a `requestId`; this acknowledges only that
  the existing control queue accepted the operation. Poll the request-status
  route until it reports `PERSISTED`. A mutation is not successful before that
  state. Clients time out after 10 seconds and may retry idempotent operations.
- Restart is the exception: its accepted response is final because the
  controller intentionally becomes unreachable. Do not poll its request ID.
- Webhook, preset, and Quick Settings mutations retain the firmware's existing
  safe-state gate and are rejected while a shot cycle is active. Restart is
  accepted during a shot but the existing control owner defers it until idle.

## Status and error codes

| HTTP | Code | Meaning |
| --- | --- | --- |
| 400 | `INVALID_REQUEST` | Malformed JSON, field, identifier, or bound |
| 401 | `INTEGRATION_AUTH_REQUIRED` | The login is on and the bearer token is missing or no longer valid |
| 401 | `DEVICE_PASSWORD_INVALID` | Wrong device password in an exchange |
| 403 | `HOST_NOT_ALLOWED` | The `Host` header does not name this controller |
| 409 | `AUTH_NOT_REQUIRED` | Exchange or pairing asked for while the login is off |
| 429 | `AUTH_COOLDOWN` | Five wrong device passwords in a row from this client; retry after 30 s |
| 429 | `PAIRING_BUSY` | Another pairing request is open or was just decided; retry later (at most 2.5 min) |
| 404 | `PRESET_NOT_FOUND` | Stable preset ID does not exist |
| 409 | `CONFIG_LOCKED_DURING_ACTIVE_CYCLE` | Existing firmware safety gate is closed |
| 409 | `STALE_REVISION` | Quick Settings changed since the caller's snapshot |
| 409 | `REQUEST_BUSY` | Another persistent request owns the queue/staging slot |
| 413 | `CONTENT_TOO_LARGE` | Body exceeds the fixed request buffer |
| 500 | `PERSISTENCE_FAILED` | The durable write failed |
| 503 | `CONTROL_QUEUE_FULL` | The bounded control queue could not accept the command |
| 503 | `WEBHOOK_UNAVAILABLE` | Test event could not be queued or delivered |

## Access

These routes always answer without a token. They let a client discover whether
the controller asks for one and obtain it without the user copying anything.

### `GET /api/v1/integration/info`

```json
{"apiVersion": 1, "name": "openbrewbyweight", "firmwareVersion": "0.1.0", "authRequired": true}
```

`name` is the controller's device name. `authRequired` is `true` only while the
Web UI login is on.

### `POST /api/v1/integration/auth`

Exchanges the device password, once, for the integration token:
`{"password": "<device password>"}` returns `{"apiVersion": 1, "token": "<32
lowercase hex digits>"}`. The client must not keep the password. Wrong
passwords share the Web UI's failure counter and cooldown.

### `POST /api/v1/integration/pair` and `GET /api/v1/integration/pair?id=<id>`

Asks the person at the Web UI to approve the client instead. `POST` with `{}`
returns `{"apiVersion": 1, "id": 3735928559, "code": "4821", "expiresInS":
120}`; show `code` to the user, who sees the same four digits in the Web UI and
taps **Allow** or **Deny**. Poll `GET ?id=<id>` about once a second; `state` is
`pending`, `approved`, `denied`, `expired`, or `unknown`. `approved` carries
`token` exactly once. Only one request exists at a time: an open request
blocks new ones for up to 2 minutes, and a denied or expired one (or an
approved one whose token was not fetched) for 30 s more.

### Token rules

- One integration token exists at a time; a new exchange or approval replaces
  the previous one.
- It survives restarts and device-password changes. It ends when the user taps
  **Disconnect** in Admin → Access, turns the login off, or restores the factory
  password.
- Any `401 INTEGRATION_AUTH_REQUIRED` means "ask the user again": start a
  reauthentication with either method above.

## Routes

### `GET /api/v1/integration`

Returns identity, compatibility, current shot state, the newest recorded shot,
preset revision, and the complete Quick Settings snapshot.

```json
{
  "apiVersion": 2,
  "minimumClientApiVersion": 2,
  "deviceId": "AA:BB:CC:DD:EE:FF",
  "wifiMac": "AA:BB:CC:DD:EE:FF",
  "bluetoothMac": "AA:BB:CC:DD:EE:10",
  "manufacturer": "Open Brew by Weight",
  "model": "Open Brew by Weight",
  "firmwareVersion": "0.1.0",
  "capabilities": ["webhook_v1", "preset_select_v1", "quick_settings_v1", "restart_v1", "stored_shots_v1", "preset_rules_v1", "last_shot_curve_v1"],
  "shotState": "idle",
  "bootId": 123,
  "activePresetId": 2,
  "presetRevision": 19,
  "quickSettings": {
    "revision": 19,
    "activePresetId": 2,
    "brewByWeight": true,
    "noScaleBbwMode": "warn_once",
    "autoToManualGuardEnabled": true,
    "slowExtractionGuardEnabled": true,
    "fastExtractionGuardEnabled": true,
    "avoidAccidentalTouchEnabled": true,
    "cupProtectionEnabled": true
  },
  "lastShot": null,
  "lastActivation": null,
  "stats": {"window": 10, "shotCount": 0, "totalDurationS": 0, "avgDurationS": null, "avgYieldG": null, "avgErrorPct": null, "avgFlowGps": null, "shotsPerDay": null, "durationsS": []}
}
```

`lastShot` is nullable and describes the newest good shot in shot history: a
confirmed non-rinse cycle longer than 12 seconds with a valid final yield over
2 g. Shot history also stores the other cycles started with a scale as bad
shots, which no integration field reports; other completed activations appear
only in `lastActivation`. A bad cycle never changes `lastShot`: its cycle
fields (`cycleId`, `stopDetail` and the like) come from the last cycle when
that cycle is this shot, otherwise from the last good shot, exactly as Home
shows it.
When present, `lastShot` contains
`cycleId`, `shotId` (the shot's id in shot history, the `id` column of
[the last shot route](#get-apiv1integrationshotslast)), `uptimeMs`, `durationMs`, `targetWeightG`, `presetId`, `presetName`,
`shotType`, `stopDetail`, `savePending`, and the optional `firstDropMs`, `weightG`,
`averageFlowGps`, and `rating` fields defined by the webhook contract. When the
controller has a valid clock at shot completion, it also includes
`endedAtUnixSec` (UTC) and `endedAtLocalSec`; otherwise both are absent.
The local value uses the saved time zone's offset at shot completion and stays
fixed if the zone changes later. Consumers should use `endedAtUnixSec` for an
unambiguous instant.
`savePending` is true while the deferred flash write still needs confirmation;
if saving fails, the record remains in RAM for a retry. Firmware with the
earlier v1 snapshot instead had distinct `lastShot` and `lastGoodShot` fields.
Clients that require that older meaning should check `apiVersion` before using
the new field.

`lastActivation` mirrors the newest activation-history record (the Web UI
History page). It is null before the first activation after a full data reset
and otherwise carries `id`, `type` (`shot`, `rinse`, `other`, `backflush`, `power_on`, or
`no_scale_guard_aborted`), `durationS`, `hasWallTime`, `endedAtUnixSec`, and
`endedAtLocalSec`.

`backflush` identifies a confirmed automatic-cleaning activation, including an
interrupted one. Duration measures electrical activation, not certified program
completion. It does not update shot statistics. The additive type preserves
the existing history record layout and schema.

`stats` carries the rolling aggregate the Web UI Stats page shows by default:
`window`, `shotCount`, `totalDurationS`, `avgDurationS`, `avgYieldG`,
`avgErrorPct`, `avgFlowGps`, `shotsPerDay`, and `durationsS` for the duration
chart. `window` is always `10`: the integration aggregate covers the newest ten
good shots, including manual and timer-only endings, whichever
window the Web UI shows. `avgErrorPct` is the mean absolute percentage miss of
the newest ten weight cuts (normal, raised or lowered cut line), measured
against the cut line at the stop rather than the target. Unavailable averages are `null`.
The controller derives these values from RAM; the Web UI streams the same
object over its data WebSocket, and this payload carries it under `stats`,
alongside `savePending`.

`shotState` is `idle` or `brewing`. `wifiMac` and `bluetoothMac` repeat the
station interface addresses as upper-case `AA:BB:CC:DD:EE:FF` strings; the
Bluetooth address uses the controller's Bluetooth MAC base. Receivers may
surface them as device connections but must keep using `deviceId` as the
stable identity.

### `GET /api/v1/integration/request`

Returns the most recent durable command as
`{"apiVersion":1,"requestId":17,"state":"PERSISTED"}`. Compare its ID with
the mutation response. Valid terminal states are `PERSISTED`, `FAILED`, and
`CANCELED`.

### `GET /api/v1/integration/webhook`

Returns `enabled`, `url`, `brewState`, `firstDrop`, `end`, `presetChanges`, and
`deferDuringShot`. When that flag is true, queued requests wait while a shot is
active; a request that already started is never cancelled.

The read-only `delivery` object reports the outbound worker without Admin
access:

| Field | Meaning |
| --- | --- |
| `workerReady`, `sending` | The delivery task runs; a request is in flight |
| `held`, `queued` | Outbound admission currently withholds events (shot deferral or scale acquisition); events waiting in the queue |
| `lastEvent`, `lastAttemptAgoMs` | Newest attempted event and its age, or `null` before the first attempt |
| `lastSuccess`, `lastHttpStatus`, `lastError` | Outcome, receiver HTTP status (0 when none arrived), and ESP-IDF error code |
| `lastPhase`, `lastCancellation` | Request phase reached (`prepare` … `response`) and why it was cancelled, if it was |
| `sent`, `dropped`, `staleConfigDropped`, `workerStartFailures` | Counters since boot |

Each failed delivery is also logged at Error level in the network category of
the Diagnostic log ring.

### `PUT /api/v1/integration/webhook`

Requires all webhook fields. The URL must be a bounded `http://` URL without
embedded credentials. Repeating the same request is successful and does not
create another destination.

### `POST /api/v1/integration/webhook/test`

Accepts `{"correlationId":"<1-64 URL-safe>"}` and queues the matching `test`
event. The event echoes `correlationId` so Home Assistant can prove that its
newly registered receiver is reachable.

### `GET /api/v1/integration/presets`

Returns a bounded snapshot. With the `preset_rules_v1` capability each item
is the same preset render the Web UI draws its preset chart from (shortened
here to one item):

```json
{"apiVersion":1,"revision":19,"activeId":1,"items":[
  {"id":1,"name":"Double · Adaptive","isFactory":true,"brewByWeight":true,
   "doseG":18.0,"goalWeightG":36.0,"fastBeforeS":27.0,"slowAfterS":36.0,
   "fastMarginG":6.0,"slowMarginG":3.0,"operationalWallMs":50000,
   "bbwProtectionMs":12000,"fastExtractionGuardEnabled":true,
   "slowExtractionGuardEnabled":true,"grindHintsEnabled":true,
   "lineaMicraBrewTargetC":93.0,"fastRampS":5.0,"slowRampS":5.0,
   "fastCurve":"even","slowCurve":"even","fastCurveHardness":"soft",
   "slowCurveHardness":"soft",
   "shotStats":{"n":5,"total":5,"minS":21.4,"maxS":34.2,"medianS":30.6},
   "firstDropS":4.9,"firstDropN":5,"arrivalsS":[21.4,29.1,30.6,32.7,34.2],
   "grindHint":null}
]}
```

`id`, `name`, and `isFactory` are the original contract; the rest is
additive. Each side eases between the target and its extended or shortened
cut line over its own transition: `fastRampS` before the window and
`slowRampS` after it (0–10 s; 0 is a step), along a curve named by
`fastCurve` / `slowCurve` (`early`, `even` or `late`: where the change is
steepest) and `fastCurveHardness` / `slowCurveHardness` (`soft`, `medium` or
`hard`). The curves are the regularized incomplete beta I(u; a, b) of the
firmware's `RAMP_CURVES`, and a reader that draws the line must use the same
exact integer law to match the stops. Earlier firmware sent one top-level
`cutRampS` for both sides instead. The rule fields mirror the preset
editor: the on-recipe window runs from `fastBeforeS` to `slowAfterS`, the
cut line is raised by `fastMarginG` before it and lowered by `slowMarginG`
after it when the matching guard is enabled, nothing is cut before
`bbwProtectionMs`, and the shot ends by `operationalWallMs`. `shotStats` and
`arrivalsS` describe the preset's newest timed shots (arrival times at the
target, oldest first), `firstDropS` is their median first drop (`firstDropN`
shots; `0` falls back to other presets), and `grindHint` is the Web UI grind
hint object or `null`. `lineaMicraBrewTargetC` only matters on Linea Micra
builds. The statistics follow every stored good shot (bad shots never count),
so Home Assistant's read after the `end` webhook already returns the new
arrival.

Preset IDs are stable while an item exists and names are unique. A fresh or
factory-reset controller has the five factory presets (ids 1–4 and 255),
which can be restored but not deleted. User presets never receive a factory id.
Manual (id 255) has brew by weight off. A controller updated from earlier
firmware appends it on restart when a slot is free and no preset is named
"Manual".

### `GET /api/v1/integration/shots/last`

Available with the `last_shot_curve_v1` capability. Returns the shot that
`lastShot` in the snapshot describes as one row of the Web UI Stats page,
with its weight curve:

```json
{"apiVersion":1,"cols":"id,bootId,endedAtMs,…,actualWeightSource,rating,badReason",
 "shot":[1042,30,930100,true,…,"post_drip",4,null,
   {"wD":"AAAA…","wBreakBefore":[],"wHeld":[],"wTruncated":false,
    "dropS":5.200,"dropCg":20,"extendedS":null,"extCg":null,"atmS":null,
    "atmCg":null,"atmClearedS":null,"endS":31.000,"endCg":3632}]}
```

`shot` is `null` before the first qualifying shot. When the shot's curve
cannot be read right now, the route answers `503 SHOT_CURVE_BUSY` instead of
the shot without its curve: right after a boot, until the controller has
loaded the newest curves back from storage, and, rarely, during a shot or its
drip delay when the newest good shot is older than the curves kept in memory.
Keep the copy you hold and read it again on your next refresh (the Home
Assistant integration does this). `cols` names the row's
values in order and the last element is the curve object; read values by
column name, because later firmware may add columns. The columns and curve
keys are the ones the Stats page and the [shot JSON export](schemas/README.md)
use: for example `goalG`, `actualG`, `durationS`, `firstDropS`, `doseG`,
`ratio`, `targetTempDeciC`, `cutLineG`, `arrivalS`, `verdict`, `stopDetail`,
`scaleName`, and the rule the shot ran with (`fastBeforeS`, `slowAfterS`,
`fastMarginG`, `slowMarginG`, `delayS`, `wallS`, `fastRampS`, `slowRampS`,
`fastCurve`, `slowCurve`, `fastCurveHardness`, `slowCurveHardness`).
`badReason` is `short`, `no_yield` or `low_yield` for a bad shot and `null`
for a good one; this route always returns a good shot.

`wD` is the curve as base64 point records: each point is a time step in
milliseconds (one byte, or `255` followed by the absolute time as a
little-endian `uint16`) and a weight step in centigrams (one signed byte, or
`-128` followed by the absolute weight as a little-endian `int16`), starting
from 0 ms and 0 cg. `wBreakBefore` lists points that start a new segment,
`wHeld` points the scale held, and the scalars mark the first drop, a
fast-shot extension, the scale-loss takeover, and the end of the shot.

The newest 500 curves are kept in memory, so this route's curve is normally
there at once; a shot without a stored curve comes with an empty one. The row
is a few kilobytes, so read it once per shot: after the `end` webhook, and only when the snapshot's `lastShot.shotId` differs from the
`id` of the shot you already hold.

### `PUT /api/v1/integration/presets/active`

Requires `{"id":2}`. The `202` response identifies the existing persistent
command. After it reaches `PERSISTED`, read the preset snapshot for the
authoritative active value. A failed or timed-out write never reports the
candidate as confirmed.

### `PUT /api/v1/integration/quick-settings`

Requires `baseRevision` plus one or more settings. Boolean settings are
`brewByWeight`, `autoToManualGuardEnabled`, `slowExtractionGuardEnabled`,
`fastExtractionGuardEnabled`, `avoidAccidentalTouchEnabled`, and
`cupProtectionEnabled`; `noScaleBbwMode` accepts `off`, `warn_once`, or
`require_scale`. Settings sent together are one change: they are validated
together and go live all at once or not at all, with one revision, one
`quick_settings_changed` webhook, and one restart of the revert timer. Unknown,
duplicate, or incorrectly typed fields are rejected. Instead of settings, the
request may carry only `revertAfter` (`next_shot`, `15m`, `30m`, `60m`, or
`keep`) to choose how long overrides last, or only `revertNow: true` to drop
every override at once. A request with a single setting, as earlier clients
send, keeps working unchanged.

The Web UI header panel sends one such request per pause: switches flip at
once on screen, and about 0.3 s after the last tap only the switches that end
up different from the controller's values travel, at most one request every
second. A switch turned back before then sends nothing.

These are the same session-only overrides as the Web UI header panel. A change
applies to the next shot immediately and is never written to flash, a preset,
or the shared settings: it lasts until the active preset changes or the
controller restarts, which restores the saved values. `revertAfter` can end it
sooner: after the next shot that ends (rinses and abandoned starts excluded),
or 15, 30, or 60 minutes after the last override change, each change restarting
one shared timer. The controller performs the revert itself, waits for an
active cycle to end and the relay to open, and then emits
`quick_settings_changed`. The policy is RAM-only and returns to `keep` at boot. `brewByWeight=false`
stops shots manually for now without changing the preset. Snapshots and
webhooks report the effective values, overrides included, so the Web UI and
every integration show the same state. Overrides are refused during an active
cycle. A later Settings save that changes the same shared setting, or an edit
of the active preset's Brew by weight, fast or slow choice, replaces the
override with the saved value.

In the Web UI, which is separate from this API, the Home stream marks the
overrides as `quickRevert.overrides`, a bit mask in field order: `brewByWeight`
(1), `noScaleBbwMode` (2), `autoToManualGuardEnabled` (4),
`slowExtractionGuardEnabled` (8), `fastExtractionGuardEnabled` (16),
`avoidAccidentalTouchEnabled` (32), and `cupProtectionEnabled` (64); 0 means no
override is active. The Settings stream adds `quickRevert.saved`, the saved value
each active override hides, keyed by the same field names, so the Settings form
shows saved values while Home shows the effective ones.

The command completes as soon as it is live, so its `lastCommand` reaches
`PERSISTED` without a flash write; an unchanged value completes immediately.
Authoritative-refresh rules are otherwise identical to active-preset selection.

### `POST /api/v1/integration/restart`

Requires the exact empty object `{}`. A `202` response means the existing safe
restart path accepted the request. During an extraction it waits for the cycle
to end; it never closes the relay or resumes a cycle after boot. This trusted-
LAN route exposes no machine actuation counterpart.

### `POST /api/v1/diagnostic/audit/empty`

Erases the durable audit log and the RAM latch. Requires the Admin unlock (or
a development build) and the exact body `{"confirm":"EMPTY_AUDIT_LOG"}` while
the machine is Ready; responds `200` with `{}` on success. Mirrors
`crashes/empty` semantics (gate, confirmation, single flash owner).

### `POST /api/v1/diagnostic/audit/verbose`

Arms or stops a 30-minute verbose audit hunt session with
`{"enabled":true|false}`. While armed, marked debug events join the audit log
in addition to the always-audited errors and allowlisted warnings. Requires a
synchronized wall clock; without one it responds `409 AUDIT_VERBOSE_UNAVAILABLE`.
The session is RAM-scoped (a reboot ends it) and never affects control.

### `GET /api/v1/diagnostic/audit`

Downloads the whole retained audit log (durable window plus RAM latch, oldest
first) as a plain-text attachment (`openbrewbyweight-audit.txt`): a small header
with the store state, then one tab-separated line per record — wall time in
UTC (or `unset` when the record predates clock synchronization), level,
category, event, and the full detail text. Requires the Admin unlock (or a
development build), like the other audit routes.

The diagnostic stream carries the audit surface on its own fingerprinted
frame under `changes.audit`: `state` (`ready` / `unsupported` / `io_error`),
`retained`, `dropped`, `rateLimited`, `verboseUntilUtcSec`, and `items` with
the newest records (`sequence`, `wallSec`, `level`, `category`, `code`,
`text`); the debug export's records also carry `atMs`, `argument1` and
`argument2`. In both this frame and the debug export, `text` is the full
human-readable detail, rebuilt from the record's
stored arguments and excerpt — for example
`health heap low free=31000 largest=12000 (limits 49152/16384 bytes)`. The
debug export's `audit` section carries the same records for the whole
retained window.

### `POST /api/v1/diagnostic/scale-profile`

Starts, stops, or deletes the manual scale profile capture. Requires the
exclusive Web UI claim like every owned API route. The body is
`{"action":"start"}` or `{"action":"stop"}` or `{"action":"delete"}`; replies
`202 Accepted` with a request id that follows the common acknowledgement
rules. Bounded state (including `state`, `persistence`, counts, `stopReason`,
`lostCount`, and `canStart`/`canStop`/`canDelete`/`canDownload`) is reported
as `scaleProfile` in the diagnostic status document.

Capture has no duration cutoff: it stops on explicit request or when all
ordinary record slots are occupied, preserving one slot for the closing event.
The existing numeric `durationLimitMs` field is deprecated and is always `0`
(no limit). `elapsedMs` reports elapsed capture time independently of capacity.

| Status field | Meaning |
| --- | --- |
| `recordCount` / `recordCapacity` | Accepted records in the current or saved capture / total payload slots (8,192). Empty/preparing states expose zero current records. |
| `recordBytes` | Fixed stored record size, 32 bytes; downloaded text size is unrelated. |
| `reservedRecords` | One terminal slot while recording, zero otherwise. Capacity used is `(recordCount + reservedRecords) / recordCapacity`. |
| `estimatedRemainingMs` | Smoothed estimate until capture capacity fills, or JSON `null` until enough data arrives, after a sustained stall, or outside recording. Full captures report zero. It never acts as a deadline. |

The estimate is maintained by the firmware independently of status polling. It
uses all accepted records, samples growth about once per second, and smooths
the rate with a five-second exponential time constant. Initialization requires
at least five seconds and ten records after the initial state snapshot; five
seconds without progress invalidates it. A new session resets the estimate.
`persistence` remains the authority for pending/saving/saved/failed state:
recorded bytes are not claimed as durable before the save completes.

### `GET /api/v1/diagnostic/scale-profile/download`

Streams the most recent completed capture as a chunked `text/plain`
attachment (`Content-Disposition: attachment`,
`Cache-Control: no-store`). Disabled while a capture is recording; a trace
that stopped but has not finished saving downloads from memory and is labeled
accordingly. Concurrent Start/Delete are refused while the download streams.

The trace retains the 32-byte record layout and schema version 1; new event IDs
are appended, preserving the meaning of saved records. `STATE_CHANGED` rows
contain a named scalar signal, `from`, `to`, originating `capture` sequence,
and connection generation. `from=initial` means the first control-owner
observation in this capture, not a transition from an assumed default.
Control observations are contiguous batches; their timestamps are observation
times, independently of the initial header snapshot. No initial observations
means control was not observed before capture stopped. Unchanged signals are
omitted; a new connection generation renews the observations.

New `PROFILE_START` rows state `limit=capacity`; historical `maxMs` and timeout
overrun rows remain readable. Durations and exported relative times can exceed
the 32-bit millisecond rollover: ordinal bits 0–13 retain append order and bits
14–31 contain the high word of the record time. Header `reserved[0]` contains
the high word of `durationMs`. Legacy records and headers have zero extension
bits; their interpretation is unchanged. Readers that decode binary records
must combine these words before sorting or displaying long captures. Old
firmware retains its original 32-bit time display for such long captures.

Signals cover cup presence, settling and removal qualification, reference/mass
validity, tare eligibility/presentation/reasons, request and placement IDs,
accessory settling, stream and weight control, retare/baseline waiting, first
flow/touch phases, brew protection, scale-loss guard, and pending finalization.
`IDLE_STATUS` uses the same decision rule as Web status, with readable
`waiting_for_settle` and `ready_for_cup` names for Web `empty` and `ready`.
Other booleans use 0/1. `*_CG` values are signed centigrams (unknown references
are rendered `unknown`); `*_MS` values are milliseconds. Effective tare/cup
thresholds and timing settings are emitted initially and when they change.
Raw `WEIGHT` rows retain the received floating-point weight precision.

`TARE_PHASE` preserves queued/writing/succeeded/failed command transitions even
when they occur between control observations. Its ID belongs to the idle-tare
request namespace; `boundary` is a capture sequence. `TARE_REJECTED` identifies
the placement in `context`; a placement while idle tare is not eligible is
rejected with its eligibility reason (for example `maintenance`).
`CUP_QUALIFICATION_RESET` uses context 0 for empty
pan, 1 for cup placement, and 2 for accessory settling. Their `pkt` is the
control packet sequence. `CUP_ANCHOR_REJECTED` marks one steady empty-pan
plateau rejected as off the empty reference, with its `placement` and signed
`offset_cg`. `CUP_RESET` is a logical reset, not proof of physical
removal. `FINALIZE_CANCELLED` distinguishes cup continuity, a new cycle, and
rinse. Existing shot-end reasons continue to describe guard stop outcomes.

For correlation, follow `IDLE_REQUEST_ID`/`IDLE_PLACEMENT_ID`/`IDLE_ORIGIN` for
idle tare, and `SHOT_TARE_REQUEST_ID`/`RETARE_REQUEST_ID`/`CYCLE_ID` for shot
tare. Do not equate idle request IDs with shot request IDs, or packet sequences
with capture sequences. A zero sequence means no specific sample is identified
(for example, a timer-only observation). Transport success does not establish
physical zero; baseline confirmation and timeout remain separate events.
For existing `TARE_REQUEST` rows, `seq` is the qualifying control packet for
idle/accessory tare and zero for shot/manual requests. A return from writing
to queued in `TARE_PHASE` means the worker deferred the write; it is not success.
Internal per-sample counters, UI rendering, unrelated network state, and
unreported physical scale-button actions are outside this trace's scope.

The capture still reserves a terminal record and reports overflow/loss. Its
per-signal deduplication cache is bounded PSRAM, reset for every capture;
no new partition, endpoint, or control owner is introduced. Older captures
remain readable; unknown event IDs render their numeric payload safely.

### `GET /api/v1/diagnostic/shot-trace`

Downloads one [shot trace](SHOT_TRACE.md) as chunked
`application/octet-stream`. Requires the exclusive Web UI claim like every
owned route. A trace exists for every brew cycle started with a scale; its
header links it to the stored shot by id, boot id and end time, and carries a
CRC-32. Nothing is read or sent while a shot runs.

- **Without a query**: the newest completed trace still in RAM, as the
  attachment `openbrewbyweight-shot-trace.bin` with `Cache-Control: no-store`.
  RAM keeps at most the last two traces until they are saved; after a
  restart it has none until the next shot, except an
  [interrupted trace](SHOT_TRACE.md#interrupted-traces) recovered from the
  crash.
- **`?id=<shot id>&boot=<boot id>&end=<endedAtMs>`**: that shot's trace from
  its [flash pool](SHOT_TRACE.md#flash-pool) record, the trace section only.
  A stored shot is reached only through its summary, so all three must match
  the shot's Stats row (`id`, `bootId`, `endedAtMs`); a deleted shot has
  none. `id=0&boot=<boot>&end=<end>` is a trace without a summary: an
  interrupted one (`end=0`, the boot that crashed) or one whose shot the log
  refused (Diagnostic Shot storage lists the newest 16 by boot and end). The URL names an
  immutable record, so the answer carries `ETag: "<record CRC, 8 hex>"` and
  `Cache-Control: private, max-age=31536000, immutable`; a request with that
  `If-None-Match` gets `304` without the body.

| Status | Code | When |
| --- | --- | --- |
| `200` | — | The trace, 792 B to 16 KiB |
| `304` | — | `If-None-Match` names the record's current ETag |
| `404` | `NO_SHOT_TRACE` | No query: no trace since the last restart (or the recovered one failed verification). With a query: no summary with that identity, the shot has no trace (made without one, or a later shot's capture overwrote it before it was saved), or the pool recycled the record |
| `409` | `SHOT_TRACE_BUSY` | The controller may not read storage now: a shot runs (cycle start to the end of its drip delay), or a rinse, a closed relay, a scale connecting or a maintenance lease holds the durable write gate |
| `409` | `SHOT_TRACE_SAVING` | The shot's record is not written yet (its curve or trace still waits, or the pool is not recovered after the boot); ask again shortly |
| `422` | `INVALID_SHOT_TRACE` | The query is not three whole numbers `id`, `boot` and `end` |
| `503` | `SHOT_TRACE_UNAVAILABLE` | The pool failed to recover, or a flash read failed |

A shot that starts mid-download, or a record the ring recycles while it is
sent, ends the stream without its terminating chunk, so the client sees a
broken download, never other data.

### `GET /api/v1/diagnostic/shot-trace/vocabulary`

The names of a trace's numbers, from the firmware's own tables, so the Web UI
needs no copy of them: `{"version":<vocabulary version>,"profile":[…],
"debug":[…],"signals":[…],"endReasons":[…],"weightSources":[…]}`, each list
indexed by the event kind, debug code, signal, end reason or yield source
(an unused number is `""`).
Names apply to traces whose header has the same `vocabularyVersion`. The list
changes only with the firmware, so the Web UI asks with the firmware version
string as the query (`?fw=1.1.0%2Babc1234`): for a unique build identity
(`x.y.z+sha`) the answer is cacheable (`Cache-Control: private,
max-age=31536000, immutable`); a build with uncommitted changes (`-dirty`) or
without Git (`+unknown`) answers `Cache-Control: no-cache`. About 6 KB.

### `GET /api/v1/diagnostic/shot-storage`

The Diagnostic **Shot storage** section, read on demand when the section opens
(it is not in the polled status). Needs Admin or the public Diagnostic page,
like the other Diagnostic actions. `409 SHOT_STORAGE_BUSY` while the
controller may not read storage, as for `SHOT_TRACE_BUSY`.

```json
{"ready":true,"lastError":"none","poolBytes":5476352,"liveBytes":1212416,
 "reclaimableBytes":20480,"unindexedBytes":61440,"unusedBytes":4182016,
 "records":412,"meanRecordBytes":2992,"droppedRecords":0,"good":380,"bad":30,
 "traceOnly":1,"interrupted":1,"traces":402,"truncated":3,"sinceShotId":7,
 "sinceLocalSec":1792840364,"pendingCurves":0,"pendingTraces":0,
 "tracesDropped":0,"shotCapacity":1000,
 "orphanTraces":[{"boot":43,"end":918204},{"boot":41,"end":0}]}
```

`poolBytes` = `liveBytes` + `reclaimableBytes` + `unindexedBytes` +
`unusedBytes`. A record is live while a summary of its identity exists, or
when it is a trace without one (shot 0); a deleted or evicted shot's record is
reclaimable until the ring recycles it. `good` and `bad` count the live
records of good and bad summaries, `traceOnly` the traces of shots the log
refused and `interrupted` the interrupted ones. `orphanTraces` lists these
traces without a shot, newest first, at most 16, by `boot` and `end` (`end` 0
is interrupted); each is reached through the trace route with `id=0`. `traces` counts the
live records holding a trace and `truncated` those whose events did not all
fit. Records written or recycled while the section is read can shift a count
by one.
`sinceShotId` and `sinceLocalSec` (local Unix seconds, `0` without wall time)
are the oldest stored shot that still has its record. `pendingCurves` and
`pendingTraces` (0 to 2) wait for the worker, `tracesDropped` counts traces a
later shot's capture overwrote before they were saved since the restart, `droppedRecords`
those the full index dropped since the last recovery, and `lastError` is
`none`, `read`, `erase`, `program`, `lock`, `too_large` or `source`.
`shotCapacity` is how many summaries the history keeps (an upper bound for
how many shots the pool can serve, whatever their size). `ready` is `false`
until the pool is recovered after the boot.

## Web UI record API

Neither record page has a REST read: Stats and History both subscribe over
the owned WebSocket (see [History live updates](#history-live-updates) and
[Stats live updates](#stats-live-updates)). Both surfaces require the active
`X-WebUI-Client` obtained from `POST /api/v1/ui/claim`. They are separate from
the public integration routes described above. Mutations stay confirmed REST
commands: `POST /api/v1/stats/clear|delete|rate`,
`POST /api/v1/history/clear|delete`.

Stats and History frames both carry `boot`, `total`, `offset`, `limit`, and
`hasMore`. Stats frames also
carry `stats` (the firmware-computed aggregate, without the integration
payload's `window`, `totalDurationS` and `durationsS`, which the page does not
show) and `rows`. The first frame
of a page names the row fields once in `cols`, a comma-separated list; each
row is then an array of those values in that order, followed by the row's
curve object. The browser rebuilds the row objects (the same fields the table
and CSV export use) before validating them; pages larger than the frame
budget continue across frames with `rowBase` and `more` until a terminating
frame, and continuation frames reuse the page's `cols`. History frames carry
`records`. Paging and sorting parameters are unchanged: `offset`, `limit`, and `dir` (`asc` or `desc`), plus
`sort` (`date` or `rating`) for Stats. The page sizes used by the browser are
ten shots and twenty activations. Both lists filter in the firmware (History
`types`, Stats `filter`, below): `total`, `offset`, `hasMore`, and every page
count the matching records only, so each page is full of matches in the
selected order.

The browser derives each shot's error from its displayed yield and goal;
WS rows omit `errorG` and `errorPct`. Per-shot `bootId` remains part of the
row identity. History records carry `hasWallTime` and both timestamps;
the unused `hasWeight` flag is omitted. These owned Web UI projections
ship with the embedded frontend and do not change the public integration API.

Record frames carry no page-level UI state: ownership and the edit gate, the
clock anchor, timezone, firmware version and header signals come from the
Home stream, which the firmware sends first in every dispatch on the same
socket. Accepted WebSocket ops (claim, bind,
activity, and the view subscriptions) retain the confirmation of pending
Wi-Fi settings on a station connection.

Home's live stream and the Admin status response include
`connections` (the Home stream names the scale link `scale.available`
instead of `connections.bluetoothConnected`): `wifiConnected` and
`bluetoothConnected` are booleans;
`wifiLevel` and `bluetoothLevel` are firmware-calculated levels:
`1` weak, `2` medium, `3` strong, or `null` when unavailable. The connection
booleans determine whether to display disconnected, regardless of a cached level.
`wifiName` is the connected station SSID; `bluetoothName` is the connected
scale's saved friendly name, falling back to its recognized model name, or to
its advertised BLE name when the model is unknown. Names
are empty when the link is disconnected or no matching scale identity is
available. The browser hides these identities when the snapshot is stale.
Wi-Fi refers to the controller's station link, and Bluetooth refers to its scale
link. Valid readings range from −128 through 0 dBm. The header uses three
display levels: weak below −80 dBm, medium from −80 to below −60 dBm, and
strong at −60 dBm or above. Once a link has a level, it changes only when the
reading clears the boundary by 3 dB (for example, medium becomes strong at
−57 dBm and strong drops to medium below −63 dBm), so a link sitting on a
boundary does not flicker. A disconnected link takes
precedence over any reading. Stale control snapshots or missing measurements
show unavailable signal quality. These display fields add no radio queries or
extra polling requests and do not change the public integration endpoints.

### Home live updates

The claimed browser binds the existing WebSocket at `/api/v1/ui/ui-stream`
with `{"op":"bind","client":"<claimed X-WebUI-Client>"}`. A reconnecting page
adds the `presetsTag` of the presets it still holds; when it matches, that
connection's first Home snapshot omits `presets` (it still carries
`presetsTag`) and the page keeps its copy. Home sends `presetsTag` with every
presets change. The bind also carries the page's standing subscriptions as
`"ops":[…]`, each exactly the op it would otherwise send on its own (Settings,
History, Stats, Diagnostic and its log), so a reconnect costs one request
frame; a page with nothing to replay omits the list. The controller runs them
in order once the bind is accepted, and one it rejects, or a list that is not
an array, closes the stream with code 4003 like a separate op. A request
frame holds at most 640 bytes, so the page sends any subscription that does
not fit (a Stats subscribe with a long filter) as its own op after the bind.
Home receives an
initial snapshot and then field changes, without periodic REST status requests.
While a browser is connected and a scale is streaming, the firmware's periodic
comparison of the published state runs right after every second weight sample
(about every 200 ms with a 10 Hz scale, never closer than 150 ms), so stream
traffic stays clear of the scale's next Bluetooth reading. Without a scale,
while it is silent, or when two samples take longer than 300 ms, it compares
every 300 ms. Urgent updates (shot start and end, control changes, and
subscription requests) are sent immediately.
Unchanged displayed values produce no Home data frames. Weight
and timer are inspected only while the scale is connected, at the displayed
precision (0.1 g and 0.1 s); unavailable readings become `null` once.

A Home snapshot has `v: 1`, `type: "home"`, `boot`, `snapshot: true`, and
`changes`; a Home increment carries only `type` and `changes`, and continues
the identity of the last snapshot on the socket.
The header uses the firmware-calculated signal levels described above.
RSSI changes within a level produce no signal delta; raw RSSI remains
available in Diagnostics. The header popups display level names without dBm.
The initial snapshot supplies the complete Home state; subsequent `changes`
objects contain only changed paths, such as `scale.timerMs` or
`lineaMicra.websocket.cleaning`. Objects such as `presets` are replaced as a
unit when they change. Reliable, ordered WebSocket delivery applies these
scalar assignments in order. The shot card and curve retain their independent
sequence and cursor contract on the same socket. A shot snapshot
(`snapshot: true`) names the whole identity (`v`, `boot`, `seq`, `cycle`,
`stale`, `phase`, `cursor`) and carries the complete card and curve.
Each following frame carries only `seq` and `cursor` (plus `stale` and `phase`
together when either changed), the card fields that changed, which the browser
merges over the previous card, the curve points after the previous cursor
(with `wBreakBefore` or `wHeld` left out when the new points add no index to
it; a snapshot always carries both), and
the curve scalars (`wTruncated` and the markers: first drop, extension, scale
loss and recovery, end) that changed. A frame whose card did not change
carries an empty `card` object; everything a frame omits continues from the
previous one, and a sequence gap requests recovery. Only a new shot or controller boot, a reconnect or resync, or a
change to an already sent point starts a new snapshot. Equipment updates do not resend an
unchanged shot card or curve. Commands continue to use the existing
HTTP endpoints and ownership checks.

Micra firmware streams an informational boiler-readiness layer on the same
frames: `lineaMicra.readiness` (the derived rollup: `off`, `warming_up`,
`waiting_for_steam`, `ready`, `needs_water`, or `unknown`),
`lineaMicra.boiler.coffee.state` and `lineaMicra.boiler.steam.state`, and each
boiler's `readyAtUtcSec` estimate. `readyAtUtcSec` is an absolute UTC second
timestamp (0 when the machine gave no estimate), and the browser derives the
ticking countdown locally, so a stable estimate emits no frames. Readiness is
never evidence for control; power stays ON/OFF-only.

Reconnect and `{"op":"resync"}` request fresh snapshots; the resync op also
forces a fresh History snapshot for subscribed sessions. The browser sends a
resync only after rejecting a frame. Returning to a page that stayed open
needs none: the socket delivered every frame in order, so the browser only
sends its liveness `activity` message. A page hidden for 5 minutes or longer
(screen locked, another app or tab) closes its stream and reconnects with
fresh snapshots when it becomes visible again, except while a shot is live.
The firmware also closes a bound session that has sent nothing for 5.5
minutes outside a live cycle (a phone that froze the page) with WebSocket
close code 4002 and notes it in the Web UI log; the page reconnects quietly
once visible, without the out-of-reach notice. Neither close affects the
15-minute inactivity deactivation, and a setup session never closes on hide. Ownership takeover
closes the previous socket with code 4001. An op from the owning page that
fails validation closes the stream with code 4003 and a Web UI log warning; the
page clears its list filters and Stats window before it reconnects, so the
rejected subscription is not repeated. The browser's bounded
`{"op":"activity","seconds":0-30,"hidden":bool}` message, answered with
`{"type":"alive"}`, only fills gaps in the traffic the page already has: every
received frame proves the socket and clears the liveness deadline, and every
op or claimed API request proves the page (an accepted API request also renews
the silence timer of that client's stream). The page sends `activity` every
20 seconds while the user is active outside a live shot (`seconds` keeps the
existing power activity window), during a live shot only after 20 seconds
without frames, and otherwise at least every 2 minutes. These messages do not
request or repeat Home data. A hidden page sends `activity` with
`hidden: true` at once; the firmware then holds that session's frames outside a
live shot (an owed snapshot still goes out) and keeps their fingerprints, so
the visible `activity` (`hidden: false`) is followed by one catch-up delta
instead of every intermediate change. Clock synchronization
supplies a new time anchor; the browser advances relative time locally.
`GET /api/v1/status/home` no longer exists: navigation visibility
(`diagnosticPageVisible`, `compatibilityMode`) is carried by the Home stream
itself and applied on every view, and the browser no longer probes a status
route when entering Stats, History, or Diagnostic. Settings and Diagnostic are
WebSocket-only as well and are described next; only the Admin page (and the
setup portal) still reads `GET /api/v1/status/admin`. These Web UI routes and
streams belong to the embedded page alone: Home Assistant and other
integrations use the public `/api/v1/integration` routes and webhooks.

### Settings live updates

The Settings page subscribes on the same owned socket with
`{"op":"settings","on":true}` once per connection, in the background after the
first view has loaded, and stays subscribed for the rest of the session, so
opening Settings paints at once from the model it already holds, without
another message or request. Compatibility mode, which has no Settings page,
never subscribes. The op accepts only `op`, `on` (which must be `true`) and an
optional `tag`; anything else closes the stream with code 4003 and a Web UI log
warning, like the other ops. Settings has no REST status route.

A settings snapshot has `v: 1`, `type: "settings"`, `boot`, `snapshot: true`,
`changes` and `tag`; later frames carry `type`, `changes` and `tag`, with only
the paths whose values changed. The frame carries only paths the Home frame
does not: Home owns `config.revision`, the shared `config.*` keys it already
streams, `presets` (including the active preset's dose and grind-hint switch),
`lastCommand`, `quickRevert.after`, `quickRevert.overrides`,
`quickRevert.remainingS` (rounded up to whole minutes, the resolution the
header panel shows), `connections`, `scale.preferred*` and its
`lineaMicra.*` keys, and the page reads them from the Home model, so a change
to one of those settings reaches the browser on the Home frame alone. Home's
`config.*` values are the effective ones, Quick Settings overrides included.
The settings frame adds `quickRevert.saved`, an object with the saved value of
each overridden field (empty when none is active, sent whole when it changes),
which the form shows in place of the effective value so **Use saved setting**
sends it and drops exactly that override. It also adds the remaining
`config.*` keys the form shows, the drip
chart and quick start as one `config.bbwDrip` object (sent whole when any part
changes), `config.bbwPresetId`, `config.bbwEwmaOffsetG` and `config.bbwAlpha`,
`scale.history`, `scaleLoss.projectedS` and `scaleLoss.source`,
`buzzerSupported`, and in Micra firmware the Settings part of `lineaMicra`
described under [Linea Micra Web API](#linea-micra-web-api). The cup-minimum
hint follows Home's `scale.observedWeightG`, so this frame carries no live
measurement.

A dispatch writes its Home, shot-card and settings frames together, so a
save's new `config.revision` and its settings changes arrive in one write and
in order. The firmware evaluates the projection only when one of its sources
changed (saved settings and their revision, the saved values behind Quick
Settings overrides, the bullseye tune, scale history,
drip and learning readback, the scale-loss projection, Micra account state),
so an idle controller sends no settings frames; each dispatch only compares a
checksum of those sources, without formatting the projection. In
compatibility mode the controller accepts the subscribe but sends no
settings frames.
`tag` names the exact model a frame leaves the page with. The page keeps that
model when the socket drops and replays the subscribe with
`"tag":<last tag>`; when the controller's model still matches, that connection
sends no settings snapshot and only later changes follow. `{"op":"resync"}`
and a controller restart always send one. Saves stay REST commands and still
wait for `lastCommand` and `config.revision` on the Home frame; the form then
follows the streamed values field by field and keeps any field with a pending
local edit.

### Diagnostic live updates

The Diagnostic page subscribes on the same owned socket with
`{"op":"diagnostic","on":true}` when the view appears and unsubscribes on
every navigation. The first frame after subscribing is a snapshot; later
frames are `changes` deltas merged in order, exactly like Home. The
`{"op":"resync"}` message re-arms a fresh snapshot, and `GET /api/v1/log`
and `/api/v1/status/diagnostic` no longer exist.

Diagnostic frames follow the same shape: the snapshot names `v: 1`,
`type: "diagnostic"`, `boot` and `snapshot: true`; later frames carry only
`type` and `changes`. They carry only paths no other frame owns: the page
reads the machine and cup state, safety state, scale weight, timer and stream
state, guard switches (the `config.*` and `noScaleShotGuard.*` paths),
firmware identity, unlock state, last command, timezone and the Micra summary
from the Home frame, which every dispatch sends first, and the Micra account,
selection and request phase from the Settings frame. In compatibility mode,
which has no Settings subscription, Diagnostic sends those four Micra paths
itself. The extraction guards share one `guards.activeStopWeightG` and one
`guards.targetReachedEarly`. A field is sent only when its value changed since it was last
sent. An unchanged projection emits no frame; displayed ages, uptime,
traffic and other live measurements can still change on an otherwise idle device.
Event-driven state (control, safety, guards, maintenance, crash count, NTP
and Wi-Fi link state, machine integration) is evaluated on every dispatch
tick and lands within one tick (about 200 ms with a 10 Hz scale, at most
300 ms). Countdowns and ages are quantized to whole seconds (the no-scale
cooldown to whole minutes) so a ticking value costs at most one small frame
per second. Sampled telemetry (heap and CPU health, hardware monitor, NVS
statistics, steady-state Wi-Fi RSSI and signal quality, task profiler, scale
profiler, machine-integration traffic counters) shares a four-second
evaluation gate, so its producers cost no more than the retired status poll.
NTP synchronization age/retry countdowns and raw Micra subscription/reconnect
counters are not sent to this view. The task-profiler projection includes
the displayed CPU, stack and loop-gap measurements; the full debug export
retains additional capture and execution statistics. Scale-profile action
availability and errors remain in the live projection, while internal
session and storage metadata remain owned by the profiler.
A snapshot arrives in stages: the core frame carries the live projection and
the following frames deliver the heavier sections; the page renders each
section as it lands. The wall clock is not streamed: the page renders UTC and
local time from the Home stream's time anchor and ticks locally.

The serial log subscribes on the same socket with
`{"op":"log","on":true,"after":<last rendered sequence>}` while the page is
shown and unsubscribes on navigation. Log frames have `v: 1`, `type: "log"`,
the display payload (`bootId`, `historyOverwritten`,
`missedEvents`, `serialDropped`, `hasMore`,
`cursorInvalid`, `events`), and are pushed when the log ring advances; a
backlog pages across dispatches with `hasMore`, and an invalidated cursor
refills the view from the oldest retained event without a client round trip.
Each event carries `sequence`, `atMs`, `wallSec`, `localSec`, `level`,
`category` and the formatted `message`; internal event codes and arguments
are retained in the firmware rather than repeated on the wire.

Commands from the Diagnostic page (log levels, profiler start and stop, loop
and scale-gap resets, reset-history clearing, machine-integration refresh)
keep their HTTP endpoints; their results reach the page through the stream's
`lastCommand` and configuration revision fields, so no status polling backs
them. The diagnostic actions under `/api/v1/diagnostic/` (verbose audit,
task profiler, scale profile start/stop/delete, heap map, loop and scale-gap
resets, tare, reset-history clearing) additionally need **Show diagnostic
page** on or the Admin unlock; otherwise they answer `401 ADMIN_LOCKED`.

The former `/api/v1/shots` route family was renamed to `/api/v1/stats` and its
GET read has since moved to the owned WebSocket: external Web UI API callers
must use the stream described below for reads and keep the POST URLs
`/api/v1/stats/clear`, `/api/v1/stats/delete`, and `/api/v1/stats/rate`
(request bodies and mutation safety checks are unchanged).
The public `/api/v1/integration` endpoints are unchanged.

Stats shot records and Home's WebSocket `curve` carry the observations as
`wD`, the firmware's delta stream in base64: recorded centigram weights with
their elapsed reception times in integer milliseconds, up to 1201
observations. Each point is one time byte (0–254 ms after the previous point)
and one signed weight byte (−127…127 cg); a larger step stores the marker
`0xFF` (time) or `0x80` (weight) followed by the little-endian absolute value.
A whole curve starts from 0 ms / 0 cg; a Home fragment that continues earlier
points names the last one it follows as `wRef: [atMs, cg]`, which must match
the point the client already holds. The browser decodes the stream into the
`wCg`/`wAtMs` arrays its charts and CSV use. `wBreakBefore` lists zero-based indices that
start new continuous segments. `wHeld` lists the indices of readings the
accidental-touch guard held (a hand on the cup or scale): they are what the
scale showed, not coffee weight, and belong to no segment. Curves recorded
before this field existed report it empty. `wTruncated` identifies an
incomplete capture.
Curve event fields such as `dropS` and `endS` remain seconds and retain three
decimal places. Events are annotations, not extra received observations.
The [shot-history export contract](features/shot-history.md#read-a-result)
describes the corresponding ordinal CSV sample groups.

### History live updates

The History page rides the same owned WebSocket as Home. Entering the view
subscribes with the page window:

```json
{"op":"history","on":true,"offset":0,"limit":20,"dir":"desc","types":["shot","rinse"]}
```

`types` (optional) lists the activation types to show — `shot`, `rinse`,
`backflush`, `power_on`, `other`, `no_scale_guard_aborted` — and a subscribe
without it shows every type. The session keeps this standing window and
filter. Scrolling to the page sentinel sends a one-shot append fetch for the
next window without disturbing the subscription; a fetch without `types`
keeps the standing filter, and the CSV export sends `"types":[]` to read
every record:

```json
{"op":"history","on":true,"fetch":true,"offset":20}
```

History subscribes on the first visit to the view, and leaving the view does
not unsubscribe it: the browser keeps the History window standing for the
rest of the UI session, so arriving records update the page even while
another view is open. Opening History again with that live data paints
straight from the cached page. After a reconnect the browser
replays the subscription, so the first open still starts from a snapshot;
returning to a page whose socket stayed open needs no resync. Every accepted
subscribe forces the next frame to be a snapshot of the requested window.

Every History frame carries `tag`, which names the standing page it leaves
the browser with. A subscribe may add `"tag":<last tag>` for the page the
browser still holds (also after a reconnect); when the controller's page
still matches, that snapshot sends every record as its bare `id`, so an
unchanged page costs a few bytes. `{"op":"resync"}` always sends whole
records.

History frames have `v: 1`, `type: "history"`, `boot`, `snapshot`, `epoch`,
`request`, `tag`, `total`, `offset`, `limit`, `hasMore`,
and `records`. Each record carries `id`, `type`, `durationS`, `hasWallTime`,
`endedAtUnixSec`, and `endedAtLocalSec`. A store epoch and a fingerprint over the finished frame
suppress sends while the page is unchanged, the same no-change contract as the
Home delta: an untouched activation log produces no frames. Appends, single
deletes, and clears move the epoch, so the standing subscription pushes the
updated page without any browser request; clearing and deleting remain the
confirmed REST commands above. In such a standing refresh (`snapshot: false`)
a record the browser already holds unchanged from the previous standing page
is sent as its bare `id` number, so a new activation costs one record; a
snapshot carries whole records unless its subscribe named the unchanged page
by `tag`, append fetches always carry whole records, and an id the browser
cannot resolve requests a resync.

### Stats live updates

The Stats page uses the same owned WebSocket. Entering the view subscribes
with the page window:

```json
{"op":"stats","on":true,"offset":0,"limit":10,"sort":"date","dir":"desc",
 "window":50,"filter":{"result":["fast","slow"],"preset":[3],"scale":[2884452355]}}
```

`filter` (optional) narrows the shot list: `result` takes `fast`, `slow`, and
`on_recipe` (only timed by-weight shots have one), `preset` takes preset ids,
and `scale` takes the scale keys listed in the header's `facets`. Each list
holds at most eight values; an empty or missing list does not restrict, lists
combine with AND and the values of a list with OR. Every page and fetch holds
good shots only (longer than 12 s with a settled yield over 2 g); `"bad":true`
also lists the bad shots, combined with the other lists as usual, and each row
names why it is bad in `badReason` (`short`, `no_yield`, `low_yield`, or
`null` for a good shot). `window` (`10`, `50`, or `100`, default `10`) picks
how many of the newest good shots the `stats` header covers. A subscribe
carrying only `window` (no `request`) changes the header alone: the firmware
sends a standing refresh with the new header and the rows it already holds as
bare ids.

Scrolling to the page sentinel sends a one-shot fetch for the next window,
and the CSV and JSON exports ask for their own export window — up to 1,000
rows, newest-first by date, good shots only (the diagnostics export adds
`"bad":true`), regardless of the on-screen sort. A fetch without `filter`
keeps the standing filter. Nothing is exported during a shot, drip delay
included: while the shot's trace capture is open (Home's `cycle.capturing`)
the firmware sends no frame of a fetch, an export window or a scroll page,
and resumes it when the capture closes. The Web UI starts no export then and
cancels a running one when a shot starts, by subscribing again, which drops
the pending fetch:

```json
{"op":"stats","on":true,"fetch":true,"offset":20}
{"op":"stats","on":true,"fetch":true,"offset":0,"limit":1000,"sort":"date","dir":"desc","filter":{}}
```

Stats likewise subscribes on the first visit, and leaving the view does not
unsubscribe it: the window stays standing for the rest of the UI session,
refreshes in the background while any view is open, and re-opens paint
straight from the cached rows. After a reconnect the browser replays the
subscription, and every accepted op except a
window-only change forces the next page to start from a snapshot. As in
History, a subscribe may name the standing page the browser still holds with
`"tag":<last tag>`; a page that still matches comes back as a snapshot whose
rows are bare ids (the header is sent again).

Stats frames have `v: 1`, `type: "stats"`, `boot`, `snapshot`, `epoch`,
`seq`, `request`, the `stats` aggregate, `cols` and the page's `tag` (first
frame of a page only), `total`, `offset`, `limit`, `hasMore`, `rows`, `rowBase`, and `more`.
In this stream the aggregate covers the subscribed `window` (and leaves out
`durationsS`), and it also carries what the Stats charts draw, so they match
the averages whatever the sort, filter, or scroll:

- `dens`: the arrival-time density charts for all presets and for the active
  preset. Each is `{lo, hi, a, b, shared, n, h}`: the time axis from `lo` to
  `hi` seconds, the on-recipe zone from `a` to `b` (window seconds when
  `shared`, else offsets from a common window), the `[fast, on_recipe, slow]`
  shot counts, and `h`, 121 heights from 0 to 100 sampled evenly from `lo` to
  `hi`, scaled to the curve's own peak. The active-preset entry is `null`
  without timed shots of that preset and `true` when it equals the first.
- `cons`: the window's timed shots oldest first, for the consistency chart:
  `t`, times to target in tenths of a second, and `p`, their preset ids (one
  number when they share one).
- `facets`: the presets (`[id, name]`) and scales (`[key, name]`) the good
  shots of the whole log hold, newest first, up to eight of each, for the
  list filter.

The integration API's stats object stays at ten shots.
Each row retains its own `bootId` and curve arrays; row errors are calculated
from the displayed yield by the browser. Because full curves can exceed one
frame, a page is emitted under an 8 KiB frame budget (a single larger row goes
alone): the first frame ends with `more: true` and a
continuation frame resumes at its `rowBase`; the browser buffers partial
pages and applies them whole when a frame ends with `more: false`. A store
epoch and a fingerprint (window, aggregate, and per-row id, rating, and
curve hashes) suppress pages while nothing changed — rating a shot, saving,
deleting, or clearing moves the epoch, so the standing subscription pushes
the updated page without any browser request. As in History, a standing
refresh sends each row the browser already holds with the same rating and
curve as its bare `id` number, so a new shot sends only its own row and curve
plus the page header; scroll fetches, the export, and snapshots not named by
`tag` always carry whole rows. The export window streams the
requested rows once without touching the subscribed view state. The newest
500 curves are in memory; an older row's curve is read from flash, which is
never done while a shot runs: such a page pauses at that row, sending
nothing, and continues once the shot (and its drip delay) is over. A curve
that storage recycled comes as an empty curve. The server
refreshes its cached records only when the store epoch moves; sort, filter,
and window changes reuse that snapshot, and the header is rebuilt when the
store, the window, or the active preset changes. This preserves chronological
CSV order after browsing by rating without repeated store captures per frame.

### Record index for the offline copy

A page opened over HTTPS (where its service worker can start it with the
controller off) keeps every Stats row and History record in IndexedDB. To
sync, it names the store epoch its copy holds (0 for none):

```json
{"op":"records","shots":1234567}
{"op":"records","history":0}
```

The op needs no subscription and answers each named store once, one frame per
dispatch: `v: 1`, `type: "records"`, `boot`, `store` (`"shots"` or
`"history"`), and `epoch`. Only when that epoch differs from the one sent does
the frame add `rows`, a flat array of `[id, revision]` pairs for every record
(for shots, every good shot: the offline copy keeps no bad ones), newest
first. The revision is the low 24 bits of a CRC over the
stored record, so a rating change, or an id reused after a clear, reads as a
different record. Store epochs survive restarts, so an unchanged store costs
one small frame per sync; 1000 History pairs stay under the 20 KiB frame
budget. The browser then fetches only the missing or changed records through
the standing subscriptions' fetch windows (Stats: `sort: "date"`,
`dir: "desc"`, `filter: {}`; History: `types: []`), drops records the index
no longer lists, and keeps the new epoch only once every listed record is in
its copy. It syncs after each view load (at most once a minute) and 30 s after
the machine stops running, never during a live shot. Plain HTTP pages never
send the op.


## Linea Micra Web API

Micra-compiled firmware exposes `POST /api/v1/machine/linea-micra` to the
claimed Web UI. It accepts one strict `action`: `connect`, `select`, `save`,
`refresh`, `relist`, or `disconnect`. `connect` requires only `username` and `password`
and is rejected unless STA is connected and the setup AP is closed. It queues
cloud authentication and account-machine discovery but does not persist or
enable an account. `select` requires a `serial` from the latest discovery plus
the independent `applyTemperature`, `observeState`, and
`recognizeWakeGesture` booleans; it persists the
account credentials, installation key, and chosen machine through the normal
single-writer command path. `select` and `save` also carry the `powerOnWithScale`
and `shutdownWithScale` machine-link booleans with `shutdownGraceSeconds`,
and the `scaleOffWithMachine` scale-link boolean. `select` and `save` optionally accept `connectionType` as exactly `websocket`
or `api`; missing retains the saved preference. Null, wrong types, unknown
values and use with any other action are rejected. WebSocket is the default.
`save` is a patch for an already selected machine: it needs at least one option or `connectionType`, and every option it omits keeps its saved value. `refresh`, `relist`, and `disconnect` accept no additional fields;
`relist` requires a selected machine, STA, and a closed setup AP, and lists the
saved account's machines again over the existing session and installation key
without changing the active machine, the published request phase/error, or
persisted settings, so a following `select` can switch machines;
disconnect erases durable credentials/selection and the RAM session. Unknown,
duplicate, or action-inappropriate fields are rejected. Accepted commands
return `202`.

The Web UI streams carry a `lineaMicra` subtree only in Micra firmware. The
Home frame reports whether an account is configured, `observeState`,
`connectionType`, the ON/OFF/UNKNOWN power state, evidence quality,
optimistic-ON and optimistic-OFF provenance, `readiness` (`off`, `warming_up`,
`waiting_for_steam`, `ready`, `needs_water` or `unknown`) and the boiler states,
estimates and brew target (`lineaMicra.boiler.coffee.targetDeciC`, 0 when the
machine reported none). The Settings frame adds the saved account `email`, the
selected name and serial, the option flags with `shutdownGraceSeconds`, the
request `phase` and `error`, `machineOffline` (true when the cloud reports that
the machine itself is disconnected), the STA/AP/shot gates (`staConnected`,
`apActive`, `shotPaused`), and the bounded machine list returned while
connecting or after `relist`: `machines`, `machineListing` (true while a
`relist` is queued or running) and `machineListError` (its outcome, with the
same error names). The saved account email is visible in Settings and
Diagnostics; password, installation private key, access token and refresh
token are never returned. `refresh` is read-only and requires a selected
machine; none of these fields authorizes machine actuation.

The Diagnostic stream adds the raw observed mode, power source, sample age and
freshness, and temperature state. Beyond the cleaning fields Home shows, only
it includes the `lineaMicra.websocket` detail: socket lifecycle/reason/epoch,
subscription and machine connectivity, genuine evidence timestamps, payload
byte counters, fixed 1s/60s rates, retry/stop/publication latency and memory
metrics. It also carries the boiler detail beyond the Home projection:
`lineaMicra.boiler.steam.level` (`level_1`–`level_3`) and a whole-second
`lineaMicra.boiler.sampleAgeMs`.
`cloudCall` remains the last HTTP call. Cleaning never changes control state.
Payload byte totals use 64-bit counters and include positive partial sends.
Rate windows use the 64-bit monotonic clock, independently of millisecond
timestamp rollover; these counters exclude TCP/TLS framing and radio traffic.
Connect/stop free/largest deltas refer to internal heap; retained workspace bytes
refer to PSRAM. Cleaning age/source hints are hidden when observation is disabled
or API is selected.
Generic diagnostic `outboundPause` exposes acquisition generation/reason,
concurrent `scaleSetup`, `bleQuiet`, `shot` and `maintenance` gates, and per-client
applicability and owner completion/latency. `requested` and `completed` refer to
the acquisition generation; completion excludes residual DNS work.
Webhook status distinguishes `lastDispatched` and `lastDeliveryUnknown`.
`lastDispatched` becomes true only once the request headers reached the socket,
so DNS and connection failures report "not sent" rather than an unknown result.
`dropped` counts only subscribed events that could not be queued or delivered;
events the receiver did not select are not counted.

## Webhook version 1

All messages are POSTed as bounded JSON. Common fields are `schemaVersion: 1`,
`event`, `deviceId`, `bootId`, `cycleId`, `uptimeMs`, `timestamp`, and
`sentAtUptimeMs`. `brew_state`, `first_drop`, and `end` contain both `presetId`
and the name captured at cycle start in `presetName`.

`presets_changed` is emitted only after confirmed persistent preset mutation
and contains `activeId`, monotonic `revision`, and the same bounded `items`
array as the presets route. It is disabled for migrated webhook configurations
and enabled explicitly by the native integration.

`quick_settings_changed` is emitted as soon as a session override is live, and
after persistence when a saved change (a Settings save or a preset change)
alters the effective values. Its complete Quick Settings snapshot is flat in
the event envelope:

```json
{
  "schemaVersion": 1,
  "event": "quick_settings_changed",
  "deviceId": "AA:BB:CC:DD:EE:FF",
  "bootId": 123,
  "cycleId": 0,
  "uptimeMs": 930100,
  "timestamp": 1767225619,
  "sentAtUptimeMs": 930110,
  "revision": 20,
  "activePresetId": 2,
  "brewByWeight": true,
  "noScaleBbwMode": "warn_once",
  "autoToManualGuardEnabled": true,
  "slowExtractionGuardEnabled": true,
  "fastExtractionGuardEnabled": false,
  "avoidAccidentalTouchEnabled": true,
  "cupProtectionEnabled": true
}
```

`controller_started` is one best-effort boot hint containing controller identity,
`bootId`, and the current revision; receivers use it to perform one full REST
reconciliation. The existing
`presetChanges` subscription bit gates all integration-state events:
`presets_changed`, `quick_settings_changed`, `controller_started`, and
`integration_history_end`.

`integration_history_end` mirrors the newest activation-history record
(shot, rinse, other, power on, or a no-scale guard abort) and carries `id`,
`type`, `durationS`, `hasWallTime`, `endedAtUnixSec`, and `endedAtLocalSec` —
the same shape as the snapshot's `lastActivation`. It is emitted only after
the activation record is confirmed in history, including after controller
restart when the persisted `presetChanges` subscription remains enabled. A
receiver that misses it can
recover the value at the next reconciliation.

Receivers deduplicate by `(deviceId, bootId, cycleId, event, uptimeMs)`, reject
older events for the same boot, and refresh the REST snapshot when configuration
or preset revisions skip. Numeric shot fields must be finite and non-negative;
`targetWeightG` is the recipe target in grams quantized to tenths (`36.5`). Supported
shot types are `auto`, `timer_only`, and `manual`; supported stop details are
`normal_target`, `extended_max_weight`, `extended_min_time`, `auto_to_manual`,
`slow_max_time`, `slow_min_weight`, `cup_removed`, `activator`, `web_stop`,
`web_heartbeat`, `physical_override`, `hard_limit`, `wall_limit`,
`relay_safety`, `weight_anomaly`, `touch_weight_fallback`, and `other`. The retired
`prediction` is never sent; receivers treat it as `other`. These stop details, the Quick Settings names, the end
event keys, and the Stats CSV columns are frozen by
`src/tests/fixtures/wire_contracts_v1.json`; a change to any of them updates
that fixture, this reference, and the Home Assistant integration together.
`touch_weight_fallback` identifies a stop after sustained above-threshold
readings while accidental-touch protection blocks the normal cut. REST `lastShot`,
end webhooks and local history preserve this same value. Update strict receivers,
including the Home Assistant integration, before installing firmware that emits
it; older integration versions reject unknown stop details.

The webhook ID protects only the Home Assistant receiving endpoint; it does not
authenticate controller API calls. Do not log or include it in diagnostics.
Removing a Home Assistant entry conditionally clears only a callback whose
exact URL still belongs to that entry.

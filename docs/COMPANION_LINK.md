# Companion link protocol

This is the developer reference for the LAN contract between the controller
and [Open Brew by Weight Companion](features/companion.md). Pairing is
protocol 1, the link protocol 2, and the relay protocol 2 (sealed bodies and
headers). The companion is optional: a controller that never paired, or whose
companion is away, behaves exactly as it does without this feature.
Everything here is opened by the companion; the controller never connects to
the companion, never browses mDNS, and never retries.

Pairing (protocol 1), the link (protocol 2: store epochs in the ping
answer, `rs` once a change is saved, and the silent sync hold) and the relay
(protocol 2) let the companion serve the Web UI to several screens, keep a
replica of every shot and activation, and, on Linea Micra builds, carry the
La Marzocco route. The diagnostic log stream rides the link on every build.
The link version changes alone: a controller and a companion of different
releases keep pairing and the relay (so the Web UI, firmware updates
included, still reaches the controller through the companion) while the link
refuses the other side's hello with `PROTOCOL`. Every build registers these routes; a controller that
is not paired answers them and does nothing else.

## Common rules

- Routes answer only on the controller's own `Host` names, like every other
  route (`403 HOST_NOT_ALLOWED`).
- They are exempt from the Web UI login: pairing is approved in Admin, and the
  link authenticates with the pairing secret, not with a Web UI session.
- Requests from a peer outside the private and link-local ranges (RFC 1918,
  169.254/16, loopback) answer `403 COMPANION_PEER`. The controller records
  the peer address from the socket; an address in a request body is never
  trusted.
- Errors use `{"error":"STABLE_CODE","reason":"STABLE_REASON","message":"English diagnostic"}`.
  `reason` is an additive per-sentence code documented in
  [Integration API](INTEGRATION_API.md#error-reasons); `error` and `reason`
  are the stable contract, while `message` is a non-normative debugging aid
  whose wording may change in any release. Exception: the relay handshake's
  signed erase reply keeps its own frozen `{"error","message","erase"}`
  body (a companion-native surface outside the reason catalog).
- Byte strings are standard base64 with padding. Public keys are 65-byte
  uncompressed P-256 points.

## Pairing

### `POST /api/v1/companion/pair`

`Content-Type: application/json` is required (`415` otherwise), so a web page
cannot send it without a preflight the controller never answers.

```json
{"id":"0b9f6a3c-2f4e-4d7a-9c1b-5e8f0a1d2c3b","name":"Kitchen server",
 "version":"1.0.0","protocol":1,"commitment":"OXjC…"}
```

`id` is the companion's UUID (36 characters), `name` 1 to 32 bytes of UTF-8
without control characters, `version` up to 24 printable bytes,
`commitment` the 32-byte `SHA-256("obbw-companion-commit-v1" ‖ companionKey)`
of the key the companion made for this attempt. A key seen in an earlier attempt
would let a device in the middle search for one that shows matching codes, so
each attempt makes its own. The key itself is revealed only after the
controller's key is known (below).

**Commit, then reveal.** Neither side learns the other's key before its own
is fixed, so a device in the middle cannot search for a key pair that makes
two different exchanges show the same code (it gets one chance in a million).

1. The companion sends the commitment (this request).
2. The controller makes its request key; `GET` returns `controllerKey`.
3. The companion sends the same request again with `"publicKey"` (the
   revealed key) instead of `"commitment"`. The controller accepts it only
   for the open request of that `id`, after `controllerKey` was issued, when
   the key hashes to the commitment and is a P-256 point (`400
   INVALID_REQUEST` otherwise, which also denies the request). Only then does
   Admin get the code (`codeReady`).

| Status | Body | When |
| --- | --- | --- |
| `202` | `{"state":"pending","controllerKey":"BHk…","expiresInS":600}` | A request is open (new, the same `id` and commitment asking again, or an accepted reveal) |
| `400` | `INVALID_REQUEST` | Malformed field, a key that is not a P-256 point, or a `protocol` other than 1 |
| `415` | `UNSUPPORTED_MEDIA_TYPE` | The body is not `application/json` |
| `403` | `COMPANION_IGNORED` | The user chose **Ignore** for this `id` |
| `409` | `ALREADY_PAIRED` | The controller is paired (Forget it in Admin first) |
| `409` | `PAIRING_BUSY` | Another companion's request is open |
| `429` | `PAIRING_RATE_LIMITED` | More than one new request in 5 s |

Only one request exists at a time, in RAM. It expires after 10 minutes or
when the controller restarts. `controllerKey` is a fresh key pair the
controller makes for this request, in the background: the answer to a new
request usually comes before the key exists and omits `controllerKey`, which
`GET` returns once it is ready (normally within a second). The same `id`
asking with a different commitment replaces its request, as a new one.

**Verification code.** Both sides compute
`SHA-256("obbw-companion-sas-v1" ‖ companionKey ‖ controllerKey)`, take the
first four bytes as a big-endian integer, reduce it modulo 1,000,000 and show
it as six digits with leading zeros. The companion's page shows it; the user
types it in Admin → Companion. Three wrong codes deny the request.

### `GET /api/v1/companion/pair?id=<id>`

`{"state":"pending","controllerKey":"BHk…","expiresInS":540}` (with
`controllerKey` once the controller has made it),
`{"state":"approved","controllerKey":"BHk…"}`, or `{"state":…}` with
`denied`, `expired` or `unknown` (no open request for this `id`). Poll about
once a second while `pending`. A request stays `pending` until the controller
has derived and kept `S`, a moment after the user approves it.

**Pairing secret.** On approval both sides derive
`S = HKDF-SHA256(ikm = ECDH x-coordinate, salt = SHA-256(companionKey ‖
controllerKey), info = "obbw-companion-pair-v1", L = 32)`. Nothing secret
crosses the network. The controller keeps `S`, the companion's id, name and
public key; it discards its request key.

### Admin actions

`POST /api/v1/companion/admin` needs Admin unlocked (`401 ADMIN_LOCKED`):

| Body | Effect |
| --- | --- |
| `{"action":"approve","code":"123456","lm":true,"log":true,"archive":true,"bc":true}` | Approves the open request; `lm`, `log`, `archive` and `bc` (optional, `true` when absent) are the La Marzocco, diagnostic log, shot archive and Beanconqueror switches chosen in the approval panel. `422 COMPANION_CODE` for a wrong code (the third one denies the request), `409 PAIRING_NOT_READY` before the controller's key exists |
| `{"action":"ignore"}` | Denies the open request and ignores that `id` until **Stop ignoring** (the four most recent ids, kept across restarts) |
| `{"action":"unignore"}` | Forgets the ignored ids |
| `{"action":"forget"}` | Removes the pairing and closes the link; the next link attempt receives `erase` |
| `{"action":"switches","ui":false,"lm":true,"log":true,"archive":true,"bc":true}` | Sets the switches present in the body: `ui` is **Web UI through the companion** (on when paired), `lm` the La Marzocco switch (Linea Micra builds only; `400 INVALID_REQUEST` elsewhere), `log` **Diagnostic log through the companion**, `archive` **Shot archive in the companion** and `bc` **Beanconqueror through the companion** (every build). An open link receives a new `cfg` |

Every action answers `{"ok":true}`. Approve and Ignore without an open request
answer `409 PAIRING_NOT_PENDING`; Forget and Switches without a pairing answer
`409 COMPANION_NOT_PAIRED`.

Admin reads the pairing, the open request and the link state from
`GET /api/v1/status/admin`:

```json
"companion":{"paired":true,"name":"Kitchen server","address":"192.168.1.20",
 "version":"1.0.0","link":"open","linkAgeS":3600,"peerProtocol":0,
 "ui":true,"log":true,"archive":true,"bc":true,"relay":"ready","relayAgeS":12,"relayRejects":0,
 "route":"companion","lm":true,"upstream":true,"refused":false,"credentials":true,
 "request":null,"ignored":0}
```

`link` is `open` or `down`, `linkAgeS` the seconds since the link last came
up or went down (`-1` when neither happened since the controller started),
`peerProtocol` the protocol of the paired companion's last hello that this
controller refused with `PROTOCOL` (0 when none, or once a link opens), `ui`
the Web UI switch, `log` the diagnostic log switch, `archive` the shot archive
switch, `bc` the Beanconqueror switch, `relay` `ready` once a
[relay handshake](#relay) succeeded
since the controller started (`down` before and after Forget), `relayAgeS` the
seconds since the last relay handshake or admitted request (`-1` none since it
started), `relayRejects` the relayed requests refused since then; on Linea Micra builds `route` is
`direct`, `pending` or `companion`, `refused` mirrors the `lm` frames'
`err:"auth"` while the companion's cloud reads are being refused,
`credentials` tells whether the
companion confirmed the current account (other builds omit `route`, `lm`,
`upstream`, `refused` and `credentials`), and `request` is
`{"name","address","version","expiresInS","codeReady"}` while a request is
open (`codeReady` once the companion revealed its key and the controller's key
exists). Home's stream carries `companion.requestName` and
`companion.requestAddress` (empty strings when no request is open) and
`companion.archive` (paired with the shot archive switch on, so History and
Stats opened directly can say the companion keeps a copy). The Settings
stream carries `companion.beanconqueror` (paired with the shot archive and
Beanconqueror switches on, so Coffee apps can show the companion's address).
The Diagnostic stream carries `companion.paired`, `name`, `address`, `version`,
`protocol` (the open link's, 0 while down), `peerProtocol`, `link`,
`linkAgeS`, `reconnects`, `authFailures` (link and relay handshakes), `ui`,
`log`, `logCursor`, `logFlushS` (age of the last log flush, `-1` never),
`logGaps` (events reported missing this boot), `archiveSwitch`, `bcSwitch`,
`archiveSyncS` (age of the last `rec` or `trc` frame, `-1` never this boot),
`pushWanted` (this session's `ps`), `pushSent` and `pushSkipped` (this boot),
`pushLastId`, `pushLastS` (age of the last `ev`, `-1` never this boot),
`pushSkip` (why the last shot was not sent: `bad`, `stale`, or empty),
`relay`, `relayRejects` and,
on Linea Micra builds, `route`, `reason` (why the
route is direct; `protocol` while `peerProtocol` is set), `lm`, `upstream`,
`routeSwitches` and `commands`. Names, versions and addresses are stored
without quotes, backslashes or angle brackets; the name and version follow the
companion's authenticated `hello`.

## Link

The companion opens the link only while a feature needs it: the [relay
handshake](#relay) answers `link` (`true` on Linea Micra builds with the La
Marzocco switch on, and on every build while the **Diagnostic log through the
companion** or the **Shot archive in the companion** switch is on). While the
relay handshake fails, the companion keeps the link open.

`GET /api/v1/companion/link` upgrades to a WebSocket. Only text frames are
used, each at most 4,096 bytes including a signed frame's MAC prefix, except
the controller's `rec` and `trc` answers (below), which may reach 28,672
bytes; the companion's receive limit is that size, and everything it sends
stays within 4,096. One
link exists at a time: a newly authenticated link replaces the previous one.
The upgrade answers `403 COMPANION_PEER` from a peer outside the local ranges,
`404 COMPANION_NOT_PAIRED` when the controller is not paired (a forgotten
companion is still let in, to receive `erase`), and `409 COMPANION_BUSY` while
a link and another opening link already exist. The opening must finish within
10 seconds. The controller keeps the link ahead of idle Web UI connections when
its HTTP server needs a free socket, and the Web UI log names why a link went
down (silence, a frame that could not be sent, the companion closing it, a lost
connection, or an eviction for another connection).

### Opening

1. Companion: `{"t":"hello","id":"…","name":"…","version":"…","protocol":2,
   "nonce":"<16 bytes>"}`.
2. Controller: `{"t":"challenge","nonce":"<16 bytes>"}`, or
   `{"t":"error","code":…}` and close: `NOT_PAIRED` (unknown id),
   `PROTOCOL` (a link version other than 2; Admin and Diagnostic then say
   that the two sides use different link versions, and the relay keeps
   working), `RETRY_LATER` (a shot with a scale, a
   scale connection or the scale's quiet interval is in progress; the same
   rule as the controller's own La Marzocco connection), or a forgotten companion's
   `{"t":"erase","mac":HMAC(S, "E" ‖ nc)}`, after which the companion deletes
   its pairing and credentials.
3. Companion: `{"t":"proof","mac":HMAC(S, "C" ‖ nc ‖ ns)}`.
4. Controller: `{"t":"ready","mac":HMAC(S, "F" ‖ nc ‖ ns)}`; the companion
   checks it before trusting anything.

HMAC is HMAC-SHA256 (32 bytes, base64), `nc` and `ns` the raw companion and
controller nonces. The session key is
`K = HKDF-SHA256(ikm = S, salt = nc ‖ ns, info = "obbw-link-v1", L = 32)`.
Any failure closes the socket; the controller logs one rate-limited line.

### Signed frames

After `ready`, every frame is `<mac>.<json>`: `mac` is the first 16 bytes of
`HMAC-SHA256(K, dir ‖ json)` as 32 lowercase hex digits, `dir` the single byte
`c` (companion to controller) or `f` (controller to companion), and `json`
carries `"q"`, a sequence number that starts at 1 in each direction and grows
by exactly one. A bad MAC, a wrong `q` or a known `t` with malformed fields
closes the link. A frame with a `t` the receiver does not know is ignored (it
still takes its `q`), so a later version can add messages without breaking an
older peer; the controller counts them in one rate-limited Web UI log line.

### Controller to companion

| Frame | Meaning |
| --- | --- |
| `{"t":"cfg","lm":true,"conn":"ws","observe":true,"ui":true,"log":true,"archive":true,"bc":"gaggimate"}` | Switches and the La Marzocco options that apply (`conn` `ws` or `api`); builds without La Marzocco send only `{"t":"cfg","ui":true,"log":true,"archive":true,"bc":"off"}`. `bc` is the [Beanconqueror](schemas/README.md#beanconqueror-machine-endpoints-v1) format the companion serves from its archive: **Settings → Coffee apps**' choice (`gaggiuino` or `gaggimate`) while the archive and Beanconqueror switches are on, otherwise `off`; the companion keeps it, so it answers with the controller off, and never asks the controller for those answers. A companion that does not know `bc` serves nothing. Sent after `ready` and on every change, a new format included |
| `{"t":"dev","name":"Kitchen","id":"AA:BB:CC:DD:EE:FF"}` | The controller's device name, its identity on the local network (`<name>.local`), so the companion can label it instead of by address, and its stable device id (the webhook's `deviceId`), the log store's key. Up to 32 bytes of letters, digits, spaces and hyphens, empty while no name is published yet. Sent first after `ready` and again whenever the name changes |
| `{"t":"creds","gen":3,"iv":"…","ct":"…"}` | La Marzocco account, encrypted (below). `{"t":"creds","gen":4,"erase":true}` deletes it |
| `{"t":"route","to":"companion"}` | Start sending machine state; `"direct"` stops it |
| `{"t":"hold","on":true,"sync":true}` | `on`: a shot with a scale, a scale connection, scale setup or the scale's quiet interval is in progress; hold `read` and `push` frames until `on` is `false` (Linea Micra builds; always `false` elsewhere). `sync`: a [sync hold](#sync-holds) is active. Sent whenever `sync` changes, on every build. As the answer to a `ping` it reads `{"t":"hold","on":false,"sync":false,"sh":E1,"hi":E2}`, with both store epochs (below) |
| `{"t":"cmd","id":7,"op":"temp","v":935}` | Command: `temp` (tenths of °C), `standby`, `on`, or `refresh` (one read) |
| `{"t":"log","b":42,"e":[[seq,atMs,wallSec,lvl,cat,"message"],…]}` | Diagnostic log events (below), the formatted text the Web UI log view renders, `lvl` one of `c/e/w/i/d` |
| `{"t":"rs","sh":E1,"hi":E2}` | The shot and activation store epochs, once a change to either is saved ([archive sync](#archive-sync)) |
| `{"t":"ev","topic":"shot_result","key":"42-1003","ttl":3600,"d":{…}}` | An event for the devices that asked for its topic ([events](#events)) |
| `{"t":"rec","f":{…}}` | A record answer: `f` is a record index (`records`) or a ui-stream `history` or `stats` frame ([archive sync](#archive-sync)) |
| `{"t":"trc","id":12,"b":43,"e":918204,"r":"ok","d":"…"}` | One shot's trace, or why there is none ([archive sync](#archive-sync)) |

After `ready` the controller sends `dev`, `cfg`, `route` and `hold` with their
current values, and `creds` when it has an account generation; afterwards each
one again only when it changes. `hold` goes whenever the link is open: when the
La Marzocco hold changes while the route is `pending` or `companion` (each
request for the `companion` route carries the current `hold`), and on every
build whenever a sync hold starts or ends, whatever the route. Those two `hold`
frames are the only link frames a shot causes. While the log or archive switch
is on, the controller also answers each `ping` with
`{"t":"hold","on":…,"sync":false,"sh":E1,"hi":E2}`, the store epochs the
replica checks itself against, but only when no sync hold is active: it
leaves in the same segment as the ping's ACK, and during a hold no ping
arrives. A command id is
sent once: a command that was in flight when a link closed is not repeated on
the next link (the controller has already counted it as failed). A frame the
controller's socket was not ready to take is sent again a moment later, in
order.

The credentials are AES-256-GCM with
`key = HKDF-SHA256(ikm = S, salt = "", info = "obbw-creds-v1", L = 32)`,
a random 12-byte `iv`, additional data `"obbw-creds|" + gen` and plaintext
`{"email":"…","password":"…","serial":"…"}`. The companion keeps them in its
secrets directory and never in logs.

### Companion to controller

| Frame | Meaning |
| --- | --- |
| `{"t":"ping","up":true}` | Liveness; `up` is true while its La Marzocco subscription is open and alive. Every 15 s while the route is `companion`, every 120 s otherwise; none during a sync hold, and one at once when it ends |
| `{"t":"lm","sub":true,"ep":12,"tok":2400,"err":"auth"}` | Its La Marzocco state: subscribed, its subscription epoch, seconds until its next token renewal, and `"err":"auth"` while the cloud refuses its dashboard reads or commands (401/403) with the subscription still open. `"policy":true` joins `err` only while that refusal is La Marzocco's pending-policy 403 (`USER_MUST_ACCEPT_POLICY`); the controller then reports `policy_required` and clears it when a frame without `policy` follows. Sent on every change and after `cfg` |
| `{"t":"creds","gen":3,"ok":true}` | Credentials stored and signed in; `"ok":false` when La Marzocco rejected them |
| `{"t":"st","k":"read","ep":12,"ac":7,"d":{…}}` | Machine state (below); a `push` has no `ac` |
| `{"t":"res","id":7,"r":"accepted"}` | Command result: `accepted` or `failed` with `"http":<status>` (0 for a transport failure) |
| `{"t":"rx","s":"shots","ep":12,"d":1}` | A store's record index (`s` `shots` or `history`); `ep` is the epoch the companion holds (0: none). Optional `d` (only `1`) accepts a delta against that epoch (below). The session's first `rx` subscribes it to `rs` |
| `{"t":"rf","s":"shots","o":0,"n":50,"r":7}` | A fetch window: offset `o` 0–1000, `n` 1–50 shots or 1–120 activations, `r` echoed as the page's `request` |
| `{"t":"rt","id":12,"b":43,"e":918204}` | One shot's trace by its Stats identity (`id` ≥ 1, `bootId`, `endedAtMs`) |
| `{"t":"ps","on":true}` | Whether any device wants events ([events](#events)) |
| `{"t":"logsub","on":true,"boot":42,"after":1234}` | The diagnostic log subscription: the boot the companion stored last and its last stored sequence; the controller maps another boot to `after` 0. Sent once per session, after `cfg.log` is true and `dev` named the controller (so the cursor is that controller's), and again from the same cursor when the companion could not store a frame; `on:false` asks the controller to stop the stream (the switch itself is `cfg`'s to say) |

Builds without La Marzocco know only `ping` (its `up` is ignored there),
`ps`, `logsub`, `rx`, `rf` and `rt`; the other four are types they do not know and
ignore. A malformed `rx`, `rf` or `rt` closes the link like any known frame.

The controller closes a link that sends nothing for two ping intervals: 35
seconds while the La Marzocco route is `companion`, 250 seconds otherwise. A
route request (`pending`) keeps the long limit, since the companion may still
be on its 120 s cadence; the request itself fails after 30 s without a ping or
the `full` frame. Liveness is one-sided: the controller sends nothing
periodically, so the companion notices a half-open link when one of its own
pings goes unacknowledged. During a [sync hold](#sync-holds) both sides trust
the link: the companion does not ping, and neither the controller's close
timer nor the La Marzocco route's 30 s ping limit counts; both count again from
the hold's end, when the companion pings at once, so only a missing answer to
that ping finds a link that dropped during the shot. The one ping a hold
does not stop follows a controller announcement or relay handshake, since a
controller that restarted mid-shot never sends the `hold` that ends it.
Neither side uses TCP keepalive on the link socket.

### Machine state

`d` is La Marzocco's own dashboard object reduced to what the controller
decodes, so the controller runs the same decoder as for a direct connection:

- `connected`;
- in `widgets`, only `CMMachineStatus` (`output.mode`), `CMCoffeeBoiler`
  (`output.status`, `targetTemperature`, `readyStartTime`),
  `CMSteamBoilerLevel` (`output.status`, `targetLevel`, `readyStartTime`) and,
  in pushes, `CMBackFlush` (`output.status`), each with only those keys;
- in `removedWidgets`, only those four codes.

The companion forwards every La Marzocco event that carries any of these
fields, whether or not they changed, and drops the others. `k` says where the
state came from:

- `full`: the read after a new La Marzocco subscription. Sent once after
  `route` and again whenever `ep` changes, also while a hold is on: a missing
  one is what the controller waits for on its route timer. The controller
  treats it as a new subscription plus its initial read.
- `push`: a La Marzocco WebSocket push. Held while `hold` is on — a shot with
  a scale hears nothing of the machine — so only the latest held push and
  `read` go, each in the order it was last observed, when the hold ends; a
  held frame from an older subscription (`ep`) is dropped.
- `read`: the companion's own read (after a token renewal, a `refresh`, a
  command's verification, or API-mode polling every 30 s). Held while `hold`
  is on and sent once when it ends; the controller drops a `read` that arrives
  while it would hold one.

In `full` and `read` frames, `ac` is the id of the last command the companion
had sent to La Marzocco when it took the observation (0 for none). The
controller uses it, where a direct read uses its request time, so an older
observation never undoes a newer power command. A `push` carries no `ac`: like
a push on the controller's own socket, it is ordered by the moment it arrives.

A `push` the controller cannot decode closes the link, as it fails the
controller's own socket. A `full` or `read` frame it cannot decode is logged
and ignored, as a failed direct read is; a route request then fails if no
valid `full` frame follows.

### Diagnostic log stream

With **Diagnostic log through the companion** on, the companion subscribes
once `dev` named the controller, with the boot it stored last and that
boot's last sequence, and the controller streams the ring's formatted events
back, one `log` frame per send pass while a backlog drains. `b` is the
controller's current boot id; the companion numbers boots in the order they
arrive, so a repeated id never merges two boots, and boot separators and
reset reasons are the ring's own `BOOT_BANNER` and reset-reason events.

- **Radio silence during a shot is absolute.** No `log` frame is transmitted
  during a [sync hold](#sync-holds). Events queue (they stay in the ring, the
  cursor stays) and are sent in order when the hold ends. Only a hold long enough for the
  512-event ring to wrap loses events, and those are reported as a `miss`.
- **Plain events ride the heartbeat.** Outside a hold, `info` and `debug`
  events flush only when a frame arrived since the last flush — the
  companion's 15/120 s ping — so the log adds no radio wakeup of its own.
  `warning` and noisier events flush at once outside a hold.
- **Gaps are explicit.** `"miss":N` in a frame says the ring overwrote `N`
  events the companion had not read (the companion stores the gap); a
  `"reset":true` frame says the companion's cursor is ahead of this boot's
  ring, so the frame refills from the oldest retained event. The ring is
  never cleared while running, so this happens only when a boot id repeats
  (one the controller could not save, or a counter a full flash erase
  restarted): the companion stores it as a new boot and keeps every row.
- The switch off stops the frames at the next `cfg`; the companion keeps
  what it stored. A never-paired controller, or one whose companion is away,
  runs none of this: the producer wakes only for an authenticated,
  subscribed link.

### Archive sync

With **Shot archive in the companion** on, the companion keeps a copy of
every shot and activation through the same records the Web UI shows. The
controller keeps no state for it beyond the open link, the index each store
last sent on it (in PSRAM, for deltas) and the user deletion marks it saves
with each store anyway, so syncing writes nothing to flash; the companion
infers ratings, deletions and restarted histories from the index.

- **Requests.** `rx`, `rf` and `rt` each replace the pending request of
  their kind. The controller answers one frame per pass, paced like a Stats
  page, and keeps a request through a hold, a busy workspace or a socket that
  cannot take a frame yet, never dropping it. A hold that begins after a
  frame was scheduled still stops it: the controller checks again just
  before sealing, and drops a queued `log`, `rs` or ping answer (rewinding
  the log cursor), sending it after the hold.
- **`rec` answers.** `f` is a ui-stream frame byte for byte: a Stats or
  History page as the [Integration API](INTEGRATION_API.md) documents it, or
  the record index below. Fetch windows are newest first by date and
  unfiltered (every shot with `badReason`, every activation type), and Stats
  pages carry no `stats` header member. A Stats page's first frame carries
  `good`, as for the browser. History and index frames reach about 21 KiB,
  which is why record answers may exceed 4,096 bytes.
- **Record index.** An `rx` answers one frame: `v: 1`, `type: "records"`,
  `boot`, `store` (`"shots"` or `"history"`), `epoch`, and `gen` and `next`,
  the store header's write counter and next record id (`gen` only grows;
  `next` returns to 1 on a Clear or a factory reset, so a decrease tells the
  companion its history restarted), then `clr` and `gone` (below). Only when `epoch`
  differs from the `ep` asked does the frame add `rows`, a flat array of
  `[id, revision]` pairs for every record, bad shots included, newest first
  (ids descending). The revision is the low 24 bits of a CRC over the stored
  record, so a rating change, or an id reused after a clear, reads as a
  different record. Store epochs survive restarts, so an unchanged store
  costs one small frame; 1000 History pairs stay under the 20 KiB frame
  budget. Every frame that lists the index also carries `sum`, the CRC-32
  (the zlib one, as Python's `zlib.crc32`) of the whole resulting index
  serialized as little-endian `u32 id, u32 revision` pairs in index order;
  an unchanged answer carries neither `rows` nor `sum`.
- **User deletions.** Every index frame, whole, delta or unchanged, carries
  `clr`, how many times the user cleared that store (Clear in the Web UI or
  USB `CLEAR_SHOTS`), and `gone`, the ids the user deleted since the store
  was last emptied, newest first, at most 16. The controller keeps both with
  the store, in the same save as its records: `clr` only grows, a factory
  reset never raises it (and keeps it), and both read as `0` and `[]` after a
  store written by an older image. `clr` above the value the companion stored
  is a user Clear: it deletes its copy of every history of that store and
  starts a new one; a lower value only restarts the count (rolled-back
  firmware, a store the controller had to reset) and deletes nothing. An id
  in `gone` that the index no longer lists is deleted from the companion's
  current history even below the controller's oldest id; an id at or above
  the oldest that the index no longer lists is a deletion too, and one below
  it that `gone` does not name rotated out and stays. The companion treats
  an index without `gen`, `next`, `clr` or `gone` as malformed. After more
  than 16 deletions below the oldest
  while the companion is away, the earlier ones read as rotation.

  ```json
  {"v":1,"type":"records","boot":42,"store":"shots","epoch":1239999,"gen":7,"next":1003,"clr":2,"gone":[3],"base":1234567,"up":[1002,789],"del":[3],"sum":2864434397}
  ```

- **Index deltas.** The companion sends `d:1` only after an index answer in
  the same link session listed the index. The controller keeps, per store, the index this link session last
  received (its baseline, recorded once the frame actually left, in PSRAM).
  With `d`, an `rx` whose `ep` is that baseline's epoch gets the difference
  instead of `rows` whenever that is smaller than the whole index:

  `base` is the epoch the delta applies to (the `ep` asked), `up` the
  `[id, revision]` pairs that are new or changed, and `del` the ids gone.
  The companion applies it to its own copy of that index (kept in memory,
  never on disk): it drops `del`, upserts `up`, keeps the result newest
  first, checks `sum`, and on any mismatch, or a `base` it does not hold,
  asks `ep` 0 without `d`, which returns the whole index. Any other case (no
  baseline, another epoch, a new link session, a controller or companion
  restart, no memory for baselines, or a delta not smaller) answers with the
  whole index, so `d` is a hint, never a requirement.
- **`trc` answers.** `r` is `ok` (with `d`, the trace section the
  [trace route](INTEGRATION_API.md#get-apiv1diagnosticshot-trace) sends, base64),
  `none` (no summary with that identity, no trace, or the record was
  recycled), `busy`, `saving` or `error`. Traces of shots the log refused
  (`id` 0) are not archived.
- **`rs` notices.** After the session's first `rx`, the controller sends
  `rs` when either store epoch moved from the one the companion last learned
  (from an `rs` or an index answer) and the change is saved: the records, the
  curves and, for a shot, its trace. Never during a sync hold. A change that
  came across a hold (a shot) goes the moment it is saved after the hold, so
  the companion's catch-up never falls into the next shot; any other change
  (a rating, a deletion, a Clear from any screen, Home Assistant or USB)
  waits one second for the ones that follow it. It does not wait for a ping.
  There is no `rs` at the session start, since each `rx` answer already names
  its store's epoch, and each ping answer names both epochs again.

### Events

The controller decides which events exist and when they fire, and the Web UI's
service worker turns them into text. The companion only delivers: it passes `d`
on unread, and holds no notification wording and no shot rule.

- **`ps`.** The companion sends `on:true` when a device is subscribed: on each
  session's `cfg`, and when the first device arrives. `on:false` follows the last
  device. No `ps` in a session means none wanted, so the controller then sends no
  `ev` and does not read the shot store for it. The first `on:true` of a boot arms the
  events: shots that ended before it never notify. `ps` is a sync request: it waits
  for the end of a sync hold.
- **`ev`.** `topic` and `key` are opaque strings (at most 32 and 64 bytes), `ttl` is
  how long a push service may hold the message, in seconds, and `d` is an object of at
  most 1 KiB. A malformed `ev` closes the link. `ev` is a sync body: it never crosses
  during a hold, and leaves in the pass that finds the store saved after one.
- **`shot_result`.** `d` is
  `{"v":1,"id":1003,"b":42,"e":918204,"at":1762268000,"s":28.4,"r":"fast","g":36.20,"w":"m"}`:
  the shot's Stats identity (`id`, `bootId`, `endedAtMs`), `at` its end in Unix seconds
  (0 without a clock), `s` its duration, `r` its result (`on_recipe`, `fast`, `slow` or
  null) and `g` its yield, measured (`w` `m`) or estimated after a scale loss (`w` `e`).
  Only the newest shot is looked at. It fires for a shot of this boot that ended since
  the last one looked at and at most 10 minutes ago. A rating, a deletion or a Clear
  never fires. A short shot, or one with no yield and no estimate, does not fire: the
  Web UI log and `pushSkip` say so. An event not sent before a restart is lost.
- **`micra_policy`.** `d` is `{"v":1}`, `key` is always `micra-policy` and `ttl` is
  86400. It fires when the controller's La Marzocco error is `policy_required`, whether
  it met the 403 itself or the companion reported it, and a session has asked for events.
  It is a state, so a session or a `ps` that arrives later still hears of it. It fires
  once until a machine sample newer than the event arrives with the error gone; other
  errors in between are the same pending acceptance. The Web UI log records it. A restart
  forgets that it was told, so a controller that restarts with the terms pending sends it
  again. Devices choose it with their own `topics`, so the companion needs no change.

### Sync holds

Nothing crosses the link in either direction while a sync hold is active: a
shot with a connected scale, a scale connection or discovery, the BLE quiet
interval, or any state that closes the flash-write gate (a brew cycle and its
drip delay, a rinse, a closed relay, a running machine, the physical
activator on, a maintenance lease, a scale connecting). The controller sends
the `hold` frame that starts it (`sync` true) and the one that ends it, and
nothing in between on its own: no `log`, `rs`, `rec` or `trc`, and no ping
answer. The companion sends no ping, `logsub`, `rx`, `rf` or `rt`; it keeps
them armed and sends them when the `hold` with `sync` false arrives, together
with a ping. A request that still arrives during a hold is answered after it.
What a person does during a shot (opening a page, saving a setting, any
command) still reaches the controller as always; the La Marzocco `st` frames
follow their own `hold` rule above.

### Replica serving

With **Shot archive in the companion** on, the companion's copy is a replica
it serves Shots and History from, so a screen opened through the companion
costs the controller nothing for its records:

- **Current.** A store is current once a sync pass ended at the epoch the
  controller last named (`rs`, a ping answer, an index answer) with nothing
  changed since. A notice of another epoch starts a catch-up at once: `rx`
  (a delta when it can), then `rf` for the changed records and `rt` for new
  traces, back to back, paused only by a sync hold. A rating, deletion or
  Clear that a companion screen sends through the relay makes its store not
  current as it leaves, and starts the catch-up once the controller answered.
- **Pages.** Every History and Stats subscribe, fetch and export window is
  answered by the companion from the whole history: the controller's records
  and the ones only the companion keeps, filtered, ordered and sliced as the
  controller does (`total`, `hasMore`, `good`, the Stats header over the
  whole history for the active preset from Home's model). Its frames match
  the controller's for the same rows member for member (held to the shared
  golden vectors in `companion-records-v1.vectors.json`, its `page_*` cases)
  except the page identities `tag`, `seq` and `request`, which the companion
  assigns itself. A row only the companion keeps carries `archiveOnly`,
  `archiveGeneration` and, for a shot, `archiveTrace` (a Stats row in its
  last object, beside the curve), and that page's first frame adds
  `archive: {generations, holes}` for the lines the list draws between them.
  Offsets may go past 1,000 for the whole history.
- **Waiting and falling back.** A page of a store that is not current waits
  for its catch-up; only when it cannot finish (the link is down, or about
  3 seconds pass without an answer) does that page come whole from the
  controller, never mixed. During a sync hold the replica serves as it is and
  nothing goes to the controller; open pages refresh once the catch-up after
  the hold has ended.
- **Traces.** The relay answers `GET /api/v1/diagnostic/shot-trace` from the
  replica when it holds that trace, and forwards it otherwise.
- **Switch off.** Every page takes the controller's path, as without a
  companion.

### Route and failover

The controller alone decides the route. It asks for `companion` only at a
quiet point (no shot, scale connection, backflush or machine command in
progress) once the companion has confirmed the current credentials and reports
`sub` with `up`; while the companion reports `err:"auth"` it stays on its
direct connection instead, without counting that as a lost route. It switches
its source when the `full` frame arrives and then
closes its own La Marzocco connection. On link loss, a missing ping, `up`
false for 30 s, or the switch turned off, it returns to its direct connection
under the same rules as any reconnection (never during a shot with a scale, a
scale connection or the BLE quiet interval). Two La Marzocco sessions for a
moment are harmless: the companion has its own installation. Commands go only
through the route active when they are sent.

## Relay

With **Web UI through the companion** on, the companion serves the Web UI to
any number of screens while the controller keeps one claim and one ui-stream
session for the companion, apart from those of a browser at its own address. The
companion forwards each screen's REST
calls and its own ui-stream upgrade with a signed header. The relay works
without the link, uses no clock and writes nothing to flash. A request without
the header takes exactly the path it takes without a companion.

The source address of a request is never trusted: a Docker host can run the
companion and Home Assistant behind one address. Only a valid signature makes
a request relayed; the local-peer rule still applies on top of it.

### Handshake

`GET /api/v1/companion/relay` answers
`{"challenge":"<16 bytes>","protocol":1}`, or `404 COMPANION_NOT_PAIRED`.
The challenge `rb` is random per controller start and changes only after a
successful `POST`, so a device that reads it cannot replace it.

`POST /api/v1/companion/relay` (`Content-Type: application/json`, `415`
otherwise) with `{"id":"…","nonce":"<16 bytes>","mac":"<32 bytes>"}`, where
`mac = HMAC(S, "R" ‖ rb ‖ nc)` and `nc` is the companion's fresh nonce:

| Status | Body | When |
| --- | --- | --- |
| `200` | `{"mac":"…","ui":true,"link":false}` | `mac = HMAC(S, "Q" ‖ rb ‖ nc)` (the companion checks it); `ui` the Web UI switch; `link` whether a feature needs the link |
| `400` | `INVALID_REQUEST` | Malformed fields |
| `401` | `COMPANION_AUTH` | Wrong `mac` (counted in `authFailures`, one rate-limited log line) |
| `404` | `COMPANION_NOT_PAIRED`, plus `"erase":"<HMAC(S, "E" ‖ nc)>"` for a forgotten companion | Unknown or forgotten `id`, decided before `mac` is checked: a companion whose `GET` was answered `404` sends any `mac` (for example over 16 zero bytes as `rb`) to learn whether to erase |

On success both sides derive
`Kr = HKDF-SHA256(ikm = S, salt = rb ‖ nc, info = "obbw-relay-v1", L = 32)`
for signing, and from it the sealing key
`Kb = HKDF-SHA256(ikm = Kr, salt = "", info = "obbw-relay-body-v2", L = 32)`
for every sealed body and header value below. The controller resets the
replay window and draws a new `rb`; the response counter never resets
within a boot. A replayed
handshake fails because `rb` changed. Forget and a factory reset end the
relay session, and so does a change of either switch in Admin → Companion:
the companion's next relayed request is `RELAY_STALE`, and its new handshake
carries the new `ui` and `link`. A missing `ui` reads as `true` and a missing
`link` as `false`. Forget and turning **Web UI through the companion** off also
close the relayed ui-stream, without a close frame, and end its claim.

The relay does not run over the SoftAP: a handshake or relayed request on a
socket the SoftAP serves answers `403 COMPANION_PEER`, because the trust given to
whoever joined the SoftAP must not reach every screen of a companion.

### Signed requests

```text
X-OBBW-Relay: 2 <q> <addr> <screen> <mac>
```

Relay protocol 2; the header's first field is the version, and a request
that names one other than 2 or [3](#relay-features) is `400 RELAY_INVALID`. The
fields are as before:

- `q`: the request counter of this relay session, from 1, decimal without
  leading zeros, at most 4294967295; the companion makes a new handshake
  before it would wrap;
- `addr`: the browser's address as the companion's gateway saw it (IPv4 or
  IPv6 text, at most 45 characters);
- `screen`: the browser's own `X-WebUI-Client` (16 to 24 lowercase hex
  digits);
- `mac`: the first 16 bytes of `HMAC-SHA256(Kr, canonical)` as 32 lowercase
  hex digits, where `canonical` is
  `obbw-relay-v2\nMETHOD\nURI\nq\naddr\nscreen\nsession\n`: the HTTP method,
  the request target exactly as sent (path and query), and the sealed
  `X-WebUI-Session` value as sent, or nothing. The signature still covers
  the request's identity; the seal below covers its content.

The controller accepts `q` above the highest seen, or one of the 63
below it not seen yet. The companion rewrites `X-WebUI-Client` to its own
random relay id (24 hex digits, one per companion process) and forwards
`X-WebUI-Activity`, `Content-Type`, `Content-Length` and, for firmware
updates, `X-OTA-Transfer`, `X-OTA-Offset`, `X-OTA-Length`,
`X-OTA-Allow-Downgrade` and `Content-Range` as they are. Its shared
ui-stream upgrade (`GET /api/v1/ui/ui-stream`) carries the header too, with
`addr` `0.0.0.0`, the relay id as `screen`, no session, and the controller's
own `Host` and `Origin` (`http://<host>`); that identity names no screen and
holds nothing. Its gateway relays `POST /api/v1/companion/admin` (Admin →
Companion on a companion screen, with that screen's Admin unlock) and no
other `/api/v1/companion/` route.

### Sealed bodies and headers (protocol 2)

Everything that carries a credential is sealed with AES-256-GCM under `Kb`:
no device password, session token, or sign-in body crosses the
companion→controller hop in the clear, and a seal cannot be moved between
requests.

**Nonce.** 12 bytes: the direction byte `c` (companion to controller) or
`f` (controller to companion), one context byte, two zero bytes, and the
counter as 8 bytes little-endian:

| Context | Counter | Covers |
| --- | --- | --- |
| `0` | the request's `q` | the `X-WebUI-Session` value |
| `1` | the request's `q` | the `X-Device-Password` value |
| `2` | the request's `q` | the request body |
| `3` | the response counter | a response body |

A nonce is never reused under one `Kb`: `q` is fresh by the replay window,
and the response counter grows by exactly one per sealed response without
ever repeating within a boot, so no nonce repeats under any `Kb`. A
request's seals and its answer use the `Kb` of the relay session that
admitted it, even if a newer handshake has happened by the time the answer
leaves. No random bytes and no stored state are needed.

**Sealed header values.** `X-WebUI-Session` and `X-Device-Password` keep
their names but carry standard padded base64 of the ciphertext (the 16-byte
GCM tag appended), with no additional data. Moving a sealed value to another
request fails: the nonce names that request's `q`, and the signature binds
the session value to the request it traveled on.

**Sealed request bodies.** A relayed request with a body carries the raw
ciphertext (tag appended, no base64); `Content-Length` includes the 16 tag
bytes. The controller opens the body where it reads it, before any parsing.
The exception is the firmware update range upload, `PATCH /api/v1/ota`,
whose body travels as it is (see below).

**Sealed response bodies.** Every whole JSON response to a relayed request is
sealed the same way and carries `X-OBBW-Enc: <counter>` (decimal, the
response counter used as the nonce). A response without the header — web
assets, WebSocket upgrades, JSON streamed in chunks, a route's own refusals
sent before admission — is not sealed. The companion refuses whole JSON that
claims success without a seal, and a sealed answer it has already opened under
this `Kb`.

**What is deliberately not sealed.** OTA PATCH range bodies and web assets
carry no secret: they are the user's own firmware image and the Web UI's
public files. Their integrity does not rest on the seal — the image is
pinned by the session's `sha256`, which arrives in a sealed
`/api/v1/ota/session` body and is verified over the assembled image before
anything is committed, and assets are pinned by the sealed list below. A build
tag in a URL names a file; it proves nothing about the bytes.

### Relay features

`GET /api/v1/integration/info` carries `"relayFeatures":1` on a controller that
has what this section describes. Read through the relay it arrives sealed, so a
device on the network cannot lower it. A whole answer with exactly info's keys
and no such field is an older controller, and the only case a companion treats
as one: anything else it cannot read is a refusal.

**Bound answers (relay version 3).** The header starts with `3` and its signature
uses `obbw-relay-v3` as the canonical first line, so rewriting a header to `2`
fails. The sealed answer then has additional data `obbw-resp-v3\n<q>\n<status>`,
the status being the line's three digits. An answer held back from one request
therefore opens for no other: without this, someone in the middle could deliver
a victim's sign-in answer for their own request. Version 2 stays accepted; the
companion moves to 3 for a relay session once it read the feature.

**The asset list.** `GET /api/v1/companion/assets`, relayed only (`404` otherwise),
answers with what each file must be, generated with them, so it costs flash and
no hashing on the controller:

```json
{"v":1,"tag":"89abcdef","etag":"\"1.2.0.89abcdef\"","csp":"…",
 "shell":["<sha256 base64>",96000],"shellPaths":["/","/stats"],
 "assets":[["/app.js","<sha256 base64>",1234,"text/javascript; charset=utf-8",true]]}
```

Digests cover the exact bytes sent, gzip included. A path ending in `*` is a
prefix. List and files share one image and a relay session ends with the boot,
so an answer opened under the current session always describes the running image;
after an OTA or a rollback the companion reads it again.
The ui-stream and link frames after their handshakes carry no credentials.

A request with the header is refused before any handler runs; after
`RELAY_STALE` alone the companion makes a new handshake and retries once:

| Status | Code | When |
| --- | --- | --- |
| `403` | `COMPANION_PEER` | The companion's socket is outside the local ranges |
| `400` | `RELAY_INVALID` | The header is malformed, names another version, or a seal does not open |
| `401` | `RELAY_STALE` | No relay session, or the signature does not match it |
| `401` | `RELAY_REPLAY` | `q` was used or is too old |
| `403` | `RELAY_DISABLED` | **Web UI through the companion** is off |

Refusals count in `relayRejects` and share one rate-limited log line.

### What a relayed request changes

Any route a browser may call works through the relay, with the same sessions,
claim and safety gates. The signature adds only this:

- **Claim attach.** `POST /api/v1/ui/claim` from a companion screen while the
  relay id already holds control joins that claim: it validates the
  forwarded session, keeps the other screens' Admin unlock and override, and
  drops only the ones this screen held.
- **One claim per channel.** A relayed claim and a direct claim never displace
  each other, nor clear what the other channel holds. A new direct window displaces
  only the previous direct window. The first of three ui-stream slots can only be
  the companion's, so a direct browser can never occupy it, and each dispatch serves
  the companion first, sharing one 800 ms budget. `409 UI_TAKEN_OVER` and close
  code 4001 reach the companion only from firmware older than this, or after every
  session is revoked.
- **Before pairing nothing is relayed.** A companion that requires pairing, shown
  by `pairingRequired` in `/__companion/status`, answers `/api/` with
  `409 COMPANION_NOT_PAIRED` and redirects pages to `/__companion/?pair=1`, where
  the Web UI also sends a window that is already open.
- **A window from an older, proxying companion keeps its place.** The companion
  closes its stream with `4001` and the reason `paired`, the window claims again
  without a notice, and the first relayed request or claim naming the same id
  moves its Admin unlock and unsafe override onto that screen. They move only this
  way: a direct request never takes a screen's unlock.
- **Admin unlock and the unsafe override belong to the screen.** Unlocking
  Admin, signing in, the unsafe override, Admin lock and the OTA password
  check act on the `screen` of the request, not on the relay id. One screen or
  window holds Admin at a time, across both channels: unlocking on another
  replaces it, so the controller's own address can always reach Admin when the
  companion cannot.
  `GET /api/v1/auth` reports `adminUnlocked` for that `screen` too: the
  companion asks it signing the requesting browser's own `X-WebUI-Client` as
  `screen` before deleting a record or the log history it alone keeps, and
  treats an answer without a relay session as locked.
- **Sign-in and sign-out.** Signing in through the relay claims for the relay
  id (attaching like a claim) and unlocks Admin for that screen. Signing out
  ends that screen's session and its Admin unlock; the companion keeps
  control for its other screens.
- **Password attempts** count against `addr`, the browser, not the companion.
- **Home** on the relayed ui-stream carries `relay.adminScreen` and
  `relay.overrideScreen`: the screen holding Admin or the override as 8 hex
  digits of FNV-1a over its `X-WebUI-Client`, or `""`. `adminUnlocked` and
  `webUiOverrideActive` on that stream stay `false` for a screen's unlock or
  override, so each screen compares the tag with its own id.
- **Per-session fields.** The companion hands later screens its copy of
  Home, Settings and Diagnostic, so a field the controller computes for one
  stream session would reach them stale. Each one is listed in
  `src/tests/fixtures/companion_replay_v1.json` with how the companion
  treats it: today only Home's `timeUtcSec`, a `clock` that the controller
  sends only when the session opens or its clock resynchronizes, and that
  the companion moves on by the seconds its copy has waited. The web
  contract `27-companion-replay` fails when a sender reads session state
  for a field the fixture does not list; the companion's tests hold its
  code to a copy of the file. SHA-256 of the current file:
  `10c1609c9b253db1750db426e76d923a42acb5d65aa0ba865ff5d1061b26d076`.

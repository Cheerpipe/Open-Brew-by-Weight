# Companion link protocol

This is the developer reference for the LAN contract between the controller
and [Open Brew by Weight Companion](features/companion.md), version 1. The
companion is optional: a controller that never paired, or whose companion is
away, behaves exactly as it does without this feature. Everything here is
opened by the companion; the controller never connects to the companion,
never browses mDNS, and never retries.

Version 1 covers pairing, the [relay](#relay) that lets the companion serve
the Web UI to several screens, and, on Linea Micra builds, the La Marzocco
route. Every build registers these routes; a controller that is not paired
answers them and does nothing else.

## Common rules

- Routes answer only on the controller's own `Host` names, like every other
  route (`403 HOST_NOT_ALLOWED`).
- They are exempt from the Web UI login: pairing is approved in Admin, and the
  link authenticates with the pairing secret, not with a Web UI session.
- Requests from a peer outside the private and link-local ranges (RFC 1918,
  169.254/16, loopback) answer `403 COMPANION_PEER`. The controller records
  the peer address from the socket; an address in a request body is never
  trusted.
- Errors use `{"error":"STABLE_CODE","message":"English diagnostic"}`.
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
of the companion's long-lived pairing key. The key itself is revealed only
after the controller's key is known (below).

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
| `{"action":"approve","code":"123456","lm":true}` | Approves the open request; `lm` (optional, `true` when absent) is the La Marzocco switch chosen in the approval panel. `422 COMPANION_CODE` for a wrong code (the third one denies the request), `409 PAIRING_NOT_READY` before the controller's key exists |
| `{"action":"ignore"}` | Denies the open request and ignores that `id` until **Stop ignoring** (the four most recent ids) |
| `{"action":"unignore"}` | Forgets the ignored ids |
| `{"action":"forget"}` | Removes the pairing and closes the link; the next link attempt receives `erase` |
| `{"action":"switches","ui":false,"lm":true}` | Sets the switches present in the body: `ui` is **Web UI through the companion** (on when paired), `lm` the La Marzocco switch (Linea Micra builds only; `400 INVALID_REQUEST` elsewhere). An open link receives a new `cfg` |

Every action answers `{"ok":true}`. Approve and Ignore without an open request
answer `409 PAIRING_NOT_PENDING`; Forget and Switches without a pairing answer
`409 COMPANION_NOT_PAIRED`.

Admin reads the pairing, the open request and the link state from
`GET /api/v1/status/admin`:

```json
"companion":{"paired":true,"name":"Kitchen server","address":"192.168.1.20",
 "version":"1.0.0","link":"open","linkAgeS":3600,"peerProtocol":0,
 "ui":true,"relay":"ready","relayRejects":0,
 "route":"companion","lm":true,"upstream":true,"refused":false,"credentials":true,
 "request":null,"ignored":0}
```

`link` is `open` or `down`, `linkAgeS` the seconds since the link last came
up or went down (`-1` when neither happened since the controller started),
`peerProtocol` the protocol of the paired companion's last hello that this
controller refused with `PROTOCOL` (0 when none, or once a link opens), `ui`
the Web UI switch, `relay` `ready` once a [relay handshake](#relay) succeeded
since the controller started (`down` before and after Forget), `relayRejects`
the relayed requests refused since then; on Linea Micra builds `route` is
`direct`, `pending` or `companion`, `refused` mirrors the `lm` frames'
`err:"auth"` while the companion's cloud reads are being refused,
`credentials` tells whether the
companion confirmed the current account (other builds omit `route`, `lm`,
`upstream`, `refused` and `credentials`), and `request` is
`{"name","address","version","expiresInS","codeReady"}` while a request is
open (`codeReady` once the companion revealed its key and the controller's key
exists). Home's stream carries `companion.requestName` and
`companion.requestAddress` (empty strings when no request is open). The
Diagnostic stream carries `companion.paired`, `name`, `address`, `version`,
`protocol` (the open link's, 0 while down), `peerProtocol`, `link`,
`linkAgeS`, `reconnects`, `authFailures` (link and relay handshakes), `ui`,
`relay`, `relayRejects` and, on Linea Micra builds, `route`, `reason` (why the
route is direct; `protocol` while `peerProtocol` is set), `lm`, `upstream`,
`routeSwitches` and `commands`. Names, versions and addresses are stored
without quotes, backslashes or angle brackets; the name and version follow the
companion's authenticated `hello`.

## Link

The companion opens the link only while a feature needs it: the [relay
handshake](#relay) answers `link` (today `true` only on Linea Micra builds with
the La Marzocco switch on). A companion that finds no relay route (an older
controller) opens it as before.

`GET /api/v1/companion/link` upgrades to a WebSocket. Only text frames are
used, each at most 4,096 bytes including a signed frame's MAC prefix. One
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

1. Companion: `{"t":"hello","id":"…","name":"…","version":"…","protocol":1,
   "nonce":"<16 bytes>"}`.
2. Controller: `{"t":"challenge","nonce":"<16 bytes>"}`, or
   `{"t":"error","code":…}` and close: `NOT_PAIRED` (unknown id),
   `PROTOCOL` (unsupported version; Admin and Diagnostic then say that the two
   sides use different link versions), `RETRY_LATER` (a shot with a scale, a
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
| `{"t":"cfg","lm":true,"conn":"ws","observe":true,"ui":true}` | Switches and the La Marzocco options that apply (`conn` `ws` or `api`); builds without La Marzocco send only `{"t":"cfg","ui":true}`. Sent after `ready` and on every change |
| `{"t":"dev","name":"Kitchen"}` | The controller's device name, its identity on the local network (`<name>.local`), so the companion can label it instead of by address. Up to 32 bytes of letters, digits, spaces and hyphens. Sent after `ready` and again whenever the name changes |
| `{"t":"creds","gen":3,"iv":"…","ct":"…"}` | La Marzocco account, encrypted (below). `{"t":"creds","gen":4,"erase":true}` deletes it |
| `{"t":"route","to":"companion"}` | Start sending machine state; `"direct"` stops it |
| `{"t":"hold","on":true}` | A shot with a scale, a scale connection, scale setup or the scale's quiet interval is in progress; hold `read` frames until `on` is `false` |
| `{"t":"cmd","id":7,"op":"temp","v":935}` | Command: `temp` (tenths of °C), `standby`, `on`, or `refresh` (one read) |

After `ready` the controller sends `cfg`, `dev`, `route` and `hold` with their
current values, and `creds` when it has an account generation; afterwards each
one again only when it changes. `hold` changes are sent only while the route is
`pending` or `companion` (the companion forwards nothing otherwise); each
request for the `companion` route carries the current `hold`. A command id is
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
| `{"t":"ping","up":true}` | Liveness; `up` is true while its La Marzocco subscription is open and alive. Every 15 s while the route is `companion`, every 120 s otherwise |
| `{"t":"lm","sub":true,"ep":12,"tok":2400,"err":"auth"}` | Its La Marzocco state: subscribed, its subscription epoch, seconds until its next token renewal, and `"err":"auth"` while the cloud refuses its dashboard reads or commands (401/403) with the subscription still open. Sent on every change and after `cfg` |
| `{"t":"creds","gen":3,"ok":true}` | Credentials stored and signed in; `"ok":false` when La Marzocco rejected them |
| `{"t":"st","k":"read","ep":12,"ac":7,"d":{…}}` | Machine state (below); a `push` has no `ac` |
| `{"t":"res","id":7,"r":"accepted"}` | Command result: `accepted` or `failed` with `"http":<status>` (0 for a transport failure) |

Builds without La Marzocco know only `ping` (its `up` is ignored there); the
other four are types they do not know and ignore.

The controller closes a link that sends nothing for two ping intervals: 35
seconds while the La Marzocco route is `companion`, 250 seconds otherwise. A
route request (`pending`) keeps the long limit, since the companion may still
be on its 120 s cadence; the request itself fails after 30 s without a ping or
the `full` frame. Liveness is one-sided: the controller sends nothing
periodically, so the companion notices a half-open link when one of its own
pings goes unacknowledged.

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
  `route` and again whenever `ep` changes. The controller treats it as a new
  subscription plus its initial read.
- `push`: a La Marzocco WebSocket push, forwarded at once, also during shots.
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
session, held by the companion. The companion forwards each screen's REST
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
`Kr = HKDF-SHA256(ikm = S, salt = rb ‖ nc, info = "obbw-relay-v1", L = 32)`,
the controller resets the replay window and draws a new `rb`. A replayed
handshake fails because `rb` changed. Forget and a factory reset end the
relay session, and so does a change of either switch in Admin → Companion:
the companion's next relayed request is `RELAY_STALE`, and its new handshake
carries the new `ui` and `link`. A missing `ui` reads as `true` and a missing
`link` as `false`.

### Signed requests

```text
X-OBBW-Relay: 1 <q> <addr> <screen> <mac>
```

- `q`: the request counter of this relay session, from 1, decimal without
  leading zeros, at most 4294967295; the companion makes a new handshake
  before it would wrap;
- `addr`: the browser's address as the companion's gateway saw it (IPv4 or
  IPv6 text, at most 45 characters);
- `screen`: the browser's own `X-WebUI-Client` (16 to 24 lowercase hex
  digits);
- `mac`: the first 16 bytes of `HMAC-SHA256(Kr, canonical)` as 32 lowercase
  hex digits, where `canonical` is
  `obbw-relay-v1\nMETHOD\nURI\nq\naddr\nscreen\nsession\n`: the HTTP method,
  the request target exactly as sent (path and query), and the
  `X-WebUI-Session` value or nothing.

The body is not covered: the signature protects what only a relayed request
may do. The controller accepts `q` above the highest seen, or one of the 63
below it not seen yet. The companion rewrites `X-WebUI-Client` to its own
random relay id (24 hex digits, one per companion process) and forwards
`X-WebUI-Session`, `X-WebUI-Activity`, `X-Device-Password`, `Content-Type`,
`Content-Length` and, for firmware updates, `X-OTA-Transfer`, `X-OTA-Offset`,
`X-OTA-Length`, `X-OTA-Allow-Downgrade` and `Content-Range`. Its shared
ui-stream upgrade (`GET /api/v1/ui/ui-stream`) carries the header too, with
`addr` `0.0.0.0`, the relay id as `screen`, no session, and the controller's
own `Host` and `Origin` (`http://<host>`); that identity names no screen and
holds nothing. Its gateway relays `POST /api/v1/companion/admin` (Admin →
Companion on a companion screen, with that screen's Admin unlock) and no
other `/api/v1/companion/` route.

A request with the header is refused before any handler runs; after
`RELAY_STALE` alone the companion makes a new handshake and retries once:

| Status | Code | When |
| --- | --- | --- |
| `403` | `COMPANION_PEER` | The companion's socket is outside the local ranges |
| `400` | `RELAY_INVALID` | The header is malformed |
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
  drops only the ones this screen held. Otherwise the claim takes control as
  usual, so a newly opened companion screen takes control from a browser
  that opened the controller directly, and a direct claim takes it back
  (relayed requests then get `409 UI_TAKEN_OVER` and the companion's stream
  closes with 4001).
- **Admin unlock and the unsafe override belong to the screen.** Unlocking
  Admin, signing in, the unsafe override, Admin lock and the OTA password
  check act on the `screen` of the request, not on the relay id. One screen or
  window holds Admin at a time: unlocking on another replaces it.
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

# Mahlkönig Grind-by-Sync

This is the developer reference for Mahlkönig Grind-by-Sync (GbS): a
Mahlkönig grinder learns from the shots the controller measures and adjusts
its grind for the next one. The user guide is
[Settings → Mahlkönig](settings/mahlkonig.md). The integration is **off by
default** (Admin → Integrations) and **not yet tested with a real grinder**;
every value below that the protocol facts leave open is marked as a
tolerance and is confirmed only by a real-grinder record.

Sources: `src/OpenBrewByWeightMahlkonig.h` (the pure protocol core),
`src/OpenBrewByWeightMahlkonigStore.h` (`gbs_ident`),
`src/network/OpenBrewByWeightMahlkonig.inc` (network task) and
`src/network/OpenBrewByWeightMahlkonigHttp.inc` (httpd). Golden vectors:
`docs/schemas/mahlkonig-gbs-v1.vectors.json` and the `gbs` cases of
`docs/schemas/companion-link-v1.vectors.json`, reproduced byte for byte by
the firmware (`mahlkonig_host_test`) and the companion (`tests/test_gbs.py`).
Protocol knowledge comes from the public `gbs-anywhere` projects (AGPL); no
code, test or fixture was taken from them.

Grinder input is a signal only. The start-brew request (`execute`) never
reaches the relay, a machine command or the control queue; a host test checks
that the GbS sources name no actuation path.

## Models and modes

| Model | Mode | Who asks |
| --- | --- | --- |
| E64 WS | pull | the grinder polls the machine over HTTP, port 80, every 2 s while on |
| E65W, E65T, E80W, E80T | push | the machine side asks the grinder over HTTPS, port 30001 |

`RuntimeConfig.mahlkonigModel` (tag 98) stores the choice (`none`, `e64ws`,
`e65w`, `e65t`, `e80w`, `e80t`); E65 and E80 share one implementation.

## Settings and storage

| Value | Where | Notes |
| --- | --- | --- |
| `integrationBeanconqueror` | `RuntimeConfig` tag 96, Admin | default on; off also stops the Beanconqueror endpoints and the companion's |
| `integrationMahlkonig` | `RuntimeConfig` tag 97, Admin | default off; off: `/api/v2/*` answers 404, no client, no state, no `cfg.gbs` |
| `mahlkonigModel` | `RuntimeConfig` tag 98, Settings | |
| `mahlkonigPushCadenceS` | `RuntimeConfig` tag 99, Settings | 15, 20 or 25; anything else reads 20 (D12) |
| pairing identity | NVS `gbs_ident` | `brewDeviceId` (UUIDv4, made at the first pairing), `pairingId`, certificate pin (SHA-256 of the leaf DER), grinder address |

`gbs_ident` is a record of its own: no settings blob carries it, so restoring
settings never clones a brew device. **Unpair**/**Forget** and a factory reset
erase it; OTA keeps it. It is written only while the flash-write gate is open.
The grinder address lives with the pairing (not in `RuntimeConfig`, whose
copies fill every control queue item).

Settings actions: `POST /api/v1/mahlkonig` (Web UI claim required) with
`{"action":"pair","host":"192.168.1.87"}`, `{"action":"pairing_id","pairingId":N}`,
or `{"action":"cancel"|"release"|"trust"|"forget"}`; `409 MAHLKONIG_OFF`
while the integration is off, `409 MAHLKONIG_NOT_READY` for pairing without
a push model. The Settings stream carries `config.mahlkonigModel`,
`config.mahlkonigPushCadenceS`, `companion.gbs` and `gbs.*` (`route`, `pull`,
`waitS`, `seenAgeS`, `okAgeS`, `paired`, `pairing`, `code`, `link`,
`companion`, `host`, `last`); Home carries `config.integration*` and
`gbs.adjusts`.

## Pull mode (E64 WS)

Routes on the existing httpd, `/api/v2/*` GET and POST (trailing `/` and the
query ignored). Answers are `200 application/json`, unknown routes `{}`. A
peer outside the private and link-local ranges, or a request carrying
`Origin` or `Sec-Fetch-Site`, gets `403 MAHLKONIG_PEER`; POST bodies over
1024 bytes `413`. The `Host` check applies as on every route (IPs pass).

| Request | Answer |
| --- | --- |
| `GET /api/v2/machine` | generic Xenia identity: `MA_TYPE` 1, `MA_SUBTYPE` 1, `MA_SN` `OBBW` + the MAC hex, firmware 1.0 (types are a tolerance) |
| `GET /api/v2/mako` | the state below; counts a poll |
| `POST /api/v2/brewratio` | stores `SYNC_BEVERAGE_WEIGHT` (the no-weight fallback) |
| `POST /api/v2/scripts/execute` | `{"ID":9}` arms; anything else is ignored |

Answers come from a snapshot the network task republishes on every change
and once a second (`TIMESTAMP`, the rising time): no lock, no allocation, no
cJSON, no flash, no log line per poll, also during a shot (an unanswered poll
costs more airtime than an answer).

| State | `MA_STATUS` | `MA_EXTRACTION_STATUS` | Time | Volume | Leaves on |
| --- | --- | --- | --- | --- | --- |
| READY | 1 | 0 | 0 | 0 | `execute` → ARMED |
| ARMED | 3 | 1 | since `execute` | 0 | a cycle starts → BREWING; 180 s → ABORTED; Release → ABORTED |
| BREWING | 3 | 1 | since `execute` | 0 | its shot record → FINISHING/ABORTED; 60 s with no cycle and no record (a rinse) → ARMED |
| FINISHING | 4 | 0 | pump time | yield (g as ml) | 6 s **and** one poll, or 60 s → READY |
| ABORTED | 4 | 2 | elapsed | 0 | as FINISHING |

Always "ready to grind": set and sensed temperatures 93.0, tank 1. A repeated
`execute` while armed is the grinder's retry; `execute` in status 4 arms a
new grind. Only a shot that **starts after** `execute` settles it.

## Push mode (E65W, E65T, E80W, E80T)

HTTPS POST to `https://<address>:30001/grinderApi/<request>`, one TLS
connection per request (`Connection: close`, `Content-Length`), 8 s timeout,
async `esp_http_client` stepped by the network task (no new task),
`OutboundClient::MAHLKONIG_HTTP` in the outbound admission. Bodies are pinned
by the vectors: `pairingApplication`, `pairingStatus`, `brewJobs`,
`brewAccept`, `brewDone`.

- **Certificate**: trust on first use. A verify hook passed through
  `crt_bundle_attach` hashes the leaf; unpinned, the first good exchange pins
  it; pinned, another certificate fails the handshake and the link reads
  `identity` until **Trust this grinder** clears the pin (the pairing is
  kept). No global `CONFIG_ESP_TLS_INSECURE`.
- **Pairing** (person present): apply → the code the grinder shows →
  `pairingStatus` every 5 s for up to 3 min, tolerating the grinder's restart
  after accepting; `ClientErrorDevAlreadyPaired` asks for the Pairing ID;
  `ClientErrorPairingAppIdNotFound` applies again.
- **Cadence** (D12): `brewJobs` every 15, 20 or 25 s (default 20) while the
  grinder answers; after a failure the first retry waits one cadence, then 60
  and 120 s. `403` for 60 s in a row means the pairing was removed (shorter
  refusals are a grinder booting).
- **Jobs**: a new job is taken at once (`brewAccept`), taken again when it is
  handed out again; a newer job supersedes; a taken job with no shot within
  180 s is reported `Canceled`. `ClientErrorOngoingJobNotExisting` on
  `brewDone` takes the job again and resends. Result retries: 120 s for a
  result, 20 s for a cancellation, every 5 s.
- **After a shot** (S3): the shot's record lands once its hold ended; a job in
  hand gets `brewDone` first (with `brewAccept` before it when needed), then
  one `brewJobs` poll goes at once, not at the next cadence tick, and the
  cadence restarts from it. A job first seen at that poll is attributed to
  the shot that just ended when it was absent from the last poll before the
  shot started and the shot ended less than 60 s earlier; then `brewAccept`
  and `brewDone` follow immediately.

**Residual limitation**: the grinder drops an untaken job after about a
minute (a tolerance). Without the companion, a long shot pulled right after
grinding can outlast that minute before the post-shot poll sees the job; that
shot is then not synced (the grinder learns nothing wrong). The companion
asks every 2 s and takes the job while the shot runs.

## Reporting rules (every route)

A shot reports **Success** with its pump time (start to relay cut, drip
excluded) and its yield settled after the drip only when it was cut by weight
(`normal_target`, `extended_max_weight`, `extended_min_time`,
`slow_max_time`, `slow_min_weight`) or at the max shot time (`wall_limit`),
was not started without a scale, was not channeling, is a good shot, and its
yield source is `post_drip`. Everything else reports **Canceled**: a hand
stop, scale loss, a cup removed, a cut-short drip, a fault or safety limit.
The grinder applies its own windows (E64 WS 10–80 s; push ±15 s of the job);
the controller does not pre-filter. Rinses leave no shot record and are never
reported.

## Wi-Fi and BLE safeguards

- No request starts, and an in-flight one is torn down, during a sync hold
  (a shot with a scale until the drip ends, a scale connection or discovery,
  the BLE quiet interval, a closed flash-write gate).
- Pull answers are minimal and precomputed; no per-poll log.
- The Web UI log records transitions only (checked in, not seen for 30 s,
  armed, reported, pairing phases, link changes, route changes) and one line
  per shot. The client's own ESP-IDF transport lines are DEBUG traces; its
  first failure in a run logs the error codes.
- Coexistence, modem sleep, HT20 and the one-stream-per-channel rule are
  untouched; no new stream, keepalive or task.
- Integration off: no state, no client, `404` answers.

## Through the companion

With **Shot archive in the companion** and **Mahlkönig through the
companion** on, `cfg` carries `gbs` (`{"m":"e64ws"}`, or for a paired and
pinned push model `{"m","h","d","p","fp"}`), and the companion answers with
`{"t":"gbs","own":true|false,"st":"ok"|"unreach"|"unpaired"|"identity"|"seen"|"lost","ok":<shot id>}`
on change, never during a sync hold. While it owns the grinder it pings every
15 s; the controller stops its own polling and returns to it on link loss, a
missed ping (35 s), `own:false` or the switch off. The controller does no
per-poll work and adds no frame during a shot; `ok` (the last shot delivered
as Success) feeds Home's grind-advice pause. Pairing always runs on the
controller. See [Companion link](COMPANION_LINK.md#mahlkönig-grind-by-sync).

For an E64 WS the address Settings shows follows Beanconqueror's rule: the
companion's IP while it answers the grinder on port 80 (the controller's as
the fallback), otherwise the controller's with the reason.

## Diagnostics

The Diagnostic export (`/api/v1/debug/export`) has a `mahlkonig` section:
model, route, pull state, pairing phase, link, counters (polls, posts,
refused, requests, failures, seen age) and a 32-entry exchange journal
(request, outcome, HTTP status, error code, bytes, age; no bodies, no ids).
With the Web UI log this is enough for a first field report.

## Tolerances (confirmed only by a real grinder)

| Item | Assumed | Handled by |
| --- | --- | --- |
| E64 WS connection reuse | new connection per poll | `Connection: close` on every answer |
| `execute` retries | a few | repeats while armed are ignored |
| Untaken job lifetime | about 60 s | post-shot poll; a lost job is simply not synced |
| Taken job re-handed | about every 50 s | taken again on sight |
| TLS version, suites, SNI | ESP-IDF defaults work | the name check is replaced by the pin; SNI carries the address |
| `MA_TYPE`, `MA_SUBTYPE`, `script` types | numbers | the grinder reads only `MA_TYPE`, `MA_SN` and the versions |

## Simulator

The companion repository's `tests/grinder_simulator.py` plays both models on
localhost or the LAN (stdlib and `cryptography` only, no network beyond the
given address): an E64 WS client (2 s polls, `brewratio`, `execute` retries)
and an E65/E80 HTTPS server (self-signed certificate, pairing with code,
already-paired, application forgotten, restart after accepting, job expiry
and re-hand with tunable timers, `403` after Clear Pairing and while booting,
slow and dropped answers, certificate change). The companion's tests use it
end to end; a device run against it needs a `--development` build and the
owner's request.

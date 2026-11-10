# Data formats

Published formats other projects can read without depending on the firmware's
internal wire protocol.

## Shot export v1

- Schema: [`openbrewbyweight-shot-export-v1.schema.json`](openbrewbyweight-shot-export-v1.schema.json)
  (JSON Schema draft 2020-12).
- Example: [`openbrewbyweight-shot-export-v1.example.json`](openbrewbyweight-shot-export-v1.example.json),
  produced by the Web UI's own exporter; its first shot carries a trace.
- Each side's transition is `control.fastRampS`, `slowRampS`, `fastCurve`,
  `slowCurve`, `fastCurveHardness` and `slowCurveHardness`.
- `outcome.badReason` says why a shot is bad (`short`, `no_yield` or
  `low_yield`) and is `null` for a good one.
- `trace` (optional) is the shot's decoded decision record: header fields,
  `flags`, the start `snapshot`, the end `counters`, the `preShot` entries and
  the `events`, each with its `type` (`profile`, `debug`, `trace`), `code`,
  `name`, raw `a` and `b`, and a merged continuation in `ext`. The meaning of
  `a` and `b` per kind is in the firmware's
  [shot trace reference](../SHOT_TRACE.md#event-records). It is absent when
  the shot has no stored trace. `traceUnavailable` (optional, `true`) marks a
  shot whose trace could not be read when the file was made (the controller
  stayed busy after a few retries, or a read failed); the file goes on
  without it.
- `orphanTraces` (optional, top level, full-history export only) holds the
  newest 16 traces that have no shot, newest first: each with `bootId`,
  `endedAtMs` (0 for an interrupted one), `interrupted` (`true` when a
  restart cut its shot short, `false` when the history did not keep the
  shot) and `trace`, the decoded record as above, or `traceUnavailable`
  (`true`) instead when it could not be read. It is absent when Shot storage
  could not be read.
- Produced by: the Shots page, **Export → JSON** (every shot, bad ones
  included, each with its trace when it has one, plus `orphanTraces`) and a
  card's **⋯ → JSON** (that shot with its trace).
  All write the same envelope; a single-shot file has one entry in `shots`.
  File names are `openbrewbyweight-shots-diagnostics-YYYYMMDD-HHMM.json.gz`
  (`.json` where the browser cannot compress) and
  `openbrewbyweight-shot-<id>.json`.
- Source and contract: `shotsJson` and `decodeShotTrace` in
  `src/web/js/stats.js`; `src/tests/web_contracts/33-shot-export.inc.js`
  validates the output and the example against the
  schema, decodes the firmware's own trace fixture
  (`src/tests/fixtures/shot_trace_v1_hex.txt`, also checked by host case
  TRC25), and fails when a Stats row field has no home in the export.

Conventions:

- Units are part of the key name: `G` grams, `S` seconds, `GS` grams per
  second, `C` degrees Celsius, `Ms` milliseconds.
- Times are ISO 8601 with the shot's own UTC offset; they are `null` when the
  controller did not know the wall-clock time.
- Unknown or not applicable values are `null`, never sentinels such as `-1`,
  `0`, or an empty string.
- Enumerations use the firmware's lowercase names.
- What the firmware stored lives in the shot's main blocks. Values the Web UI
  computes at export time (per-reading flow, maximum flow, errors, the cut
  point, drip) live under `derived`, so readers may recompute or ignore them.
- The schema uses only `type`, `required`, `properties`, `items`, `enum`,
  `const`, `minimum`, and `$ref`, so a reader can validate it without a full
  JSON Schema library.

Versioning:

- `version` is `1`. This schema replaces the export formats shipped up to
  firmware 1.1.0 (the earlier version 1 and version 2 files), which are no
  longer supported: the project is still in development and those files are
  not read back.
- Adding optional properties keeps version 1; readers must ignore properties
  they do not know.
- Removing or renaming a property, or changing its meaning or unit, requires
  version 2 with a new schema file; the v1 file then stays published.

## Companion link test vectors v1

- [`companion-link-v1.vectors.json`](companion-link-v1.vectors.json): fixed
  inputs and expected outputs for the [companion link](../COMPANION_LINK.md)
  (verification code, pairing secret, session key, handshake MACs, one signed
  frame each way, the credentials key and ciphertext with a fixed `iv`, and
  under `relay` the relay handshake MACs, its key `Kr` and three signed relay
  headers with their canonical strings). The private scalars are for testing
  only.
- Consumed by the firmware host test `src/tests/companion_link_host_test.cpp`
  and by the companion's own tests.

## Stats header test vectors v1

- [`stats-header-v1.vectors.json`](stats-header-v1.vectors.json): the Web UI
  Stats header (the aggregate, both density charts, the consistency series
  and the filter facets) as the firmware's `formatStatsHeader` builds it over
  fixed sets of shots. The companion builds the same header over the whole
  history and must reproduce every case. Each case gives `window`, the active
  `presetId` of the "This preset" chart, `rows` (newest first) and the
  expected `header`; headers compare as parsed JSON, field for field.
- The rows are Stats rows in the units the Web UI and the companion store,
  limited to the columns the header reads. Back to the stored record:
  `durationDs = round(durationS × 10)`, `goalWeightDg = round(goalG × 10)`,
  `fastBeforeDs`, `slowAfterDs` and `arrivalDs` = the seconds × 10,
  `actualWeightCg`, `cutLineCg` and `avgFlowCgS` = the grams × 100 (`null` is
  the missing value), `verdict` `on_recipe`/`fast`/`slow` is 0/1/2 and `null`
  3, `shotType` and `stopDetail` are their wire names, `presetId`,
  `presetName`, `scaleName`, `hasWallTime` and `endedAtLocalSec` as they are.
  `badReason` is the reason that record derives (a check, not an input).
- Regenerate after a deliberate change to the header with
  `STATS_HEADER_REGENERATE=1 build-host/normal/src/tests/stats_header_host_test`
  (from `./scripts/dev test normal`'s build), then update the companion's copy.
  SHA-256 of the current file:
  `ad46e4e9d0acc434d37a72b3f60e2dc2f4b7a1b713f37c0410f316d8188de547`.
- Consumed by the firmware host test `src/tests/stats_header_host_test.cpp`
  and by the companion's tests.

## Companion record frames v1

- [`companion-records-v1.vectors.json`](companion-records-v1.vectors.json):
  the archive sync's link frames ([Companion link](../COMPANION_LINK.md)) as
  the firmware builds them for a fixed store: each case names the companion's
  `request` (`rx`, `rf`, `rt` or `ping`, without `q`; `null` for the `rs`
  notice) and the controller's `frames` in send order, each the exact JSON
  text after the MAC and the dot, `q` kept. Cases: both indexes (with rows
  and `sum`, and unchanged; the shots index lists the bad shot), a Stats
  fetch paged over two frames, a History fetch, traces `ok`/`none`/`busy`,
  `rs`, the ping's `hold` answer (with both store epochs), the `page_*`
  cases and, last, the store moved (a new shot, a rating change and a
  deletion): a shots index delta (`rx` with `d`) against the first case and
  the two one-shot fetch windows the companion's archive then asks. The
  `page_*` cases are a screen's own Stats and History pages over the same
  records (`request` is the ui-stream op, with `active`, the active preset,
  and for a fetch `view`, whether the screen holds Stats): date and rating
  orders both ways, result, preset, bad and scale filters, windows 10, 50
  and 100, another active preset, a scrolled page, an export window over two
  frames, Home's rows-only fetch, and History in both orders, by type and as
  a fetch. The companion's replica must serve each frame for frame, every
  member but `tag`, `seq` and `request`. The senders, the row formatter, the
  header, the trace frame and the sealer are the firmware's own; the store,
  its curves and the trace bytes are fixtures (the History store pages as
  `HistoryLog::copyPage` does), and the MAC is zeros (the link vectors cover
  it).
- Regenerate after a deliberate frame change with
  `COMPANION_RECORDS_REGENERATE=1 node src/tests/check_web_assets.js`, then
  update the companion's copy. SHA-256 of the current file:
  `bf6e49994641a7975933f8c4370012ab4b36023648433c8910776287c931a0f4`.
- Produced and checked byte for byte by the web contract
  `src/tests/web_contracts/27-companion-records-vectors.inc.js`; replayed by
  the companion's sync and replica tests.

## Formats written for other apps

The Shots card menu also writes three third-party formats. They follow those
projects' own formats and are not versioned here:

- **Gaggiuino shot** (`openbrewbyweight-shot-<id>-gaggiuino.json`), as read by
  Visualizer.coffee's Gaggiuino importer: `datapoints` with `timeInShot` (ms),
  `shotWeight`, `weightFlow`, and `targetTemperature`; `duration` in
  deciseconds; `timestamp` is the start in Unix seconds, or 0 without wall
  time. Channels the controller does not measure are omitted.
- **Visualizer shot** (`openbrewbyweight-shot-<id>-visualizer.json`), the
  de1app shot JSON (`version` 2) that Visualizer.coffee reads with its default
  Decent JSON parser; `shotVisualizer` in `src/web/js/stats.js`. It must not
  carry the top-level keys that route a file to another parser (`mill`,
  `brewFlow`, `datapoints`, `samples`, or `profile_name` with `data`).
  `timestamp` is the start in Unix seconds, or the download time without wall
  time (the parser has no fallback). `elapsed` (s), `totals.weight` (g),
  `flow.by_weight` (g/s), `temperature.goal` (°C, only when known), and
  `state_change` (1 before the first drop, 2 pouring, 3 after the cut) share one
  index; held readings are left out. `app.data.settings` carries
  `profile_title`, `drink_weight`, `bean_weight`, `final_desired_shot_weight`,
  `espresso_temperature`, `espresso_enjoyment` (rating × 20), and
  `espresso_notes`, a Markdown list of the stop, yield error, cut, drip, scale,
  scale loss, and brew-by-weight details.
- **Beanconqueror brew handoff**: `beanconqueror://ADD_BREW?len=<n>&shareBrew0=…`,
  a gzip-compressed `IHandoffEnvelope` v1 in unpadded base64url split into
  400-character `shareBrew<n>` parameters. Water in is sent as 0 ml because it
  is not measured. Beanconqueror accepts it from 8.8; earlier releases have no
  `ADD_BREW` route and show their generic wrong-QR-code alert.

## Beanconqueror machine endpoints v1

Beanconqueror imports shots from a connected espresso machine. With
**Settings → Coffee apps → Beanconqueror** on, the controller answers as a
Gaggiuino or a GaggiMate under one base address, `http://<host>/beanconqueror`.
A companion with **Beanconqueror through the companion** on answers the same
paths from its archive, without contacting the controller. The routes are
read-only, need no token by design (like the machines they stand in for), keep
the controller's `Host` check, and send no CORS headers. With the setting Off,
every path answers `404`. The controller answers `503` while it keeps the
radio quiet: a shot until its drip delay ends, a rinse, a scale connecting
(the companion link's sync hold). Only good shots are served; a bad or
missing shot answers `404`.

| Format | Path under `/beanconqueror` | Answer |
| --- | --- | --- |
| Gaggiuino | `/api/system/status` | `{}` |
| Gaggiuino | `/api/shots/latest` | `[{"lastShotId":N}]`, the newest good shot; `[]` with none |
| Gaggiuino | `/api/shots/<id>` | the shot as JSON (below) |
| GaggiMate | `/api/status` | `{"mode":0}` |
| GaggiMate | `/api/history/index.bin` | `SIDX` v1 index of every good shot, newest first, `nextId` = newest id + 1 |
| GaggiMate | `/api/history/recent.bin?limit=n` | the same index cut to the newest `n` (default 8, 1–50), `nextId` 0 |
| GaggiMate | `/api/history/<id, 6 digits>.slog` | `SHOT` v7 file (below) |
| GaggiMate | `/api/history/<id>.json` | notes `{"id":"<id>","doseIn":"18.0"}`; `404` without a dose |

JSON answers are `application/json`; binary answers are
`application/octet-stream`, little-endian.

**Samples.** Both formats send the curve's readings in order, drip included,
leaving out held readings, as the shot JSON export does. Rounding is half up
(`floor(x + 0.5)`). A weight is clamped at 0 and sent in decigrams
(`floor((cg + 5) / 10)`). The flow is the Stats page's weight flow
(`buildShotSparkModel().flowCurve` in `runtime.js`), in g/s: the gain over a
trailing window of at least 1 s, with none at a segment's first reading,
before the first drop, across a break or a scale-loss window, or overlapping
a held reading. A missing value is sent as 0. The start time is
`round(endedAtUnixSec - durationS)` with wall time, otherwise absent (`0` in
GaggiMate's fields). The yield is `actualG`, which every good shot has;
Beanconqueror itself takes the yield from the last reading's weight in both
formats.

**Gaggiuino shot.** `id`; `timestamp` (Unix s, only with wall time);
`duration` (ds); `datapoints` with ten arrays of one entry per sample:
`timeInShot` (ds), `shotWeight` (dg), `pumpFlow` and `weightFlow` (both the
weight flow, tenths of g/s), `temperature` and `targetTemperature` (the
target, tenths of °C; 0 without one), and `pressure`, `waterPumped`,
`targetPumpFlow` and `targetPressure`, all 0 because they are not measured;
and `profile` `{id, name, waterTemperature}` (the preset and its target in
°C). There is no `globalStopConditions`, so Beanconqueror takes the yield
from the last reading.

**GaggiMate index entry** (128 bytes): `id`; `timestamp`; `duration`
(`durationS` in ms, which Beanconqueror uses as the brew time, so it leaves
out the drip wait); `volume` (yield, dg); `rating` (0–5); `flags` `0x01`
completed, plus `0x04` when there is a dose; `profileId` (the preset id as
text, 32 bytes) and `profileName` (UTF-8, at most 47 bytes, never splitting a
character, 48 bytes); `avgTemp` and `maxPressure` 0 (not measured);
`avgFlow` (`avgFlowGS` × 100, 0 when missing); the rest 0.

**GaggiMate `.slog`**: a 512-byte header with `magic` `SHOT`, `version` 7,
the sample size 30, `headerSize` 512, `sampleInterval` 50, `fieldsMask`
`0x3FFF`, `sampleCount`, `durationMs` (`durationS` in ms), `startEpoch`,
`profileId`,
`profileName`, `finalWeight` (yield, dg) and no phase transitions; then
30-byte samples with `t` (ms), `tt` (target, tenths of °C), `vf` (weight
flow, hundredths of g/s, at most 32767) and `v` (dg). Every other field is 0.

- [`beanconqueror-v1.vectors.json`](beanconqueror-v1.vectors.json): Stats-row
  inputs (`cases[].shot`), the derived flow (`flowGS`), and every answer above
  for each served case; `bad-short` is not served, and the `edge-` cases
  hold the readings the ports could disagree on (repeated times, long gaps,
  falling and negative weight, the flow field's limit, held readings beside a
  break, no cutoff, an open scale-loss window). Generated from the Web
  UI's flow; consumed by the firmware host tests, the Web contracts (the
  Settings preview decoder) and the companion's tests, whose fixture copy is
  byte-identical.

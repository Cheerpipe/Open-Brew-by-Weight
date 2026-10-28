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
- `orphanTraces` (optional, top level, diagnostics export only) holds the
  newest 16 traces that have no shot, newest first: each with `bootId`,
  `endedAtMs` (0 for an interrupted one), `interrupted` (`true` when a
  restart cut its shot short, `false` when the history did not keep the
  shot) and `trace`, the decoded record as above, or `traceUnavailable`
  (`true`) instead when it could not be read. It is absent when Shot storage
  could not be read.
- Produced by: the Shots page, **Export → JSON** (every good shot, no traces),
  **Export → JSON + diagnostics** (every shot, bad ones included, each with its
  trace when it has one, plus `orphanTraces`), and a card's **⋯ → JSON** (that shot with its trace).
  All write the same envelope; a single-shot file has one entry in `shots`.
  File names are `openbrewbyweight-shots-YYYYMMDD-HHMM.json`,
  `openbrewbyweight-shots-diagnostics-YYYYMMDD-HHMM.json.gz` (`.json` where
  the browser cannot compress) and `openbrewbyweight-shot-<id>.json`.
- Source and contract: `shotsJson` and `decodeShotTrace` in
  `src/web/js/stats.js`; `src/tests/web_contracts/33-shot-export.inc.js`
  validates the output, the diagnostics export and the example against the
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
  is not measured. Beanconqueror releases up to 8.7.0 do not accept it.

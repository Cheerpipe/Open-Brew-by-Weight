# Data formats

Published formats other projects can read without depending on the firmware's
internal wire protocol.

## Shot export v2

- Schema: [`openbrewbyweight-shot-export-v2.schema.json`](openbrewbyweight-shot-export-v2.schema.json)
  (JSON Schema draft 2020-12).
- Example: [`openbrewbyweight-shot-export-v2.example.json`](openbrewbyweight-shot-export-v2.example.json),
  produced by the Web UI's own exporter.
- Version 2 replaces `control.cutRampS`, one transition length for both
  sides, with each side's transition: `fastRampS`, `slowRampS`, `fastCurve`,
  `slowCurve`, `fastCurveHardness` and `slowCurveHardness`. The
  [version 1 schema](openbrewbyweight-shot-export-v1.schema.json) and its
  [example](openbrewbyweight-shot-export-v1.example.json) stay published for
  files exported before.
- `outcome.badReason`, added within version 2, says why a shot is bad
  (`short`, `no_yield` or `low_yield`) and is `null` for a good one. The
  whole-log export holds good shots only; a bad shot exported from its card
  carries its reason.
- Produced by: the Shots page, **Export → JSON** (every good shot) and a
  card's **⋯ → JSON** (that shot). Both write the same envelope; a single-shot
  file has one entry in `shots`. File names are
  `openbrewbyweight-shots-YYYYMMDD-HHMM.json` and
  `openbrewbyweight-shot-<id>.json`.
- Source and contract: `shotsJson` in `src/web/js/stats.js`;
  `src/tests/web_contracts/33-shot-export.inc.js` validates the output and the
  example against the schema and fails when a Stats row field has no home in
  the export.

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

- `version` is `2`. Adding optional properties keeps version 2; readers must
  ignore properties they do not know.
- Removing or renaming a property, or changing its meaning or unit, requires
  version 2 with a new schema file; the v1 file stays published.

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

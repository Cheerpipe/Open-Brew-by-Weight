# Data formats

Published formats other projects can read without depending on the firmware's
internal wire protocol.

## Shot export v1

- Schema: [`openbrewbyweight-shot-export-v1.schema.json`](openbrewbyweight-shot-export-v1.schema.json)
  (JSON Schema draft 2020-12).
- Example: [`openbrewbyweight-shot-export-v1.example.json`](openbrewbyweight-shot-export-v1.example.json),
  produced by the Web UI's own exporter.
- Produced by: the Shots page, **Export → JSON** (every stored shot) and a
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

- `version` is `1`. Adding optional properties keeps version 1; readers must
  ignore properties they do not know.
- Removing or renaming a property, or changing its meaning or unit, requires
  version 2 with a new schema file; the v1 file stays published.

## Formats written for other apps

The Shots card menu also writes two third-party formats. They follow those
projects' own formats and are not versioned here:

- **Gaggiuino shot** (`openbrewbyweight-shot-<id>-gaggiuino.json`), as read by
  Visualizer.coffee's Gaggiuino importer: `datapoints` with `timeInShot` (ms),
  `shotWeight`, `weightFlow`, and `targetTemperature`; `duration` in
  deciseconds; `timestamp` is the start in Unix seconds, or 0 without wall
  time. Channels the controller does not measure are omitted.
- **Beanconqueror brew handoff**: `beanconqueror://ADD_BREW?len=<n>&shareBrew0=…`,
  a gzip-compressed `IHandoffEnvelope` v1 in unpadded base64url split into
  400-character `shareBrew<n>` parameters. Water in is sent as 0 ml because it
  is not measured. Beanconqueror releases up to 8.7.0 do not accept it.

# Contributing

Start with `AGENTS.md`, the [documentation index](docs/README.md), and the
nearest scoped `AGENTS.md`. Set up dependencies using [Build](docs/BUILD.md);
tests never install packages or contact hardware implicitly, and
`./scripts/dev doctor` reports the local tool versions before tests or
firmware compilation.
Routine setup uses the committed npm and IDF locks; follow the
[explicit upgrade steps](docs/BUILD.md#upgrade-dependencies-deliberately) when
changing a dependency.

## One change, from discovery to validation

1. Find the area with `./scripts/dev context <area>`. Use `build` for toolchain
   work, `web` for UI, and `control` for brewing; all areas are listed in
   [the developer map](docs/README.md#developer-map).
2. Run `./scripts/dev classify <paths…>` before editing. Unknown paths are R3;
   safety-related documentation can also be R3. State affected invariants.
3. Before multi-step implementation, create a plan in `docs/plans/`;
   before an audit, create its file in `docs/audits/`. Read or resume existing
   plans/audits only when the user explicitly asks to read or work with them, or
   when they are referenced within two levels from such a requested record; never
   load them proactively as context. Follow
   [the shared tracking and handoff contract](docs/AI_WORKFLOW.md#plans-audits-and-session-handoff).
   Track every step and substep with a status and checkbox; update completed work
   immediately and keep the recovery checkpoint in the same file current.
   Keep other temporary working files in `temp/`, inside the local project.
   Follow [local storage and migration rules](docs/AI_WORKFLOW.md#local-project-files-and-git)
   for existing external task files, IDE access, and cleanup.
4. Make a coherent change, preserving unrelated work. Do not combine source
   moves with GPIO, relay behavior, NVS migrations, or unrelated features.
5. Follow [documentation maintenance](docs/AI_WORKFLOW.md#documentation-maintenance)
   to update the canonical guides for changed settings, commands, interfaces, or
   behavior in the same change. Link to reference tables rather than copying them.
   A change to a Web UI chart that the Home Assistant cards copy (the preset
   chart, the last-shot charts, the shot card, or the Stats charts) also
   changes the matching card in
   [ha-open-brew-by-weight](https://github.com/Cheerpipe/ha-open-brew-by-weight)
   in the same piece of work; see `src/web/AGENTS.md`.
6. Run `./scripts/dev validate` on the final changed paths. Consult
   [VALIDATION.md](VALIDATION.md) for required gates and evidence.
   R2/R3 validation builds every profile with transient development and USB
   Serial/JTAG options so its size check matches the conservative resource baseline.
   Changes to HTTP route or handler definitions must also run
   `./scripts/dev test web`; its first check verifies that registered routes
   remain below the configured handler limit.
7. Update the plan or audit with final status and evidence. Report risk, checks,
   failures, full-log location, and any manual evidence still needed. Every
   code-modification request ends with a commit containing only task files. Use
   the exact suggested Extended Conventional Commit title and a nonempty body
   that lists the changes and explains their purpose or impact. A read-only
   audit with no tracked changes needs no commit.

## Examples

| Change | Discover / classify | Validate |
| --- | --- | --- |
| Non-critical README wording | `./scripts/dev classify README.md` | `./scripts/dev validate README.md` |
| Web styling | `./scripts/dev context web`; classify exact changed files | Focused `dev test web` (and `dev test lint` after `npm ci`), then the classified gate |
| Brew or switch behavior | `./scripts/dev context control` or `machine`; classify paths and state invariants | Complete R3 gate and applicable bench/HIL evidence |

Focused tests help during editing but do not replace a higher-risk gate.
Flash, OTA, relay operation, and HIL require explicit authorization.

## Documentation style

Write concise English for the reader's task: purpose, prerequisites, action,
expected result, then exceptions. Keep exact UI labels and units. Defaults
must identify their preset/build or migration context.

Use short guides and stable headings. Keep advanced contracts separate from
daily-use steps, one maintained example per integration, and relative links to
sources or related pages. Do not duplicate large payloads or configuration
blocks to support another reading path.

Canonical behavior belongs in versioned guides under `docs/`. Development plans
belong in Git-ignored `docs/plans/`; audits and their findings belong in
Git-ignored `docs/audits/`. Keep each record's progress, decisions, evidence, and
handoff together in its own file. Retain completed plans and audits locally;
never stage or force-add them. Canonical guides must remain usable without these
local files. `temp/`, `docs/plans/`, and `docs/audits/` are all part of the local
project and accessible from the IDE; their Git exclusion does not place them
outside the project directory.

# Validation gates

Classify changed paths with `./scripts/dev classify [paths...]`. Unknown paths
are R3 and an override can only raise risk. Complete logs and a redacted JSON
summary are written to `artifacts/runs/`.

When several agents share this checkout, always pass the paths of your change
and read the `warning: changed path outside requested scope` lines: they name
other sessions' working-tree edits that your gate did not cover, so a clean
gate is never mistaken for a clean tree. Omitting paths classifies the whole
working tree and prints no warning.

Every change — including documentation-only edits — must be reviewed and
validated rigorously: run **all** gates that apply to the change's classified
risk, review the complete diff against the affected invariants, and record a
gate that cannot run as failed instead of skipping it silently. Passing a
subset of the applicable gates does not validate the change.

| Risk | Typical change | Required automated gate | Manual evidence |
| --- | --- | --- | --- |
| R0 | Non-critical docs/meta | contract, link, and path checks | none |
| R1 | Web UI, tests, tooling, pure logic | R0 + focused tests and generated assets | as identified |
| R2 | BLE, network, persistence, OTA, build | full host + ASan/UBSan + TSAN + architecture + development and release images of every official validation profile pair | subsystem-dependent |
| R3 | Relay, machine, ISR, watchdog, boot, GPIO, partitions, remote control, unknown | R2 + warnings/cppcheck + build variants | HIL/manual required |
| Release | Candidate firmware image | complete analysis, resource budgets, applicable soak, HIL and manual plan | required |

Use `./scripts/dev validate [--risk R0..R3] [paths...]`. R2/R3 run the host
tests and the firmware steps as two parallel lanes; see
[scripts](docs/SCRIPTS.md) for the order and failure behavior. Focused commands are
`./scripts/dev test [normal|asan|tsan|web|ble|ota|tooling|lint]`, `./scripts/dev build`, and
`./scripts/dev analyze`; `lint` is the opt-in Web UI static analysis and is not part
of any risk gate. The supported IDF setup and commands remain documented
in [Build](docs/BUILD.md), [scripts](docs/SCRIPTS.md), and
[static analysis](docs/STATIC_ANALYSIS.md). Hardware acceptance is defined by
the [manual test plan](docs/MANUAL_TEST_PLAN.md) and curated evidence by
[target trace qualification](docs/P2_TARGET_TRACE.md).

Any change to `src/OpenBrewByWeightNetwork.cpp`,
`src/network/OpenBrewByWeightHttpLifecycle.inc`, or another source that defines or
registers HTTP routes or handlers must run `./scripts/dev test web`. That profile,
and every R2/R3 validation gate, runs the named `http-route-capacity` check and
the Web UI budgets (`web-budgets`) before the broader Web contracts, then
checks that the design preview still renders from the current sources
(`web-preview`); the profile first checks that the Web UI sources are
Prettier-formatted (`web-format`). The route-capacity check counts all `registerHandler(server_, ...)`
registrations in the network sources and requires `max_uri_handlers` to leave
at least ten spare slots, the headroom the server configuration promises.

Missing tools or dependencies fail profiles that require them with exit 127.
Tests never bootstrap packages or access hardware/network implicitly. R3 is not
release-ready while required HIL/manual evidence is pending.
Versioned firmware budgets come in two kinds, defined by
`docs/P2_RESOURCE_BUDGETS.md`: the shared development baseline, measured with
the `--development` profile (admin unlock plus the JTAG console), and one
release baseline per supported profile pair, measured on the flag-free release
image that ships. Other builds (extra flags, individual `--jtag` or
`--no-auth-admin`, other optimization levels) retain the OTA-slot and
memory-placement checks but print "no versioned baseline for this build
profile" and are **not** budget evidence. Any change that can move image size
(embedded Web assets included) must be verified with the R2/R3 gate, or with
both a `--development` build and a flag-free release build of each affected
pair, before it is called done.
R2/R3 validation builds all supported built-in profile pairs in
`docs/BUILD_PROFILES.md` both ways: the development images stay in their
profile build directories and the release images in `build-idf/release/`.
Image and memory regions are compared by
`src/tests/check_firmware_size.js` against those versioned budgets.

After each development and release build, R2/R3 also run the stack-depth gate
(`stack-<pair>` and `stack-release-<pair>`, `scripts/check_stack_depth.py`). It
recompiles every unit of that build with GCC `-fcallgraph-info=su` into
`<build>/stack-usage/` and bounds the worst-case stack of the destructive
recovery paths and the deepest httpd requests against their task stacks: boot
recovery on the Arduino loopTask (`CONFIG_ARDUINO_LOOP_STACK_SIZE`), the Web
UI factory reset on `network_manager` (`NETWORK_MANAGER_TASK_STACK_SIZE`), the
shot trace pool and shot-store image on `settings_persist`
(`SETTINGS_PERSIST_TASK_STACK_SIZE`), the
OTA transfer and commit on `ota_worker` and its inline httpd fallback, and the
UI stream request and crash archive download on httpd
(`HTTP_SERVER_TASK_STACK_SIZE`). Calls GCC cannot follow are charged declared
reserves (function pointers, ROM/libc, the kernel's per-task context); a
function pointer that leads to a gated root must be declared in the gate.
Recursion is charged its declared bound (for example the 32-level JSON
nesting limit) times the frames of the cycle; undeclared recursion or an
unbounded dynamic frame on a gated path fails. A path over budget fails with its full call chain; shrink the
deepest frames (for example a `PersistedSettings` copy on the stack) instead
of raising a task stack.

## Size budgets and parallel work

Every gate that measures sizes (`test web`, `test budgets`, and the R2/R3
`validate` gates) records each measurement and ends with one `budget-verdict`
step that lists every overrun at once: the Web UI budgets in
`config/web-asset-budgets.json` and every development and release image
against `config/resource-baselines.json`. `./scripts/dev test budgets` checks
only the Web UI budgets, in a few seconds.

When agents work in parallel:

- Give each agent its own worktree. Web contracts and firmware builds rewrite
  the shared generated Web UI header under `src/`, so in one checkout they
  hold a checkout-wide lock and wait for each other, and a measurement there
  includes every other session's edits. Gates in separate worktrees run
  independently.
- While a change is still in development, validate with `--budget-report`
  (on `validate` and `test web|budgets`). Overruns print as warnings and the
  run passes, but it is not budget evidence. Do not edit budget numbers in
  that change; report the measured deltas instead.
- The change that lands last integrates, whether or not a coordinator
  session runs it. Before landing in `main`, rebase on the current `main`
  and run the full classified gate without `--budget-report`. If its only
  failed check is `budget-verdict` and the growth is reviewed, run
  `./scripts/dev budget update --from <run-id>` for that same run and commit
  the budget change with the feature, explaining in the body why each budget
  moved. That run together with the update's re-check of its measurements is
  the gate evidence; the gate does not run again.

`budget update` refuses a run that recorded no budget measurements, used
`--budget-report`, failed any check other than `budget-verdict` or stopped
before it, or, for R2/R3, lacks the development or release measurement of
some pair. It also refuses a stale run: every budget gate records `HEAD`, a
fingerprint of tracked changes and untracked files, and the hash of both
budget files, and any difference from the current tree (for example, a run
taken before the rebase) means the gate must run again. The budget files
classify as R1 validation data.

The documentation check covers root guides, canonical guides in `docs/`, the BLE
library README, and the safety README. Local working files in `temp/`,
`docs/plans/`, and `docs/audits/` are not documentation sources for this check.
They remain part of the local project but outside Git; validate their evidence
and progress through the [AI workflow](docs/AI_WORKFLOW.md#local-project-files-and-git).
The check validates local paths, images, and Markdown heading/
explicit HTML anchors (including duplicate headings and multiline links).
External URLs and example contents are not certified by this offline check.

## Reading a result

A zero exit code means the automated profile passed, not that hardware is
qualified. Use the printed `artifacts/runs/<run-id>/summary.json` and full log
to inspect which steps ran. Exit 127 means a required dependency is missing;
record the gate as failed and prepare the dependency explicitly.

Failed runs keep their evidence: only steps that started can produce
diagnostics, and a failed prerequisite may leave later entries absent in the
run record.

Every environment verifies the required tools are present and runnable
before tests or builds — Node and Cppcheck carry no project version pin, so
the machine's current release is what runs. Firmware builds require
ESP-IDF 6.1.0 and reject any change to the IDF component lock during a
build.

Classify concrete files, not a directory name such as `docs`. Safety-related
documents and BLE/OTA references can select R3/R2 even though they are Markdown.
Do not lower risk to avoid an unavailable tool or missing physical evidence.

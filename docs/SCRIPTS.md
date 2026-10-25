# Script reference

`./scripts/dev` is the only supported public firmware command. It never
installs dependencies or contacts hardware unless a command explicitly asks to
flash over USB.

## Commands

| Task | Command |
| --- | --- |
| Inspect profile compatibility | `./scripts/dev profiles` |
| Build firmware | `./scripts/dev build --hardware <hardware> --machine <machine>` |
| Build and install over USB | `./scripts/dev build flash --confirm --hardware <hardware> --machine <machine> --port <port>` |
| Flash an existing image | `./scripts/dev flash --confirm --hardware <hardware> --machine <machine> --port <port>` |
| Monitor USB serial | `./scripts/dev monitor --port <port> --speed 115200` |
| Run host tests | `./scripts/dev test normal` |
| Preview Web UI designs | `./scripts/dev preview` |
| Lint the Web UI sources | `./scripts/dev test lint` |
| Validate changes | `./scripts/dev validate` |

Successful firmware builds automatically retain ELF/BIN pairs in
`artifacts/firmware/<hardware>--<machine>/<ELF SHA>/`, including `build` without
`flash`. The directory also contains image identity for matching a crash log to
its ELF; `./scripts/dev clean` keeps these archives.

At the end of every successful command that includes `build`, the terminal
highlights the absolute build-output folder and the absolute `openbrewbyweight.bin`
path ready for installation.

## Web UI sources

The Web UI sources in `src/web/` (HTML partials, `app.css`, `app.js`, and
`js/`) are kept readable with the Prettier version pinned in `package.json`:
run `npm run format:web` after editing them. `./scripts/dev test web` starts
with the same check as `npm run format:web:check` and fails while any source
is out of style. The generator (`scripts/gen_web_ui.js`) alone
compacts what ships: html-minifier-terser collapses markup whitespace, Lightning
CSS re-serializes the stylesheet before clean-css restructures it, and Terser
minifies the JavaScript. Reformatting a source therefore never changes the
embedded bytes; the Web contracts print the sources at two widths and require
identical generator output.

## Web UI lint

`./scripts/dev test lint` checks the Web UI sources with the linters pinned in
`package.json`, so install them once with `npm ci`; the command never installs
anything and exits with 127 when they are missing. It runs three steps:

- **ESLint** on `src/web/app.js` and `src/web/js/` with correctness rules only
  (`scripts/lint/eslint.config.mjs`). The build-time constants and markers
  (`__WEBUI_TEXT__`, `__LINEA_MICRA__`, `__REMOTE_CONTROL__`, `__homeModule`) are
  declared there; formatting stays with `npm run format:web`.
- **Stylelint** on `src/web/app.css` (`scripts/lint/stylelint.json`), allowing the
  `__WEBUI_CSS_TEXT__()` localization marker and the vendor prefixes Safari needs.
- **Markup** (`scripts/lint_web_ui.js`): renders the English shell with every view
  partial injected, as the browser receives it, and runs html-validate with
  `scripts/lint/htmlvalidate.json`, which also fails on any id used twice
  across views. The Web contracts check duplicate ids too, so every Web gate
  catches them without the lint.

Warnings fail the run like errors. The lint is static analysis, so
`./scripts/dev validate` does not run it; use it while editing the Web UI or
when a review asks for it.

## Web UI design preview

The design preview server renders the embedded Web UI in a desktop browser
for design iteration. Only the server itself is part of the project; every
design asset it serves is a local file that never enters Git:

- **Project tooling:** `scripts/webui-preview/server.js`, started with
  `./scripts/dev preview` (use `--port 4174` when the default port 4173 is
  busy). It listens only on this computer, reads the current firmware
  markup, styles, and scripts on every page load, and stops with Ctrl-C.
- **Local design assets:** the `webui-mockups/` folder at the repository
  root. The folder is Git-ignored, so a fresh clone starts without it and
  mockup work never produces commits, documentation, or tests.

The server offers two kinds of pages. Reference views are rendered live
from the firmware sources: `/` shows the Home screen, `/mobile-menu` the
bottom-navigation sample views, and `/micra` the machine diagnostics, with
the firmware stylesheet at `/app.css`. Everything else is served from
`webui-mockups/` by file name: drop `proposal.html` into the folder and
open `http://127.0.0.1:4173/proposal`; `proposal.css` and `proposal.js`
resolve the same way, so adding or removing a mockup never requires a
server change. The header and Micra scripts the reference views load are
assembled from named functions in the firmware sources, and
`./scripts/dev test tooling` renders every reference view and fails when one
of those scripts no longer compiles or calls a Web UI function it lacks.

The preview does not connect to a device or change its settings; the
values it shows are illustrative. The header's **How shots stop** icons and
panel run against a small simulated controller in the page, so you can try
the mode and guard switches; reloading starts over. Mockups and assets created for the
preview are development resources, not product files: they must not
generate commits, documentation updates, or tests, and they never ship in
firmware assets. Bringing a finished design into the product is a
separate, deliberate change.

## USB installation

Review the selected profiles and physical safety before adding `--confirm`. The
flag authorizes hardware access; it does not skip image, profile, or partition
checks. A controller whose partition table is not the current one, including
any development image from before v1, needs a [clean
installation](BUILD.md#clean-installation). Use `--erase-all` only for an
explicitly chosen clean installation:

```sh
./scripts/dev build flash --confirm \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --port /dev/cu.usbmodem2101 \
  --erase-all
```

The clean install erases both firmware slots and all persisted data: settings,
Wi-Fi credentials, presets, scale preferences, shot history, curves, and the
last-shot record. Record anything needed before running it.

`--image <path>` flashes an existing verified project image and is rejected
with `build`. Flash and OTA still need `--hardware` and `--machine`, and the
image must have been built for that pair, so firmware for a different switch
or machine is never installed by mistake. `--no-check` skips only local image identity verification; it
does not skip partition checks. `--port` must name a connected USB serial
device. On macOS use `/dev/cu.usbmodem*`; on Linux use `/dev/ttyACM*` or
`/dev/ttyUSB*`.

## Wi-Fi update (OTA)

Upload a built image to a controller on the same network without a USB cable:

```sh
./scripts/dev build ota --confirm \
  --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra \
  --host 192.168.1.50 \
  --yes --wait-for-confirmation
```

`--host` accepts the controller's station address, or `192.168.4.1` on its
access point. The hidden prompt, or `OPENBREWBYWEIGHT_DEVICE_PASSWORD`, supplies
the device password; commands never take it as an argument. `--image <path>`
uploads an existing image instead of building. `--yes` answers the commit
question; `--wait-for-confirmation` independently verifies the rebooted image.
Transfers are resumable: an interrupted run queries the confirmed offset and
repeats only the missing ranges. Safety behavior, session details, and
troubleshooting: [OTA](features/ota.md).

## Build profiles

Every project build requires an exact hardware and machine profile. Use
`./scripts/dev profiles` to inspect built-in profiles and physically compatible
pairs. The compatible built-in pairs are listed in
[Build profiles](BUILD_PROFILES.md#capability-matching); each machine profile
declares its compatible hardware profiles. The optional `--development`
profile enables the development admin/JTAG settings; `--release` is the
default. Passing `--release` explicitly marks a publishable image and requires
a clean Git tree, untracked files included. These options are compile-time
only and are never persisted.

`./scripts/dev validate --risk R2` builds each official validation pair twice:
a `--development` budget build and the flag-free release image that ships,
each checked against its own size baseline. `--risk R3` also checks each pair
with Cppcheck and a GCC warning build.
R2 and R3 run two lanes at once: the host tests, and the firmware steps in a
fixed order (route capacity, Web UI budgets, Web contracts, then each pair's
development build, release build, Cppcheck, and warning build). Each step's
output appears in the log as one block when
the step finishes. After any failure neither lane starts another step;
steps already running finish first. When both lanes pass, a final
`budget-verdict` step judges every size budget the run measured. The release and warning builds use their
own trees, `build-idf/release/<hardware>--<machine>/` and
`build-idf/warnings/<hardware>--<machine>/`, so the gate's
`build-idf/<hardware>--<machine>/` images stay incremental. Host tests and
Cppcheck use one job per CPU.

The supported transient options are `--webui-language`, `--flags`, `--o0`,
`--og`, `--o2`, and `--os`. The build also accepts
`--force-sdkconfig-regenerate` to recreate its selected variant's configuration
from repository defaults without cleaning the build tree. Builds do this
automatically when selected defaults change or a legacy tree has no recorded
input fingerprint. Regeneration discards local `menuconfig` choices; an
unchanged variant retains them. The flag is rejected for standalone flash, OTA,
and monitor. The stored device password is never accepted on a
command line; use the hidden prompt or the documented USB serial procedure.
The current n16r8 resource baseline applies to the default `-O2` builds.
Other optimization levels
retain the physical image and memory placement checks; see [Build](BUILD.md#compiler-optimization-and-existing-sdkconfig).

## Size budgets

`./scripts/dev test budgets` measures every Web UI size budget in
`config/web-asset-budgets.json` in a few seconds and lists each measurement
against its limit. `test web` runs the same step before the Web contracts.
Inside `test web`, `test budgets`, and the R2/R3 `validate` gates, the Web
UI budget step and each firmware size check only record overruns under the
run's `artifacts/runs/<run-id>/budgets/`; the final `budget-verdict` step
fails once, listing all of them together with the command that accepts them.
Running `node src/tests/check_web_budgets.js` or a firmware build on its own
still fails on the first measurement over its limit.

`--budget-report` on `validate`, `test web`, or `test budgets` turns the
verdict into warnings and marks the run as not budget evidence, for work that
is still in development.

`./scripts/dev budget update --from <run-id>` accepts the sizes a finished
run measured: it raises only the exceeded limits in
`config/web-asset-budgets.json` and `config/resource-baselines.json`, prints
each change, and re-checks the run's measurements, without rebuilding. It
refuses report-mode, incomplete, or failed runs and any run whose recorded
tree (`HEAD`, working-tree fingerprint, and budget-file hashes) no longer
matches; see [Validation](../VALIDATION.md#size-budgets-and-parallel-work) for
the parallel-work rules.

Firmware builds and the Web contracts both rewrite the generated Web UI header
under `src/`, so one checkout runs them one at a time behind
`build-idf/.locks/source-tree.lock`; separate worktrees build in parallel.

## Diagnostics and recovery

Use `./scripts/dev doctor` to inspect local tooling and
`./scripts/dev context <area>` for focused repository paths. Use
`./scripts/dev classify` before a change and `./scripts/dev validate` for the
required validation gate. Missing dependencies are reported as failures;
scripts never install them automatically. `doctor` also reports each tool's
resolved path and version and warns when a Homebrew-managed tool is outdated,
so checks always run against the machine's current releases. It also shows
whether the optional `ccache` is installed: when it is, firmware builds use it
automatically, which makes rebuilds after a flag or configuration change much
faster. Set `IDF_CCACHE_ENABLE=0` to build without it.

Node.js and Cppcheck carry no project version pin: every script resolves them
from `PATH`, so install current releases with your package manager (see
[Build](BUILD.md#2-install-host-prerequisites)). `./scripts/dev analyze` needs
Cppcheck on `PATH`; see [Static analysis](STATIC_ANALYSIS.md) for local setup.
Firmware builds require ESP-IDF 6.1.0 and fail if the IDF component lock
changes during resolution. Review upgrades through [Build](BUILD.md#upgrade-dependencies-deliberately).

To regenerate the immutable time-zone table after reviewing a new IANA
release, place the official `tzdata2026d.tar.gz` archive in
`temp/ai_temp_timezone_impl/` and run:

```sh
python3 scripts/generate_timezones.py \
  temp/ai_temp_timezone_impl/tzdata2026d.tar.gz \
  src/OpenBrewByWeightTimeZoneData.h
```

The script checks the pinned archive SHA-256 and release version, uses the
local `zic` compiler, and writes the bounded 2025–2099 table. It never
downloads data. Review the generated diff and run the normal validation gate.
For a future release, update the pinned version and hash deliberately along
with boundary fixtures and resource measurements.

For a failed boot or a changed partition layout, reinstall the complete image
over USB with `--erase-all` and follow [Emergency recovery](EMERGENCY_RECOVERY.md).


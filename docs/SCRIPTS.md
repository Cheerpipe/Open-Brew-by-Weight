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

The Web UI sources in `src/web/` (HTML partials, the `css/` layer tree,
`app.js`, and `js/`) are kept readable with the Prettier version pinned in
`package.json`:
run `npm run format:web` after editing them. `./scripts/dev test web` starts
with the same check as `npm run format:web:check` and fails while any source
is out of style. The `css/` sources concatenate in the fixed layer order
that `CSS_SOURCES` in `scripts/localize_web_ui.js` defines, into the single
`/app.css` asset the generator emits. The generator (`scripts/gen_web_ui.js`) alone
compacts what ships: html-minifier-terser collapses markup whitespace, Lightning
CSS re-serializes the stylesheet before clean-css restructures it, and Terser
minifies the JavaScript. Lightning CSS targets the oldest supported browsers,
the first releases with `:has()` (Safari and iOS 15.4, Chrome 105, Firefox 121):
it adds the vendor prefixes they need and rewrites newer syntax such as range
media queries, so the CSS sources stay unprefixed and modern. Reformatting a source therefore never changes the
embedded bytes; the Web contracts print the sources at two widths and require
identical generator output.

## Web UI lint

`./scripts/dev test lint` checks the Web UI sources with the linters pinned in
`package.json`, so install them once with `npm ci`; the command never installs
anything and exits with 127 when they are missing. It runs three steps:

- **ESLint** on `src/web/app.js` and `src/web/js/` with its full recommended
  rule set (`scripts/lint/eslint.config.mjs`). The build-time constants and markers
  (`__WEBUI_TEXT__`, `__LINEA_MICRA__`, `__REMOTE_CONTROL__`, `__homeModule`) are
  declared there; formatting stays with `npm run format:web`.
- **Stylelint** on `src/web/css` (`scripts/lint/stylelint.json`) with the
  standard config; class, id, custom property and keyframe names must be
  lowerCamelCase.
- **Markup** (`scripts/lint_web_ui.js`): renders the English shell with every view
  partial injected, as the browser receives it, and runs html-validate with
  every recommended and accessibility rule (`scripts/lint/htmlvalidate.json`,
  which only sets the Prettier styles for void elements and the doctype). It
  also fails on any id used twice across views. The Web contracts check
  duplicate ids too, so every Web gate catches them without the lint.

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
bottom-navigation sample views, `/stats`, `/history`, `/settings`,
`/diagnostic`, and `/admin` open those same views directly, and `/micra` the
machine diagnostics, with the firmware stylesheet at `/app.css`. These names
always render the live views, even when a mockup with the same name exists.
Everything else is served from
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
each checked against its own size baseline and, right after it, against the
stack-depth gate of the destructive recovery paths, the settings_persist
shot pool worker and the deepest httpd requests (see
[Validation](../VALIDATION.md)). `--risk R3` also runs Cppcheck's
`warning` category on each pair, over only the translation units the change
reaches: the changed sources and every unit whose build dependencies include
a changed header. A change to `idf/`, the hardware or machine profiles, the
Cppcheck suppressions, or `scripts/static-idf` checks every unit, and a change
that reaches none skips Cppcheck. Every build already fails on any compiler
warning in project-owned code.

`./scripts/dev quality` is the non-functional quality gate, run on demand and
before each release tag. It builds every pair both ways, runs the full
Cppcheck analysis on each and the dead-code check over all of them, checks
that the design preview renders, and ends with the only budget verdict that
fails on an overrun.

R2 and R3 run parallel lanes: the host tests; the Web checks (route
capacity, Web UI budgets, Web contracts); and, for each profile pair, a
development lane (build, stack depth, Cppcheck on R3) and a release lane
(build, stack depth). The quality gate runs the Web UI budgets with the design
preview, the dead-code check, and the same two lanes per pair (build and
Cppcheck, release build). All six builds run in parallel, each with its share
of the CPUs (`IDF_PY_BUILD_JOBS`).
Each step's output appears in the log as one
block when the step finishes. After any failure no lane starts another step;
steps already running finish first. When every lane passes, a final
`budget-verdict` step reports every size budget the run measured. The release builds use their
own trees, `build-idf/release/<hardware>--<machine>/`, so the gate's
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
Inside `test web`, `test budgets`, the R2/R3 `validate` gates, and
`quality`, the Web UI budget step and each firmware size check only record
overruns under the run's `artifacts/runs/<run-id>/budgets/`; the final
`budget-verdict` step lists all of them together. It fails only in the
`quality` gate, which also prints the command that accepts them; the other
gates print the overruns as warnings and are not budget evidence.
Running `node src/tests/check_web_budgets.js` or a firmware build on its own
still fails on the first measurement over its limit.

`./scripts/dev budget update --from <run-id>` accepts the sizes a finished
run measured: it raises only the exceeded limits in
`config/web-asset-budgets.json` and `config/resource-baselines.json`, prints
each change, and re-checks the run's measurements, without rebuilding. It
refuses runs other than `quality`, incomplete or failed runs, and any run whose recorded
tree (`HEAD`, working-tree fingerprint, and budget-file hashes) no longer
matches; see [Validation](../VALIDATION.md#size-budgets-and-parallel-work) for
the parallel-work rules.

The Web UI generator keeps every Zopfli-compressed asset in
`build-host/zopfli-cache/` of the main checkout, which worktrees share, keyed
by the asset's bytes, the compression options, and the Zopfli version. It uses
a cached copy only after checking that it decompresses to the same bytes;
`./scripts/dev clean` in the main checkout removes it.
Each firmware build generates the Web UI in its own tree: a small stable
`<build>/generated/OpenBrewByWeightWebAssetsGzip.h` and the content in the
`.cpp` beside it, so a Web UI change does not recompile the network code. The
Web contracts write theirs under `build-host/webui/` and take turns behind
`build-idf/.locks/web-ui-header.lock`.
Two builds of the same tree take turns behind that tree's lock under
`<build root>/.locks/`; different trees build in parallel, and only their CMake
configures take turns behind `build-idf/.locks/idf-configure.lock`, because
each one runs the component manager over the shared `idf/` project.

## Diagnostics and recovery

Use `./scripts/dev doctor` to inspect local tooling and
`./scripts/dev context <area>` for focused repository paths. Use
`./scripts/dev classify` before a change and `./scripts/dev validate` for the
required validation gate. Missing dependencies are reported as failures;
scripts never install them automatically. `doctor` also reports each tool's
resolved path and version and warns when a Homebrew-managed tool is outdated,
so checks always run against the machine's current releases. It also shows
whether the optional `ccache` is installed, and warns when it is nearly full:
when it is installed, firmware builds use it automatically, and worktrees share
it (`CCACHE_BASEDIR` is the main checkout), which makes rebuilds after a flag
or configuration change and first builds in a new worktree much faster. Set
`IDF_CCACHE_ENABLE=0` to build without it.

Images name source files relative to the repository (`src/...`,
`./managed_components/...`), whatever the checkout, the build tree, or ccache;
a build fails if its image contains the checkout path, `$HOME`, or a path
relative to the build tree.

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


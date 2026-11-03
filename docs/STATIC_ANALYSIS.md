# Static analysis

How to prepare the tooling and run the static inspection suite for Open
Brew by Weight on macOS, Linux (Ubuntu) and Windows. Walkthrough from clone to
flash: [Build environment](BUILD.md). Script reference: [Build scripts](SCRIPTS.md).

## 1. What the suite runs

Cppcheck, clang-tidy and IWYU read the ESP-IDF compilation database
(`build-idf/<hardware>--<machine>/compile_commands.json`) from a normal build.
GCC `-fanalyzer` and the dead-code check are different: their scripts build
with analysis enabled. Compiler warnings need no separate tool: every build
compiles the project-owned components with `-Wall -Wextra -Wunused-parameter`
and fails on any warning they emit. Each tool deletes and recreates its own
reports directory on every run.

Before Cppcheck or clang-tidy starts, `audit_compile_commands.py` compares the
database, the production source tree and the versioned translation-unit manifest.
Every registered project C++ translation unit must occur exactly once; adding a `.cpp` without registering and reviewing it cannot silently
reduce coverage.

| Tool | Script | Reports directory | Contract |
|------|--------|-------------------|----------|
| Cppcheck | `./scripts/static-idf` | `reports/static-analysis/` | Fails (exit 1) on any warning/performance/portability/style finding |
| GCC `-fanalyzer` | `./scripts/gcc_analyzer` | `reports/gcc-analyzer/` | Fails on diagnostics in versioned code (builds with the analyzer enabled) |
| Dead functions | `./scripts/dead-code-idf` | `reports/dead-code/` | Fails on any project function that no validated build uses (builds every validated variant without inlining) |
| clang-tidy | `./scripts/static-tidy-idf` | `reports/static-tidy/` | Fails on in-scope diagnostics, parse errors, or a tool error |
| Include-What-You-Use | `./scripts/iwyu-idf` | `reports/iwyu/` | Advisory on suggestions: fails only when tooling is missing, the translation-unit audit fails, or nothing parses |

The Web UI has its own opt-in lint, `./scripts/dev test lint`: ESLint,
Stylelint and html-validate, pinned in `package.json` and installed with
`npm ci`, over `src/web/` only. It writes no reports directory, is not part of
`./scripts/dev validate`, and is described in [Web UI lint](SCRIPTS.md#web-ui-lint).

Analysis scope (identical for every tool): `src/`,
`libraries/EspressoScaleBLE/`, `idf/main/`, and `idf/components/` (the
project-owned components). ESP-IDF internals, `idf/managed_components/`,
`idf/third_party/` and Arduino cores are out of scope.

Concurrent host coverage runs real `std::thread` producers/readers under TSAN
for control snapshots, safety flags/timers, OTA state and JSON parsing.
ASan/UBSan cover the main functional, persistence, external safety, OTA,
and unique-resource harnesses. Target-only scheduler/radio
coverage is specified in [P2 target trace qualification](P2_TARGET_TRACE.md).

Prepare the database once, then run any tool:

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra
./scripts/static-idf --arch n16r8 \
  --build-dir build-idf/esp32-s3-relay-x1-speaker--la-marzocco-linea-micra
```

`--arch` is `n16r8`; see [Build scripts](SCRIPTS.md) for how
parameters resolve. `--build-dir` and `--output-dir` are one-shot overrides on
the analysis scripts and are never persisted to `.openbrewbyweight`. Without
`--build-dir`, the analysis scripts use the most recently built firmware
variant under `build-idf/` and fall back to the per-architecture directory
for older trees.

## 2. macOS prerequisites

After the [host prerequisites](BUILD.md#2-install-host-prerequisites), nothing
else is required: the analysis scripts resolve Cppcheck and Node from `PATH`.

```sh
cppcheck --version   # any current release, e.g. Cppcheck 2.22.0
```

Different compiler and Cppcheck versions can produce different findings;
`./scripts/dev doctor` shows the exact versions each run will use.

For optional IWYU checks, run `brew install include-what-you-use` separately.
`include-what-you-use` from Homebrew is built against the Homebrew `llvm`
formula, so the clang version always matches — do not mix it with another
clang. Xcode Command Line Tools (`xcode-select --install`) provide the host
compiler used by the host tests; IWYU does not need it.

## 3. Linux (Ubuntu) prerequisites

After the [Ubuntu 24.04 prerequisites](BUILD.md#2-install-host-prerequisites),
install the distribution's Cppcheck and verify it resolves on `PATH`:

```sh
sudo apt-get install cppcheck
cppcheck --version   # any current release
```

Newer Cppcheck releases can surface findings that older ones miss — for
example, `arrayIndexThenCheck` moved into `style` in 2.21 — so keep the
installed version current to avoid drift between machines.

IWYU is version-locked to the clang it was compiled against, and the Ubuntu
`iwyu` package is built against the distro clang, which may not be the clang
you have. Build it from source instead (replace the versions with your
installed clang):

```sh
sudo apt-get install clang-18 libclang-18-dev llvm-18-dev cmake ninja-build
git clone --branch clang_18 --depth 1 \
  https://github.com/include-what-you-use/include-what-you-use.git
cmake -S include-what-you-use -B iwyu-build -G Ninja \
  -DCMAKE_PREFIX_PATH=/usr/lib/llvm-18
cmake --build iwyu-build
sudo cp iwyu-build/bin/include-what-you-use /usr/local/bin/
```

Verify the pairing before running the analysis:

```sh
include-what-you-use --version   # must report the same clang as: clang --version
```

## 4. Windows prerequisites (native, no WSL)

These are tool-preparation notes, not a qualified end-to-end Windows build or
hardware-installation procedure. The documented build walkthrough covers
macOS/Linux. Verify tool paths and a compatible compilation database before
claiming Windows validation.

1. **Git for Windows** — the repo scripts are bash; run them from **Git Bash**
   (`winget install Git.Git`).
2. **ESP-IDF 6.1.0** — use [EIM](https://docs.espressif.com/projects/idf-im-ui/en/latest/)
   (or the VS Code extension backend) and select release v6.1.
   In Git Bash, point `IDF_PATH` at that SDK, or use the supported legacy clone
   at `%USERPROFILE%\esp\esp-idf-v6.1`. EIM also provides CMake, Ninja and the
   Xtensa GCC toolchain that produce the compilation database.
3. **Cppcheck** — install a current release from the
   [official releases](https://github.com/cppcheck-opensource/cppcheck/releases)
   or your package manager, put its directory on `PATH` in Git Bash, and
   verify `cppcheck --version`.
4. **Node.js** — needed by firmware image checks. Install a current release
   and verify `node --version`; see [Build](BUILD.md#2-install-host-prerequisites).

Windows notes:

- The analysis scripts resolve ESP-IDF and toolchain paths the same way as on
  macOS/Linux; inside Git Bash, `%USERPROFILE%` is `$HOME`.
- ESP-IDF's own `export.bat`/`export.ps1` are for CMD/PowerShell. The repo
  scripts do not source them; they only need the paths above to exist. For
  interactive `idf.py` work, use the **ESP-IDF 6.1 CMD**/PowerShell shortcuts
  EIM creates.
- IWYU is optional on Windows: there are no official prebuilt binaries, so
  build it from source with Visual Studio Build Tools against your clang, or
  skip `./scripts/iwyu-idf` (the other three tools work without it).

## 5. ESP-IDF and esp-clang

The firmware needs ESP-IDF **6.1.0** (tag v6.1):

Select v6.1 in EIM. From a fresh shell, use **Open IDF
Terminal** or source the activation script EIM printed, then confirm the
version before building the compilation database:

```sh
source "$HOME/.espressif/tools/activate_idf_v6.1.sh"  # use EIM's actual filename
idf.py --version
```

The analysis wrappers preserve a valid EIM-pinned Python environment. If it is
stale or mismatched, they discard it and use the supported legacy fallback:

```sh
mkdir -p "$HOME/esp" && cd "$HOME/esp"
git clone -b v6.1 --recursive https://github.com/espressif/esp-idf.git esp-idf-v6.1
cd esp-idf-v6.1 && ./install.sh esp32s3 && . ./export.sh   # macOS/Linux
```

clang-tidy must understand the `xtensa-esp32s3-elf` target. The stock clang
from Homebrew or apt does **not** — use Espressif's LLVM fork (`esp-clang`),
which ships a clang-tidy with Xtensa support:

```sh
python "$HOME/esp/esp-idf-v6.1/tools/idf_tools.py" install esp-clang
```

This installs into `~/.espressif/tools/esp-clang/` (all OSes, including the
Windows `x86_64-w64-mingw32` package); `./scripts/static-tidy-idf` finds it there
automatically. To use a specific build instead, point the script at it:

```sh
export ESP_CLANG_TIDY=/path/to/esp-clang/bin/clang-tidy
```

## 6. Running clang-tidy

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra
./scripts/static-tidy-idf --arch n16r8 \
  --build-dir build-idf/esp32-s3-relay-x1-speaker--la-marzocco-linea-micra
```

What the script does:

1. Rewrites the GCC compilation database into a clang-friendly one
   (`reports/static-tidy/compile_commands.json`): splices CMake `@response`
   files, drops GCC-only flags (`-fno-tree-*`, `-mdisable-hardware-atomics`,
   `-fanalyzer`, `-mno-target-align`, `-freorder-blocks`, `-mlongcalls`, …),
   adds the explicit `-target xtensa-esp32s3-elf`, and uses the selected
   picolibc headers with the GCC sysroot and libstdc++ include paths.
2. Runs esp-clang clang-tidy per translation unit, using the check set from
   `.clang-tidy` at the repository root.
3. Reports to `reports/static-tidy/tidy.txt` (full output, including
   third-party diagnostics) and `reports/static-tidy/README.txt` (summary).

Exit code: **1** when any in-scope file has diagnostics, failed to parse, or
clang-tidy exits unsuccessfully; **0** when clean. Missing picolibc specs or
headers also fail the run. Third-party advisory diagnostics alone are recorded
without failing the run; compiler and tool failures still fail. In-scope
diagnostics follow the `HeaderFilterRegex` in `.clang-tidy`, mirrored by the script.

### Configuration and suppressions policy

`.clang-tidy` enables `bugprone-*`, `performance-*`,
`clang-analyzer-core.*`/`cplusplus.*` and a few `modernize-*` checks with no
check disabled, and the codebase is clean against all of them. Findings are
fixed in the code, never silenced: the project uses no `// NOLINT` and no
`#pragma GCC diagnostic` to hide warnings. To grow the set:

1. Add the check to `Checks` in `.clang-tidy`.
2. Run `./scripts/static-tidy-idf -a n16r8` and fix every finding in the code.
3. If a finding looks impossible to fix without making the code worse, raise
   it for review before changing any configuration.

**Caution with `clang-tidy -fix`:** fixes are applied for every file that
contributed diagnostics, including headers outside this repository (the
ESP-IDF checkout itself). If you use `-fix`, review `git status` in **both**
this repository and `$HOME/esp/esp-idf-v6.1` afterwards and revert any changes
under `esp-idf/`.

## 7. Running Include-What-You-Use

```sh
./scripts/dev build --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra
./scripts/iwyu-idf --arch n16r8 \
  --build-dir build-idf/esp32-s3-relay-x1-speaker--la-marzocco-linea-micra
```

IWYU is **advisory**: the script sanitizes the database for the host compiler
(no Xtensa target in stock IWYU), runs it per translation unit, and collects
suggestions into `reports/iwyu/iwyu.txt`. Translation units that reach
Xtensa-specific headers may fail to parse on the host; they are skipped and
listed, which is expected. The run only fails when IWYU is missing or nothing
parsed at all. Like the other tools, it first audits the database against the
project translation-unit manifest, so a database that misses project files
fails before IWYU runs.

Workflow for the suggestions:

1. Review `reports/iwyu/iwyu.txt` — do not apply blindly: embedded builds care
   about include weight, and ESP-IDF headers sometimes need the C spelling.
2. Apply selected fixes with the `fix_includes.py` shipped next to your IWYU
   build, or by hand.
3. Rebuild with the same hardware and machine profiles and re-run the host tests
   (`src/tests/run_host_tests.sh`) before committing.

The mapping file `scripts/iwyu-openbrewbyweight.imp` is intentionally empty: stock
IWYU already maps the C standard headers to their C++ wrappers, and
re-mapping them aborts IWYU. Add entries there only when IWYU repeatedly
suggests something wrong for ESP-IDF/Arduino headers in this codebase, and
document each one — note that `<stdint.h>` must **not** be mapped to
`<cstdint>` (libstdc++ ships both as mutual wrappers and IWYU aborts on the
double visibility).

## 8. Running Cppcheck and the GCC analyzer

```sh
cppcheck --version          # current release from PATH; see sections 2 or 3
./scripts/static-idf --arch n16r8 \
  --build-dir build-idf/esp32-s3-relay-x1-speaker--la-marzocco-linea-micra
./scripts/gcc_analyzer --hardware esp32-s3-relay-x1-speaker \
  --machine la-marzocco-linea-micra
```

`static-idf` never builds, checks the `warning`, `performance`, `portability`
and `style` categories, and fails on any finding. Fix the code first. A finding
that is correct by design, such as a condition that a build profile makes
constant or a callback signature fixed by the SDK, gets a single-line
`// cppcheck-suppress <id> ; <reason>` comment directly above the reported
line, one id per comment, with the concrete technical reason. File-wide,
block or global suppressions are not used for project code. The suppression
list `scripts/cppcheck-suppressions.txt` only excludes third-party dependency
paths.
Dependency suppressions identify the vendor header instead of a local ESP-IDF
installation root, so an ESP-IDF checkout at `/opt/esp/idf` and a developer
checkout behave the
same without suppressing diagnostics in versioned project code.
`gcc_analyzer` **builds** the firmware with `-fanalyzer` enabled and keeps
only diagnostics whose primary location is versioned code.

Cppcheck's `unusedFunction` is not used: it cannot follow member calls across
translation units, so it reports functions the firmware does call. The
dead-code check below replaces it.

### Dead functions

```sh
./scripts/dead-code-idf
```

The check answers which project functions no firmware image uses, and runs
in every R3 validation (`./scripts/dev validate`). A normal
build cannot tell: it inlines small functions and discards their out-of-line
copies even though their code runs. So the script compiles every profile pair
that R2/R3 validation builds twice, each in its own tree under
`build-idf/dead-code/<profile>/`: as a development image that also enables the
remote machine control opt-in, so code only that supported build runs counts,
and as a release image. The gate's `build-idf/<hardware>--<machine>/` trees
are untouched. Only the project components get the analysis options
(`-fno-inline -fkeep-inline-functions -fno-ipa-sra -fno-partial-inlining
-fno-ipa-cp-clone -fno-ipa-icf -fno-ipa-pure-const -fno-ipa-modref`): no call
is inlined, cloned, redirected to an identical twin, or dropped because the
callee does nothing in that build. Only the inputs of the application link are
built; the ELF is never linked, so code without inlining never has to fit the
image.

`scripts/dead_code.py` then reads the project objects with the toolchain's
`readelf` and `nm`. A function is used when a relocation in another section of
a project object reaches it (a call, a function pointer, a vtable, a static
initializer), when a library outside the project leaves its name undefined or
defines it weak (an override such as `app_main`, `setup`, or an ESP-IDF hook),
when the link command or a linker script names it (`-u`, `--wrap`, `ENTRY`,
`EXTERN`), or when a linker script keeps its section. A function that only
calls itself is not used. Two cases have no trace in the objects and are read
from elsewhere: bodies the compiler writes (implicit and `= default` members,
lambda conversion operators) are recognized from their debug information and
never reported, and a `constexpr` function counts as used when firmware
sources outside the host tests name it, because the compiler evaluates such
calls in constant expressions without emitting one. Every entry point comes
from the build: the check has no list of known functions and no suppressions.

A function defined in project code and used in none of the builds is listed in
`reports/dead-code/dead-code.txt` as `file:line: name`, and the script exits 1.
Delete it, or, when only host tests call it, make it test-only. A build failure
keeps its own exit status and an analysis error exits 2; the build logs stay
next to the report. `--hardware` and `--machine` restrict a quick run to one
pair; that run also reports functions only the other pairs use. A full run
takes about ten minutes.

## 9. Recommended order for a release check

```sh
./scripts/dev validate --risk R3
src/tests/run_host_tests.sh
```

## 10. Troubleshooting

- **`unknown argument '-mlongcalls'` / `unknown target`** — you are using a
  stock clang-tidy (Homebrew/apt) against the Xtensa database. Install
  esp-clang (section 5) or point `ESP_CLANG_TIDY` at it.
- **`clang-tidy (esp-clang) not found`** — run
  `python "$HOME/esp/esp-idf-v6.1/tools/idf_tools.py" install esp-clang`, or set
  `ESP_CLANG_TIDY`.
- **IWYU version mismatch** (instant assertion or `unsupported option` on
  ordinary code) — IWYU was built against a different clang than the one it
  runs with. Rebuild IWYU against your clang or point `IWYU_BINARY` at a
  matching build.
- **`'new' / 'algorithm' file not found`** in the clang-tidy report — the
  GCC sysroot or libstdc++ include paths could not be resolved from the
  compilation database. Rebuild the firmware for that architecture first so
  the database and the toolchain on disk agree; if it persists, compare
  `xtensa-esp32s3-elf-g++ -print-sysroot` output with the sysroot embedded in
  the sanitized database under `reports/static-tidy/`.
- **A whole architecture fails in clang-tidy after changing build flags** —
  stale `CMakeCache.txt` in the build tree can leak analyzer-only flags into
  the database (e.g. `-fanalyzer`). Delete `build-idf/<arch>/CMakeCache.txt`
  and rebuild.
- **Windows: `command not found` inside Git Bash** — the tool was installed
  but its directory is not on the Git Bash `PATH`. Prefer `winget`/installer
  paths that update the system `PATH`, then reopen Git Bash.

#!/usr/bin/env python3
"""Golden contracts for the developer facade."""

from __future__ import annotations

import json
import subprocess
from contextlib import contextmanager
from pathlib import Path
import os
import re
import csv
import io
import runpy
import shutil
from tempfile import mkdtemp
import sys
import tempfile
from unittest.mock import patch

if not __debug__:
    raise SystemExit("assertions are disabled (PYTHONOPTIMIZE); these checks would not run")

ROOT = Path(__file__).resolve().parents[2]
DEV = ROOT / "scripts/dev"
INTERNAL = ROOT / "scripts/internal"


# Check rendered-document targets without depending on the working tree's docs.
dev_module = runpy.run_path(str(DEV))
doc_check = dev_module["markdown_errors"]
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-docs-") as temporary:
    fixture = Path(temporary)
    (fixture / "docs").mkdir()
    (fixture / "libraries/EspressoScaleBLE").mkdir(parents=True)
    (fixture / "docs/guide.md").write_text(
        '# Guide\n## Repeated\n## Repeated\n<a id="old-section"></a>\n')
    (fixture / "docs/photo.png").write_bytes(b"test fixture")
    (fixture / "docs/with space.md").write_text('# Spaced\n')
    (fixture / "README.md").write_text(
        '# Home\n[wrapped\nlink](docs/guide.md#repeated-1)\n'
        '[alias](docs/guide.md#old-section)\n[space](<docs/with space.md#spaced>)\n'
        '![photo](docs/photo.png)\n[local](#home)\n'
        '[external](https://example.org/missing#ignored)\n'
        '```md\n[example](missing-example.md)\n```\n')
    for directory in ("temp/ai_temp_check", "docs/plans", "docs/audits/nested"):
        local = fixture / directory
        local.mkdir(parents=True)
        (local / "review.md").write_text('# Local record\n[broken](missing.md)\n')
    with patch.dict(doc_check.__globals__, ROOT=fixture, AREAS={}):
        assert doc_check() == [], doc_check()
        canonical = fixture / "docs/audits-guide.md"
        canonical.write_text('# Canonical guide\n[broken](missing.md)\n')
        errors = doc_check()
        assert len(errors) == 1 and 'docs/audits-guide.md:' in errors[0], errors
        canonical.write_text('# Canonical guide\n')
        (fixture / "libraries/EspressoScaleBLE/README.md").write_text(
            '# Library\n[bad](../../docs/guide.md#missing)\n'
            '[ref]: ../../docs/absent.md\n<img src="missing.png">\n')
        errors = doc_check()
        assert len(errors) == 3, errors
        assert any('missing anchor' in error for error in errors), errors
        assert all('libraries/EspressoScaleBLE/README.md:' in error for error in errors)


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(DEV), "--verbosity", "compact", *args],
                          cwd=ROOT, text=True, capture_output=True)


golden = {
    ".gitignore": "R0",
    ".git-blame-ignore-revs": "R0",
    "README.md": "R0",
    "src/web/app.js": "R1",
    "scripts/dev": "R1",
    "libraries/EspressoScaleBLE/src/ScaleProtocol.h": "R2",
    "src/OpenBrewByWeightNetwork.cpp": "R2",
    "src/OpenBrewByWeightOta.cpp": "R2",
    "src/OpenBrewByWeightMachineRelay.h": "R3",
    "idf/partitions-n16r8.csv": "R3",
    "unknown/new.file": "R3",
}
for path, expected in golden.items():
    result = run("classify", path)
    assert result.returncode == 0, result.stderr
    assert f"risk={expected}" in result.stdout, (path, result.stdout)

lower = run("validate", "--risk", "R0", "src/OpenBrewByWeightMachineRelay.h")
assert lower.returncode == 2 and "cannot lower" in lower.stderr

unsafe = run("flash")
assert unsafe.returncode == 2 and "requires --confirm" in unsafe.stderr
secret = run("ota", "--confirm", "--password", "do-not-log")
assert secret.returncode == 2 and "never argv" in secret.stderr
obsolete = run("ota", "--confirm", "--force")
assert obsolete.returncode == 2 and "was removed" in obsolete.stderr
for invalid in (("build", "monitor"), ("build", "flash", "ota", "--confirm"),
                ("monitor", "--host", "controller.local"),
                ("build", "flash", "--confirm", "--image", "firmware.bin")):
    result = run(*invalid)
    assert result.returncode == 2, (invalid, result.stdout, result.stderr)


def captured_firmware(*args: str, stdin: str = "") -> dict:
    captured = {}

    def capture(command, risk, steps, verbosity, manual=None, prelude=None,
                interactive=False, env_extra=None):
        captured.update(command=command, risk=risk, steps=steps, manual=manual,
                        interactive=interactive, env_extra=env_extra or {})
        return 0

    main = dev_module["main"]
    with patch.dict(main.__globals__, execute=capture), \
            patch.object(sys, "argv", [str(DEV), *args]), \
            patch.object(sys, "stdin", io.StringIO(stdin)):
        assert main() == 0
    return captured


for pipeline in (("build",), ("flash",), ("monitor",),
                 ("build", "flash"),
                 ("flash", "monitor"),
                 ("build", "flash", "monitor"),
                 ):
    invocation = list(pipeline)
    if {"flash", "ota"}.intersection(pipeline):
        invocation.append("--confirm")
    captured = captured_firmware(*invocation)
    child = captured["steps"][0][1]
    assert child[:len(pipeline) + 1] == [
        "./scripts/internal/firmware-idf", *pipeline], (pipeline, child)
    assert "--confirm" not in child
    assert "--flags=" not in child

jtag_forwarded = captured_firmware("build", "--jtag")
assert jtag_forwarded["steps"][0][1][-2:] == ["--", "--jtag"]
jtag_rejected = run("monitor", "--jtag")
assert jtag_rejected.returncode == 2 and "does not apply" in jtag_rejected.stderr
flash_jtag_rejected = run("flash", "monitor", "--jtag")
assert flash_jtag_rejected.returncode == 2 and "does not apply" in flash_jtag_rejected.stderr
opt_forwarded = captured_firmware("build", "--os")
assert opt_forwarded["steps"][0][1][-2:] == ["--", "--os"]
force_forwarded = captured_firmware("build", "--force-sdkconfig-regenerate")
assert force_forwarded["steps"][0][1][-2:] == ["--", "--force-sdkconfig-regenerate"]

# The Web UI design preview server is official tooling dispatched by the facade.
preview_captured = captured_firmware("preview", "--port", "4174")
assert preview_captured["steps"][0][1] == ["node", "scripts/webui-preview/server.js"]
assert preview_captured["env_extra"] == {"PORT": "4174"}, preview_captured
assert captured_firmware("preview")["env_extra"] == {}
assert (ROOT / "scripts/webui-preview/server.js").is_file()
# Its reference pages and preview scripts must render from the current sources.
preview_scripts = subprocess.run(["node", str(ROOT / "src/tests/webui_preview_test.js")],
                                 cwd=ROOT, text=True, capture_output=True)
assert preview_scripts.returncode == 0, preview_scripts.stderr
for invalid_preview in (("preview", "extra"), ("preview", "--port", "0"),
                        ("preview", "--port", "70000")):
    rejected_preview = run(*invalid_preview)
    assert rejected_preview.returncode == 2, (invalid_preview, rejected_preview.stderr)
for stage in ("flash", "ota", "monitor"):
    rejected = run(stage, "--force-sdkconfig-regenerate")
    assert rejected.returncode == 2 and "does not apply" in rejected.stderr
# The repository default optimization level is -O2/PERF: sdkconfig.defaults
# selects it and every script fallback agrees.
defaults_text = (ROOT / "idf" / "sdkconfig.defaults").read_text()
assert "CONFIG_COMPILER_OPTIMIZATION_PERF=y" in defaults_text
assert "CONFIG_COMPILER_OPTIMIZATION_SIZE=y" not in defaults_text
internal_build = (INTERNAL / "build-idf").read_text()
assert internal_build.count("OBBW_IDF_OPT_LEVEL_KCONFIG=CONFIG_COMPILER_OPTIMIZATION_PERF") == 2
assert "OBBW_IDF_OPT_LEVEL_KCONFIG=CONFIG_COMPILER_OPTIMIZATION_SIZE" in internal_build
firmware_dispatcher = (INTERNAL / "firmware-idf").read_text()
assert 'Build output folder: %s\\n' in firmware_dispatcher
assert 'Flash-ready firmware: %s/openbrewbyweight.bin\\n' in firmware_dispatcher
idf_helper = (INTERNAL / ".." / "openbrewbyweight_idf.sh").resolve().read_text()
assert idf_helper.count("-CONFIG_COMPILER_OPTIMIZATION_PERF}") == 2
assert idf_helper.count("-CONFIG_COMPILER_OPTIMIZATION_SIZE}") == 0

# Selected defaults are content-sensitive and refresh only the locked variant.
(ROOT / "temp").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="ai_temp_sdkconfig_", dir=ROOT / "temp") as temporary:
    fixture = Path(temporary)
    project = fixture / "idf"
    (project / "main").mkdir(parents=True)
    shared = project / "sdkconfig.defaults"
    overlay = fixture / "optlevel"
    kconfig = project / "main/Kconfig.projbuild"
    shared.write_text("CONFIG_TEST_A=y\n")
    overlay.write_text("CONFIG_COMPILER_OPTIMIZATION_PERF=y\n")
    kconfig.write_text("config TEST_A\n    default y\n")

    def config_helper(variant: str, action: str, fingerprint: str = "", force: bool = False):
        build = fixture / "build-idf" / variant
        build.mkdir(parents=True, exist_ok=True)
        env = {**os.environ, "IDF_PROJECT": str(project),
               "IDF_SDKCONFIG_DEFAULTS": f"{shared};{overlay}",
               "IDF_BUILD_DIR": str(build), "IDF_SDKCONFIG": str(build / "sdkconfig"),
               "OPENBREWBYWEIGHT_ARCH": "n16r8", "OPENBREWBYWEIGHT_VARIANT": variant,
               "OPENBREWBYWEIGHT_MACHINE_INTEGRATION": "none",
               "OPENBREWBYWEIGHT_EXTRA_FLAGS": "", "OBBW_CLI_FORCE_SDKCONFIG_REGENERATE": str(int(force))}
        return subprocess.run(
            ["bash", "-c", f'source "{ROOT / "scripts/openbrewbyweight_idf.sh"}"; '
             '"$1" "$2"', "sdkconfig-test", action, fingerprint],
            cwd=ROOT, env=env, text=True, capture_output=True)

    def digest(variant="pair-a"):
        result = config_helper(variant, "obbw_idf_config_fingerprint")
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    baseline = digest()
    assert baseline != digest("pair-b")
    for variant in ("pair-a", "pair-b"):
        build = fixture / "build-idf" / variant
        (build / "sdkconfig").write_text("CONFIG_TEST_A=y\n")
        (build / "CMakeCache.txt").write_text("IDF_TARGET:STRING=esp32s3\n")
        (build / "sdkconfig.inputs.sha256").write_text(digest(variant) + "\n")
    a = fixture / "build-idf/pair-a"
    b = fixture / "build-idf/pair-b"
    assert config_helper("pair-a", "obbw_idf_refresh_config", baseline).returncode == 0
    assert (a / "sdkconfig").exists() and (a / "CMakeCache.txt").exists()
    shared.write_text("CONFIG_TEST_A=y\nCONFIG_TEST_B=y\n")
    added = digest()
    assert added != baseline
    refreshed = config_helper("pair-a", "obbw_idf_refresh_config", added)
    assert "local menuconfig choices will be discarded" in refreshed.stdout
    assert not (a / "sdkconfig").exists() and not (a / "CMakeCache.txt").exists()
    assert (b / "sdkconfig").exists() and (b / "CMakeCache.txt").exists()
    shared.write_text("CONFIG_TEST_B=y\n")
    removed = digest()
    assert removed != added  # Removing a managed default also changes the input set.
    overlay.write_text("CONFIG_COMPILER_OPTIMIZATION_SIZE=y\n")
    changed_overlay = digest()
    assert changed_overlay != removed
    kconfig.write_text("config TEST_A\n    default n\n")
    assert digest() != changed_overlay
    (a / "sdkconfig").write_text("CONFIG_TEST_A=y\n")
    (a / "CMakeCache.txt").write_text("IDF_TARGET:STRING=esp32s3\n")
    (a / "sdkconfig.inputs.sha256").unlink()
    assert "no configuration fingerprint" in config_helper(
        "pair-a", "obbw_idf_refresh_config", digest()).stdout
    assert not (a / "sdkconfig").exists()
    (b / "sdkconfig").write_text("CONFIG_TEST_A=y\n")
    assert "requested by" in config_helper(
        "pair-b", "obbw_idf_refresh_config", digest("pair-b"), force=True).stdout
    assert not (b / "sdkconfig").exists()

assert internal_build.index('obbw_idf_verify_firmware') < internal_build.index(
    'printf \'%s\\n\' "$config_fingerprint" > "$fingerprint_file"')

# The resource verifier must validate measurements at every optimization level.
(ROOT / "temp").mkdir(exist_ok=True)
with tempfile.TemporaryDirectory(prefix="ai_temp_size_check_", dir=ROOT / "temp") as temporary:
    build = Path(temporary)
    image = build / "openbrewbyweight.bin"
    image.write_bytes(b"x")
    (build / "openbrewbyweight.map").write_text(
        "0x3c000000 _ext_ram_bss_start\n0x3c000010 _ext_ram_bss_end\n"
        "0x3fc80000 localBuzzer\n0x3fc80010 taskProfiler\n"
        "0x3fc80020 publishedControlStatus\n0x3fc80030 controlStatusMutex\n"
        "0x3fc80040 publishedControlGate\n0x3fc80050 controlGateMutex\n"
        "0x3c000004 publishedControlCurve\n0x3c000008 taskProfilerReport\n")
    size = build / "size.json"
    metrics = {"total_size": 1, "used_diram": 1, "flash_code": 1, "flash_rodata": 1}
    size.write_text(json.dumps(metrics))
    sdkconfig = build / "sdkconfig"

    def check_size(arch="n16r8", development=False, env=None):
        args = ["node", str(ROOT / "src/tests/check_firmware_size.js"),
                str(image), "--arch", arch,
                "--development" if development else "--release"]
        return subprocess.run(args, cwd=ROOT, env={**os.environ, **(env or {})},
                              text=True, capture_output=True)

    sdkconfig.write_text("CONFIG_COMPILER_OPTIMIZATION_PERF=y\n")
    release_profile = check_size()
    assert release_profile.returncode == 0
    assert "budget evidence requires a --development build" in release_profile.stdout
    memory_map = build / "openbrewbyweight.map"
    valid_map = memory_map.read_text()
    memory_map.write_text(valid_map.replace("0x3c000004 publishedControlCurve",
                                           "0x3fc80100 publishedControlCurve"))
    assert "publishedControlCurve must reside in external BSS" in check_size().stderr
    memory_map.write_text(valid_map.replace("0x3fc80020 publishedControlStatus",
                                           "0x3c000008 publishedControlStatus"))
    assert "publishedControlStatus must remain in internal SRAM" in check_size().stderr
    memory_map.write_text(valid_map)
    size.write_text("{}")
    invalid = check_size()
    assert invalid.returncode != 0 and "total_size is missing or invalid" in invalid.stderr
    size.write_text(json.dumps(metrics))
    sdkconfig.write_text("CONFIG_COMPILER_OPTIMIZATION_UNKNOWN=y\n")
    assert "must select one supported level" in check_size().stderr
    sdkconfig.write_text("CONFIG_COMPILER_OPTIMIZATION_PERF=y\n")
    size.write_text(json.dumps({**metrics, "total_size": 3_000_000}))
    assert check_size().returncode == 0
    assert "total_size 3000000 > baseline budget" in check_size(development=True).stderr
    # A deferring gate records the overrun for its final verdict instead.
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-budget-run-") as run_dir:
        deferred = check_size(development=True, env={
            "OPENBREWBYWEIGHT_BUDGET_DEFER": "1", "OPENBREWBYWEIGHT_RUN_DIR": run_dir})
        assert deferred.returncode == 0 and "BUDGET EXCEEDED total_size: 3000000" in deferred.stdout
        recorded = json.loads(next(Path(run_dir, "budgets").glob(
            "firmware-development-*.json")).read_text())
        assert recorded["target"] == "targets.n16r8" and {
            "metric": "total_size", "value": 3_000_000,
            "baseline": json.loads((ROOT / "config/resource-baselines.json").read_text())
            ["targets"]["n16r8"]["total_size"]} in recorded["rows"], recorded
    sdkconfig.write_text("CONFIG_COMPILER_OPTIMIZATION_SIZE=y\n")
    assert check_size().returncode == 0
    assert check_size(development=True).returncode == 0
    image.write_bytes(b"x" * 3_145_729)
    assert "image 3145729 > OTA slot 3145728" in check_size().stderr
    # A flag-free release build of an official pair compares its own baseline.
    with tempfile.TemporaryDirectory(prefix="ai_temp_release_size_", dir=ROOT / "temp") as other:
        image.write_bytes(b"x")
        sdkconfig.write_text("CONFIG_COMPILER_OPTIMIZATION_PERF=y\n")
        size.write_text(json.dumps({**metrics, "total_size": 3_000_000}))
        official = Path(other) / "esp32-s3-relay-x1-speaker--rancilio-silvia-pro-x"
        image = shutil.copytree(build, official) / image.name
        assert "total_size 3000000 > baseline budget" in check_size().stderr
        (image.parent / "size.json").write_text(json.dumps(metrics))
        assert "within versioned budgets" in check_size().stdout

for level in ("--o0", "--og", "--o2", "--os"):
    rejected = run("flash", level)
    assert rejected.returncode == 2 and "does not apply" in rejected.stderr, level

for area in ("safety", "control", "machine", "scale", "ble", "network",
             "ota", "persistence", "web", "build", "tests"):
    result = run("context", area)
    assert result.returncode == 0 and area in result.stdout, (area, result.stderr)

dry_run = run("clean", "--dry-run")
assert dry_run.returncode == 0 and "build-host" in dry_run.stdout

# Parallel-agent hardening: gates must name changed paths outside an explicit
# scope instead of silently classifying only what was passed.
assert dev_module["uncovered_changes"]([]) == []
assert dev_module["uncovered_changes"](["."]) == []
assert (dev_module["uncovered_changes"](["./scripts/dev"])
        == dev_module["uncovered_changes"](["scripts/dev"]))

# Compact-mode failure hint: a failed gate must summarize the failing check
# and the likeliest cause instead of making the reader dig through the tail.
failed_line = dev_module["failed_check_line"]
hint = failed_line([{"name": "build-normal", "status": "failed"}],
                   ["==> build-normal: cmake --build build-host",
                    "src/X.h:37:15: error: something broke"], "failed")
assert hint == ("failed check: build-normal — src/X.h:37:15: error: "
                "something broke"), hint
assert "cmake: MISSING" in failed_line(
    [{"name": "doctor", "status": "failed"}], ["cmake: MISSING"],
    "missing_dependency")

# Checkout-wide build serialization: build-idf must re-exec through with-flock
# holding the source-tree lock the Web contracts share, and the helper must be
# the portable fcntl holder.
with_flock = INTERNAL / "with-flock"
assert with_flock.is_file() and with_flock.stat().st_mode & 0o111
assert "fcntl.flock" in with_flock.read_text()
assert "set_inheritable" in with_flock.read_text(), (
    "PEP 446 close-on-exec would release the lock before the build starts")
assert "OBBW_BUILD_LOCKED" in internal_build and "with-flock" in internal_build
assert '"$root/build-idf/.locks/source-tree.lock"' in internal_build
assert dev_module["SOURCE_TREE_LOCK"] == "build-idf/.locks/source-tree.lock"
assert dev_module["WEB_CONTRACTS_STEP"][1][:3] == [
    "python3", "scripts/internal/with-flock", dev_module["SOURCE_TREE_LOCK"]]

# Serialization actually blocks: a held lock must delay a second holder.
import time as _time
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-flock-probe-") as flock_dir:
    lock_path = Path(flock_dir) / "probe.lock"
    first = subprocess.Popen(
        [sys.executable, str(with_flock), str(lock_path), "sleep", "2"])
    _time.sleep(0.5)
    started = _time.monotonic()
    second = subprocess.run(
        [sys.executable, str(with_flock), str(lock_path), "true"])
    elapsed = _time.monotonic() - started
    first.wait()
    assert first.returncode == 0 and second.returncode == 0
    assert elapsed >= 1.0, f"lock did not serialize (waited {elapsed:.2f}s)"


@contextmanager
def idf_doctor_fixture(with_install: bool, with_venv: bool):
    """Create a sandbox HOME with an optional ESP-IDF checkout and python_env."""
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-doctor-idf-") as temporary:
        root = Path(temporary)
        fallback = root / "esp/esp-idf-v6.1"
        installed_env = root / ".espressif/python_env/idf6.1_py3.14_env"
        missing_env = root / ".espressif/python_env/idf6.1_py3.11_env"
        if with_install:
            (fallback / "components/esp_common/include").mkdir(parents=True)
            (fallback / "export.sh").write_text("#!/bin/sh\n")
            (fallback / "components/esp_common/include/esp_idf_version.h").write_text(
                "#define ESP_IDF_VERSION_MAJOR 6\n"
                "#define ESP_IDF_VERSION_MINOR 1\n")
        if with_venv:
            (installed_env / "bin").mkdir(parents=True, exist_ok=True)
            (installed_env / "bin/python").write_text("#!/bin/sh\nexit 0\n")
            (installed_env / "bin/python").chmod(0o755)
            (installed_env / "idf_version.txt").write_text("6.1")
        missing_env.mkdir(parents=True, exist_ok=True)
        yield root, fallback, installed_env, missing_env


def idf_environment_probe(root: Path, extra_env: dict[str, str] | None = None):
    env = os.environ.copy()
    env.update(HOME=str(root), OBBW_CLI_ROOT=str(root))
    for name in ("IDF_PATH", "IDF_PYTHON_ENV_PATH", "ESP_PYTHON",
                 "ESP_IDF_VERSION", "IDF_DEACTIVATE_FILE_PATH"):
        env.pop(name, None)
    env.update(extra_env or {})
    command = (
        f'source "{ROOT / "scripts/openbrewbyweight_board.sh"}"; '
        f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
        f'source "{ROOT / "scripts/openbrewbyweight_idf.sh"}"; '
        'obbw_idf_environment_report')
    return subprocess.run(["bash", "-c", command], env=env, cwd=ROOT,
                          capture_output=True, text=True)


def doctor_with_probe_stdout(probe_stdout: str) -> list[str]:
    """Render doctor output for a canned environment probe result."""
    captured = {}

    def capture(command, risk, verbosity, lines, code=0, check="contract"):
        captured.update(lines=list(lines))
        return 0

    class FakeSubprocess:
        @staticmethod
        def run(*args, **kwargs):
            return subprocess.CompletedProcess(args=[], returncode=0,
                                               stdout=probe_stdout, stderr="")

    main = dev_module["main"]
    with patch.dict(main.__globals__, {"subprocess": FakeSubprocess,
                                       "record": capture}), \
            patch.object(sys, "argv", [str(DEV), "doctor"]):
        assert main() == 0
    return captured["lines"]


for with_install, with_venv, extra, expected_idf, expected_venv, mismatched in (
        (True, True, None, True, "auto-selected installed venv", False),
        (True, True, {"IDF_PYTHON_ENV_PATH": "ACTIVE"}, True, "active environment", False),
        (True, False, None, True, "resolved by export.sh at build time", False),
        (True, True, {"IDF_PYTHON_ENV_PATH": "STALE"}, True,
         "auto-selected installed venv", True),
        (False, True, None, False, None, False)):
    with idf_doctor_fixture(with_install, with_venv) as (root, fallback,
                                                         installed_env,
                                                         missing_env):
        extra_env = {name: (str(installed_env) if value == "ACTIVE" else
                            str(missing_env) if value == "STALE" else value)
                     for name, value in (extra or {}).items()}
        probe = idf_environment_probe(root, extra_env)
        assert (probe.returncode == 0) == expected_idf, probe.stderr
        lines = doctor_with_probe_stdout(probe.stdout)
        if expected_idf:
            assert any(line == f"ESP-IDF: {fallback}" +
                       (" (venv mismatch)" if mismatched else "")
                       for line in lines), lines
            assert any(expected_venv in line and
                       line.startswith("IDF Python environment: ")
                       for line in lines), lines
            assert any("venv mismatch" in line for line in lines) == mismatched, lines
        else:
            assert "ESP-IDF: not found; install 6.1.x per docs/BUILD.md" in lines, lines

live_doctor = run("doctor")
assert live_doctor.returncode == 0 and "ESP-IDF:" in live_doctor.stdout, \
    live_doctor.stderr

for script in (ROOT / "scripts").rglob("*"):
    if not script.is_file() or script.suffix == ".js":
        continue
    first = script.read_text(errors="replace").splitlines()[0]
    if first.startswith("#!") and "sh" in first:
        checked = subprocess.run(["bash", "-n", str(script)], capture_output=True)
        assert checked.returncode == 0, f"shell syntax: {script.name}"


def partition_rows(name: str) -> dict[str, tuple[int, int]]:
    with (ROOT / "idf" / name).open(newline="") as handle:
        rows = {}
        for row in csv.reader(line for line in handle if not line.startswith("#")):
            if not row:
                continue
            rows[row[0].strip()] = (int(row[3].strip(), 0),
                                    int(row[4].strip(), 0))
        return rows


partition_contracts = {
    "partitions-n16r8.csv": {
        "flash": 0x1000000,
        "rows": {"nvs": (0x9000, 0x65000), "otadata": (0x6E000, 0x2000),
                 "app0": (0x70000, 0x300000), "app1": (0x370000, 0x300000),
                 "history": (0x670000, 0x8000),
                 "scaleprof": (0x678000, 0x41000),
                 "auditlog": (0x6B9000, 0x10000),
                 "shotlog": (0x6C9000, 0xFC000),
                 "shotcurve": (0x7C5000, 0x3EC000),
                 "reserved": (0xBB1000, 0x24F000),
                 "coredump": (0xE00000, 0xA0000),
                 "crashhist": (0xEA0000, 0x160000)},
    },
}
for filename, contract in partition_contracts.items():
    rows = partition_rows(filename)
    text = (ROOT / "idf" / filename).read_text()
    assert rows == contract["rows"], (filename, rows)
    assert "shotcurve,data, 0x40" in text, "dedicated curve subtype changed"
    assert "scaleprof,data, 0x40" in text, "dedicated scale-profile subtype changed"
    ordered = sorted(rows.items(), key=lambda item: item[1][0])
    for (_, (offset, size)), (_, (next_offset, _)) in zip(ordered, ordered[1:]):
        assert offset + size == next_offset, f"partition gap or overlap in {filename}"
    last_offset, last_size = ordered[-1][1]
    assert last_offset + last_size == contract["flash"]
    assert rows["shotcurve"][1] == 4016 * 1024, "shot-curve partition changed"
    assert rows["shotlog"][1] == 1008 * 1024, "shot-log partition changed"
    assert rows["history"][1] == 32 * 1024, "activation-history partition changed"
    assert rows["scaleprof"][1] == 0x41000, "scale-profile partition changed"

flash_idf = (INTERNAL / "flash-idf").read_text()
for required in ("read-flash 0x8000 0x1000", "installed_layout=blank",
                 'current_csv="$IDF_PROJECT/partitions-$OPENBREWBYWEIGHT_ARCH.csv"',
                 "erase-flash",
                 '"$installed_app0_offset" "$image"'):
    assert required in flash_idf, f"flash-idf erase-all contract missing: {required}"
assert '0x10000 "$image"' not in flash_idf, \
    "external app offset must be dynamic"
parsed_erase = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse --erase-all; test "$OBBW_CLI_ERASE_ALL" = 1'],
    cwd=ROOT, capture_output=True, text=True)
assert parsed_erase.returncode == 0, parsed_erase.stderr
for controls, expected in (((), "0|0|"), (("--yes",), "1|0|--yes"),
                           (("--wait-for-confirmation",),
                            "0|1|--wait-for-confirmation"),
                           (("--yes", "--wait-for-confirmation"),
                            "1|1|--yes --wait-for-confirmation")):
    parsed_ota_controls = subprocess.run(
        ["bash", "-c", 'source "$1"; shift; obbw_cli_parse "$@"; '
         'obbw_cli_flags_for yes wait_for_confirmation; '
         'printf "%s|%s|%s" "$OBBW_CLI_YES" "$OBBW_CLI_WAIT_FOR_CONFIRMATION" '
         '"${OBBW_CLI_FORWARD[*]}"', "ota-controls",
         str(ROOT / "scripts/openbrewbyweight_cli.sh"), *controls],
        cwd=ROOT, capture_output=True, text=True)
    assert parsed_ota_controls.returncode == 0 and \
        parsed_ota_controls.stdout == expected
removed_force = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; obbw_cli_parse --force'],
    cwd=ROOT, capture_output=True, text=True)
assert removed_force.returncode == 2 and "--wait-for-confirmation" in \
    removed_force.stderr
profile_cli = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse --hardware esp32-s3-relay-x1-speaker '
     '--machine rancilio-silvia-pro-x --development --flags=-DCUSTOM=1; '
     'obbw_cli_flags_for hardware_config machine_config development; '
     'printf "%s|%s|%s" "$(obbw_get hardware_config)" '
     '"$(obbw_get machine_config)" "${OBBW_CLI_FORWARD[*]}"'],
    cwd=ROOT, capture_output=True, text=True)
assert profile_cli.returncode == 0, profile_cli.stderr
assert profile_cli.stdout == (
    "esp32-s3-relay-x1-speaker|rancilio-silvia-pro-x|"
    "--hardware esp32-s3-relay-x1-speaker --machine rancilio-silvia-pro-x "
    "--development")
removed_profile_cli = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse --hardware-config old-hardware.json'],
    cwd=ROOT, capture_output=True, text=True)
assert removed_profile_cli.returncode == 2 and \
    "Unknown option: --hardware-config" in removed_profile_cli.stderr


def static_paths_probe(*args: str, output_dir: str | None = None,
                       build_dir: str | None = None,
                       databases: tuple[str, ...] = (),
                       empty_variants: tuple[str, ...] = ()) -> subprocess.CompletedProcess[str]:
    """Resolve the shared static-analysis path set in a sandboxed CLI root."""
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-static-paths-") as temporary:
        root = Path(temporary)
        env = os.environ.copy()
        env.update(OBBW_CLI_ROOT=str(root), OPENBREWBYWEIGHT_NONINTERACTIVE="1",
                   HOME=str(root))
        for name in ("IDF_PATH", "IDF_PYTHON_ENV_PATH"):
            env.pop(name, None)
        flags = ["--arch", "n16r8"]
        if build_dir is not None:
            flags += ["--build-dir", build_dir]
        if output_dir is not None:
            flags += ["--output-dir", output_dir]
        for database, stamp in databases:
            target = root / database
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("[]")
            os.utime(target, (stamp, stamp))
        for variant in empty_variants:
            (root / variant).mkdir(parents=True, exist_ok=True)
        command = (
            f'source "{ROOT / "scripts/openbrewbyweight_board.sh"}"; '
            f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
            f'source "{ROOT / "scripts/openbrewbyweight_idf.sh"}"; '
            'obbw_cli_parse "$@" || exit $?; '
            'openbrewbyweight_resolve_board "$(obbw_get arch)" || exit 2; '
            'obbw_idf_resolve_paths; obbw_idf_static_paths reports/static-tidy || exit $?; '
            'printf "build_dir=%s\\nbuild_path=%s\\noutput_dir=%s\\n" '
            '"$build_dir" "$build_path" "$output_dir"')
        return subprocess.run(["bash", "-c", command, "probe", *flags, *args],
                              env=env, cwd=ROOT, capture_output=True, text=True)


discovered = static_paths_probe(
    databases=(("build-idf/n16r8/compile_commands.json", 1000.0),))
assert discovered.returncode == 0, (discovered.returncode, discovered.stdout,
                                    discovered.stderr)
fields = dict(line.split("=", 1) for line in discovered.stdout.splitlines())
assert fields["build_dir"].endswith("build-idf/n16r8"), discovered.stdout
assert fields["build_path"].endswith("build-idf/n16r8"), discovered.stdout

newer = static_paths_probe(
    databases=(("build-idf/older--pair/compile_commands.json", 1000.0),
               ("build-idf/newer--pair/compile_commands.json", 2000.0)))
fields = dict(line.split("=", 1) for line in newer.stdout.splitlines())
assert newer.returncode == 0 and \
    fields["build_dir"].endswith("build-idf/newer--pair"), newer.stdout
spaced = static_paths_probe(
    databases=(("build-idf/my variant--pair/compile_commands.json", 3000.0),))
fields = dict(line.split("=", 1) for line in spaced.stdout.splitlines())
assert spaced.returncode == 0 and \
    fields["build_dir"].endswith("build-idf/my variant--pair"), spaced.stdout
ignored_empty = static_paths_probe(
    databases=(("build-idf/older--pair/compile_commands.json", 1000.0),),
    empty_variants=("build-idf/empty--pair",))
fields = dict(line.split("=", 1) for line in ignored_empty.stdout.splitlines())
assert ignored_empty.returncode == 0 and \
    fields["build_dir"].endswith("build-idf/older--pair"), ignored_empty.stdout
explicit = static_paths_probe(
    build_dir="build-idf/older--pair", output_dir="reports/custom",
    databases=(("build-idf/older--pair/compile_commands.json", 1000.0),
               ("build-idf/newer--pair/compile_commands.json", 2000.0)))
assert explicit.returncode == 0, (explicit.returncode, explicit.stdout, explicit.stderr)
fields = dict(line.split("=", 1) for line in explicit.stdout.splitlines())
assert fields["build_dir"] == "build-idf/older--pair", explicit.stdout
assert fields["build_path"].endswith("build-idf/older--pair"), explicit.stdout
assert fields["output_dir"] == "reports/custom", explicit.stdout

missing = static_paths_probe()
assert missing.returncode == 1, (missing.returncode, missing.stdout, missing.stderr)
assert "build-idf/n16r8/compile_commands.json does not exist." in missing.stderr, \
    missing.stderr

for unsafe in ("/tmp/escape", "reports/../.."):
    rejected = static_paths_probe(output_dir=unsafe)
    assert rejected.returncode == 2, (unsafe, rejected.returncode,
                                      rejected.stdout, rejected.stderr)
    assert "relative path inside the repository" in rejected.stderr, rejected.stderr


def fake_database_sandbox() -> tuple[Path, Path, tempfile.TemporaryDirectory]:
    """Sandbox holding a fake compile database mirroring the project manifest."""
    sandbox = tempfile.TemporaryDirectory(prefix="openbrewbyweight-static-idf-")
    root = Path(sandbox.name)
    build = root / "build-idf" / "n16r8"
    (build / "generated").mkdir(parents=True)
    (build / "generated" / "build-profile.json").write_text(
        '{"machine_integration": "none"}\n')
    # The real audit enumerates the manifest for the integration the
    # build profile selects; mirror that selection for "none".
    expected = []
    for raw in (ROOT / "scripts/project-translation-units.txt").read_text().splitlines():
        entry = raw.strip()
        if not entry or entry.startswith("#"):
            continue
        relative, separator, selector = entry.partition("|")
        if separator and selector.strip().partition("=")[2] != "none":
            continue
        expected.append(relative.strip())
    (build / "compile_commands.json").write_text(json.dumps(
        [{"directory": str(ROOT), "file": str(ROOT / relative),
          "arguments": ["cc", relative]} for relative in expected]))
    tools = root / "tools"
    tools.mkdir()
    return root, tools, sandbox


def static_idf_run(*extra: str) -> tuple[subprocess.CompletedProcess[str], Path,
                                         tempfile.TemporaryDirectory]:
    """Run static-idf end to end over a sandboxed fake compile database."""
    root, tools, sandbox = fake_database_sandbox()
    stub = tools / "cppcheck"
    stub.write_text("#!/bin/sh\nif [ \"$1\" = --version ]; then\n"
                    "  printf 'Cppcheck 9.9.9-stub\\n'\nelse\n"
                    "  printf 'cppcheck-stub\\n'\nfi\n")
    stub.chmod(0o755)
    env = os.environ.copy()
    env.update(OBBW_CLI_ROOT=str(root), OPENBREWBYWEIGHT_NONINTERACTIVE="1",
               PATH=f"{tools}:{os.environ['PATH']}")
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/static-idf"), "--arch", "n16r8",
         "--build-dir", str(root / "build-idf" / "n16r8"), *extra],
        cwd=ROOT, env=env, capture_output=True, text=True)
    return result, root / "reports" / "static-idf-golden", sandbox


static_idf_result, static_idf_report, static_idf_sandbox = static_idf_run(
    "--output-dir", "reports/static-idf-golden")
try:
    assert static_idf_result.returncode == 0, (
        static_idf_result.returncode, static_idf_result.stdout,
        static_idf_result.stderr)
    assert re.search(r"translation-unit coverage: (\d+)/\1 production C\+\+ "
                     "files for integration=none", static_idf_result.stdout), \
        static_idf_result.stdout
    assert "Reports saved under reports/static-idf-golden/" in static_idf_result.stdout
    assert "cppcheck-stub" in (static_idf_report / "cppcheck.txt").read_text()
    static_idf_readme = (static_idf_report / "README.txt").read_text()
    assert "Cppcheck exit status: 0" in static_idf_readme
    assert "Build database: " in static_idf_readme
finally:
    static_idf_sandbox.cleanup()

with tempfile.TemporaryDirectory(prefix="openbrewbyweight-version-") as temporary:
    fake_git = Path(temporary) / "git"
    fake_git.write_text("#!/bin/sh\nexit 1\n")
    fake_git.chmod(0o755)
    output = Path(temporary) / "version.h"
    env = dict(os.environ, PATH=f"{temporary}:{os.environ['PATH']}")
    command = ["sh", str(ROOT / "scripts/gen_version.sh"), "n16r8", "hw",
               "machine", str(output)]
    assert subprocess.run(command, env=env, capture_output=True).returncode == 0
    release = (ROOT / "VERSION").read_text().strip()
    assert f'{release}+unknown' in output.read_text()
    # VERSION is strict MAJOR.MINOR.PATCH; untracked files mark the build dirty.
    version_root = Path(temporary) / "repo"
    (version_root / "scripts").mkdir(parents=True)
    shutil.copy2(ROOT / "scripts/gen_version.sh", version_root / "scripts")
    version_command = ["sh", str(version_root / "scripts/gen_version.sh"), "n16r8", "hw",
                       "machine", str(output)]
    for invalid in ("1", "1.0", "1.0.0.0", "01.0.0", "1.0.0-rc1", "v1.0.0"):
        (version_root / "VERSION").write_text(invalid + "\n")
        assert subprocess.run(version_command, env=env, capture_output=True).returncode == 1, \
            invalid
    (version_root / "VERSION").write_text("1.2.3\n")
    git = ["git", "-C", str(version_root), "-c", "user.name=t", "-c", "user.email=t@t"]
    subprocess.run([*git, "init", "-q"], check=True)
    subprocess.run([*git, "add", "."], check=True)
    subprocess.run([*git, "commit", "-qm", "v"], check=True)
    subprocess.run(version_command, check=True, capture_output=True)
    assert "1.2.3+" in output.read_text() and "-dirty" not in output.read_text()
    (version_root / "new.txt").write_text("x")
    subprocess.run(version_command, check=True, capture_output=True)
    assert '-dirty"' in output.read_text()


def iwyu_idf_run(*extra: str) -> tuple[subprocess.CompletedProcess[str], Path,
                                       tempfile.TemporaryDirectory]:
    """Run iwyu-idf end to end over a sandboxed fake compile database."""
    root, tools, sandbox = fake_database_sandbox()
    stub = tools / "include-what-you-use"
    stub.write_text(
        "#!/bin/sh\n"
        "if [ \"$1\" = \"--version\" ]; then\n"
        "  printf 'include-what-you-use 0.23-stub\\n'\n"
        "  exit 0\n"
        "fi\n"
        "printf 'iwyu-stub suggestion\\n'\n")
    stub.chmod(0o755)
    env = os.environ.copy()
    env.update(OBBW_CLI_ROOT=str(root), OPENBREWBYWEIGHT_NONINTERACTIVE="1",
               PATH=f"{tools}:{os.environ['PATH']}")
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/iwyu-idf"), "--arch", "n16r8",
         "--build-dir", str(root / "build-idf" / "n16r8"), *extra],
        cwd=ROOT, env=env, capture_output=True, text=True)
    return result, root / "reports" / "iwyu-golden", sandbox


iwyu_result, iwyu_report, iwyu_sandbox = iwyu_idf_run(
    "--output-dir", "reports/iwyu-golden")
try:
    assert iwyu_result.returncode == 0, (
        iwyu_result.returncode, iwyu_result.stdout, iwyu_result.stderr)
    # The shared translation-unit audit now also gates the IWYU run.
    assert re.search(r"translation-unit coverage: (\d+)/\1 production C\+\+ "
                     "files for integration=none", iwyu_result.stdout), \
        iwyu_result.stdout
    parsed = re.search(r"Parsed (\d+) of (\d+) translation units", iwyu_result.stdout)
    assert parsed and parsed.group(1) == parsed.group(2), iwyu_result.stdout
    assert "iwyu-stub suggestion" in (iwyu_report / "iwyu.txt").read_text()
    iwyu_readme = (iwyu_report / "README.txt").read_text()
    assert "Tool: include-what-you-use 0.23-stub" in iwyu_readme
    assert "Build database: " in iwyu_readme and "Mappings: " in iwyu_readme
finally:
    iwyu_sandbox.cleanup()


def cli_probe(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'obbw_cli_parse "$@" || exit $?; obbw_cli_effective_flags',
         "probe", *args],
        cwd=ROOT, capture_output=True, text=True)


jtag_append = cli_probe("--jtag", "--flags=-DCUSTOM=1")
assert jtag_append.returncode == 0 and \
    jtag_append.stdout == "-DCUSTOM=1 -DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1"
jtag_existing = cli_probe("--jtag", "--flags=-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1")
assert jtag_existing.returncode == 0 and \
    jtag_existing.stdout == "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1"
jtag_with_dev = cli_probe("--jtag", "--development")
assert jtag_with_dev.returncode == 0 and jtag_with_dev.stdout == \
    "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1 -DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1"


def cli_profile(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'obbw_cli_parse "$@" || exit $?; obbw_cli_flags_for development release '
         'no_auth_admin jtag; '
         'printf "%s|%s" "$(obbw_cli_effective_flags)" '
         '"${OBBW_CLI_FORWARD[*]:-}"', "probe", *args],
        cwd=ROOT, capture_output=True, text=True)


release_profile = cli_profile("--release")
assert release_profile.returncode == 0 and release_profile.stdout == "|--release"
no_profile = cli_profile()
assert no_profile.returncode == 0 and no_profile.stdout == "|", no_profile.stdout
release_jtag = cli_profile("--release", "--jtag")
assert release_jtag.returncode == 0 and \
    release_jtag.stdout == "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1|--release --jtag"
admin_unlock = cli_profile("--no-auth-admin")
assert admin_unlock.returncode == 0 and \
    admin_unlock.stdout == "-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1|--no-auth-admin"
dev_profile = cli_profile("--development")
assert dev_profile.returncode == 0 and dev_profile.stdout == (
    "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1 -DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1"
    "|--development --no-auth-admin --jtag")
release_wins = cli_profile("--release", "--no-auth-admin")
assert release_wins.returncode == 0 and release_wins.stdout == (
    "-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1|--release --no-auth-admin")
profile_conflict = cli_profile("--development", "--release")
assert profile_conflict.returncode == 2 and \
    "mutually exclusive" in profile_conflict.stderr
admin_conflict = cli_probe("--no-auth-admin", "--flags=-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=0")
assert admin_conflict.returncode == 2 and \
    "--no-auth-admin conflicts with OPEN_BREW_BY_WEIGHT_DEVELOPMENT=0" in admin_conflict.stderr

for define, switch in (("-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1", "--no-auth-admin"),
                       ("OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1", "--jtag")):
    release_define = cli_probe(f"--flags={define}")
    assert release_define.returncode == 2 and switch in release_define.stderr, define
for define in ("-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT", "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=2",
               "-DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL"):
    odd_value = cli_probe("--development", f"--flags={define}")
    assert odd_value.returncode == 2 and "=0 or =1" in odd_value.stderr, define
remote_define = "-DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1"
remote_cli_flags = cli_probe(f"--flags={remote_define}")
assert remote_cli_flags.returncode == 0 and remote_cli_flags.stdout == remote_define
remote_env = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse || exit $?; obbw_cli_apply_env; obbw_cli_effective_flags'],
    cwd=ROOT, env={**os.environ, "OPENBREWBYWEIGHT_FLAGS": remote_define},
    capture_output=True, text=True)
assert remote_env.returncode == 2 and "per-build opt-in" in remote_env.stderr

jtag_off_untouched = cli_probe("--flags=-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0")
assert jtag_off_untouched.returncode == 0 and \
    jtag_off_untouched.stdout == "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0"
jtag_conflict = cli_probe("--jtag", "--flags=-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0")
assert jtag_conflict.returncode == 2 and \
    "conflicts with OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0" in jtag_conflict.stderr
jtag_valued = cli_probe("--jtag=1")
assert jtag_valued.returncode == 2 and "does not take a value" in jtag_valued.stderr
jtag_forward = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse "$@" || exit $?; obbw_cli_flags_for development jtag; '
     'printf "%s" "${OBBW_CLI_FORWARD[*]}"', "probe", "--jtag"],
    cwd=ROOT, capture_output=True, text=True)
assert jtag_forward.returncode == 0 and jtag_forward.stdout == "--jtag"


def cli_opt_level(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'obbw_cli_parse "$@" || exit $?; obbw_cli_flags_for opt_level; '
         'printf "%s" "${OBBW_CLI_FORWARD[*]:-}"', "probe", *args],
        cwd=ROOT, capture_output=True, text=True)


for level in ("--o0", "--og", "--o2", "--os"):
    forwarded = cli_opt_level(level)
    assert forwarded.returncode == 0 and forwarded.stdout == level, level
opt_none = cli_opt_level()
assert opt_none.returncode == 0 and opt_none.stdout == ""
opt_exclusive = cli_opt_level("--o2", "--os")
assert opt_exclusive.returncode == 2 and "exclusive" in opt_exclusive.stderr
opt_repeat = cli_opt_level("--o2", "--o2")
assert opt_repeat.returncode == 0 and opt_repeat.stdout == "--o2"
for level in ("--o1", "--o3", "--o1=1", "--oz", "--Ofast"):
    rejected = cli_opt_level(level)
    assert rejected.returncode == 2 and "not an ESP-IDF Kconfig" in rejected.stderr, level
opt_not_stored = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse "$@" || exit $?; obbw_cli_save; cat "$OBBW_CLI_STORE"',
     "probe", "--os", "--flags=-DCUSTOM=1"],
    cwd=ROOT, env={**os.environ, "OBBW_CLI_ROOT": str(Path(mkdtemp()))},
    capture_output=True, text=True)
assert opt_not_stored.returncode == 0 and \
    "opt_level" not in opt_not_stored.stdout and \
    "--os" not in opt_not_stored.stdout
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-jtag-store-") as temporary:
    jtag_saved = subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'obbw_cli_parse "$@" || exit $?; obbw_cli_save; cat "$OBBW_CLI_STORE"',
         "probe", "--jtag", "--flags=-DCUSTOM=1"],
        cwd=ROOT, env={**os.environ, "OBBW_CLI_ROOT": temporary},
        capture_output=True, text=True)
    assert jtag_saved.returncode == 0 and \
        "flags=" not in jtag_saved.stdout and \
        "jtag" not in jtag_saved.stdout
development_conflict = subprocess.run(
    ["bash", "-c",
     f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
     'obbw_cli_parse --development --release || exit $?; obbw_cli_effective_flags'],
    cwd=ROOT, capture_output=True, text=True)
assert development_conflict.returncode == 2 and \
    "mutually exclusive" in development_conflict.stderr

with tempfile.TemporaryDirectory(prefix="openbrewbyweight-defaults-") as temporary:
    defaults_root = Path(temporary)
    store = defaults_root / ".openbrewbyweight"
    store.write_text("host=stored.local\nspeed=115200\nport=/missing/device\n")
    defaults_env = {**os.environ, "OBBW_CLI_ROOT": str(defaults_root),
                    "OPENBREWBYWEIGHT_NONINTERACTIVE": "1",
                    "OPENBREWBYWEIGHT_HOST": "environment.local",
                    "OPENBREWBYWEIGHT_DEVICE_PASSWORD": "environment-secret"}
    resolved = subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'obbw_cli_parse --host cli.local; '
         'obbw_cli_resolve "host speed password"; '
         'printf "RESULT:%s|%s|%s" "$(obbw_get host)" "$(obbw_get speed)" '
         '"$(obbw_origin password)"'],
        cwd=ROOT, env=defaults_env, capture_output=True, text=True)
    assert resolved.returncode == 0 and \
        "RESULT:cli.local|115200|env" in resolved.stdout
    saved = store.read_text()
    assert "host=cli.local" in saved and "speed=115200" in saved
    assert "\npassword=" not in saved and "environment-secret" not in saved
    stale = subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_board.sh"}"; '
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'obbw_cli_resolve "port"'], cwd=ROOT, env=defaults_env,
        capture_output=True, text=True)
    assert stale.returncode == 2 and "does not exist" in stale.stderr
    detected = subprocess.run(
        ["bash", "-c",
         f'source "{ROOT / "scripts/openbrewbyweight_board.sh"}"; '
         f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
         'openbrewbyweight_detect_ports(){ printf "/dev/null\\n"; }; '
         'obbw_prompt_suggestion port'], cwd=ROOT, env=defaults_env,
        capture_output=True, text=True)
    assert detected.returncode == 0 and detected.stdout == "/dev/null"

missing_profiles = subprocess.run(
    [str(INTERNAL / "build-idf"), "--flags="], cwd=ROOT,
    env={**os.environ, "OPENBREWBYWEIGHT_NONINTERACTIVE": "1",
         "OPENBREWBYWEIGHT_HARDWARE": "", "OPENBREWBYWEIGHT_MACHINE": ""},
    capture_output=True, text=True)
assert missing_profiles.returncode == 2
assert "hardware_config machine_config" in missing_profiles.stderr
# An explicit --release refuses a tree with untracked files before any build work.
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-release-tree-") as temporary:
    subprocess.run(["git", "init", "-q", temporary], check=True)
    (Path(temporary) / "untracked.txt").write_text("x")
    dirty_release = subprocess.run(
        [str(INTERNAL / "build-idf"), "--release", "--flags=", "--hardware", "hw",
         "--machine", "machine"], cwd=ROOT, capture_output=True, text=True,
        env={**os.environ, "OPENBREWBYWEIGHT_NONINTERACTIVE": "1", "OBBW_CLI_ROOT": temporary})
    assert dirty_release.returncode == 2 and "clean Git tree" in dirty_release.stderr, \
        dirty_release.stderr
erase_image = subprocess.run(
    [str(INTERNAL / "flash-idf"), "--erase-all", "--image", "external.bin"],
    cwd=ROOT, capture_output=True, text=True)
assert erase_image.returncode == 2 and "cannot be combined" in erase_image.stderr


def language_cli(*args: str, environment: str | None = None):
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-language-") as temporary:
        root = Path(temporary)
        (root / ".openbrewbyweight").write_text("webui_language=fr\n")
        env = os.environ.copy()
        env.update(OBBW_CLI_ROOT=str(root), OPENBREWBYWEIGHT_NONINTERACTIVE="1")
        if environment is None:
            env.pop("OPENBREWBYWEIGHT_WEBUI_LANGUAGE", None)
        else:
            env["OPENBREWBYWEIGHT_WEBUI_LANGUAGE"] = environment
        command = (
            f'source "{ROOT / "scripts/openbrewbyweight_cli.sh"}"; '
            'obbw_cli_parse "$@" || exit $?; obbw_cli_resolve "webui_language" || exit $?; '
            'obbw_cli_flags_for webui_language; '
            'printf "RESULT:%s|%s|%s\n" "$(obbw_get webui_language)" '
            '"$(obbw_origin webui_language)" "${OBBW_CLI_FORWARD[*]}"')
        result = subprocess.run(["bash", "-c", command, "probe", *args],
                                env=env, capture_output=True, text=True)
        stored = (root / ".openbrewbyweight").read_text()
        return result, stored


default_language, default_store = language_cli()
assert default_language.returncode == 0 and \
    "RESULT:en|default|--webui-language en" in default_language.stdout
assert "Defaults:" in default_language.stdout and "webui_language=" not in default_store
env_language, env_store = language_cli(environment="EN_us")
assert "RESULT:en-us|env|--webui-language en-us" in env_language.stdout
assert "webui_language=" not in env_store
flag_language, flag_store = language_cli("--webui-language=En_GB", environment="fr")
assert "RESULT:en-gb|cli|--webui-language en-gb" in flag_language.stdout
assert "webui_language=" not in flag_store
invalid_language, _ = language_cli("--webui-language", "../en")
assert invalid_language.returncode == 2 and "Invalid WebUI language" in invalid_language.stderr


def validate_language_steps(args: list[str], environment: str | None = None,
                            risk: str = "R3", lanes: bool = False):
    captured = {}

    def capture(command, risk, steps, verbosity, manual=None, prelude=None,
                lanes=None, **options):
        assert lanes is None or steps == [s for lane in lanes for s in lane]
        captured.update(command=command, risk=risk, steps=steps, lanes=lanes, **options)
        return 0

    main = dev_module["main"]
    with patch.dict(main.__globals__, execute=capture, markdown_errors=lambda: []), \
            patch.object(sys, "argv", [str(DEV), "validate", "--risk", risk, *args]), \
            patch.dict(os.environ, {}, clear=False):
        if environment is None:
            os.environ.pop("OPENBREWBYWEIGHT_WEBUI_LANGUAGE", None)
        else:
            os.environ["OPENBREWBYWEIGHT_WEBUI_LANGUAGE"] = environment
        assert main() == 0
    validate_language_steps.captured = captured
    return captured["lanes"] if lanes else captured["steps"]


for validate_args, environment, expected in (
        (["src/web/app.js"], None, "en"),
        (["src/web/app.js"], "EN_us", "en-us"),
        (["--webui-language=FR_ca", "src/web/app.js"], "en", "fr-ca")):
    validate_steps = validate_language_steps(validate_args, environment)
    build_steps = [argv for name, argv in validate_steps
                   if name.startswith(("idf-", "warnings-"))]
    assert len(build_steps) == 6
    assert all(argv.count("--webui-language") == 1 and
               argv[argv.index("--webui-language") + 1] == expected
               for argv in build_steps), build_steps
    assert all("--development" in argv and "--jtag" not in argv
               for argv in build_steps), build_steps

# R2 shares the firmware budget builds: every profile pair is compiled with
# --development so the versioned size baselines are always compared.
r2_steps = validate_language_steps(["src/web/app.js"], risk="R2")
r2_builds = [argv for name, argv in r2_steps if name.startswith("idf-")]
assert len(r2_builds) == 3, r2_builds
assert all("--development" in argv for argv in r2_builds), r2_builds
# The shipped (flag-free release) image of every pair builds in its own tree
# and compares its own versioned baseline.
r2_release = [argv for name, argv in r2_steps if name.startswith("release-")]
assert len(r2_release) == 3 and all(
    argv[:2] == ["env", f"OPENBREWBYWEIGHT_IDF_BUILD_ROOT={ROOT}/build-idf/release"] and
    argv[-1] == "--flags=" and "--development" not in argv and "--jtag" not in argv
    for argv in r2_release), r2_release

# Host tests and firmware work run as two parallel lanes. Every step that
# touches IDF trees, the shared Web UI header, or the CLI store stays serial in
# the firmware lane: route capacity before Web contracts, then per variant the
# budget build, cppcheck over its database, and the separate warnings build.
host_lane, firmware_lane = validate_language_steps(["src/web/app.js"], lanes=True)
assert [name for name, _ in host_lane] == [
    f"{stage}-{preset}" for preset in ("normal", "asan-ubsan", "tsan")
    for stage in ("configure", "build", "test")] + ["architecture"], host_lane
firmware_names = [name for name, _ in firmware_lane]
assert firmware_names[:4] == ["http-route-capacity", "web-budgets", "web-contracts",
                             "web-preview"], firmware_names
assert firmware_names[4:] == [f"{kind}-{hardware}--{machine}"
                              for hardware, machine in dev_module["BUILD_PROFILES"]
                              for kind in ("idf", "stack", "release", "stack-release",
                                           "cppcheck", "warnings")], firmware_names
assert all(argv[-2:] == ["-j", str(os.cpu_count() or 1)]
           for name, argv in host_lane if name.startswith("test-")), host_lane
assert validate_language_steps(["src/web/app.js"], risk="R1", lanes=True) is None

# Budget gates defer every size check to one verdict after all lanes, and
# record the measured tree; report mode is never budget evidence.
validate_language_steps(["src/web/app.js"], risk="R2")
gate = validate_language_steps.captured
assert gate["env_extra"] == {"OPENBREWBYWEIGHT_BUDGET_DEFER": "1"}, gate
assert gate["final"] == [("budget-verdict", ["python3", "scripts/internal/budgets", "verdict"])]
assert gate["summary_extra"]["budget"]["evidence"] is True
assert gate["summary_extra"]["budget"]["firmware"] is True
assert set(gate["summary_extra"]["budget"]["tree"]) == {"head", "worktree", "budgetFiles"}
validate_language_steps(["--budget-report", "src/web/app.js"], risk="R2")
report_gate = validate_language_steps.captured
assert report_gate["final"][0][1][-1] == "--report"
assert report_gate["summary_extra"]["budget"]["evidence"] is False
for path in dev_module["BUDGET_FILES"]:
    assert dev_module["path_risk"](path) == "R1", path

# The budget helper judges every recorded overrun at once and raises only the
# exceeded limits, each by its file's update headroom.
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-budgets-") as temporary:
    root = Path(temporary)
    (root / "scripts/internal").mkdir(parents=True)
    (root / "config").mkdir()
    shutil.copy2(INTERNAL / "budgets", root / "scripts/internal/budgets")
    web_file, firmware_file = root / "config/web-asset-budgets.json", root / "config/resource-baselines.json"
    web_file.write_text(json.dumps({"updateHeadroomBytes": {"gzip": 32, "source": 0},
                                    "limits": {"gzip.css": 100, "source.js": 50}}))
    firmware_file.write_text(json.dumps({
        "allowedGrowthBytes": {"image": 10}, "updateHeadroomBytes": 64,
        "targets": {"n16r8": {"image": 1000}}, "releaseTargets": {"pair": {"image": 2000}}}))
    run_dir = root / "artifacts/runs/r"
    (run_dir / "budgets").mkdir(parents=True)
    (run_dir / "budgets/web.json").write_text(json.dumps({"kind": "web", "rows": [
        {"name": "gzip.css", "actual": 120, "limit": 100},
        {"name": "source.js", "actual": 40, "limit": 50}]}))
    for profile, target, value in (("development", "targets.n16r8", 1030),
                                   ("release", "releaseTargets.pair", 2050)):
        (run_dir / f"budgets/firmware-{profile}-pair.json").write_text(json.dumps({
            "kind": "firmware", "target": target,
            "rows": [{"metric": "image", "value": value, "baseline": 0}]}))

    def helper(*args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(["python3", str(root / "scripts/internal/budgets"), *args],
                              env={**os.environ, "OPENBREWBYWEIGHT_RUN_DIR": str(run_dir)},
                              text=True, capture_output=True)

    judged = helper("verdict")
    assert judged.returncode == 1 and "source.js" not in judged.stdout, judged.stdout
    assert "BUDGET EXCEEDED gzip.css: 120 > 100 (+20)" in judged.stdout, judged.stdout
    assert "BUDGET EXCEEDED targets.n16r8.image: 1030 > 1010 (+20)" in judged.stdout
    assert "BUDGET EXCEEDED releaseTargets.pair.image: 2050 > 2010 (+40)" in judged.stdout
    reported = helper("verdict", "--report")
    assert reported.returncode == 0 and "not budget evidence" in reported.stdout
    updated = helper("update", str(run_dir))
    assert updated.returncode == 0, updated.stderr
    assert json.loads(web_file.read_text())["limits"] == {"gzip.css": 152, "source.js": 50}
    assert json.loads(firmware_file.read_text())["allowedGrowthBytes"] == {"image": 114}
    assert helper("verdict").returncode == 0
    assert "nothing changed" in helper("update", str(run_dir)).stdout

# `budget update` accepts only a complete, non-report run of this exact tree
# whose only failure is the budget verdict.
update_problems = dev_module["budget_update_problems"]
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-budget-runs-") as temporary:
    tree = {"head": "a", "worktree": "b", "budgetFiles": {"config/x.json": "c"}}
    run_dir = Path(temporary) / "artifacts/runs/r"
    (run_dir / "budgets").mkdir(parents=True)

    def problems_for(checks, firmware=False, evidence=True, recorded=tree):
        (run_dir / "summary.json").write_text(json.dumps({"checks": checks, "budget": {
            "evidence": evidence, "firmware": firmware, "tree": recorded}}))
        with patch.dict(update_problems.__globals__, ROOT=Path(temporary),
                        tree_state=lambda: dict(tree)):
            return " | ".join(update_problems("r"))

    verdict_failed = [{"name": "web-budgets", "status": "passed"},
                      {"name": "budget-verdict", "status": "failed"}]
    assert problems_for(verdict_failed) == ""
    assert "--budget-report" in problems_for(verdict_failed, evidence=False)
    assert "check web-contracts failed" in problems_for(
        [{"name": "web-contracts", "status": "failed"}, *verdict_failed])
    assert "stopped before its budget verdict" in problems_for(verdict_failed[:1])
    assert "lacks a development or release measurement" in problems_for(
        verdict_failed, firmware=True)
    assert "is stale: head" in problems_for(verdict_failed, recorded={**tree, "head": "old"})
    assert "is stale: worktree, budgetFiles" in problems_for(
        verdict_failed, recorded={**tree, "worktree": "x", "budgetFiles": {}})
    (run_dir / "summary.json").unlink()
    with patch.dict(update_problems.__globals__, ROOT=Path(temporary)):
        assert "recorded no budget measurements" in update_problems("r")[0]

# A failing lane stops every lane from starting another step, while the step
# already running finishes and its whole output stays one contiguous block.
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-lanes-") as temporary:
    marker = Path(temporary) / "late"
    with patch.dict(dev_module["execute"].__globals__,
                    ROOT=Path(temporary), write_summary=lambda *a: Path(temporary) / "s.json"), \
            patch("builtins.print"):
        (Path(temporary) / "artifacts/runs").mkdir(parents=True)
        slow = ("slow", ["sh", "-c", "echo one; sleep 1; echo two"])
        late = ("late", ["sh", "-c", f"touch {marker}"])
        fail = ("fail", ["sh", "-c", "echo broken; exit 3"])
        code = dev_module["execute"](["x"], "R2", [], "compact",
                                     lanes=[[slow, late], [fail]])
    assert code == 1 and not marker.exists()
    log = next(Path(temporary).glob("artifacts/runs/*/run.log")).read_text()
    assert "==> slow: sh -c echo one; sleep 1; echo two\none\ntwo\n" in log, log


FAKE_RESOLVER = (
    "print('arch=n16r8')\n"
    "print('variant=esp32-s3-relay-x1-speaker--rancilio-silvia-pro-x')\n"
    "print('hardware_compat=esp32-s3-relay-x1-speaker')\n"
    "print('machine_compat=rancilio-silvia-pro-x')\n"
    "print('machine_integration=none')\n"
    "print('generated_dir=build-idf/generated')\n")


def dispatched(stages: tuple[str, ...], args: list[str], fail: str = "",
               store: str | None = None) -> tuple[subprocess.CompletedProcess[str], list[str]]:
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-wrapper-") as temporary:
        root = Path(temporary)
        scripts = root / "scripts"
        internal = scripts / "internal"
        internal.mkdir(parents=True)
        for name in ("openbrewbyweight_cli.sh", "openbrewbyweight_board.sh"):
            shutil.copy2(ROOT / "scripts" / name, scripts / name)
        shutil.copy2(INTERNAL / "firmware-idf", internal / "firmware-idf")
        (scripts / "resolve_build_profiles.py").write_text(FAKE_RESOLVER)
        if store is not None:
            (root / ".openbrewbyweight").write_text(store)
        log = root / "children.log"
        for child in ("build-idf", "flash-idf", "ota-idf", "monitor-idf"):
            target = internal / child
            target.write_text(
                f'#!/bin/sh\nprintf "%s:%s\\n" "${{0##*/}}" "$*" >>"{log}"\n'
                + ("exit 9\n" if child == fail else ""))
            target.chmod(0o755)
        env = os.environ.copy()
        env.update(OBBW_CLI_ROOT=str(root), OPENBREWBYWEIGHT_NONINTERACTIVE="1",
                   OPENBREWBYWEIGHT_DEVICE_PASSWORD="test-only")
        env.pop("OPENBREWBYWEIGHT_FLAGS", None)
        result = subprocess.run(
            [str(internal / "firmware-idf"), *stages, "--", *args],
            cwd=root, env=env, capture_output=True, text=True)
        children = log.read_text().splitlines() if log.exists() else []
        return result, children


def stubbed_dispatcher(stages: tuple[str, ...], args: list[str], fail: str = "",
                       expected: int = 0, store: str | None = None) -> list[str]:
    result, children = dispatched(stages, args, fail, store)
    assert result.returncode == expected, (stages, result.stdout, result.stderr)
    return children


profile_args = ["--hardware", "esp32-s3-relay-x1-speaker",
                "--machine", "rancilio-silvia-pro-x"]
flash_monitor = stubbed_dispatcher(
    ("build", "flash", "monitor"),
    [*profile_args, "--port", "/dev/null", "--speed", "115200",
     "--webui-language", "EN_us", "--erase-all", "--jtag"])
assert [line.split(":", 1)[0] for line in flash_monitor] == [
    "build-idf", "flash-idf", "monitor-idf"]
assert "--webui-language en-us" in flash_monitor[0]
assert "--jtag" in flash_monitor[0]
assert "--erase-all" in flash_monitor[1]
assert "--speed 115200" in flash_monitor[2]

legacy_jtag, legacy_jtag_children = dispatched(
    ("build", "flash", "monitor"),
    [*profile_args, "--port", "/dev/null", "--speed", "115200",
     "--webui-language", "EN_us", "--flags", "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1"])
assert legacy_jtag.returncode == 2 and "--jtag" in legacy_jtag.stderr and \
    not legacy_jtag_children, legacy_jtag.stderr
legacy_admin, legacy_admin_children = dispatched(
    ("build",), [*profile_args, "--flags", "-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1"])
assert legacy_admin.returncode == 2 and "--no-auth-admin" in legacy_admin.stderr and \
    not legacy_admin_children, legacy_admin.stderr

blocked, blocked_children = dispatched(("build", "flash", "monitor"), [])
assert blocked.returncode == 2 and "--jtag" in blocked.stderr and \
    not blocked_children, (blocked.returncode, blocked.stderr)
build_only = stubbed_dispatcher(("build",), [*profile_args, "--webui-language", "EN_us"])
assert [line.split(":", 1)[0] for line in build_only] == ["build-idf"]
build_result, _ = dispatched(("build",), [*profile_args, "--webui-language", "EN_us"])
assert re.search(r"Build output folder: /.+/build-idf/esp32-s3-relay-x1-speaker--rancilio-silvia-pro-x$",
                 build_result.stdout, re.M)
assert re.search(r"Flash-ready firmware: /.+/openbrewbyweight\.bin$", build_result.stdout, re.M)
flash_monitor_only = stubbed_dispatcher(
    ("flash", "monitor"),
    [*profile_args, "--port", "/dev/null", "--speed", "115200"])
assert [line.split(":", 1)[0] for line in flash_monitor_only] == [
    "flash-idf", "monitor-idf"]
assert all("--hardware esp32-s3-relay-x1-speaker" in line for line in flash_monitor_only)
for stage in ("flash", "ota"):
    arch_only, arch_children = dispatched(
        (stage,), ["--arch", "n16r8", "--port", "/dev/null", "--host", "127.0.0.1",
                   "--yes"])
    assert arch_only.returncode == 2 and "hardware_config" in arch_only.stderr and \
        not arch_children, (stage, arch_only.stderr)

# Stored compile flags are never reloaded, so a leftover define cannot reach a
# later (release) build or satisfy the console requirement of build+monitor.
for stored in ("-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1",
               "-DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1"):
    store_blocked, store_children = dispatched(
        ("build", "flash", "monitor"),
        [*profile_args, "--port", "/dev/null", "--speed", "115200"],
        store=f"flags={stored}\n")
    assert store_blocked.returncode == 2 and "--jtag" in store_blocked.stderr and \
        not store_children, (store_blocked.returncode, store_blocked.stderr)
    store_build = stubbed_dispatcher(("build",), profile_args, store=f"flags={stored}\n")
    assert stored not in store_build[0], store_build
remote_cli = stubbed_dispatcher(
    ("build",), [*profile_args, "--flags",
                 "-DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1"])
assert "-DOPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL=1" in remote_cli[0]

ota_monitor = stubbed_dispatcher(
    ("build", "ota", "monitor"),
    [*profile_args, "--host", "127.0.0.1", "--port", "/dev/null",
     "--speed", "115200", "--yes", "--wait-for-confirmation", "--jtag"])
assert [line.split(":", 1)[0] for line in ota_monitor] == [
    "build-idf", "ota-idf", "monitor-idf"]
assert "--yes --wait-for-confirmation" in ota_monitor[1]
assert "--jtag" in ota_monitor[0]
assert "test-only" not in "\n".join(ota_monitor)

build_opt = stubbed_dispatcher(
    ("build",), [*profile_args, "--webui-language", "EN_us", "--o0"])
assert build_opt == [] or "--o0" in build_opt[0]
assert len(build_opt) == 1
build_force = stubbed_dispatcher(
    ("build",), [*profile_args, "--force-sdkconfig-regenerate"])
assert len(build_force) == 1 and "--force-sdkconfig-regenerate" in build_force[0]

stopped = stubbed_dispatcher(
    ("build", "flash"), [*profile_args, "--port", "/dev/null"],
    fail="build-idf", expected=9)
assert [line.split(":", 1)[0] for line in stopped] == ["build-idf"]

no_yes = subprocess.run(
    [str(INTERNAL / "firmware-idf"), "ota", "--", *profile_args],
    cwd=ROOT, env={**os.environ, "OPENBREWBYWEIGHT_NONINTERACTIVE": "1"},
    capture_output=True, text=True)
assert no_yes.returncode == 2 and "requires --yes" in no_yes.stderr

for removed in ("b-idf", "f-idf", "m-idf", "o-idf", "s-idf", "bf-idf",
                "bfm-idf", "bo-idf", "bsfm-idf", "build-idf", "flash-idf",
                "monitor-idf", "ota-idf"):
    assert not (ROOT / "scripts" / removed).exists(), \
        f"obsolete public script remains: {removed}"


def idf_activation(active_version: str | None, active_python: bool = True):
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-idf-") as temporary:
        root = Path(temporary)
        active = root / "active"
        fallback = root / "esp/esp-idf-v6.1"
        active_env = root / "active-python"
        fallback_env = root / "fallback-python"
        for idf, env, version in ((active, active_env, active_version),
                                  (fallback, fallback_env, "6.1")):
            (idf / "tools").mkdir(parents=True)
            (env / "bin").mkdir(parents=True)
            (env / "bin/python").write_text("#!/bin/sh\nexit 0\n")
            (env / "bin/python").chmod(0o755)
            (env / "idf_version.txt").write_text(
                "6.1" if version is not None else "\n")
            (idf / "tools/idf.py").write_text(
                f"#!/bin/sh\necho 'ESP-IDF v{version}'\n")
            (idf / "tools/idf.py").chmod(0o755)
            (idf / "components/esp_common/include").mkdir(parents=True,
                                                          exist_ok=True)
            (idf / "components/esp_common/include/esp_idf_version.h").write_text(
                "#define ESP_IDF_VERSION_MAJOR 6\n"
                "#define ESP_IDF_VERSION_MINOR 1\n")
        (active / "export.sh").write_text("return 99\n")
        (fallback / "export.sh").write_text(
            f'export IDF_PATH="{fallback}"\n'
            'if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && '
            '[ -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]; then\n'
            '  :\n'
            'else\n'
            f'  export IDF_PYTHON_ENV_PATH="{fallback_env}"\n'
            'fi\n'
            f'export PATH="{fallback}/tools:$PATH"\n')
        if not active_python:
            (active_env / "bin/python").unlink()
        env = os.environ.copy()
        env.update(HOME=str(root), PATH="/usr/bin:/bin")
        for name in ("IDF_PATH", "IDF_PYTHON_ENV_PATH", "ESP_PYTHON",
                     "ESP_IDF_VERSION", "IDF_DEACTIVATE_FILE_PATH"):
            env.pop(name, None)
        if active_version is not None:
            env.update(PATH=f"{active}/tools:{env['PATH']}", IDF_PATH=str(active),
                       IDF_PYTHON_ENV_PATH=str(active_env))
        command = (
            f'source "{ROOT / "scripts/openbrewbyweight_idf.sh"}"; '
            'OBBW_IDF_QUIET=1; obbw_idf_source; '
            'printf "%s|%s|%s" "$IDF_PATH" "$IDF_PYTHON_ENV_PATH" '
            '"$(command -v idf.py)"')
        return subprocess.run(["bash", "-c", command], env=env,
                              capture_output=True, text=True), active, fallback


active_idf, active_root, _ = idf_activation("6.1")
active_values = active_idf.stdout.split("|")
assert active_idf.returncode == 0 and active_values == [
    str(active_root), str(active_root.parent / "active-python"),
    str(active_root / "tools/idf.py")], active_idf.stderr
for version, has_python in (("6.0", True), ("6.1", False)):
    fallback_idf, _, fallback_root = idf_activation(version, has_python)
    fallback_values = fallback_idf.stdout.split("|")
    assert fallback_idf.returncode == 0 and fallback_values == [
        str(fallback_root), str(fallback_root.parent.parent / "fallback-python"),
        str(fallback_root / "tools/idf.py")], fallback_idf.stderr
legacy_idf, _, fallback_root = idf_activation(None)
legacy_values = legacy_idf.stdout.split("|")
assert legacy_idf.returncode == 0 and legacy_values == [
    str(fallback_root), str(fallback_root.parent.parent / "fallback-python"),
    str(fallback_root / "tools/idf.py")], legacy_idf.stderr


def idf_python_env_selection(python_env: str | None, home_leaf: str = "home"):
    """Run the fallback path with an optional installed python_env layout."""
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-idf-env-") as temporary:
        root = Path(temporary)
        home = root / home_leaf
        fallback = home / "esp/esp-idf-v6.1"
        missing_env = home / ".espressif/python_env/idf6.1_py3.11_env"
        installed_env = home / ".espressif/python_env/idf6.1_py3.14_env"
        for directory in (fallback / "tools",
                          fallback / "components/esp_common/include",
                          missing_env / "bin", installed_env / "bin"):
            directory.mkdir(parents=True)
        (fallback / "tools/idf.py").write_text("#!/bin/sh\necho 'ESP-IDF v6.1'\n")
        (fallback / "tools/idf.py").chmod(0o755)
        (fallback / "components/esp_common/include/esp_idf_version.h").write_text(
            "#define ESP_IDF_VERSION_MAJOR   6\n"
            "#define ESP_IDF_VERSION_MINOR   1\n")
        if python_env == "installed":
            (installed_env / "bin/python").write_text("#!/bin/sh\nexit 0\n")
            (installed_env / "bin/python").chmod(0o755)
            (installed_env / "idf_version.txt").write_text("6.1")
        (fallback / "export.sh").write_text(
            f'export IDF_PATH="{fallback}"\n'
            'if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && '
            '[ -x "${IDF_PYTHON_ENV_PATH}/bin/python" ]; then\n'
            '  :\n'
            'else\n'
            f'  export IDF_PYTHON_ENV_PATH="{missing_env}"\n'
            'fi\n'
            f'export PATH="{fallback}/tools:$PATH"\n')
        env = os.environ.copy()
        env.update(HOME=str(home), PATH="/usr/bin:/bin")
        for name in ("IDF_PATH", "IDF_PYTHON_ENV_PATH", "ESP_PYTHON",
                     "ESP_IDF_VERSION", "IDF_DEACTIVATE_FILE_PATH"):
            env.pop(name, None)
        expected = (str(installed_env) if python_env == "installed"
                    else str(missing_env))
        command = (
            f'source "{ROOT / "scripts/openbrewbyweight_idf.sh"}"; '
            'OBBW_IDF_QUIET=1; obbw_idf_source; '
            'printf "%s|%s" "$IDF_PATH" "$IDF_PYTHON_ENV_PATH"')
        result = subprocess.run(["bash", "-c", command], env=env,
                                capture_output=True, text=True)
        return result, fallback, installed_env, missing_env, expected


selection, fallback_root, installed_env, missing_env, expected = \
    idf_python_env_selection("installed")
assert selection.returncode == 0 and selection.stdout == (
    f"{fallback_root}|{expected}"), selection.stderr
selection, fallback_root, _, _, expected = idf_python_env_selection("missing")
assert selection.returncode == 0 and selection.stdout == (
    f"{fallback_root}|{expected}"), selection.stderr
# A HOME containing spaces must not break the environment scan.
selection, fallback_root, _, _, expected = \
    idf_python_env_selection("installed", home_leaf="my esp home")
assert selection.returncode == 0 and selection.stdout == (
    f"{fallback_root}|{expected}"), selection.stderr


def flash_command(layout: str, *extra: str, arch: str = "n16r8"):
    with tempfile.TemporaryDirectory(prefix="openbrewbyweight-flash-") as temporary:
        root = Path(temporary)
        idf = root / "fake-idf"
        tools = idf / "tools"
        python_env = root / "python-env/bin"
        build = root / "build-idf/esp32-s3-relay-x1-speaker--rancilio-silvia-pro-x"
        partition_tool = idf / "components/partition_table/gen_esp32part.py"
        for directory in (tools, python_env, build, partition_tool.parent,
                          root / "idf"):
            directory.mkdir(parents=True, exist_ok=True)
        log = root / "commands.log"
        image = root / "external.bin"
        image.write_bytes(b"external")
        (build / "openbrewbyweight.bin").write_bytes(b"project")
        (build / "flash_args").write_text("0x0 bootloader.bin\n")
        (idf / "export.sh").write_text("return 99\n")
        (python_env / "python").write_text("#!/bin/sh\nexit 0\n")
        (python_env / "python").chmod(0o755)
        # Emits the current layout the way gen_esp32part prints it (decimal
        # subtypes, K sizes), or a current-layout mutation.
        partition_tool.write_text(
            "from pathlib import Path\nimport os,sys\n"
            "arch, layout = os.environ['EXPECTED_ARCH'], os.environ['FLASH_LAYOUT']\n"
            "source = Path('idf', f'partitions-{arch}.csv')\n"
            "rows = [[cell.strip() for cell in line.split(',')][:5] for line in "
            "source.read_text().splitlines() if line and not line.startswith('#')]\n"
            "edits = {'wrong': (6, 3, '0x69F000'), 'type': (6, 1, 'app'), "
            "'subtype': (6, 2, '0x41'), 'size': (6, 4, '0xE000'), 'nvs': (0, 4, '0x5000')}\n"
            "if layout in edits: row, column, value = edits[layout]; rows[row][column] = value\n"
            "drop = {'missing': 'shotcurve', 'loose': 'auditlog'}.get(layout)\n"
            "rows = [row for row in rows if row[0] != drop]\n"
            "fmt = lambda value: str(int(value, 0)) if value.startswith('0x') else value\n"
            "Path(sys.argv[-1]).write_text(''.join(f'{n},{ty},{fmt(st)},{o},{int(sz, 0) // 1024}K\\n' "
            "for n, ty, st, o, sz in rows))\n")
        shutil.copy(ROOT / f"idf/partitions-{arch}.csv", root / "idf")
        (root / "scripts").mkdir()
        (root / "scripts/resolve_build_profiles.py").write_text(FAKE_RESOLVER)
        (tools / "idf.py").write_text(
            "#!/bin/sh\n"
            f'printf "idf:%s\\n" "$*" >>"{log}"\n'
            "case \" $* \" in *' --version '*) echo 'ESP-IDF v6.1';; esac\n")
        (tools / "node").write_text("#!/bin/sh\nexit 0\n")
        (tools / "python").write_text(f'#!/bin/sh\nexec "{sys.executable}" "$@"\n')
        (tools / "esptool").write_text(
            "#!/usr/bin/env python3\nimport os, pathlib, sys\n"
            f"open({str(log)!r}, 'a').write('esptool:' + ' '.join(sys.argv[1:]) + '\\n')\n"
            "if 'read-flash' in sys.argv:\n"
            "    if os.environ['FLASH_LAYOUT'] == 'unknown': sys.exit(1)\n"
            "    fill = 255 if os.environ['FLASH_LAYOUT'] == 'blank' else 0\n"
            "    pathlib.Path(sys.argv[-1]).write_bytes(bytes([fill]) * 4096)\n")
        shutil.copy2(tools / "python", tools / "python3")
        for executable in (tools / "idf.py", tools / "node", tools / "python",
                           tools / "python3", tools / "esptool"):
            executable.chmod(0o755)
        env = os.environ.copy()
        env.update(OBBW_CLI_ROOT=str(root), OPENBREWBYWEIGHT_NONINTERACTIVE="1",
                   IDF_PATH=str(idf), IDF_PYTHON_ENV_PATH=str(python_env.parent),
                   FLASH_LAYOUT=layout, EXPECTED_ARCH=arch, TMPDIR=str(root),
                   PATH=f"{tools}:/usr/bin:/bin")
        args = [str(INTERNAL / "flash-idf"), "--port", "/dev/null",
                "--hardware", "esp32-s3-relay-x1-speaker",
                "--machine", "rancilio-silvia-pro-x", *extra]
        if "--image" in extra:
            args[args.index("--image") + 1] = str(image)
        result = subprocess.run(args, env=env, cwd=ROOT, capture_output=True, text=True)
        return result, log.read_text().splitlines()


blank_result, blank_flash = flash_command("blank")
assert blank_result.returncode == 0 and \
    any("write-flash @flash_args" in line for line in blank_flash), blank_result.stderr
erased_result, erased_flash = flash_command("missing", "--erase-all")
assert erased_result.returncode == 0 and \
    any("erase-flash" in line for line in erased_flash) and \
    any("write-flash @flash_args" in line for line in erased_flash), erased_flash
present_result, present_flash = flash_command("present")
assert present_result.returncode == 0 and \
    any("write-flash @flash_args" in line for line in present_flash) and \
    not any(" flash" in line for line in present_flash if line.startswith("idf:")), present_flash
unchecked_result, unchecked_flash = flash_command("present", "--no-check")
assert unchecked_result.returncode == 0 and \
    any("write-flash @flash_args" in line for line in unchecked_flash) and \
    not any(" flash" in line for line in unchecked_flash if line.startswith("idf:")), \
    unchecked_flash
external_result, external_flash = flash_command("present", "--image", "placeholder")
assert external_result.returncode == 0 and \
    any("write-flash 0x70000" in line for line in external_flash) and \
    not any(" flash" in line for line in external_flash if line.startswith("idf:")), \
    external_flash
for incompatible_layout in ("missing", "wrong", "type", "subtype", "size", "nvs", "loose"):
    for extra in ((), ("--image", "placeholder")):
        rejected, commands = flash_command(incompatible_layout, *extra)
        assert rejected.returncode == 1 and \
            "not the current n16r8 project layout" in rejected.stderr and \
            "--erase-all" in rejected.stderr, (incompatible_layout, extra, rejected.stderr)
        assert not any("write-flash" in line or "erase" in line for line in commands), commands
flash_language = subprocess.run(
    [str(INTERNAL / "flash-idf"), "--webui-language", "en"],
    cwd=ROOT, capture_output=True, text=True)
assert flash_language.returncode == 2 and "build-only" in flash_language.stderr

scripts_text = "\n".join(
    path.read_text(errors="replace") for path in (ROOT / "scripts").rglob("*")
    if path.is_file())
idf_helpers = (ROOT / "scripts/openbrewbyweight_idf.sh").read_text()
assert 'IDF_DEFAULT_HOME="${HOME}/esp/esp-idf-v6.1"' in idf_helpers, \
    "official SDK discovery must use the versioned ESP-IDF 6.1 checkout"
assert "python@3.12" not in idf_helpers, \
    "ESP-IDF 6.1 must activate the Python environment created by its installer"
assert '. "${idf_root}/export.sh" >/dev/null' in idf_helpers, \
    "non-interactive builds must not print ESP-IDF shell-completion warnings"


def verify_production_profile(arch: str, selectors: list[str], integration: str = "none",
                              extra_flags: str = ""):
    with tempfile.NamedTemporaryFile(mode="w", prefix="openbrewbyweight-sdkconfig-",
                                     delete=False) as fixture:
        fixture.write("\n".join(selectors) + "\n")
        fixture_path = fixture.name
    try:
        return subprocess.run(
            ["bash", "-c",
             f'source "{ROOT / "scripts/openbrewbyweight_idf.sh"}"; '
             'obbw_idf_verify_production_profile "$1" "$2"',
             "profile-test", arch, fixture_path],
            cwd=ROOT, env={**os.environ, "OPENBREWBYWEIGHT_MACHINE_INTEGRATION": integration,
                           "OPENBREWBYWEIGHT_EXTRA_FLAGS": extra_flags},
            text=True, capture_output=True)
    finally:
        Path(fixture_path).unlink()


profile_common = [
    'CONFIG_PARTITION_TABLE_CUSTOM=y',
    'CONFIG_ESPTOOLPY_FLASHMODE_DIO=y',
    'CONFIG_ESPTOOLPY_FLASHMODE="dio"',
    'CONFIG_ESPTOOLPY_FLASHMODE_VAL=3',
    'CONFIG_ESPTOOLPY_FLASHFREQ_80M=y',
    'CONFIG_ESPTOOLPY_FLASHFREQ="80m"',
    'CONFIG_SPIRAM_SPEED_80M=y',
    'CONFIG_SPIRAM_SPEED=80',
    'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768',
    'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=2048',
    'CONFIG_MMU_PAGE_SIZE_64KB=y',
    'CONFIG_MMU_PAGE_SIZE=0x10000',
    'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y',
    'CONFIG_BOOTLOADER_WDT_ENABLE=y',
    'CONFIG_BOOTLOADER_WDT_TIME_MS=9000',
    'CONFIG_ESP_INT_WDT=y',
    'CONFIG_ESP_INT_WDT_TIMEOUT_MS=800',
    'CONFIG_ESP_TASK_WDT_EN=y',
    'CONFIG_ESP_TASK_WDT_INIT=y',
    'CONFIG_ESP_TASK_WDT_PANIC=y',
    'CONFIG_ESP_TASK_WDT_TIMEOUT_S=5',
    'CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y',
    'CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y',
    'CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y',
    'CONFIG_ESP_SYSTEM_PANIC_REBOOT_DELAY_SECONDS=0',
    'CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y',
    '# CONFIG_RMT_TX_ISR_HANDLER_IN_IRAM is not set',
    '# CONFIG_RMT_RX_ISR_HANDLER_IN_IRAM is not set',
    '# CONFIG_RMT_ENCODER_FUNC_IN_IRAM is not set',
    '# CONFIG_SPI_MASTER_ISR_IN_IRAM is not set',
    '# CONFIG_SPI_SLAVE_ISR_IN_IRAM is not set',
    '# CONFIG_GDMA_CTRL_FUNC_IN_IRAM is not set',
    '# CONFIG_GDMA_ISR_HANDLER_IN_IRAM is not set',
    'CONFIG_LWIP_IPV6=y',
    'CONFIG_LWIP_MAX_SOCKETS=18',
    'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=8',
    'CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32',
    'CONFIG_ESP_WIFI_RX_BA_WIN=16',
    'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16',
    'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=32',
    'CONFIG_LWIP_TCP_SND_BUF_DEFAULT=16384',
    'CONFIG_LWIP_TCP_WND_DEFAULT=16384',
    'CONFIG_LWIP_TCP_RECVMBOX_SIZE=16',
    'CONFIG_MDNS_MAX_INTERFACES=2',
    'CONFIG_MDNS_MEMORY_ALLOC_SPIRAM=y',
    'CONFIG_BT_CTRL_MODEM_SLEEP=y',
    'CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y',
    '# CONFIG_OPEN_BREW_BY_WEIGHT_DEVELOPMENT is not set',
    '# CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_REMOTE_MACHINE_CONTROL is not set',
    'CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0',
    'CONFIG_ESP_CONSOLE_NONE=y',
]
profile_arch = {
    "n16r8": ['CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y',
              'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"',
              'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions-n16r8.csv"',
              'CONFIG_PARTITION_TABLE_FILENAME="partitions-n16r8.csv"',
              'CONFIG_SPIRAM_MODE_OCT=y',
              'CONFIG_SPIRAM_XIP_FROM_PSRAM=y',
              'CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y',
              'CONFIG_MDNS_TASK_CREATE_FROM_INTERNAL=y'],
}
for arch, arch_selectors in profile_arch.items():
    selectors = profile_common + arch_selectors
    assert verify_production_profile(arch, selectors).returncode == 0
    for selector in selectors:
        result = verify_production_profile(
            arch, [item for item in selectors if item != selector])
        assert result.returncode == 1 and "Production profile mismatch" in result.stderr
    assert verify_production_profile(
        arch, selectors + ["CONFIG_SPIRAM_MODE_QUAD=y"]).returncode == 1
    micra = selectors + ["CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y",
                         "CONFIG_MBEDTLS_DYNAMIC_BUFFER=y",
                         "CONFIG_WS_BUFFER_SIZE=4096"]
    assert verify_production_profile(arch, micra, "linea_micra_cloud").returncode == 0
    assert "CONFIG_WS_BUFFER_SIZE=4096" in verify_production_profile(
        arch, micra[:-1], "linea_micra_cloud").stderr
    assert "unexpected Micra TLS allocator" in verify_production_profile(
        arch, micra).stderr
    jtag_define = "-DOPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1"
    jtag_selectors = [item for item in selectors if item not in {
        "CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=0", "CONFIG_ESP_CONSOLE_NONE=y"}] + [
        "CONFIG_OPEN_BREW_BY_WEIGHT_ENABLE_JTAG=1", "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y"]
    assert verify_production_profile(
        arch, jtag_selectors, extra_flags=jtag_define).returncode == 0
    assert verify_production_profile(arch, jtag_selectors).returncode == 1
    assert verify_production_profile(arch, selectors, extra_flags=jtag_define).returncode == 1

durable_stores = (ROOT / "src/OpenBrewByWeightDurableStores.h").read_text()
network_reset = durable_stores.split("bool resetPersistedNetworkAccess", 1)[1].split(
    "bool resetAllDurableStores", 1)[0]
assert re.findall(r"^\s+PersistedSettings\s+(\w+);", network_reset, re.MULTILINE) == []
scale_worker = (ROOT / "src/OpenBrewByWeightScaleWorker.cpp").read_text()
cleanup = scale_worker.split("if (!runtimeReady) {", 1)[1].split(
    "scaleWorkerStartupFinished.store", 1)[0]
cleanup_order = [cleanup.index(token) for token in (
    "feedCurrentTaskWatchdog()", "openBrewByWeightBleRuntimeStop(BLE_STACK_STOP_WAIT_MS)",
    "esp_task_wdt_delete(nullptr)", "scaleWorkerTaskHandle.store(nullptr")]
assert cleanup_order == sorted(cleanup_order)
assert "vQueueDelete" not in cleanup
assert "std::atomic<TaskHandle_t> scaleWorkerTaskHandle" in scale_worker
assert "TaskLockGuard lock(scaleWorkerTaskHandleMutex)" in cleanup
reset_history = (ROOT / "src/OpenBrewByWeightResetHistoryStore.h").read_text()
checkpoint = reset_history.split("bool persistResetUptimeCheckpoint", 1)[1].split(
    "bool clearPersistedResetHistory", 1)[0]
assert checkpoint.count("resetHistoryStoreLastCheckpointMs.load") == 2
assert checkpoint.index("resetHistoryStoreLastCheckpointMs.load") < checkpoint.index(
    "tryLockFlashIo")
buzzer = (ROOT / "src/OpenBrewByWeightBuzzer.h").read_text()
assert "TaskMutex mutex" in buzzer and "portMUX" not in buzzer
network_header = (ROOT / "src/OpenBrewByWeightNetwork.h").read_text()
assert "std::atomic<uint32_t> lastTaskProgressAtMs_" in network_header
nimble_client = (ROOT / "libraries/EspressoScaleBLE/src/EspressoScaleBLENimble.cpp").read_text()
for capacity in ("kRxFrameCount", "kCriticalEventCount", "kEventCount"):
    assert f"drained < {capacity}" in nimble_client
shot_curve = (ROOT / "src/OpenBrewByWeightShotCurve.h").read_text()
assert "flashIoLockTimeouts() == lastLockTimeouts_" in shot_curve
assert "(void)load();" in shot_curve
persistence_runtime = (ROOT / "src/persistence/OpenBrewByWeightCommandPersistence.inc").read_text()
checkpoint = persistence_runtime.split("SETTINGS_PERSIST_IDLE_WAIT_MS", 1)[1].split(
    "yieldSettingsNvs", 1)[0]
for gate in ("copyControlGate(control)", "control.activeCycle", "control.relayClosed",
             "control.machineRunning", "control.physicalActivatorOn", "link.connecting"):
    assert gate in checkpoint
assert checkpoint.index("feedOrTripCurrentTaskWatchdog()") < checkpoint.index(
    "persistResetUptimeCheckpoint")
ota_service = (ROOT / "src/network/OpenBrewByWeightNetworkOta.inc").read_text()
assert "control.bootState == BootState::READY && startupComplete_" in ota_service
ota_boot_call = ota_service.split("ota.serviceBoot", 1)[1].split(
    "OTA_CONFIRM_MIN_UPTIME_MS", 1)[0]
for gate in ("control.activeCycle", "control.machineRunning",
             "control.physicalActivatorOn"):
    assert gate in ota_boot_call
control_gate = (ROOT / "src/diagnostics/OpenBrewByWeightDiagnostics.inc").read_text().split(
    "void publishControlGate()", 1)[1].split("void publishControlStatus()", 1)[0]
for source in ("bootCapabilities", "criticalTaskWatchdogFaulted()",
               "RelaySafetyState::LOCKOUT"):
    assert source in control_gate
assert "next.bootState = effectiveBoot.state()" in control_gate
assert "constexpr uint32_t FLASH_IO_LOCK_TIMEOUT_MS = 3000" in \
    (ROOT / "src/OpenBrewByWeightFlashIoScratch.h").read_text()
assert r"component_validation\.cmake" in idf_helpers and \
    'idf "esp_wifi/"' in idf_helpers and 'idf "wpa_supplicant/"' in idf_helpers, \
    "only the known ESP-IDF 6.1 Wi-Fi component warnings may be filtered"
assert 'build 2>&1 | obbw_idf_filter_output' in \
    (INTERNAL / "build-idf").read_text(), \
    "the firmware build must use the scoped external-warning filter"
for removed_alias in ("b", "bf", "bfm", "bo", "bsfm", "build", "f", "flash",
                      "iwyu", "m", "monitor", "o", "ota", "s", "static",
                      "static-tidy", "warnings"):
    assert not (ROOT / "scripts" / removed_alias).exists(), \
        f"legacy Arduino-era alias remains: {removed_alias}"
assert not re.search(r"--(?:token|password)\s+['\"]", scripts_text), \
    "credentials must not be forwarded in argv"
for analyzer in ("warnings-idf", "gcc_analyzer"):
    analyzer_text = (ROOT / "scripts" / analyzer).read_text()
    assert "./scripts/internal/build-idf" in analyzer_text
    # Profile defines reach the build only through the forwarded switches.
    assert "obbw_cli_flags_for development release no_auth_admin jtag" in analyzer_text
cppcheck_suppressions = (ROOT / "scripts/cppcheck-suppressions.txt").read_text()
assert "**" not in cppcheck_suppressions, \
    "Cppcheck suppression globs must use a single '*' wildcard"
assert "*:*/esp-idf*/components/*" in cppcheck_suppressions, \
    "Cppcheck must suppress dependencies from versioned ESP-IDF checkouts"
tests_text = "\n".join(path.read_text(errors="replace")
                       for path in (ROOT / "src/tests").iterdir() if path.is_file())
assert "npm" + " install" not in tests_text and "npm" + " ci" not in tests_text, \
    "tests must not install dependencies"

cppcheck_help = subprocess.run([str(ROOT / "scripts/static-idf"), "--help"],
                              cwd=ROOT, text=True, capture_output=True)
assert cppcheck_help.returncode == 0 and "`unusedFunction`" in cppcheck_help.stdout
assert "--arch" in cppcheck_help.stdout and "command not found" not in cppcheck_help.stderr
resolver = runpy.run_path(str(ROOT / "scripts/resolve_build_profiles.py"))
hardware = [resolver["validate_hardware"](json.loads(path.read_text()))
            for path in (ROOT / "config/hardware").glob("*.json")]
machines = [resolver["validate_machine"](json.loads(path.read_text()))
            for path in (ROOT / "config/machines").glob("*.json")]
supported_pairs = set()
for hardware_profile in hardware:
    for machine_profile in machines:
        try:
            resolver["resolve"](hardware_profile, machine_profile, "")
        except resolver["ProfileError"]:
            continue
        supported_pairs.add((hardware_profile["id"], machine_profile["id"]))
local_pairs = dev_module["BUILD_PROFILES"]
assert len(local_pairs) == len(supported_pairs) and set(local_pairs) == supported_pairs, \
    "local validation must cover every supported profile pair exactly once"
release_baselines = json.loads((ROOT / "config/resource-baselines.json").read_text())
assert set(release_baselines["releaseTargets"]) == {
    f"{hardware}--{machine}" for hardware, machine in local_pairs}, \
    "every supported pair needs its own release-image baseline"
# The opt-in Web UI lint profile never installs: absent linters fail with 127.
assert "test [normal|asan|tsan|web|budgets|ble|ota|tooling|lint]" in dev_module["help_text"]()
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-lint-") as temporary:
    lint_main = runpy.run_path(str(DEV))["main"]
    with patch.dict(lint_main.__globals__, {"ROOT": Path(temporary)}), \
            patch.object(sys, "argv", ["dev", "--verbosity", "compact", "test", "lint"]):
        assert lint_main() == 127, "missing Web UI linters must exit 127"
template = (ROOT / ".github/PULL_REQUEST_TEMPLATE.md").read_text()
for field in ("Risk", "Safety invariants", "Validation", "HIL/manual evidence"):
    assert field in template, f"PR template field missing: {field}"

# Stack-depth gate: only the entry-to-root chain counts, below the root every
# call is bounded (reserves for pointers and externals), and gaps fail loudly.
stack = runpy.run_path(str(ROOT / "scripts/check_stack_depth.py"))
Gate, kernel = stack["Gate"], stack["KERNEL_RESERVE"]


def stack_graph(frames: dict[str, int], edges: list[tuple[str, str]]):
    graph = stack["Graph"]()
    graph.add([f'node: {{ title: "{t}" label: "{t}()\\nfile.cpp:1:1\\n{n} bytes (static)" }}'
               for t, n in frames.items()] +
              [f'edge: {{ sourcename: "{a}" targetname: "{b}" }}' for a, b in edges])
    return graph


def stack_eval(graph, budget=4096, indirect=None):
    gate = Gate("t", lambda _: budget, r"\bentry\(", (r"\broot\(",), indirect or {})
    return stack["evaluate"](graph, gate, budget)


chain = stack_graph({"entry": 32, "setup": 1000, "root": 100, "leaf": 200,
                     "other": 5000, "__assert_func": 4000},
                    [("entry", "setup"), ("entry", "other"), ("setup", "root"),
                     ("root", "leaf"), ("leaf", "memcpy"), ("leaf", "__assert_func")])
result = stack_eval(chain)
assert result.depth == 32 + 1000 + 100 + 200 + stack["EXTERNAL_RESERVE"] + kernel
assert [name for _, name in result.path] == ["entry()", "setup()", "root()", "leaf()"]
assert not result.errors
pointer = stack_graph({"entry": 32, "root": 100, "ptr": 64},
                      [("entry", "root"), ("root", "ptr"), ("ptr", "__indirect_call")])
assert stack_eval(pointer).depth == 32 + 100 + 64 + stack["INDIRECT_RESERVE"] + kernel
hidden = stack_graph({"entry": 32, "dispatch": 48, "root": 100},
                     [("entry", "dispatch"), ("dispatch", "__indirect_call")])
assert "entry does not reach any root; declare the indirect edge" in stack_eval(hidden).errors
declared = stack_eval(hidden, indirect={r"\bdispatch\(": (r"\broot\(",)})
assert declared.depth == 32 + 48 + 100 + kernel and not declared.errors
loop = stack_graph({"entry": 32, "root": 100, "a": 64}, [("entry", "root"), ("root", "a"),
                                                          ("a", "root")])
assert "unbounded recursion through a(), root()" in stack_eval(loop).errors
# lwip_send <-> lwip_sendto is a declared two-level recursion: charged twice.
bounded = stack_graph({"entry": 32, "root": 100, "lwip_send": 64, "lwip_sendto": 80},
                      [("entry", "root"), ("root", "lwip_send"),
                       ("lwip_send", "lwip_sendto"), ("lwip_sendto", "lwip_send")])
bounded_result = stack_eval(bounded)
assert bounded_result.depth == 32 + 100 + 2 * (64 + 80) + kernel, bounded_result
assert not bounded_result.errors
assert any("recursion x2 over 2 functions" in name for _, name in bounded_result.path)
assert stack_eval(chain, budget=1000).depth > 1000
with tempfile.TemporaryDirectory(prefix="openbrewbyweight-stack-") as temporary:
    (Path(temporary) / "sdkconfig").write_text("CONFIG_ARDUINO_LOOP_STACK_SIZE=8192\n")
    assert stack["sdkconfig_value"](Path(temporary), "CONFIG_ARDUINO_LOOP_STACK_SIZE") == 8192
assert stack["source_constant"]("src/OpenBrewByWeightNetwork.cpp",
                                "NETWORK_MANAGER_TASK_STACK_SIZE") > 0
assert '"scripts/check_stack_depth.py"' in DEV.read_text(), "R2/R3 must run the stack gate"
print(f"developer facade: {len(golden)} risk cases, 11 context areas, and script contracts passed")

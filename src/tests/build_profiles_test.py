#!/usr/bin/env python3
"""Contract tests for named hardware and machine build profiles."""

import json
import subprocess
import tempfile
from pathlib import Path

if not __debug__:
    raise SystemExit("assertions are disabled (PYTHONOPTIMIZE); these checks would not run")


ROOT = Path(__file__).resolve().parents[2]
RESOLVER = ROOT / "scripts/resolve_build_profiles.py"
HARDWARE = ROOT / "config/hardware/esp32-s3-relay-x1-speaker.json"
HARDWARE_REED = ROOT / "config/hardware/esp32-s3-relay-x1-speaker-reed.json"
PRO_X = ROOT / "config/machines/rancilio-silvia-pro-x.json"
PRO_X_REED = ROOT / "config/machines/rancilio-silvia-pro-x-reed.json"
MICRA = ROOT / "config/machines/la-marzocco-linea-micra.json"


def run(hardware=HARDWARE, machine=PRO_X, flags=""):
    temporary = tempfile.TemporaryDirectory(prefix="openbrewbyweight-profile-")
    result = subprocess.run(
        ["python3", str(RESOLVER), "--hardware", str(hardware),
         "--machine", str(machine), f"--flags={flags}",
         "--output-root", temporary.name],
        cwd=ROOT, capture_output=True, text=True)
    return temporary, result


def changed(source: Path, mutate):
    value = json.loads(source.read_text())
    mutate(value)
    handle = tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", prefix="openbrewbyweight-profile-fixture-",
        delete=False)
    json.dump(value, handle)
    handle.close()
    return Path(handle.name)


def expect_failure(hardware=HARDWARE, machine=PRO_X, flags="", text=""):
    temporary, result = run(hardware, machine, flags)
    try:
        assert result.returncode == 2, result.stdout + result.stderr
        assert text in result.stderr, result.stderr
    finally:
        temporary.cleanup()


for hardware, machine, expected_type, integration, brand, model, icon in (
        (HARDWARE, PRO_X, "1", "none", "Rancilio", "Silvia Pro X", None),
        (HARDWARE_REED, PRO_X_REED, "2", "none", "Rancilio", "Silvia Pro X", None),
        (HARDWARE, MICRA, "0", "linea_micra_cloud", "La Marzocco", "Linea Micra",
         "linea-micra")):
    temporary, result = run(hardware=hardware, machine=machine)
    try:
        assert result.returncode == 0, result.stderr
        output = dict(line.split("=", 1) for line in result.stdout.splitlines())
        assert output["arch"] == "n16r8"
        assert output["variant"].startswith(hardware.stem + "--")
        generated = Path(output["generated_dir"])
        header = (generated / "OpenBrewByWeightBuildProfileGenerated.h").read_text()
        manifest = json.loads((generated / "build-profile.json").read_text())
        assert f"#define OPEN_BREW_BY_WEIGHT_MACHINE_TYPE {expected_type}" in header
        assert f'#define OPEN_BREW_BY_WEIGHT_MACHINE_MODEL "{model}"' in header
        expected_integration = "1" if integration == "linea_micra_cloud" else "0"
        assert (f"#define OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION "
                f"{expected_integration}") in header
        # The machine's Home icon is profile metadata the build forwards to the
        # Web UI generator; profiles without it keep the default generic glyph.
        if icon:
            assert f'#define OPEN_BREW_BY_WEIGHT_MACHINE_WEB_ICON "{icon}"' in header
        else:
            assert "OPEN_BREW_BY_WEIGHT_MACHINE_WEB_ICON" not in header
        assert manifest["machine_integration"] == integration
        assert manifest["machine"]["brand"] == brand
        assert manifest["hardware"]["relay"] == {
            "gpio": 2, "contact_type": "normally_open",
            "closed_level": "high", "open_level": "low"}
        assert manifest["hardware"]["metadata"] == {
            "product_url": "https://aliexpress.com/item/1005011880181624.html",
            "identifiers": ["303E32S3DC2", "5437314A_Y1248-250908"]}
    finally:
        temporary.cleanup()

temporary, result = run(machine=PRO_X)
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    defaults = json.loads((generated / "build-profile.json").read_text())[
        "machine"]["factory_defaults"]
    assert defaults["momentary"] == {
        "start_edge": "press", "stop_pulse_ms": 300,
        "max_single_press_ms": 1000,
        "assume_idle_on_scale_connect": True,
        "shot_reaction_timeout_s": 0}
finally:
    temporary.cleanup()

custom_micra = changed(MICRA, lambda value: value.update(id="custom-micra"))
temporary, result = run(machine=custom_micra)
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    manifest = json.loads((generated / "build-profile.json").read_text())
    header = (generated / "OpenBrewByWeightBuildProfileGenerated.h").read_text()
    assert manifest["machine_integration"] == "linea_micra_cloud"
    assert "#define OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION 1" in header
finally:
    temporary.cleanup()
    custom_micra.unlink()

unknown_integration = changed(
    PRO_X, lambda value: value.update(integration="future_machine_bus"))
try:
    expect_failure(machine=unknown_integration, text="machine.integration")
finally:
    unknown_integration.unlink()

bad_icon = changed(
    MICRA, lambda value: value.update(machine_icon="Not An Icon"))
try:
    expect_failure(machine=bad_icon, text="machine.machine_icon")
finally:
    bad_icon.unlink()

temporary, result = run(hardware=HARDWARE_REED, machine=PRO_X_REED)
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    manifest = json.loads((generated / "build-profile.json").read_text())
    assert manifest["hardware"]["reed"] == {
        "present": True, "gpio": 13, "active_level": "low",
        "pull": "up", "debounce_ms": 30}
    assert manifest["machine"]["factory_defaults"]["momentary"][
        "reed_confirm_timeout_ms"] == 1000
finally:
    temporary.cleanup()

temporary, result = run(machine=MICRA)
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    defaults = json.loads((generated / "build-profile.json").read_text())[
        "machine"]["factory_defaults"]
    assert defaults["paddle"] == {
        "mode": "natural", "return_reminder": {
            "enabled": True, "interval_ms": 30000,
            "max_duration_ms": 900000}}
    assert defaults["quick_rinse"]["enabled"] is True
finally:
    temporary.cleanup()

temporary, override = run(flags=(
    "-DOPEN_BREW_BY_WEIGHT_RELAY_GPIO=3 "
    "-DOPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_ENABLED=1 "
    "-DOPEN_BREW_BY_WEIGHT_DEVELOPMENT=1 -DCUSTOM_BUILD_NOTE=7"))
try:
    assert override.returncode == 0, override.stderr
    output = dict(line.split("=", 1) for line in override.stdout.splitlines())
    generated = Path(output["generated_dir"])
    manifest = json.loads((generated / "build-profile.json").read_text())
    header = (generated / "OpenBrewByWeightBuildProfileGenerated.h").read_text()
    assert manifest["hardware"]["relay"]["gpio"] == 3
    assert manifest["machine"]["factory_defaults"]["quick_rinse"]["enabled"]
    assert "OPEN_BREW_BY_WEIGHT_DEVELOPMENT" not in header
finally:
    temporary.cleanup()

bad = changed(HARDWARE, lambda value: value.pop("reed"))
expect_failure(hardware=bad, text="hardware is missing: reed")
bad.unlink()

bad = changed(HARDWARE, lambda value: value["reed"].update(gpio=13))
expect_failure(hardware=bad, text="hardware.reed has unknown fields: gpio")
bad.unlink()

bad = changed(HARDWARE, lambda value: value["relay"].update(gpio=21))
expect_failure(hardware=bad, text="GPIO 21 is shared")
bad.unlink()

bad = changed(HARDWARE, lambda value: value.update(development=True))
expect_failure(hardware=bad, text="unknown fields: development")
bad.unlink()

custom = changed(HARDWARE, lambda value: value["metadata"].update(
    brand="Custom PCB", author="Example builder",
    links={"schematic": "any-value"}))
temporary, result = run(hardware=custom)
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    metadata = json.loads((generated / "build-profile.json").read_text())[
        "hardware"]["metadata"]
    assert metadata["brand"] == "Custom PCB"
    assert metadata["author"] == "Example builder"
    assert metadata["links"] == {"schematic": "any-value"}
finally:
    temporary.cleanup()
    custom.unlink()

bad = changed(PRO_X, lambda value: value["interface"].update(
    control="momentary", feedback="reed"))
expect_failure(machine=bad, text="factory_defaults")
bad.unlink()

expect_failure(flags="-DOPEN_BREW_BY_WEIGHT_MACHINE_TYPE=2",
               text="conflicts with the machine profile")
expect_failure(machine=PRO_X_REED,
               text="not compatible with machine profile")
bare = changed(HARDWARE, lambda value: value.update(id="bare-controller"))
reed_on_bare = changed(PRO_X_REED, lambda value: value.update(hardware=["bare-controller"]))
expect_failure(hardware=bare, machine=reed_on_bare,
               text="machine requires reed feedback")
bare.unlink()
reed_on_bare.unlink()
expect_failure(flags="-DOPEN_BREW_BY_WEIGHT_REED_GPIO=13",
               text="cannot configure absent reed hardware")
expect_failure(flags="-DOPEN_BREW_BY_WEIGHT_SPEAKER_PRESENT=0",
               text="cannot change physical presence")
expect_failure(flags='-DOPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID="other"',
               text="profile identity cannot be overridden")
expect_failure(flags="-DOPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S=2",
               text="must be an integer from 3 to 30")

temporary, result = run()
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    header = (generated / "OpenBrewByWeightBuildProfileGenerated.h").read_text()
    # Buzzer support follows the hardware profile's speaker presence.
    assert "#define OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER 1" in header
finally:
    temporary.cleanup()

temporary, result = run(flags="-DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=0")
try:
    assert result.returncode == 0, result.stderr
    output = dict(line.split("=", 1) for line in result.stdout.splitlines())
    generated = Path(output["generated_dir"])
    header = (generated / "OpenBrewByWeightBuildProfileGenerated.h").read_text()
    # The header keeps the presence-derived value; the #ifndef guard lets the
    # explicit =0 win at compile time.
    assert "#define OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER 1" in header
finally:
    temporary.cleanup()

bad = changed(HARDWARE, lambda value: value.update(speaker={"present": False}))
expect_failure(hardware=bad, flags="-DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=1",
               text="buzzer support cannot be enabled when the hardware speaker is absent")
bad.unlink()

no_speaker = changed(HARDWARE, lambda value: value.update(
    speaker={"present": False}))
temporary, result = run(hardware=no_speaker)
try:
    assert result.returncode == 0, result.stderr
    generated = Path(dict(line.split("=", 1) for line in
                          result.stdout.splitlines())["generated_dir"])
    header = (generated / "OpenBrewByWeightBuildProfileGenerated.h").read_text()
    assert "#define OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER 0" in header
finally:
    temporary.cleanup()
    no_speaker.unlink()

with tempfile.TemporaryDirectory(prefix="openbrewbyweight-profile-id-") as temporary:
    selected = subprocess.run(
        ["python3", str(RESOLVER), "--hardware", HARDWARE.stem,
         "--machine", PRO_X.stem, "--flags=", "--output-root", temporary],
        cwd=ROOT, capture_output=True, text=True)
    assert selected.returncode == 0, selected.stderr
    assert f"variant={HARDWARE.stem}--{PRO_X.stem}" in selected.stdout

with tempfile.TemporaryDirectory(prefix="openbrewbyweight-profile-check-") as temporary:
    checked = subprocess.run(
        ["python3", str(RESOLVER), "--hardware", HARDWARE.stem,
         "--machine", PRO_X.stem, "--flags=", "--output-root", temporary,
         "--check-only"], cwd=ROOT, capture_output=True, text=True)
    assert checked.returncode == 0, checked.stderr
    assert not any(Path(temporary).iterdir()), "check-only wrote generated files"

listed = subprocess.run(["python3", str(RESOLVER), "--list"], cwd=ROOT,
                        capture_output=True, text=True)
assert listed.returncode == 0, listed.stderr
for profile in (HARDWARE.stem, HARDWARE_REED.stem, PRO_X.stem,
                PRO_X_REED.stem, MICRA.stem):
    assert profile in listed.stdout
assert ("rancilio-silvia-pro-x-reed  Rancilio Silvia Pro X "
        "control=momentary feedback=reed") in listed.stdout

print("Build profiles: schema, defaults, overrides and safety checks OK")

#!/usr/bin/env python3
"""Enforce the F-20 firmware service boundaries without compiling sources.

Also rejects discarded critical OTA, NimBLE and buzzer results, task watchdog
reconfiguration outside its owner, and the legacy project name anywhere outside
the README.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]


def text(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def service_text(root_file: str, directory: str) -> str:
    fragments = sorted((ROOT / directory).glob("*.inc"))
    return text(root_file) + "".join(
        fragment.read_text(encoding="utf-8") for fragment in fragments
    )


def main() -> int:
    failures: list[str] = []

    safety_files = (
        "src/OpenBrewByWeightSafety.h",
        "src/OpenBrewByWeightMachineRelay.h",
        "src/OpenBrewByWeightHardwareTimer.h",
        "src/OpenBrewByWeightWatchdog.h",
        "src/OpenBrewByWeightResetGuard.h",
        "src/OpenBrewByWeightResetGuard.cpp",
    )
    forbidden_safety = (
        "OpenBrewByWeightNetwork",
        "OpenBrewByWeightWebhook",
        "<WiFi",
        "cJSON",
        "esp_http",
    )
    for relative in safety_files:
        source = text(relative)
        for token in forbidden_safety:
            if token in source:
                failures.append(f"SafetyKernel dependency {token!r} in {relative}")

    scale_source = text("src/OpenBrewByWeightScaleWorker.cpp")
    for token in ('#include "OpenBrewByWeightNetwork.h"', "networkManager."):
        if token in scale_source:
            failures.append(f"ScaleService bypasses its bridge via {token!r}")
    for required in (
        "ScaleWorkerBridgeCallbacks",
        "scaleWorkerBridge.syncNetworkRf",
        "configureScaleWorkerBridge",
    ):
        if required not in scale_source and required not in text(
            "src/OpenBrewByWeightScaleWorker.h"
        ):
            failures.append(f"ScaleService bridge contract missing {required!r}")

    machine_common = (
        "src/machine/OpenBrewByWeightMachineIntegration.h",
        "src/machine/OpenBrewByWeightMachineIntegration.cpp",
        "src/machine/OpenBrewByWeightMachineBackflush.h",
    )
    for relative in machine_common:
        source = text(relative)
        for token in ("Micra", "Linea", "brewTarget", "ObservedMode", "Capabilities"):
            if token in source:
                failures.append(
                    f"common machine lifecycle exposes concrete feature {token!r} "
                    f"in {relative}"
                )
    machine_cmake = text("idf/components/openBrewByWeight/CMakeLists.txt")
    for required in (
        "OpenBrewByWeightLaMarzoccoIntegration.cpp",
        "OpenBrewByWeightMachineIntegration.cpp",
    ):
        if required not in machine_cmake:
            failures.append(f"selected machine source contract missing {required!r}")
    la_marzocco_service = text("src/machine/OpenBrewByWeightLaMarzoccoService.cpp")
    for required in (
        "esp_crt_bundle_attach",
        "networkEligible",
        "parseJsonDocumentWithinLimits",
        "clearSession",
    ):
        if required not in la_marzocco_service:
            failures.append(f"La Marzocco cloud boundary missing {required!r}")
    for forbidden in ("NimBLE", "OpenBrewByWeightBle", "ble_gap", "ble_gatt"):
        if forbidden in la_marzocco_service:
            failures.append(f"La Marzocco cloud service depends on BLE via {forbidden!r}")
    entrypoints = text("src/platform/OpenBrewByWeightEntrypoints.inc")
    if "scaleBridgeOk && machineIntegrationOk" in entrypoints:
        failures.append("optional machine integration gates scale-worker startup")

    network = service_text("src/OpenBrewByWeightNetwork.cpp", "src/network") + text(
        "src/OpenBrewByWeightNetwork.h"
    )
    for token in (
        "machineRequestStart(",
        "machineRequestStop(",
        "closeRelayElectrical",
        "openRelayElectrical",
    ):
        if token in network:
            failures.append(f"NetworkService calls control implementation {token!r}")

    independent_harnesses = (
        "src/tests/safety_external_host_test.cpp",
        "src/tests/ota_state_concurrency_host_test.cpp",
        "src/tests/persistence_host_test.cpp",
        "src/tests/webhook_error_host_test.cpp",
        "src/tests/resource_owner_host_test.cpp",
        "src/tests/machine_integration_host_test.cpp",
        "src/tests/la_marzocco_contract_host_test.cpp",
        "libraries/EspressoScaleBLE/tests/scale_ble_portable_test.cpp",
    )
    for relative in independent_harnesses:
        if not (ROOT / relative).is_file():
            failures.append(f"independent service harness missing: {relative}")

    # Versioned ceilings keep new features from silently growing the legacy
    # roots. Reducing them is encouraged; increasing them requires an explicit
    # architecture review and must not be used to land feature code.
    line_ceilings = {
        "src/OpenBrewByWeightNetwork.cpp": 2000,
        "src/openBrewByWeight.cpp": 2000,
        "src/OpenBrewByWeightDomain.h": 3300,
    }
    for relative, ceiling in line_ceilings.items():
        lines = len(text(relative).splitlines())
        if lines > ceiling:
            failures.append(f"legacy root grew beyond cap: {relative} {lines}>{ceiling}")

    for service in (
        "safety", "control", "scale", "network", "persistence",
        "diagnostics", "platform",
    ):
        directory = ROOT / "src" / service
        if not directory.is_dir():
            failures.append(f"service directory missing: src/{service}")
        for implementation in directory.glob("*.inc"):
            lines = len(implementation.read_text(encoding="utf-8").splitlines())
            if lines > 1500:
                failures.append(
                    f"service implementation exceeds cap: "
                    f"{implementation.relative_to(ROOT)} {lines}>1500"
                )

    # Critical results are never discarded with a (void) cast, and only the
    # watchdog owner reconfigures the task watchdog.
    for relative, pattern, description in (
            ("src/OpenBrewByWeightBuzzer.h",
             r"\(void\)esp_timer_(?:stop|start_once)\s*\(", "buzzer timer result"),
            ("src/OpenBrewByWeightOta.cpp",
             r"\(void\)esp_ota_abort\s*\(", "OTA abort result"),
            ("libraries/EspressoScaleBLE/src/EspressoScaleBLENimble.cpp",
             r"\(void\)ble_gap_(?:disc_cancel|conn_cancel|terminate)\s*\(",
             "NimBLE teardown result")):
        if re.search(pattern, text(relative)):
            failures.append(f"discarded {description}: {relative}")
    watchdog = text("src/OpenBrewByWeightWatchdog.h")
    if "TASK_WATCHDOG_OTA_TIMEOUT_MS" in watchdog or "TaskWatchdogOtaWindow" in watchdog:
        failures.append("obsolete global OTA TWDT window in OpenBrewByWeightWatchdog.h")
    for path in (ROOT / "src").rglob("*"):
        if (path.suffix in {".h", ".cpp", ".inc"}
                and path.name != "OpenBrewByWeightWatchdog.h"
                and "esp_task_wdt_reconfigure" in path.read_text(encoding="utf-8")):
            failures.append(
                f"task watchdog reconfigured outside its owner: {path.relative_to(ROOT)}")

    # Legacy project names must not return; only the README names the inspiration
    # (its story, credits, and non-affiliation notice). Untracked files are
    # checked before they are added.
    legacy = re.compile(
        r"shot[ _-]?stopper|tate[ _-]?mazer|acaia[ _-]?(arduino[ _-]?)?ble",
        re.IGNORECASE)
    listed = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=ROOT, check=True, capture_output=True).stdout.decode().split("\0")
    for relative in filter(None, listed):
        if legacy.search(relative):
            failures.append(f"legacy project name in path: {relative}")
        path = ROOT / relative
        content = path.read_bytes() if path.is_file() else b"\0"
        if b"\0" in content or relative in ("scripts/check_architecture.py", "README.md"):
            continue
        lines = content.decode("utf-8", errors="replace").splitlines()
        for number, line in enumerate(lines, 1):
            if legacy.search(line):
                failures.append(f"legacy project name: {relative}:{number}")

    if failures:
        for failure in failures:
            print(f"architecture error: {failure}", file=sys.stderr)
        return 1
    print(
        "architecture boundaries: "
        "Safety/Control/Scale/Machine/Network/Persistence/Diagnostics OK"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

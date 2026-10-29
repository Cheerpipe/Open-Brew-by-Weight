#!/usr/bin/env python3
"""Validate build profiles and emit one resolved manifest/header."""

from __future__ import annotations

import argparse
import copy
import json
import re
import shlex
import sys
from pathlib import Path
from typing import Any


ID_RE = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")
LEVELS = {"low": 0, "high": 1}
PULLS = {"none", "up", "down"}
TARGETS = {("esp32s3", "n16r8"): "n16r8"}
ROOT = Path(__file__).resolve().parent.parent


class ProfileError(ValueError):
    pass


def fail(message: str) -> None:
    raise ProfileError(message)


def exact(obj: Any, required: set[str], where: str,
          optional: set[str] = frozenset()) -> dict[str, Any]:
    if not isinstance(obj, dict):
        fail(f"{where} must be an object")
    missing = sorted(required - obj.keys())
    unknown = sorted(obj.keys() - required - optional)
    if missing:
        fail(f"{where} is missing: {', '.join(missing)}")
    if unknown:
        fail(f"{where} has unknown fields: {', '.join(unknown)}")
    return obj


def integer(value: Any, low: int, high: int, where: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        fail(f"{where} must be an integer from {low} to {high}")
    return value


def zero_or_integer(value: Any, low: int, high: int, where: str) -> int:
    if not isinstance(value, bool) and value == 0:
        return 0
    return integer(value, low, high, where)


def boolean(value: Any, where: str) -> bool:
    if not isinstance(value, bool):
        fail(f"{where} must be true or false")
    return value


def choice(value: Any, values: set[str], where: str) -> str:
    if not isinstance(value, str) or value not in values:
        fail(f"{where} must be one of: {', '.join(sorted(values))}")
    return value


def profile_text(value: Any, where: str, *, identifier: bool = False) -> str:
    if not isinstance(value, str) or not value or len(value) > 63:
        fail(f"{where} must be a non-empty string of at most 63 characters")
    if identifier:
        if not ID_RE.fullmatch(value):
            fail(f"{where} must be a lowercase hyphenated identifier")
    elif any(ord(c) < 32 or c in {'"', '\\'} for c in value):
        fail(f"{where} contains unsupported characters")
    return value


def gpio(value: Any, where: str) -> int:
    return integer(value, 0, 48, where)


def optional_role(obj: Any, required_when_present: set[str], where: str) -> dict[str, Any]:
    if not isinstance(obj, dict) or "present" not in obj:
        fail(f"{where} must explicitly declare present")
    present = boolean(obj["present"], f"{where}.present")
    expected = {"present"} | required_when_present if present else {"present"}
    return exact(obj, expected, where)


def load(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        fail(f"cannot read {path}: {error}")
    if not isinstance(value, dict):
        fail(f"{path} must contain one JSON object")
    return value


def profile_path(kind: str, selector: str) -> tuple[Path, bool]:
    if "/" not in selector and "\\" not in selector and not selector.endswith(".json"):
        return ROOT / "config" / kind / f"{selector}.json", True
    return Path(selector).expanduser(), False


def selected_profile(kind: str, selector: str) -> tuple[Path, dict[str, Any]]:
    path, built_in = profile_path(kind, selector)
    profile = load(path)
    validator = validate_hardware if kind == "hardware" else validate_machine
    profile = validator(profile)
    if built_in and profile["id"] != selector:
        fail(f"{path} declares id {profile['id']!r}, expected {selector!r}")
    return path, profile


def list_profiles() -> None:
    hardware_profiles = []
    for path in sorted((ROOT / "config/hardware").glob("*.json")):
        profile = validate_hardware(load(path))
        if profile["id"] != path.stem:
            fail(f"{path} declares id {profile['id']!r}, expected {path.stem!r}")
        hardware_profiles.append(profile)
    machine_profiles = []
    for path in sorted((ROOT / "config/machines").glob("*.json")):
        profile = validate_machine(load(path))
        if profile["id"] != path.stem:
            fail(f"{path} declares id {profile['id']!r}, expected {path.stem!r}")
        machine_profiles.append(profile)
    print("Hardware profiles:")
    for profile in hardware_profiles:
        target = TARGETS[(profile["target"]["chip"],
                          profile["target"]["memory_profile"])]
        print(f"  {profile['id']}  target={target} "
              f"speaker={'yes' if profile['speaker']['present'] else 'no'} "
              f"reed={'yes' if profile['reed']['present'] else 'no'}")
    print("Machine profiles:")
    for profile in machine_profiles:
        compatible = [hardware["id"] for hardware in hardware_profiles
                      if hardware["id"] in profile.get("hardware", [hardware["id"]]) and
                      (profile["interface"]["feedback"] != "reed" or
                       hardware["reed"]["present"])]
        interface = profile["interface"]
        print(f"  {profile['id']}  {profile['brand']} {profile['model']} "
              f"control={interface['control']} feedback={interface['feedback']}")
        print(f"    compatible hardware: {', '.join(compatible) or '(none)'}")


def common_profile(obj: dict[str, Any], where: str) -> None:
    integer(obj["schema_version"], 1, 1, f"{where}.schema_version")
    profile_text(obj["id"], f"{where}.id", identifier=True)
    profile_text(obj["display_name"], f"{where}.display_name")
    integer(obj["compatibility_revision"], 1, 65535,
            f"{where}.compatibility_revision")


def validate_hardware(obj: dict[str, Any]) -> dict[str, Any]:
    where = "hardware"
    exact(obj, {"schema_version", "id", "display_name", "compatibility_revision",
                "metadata", "target", "activator", "relay", "scale_status_led", "speaker",
                "usb_console_jumper", "reed", "external_safety"}, where)
    common_profile(obj, where)
    if not isinstance(obj["metadata"], dict):
        fail("hardware.metadata must be an object")
    target = exact(obj["target"], {"chip", "memory_profile"}, "hardware.target")
    target_key = (choice(target["chip"], {"esp32s3"}, "hardware.target.chip"),
                  choice(target["memory_profile"], {"n16r8"},
                         "hardware.target.memory_profile"))
    if target_key not in TARGETS:
        fail("unsupported hardware target")

    activator = exact(obj["activator"], {"gpio", "active_level", "pull", "debounce_ms"},
                      "hardware.activator")
    gpio(activator["gpio"], "hardware.activator.gpio")
    choice(activator["active_level"], set(LEVELS), "hardware.activator.active_level")
    choice(activator["pull"], PULLS, "hardware.activator.pull")
    integer(activator["debounce_ms"], 1, 99, "hardware.activator.debounce_ms")
    if activator["active_level"] != "low" or activator["pull"] != "up":
        fail("the current activator driver requires active_level=low and pull=up")

    relay = exact(obj["relay"], {"gpio", "contact_type", "closed_level", "open_level"},
                  "hardware.relay")
    gpio(relay["gpio"], "hardware.relay.gpio")
    choice(relay["contact_type"], {"normally_open"}, "hardware.relay.contact_type")
    choice(relay["closed_level"], set(LEVELS), "hardware.relay.closed_level")
    choice(relay["open_level"], set(LEVELS), "hardware.relay.open_level")
    if relay["closed_level"] == relay["open_level"]:
        fail("hardware.relay closed_level and open_level must differ")

    led = optional_role(obj["scale_status_led"], {"gpio", "active_level"},
                        "hardware.scale_status_led")
    speaker = optional_role(obj["speaker"], {"type", "gpio"}, "hardware.speaker")
    usb = optional_role(obj["usb_console_jumper"],
                        {"gpio", "active_level", "pull", "sample_at"},
                        "hardware.usb_console_jumper")
    reed = optional_role(obj["reed"], {"gpio", "active_level", "pull", "debounce_ms"},
                         "hardware.reed")
    safety = optional_role(obj["external_safety"],
                           {"heartbeat_gpio", "heartbeat_idle_level", "feedback_gpio",
                            "feedback_closed_level", "feedback_pull", "heartbeat_period_ms",
                            "feedback_settle_ms"}, "hardware.external_safety")

    pins = [("activator", activator["gpio"]), ("relay", relay["gpio"])]
    if led["present"]:
        gpio(led["gpio"], "hardware.scale_status_led.gpio")
        choice(led["active_level"], set(LEVELS), "hardware.scale_status_led.active_level")
        pins.append(("scale_status_led", led["gpio"]))
    if speaker["present"]:
        choice(speaker["type"], {"passive_pwm"}, "hardware.speaker.type")
        gpio(speaker["gpio"], "hardware.speaker.gpio")
        pins.append(("speaker", speaker["gpio"]))
    if usb["present"]:
        gpio(usb["gpio"], "hardware.usb_console_jumper.gpio")
        choice(usb["active_level"], set(LEVELS), "hardware.usb_console_jumper.active_level")
        choice(usb["pull"], PULLS, "hardware.usb_console_jumper.pull")
        choice(usb["sample_at"], {"boot"}, "hardware.usb_console_jumper.sample_at")
        if usb["gpio"] in {0, 45, 46}:
            fail("hardware.usb_console_jumper.gpio cannot use a boot/strapping pin")
        if usb["active_level"] != "low" or usb["pull"] != "up":
            fail("the current USB jumper driver requires active_level=low and pull=up")
        pins.append(("usb_console_jumper", usb["gpio"]))
    if reed["present"]:
        gpio(reed["gpio"], "hardware.reed.gpio")
        choice(reed["active_level"], set(LEVELS), "hardware.reed.active_level")
        choice(reed["pull"], PULLS, "hardware.reed.pull")
        integer(reed["debounce_ms"], 1, 99, "hardware.reed.debounce_ms")
        if reed["active_level"] != "low" or reed["pull"] != "up":
            fail("the current reed driver requires active_level=low and pull=up")
        if reed["debounce_ms"] != activator["debounce_ms"]:
            fail("hardware.reed.debounce_ms must match hardware.activator.debounce_ms")
        pins.append(("reed", reed["gpio"]))
    if safety["present"]:
        for name in ("heartbeat_gpio", "feedback_gpio"):
            gpio(safety[name], f"hardware.external_safety.{name}")
            pins.append((f"external_safety.{name}", safety[name]))
        choice(safety["heartbeat_idle_level"], set(LEVELS),
               "hardware.external_safety.heartbeat_idle_level")
        choice(safety["feedback_closed_level"], set(LEVELS),
               "hardware.external_safety.feedback_closed_level")
        choice(safety["feedback_pull"], PULLS, "hardware.external_safety.feedback_pull")
        if safety["heartbeat_idle_level"] != "low" or safety["feedback_pull"] != "up":
            fail("the current external-safety driver requires heartbeat_idle_level=low and feedback_pull=up")
        integer(safety["heartbeat_period_ms"], 10, 1000,
                "hardware.external_safety.heartbeat_period_ms")
        integer(safety["feedback_settle_ms"], 1, 1000,
                "hardware.external_safety.feedback_settle_ms")
    by_gpio: dict[int, str] = {}
    for role, pin in pins:
        if pin in by_gpio:
            fail(f"GPIO {pin} is shared by {by_gpio[pin]} and {role}")
        by_gpio[pin] = role
    return obj


def validate_machine(obj: dict[str, Any]) -> dict[str, Any]:
    where = "machine"
    exact(obj, {"schema_version", "id", "display_name", "compatibility_revision",
                "hardware", "brand", "model", "integration_revision", "integration",
                "interface", "factory_defaults"}, where,
          optional={"machine_icon"})
    common_profile(obj, where)
    if "machine_icon" in obj:
        # Names a glyph in gen_web_ui.js's catalog; absent means the default
        # generic machine icon, so existing profiles stay valid unchanged.
        profile_text(obj["machine_icon"], "machine.machine_icon", identifier=True)
    profile_text(obj["brand"], "machine.brand")
    profile_text(obj["model"], "machine.model")
    profile_text(obj["integration_revision"], "machine.integration_revision",
                 identifier=True)
    integration = choice(obj["integration"], {"none", "linea_micra_cloud"},
                         "machine.integration")
    interface = exact(obj["interface"], {"control", "feedback"}, "machine.interface")
    control = choice(interface["control"], {"paddle", "momentary"},
                     "machine.interface.control")
    feedback = choice(interface["feedback"], {"none", "reed"},
                      "machine.interface.feedback")
    if control == "paddle" and feedback != "none":
        fail("paddle control does not support reed feedback")
    if integration == "linea_micra_cloud" and (control, feedback) != ("paddle", "none"):
        fail("linea_micra_cloud requires paddle control without feedback")
    if not isinstance(obj["hardware"], list) or not obj["hardware"] or \
            any(not isinstance(entry, str) for entry in obj["hardware"]):
        fail("machine.hardware must be a non-empty list of hardware profile ids")
    for entry in obj["hardware"]:
        profile_text(entry, "machine.hardware entry", identifier=True)

    defaults = obj["factory_defaults"]
    specific = "paddle" if control == "paddle" else "momentary"
    exact(defaults, {"operational_wall_ms", specific, "quick_rinse"},
          "machine.factory_defaults")
    integer(defaults["operational_wall_ms"], 5000, 60000,
            "machine.factory_defaults.operational_wall_ms")
    rinse = exact(defaults["quick_rinse"], {"enabled", "gesture_ms", "duration_ms"},
                  "machine.factory_defaults.quick_rinse")
    boolean(rinse["enabled"], "machine.factory_defaults.quick_rinse.enabled")
    integer(rinse["gesture_ms"], 100, 5000,
            "machine.factory_defaults.quick_rinse.gesture_ms")
    integer(rinse["duration_ms"], 500, min(10000, defaults["operational_wall_ms"]),
            "machine.factory_defaults.quick_rinse.duration_ms")
    if control == "paddle":
        paddle = exact(defaults["paddle"], {"mode", "return_reminder"},
                       "machine.factory_defaults.paddle")
        choice(paddle["mode"], {"natural", "original", "auto"},
               "machine.factory_defaults.paddle.mode")
        reminder = exact(paddle["return_reminder"],
                         {"enabled", "interval_ms", "max_duration_ms"},
                         "machine.factory_defaults.paddle.return_reminder")
        boolean(reminder["enabled"],
                "machine.factory_defaults.paddle.return_reminder.enabled")
        integer(reminder["interval_ms"], 5000, 60000,
                "machine.factory_defaults.paddle.return_reminder.interval_ms")
        integer(reminder["max_duration_ms"],
                max(60000, reminder["interval_ms"]), 3600000,
                "machine.factory_defaults.paddle.return_reminder.max_duration_ms")
    else:
        momentary_fields = {"start_edge", "stop_pulse_ms", "max_single_press_ms",
                            "assume_idle_on_scale_connect", "shot_reaction_timeout_s"}
        if feedback == "reed":
            momentary_fields.add("reed_confirm_timeout_ms")
        momentary = exact(defaults["momentary"], momentary_fields,
                          "machine.factory_defaults.momentary")
        choice(momentary["start_edge"], {"press", "release"},
               "machine.factory_defaults.momentary.start_edge")
        integer(momentary["stop_pulse_ms"], 50, 1000,
                "machine.factory_defaults.momentary.stop_pulse_ms")
        integer(momentary["max_single_press_ms"], 100, 5000,
                "machine.factory_defaults.momentary.max_single_press_ms")
        boolean(momentary["assume_idle_on_scale_connect"],
                "machine.factory_defaults.momentary.assume_idle_on_scale_connect")
        zero_or_integer(momentary["shot_reaction_timeout_s"], 3, 30,
                        "machine.factory_defaults.momentary.shot_reaction_timeout_s")
        if feedback == "reed":
            integer(momentary["reed_confirm_timeout_ms"], 200, 5000,
                    "machine.factory_defaults.momentary.reed_confirm_timeout_ms")
    return obj


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def define(lines: list[str], name: str, value: Any) -> None:
    if isinstance(value, bool):
        rendered = "1" if value else "0"
    elif isinstance(value, str):
        rendered = cpp_string(value)
    else:
        rendered = str(value)
    lines += [f"#ifndef {name}", f"#define {name} {rendered}", "#endif"]


def parse_defines(flags: str) -> dict[str, str]:
    result: dict[str, str] = {}
    try:
        tokens = shlex.split(flags)
    except ValueError as error:
        fail(f"invalid --flags: {error}")
    for token in tokens:
        if token.startswith("-D"):
            body = token[2:]
            name, separator, value = body.partition("=")
            if name.startswith("OPEN_BREW_BY_WEIGHT_"):
                result[name] = value if separator else "1"
    return result


def define_int(definitions: dict[str, str], name: str, low: int, high: int) -> int | None:
    if name not in definitions:
        return None
    try:
        value = int(definitions[name], 0)
    except ValueError:
        fail(f"{name} override must be an integer")
    return integer(value, low, high, name)


def apply_overrides(hardware: dict[str, Any], machine: dict[str, Any],
                    definitions: dict[str, str]) -> None:
    forbidden = {"OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID",
                 "OPEN_BREW_BY_WEIGHT_HARDWARE_COMPATIBILITY_REVISION",
                 "OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID",
                 "OPEN_BREW_BY_WEIGHT_MACHINE_COMPATIBILITY_REVISION",
                 "OPEN_BREW_BY_WEIGHT_MACHINE_BRAND", "OPEN_BREW_BY_WEIGHT_MACHINE_MODEL"}
    selected = sorted(forbidden & definitions.keys())
    if selected:
        fail(f"profile identity cannot be overridden with --flags: {', '.join(selected)}")
    presence = {"OPEN_BREW_BY_WEIGHT_SCALE_STATUS_LED_PRESENT": "scale_status_led",
                "OPEN_BREW_BY_WEIGHT_SPEAKER_PRESENT": "speaker",
                "OPEN_BREW_BY_WEIGHT_USB_CONSOLE_JUMPER_PRESENT": "usb_console_jumper",
                "OPEN_BREW_BY_WEIGHT_REED_PRESENT": "reed",
                "OPEN_BREW_BY_WEIGHT_EXTERNAL_SAFETY_PRESENT": "external_safety"}
    for macro, role in presence.items():
        value = define_int(definitions, macro, 0, 1)
        if value is not None and bool(value) != hardware[role]["present"]:
            fail(f"{macro} cannot change physical presence declared by the profile")
    gpio_overrides = {
        "OPEN_BREW_BY_WEIGHT_ACTIVATOR_GPIO": ("activator", "gpio"),
        "OPEN_BREW_BY_WEIGHT_RELAY_GPIO": ("relay", "gpio"),
        "OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_GPIO": ("scale_status_led", "gpio"),
        "OPEN_BREW_BY_WEIGHT_BUZZER_GPIO": ("speaker", "gpio"),
        "OPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO": ("usb_console_jumper", "gpio"),
        "OPEN_BREW_BY_WEIGHT_REED_GPIO": ("reed", "gpio"),
        "OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO": ("external_safety", "heartbeat_gpio"),
        "OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_GPIO": ("external_safety", "feedback_gpio"),
    }
    for macro, (role, field) in gpio_overrides.items():
        value = define_int(definitions, macro, 0, 48)
        if value is not None:
            if role not in {"activator", "relay"} and not hardware[role]["present"]:
                fail(f"{macro} cannot configure absent {role} hardware")
            hardware[role][field] = value
    level_overrides = {
        "OPEN_BREW_BY_WEIGHT_ACTIVATOR_ACTIVE_LEVEL": ("activator", "active_level"),
        "OPEN_BREW_BY_WEIGHT_RELAY_CLOSED_LEVEL": ("relay", "closed_level"),
        "OPEN_BREW_BY_WEIGHT_RELAY_OPEN_LEVEL": ("relay", "open_level"),
        "OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_ACTIVE_LEVEL": ("scale_status_led", "active_level"),
        "OPEN_BREW_BY_WEIGHT_USB_CONSOLE_ACTIVE_LEVEL": ("usb_console_jumper", "active_level"),
        "OPEN_BREW_BY_WEIGHT_REED_ACTIVE_LEVEL": ("reed", "active_level"),
        "OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_CLOSED_LEVEL":
            ("external_safety", "feedback_closed_level"),
    }
    for macro, (role, field) in level_overrides.items():
        value = define_int(definitions, macro, 0, 1)
        if value is not None:
            if role not in {"activator", "relay"} and not hardware[role]["present"]:
                fail(f"{macro} cannot configure absent {role} hardware")
            hardware[role][field] = "high" if value else "low"
    timing_overrides = {
        "OPEN_BREW_BY_WEIGHT_ACTIVATOR_DEBOUNCE_MS": ("activator", "debounce_ms", 1, 99),
        "OPEN_BREW_BY_WEIGHT_REED_DEBOUNCE_MS": ("reed", "debounce_ms", 1, 99),
        "OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_TOGGLE_MS":
            ("external_safety", "heartbeat_period_ms", 10, 1000),
        "OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_SETTLE_MS":
            ("external_safety", "feedback_settle_ms", 1, 1000),
    }
    for macro, (role, field, low, high) in timing_overrides.items():
        value = define_int(definitions, macro, low, high)
        if value is not None:
            if role != "activator" and not hardware[role]["present"]:
                fail(f"{macro} cannot configure absent {role} hardware")
            hardware[role][field] = value

    defaults = machine["factory_defaults"]
    scalar_defaults = {
        "OPEN_BREW_BY_WEIGHT_DEFAULT_OPERATIONAL_WALL_MS":
            (defaults, "operational_wall_ms", 5000, 60000),
        "OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_GESTURE_MS":
            (defaults["quick_rinse"], "gesture_ms", 100, 5000),
        "OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_DURATION_MS":
            (defaults["quick_rinse"], "duration_ms", 500, 10000),
    }
    if machine["interface"]["control"] == "paddle":
        paddle = defaults["paddle"]
        reminder = paddle["return_reminder"]
        scalar_defaults |= {
            "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_MODE": (paddle, "mode", 0, 2),
            "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_INTERVAL_MS":
                (reminder, "interval_ms", 5000, 120000),
            "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_MAX_MS":
                (reminder, "max_duration_ms", 60000, 3600000),
        }
    else:
        momentary = defaults["momentary"]
        scalar_defaults |= {
            "OPEN_BREW_BY_WEIGHT_STOP_PULSE_MS": (momentary, "stop_pulse_ms", 50, 1000),
            "OPEN_BREW_BY_WEIGHT_MAX_SINGLE_PRESS_MS":
                (momentary, "max_single_press_ms", 100, 5000),
        }
        if machine["interface"]["feedback"] == "reed":
            scalar_defaults["OPEN_BREW_BY_WEIGHT_REED_CONFIRM_TIMEOUT_MS"] = (
                momentary, "reed_confirm_timeout_ms", 200, 5000)
    for macro, (owner, field, low, high) in scalar_defaults.items():
        value = define_int(definitions, macro, low, high)
        if value is not None:
            if macro == "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_MODE":
                value = {0: "natural", 1: "original", 2: "auto"}[value]
            owner[field] = value
    if machine["interface"]["control"] == "momentary":
        value = definitions.get("OPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S")
        if value is not None:
            try:
                parsed = int(value, 0)
            except ValueError:
                fail("OPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S override must be an integer")
            defaults["momentary"]["shot_reaction_timeout_s"] = zero_or_integer(
                parsed, 3, 30, "OPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S")
    bool_defaults = {
        "OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_ENABLED": (defaults["quick_rinse"], "enabled"),
    }
    if machine["interface"]["control"] == "paddle":
        bool_defaults["OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_REMINDER"] = (
            defaults["paddle"]["return_reminder"], "enabled")
    else:
        momentary = defaults["momentary"]
        bool_defaults |= {
            "OPEN_BREW_BY_WEIGHT_DEFAULT_MOMENTARY_START_ON_PRESS":
                (momentary, "start_edge"),
            "OPEN_BREW_BY_WEIGHT_DEFAULT_ASSUME_IDLE_ON_SCALE_CONNECT":
                (momentary, "assume_idle_on_scale_connect"),
        }
    for macro, (owner, field) in bool_defaults.items():
        value = define_int(definitions, macro, 0, 1)
        if value is not None:
            owner[field] = ("press" if value else "release") if field == "start_edge" else bool(value)


def resolve(hardware: dict[str, Any], machine: dict[str, Any], flags: str) -> dict[str, Any]:
    hardware = copy.deepcopy(hardware)
    machine = copy.deepcopy(machine)
    definitions = parse_defines(flags)
    apply_overrides(hardware, machine, definitions)
    validate_hardware(hardware)
    validate_machine(machine)
    if hardware["id"] not in machine.get("hardware", [hardware["id"]]):
        fail(f"hardware profile {hardware['id']!r} is not compatible with machine profile "
             f"{machine['id']!r}; declare it in the machine profile hardware list")
    if machine["interface"]["feedback"] == "reed" and not hardware["reed"]["present"]:
        fail("machine requires reed feedback but the hardware profile declares reed absent")
    if definitions.get("OPEN_BREW_BY_WEIGHT_DEVELOPMENT") not in (None, "0", "1"):
        fail("OPEN_BREW_BY_WEIGHT_DEVELOPMENT must be 0 or 1")
    if "OPEN_BREW_BY_WEIGHT_MACHINE_TYPE" in definitions:
        expected = {("paddle", "none"): "0", ("momentary", "none"): "1",
                    ("momentary", "reed"): "2"}[(machine["interface"]["control"],
                                                   machine["interface"]["feedback"])]
        if definitions["OPEN_BREW_BY_WEIGHT_MACHINE_TYPE"] != expected:
            fail("OPEN_BREW_BY_WEIGHT_MACHINE_TYPE override conflicts with the machine profile")
    if definitions.get("OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER") == "1" and not hardware["speaker"]["present"]:
        fail("buzzer support cannot be enabled when the hardware speaker is absent")
    return {
        "schema_version": 1,
        "variant": f"{hardware['id']}--{machine['id']}",
        "arch": TARGETS[(hardware["target"]["chip"], hardware["target"]["memory_profile"])],
        "machine_integration": machine["integration"],
        "hardware": hardware,
        "machine": machine,
    }


def header_for(resolved: dict[str, Any]) -> str:
    hardware = resolved["hardware"]
    machine = resolved["machine"]
    defaults = machine["factory_defaults"]
    lines = ["#pragma once", "", "// Generated by scripts/resolve_build_profiles.py — do not edit.", ""]
    define(lines, "OPEN_BREW_BY_WEIGHT_PROFILE_MODE", 1)
    define(lines, "OPEN_BREW_BY_WEIGHT_HARDWARE_PROFILE_ID", hardware["id"])
    define(lines, "OPEN_BREW_BY_WEIGHT_HARDWARE_COMPATIBILITY_REVISION", hardware["compatibility_revision"])
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_PROFILE_ID", machine["id"])
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_BRAND", machine["brand"])
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_MODEL", machine["model"])
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_COMPATIBILITY_REVISION", machine["compatibility_revision"])
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_NONE", 0)
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION_LINEA_MICRA_CLOUD", 1)
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_INTEGRATION",
           {"none": 0, "linea_micra_cloud": 1}[resolved["machine_integration"]])
    # Web UI metadata, consumed by the build's gen_web_ui.js step; the firmware
    # itself never reads it.
    if "machine_icon" in machine:
        define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_WEB_ICON", machine["machine_icon"])
    define(lines, "OPEN_BREW_BY_WEIGHT_ACTIVATOR_GPIO", hardware["activator"]["gpio"])
    define(lines, "OPEN_BREW_BY_WEIGHT_ACTIVATOR_ACTIVE_LEVEL", LEVELS[hardware["activator"]["active_level"]])
    define(lines, "OPEN_BREW_BY_WEIGHT_ACTIVATOR_DEBOUNCE_MS", hardware["activator"]["debounce_ms"])
    define(lines, "OPEN_BREW_BY_WEIGHT_RELAY_GPIO", hardware["relay"]["gpio"])
    define(lines, "OPEN_BREW_BY_WEIGHT_RELAY_CLOSED_LEVEL", LEVELS[hardware["relay"]["closed_level"]])
    define(lines, "OPEN_BREW_BY_WEIGHT_RELAY_OPEN_LEVEL", LEVELS[hardware["relay"]["open_level"]])
    for json_name, macro_name in (("scale_status_led", "SCALE_STATUS_LED"),
                                  ("speaker", "SPEAKER"),
                                  ("usb_console_jumper", "USB_CONSOLE_JUMPER"),
                                  ("reed", "REED"),
                                  ("external_safety", "EXTERNAL_SAFETY")):
        role = hardware[json_name]
        define(lines, f"OPEN_BREW_BY_WEIGHT_{macro_name}_PRESENT", role["present"])
    if hardware["scale_status_led"]["present"]:
        define(lines, "OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_GPIO", hardware["scale_status_led"]["gpio"])
        define(lines, "OPEN_BREW_BY_WEIGHT_SCALE_CONNECTED_LED_ACTIVE_LEVEL",
               LEVELS[hardware["scale_status_led"]["active_level"]])
    # Buzzer support follows the physical speaker; the #ifndef guard lets an
    # explicit -DOPEN_BREW_BY_WEIGHT_ENABLE_BUZZER=… on the command line win.
    define(lines, "OPEN_BREW_BY_WEIGHT_ENABLE_BUZZER", hardware["speaker"]["present"])
    if hardware["speaker"]["present"]:
        define(lines, "OPEN_BREW_BY_WEIGHT_BUZZER_GPIO", hardware["speaker"]["gpio"])
    if hardware["usb_console_jumper"]["present"]:
        define(lines, "OPEN_BREW_BY_WEIGHT_USB_CONSOLE_GPIO", hardware["usb_console_jumper"]["gpio"])
        define(lines, "OPEN_BREW_BY_WEIGHT_USB_CONSOLE_ACTIVE_LEVEL",
               LEVELS[hardware["usb_console_jumper"]["active_level"]])
    if hardware["reed"]["present"]:
        define(lines, "OPEN_BREW_BY_WEIGHT_REED_GPIO", hardware["reed"]["gpio"])
        define(lines, "OPEN_BREW_BY_WEIGHT_REED_ACTIVE_LEVEL", LEVELS[hardware["reed"]["active_level"]])
        define(lines, "OPEN_BREW_BY_WEIGHT_REED_DEBOUNCE_MS", hardware["reed"]["debounce_ms"])
    if hardware["external_safety"]["present"]:
        safety = hardware["external_safety"]
        define(lines, "OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_GPIO", safety["heartbeat_gpio"])
        define(lines, "OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_IDLE_LEVEL", LEVELS[safety["heartbeat_idle_level"]])
        define(lines, "OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_GPIO", safety["feedback_gpio"])
        define(lines, "OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_CLOSED_LEVEL",
               LEVELS[safety["feedback_closed_level"]])
        define(lines, "OPEN_BREW_BY_WEIGHT_SAFETY_HEARTBEAT_TOGGLE_MS", safety["heartbeat_period_ms"])
        define(lines, "OPEN_BREW_BY_WEIGHT_CIRCUIT_FEEDBACK_SETTLE_MS", safety["feedback_settle_ms"])
    machine_type = {("paddle", "none"): 0, ("momentary", "none"): 1,
                    ("momentary", "reed"): 2}[(machine["interface"]["control"],
                                                machine["interface"]["feedback"])]
    define(lines, "OPEN_BREW_BY_WEIGHT_MACHINE_TYPE", machine_type)
    define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_OPERATIONAL_WALL_MS", defaults["operational_wall_ms"])
    define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_ENABLED", defaults["quick_rinse"]["enabled"])
    define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_GESTURE_MS", defaults["quick_rinse"]["gesture_ms"])
    define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_RINSE_DURATION_MS", defaults["quick_rinse"]["duration_ms"])
    if machine_type == 0:
        paddle = defaults["paddle"]
        reminder = paddle["return_reminder"]
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_MODE",
               {"natural": 0, "original": 1, "auto": 2}[paddle["mode"]])
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_REMINDER", reminder["enabled"])
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_INTERVAL_MS", reminder["interval_ms"])
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_PADDLE_RETURN_MAX_MS", reminder["max_duration_ms"])
    else:
        momentary = defaults["momentary"]
        define(lines, "OPEN_BREW_BY_WEIGHT_STOP_PULSE_MS", momentary["stop_pulse_ms"])
        define(lines, "OPEN_BREW_BY_WEIGHT_MAX_SINGLE_PRESS_MS", momentary["max_single_press_ms"])
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_MOMENTARY_START_ON_PRESS",
               momentary["start_edge"] == "press")
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_ASSUME_IDLE_ON_SCALE_CONNECT",
               momentary["assume_idle_on_scale_connect"])
        define(lines, "OPEN_BREW_BY_WEIGHT_DEFAULT_SHOT_REACTION_TIMEOUT_S",
               momentary["shot_reaction_timeout_s"])
        if machine_type == 2:
            define(lines, "OPEN_BREW_BY_WEIGHT_REED_CONFIRM_TIMEOUT_MS",
                   momentary["reed_confirm_timeout_ms"])
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--hardware")
    parser.add_argument("--machine")
    parser.add_argument("--flags", default="")
    parser.add_argument("--output-root", type=Path)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    try:
        if args.list:
            if args.hardware or args.machine or args.output_root or args.check_only:
                fail("--list cannot be combined with profile or output options")
            list_profiles()
            return 0
        if not args.hardware or not args.machine or not args.output_root:
            fail("--hardware, --machine, and --output-root are required")
        _, hardware = selected_profile("hardware", args.hardware)
        _, machine = selected_profile("machines", args.machine)
        resolved = resolve(hardware, machine, args.flags)
        output_dir = args.output_root / resolved["variant"] / "generated"
        if not args.check_only:
            output_dir.mkdir(parents=True, exist_ok=True)
            # Rewrite only on change so unchanged builds do not recompile includers.
            for path, text in (
                    (output_dir / "OpenBrewByWeightBuildProfileGenerated.h", header_for(resolved)),
                    (output_dir / "build-profile.json",
                     json.dumps(resolved, indent=2, ensure_ascii=False) + "\n")):
                if not path.is_file() or path.read_text(encoding="utf-8") != text:
                    path.write_text(text, encoding="utf-8")
    except ProfileError as error:
        print(f"Profile error: {error}", file=sys.stderr)
        return 2
    print(f"arch={resolved['arch']}")
    print(f"variant={resolved['variant']}")
    print(f"hardware_compat={resolved['hardware']['id']}-r{resolved['hardware']['compatibility_revision']}")
    print(f"machine_compat={resolved['machine']['id']}-r{resolved['machine']['compatibility_revision']}")
    print(f"machine_integration={resolved['machine_integration']}")
    print(f"generated_dir={output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

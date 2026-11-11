#!/usr/bin/env python3
"""Fail unless every project production C++ TU is in the build database."""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "scripts" / "project-translation-units.txt"
KNOWN_INTEGRATIONS = {"none", "la_marzocco_cloud"}
SOURCE_ROOTS = (
    ROOT / "src",
    ROOT / "libraries" / "EspressoScaleBLE" / "src",
    ROOT / "idf" / "main",
    ROOT / "idf" / "components",
)


def relative_project_path(path: pathlib.Path) -> str | None:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return None


def manifest_paths(path: pathlib.Path) -> dict[str, str | None]:
    entries: dict[str, str | None] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        entry = raw.strip()
        if not entry or entry.startswith("#"):
            continue
        relative, separator, selector = entry.partition("|")
        relative = relative.strip()
        integration = None
        if separator:
            key, equals, value = selector.strip().partition("=")
            if key != "integration" or not equals or not value:
                raise ValueError(f"invalid manifest selector: {entry}")
            integration = value
            if integration not in KNOWN_INTEGRATIONS:
                raise ValueError(f"unknown integration selector: {integration}")
        if relative in entries:
            raise ValueError(f"duplicate manifest entry: {relative}")
        entries[relative] = integration
    return entries


def selected_integration(database: pathlib.Path) -> str:
    profile = database.resolve().parent / "generated" / "build-profile.json"
    value = json.loads(profile.read_text(encoding="utf-8"))
    integration = value.get("machine_integration")
    if integration not in KNOWN_INTEGRATIONS:
        raise ValueError(f"invalid machine_integration in {profile}")
    return integration


def disk_paths() -> set[str]:
    entries: set[str] = set()
    for source_root in SOURCE_ROOTS:
        for path in source_root.rglob("*.cpp"):
            relative = relative_project_path(path)
            if relative is None or "/tests/" in f"/{relative}/":
                continue
            entries.add(relative)
    return entries


def database_paths(path: pathlib.Path) -> collections.Counter[str]:
    database = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(database, list):
        raise ValueError("compile database root must be an array")
    entries: collections.Counter[str] = collections.Counter()
    for item in database:
        if not isinstance(item, dict) or not isinstance(item.get("file"), str):
            raise ValueError("compile database entry lacks a string file field")
        source = pathlib.Path(item["file"])
        if not source.is_absolute():
            directory = pathlib.Path(item.get("directory", ROOT))
            source = directory / source
        relative = relative_project_path(source)
        if relative is not None:
            entries[relative] += 1
    return entries


def print_set(label: str, entries: set[str]) -> None:
    if entries:
        print(f"{label}:", file=sys.stderr)
        for entry in sorted(entries):
            print(f"  {entry}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--database", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    args = parser.parse_args()

    try:
        declared = manifest_paths(args.manifest)
        integration = selected_integration(args.database)
        expected = {
            relative for relative, selector in declared.items()
            if selector is None or selector == integration
        }
        on_disk = disk_paths()
        database = database_paths(args.database)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"translation-unit audit failed: {error}", file=sys.stderr)
        return 2

    database_set = set(database)
    declared_paths = set(declared)
    disk_missing = declared_paths - on_disk
    disk_untracked = on_disk - declared_paths
    database_missing = expected - database_set
    database_untracked = (database_set & on_disk) - expected
    duplicates = {entry for entry, count in database.items() if count != 1 and entry in expected}

    print_set("manifest entries missing on disk", disk_missing)
    print_set("production TUs absent from manifest", disk_untracked)
    print_set("manifest entries missing from compile database", database_missing)
    print_set("unexpected project TUs in compile database", database_untracked)
    print_set("project TUs occurring other than once", duplicates)
    if any((disk_missing, disk_untracked, database_missing,
            database_untracked, duplicates)):
        return 1

    print(
        f"translation-unit coverage: {len(expected)}/{len(expected)} "
        f"production C++ files for integration={integration}, "
        "each compiled exactly once"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

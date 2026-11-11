#!/usr/bin/env python3
"""Report project firmware functions that no analysis build references.

Each build directory comes from scripts/dead-code-idf: project components are
compiled without inlining and with every inline function kept, and every
input of the application link is built, but the ELF is not linked. A function
is live when a relocation in a project object reaches it from another section
(calls, function pointers, vtables, static initializers), when a linker input
outside the project leaves its name undefined or defines it weak (an
override), when the link command or a linker script names it (-u, --wrap,
ENTRY, EXTERN), or when a linker script KEEPs its section. A function reached
only from its own section (recursion) is not live. Compiler-written bodies
(DWARF artificial or defaulted) are never candidates, and a constexpr function
that firmware sources name is live through constant evaluation. A function
defined in a project source and live in no analyzed build is dead.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import runpy
import shlex
import subprocess
import sys
import traceback
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from fnmatch import fnmatchcase
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parents[1]
PROJECT_SOURCES = tuple(f"{ROOT}/{path}/" for path in
                        ("src", "libraries/EspressoScaleBLE", "idf/main", "idf/components"))
# Unwind tables describe every function; they are metadata, not uses.
METADATA_SECTIONS = {".eh_frame"}

SECTION = re.compile(r"^\s*\[\s*(\d+)\]\s(\S*)\s+(\S+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+[0-9a-f]+"
                     r"\s+[0-9a-f]+\s+([A-Za-z]*)\s+\d+\s+(\d+)\s+\d+\s*$")
SYMBOL = re.compile(r"^\s*(\d+):\s+([0-9a-f]+)\s+(\d+|0x[0-9a-f]+)\s+(\S+)\s+(\S+)\s+\S+"
                    r"(?:\s+\[[^\]]*\])?\s+(\S+)\s?(\S*)\s*$")
RELOC_SECTION = re.compile(r"^Relocation section '[^']+' at offset 0x([0-9a-f]+)")
DIE = re.compile(r"^\s*<\d+><([0-9a-f]+)>: Abbrev Number: \d+ \((DW_TAG_\w+)\)")
ATTRIBUTE = re.compile(r"^\s*<([0-9a-f]+)>\s+(DW_AT_\w+)\s*: (?:\(.*\): )?(.*?)\s*$")
RELOC = re.compile(r"^([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s*(?:[0-9a-f]+\s+\S+\s*([+-])\s*([0-9a-f]+))?")


@dataclass
class Symbol:
    name: str
    kind: str
    bind: str
    section: int | None
    value: int
    size: int


@dataclass
class ObjectFile:
    source: str
    sections: dict[int, tuple[str, str]] = field(default_factory=dict)
    symbols: dict[int, Symbol] = field(default_factory=dict)
    relocations: list[tuple[int, int, int, int]] = field(default_factory=list)
    locations: dict[str, str] = field(default_factory=dict)
    artificial: set[int] = field(default_factory=set)


@dataclass
class LinkContext:
    undefined: set[str] = field(default_factory=set)
    weak: set[str] = field(default_factory=set)
    roots: set[str] = field(default_factory=set)
    wrapped: set[str] = field(default_factory=set)
    keep: set[str] = field(default_factory=set)


def parse_readelf(source: str, text: str) -> ObjectFile:
    """Parse `readelf -W -S -s -r` output of one relocatable object."""
    obj = ObjectFile(source)
    reloc_targets: dict[int, int] = {}
    target = None
    for line in text.splitlines():
        if match := SECTION.match(line):
            obj.sections[int(match[1])] = (match[2], match[5])
            if match[3] in {"RELA", "REL"}:
                reloc_targets[int(match[4], 16)] = int(match[6])
            continue
        if match := SYMBOL.match(line):
            ndx = match[6]
            obj.symbols[int(match[1])] = Symbol(
                match[7], match[4], match[5], int(ndx) if ndx.isdigit() else None,
                int(match[2], 16), int(match[3], 0))
            continue
        if match := RELOC_SECTION.match(line):
            target = reloc_targets[int(match[1], 16)]
            continue
        if line.startswith("Symbol table"):
            target = None
        if target is not None and (match := RELOC.match(line)):
            addend = int(match[4], 16) if match[4] else 0
            obj.relocations.append((target, int(match[1], 16), int(match[2], 16) >> 8,
                                    -addend if match[3] == "-" else addend))
    return obj


def parse_locations(obj: ObjectFile, text: str) -> None:
    """Attach `nm -l --defined-only` source locations to the object's symbols."""
    for line in text.splitlines():
        fields, _, location = line.partition("\t")
        parts = fields.split()
        if len(parts) == 3 and location:
            obj.locations.setdefault(parts[2], location.split(" (")[0].strip())


def parse_artificial(obj: ObjectFile, text: str) -> None:
    """Record where compiler-generated functions start, from `readelf --debug-dump=info`.

    Implicit constructors and lambda conversion operators are DW_AT_artificial
    and `= default` members DW_AT_defaulted: the compiler writes their bodies,
    often only for constant initialization. An out-of-line copy refers to that declaration through DW_AT_specification
    or DW_AT_abstract_origin. The .debug_info offset of each copy's
    DW_AT_low_pc carries the relocation that names its code.
    """
    dies: dict[int, dict[str, str]] = {}
    current: dict[str, str] | None = None
    for line in text.splitlines():
        if match := DIE.match(line):
            current = dies.setdefault(int(match[1], 16), {}) \
                if match[2] == "DW_TAG_subprogram" else None
        elif current is not None and (match := ATTRIBUTE.match(line)):
            current[match[2]] = match[3]
            if match[2] == "DW_AT_low_pc":
                current["offset"] = match[1]

    def artificial(die: dict[str, str] | None, depth: int = 0) -> bool:
        if die is None or depth > 8:
            return False
        if die.get("DW_AT_artificial") == "1" or die.get("DW_AT_defaulted", "0")[:1] in "12":
            return True
        origin = die.get("DW_AT_specification") or die.get("DW_AT_abstract_origin")
        return bool(origin) and artificial(dies.get(int(origin.strip("<>"), 16)), depth + 1)

    obj.artificial |= {int(die["offset"], 16) for die in dies.values()
                       if "offset" in die and artificial(die)}


def parse_nm(text: str, link: LinkContext) -> None:
    """Collect undefined and weak names from `nm -A -g -P` output of non-project inputs."""
    for line in text.splitlines():
        parts = line.rsplit(": ", 1)[-1].split()
        if len(parts) < 2:
            continue
        name, kind = parts[0], parts[1]
        if kind == "U":
            link.undefined.add(name)
        elif kind in {"W", "V"}:
            link.weak.add(name)


def base_name(name: str) -> str:
    # GCC names clones and split parts name.suffix or, on Xtensa, name$suffix
    # (.cold, $part$0, $constprop$0).
    return re.split(r"[.$]", name, maxsplit=1)[0]


def home(section: str) -> str:
    # Xtensa keeps each function's literal pool in a sibling section.
    if section.startswith(".literal"):
        return ".text" + section[len(".literal"):]
    return section.removesuffix(".literal")


def resolve(name: str, wrapped: set[str]) -> str:
    if name in wrapped:
        return "__wrap_" + name
    if name.startswith("__real_") and name[7:] in wrapped:
        return name[7:]
    return name


def analyze(objects: list[ObjectFile], link: LinkContext) -> tuple[dict, set]:
    """Return ({key: {(location, name)}} defined, {key} referenced) for one build."""
    defined: dict[tuple[str, str], set[tuple[str | None, str]]] = {}
    referenced: set[tuple[str, str]] = set()
    external = {resolve(name, link.wrapped) for name in link.undefined}
    external |= link.weak | link.roots
    groups: list[set[tuple[str, str]]] = []
    for obj in objects:
        functions: dict[int, list[Symbol]] = {}
        aliases: dict[tuple[int | None, int], set[tuple[str, str]]] = {}

        def key(symbol: Symbol) -> tuple[str, str]:
            name = base_name(symbol.name)
            return (obj.source, name) if symbol.bind == "LOCAL" else ("", name)

        code = [symbol for symbol in obj.symbols.values() if symbol.kind == "FUNC" and
                symbol.section is not None and "X" in obj.sections[symbol.section][1]]
        for symbol in code:
            functions.setdefault(symbol.section, []).append(symbol)

        def targets(index: int, addend: int) -> list[Symbol]:
            symbol = obj.symbols.get(index)
            if symbol is None or symbol.section is None:
                return []
            if symbol.kind == "SECTION":
                return [f for f in functions.get(symbol.section, [])
                        if f.value <= addend < f.value + max(f.size, 1)]
            return [symbol] if symbol.kind == "FUNC" else []

        generated = {(f.section, f.value) for target, offset, index, addend in obj.relocations
                     if obj.sections[target][0] == ".debug_info" and offset in obj.artificial
                     for f in targets(index, addend)}
        for symbol in code:
            section = obj.sections[symbol.section]
            aliases.setdefault((symbol.section, symbol.value), set()).add(key(symbol))
            if (symbol.section, symbol.value) not in generated:
                defined.setdefault(key(symbol), set()).add(
                    (obj.locations.get(symbol.name), symbol.name))
            if (symbol.bind != "LOCAL" and key(symbol)[1] in external) or any(
                    fnmatchcase(section[0], pattern) for pattern in link.keep):
                referenced.add(key(symbol))

        for target, _, index, addend in obj.relocations:
            name, flags = obj.sections.get(target, ("", ""))
            symbol = obj.symbols.get(index)
            if "A" not in flags or name in METADATA_SECTIONS or symbol is None:
                continue
            if symbol.section is None:
                referenced.add(("", base_name(resolve(symbol.name, link.wrapped))))
            for hit in targets(index, addend):
                if home(obj.sections[hit.section][0]) == home(name):
                    continue
                if hit.bind != "LOCAL" or base_name(hit.name) != hit.name:
                    referenced.add(("", base_name(resolve(hit.name, link.wrapped))))
                referenced |= aliases.get((hit.section, hit.value), {key(hit)})
        groups += [group for group in aliases.values() if len(group) > 1]
    # Symbols at one address (complete and base object constructors) are one
    # function, whichever object names it.
    for group in groups:
        if group & referenced:
            referenced |= group
    return defined, referenced


def dead_functions(builds: list[tuple[dict, set]]) -> list[tuple[str | None, str]]:
    """Functions defined in a project source and referenced in no build."""
    defined: dict = {}
    referenced: set = set()
    for build_defined, build_referenced in builds:
        for k, values in build_defined.items():
            defined.setdefault(k, set()).update(values)
        referenced |= build_referenced
    return sorted({value for k, values in defined.items() if k not in referenced
                   for value in values
                   if value[0] is None or value[0].startswith(PROJECT_SOURCES)},
                  key=lambda value: (value[0] or "", value[1]))


def unqualified(demangled: str) -> str:
    """The bare function name of a demangled signature."""
    depth, end = 0, len(demangled)
    for index in range(len(demangled) - 1, -1, -1):
        depth += {")": 1, "(": -1}.get(demangled[index], 0)
        if depth == 0 and demangled[index] == "(":
            end = index
            break
    name = demangled[:end]
    depth = 0
    while name.endswith(">") or depth:
        depth += {">": 1, "<": -1}.get(name[-1], 0)
        name = name[:-1]
    return re.split(r"::|\s", name)[-1]


def compile_time_used(location: str, demangled: str, sources: dict[str, str],
                      definitions: set[str]) -> bool:
    """True for a constexpr function that project code names outside a definition.

    The compiler evaluates such calls in constant expressions (static_assert,
    constexpr tables, constant arguments) and leaves no reference in the
    object, so the name in the sources is the only evidence of that use. Lines
    where any analyzed function is defined (`file:line`) are not uses.
    """
    path, _, number = location.rpartition(":")
    lines = sources.get(path, "").splitlines()
    index = int(number) - 1
    start = index
    while 0 < start <= len(lines) and lines[start - 1].strip() and not \
            lines[start - 1].rstrip().endswith((";", "{", "}", "*/")) and not \
            lines[start - 1].lstrip().startswith("//"):
        start -= 1
    name = unqualified(demangled)
    head = " ".join(lines[start:index + 1]).split(name)[0]
    if not re.search(r"\b(?:constexpr|consteval)\b", head):
        return False
    pattern = re.compile(rf"\b{re.escape(name)}\b")
    return any(pattern.search(line) and f"{file}:{number}" not in definitions
               for file, text in sources.items()
               for number, line in enumerate(text.splitlines(), 1))


def project_sources() -> dict[str, str]:
    """Firmware sources by absolute path; host tests do not count as use."""
    return {str(path): path.read_text(errors="replace")
            for directory in PROJECT_SOURCES for path in Path(directory).rglob("*")
            if path.suffix in {".h", ".hpp", ".c", ".cpp", ".inc"}
            and not str(path).startswith(f"{ROOT}/src/tests/")}


def fail(message: str) -> NoReturn:
    print(f"dead_code: {message}", file=sys.stderr)
    raise SystemExit(2)


def run(argv: list[str], stdin: str | None = None) -> str:
    result = subprocess.run(argv, input=stdin, capture_output=True, text=True)
    if result.returncode != 0:
        fail(f"{' '.join(argv[:2])} failed ({result.returncode}): {result.stderr[-2000:]}")
    return result.stdout


def cmake_cache(build: Path) -> dict[str, str]:
    cache = {}
    for line in (build / "CMakeCache.txt").read_text().splitlines():
        if match := re.match(r"^([A-Za-z_]+):[A-Z]+=(.*)$", line):
            cache[match[1]] = match[2]
    return cache


def toolchain(cache: dict[str, str], tool: str) -> str:
    """The toolchain program next to the build's nm (c++filt, g++)."""
    nm = Path(cache["CMAKE_NM"])
    return str(nm.with_name(nm.name.removesuffix("nm") + tool))


def project_components() -> list[str]:
    text = (ROOT / "idf/CMakeLists.txt").read_text()
    match = re.search(r"set\(_obbw_project_components ([^)]*)\)", text)
    if not match:
        fail("idf/CMakeLists.txt does not define _obbw_project_components")
    return match[1].split()


def link_edge(build: Path, elf: str) -> dict[str, str]:
    lines = (build / "build.ninja").read_text().splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith(f"build {elf}:")), None)
    if start is None:
        fail(f"{build}/build.ninja has no link edge for {elf}")
    edge = {"inputs": lines[start].split(":", 1)[1]}
    for line in lines[start + 1:]:
        if not line.startswith("  "):
            break
        name, _, value = line.strip().partition(" = ")
        edge[name] = value
    return edge


def link_context(build: Path, cache: dict[str, str],
                 components: list[str]) -> tuple[LinkContext, list[Path]]:
    """Derive the link's non-project inputs, roots, wraps and KEEP sections."""
    elf = cache["CMAKE_PROJECT_NAME"] + ".elf"
    edge = link_edge(build, elf)
    tokens: list[str] = []
    for value in (edge.get("LINK_FLAGS", ""), edge.get("LINK_PATH", ""),
                  edge.get("LINK_LIBRARIES", "")):
        for token in shlex.split(value):
            if token.startswith("@"):
                tokens += shlex.split((build / token[1:]).read_text())
            else:
                tokens.append(token)
    link = LinkContext()
    search, scripts, inputs, libraries = [build], [], [], []
    project = {f"lib{component}.a" for component in components}
    for index, token in enumerate(tokens):
        following = tokens[index + 1] if index + 1 < len(tokens) else ""
        for option in token.removeprefix("-Wl,").split(","):
            if option.startswith("--wrap="):
                link.wrapped.add(option[7:])
            elif option.startswith("--undefined="):
                link.roots.add(option[12:])
        if token == "-u":
            link.roots.add(following)
        elif token == "-T":
            scripts.append(following)
        elif token.startswith("-L"):
            search.append(Path(token[2:]))
        elif token.startswith("-l"):
            libraries.append(f"lib{token[2:]}.a")
        elif token.endswith((".a", ".obj", ".o")) and Path(token).name not in project:
            inputs.append(build / token)
    for token in edge["inputs"].split():
        if token.endswith((".obj", ".o")):
            inputs.append(build / token)
    for library in libraries:
        found = next((d / library for d in search if (d / library).is_file()), None)
        if found is None:
            found = Path(run([toolchain(cache, "g++"), f"-print-file-name={library}"]).strip())
        inputs.append(found)
    inputs = list(dict.fromkeys(inputs))
    missing = [str(path) for path in inputs if not path.is_file()]
    if missing:
        fail("missing link inputs (build incomplete): " + ", ".join(missing[:5]))
    pending, seen = list(scripts), set()
    while pending:
        script = pending.pop()
        path = next((d / script for d in search if (d / script).is_file()), None)
        if path is None:
            fail(f"linker script {script} not found")
        if path in seen:
            continue
        seen.add(path)
        text = re.sub(r"/\*.*?\*/", " ", path.read_text(errors="replace"), flags=re.S)
        pending += re.findall(r"\bINCLUDE\s+(\S+)", text)
        for names in re.findall(r"\b(?:EXTERN|ENTRY)\s*\(([^)]*)\)", text):
            link.roots |= set(names.replace(",", " ").split())
        for match in re.finditer(r"\bKEEP\s*\(", text):
            depth, end = 1, match.end()
            while depth and end < len(text):
                depth += {"(": 1, ")": -1}.get(text[end], 0)
                end += 1
            body = re.sub(r"EXCLUDE_FILE\s*\([^)]*\)", " ", text[match.end():end - 1])
            link.keep |= set(re.findall(r"(?<![\w*])\.[^\s()]+", body))
    return link, inputs


def analyze_object(entry: dict, readelf: str, nm: str) -> ObjectFile:
    output = Path(entry["directory"]) / entry["output"]
    if not output.is_file():
        fail(f"missing project object (build incomplete): {output}")
    obj = parse_readelf(entry["file"], run([readelf, "-W", "-S", "-s", "-r", str(output)]))
    parse_locations(obj, run([nm, "-l", "--defined-only", str(output)]))
    parse_artificial(obj, run([readelf, "--debug-dump=info", str(output)]))
    return obj


def analyze_build(build: Path, components: list[str]) -> tuple[dict, set]:
    cache = cmake_cache(build)
    readelf, nm = cache["CMAKE_READELF"], cache["CMAKE_NM"]
    link, inputs = link_context(build, cache, components)
    parse_nm(run([nm, "-A", "-g", "-P", *map(str, inputs)]), link)
    prefixes = tuple(f"{build}/esp-idf/{component}/" for component in components)
    entries = [entry for entry in json.loads((build / "compile_commands.json").read_text())
               if str(Path(entry["directory"]) / entry["output"]).startswith(prefixes)]
    if not entries:
        fail(f"{build} has no project objects")
    with ThreadPoolExecutor() as pool:
        objects = list(pool.map(lambda entry: analyze_object(entry, readelf, nm), entries))
    return analyze(objects, link)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("pairs", help="print the profile pairs R2/R3 validation builds")
    check = sub.add_parser("analyze", help="analyze dead-code-idf build directories")
    check.add_argument("--report", type=Path, required=True)
    check.add_argument("builds", type=Path, nargs="+")
    args = parser.parse_args()
    if args.command == "pairs":
        dev = runpy.run_path(str(ROOT / "scripts/dev"))
        # The quality gate asks for the experimental pairs through this variable.
        for hardware, machine in dev["validation_pairs"](
                os.environ.get(dev["EXPERIMENTAL_PAIRS_ENV"]) == "1"):
            print(hardware, machine)
        return 0
    components = project_components()
    builds = [analyze_build(build.resolve(), components) for build in args.builds]
    dead = dead_functions(builds)
    names = run([toolchain(cmake_cache(args.builds[0]), "c++filt")],
                "".join(base_name(name) + "\n" for _, name in dead)).splitlines()
    sources = project_sources()
    definitions = {location for build_defined, _ in builds for values in build_defined.values()
                   for location, _ in values if location}
    constant = {(location, name) for (location, _), name in zip(dead, names)
                if location and compile_time_used(location, name, sources, definitions)}
    lines = sorted({f"{(location or '(no source location)').removeprefix(f'{ROOT}/')}: {name}"
                    for (location, _), name in zip(dead, names)
                    if (location, name) not in constant})
    report = "".join(line + "\n" for line in lines)
    args.report.write_text(report)
    print(report, end="")
    print(f"{len(lines)} dead function(s) across {len(builds)} build(s); "
          f"{len(constant)} constexpr function(s) are used only in constant expressions.")
    return 1 if lines else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:  # Any crash is a tool error, never a dead-code verdict.
        traceback.print_exc()
        sys.exit(2)

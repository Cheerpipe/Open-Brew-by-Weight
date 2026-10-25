#!/usr/bin/env python3
"""Bound the worst-case stack depth of task paths that must never overflow.

Every C/C++ unit of an ESP-IDF build is recompiled with GCC call-graph info
(-fcallgraph-info=su) into <build>/stack-usage; the merged graph bounds the
depth from each gate's task entry through its roots. GCC cannot follow
function pointers or see ROM/libc frames, so those calls are charged declared
reserves; recursion or an unbounded dynamic frame on a gated path fails.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

ROOT = Path(__file__).resolve().parents[1]
INDIRECT = "__indirect_call"
# Deepest pointer targets measured on these paths: esp_flash chip and host
# drivers (<= 1136 B) and esp_log_va (928 B).
INDIRECT_RESERVE = 1536
EXTERNAL_RESERVE = 256  # ROM and precompiled libc leaves: mem*, str*, crc, md5
PRINTF_RESERVE = 1024
PRINTF = re.compile(r"^(?:v?s?n?printf|ets_printf|esp_rom_printf)$")
# Each task stack also holds the coprocessor save area (XT_CP_SIZE 316 B on
# ESP32-S3), one interrupt frame (XT_STK_FRMSZ 192 B) and the 20 B canary;
# the remainder covers TLS and alignment.
KERNEL_RESERVE = 640

# Static cycles that cannot recur at run time, cut before the depth search:
# allocation only logs on an invalid-alignment error, and the log lock is
# created once, so this re-entry into the allocator never nests.
CUT_EDGES = ((r"^esp_log_impl_lock_timeout$", r"^xQueueCreateMutex$"),
             # A failed assertion resets the controller whatever its depth.
             (r"", r"^(?:__assert_func|abort|esp_system_abort)$"))

NODE = re.compile(r'^node: \{ title: "([^"]+)" label: "((?:[^"\\]|\\.)*)"')
EDGE = re.compile(r'^edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')
FRAME = re.compile(r"^(\d+) bytes \(([^)]+)\)$")


@dataclass
class Graph:
    frames: dict[str, int] = field(default_factory=dict)
    kinds: dict[str, str] = field(default_factory=dict)
    names: dict[str, str] = field(default_factory=dict)
    edges: dict[str, set[str]] = field(default_factory=lambda: defaultdict(set))

    def add(self, lines: Iterable[str]) -> None:
        for line in lines:
            if node := NODE.match(line):
                title, label = node.groups()
                parts = label.split("\\n")
                frame = FRAME.match(parts[-1])
                # Inline and template functions are emitted by several units.
                if frame and int(frame.group(1)) >= self.frames.get(title, -1):
                    self.frames[title] = int(frame.group(1))
                    self.kinds[title] = frame.group(2)
                    self.names[title] = parts[0]
            elif edge := EDGE.match(line):
                self.edges[edge.group(1)].add(edge.group(2))

    def resolve(self, target: str) -> str | None:
        if target in self.frames:
            return target
        bare = target.rsplit(":", 1)[-1]
        return bare if bare in self.frames else None

    def match(self, pattern: str) -> list[str]:
        regex = re.compile(pattern)
        return [t for t, name in self.names.items()
                if regex.search(name) or regex.search(t)]


@dataclass
class Gate:
    name: str
    budget: Callable[[Path], int]
    entry: str
    roots: tuple[str, ...]
    indirect: dict[str, tuple[str, ...]] = field(default_factory=dict)


@dataclass
class Result:
    gate: str
    budget: int
    depth: int
    path: list[tuple[int, str]]
    errors: list[str]


def sdkconfig_value(build: Path, key: str) -> int:
    found = re.search(rf"^{key}=(\d+)$", (build / "sdkconfig").read_text(), re.M)
    if not found:
        raise ValueError(f"{key} missing from {build / 'sdkconfig'}")
    return int(found.group(1))


def source_constant(path: str, name: str) -> int:
    found = re.search(rf"\b{name}\s*=\s*([\d']+)\s*;", (ROOT / path).read_text())
    if not found:
        raise ValueError(f"{name} missing from {path}")
    return int(found.group(1).replace("'", ""))


GATES = (
    Gate("loopTask boot recovery",
         lambda build: sdkconfig_value(build, "CONFIG_ARDUINO_LOOP_STACK_SIZE"),
         r"\bloopTask\(void\*\)",
         (r"\bresumePendingBootRecovery\(", r"\bmaybeRunBootRecoveryGesture\(")),
    Gate("network_manager factory reset",
         lambda _: source_constant("src/OpenBrewByWeightNetwork.cpp",
                                   "NETWORK_MANAGER_TASK_STACK_SIZE"),
         r"OpenBrewByWeightNetwork::taskEntry\(",
         (r"\bresetAllDurableStoresForNetwork\(",
          r"\breleaseNvsSpaceForFactoryResetForNetwork\("),
         # The FACTORY_RESET command calls both through callbacks_ pointers.
         {r"processPersistedCommand": (
             r"\bresetAllDurableStoresForNetwork\(",
             r"\breleaseNvsSpaceForFactoryResetForNetwork\(")}),
)


def evaluate(graph: Graph, gate: Gate, budget: int) -> Result:
    errors: list[str] = []
    entries = graph.match(gate.entry)
    roots = {t for pattern in gate.roots for t in graph.match(pattern)}
    errors += [f"no function matches {p}" for p in (gate.entry, *gate.roots)
               if not graph.match(p)]
    declared: dict[str, set[str]] = defaultdict(set)
    for source, targets in gate.indirect.items():
        for target in targets:
            found = graph.match(target)
            errors += [] if found else [f"no function matches {target}"]
            for caller in graph.match(source):
                declared[caller].update(found)

    def callees(f: str) -> Iterable[tuple[str | None, str]]:
        for target in graph.edges.get(f, ()):
            resolved = graph.resolve(target)
            if not any(re.search(a, f.rsplit(":", 1)[-1]) and resolved and
                       re.search(b, resolved.rsplit(":", 1)[-1])
                       for a, b in CUT_EDGES):
                yield resolved, target
        for target in declared.get(f, ()):
            yield target, target

    def charge(f: str) -> int:
        targets = graph.edges.get(f, ())
        reserve = INDIRECT_RESERVE if INDIRECT in targets else 0
        for target in targets:
            if target != INDIRECT and graph.resolve(target) is None:
                bare = target.rsplit(":", 1)[-1]
                reserve = max(reserve, PRINTF_RESERVE if PRINTF.match(bare)
                              else EXTERNAL_RESERVE)
        return reserve

    memo: dict[str, tuple[int, str | None]] = {}
    active: set[str] = set()

    def depth(f: str) -> int:
        if f in memo:
            return memo[f][0]
        if f in active:
            errors.append(f"recursion through {graph.names[f]}")
            return 0
        if graph.kinds[f] == "dynamic":
            errors.append(f"unbounded dynamic frame in {graph.names[f]}")
        active.add(f)
        best, child = charge(f), None
        for resolved, _ in callees(f):
            if resolved is not None and (d := depth(resolved)) > best:
                best, child = d, resolved
        active.discard(f)
        memo[f] = (graph.frames[f] + best, child)
        return memo[f][0]

    # Longest chain from the entry down to a root; only the chain is gated.
    chain: dict[str, tuple[int, str | None] | None] = {}

    def to_root(f: str) -> int | None:
        if f in roots:
            return depth(f)
        if f in chain:
            return chain[f][0] if chain[f] else None
        chain[f] = None
        best = None
        for resolved, _ in callees(f):
            if resolved is not None and (d := to_root(resolved)) is not None:
                if best is None or d > best[0]:
                    best = (d, resolved)
        chain[f] = (graph.frames[f] + best[0], best[1]) if best else None
        return chain[f][0] if chain[f] else None

    total, start = 0, None
    for entry in entries:
        found = to_root(entry)
        if found is not None and found >= total:
            total, start = found, entry
    if start is None and entries and roots:
        errors.append("entry does not reach any root; declare the indirect edge")
    path: list[tuple[int, str]] = []
    node, below_root = start, False
    while node is not None:
        path.append((graph.frames[node], graph.names[node]))
        below_root = below_root or node in roots
        node = memo[node][1] if below_root else chain[node][1]
    return Result(gate.name, budget, total + KERNEL_RESERVE if start else 0,
                  path, sorted(set(errors)))


def compile_call_graphs(build: Path, out: Path, jobs: int) -> list[str]:
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    commands = json.loads((build / "compile_commands.json").read_text())

    def run(item: tuple[int, dict]) -> str | None:
        index, entry = item
        if not entry["file"].endswith((".c", ".cc", ".cpp")):
            return None
        args = entry.get("arguments") or shlex.split(entry["command"])
        args = list(args)
        obj = out / f"{index}.o"
        args[args.index("-o") + 1] = str(obj)
        done = subprocess.run([*args, "-fcallgraph-info=su"], cwd=entry["directory"],
                              capture_output=True, text=True)
        obj.unlink(missing_ok=True)
        return None if done.returncode == 0 else f"{entry['file']}: {done.stderr[-400:]}"

    with ThreadPoolExecutor(jobs) as pool:
        return [failure for failure in pool.map(run, enumerate(commands)) if failure]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    out = build / "stack-usage"
    failures = compile_call_graphs(build, out, args.jobs)
    if failures:
        print("\n".join(["stack-depth: call-graph compile failed", *failures[:5]]))
        return 1
    graph = Graph()
    for ci in out.glob("*.ci"):
        with ci.open(errors="replace") as lines:
            graph.add(lines)
    status = 0
    for gate in GATES:
        result = evaluate(graph, gate, gate.budget(build))
        free = result.budget - result.depth
        print(f"stack-depth: {result.gate}: {result.depth} / {result.budget} B "
              f"({free} B free)")
        for frame, name in result.path:
            print(f"  {frame:6d}  {name}")
        print(f"  {KERNEL_RESERVE:6d}  kernel reserve")
        for error in result.errors:
            print(f"  error: {error}")
        if free < 0 or result.errors:
            status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())

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

# A failed assertion resets the controller whatever its depth, so calls into
# these handlers are cut before the depth search.
CUT_EDGES = ((r"", r"^(?:__assert_func|abort|esp_system_abort)$"),)

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


# Recursion GCC reports as a call cycle, with the bound that keeps it finite.
# Every function of a recursive component is charged `levels` times; any
# other recursion on a gated path fails.
RECURSION_BOUNDS: tuple[tuple[str, Callable[[], int]], ...] = (
    # The log lock is created once, so the allocator re-enters at most once.
    (r"^(?:esp_log_impl_lock_timeout|xQueueCreateMutex)$", lambda: 2),
    # send and sendto hand a socket over to each other once, by socket type.
    (r"^lwip_sendto?$", lambda: 2),
    # A listening netconn frees its accepted connections, which never listen.
    (r"^netconn_(?:drain|free)$", lambda: 2),
    # Bodies nested deeper than this are rejected before any recursive parse.
    (r"JsonStructureScanner::scanValue\(|^(?:parse_value|cJSON_Delete)$",
     lambda: source_constant("src/OpenBrewByWeightJsonArena.h",
                             "JSON_DOCUMENT_MAX_DEPTH") + 1),
    # An abandoned tag capture is replayed at most kMaxBodyReplays times.
    (r"OtaImageTagScanner::(?:feedByte|replayAbandonedBody)\(",
     lambda: source_constant("src/OpenBrewByWeightOtaImage.h", "kMaxBodyReplays") + 2),
)

OTA_WORK = (r"::otaSessionWork\(", r"::otaPatchWork\(", r"::otaFlashWork\(",
            r"::otaAbortWork\(")


def http_stack(_: Path) -> int:
    return source_constant("src/OpenBrewByWeightNetwork.cpp", "HTTP_SERVER_TASK_STACK_SIZE")


# httpd_uri calls timedHandler, which calls the registered handler through
# user_ctx; ownedApiHandler calls its route table's handlers.
HTTPD_DISPATCH = {
    r"^httpd_sess_enum$": (r"^httpd_process_session$",),
    r"^httpd_uri$": (r"\btimedHandler\(",),
    r"\btimedHandler\(": (r"::ota(?:Session|Patch|Flash|Abort)Handler\(",
                          r"::uiStreamHandler\(", r"::ownedApiHandler\("),
    r"::ownedApiHandler\(": (r"::crashArchiveDownloadHandler\(",),
}


SHOT_TRACE_ROUTES = (r"::shotTraceDownloadHandler\(", r"::shotStorageHandler\(")


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
    # The shot trace pool (recovery, curve cache load, record staging and
    # append) and the shot-store image that lands a pending pool clear.
    Gate("settings_persist shot pool",
         lambda _: source_constant("src/openBrewByWeight.cpp",
                                   "SETTINGS_PERSIST_TASK_STACK_SIZE"),
         r"\bsettingsPersistTask\(void\*\)",
         # serviceShotTracePool is partly inlined as a $part clone.
         (r"serviceShotTracePool", r"\bpersistShotStoreImage\(")),
    Gate("ota_worker OTA transfer and commit", http_stack, r"::otaWorkerTask\(",
         OTA_WORK, {r"::(?:otaWorkerTask|drainOtaJobs|runOtaJob)\(": OTA_WORK}),
    # Queue full or no memory: the same work runs inline on the httpd task.
    Gate("httpd inline OTA fallback", http_stack, r"^httpd_thread$", OTA_WORK,
         {**HTTPD_DISPATCH, r"::runOnOtaWorker\(": OTA_WORK}),
    # A stream request keeps its frame buffer live through the parse and ops.
    Gate("httpd UI stream request", http_stack, r"^httpd_thread$",
         (r"::uiStreamHandler\(",), HTTPD_DISPATCH),
    # The download verifies unverified dump digests before streaming the tar.
    Gate("httpd crash archive download", http_stack, r"^httpd_thread$",
         (r"::crashArchiveDownloadHandler\(",), HTTPD_DISPATCH),
    # A shot trace streams its pool record through callbacks_ and a sink;
    # Shot storage reads every record header behind a live predicate.
    Gate("httpd shot trace download and storage", http_stack, r"^httpd_thread$",
         SHOT_TRACE_ROUTES,
         {**HTTPD_DISPATCH, r"::ownedApiHandler\(": SHOT_TRACE_ROUTES,
          r"::shotTraceDownloadHandler\(": (r"\bstreamShotTrace\(",),
          r"\bstreamShotTrace\(": (r"\bsendShotTraceChunk\(",),
          r"::shotStorageHandler\(": (r"\bcopyShotStorage\(",),
          r"ShotTracePool::stats\(": (r"\bshotStorageLive\(",)}),
)


def components(entries: list[str],
               callees: Callable[[str], list[str]]) -> dict[str, list[str]]:
    """Iterative Tarjan: map each reachable function to its component."""
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    comp: dict[str, list[str]] = {}
    stack: list[str] = []
    for entry in entries:
        if entry in index:
            continue
        index[entry] = low[entry] = len(index)
        stack.append(entry)
        work = [(entry, iter(callees(entry)))]
        while work:
            node, pending = work[-1]
            for callee in pending:
                if callee not in index:
                    index[callee] = low[callee] = len(index)
                    stack.append(callee)
                    work.append((callee, iter(callees(callee))))
                    break
                if callee not in comp:
                    low[node] = min(low[node], index[callee])
            else:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[node])
                if low[node] == index[node]:
                    members = stack[stack.index(node):]
                    del stack[stack.index(node):]
                    for member in members:
                        comp[member] = members
    return comp


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

    adjacency: dict[str, list[str]] = {}

    def callees(f: str) -> list[str]:
        if f not in adjacency:
            short = f.rsplit(":", 1)[-1]
            adjacency[f] = [r for t in graph.edges.get(f, ())
                            if (r := graph.resolve(t)) and not any(
                                re.search(a, short) and re.search(b, r.rsplit(":", 1)[-1])
                                for a, b in CUT_EDGES)] + sorted(declared.get(f, ()))
        return adjacency[f]

    def charge(f: str) -> int:
        targets = graph.edges.get(f, ())
        reserve = INDIRECT_RESERVE if INDIRECT in targets else 0
        for target in targets:
            if target != INDIRECT and graph.resolve(target) is None:
                bare = target.rsplit(":", 1)[-1]
                reserve = max(reserve, PRINTF_RESERVE if PRINTF.match(bare)
                              else EXTERNAL_RESERVE)
        return reserve

    comp = components(entries, callees)

    def levels(members: list[str]) -> int:
        if len(members) == 1 and members[0] not in callees(members[0]):
            return 1
        for pattern, bound in RECURSION_BOUNDS:
            if any(re.search(pattern, graph.names[m]) or re.search(pattern, m)
                   for m in members):
                return bound()
        errors.append("unbounded recursion through " +
                      ", ".join(sorted(graph.names[m] for m in members)))
        return 1

    # Per component: (depth, deepest callee, own cost, label).
    memo: dict[str, tuple[int, str | None, int, str]] = {}

    def depth(f: str) -> int:
        members = comp[f]
        if members[0] not in memo:
            errors.extend(f"unbounded dynamic frame in {graph.names[m]}"
                          for m in members if graph.kinds[m] == "dynamic")
            best, child = max(charge(m) for m in members), None
            for member in members:
                for callee in callees(member):
                    if comp[callee] is not members and (d := depth(callee)) > best:
                        best, child = d, callee
            times = levels(members)
            own = times * sum(graph.frames[m] for m in members)
            label = graph.names[f] if own == graph.frames[f] else (
                f"{graph.names[f]} [recursion x{times} over {len(members)} functions]")
            memo[members[0]] = (own + best, child, own, label)
        return memo[members[0]][0]

    # Longest chain from the entry down to a root; only the chain is gated.
    chain: dict[str, tuple[int, str | None] | None] = {}

    def to_root(f: str) -> int | None:
        if f in roots:
            return depth(f)
        if f in chain:
            return chain[f][0] if chain[f] else None
        chain[f] = None
        best = None
        for callee in callees(f):
            if (d := to_root(callee)) is not None and (best is None or d > best[0]):
                best = (d, callee)
        if best and levels(comp[f]) != 1:
            errors.append(f"recursion on the path to a root: {graph.names[f]}")
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
        below_root = below_root or node in roots
        if below_root:
            _, child, own, label = memo[comp[node][0]]
            path.append((own, label))
            node = child
        else:
            path.append((graph.frames[node], graph.names[node]))
            node = chain[node][1]
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

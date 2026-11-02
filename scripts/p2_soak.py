#!/usr/bin/env python3
"""Capture and qualify Open Brew by Weight P2 heap/timing soak evidence."""

from __future__ import annotations

import argparse
import json
import math
import os
import signal
import statistics
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


COUNTERS = (
    "loopDeadlineMisses",
    "scaleWorkerDeadlineMisses",
)
LIFECYCLE_FAMILIES = ("http", "wifi", "ota", "webhookTls", "micraTls")


def number(source: dict[str, Any], key: str) -> float | None:
    value = source.get(key)
    return (float(value) if not isinstance(value, bool)
            and isinstance(value, (int, float)) and math.isfinite(value) else None)


def linear_slope_per_hour(values: list[float], interval_s: float) -> float:
    if len(values) < 2 or interval_s <= 0:
        return 0.0
    xs = [index * interval_s / 3600.0 for index in range(len(values))]
    x_mean = statistics.fmean(xs)
    y_mean = statistics.fmean(values)
    denominator = sum((x - x_mean) ** 2 for x in xs)
    if denominator == 0:
        return 0.0
    return sum((x - x_mean) * (y - y_mean) for x, y in zip(xs, values)) / denominator


def analyze(records: list[dict[str, Any]], args: argparse.Namespace) -> dict[str, Any]:
    payloads = [record["payload"] for record in records if "payload" in record]
    errors = [record for record in records if "error" in record]
    health = [payload.get("health", {}) for payload in payloads]
    failures: list[str] = []
    metrics: dict[str, Any] = {"samples": len(payloads), "fetchErrors": len(errors)}

    if len(payloads) < args.min_samples:
        failures.append(f"only {len(payloads)} valid samples; need {args.min_samples}")
    if errors:
        failures.append(f"{len(errors)} snapshot fetch/decode errors")

    uptimes = [value for item in health if (value := number(item, "uptimeMs")) is not None]
    reboot_count = sum(right < left for left, right in zip(uptimes, uptimes[1:]))
    metrics["reboots"] = reboot_count
    if reboot_count:
        failures.append(f"{reboot_count} uptime regression(s)/reboot(s)")

    stale_count = sum(item.get("snapshotStale") is True for item in health)
    metrics["staleSnapshots"] = stale_count
    if stale_count:
        failures.append(f"{stale_count} stale control snapshot(s)")

    for key in COUNTERS:
        values = [value for item in health if (value := number(item, key)) is not None]
        if not values:
            continue
        delta = max(values) - min(values)
        metrics[key + "Delta"] = delta
        if delta > 0:
            failures.append(f"{key} increased by {delta:g}")

    for key, minimum in (
        ("freeHeapBytes", args.min_internal_free),
        ("largestFreeHeapBlockBytes", args.min_internal_largest),
        ("psramFreeBytes", args.min_psram_free),
        ("psramLargestFreeBlockBytes", args.min_psram_largest),
    ):
        values = [value for item in health if (value := number(item, key)) is not None]
        if len(values) != len(health):
            failures.append(f"missing or invalid {key} in memory samples")
        if not values:
            continue
        observed_min = min(values)
        metrics[key + "Minimum"] = observed_min
        if observed_min < minimum:
            failures.append(f"{key} minimum {observed_min:g} < {minimum}")

    largest = [
        value
        for item in health
        if (value := number(item, "largestFreeHeapBlockBytes")) is not None
    ]
    if len(largest) >= 2:
        delta = largest[-1] - largest[0]
        slope = linear_slope_per_hour(largest, args.interval)
        metrics["largestBlockFirstLastDeltaBytes"] = delta
        metrics["largestBlockSlopeBytesPerHour"] = slope
        if delta < -args.max_largest_drop and slope < 0:
            failures.append(
                "internal largest block has sustained negative trend "
                f"(delta={delta:g}, slope={slope:.1f} B/h)"
            )

    internal_free = [
        value for item in health
        if (value := number(item, "freeHeapBytes")) is not None
    ]
    if len(internal_free) >= 2:
        delta = internal_free[-1] - internal_free[0]
        slope = linear_slope_per_hour(internal_free, args.interval)
        metrics["internalFreeFirstLastDeltaBytes"] = delta
        metrics["internalFreeSlopeBytesPerHour"] = slope
        if delta < -args.max_free_drop and slope < 0:
            failures.append(
                "internal free heap has sustained negative trend "
                f"(delta={delta:g}, slope={slope:.1f} B/h)"
            )

    free_blocks = [
        value for item in health
        if (value := number(item, "internalHeapFreeBlocks")) is not None
    ]
    fragmentation = [
        value for item in health
        if (value := number(item, "internalHeapFragmentationPermille")) is not None
    ]
    for key in ("internalHeapAllocatedBlocks", "internalHeapFreeBlocks",
                "internalHeapTotalBlocks", "internalHeapFragmentationPermille"):
        if sum(number(item, key) is not None for item in health) != len(health):
            failures.append(f"missing or invalid {key} in memory samples")
    if len(free_blocks) >= 2:
        delta = free_blocks[-1] - free_blocks[0]
        metrics["internalFreeBlocksFirstLastDelta"] = delta
        if delta > args.max_free_blocks_growth:
            failures.append(f"internal free-block count increased by {delta:g}")
    if len(fragmentation) >= 2:
        delta = fragmentation[-1] - fragmentation[0]
        slope = linear_slope_per_hour(fragmentation, args.interval)
        metrics["fragmentationFirstLastDeltaPermille"] = delta
        metrics["fragmentationSlopePermillePerHour"] = slope
        if delta > args.max_fragmentation_growth and slope > 0:
            failures.append(
                "internal fragmentation has sustained positive trend "
                f"(delta={delta:g}, slope={slope:.1f} permille/h)"
            )

    lifecycle_metrics: dict[str, Any] = {}
    required_lifecycles = set(args.require_lifecycle)
    for family in LIFECYCLE_FAMILIES:
        samples = []
        for payload in payloads:
            lifecycle = payload.get("heapLifecycle", {})
            item = lifecycle.get(family) if isinstance(lifecycle, dict) else None
            if isinstance(item, dict):
                samples.append(item)
        if not samples:
            if family in required_lifecycles:
                failures.append(f"missing required lifecycle telemetry: {family}")
            continue
        cycles = [value for item in samples
                  if (value := number(item, "cycles")) is not None]
        final = samples[-1]
        last_delta = final.get("lastDelta", {})
        after_samples = [item.get("after", {}) for item in samples]
        free_blocks_after = [value for item in after_samples
                             if (value := number(item, "freeBlocks")) is not None]
        allocated_blocks_after = [value for item in after_samples
                                  if (value := number(item, "allocatedBlocks")) is not None]
        largest_free_ratios = []
        for item in after_samples:
            free_bytes = number(item, "freeBytes")
            largest_block = number(item, "largestBlock")
            if free_bytes is not None and free_bytes > 0 and largest_block is not None:
                largest_free_ratios.append(largest_block / free_bytes)
        lifecycle_metrics[family] = {
            "cycleCountDelta": max(cycles) - min(cycles) if cycles else 0,
            "finalFreeDeltaBytes": number(last_delta, "freeBytes"),
            "finalLargestDeltaBytes": number(last_delta, "largestBlock"),
            "finalFreeBlocksDelta": number(last_delta, "freeBlocks"),
            "worstFreeDeltaBytes": number(final, "worstFreeDelta"),
            "worstLargestDeltaBytes": number(final, "worstLargestDelta"),
            "maximumFreeBlocksIncrease": number(
                final, "maximumFreeBlocksIncrease"),
            "freeBlocksFirstLastDelta": (
                free_blocks_after[-1] - free_blocks_after[0]
                if len(free_blocks_after) >= 2 else None),
            "allocatedBlocksFirstLastDelta": (
                allocated_blocks_after[-1] - allocated_blocks_after[0]
                if len(allocated_blocks_after) >= 2 else None),
            "largestFreeRatioFirstLastDelta": (
                largest_free_ratios[-1] - largest_free_ratios[0]
                if len(largest_free_ratios) >= 2 else None),
        }
        cycle_delta = lifecycle_metrics[family]["cycleCountDelta"]
        if family in required_lifecycles and cycle_delta <= 0:
            failures.append(f"required lifecycle had no cycle coverage: {family}")
        final_largest = lifecycle_metrics[family]["finalLargestDeltaBytes"]
        if (family in required_lifecycles and final_largest is not None and
                final_largest < -args.max_lifecycle_largest_loss):
            failures.append(
                f"{family} largest block did not recover "
                f"({final_largest:g} bytes)"
            )
    metrics["lifecycles"] = lifecycle_metrics

    psram_largest = [
        value
        for item in health
        if (value := number(item, "psramLargestFreeBlockBytes")) is not None
    ]
    psram_free = [
        value
        for item in health
        if (value := number(item, "psramFreeBytes")) is not None
    ]
    if psram_free:
        metrics["psramFreeBytesMinimum"] = min(psram_free)
    if len(psram_largest) >= 2:
        delta = psram_largest[-1] - psram_largest[0]
        slope = linear_slope_per_hour(psram_largest, args.interval)
        metrics["psramLargestFirstLastDeltaBytes"] = delta
        metrics["psramLargestSlopeBytesPerHour"] = slope
        if delta < -args.max_psram_largest_drop and slope < 0:
            failures.append(
                "PSRAM largest block has sustained negative trend "
                f"(delta={delta:g}, slope={slope:.1f} B/h)"
            )

    live_clients: list[float] = []
    worker_starts: list[float] = []
    task_counts: list[int] = []
    for payload in payloads:
        webhook = payload.get("webhooks", {})
        if isinstance(webhook, dict):
            created = number(webhook, "clientCreates")
            cleaned = number(webhook, "clientCleanups")
            if created is not None and cleaned is not None:
                live_clients.append(created - cleaned)
            starts = number(webhook, "workerStarts")
            if starts is not None:
                worker_starts.append(starts)
        else:
            failures.append("missing webhooks lifecycle snapshot")
        rows = payload.get("tasks", {}).get("rows", [])
        if isinstance(rows, list) and rows:
            task_counts.append(len(rows))
    if live_clients:
        metrics["webhookLiveClientMaximum"] = max(live_clients)
        if min(live_clients) < 0 or max(live_clients) > 1:
            failures.append(
                f"webhook live HTTP handles out of bound: {min(live_clients):g}..{max(live_clients):g}"
            )
    else:
        failures.append("missing webhook client create/cleanup counters")
    if worker_starts:
        starts_delta = max(worker_starts) - min(worker_starts)
        metrics["webhookWorkerStartsDelta"] = starts_delta
        if starts_delta > 1:
            failures.append(
                f"webhook worker restarted {starts_delta:g} times during soak"
            )
    else:
        failures.append("missing webhook worker lifecycle counters")
    if task_counts:
        metrics["profiledTaskCountRange"] = [min(task_counts), max(task_counts)]

    stack_values: list[float] = []
    for item in health:
        for stem in ("loopStackMin", "scaleStackMin", "bleRuntimeHostStackMin"):
            # Legacy *Words fields on ESP32-S3 have always contained bytes.
            key = stem + ("Bytes" if stem + "Bytes" in item else "Words")
            value = number(item, key)
            if value is not None and 0 <= value < 0xffffffff:
                stack_values.append(value)
            else:
                failures.append(f"missing or unavailable {stem} byte watermark")
    profiled_stack_values: list[float] = []
    for payload in payloads:
        tasks = payload.get("tasks", {})
        rows = tasks.get("rows", [])
        sampled_tasks: set[str] = set()
        if isinstance(rows, list):
            for row in rows:
                if isinstance(row, dict):
                    key = "stackMinBytes" if "stackMinBytes" in row else "stackMinWords"
                    value = number(row, key)
                    if value is not None and 0 <= value < 0xffffffff:
                        name = str(row.get("name", ""))
                        profiled_stack_values.append(value)
                        sampled_tasks.add(name)
                        if value == 0:
                            failures.append(f"exhausted task stack: {name}")
                        if name in args.require_task:
                            stack_values.append(value)
        if args.require_task:
            if tasks.get("state") != "running":
                failures.append("required task coverage needs a running profiler")
            for task in args.require_task:
                if task not in sampled_tasks:
                    failures.append(f"missing required task watermark: {task}")
    metrics["profiledStackMinimumBytes"] = (
        min(profiled_stack_values) if profiled_stack_values else None
    )
    if stack_values:
        metrics["stackMinimumBytes"] = min(stack_values)
        if min(stack_values) < args.min_stack_bytes:
            failures.append(
                f"stack watermark {min(stack_values):g} < {args.min_stack_bytes} bytes"
            )
    else:
        metrics["stackMinimumBytes"] = None
        failures.append("no valid stack watermarks")

    return {
        "schemaVersion": 3,
        "scenario": args.scenario,
        "passed": not failures,
        "metrics": metrics,
        "limits": {
            "minSamples": args.min_samples,
            "minInternalFreeBytes": args.min_internal_free,
            "minInternalLargestBytes": args.min_internal_largest,
            "maxLargestFirstLastDropBytes": args.max_largest_drop,
            "maxFreeFirstLastDropBytes": args.max_free_drop,
            "maxFreeBlocksGrowth": args.max_free_blocks_growth,
            "maxFragmentationGrowthPermille": args.max_fragmentation_growth,
            "maxLifecycleLargestLossBytes": args.max_lifecycle_largest_loss,
            "maxPsramLargestFirstLastDropBytes": args.max_psram_largest_drop,
            "minStackBytes": args.min_stack_bytes,
            "minPsramFreeBytes": args.min_psram_free,
            "minPsramLargestBytes": args.min_psram_largest,
            "requiredTasks": args.require_task,
            "requiredLifecycles": args.require_lifecycle,
        },
        "failures": list(dict.fromkeys(failures)),
    }


def load_headers(env_name: str) -> dict[str, str]:
    raw = os.environ.get(env_name, "")
    if not raw:
        return {}
    decoded = json.loads(raw)
    if not isinstance(decoded, dict) or not all(
        isinstance(key, str) and isinstance(value, str)
        for key, value in decoded.items()
    ):
        raise ValueError(f"{env_name} must be a JSON object of string headers")
    return decoded


def fetch(url: str, headers: dict[str, str], timeout: float) -> dict[str, Any]:
    request = urllib.request.Request(url, headers=headers, method="GET")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = json.load(response)
    if not isinstance(payload, dict):
        raise ValueError("status response is not a JSON object")
    return payload


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--url", help="Full diagnostic/admin status URL")
    result.add_argument("--output", type=Path, help="JSONL evidence path")
    result.add_argument("--scenario", default="combined")
    result.add_argument("--duration", type=float, default=8 * 3600)
    result.add_argument("--interval", type=float, default=5.0)
    result.add_argument("--timeout", type=float, default=4.0)
    result.add_argument("--headers-env", default="OPENBREWBYWEIGHT_SOAK_HEADERS")
    result.add_argument("--min-samples", type=int, default=60)
    result.add_argument("--min-internal-free", type=int, default=48 * 1024)
    result.add_argument("--min-internal-largest", type=int, default=16 * 1024)
    result.add_argument("--max-largest-drop", type=int, default=16 * 1024)
    result.add_argument("--max-free-drop", type=int, default=16 * 1024)
    result.add_argument("--max-free-blocks-growth", type=int, default=8)
    result.add_argument("--max-fragmentation-growth", type=int, default=50)
    result.add_argument("--max-lifecycle-largest-loss", type=int, default=4096)
    result.add_argument("--max-psram-largest-drop", type=int, default=64 * 1024)
    result.add_argument("--min-stack-bytes", type=int, default=1536,
                        help="Minimum free stack bytes per required task")
    result.add_argument("--min-psram-free", type=int, default=128 * 1024)
    # Must stay at or above sizeof(NetworkWorkBuf), the largest single PSRAM
    # allocation, so the largest free block always covers it.
    result.add_argument("--min-psram-largest", type=int, default=76 * 1024)
    result.add_argument("--require-task", action="append", default=[],
                        help="Require this task in every running profiler snapshot (repeatable)")
    result.add_argument("--require-lifecycle", action="append", default=[],
                        choices=LIFECYCLE_FAMILIES,
                        help="Require cycles and recovery for this lifecycle family")
    result.add_argument("--self-test", action="store_true")
    return result


def self_test(args: argparse.Namespace) -> int:
    args.min_samples = 2
    healthy = {
        "health": {
            "uptimeMs": 1000,
            "snapshotStale": False,
            "loopDeadlineMisses": 0,
            "scaleWorkerDeadlineMisses": 0,
            "freeHeapBytes": 100000,
            "largestFreeHeapBlockBytes": 60000,
            "internalHeapAllocatedBlocks": 120,
            "internalHeapFreeBlocks": 8,
            "internalHeapTotalBlocks": 128,
            "internalHeapFragmentationPermille": 400,
            "loopStackMinBytes": 2048,
            "scaleStackMinBytes": 2048,
            "bleRuntimeHostStackMinWords": 2048,
            "psramFreeBytes": 1000000,
            "psramLargestFreeBlockBytes": 900000,
        },
        "webhooks": {
            "workerStarts": 1,
            "clientCreates": 1,
            "clientCleanups": 0,
        },
        "heapLifecycle": {
            family: {
                "cycles": 1,
                "lastResult": 1,
                "after": {"freeBytes": 100000, "largestBlock": 60000,
                          "allocatedBlocks": 120, "freeBlocks": 8},
                "lastDelta": {"freeBytes": 0, "largestBlock": 0,
                              "freeBlocks": 0},
                "worstFreeDelta": 0,
                "worstLargestDelta": 0,
                "maximumFreeBlocksIncrease": 0,
            } for family in LIFECYCLE_FAMILIES
        },
    }
    records = [{"payload": healthy}, {"payload": json.loads(json.dumps(healthy))}]
    records[1]["payload"]["health"]["uptimeMs"] = 2000
    assert analyze(records, args)["passed"]
    records[1]["payload"]["health"]["loopDeadlineMisses"] = 1
    assert not analyze(records, args)["passed"]
    records[1]["payload"]["health"]["loopDeadlineMisses"] = 0
    for key in ("loopStackMinBytes", "scaleStackMinBytes",
                "bleRuntimeHostStackMinWords", "psramFreeBytes",
                "psramLargestFreeBlockBytes"):
        original = records[1]["payload"]["health"][key]
        for invalid in (0, None, float("nan"), True):
            records[1]["payload"]["health"][key] = invalid
            assert not analyze(records, args)["passed"], (key, invalid)
        records[1]["payload"]["health"][key] = original
    for watermark in (255, 256, 383, 384, 1023, 1024, 1535, 1536):
        records[1]["payload"]["health"]["loopStackMinBytes"] = watermark
        assert analyze(records, args)["passed"] == (watermark >= args.min_stack_bytes)
    records[1]["payload"]["health"]["loopStackMinBytes"] = 0xffffffff
    assert not analyze(records, args)["passed"]
    records[1]["payload"]["health"]["loopStackMinBytes"] = 2048
    args.require_task = ["httpd"]
    assert not analyze(records, args)["passed"]
    for record in records:
        record["payload"]["tasks"] = {"state": "running", "rows": [
            {"name": "httpd", "stackMinWords": 2048}]}
    assert analyze(records, args)["passed"]
    records[1]["payload"]["tasks"]["rows"].append(
        {"name": "IDLE0", "stackMinWords": 512})
    summary = analyze(records, args)
    assert summary["passed"]
    assert summary["metrics"]["profiledStackMinimumBytes"] == 512
    records[1]["payload"]["tasks"]["rows"][0]["stackMinWords"] = 1535
    assert not analyze(records, args)["passed"]
    records[1]["payload"]["tasks"]["rows"][0]["stackMinWords"] = 1536
    assert analyze(records, args)["passed"]
    records[1]["payload"]["tasks"]["rows"][-1]["stackMinWords"] = 0
    assert not analyze(records, args)["passed"]
    records[1]["payload"]["tasks"]["rows"][-1]["stackMinWords"] = 512

    legacy = json.loads(json.dumps(records))
    del legacy[1]["payload"]["health"]["internalHeapFreeBlocks"]
    assert "missing or invalid internalHeapFreeBlocks in memory samples" in \
        analyze(legacy, args)["failures"]
    records[1]["payload"]["tasks"]["state"] = "stopped"
    assert not analyze(records, args)["passed"]
    args.require_task = []
    records[1]["payload"]["tasks"]["rows"][-1]["stackMinWords"] = 0
    assert not analyze(records, args)["passed"]
    records[1]["payload"]["tasks"]["rows"][-1]["stackMinWords"] = 512

    # A transient handshake dip is acceptable when the quiescent sample recovers.
    transient = json.loads(json.dumps(records[0]))
    transient["payload"]["health"]["freeHeapBytes"] = 70000
    transient["payload"]["health"]["largestFreeHeapBlockBytes"] = 30000
    recovered = json.loads(json.dumps(records[0]))
    recovered["payload"]["health"]["uptimeMs"] = 3000
    assert analyze([records[0], transient, recovered], args)["passed"]

    args.require_lifecycle = ["http", "webhookTls"]
    degraded = json.loads(json.dumps(records[1]))
    for family in args.require_lifecycle:
        degraded["payload"]["heapLifecycle"][family]["cycles"] = 2
        degraded["payload"]["heapLifecycle"][family]["lastDelta"][
            "largestBlock"] = -8192
    assert not analyze([records[0], degraded], args)["passed"]

    # A failed asynchronous Wi-Fi attempt and OTA abort may dip transiently;
    # the following retry/success sample must restore the same block topology.
    transient_lifecycle = json.loads(json.dumps(records[1]))
    stable = json.loads(json.dumps(records[1]))
    for family in ("webhookTls", "wifi", "ota"):
        transient_item = transient_lifecycle["payload"]["heapLifecycle"][family]
        transient_item["cycles"] = 2
        transient_item["lastResult"] = 2
        transient_item["after"] = {
            "freeBytes": 92000, "largestBlock": 50000,
            "allocatedBlocks": 125, "freeBlocks": 12,
        }
        stable_item = stable["payload"]["heapLifecycle"][family]
        stable_item["cycles"] = 3
        stable_item["lastResult"] = 1
    args.require_lifecycle = ["webhookTls", "wifi", "ota"]
    recovered_summary = analyze(
        [records[0], transient_lifecycle, stable], args)
    assert recovered_summary["passed"]
    for family in args.require_lifecycle:
        family_metrics = recovered_summary["metrics"]["lifecycles"][family]
        assert family_metrics["freeBlocksFirstLastDelta"] == 0
        assert family_metrics["largestFreeRatioFirstLastDelta"] == 0
    return 0


def main() -> int:
    args = parser().parse_args()
    if args.self_test:
        return self_test(args)
    if not args.url or args.output is None:
        parser().error("--url and --output are required unless --self-test is used")
    if args.duration <= 0 or args.interval <= 0 or args.timeout <= 0:
        parser().error("duration, interval and timeout must be positive")

    try:
        headers = load_headers(args.headers_env)
    except (ValueError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, Any]] = []
    stopped = False

    def stop_capture(_signum: int, _frame: Any) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_capture)
    signal.signal(signal.SIGTERM, stop_capture)
    started = time.monotonic()
    next_sample = started
    with args.output.open("w", encoding="utf-8") as evidence:
        while not stopped and time.monotonic() - started < args.duration:
            captured = time.time()
            record: dict[str, Any] = {
                "schemaVersion": 2,
                "capturedUnixSec": captured,
                "elapsedSec": time.monotonic() - started,
                "scenario": args.scenario,
            }
            try:
                record["payload"] = fetch(args.url, headers, args.timeout)
            except (OSError, ValueError, urllib.error.URLError, json.JSONDecodeError) as error:
                record["error"] = f"{type(error).__name__}: {error}"
            records.append(record)
            evidence.write(json.dumps(record, separators=(",", ":")) + "\n")
            evidence.flush()
            next_sample += args.interval
            delay = next_sample - time.monotonic()
            if delay > 0:
                time.sleep(delay)

    summary = analyze(records, args)
    summary["durationSec"] = time.monotonic() - started
    summary["evidence"] = str(args.output)
    summary_path = args.output.with_suffix(args.output.suffix + ".summary.json")
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Collect thread-scaling metrics for the realtime demo."""

from __future__ import annotations

import argparse
import csv
import datetime as _dt
import json
import os
import pathlib
import subprocess
import sys
from typing import Any, Dict, List, Mapping, MutableMapping, Optional, Sequence, Tuple


def _positive_int(value: Optional[int], fallback: int) -> int:
    if value is None or value <= 0:
        return fallback
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Sweep worker thread counts (SMT on/off) for rtfw_demo and capture "
            "interval metrics."
        )
    )
    parser.add_argument(
        "--binary",
        default="build/bin/rtfw_demo",
        help="Path to the realtime demo binary (default: %(default)s)",
    )
    parser.add_argument(
        "--max-threads",
        type=int,
        default=None,
        help="Override logical CPU count for SMT-on sweeps.",
    )
    parser.add_argument(
        "--physical-threads",
        type=int,
        default=None,
        help="Override detected physical core count for SMT-off sweeps.",
    )
    parser.add_argument(
        "--output-dir",
        default="results/scaling",
        help="Directory that will receive the CSV/JSON artifacts.",
    )
    parser.add_argument(
        "--tag",
        default=None,
        help="Optional label to append to the artifact filenames.",
    )
    parser.add_argument(
        "--demo-arg",
        action="append",
        dest="demo_args",
        default=None,
        help="Extra argument to pass to the demo binary (repeatable).",
    )
    parser.add_argument(
        "--pin",
        action="store_true",
        help="Pin worker threads by passing --pin to the demo binary.",
    )
    parser.add_argument(
        "--env",
        action="append",
        dest="env_vars",
        default=None,
        metavar="KEY=VALUE",
        help="Environment override to export while running the demo (repeatable).",
    )
    parser.add_argument(
        "--json-indent",
        type=int,
        default=2,
        help="Indentation level for the JSON artifact (default: %(default)s).",
    )
    return parser.parse_args()


def detect_git_commit() -> Optional[str]:
    try:
        completed = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    return completed.stdout.strip() or None


def detect_logical_cores(override: Optional[int]) -> int:
    logical = os.cpu_count() or 1
    logical = _positive_int(override, logical)
    return max(1, logical)


def detect_physical_cores(override: Optional[int], logical: int) -> int:
    if override is not None and override > 0:
        return min(override, logical)

    try:
        completed = subprocess.run(
            ["lscpu", "-p=CPU,CORE,SOCKET"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        return max(1, logical // 2)

    pairs = set()
    for line in completed.stdout.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        if len(parts) < 2:
            continue
        core_id = parts[1]
        socket_id = parts[2] if len(parts) > 2 else "0"
        try:
            key = (int(core_id), int(socket_id))
        except ValueError:
            continue
        pairs.add(key)

    if not pairs:
        return max(1, logical // 2)

    return min(len(pairs), logical)


def build_sweep(limit: int) -> List[int]:
    if limit <= 0:
        return [1]
    values: List[int] = []
    current = 1
    while current < limit:
        values.append(current)
        current *= 2
    if not values or values[-1] != limit:
        values.append(limit)
    return values


def merge_env(overrides: Optional[Sequence[str]]) -> MutableMapping[str, str]:
    env = dict(os.environ)
    if not overrides:
        return env
    for item in overrides:
        if not item:
            continue
        if "=" not in item:
            raise SystemExit(f"Invalid --env override (expected KEY=VALUE): {item!r}")
        key, value = item.split("=", 1)
        env[key] = value
    return env


def run_demo(
    binary: pathlib.Path,
    threads: int,
    *,
    pin: bool,
    extra_args: Optional[Sequence[str]],
    env: Mapping[str, str],
) -> Mapping[str, Any]:
    if not binary.exists():
        raise SystemExit(f"Demo binary not found: {binary}")

    cmd: List[str] = [str(binary), "--threads", str(threads), "--metrics-json-interval"]
    if pin:
        cmd.append("--pin")
    if extra_args:
        cmd.extend(extra_args)

    try:
        completed = subprocess.run(
            cmd,
            check=True,
            capture_output=True,
            text=True,
            env=env,
        )
    except subprocess.CalledProcessError as exc:
        sys.stderr.write("\n".join([
            f"Command failed with exit code {exc.returncode}",
            "STDOUT:",
            exc.stdout or "<empty>",
            "STDERR:",
            exc.stderr or "<empty>",
        ]) + "\n")
        raise SystemExit(exc.returncode)

    return extract_metrics_payload(completed.stdout)


def extract_metrics_payload(stdout: str) -> Mapping[str, Any]:
    candidate: Optional[Mapping[str, Any]] = None
    for line in stdout.splitlines():
        text = line.strip()
        if not text:
            continue
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, Mapping):
            candidate = parsed
    if candidate is None:
        raise SystemExit("Unable to locate metrics JSON in demo output")
    return candidate


def summarise_metrics(payload: Mapping[str, Any]) -> Dict[str, float]:
    phases = payload.get("phases")
    p50 = 0.0
    p95 = 0.0
    p99 = 0.0
    if isinstance(phases, Mapping):
        for phase in phases.values():
            if not isinstance(phase, Mapping):
                continue
            p50 += _coerce_float(phase.get("p50_ms"))
            p95 += _coerce_float(phase.get("p95_ms"))
            p99 += _coerce_float(phase.get("p99_ms"))
    stdev = 0.0
    if p95 > p50:
        stdev = max((p95 - p50) / 1.645, 0.0)
    variance = stdev * stdev
    summary = {
        "p50_frame_ms": p50,
        "p95_frame_ms": p95,
        "p99_frame_ms": p99,
        "stdev_frame_ms": stdev,
        "variance_frame_ms2": variance,
    }
    return summary


def _coerce_float(value: Any) -> float:
    if isinstance(value, (int, float)):
        return float(value)
    return 0.0


def compute_speedups(rows: List[Dict[str, Any]]) -> None:
    global_baseline = next((row for row in rows if row["threads"] == 1), None)
    global_p99 = _coerce_float(global_baseline["p99_frame_ms"]) if global_baseline else 0.0

    baselines: Dict[str, float] = {}
    for row in rows:
        mode = row["mode"]
        if mode not in baselines and row["threads"] == 1:
            baselines[mode] = _coerce_float(row["p99_frame_ms"])

    for row in rows:
        p99 = _coerce_float(row["p99_frame_ms"])
        row["speedup_vs_mode"] = p99 and baselines.get(row["mode"], p99) / p99 or 0.0
        row["speedup_vs_1t"] = p99 and (global_p99 / p99 if global_p99 > 0 else 0.0) or 0.0


def write_csv(path: pathlib.Path, rows: Sequence[Dict[str, Any]]) -> None:
    fieldnames = [
        "mode",
        "threads",
        "p50_frame_ms",
        "p95_frame_ms",
        "p99_frame_ms",
        "stdev_frame_ms",
        "variance_frame_ms2",
        "speedup_vs_mode",
        "speedup_vs_1t",
    ]
    with path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field) for field in fieldnames})


def assemble_json(
    path: pathlib.Path,
    *,
    rows: Sequence[Dict[str, Any]],
    raw_metrics: Dict[Tuple[str, int], Mapping[str, Any]],
    metadata: Mapping[str, Any],
    indent: int,
) -> None:
    payload = {
        "metadata": dict(metadata),
        "runs": [
            {
                "mode": row["mode"],
                "threads": row["threads"],
                "summary": {
                    key: row[key]
                    for key in (
                        "p50_frame_ms",
                        "p95_frame_ms",
                        "p99_frame_ms",
                        "stdev_frame_ms",
                        "variance_frame_ms2",
                        "speedup_vs_mode",
                        "speedup_vs_1t",
                    )
                },
                "metrics": raw_metrics.get((row["mode"], row["threads"]), {}),
            }
            for row in rows
        ],
    }
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=indent)
        fh.write("\n")


def print_summary(rows: Sequence[Dict[str, Any]]) -> None:
    if not rows:
        print("No results captured.")
        return
    header = f"{'Mode':<10} {'Threads':>7} {'p99 (ms)':>10} {'stdev (ms)':>12} {'var (ms^2)':>12} {'speedup':>9}"
    print(header)
    print("-" * len(header))
    for row in rows:
        mode = str(row["mode"])
        threads = int(row["threads"])
        p99 = _coerce_float(row["p99_frame_ms"])
        stdev = _coerce_float(row["stdev_frame_ms"])
        var = _coerce_float(row["variance_frame_ms2"])
        speedup = _coerce_float(row["speedup_vs_1t"])
        print(
            f"{mode:<10} {threads:>7d} {p99:>10.3f} {stdev:>12.3f} {var:>12.3f} {speedup:>9.3f}"
        )


def main() -> None:
    args = parse_args()

    binary = pathlib.Path(args.binary)
    logical = detect_logical_cores(args.max_threads)
    physical = detect_physical_cores(args.physical_threads, logical)

    smt_on_sweep = build_sweep(logical)
    smt_off_sweep = build_sweep(min(physical, logical))

    output_dir = pathlib.Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    timestamp = _dt.datetime.now(tz=_dt.timezone.utc).strftime("%Y%m%d-%H%M%S")
    prefix = timestamp if not args.tag else f"{timestamp}-{args.tag}"
    csv_path = output_dir / f"{prefix}.csv"
    json_path = output_dir / f"{prefix}.json"

    env = merge_env(args.env_vars)

    rows: List[Dict[str, Any]] = []
    raw_metrics: Dict[Tuple[str, int], Mapping[str, Any]] = {}

    modes = [
        ("smt_off", smt_off_sweep),
        ("smt_on", smt_on_sweep),
    ]

    for mode, sweep in modes:
        seen: set[int] = set()
        for threads in sweep:
            if threads in seen and mode == "smt_on":
                # Avoid running duplicate thread counts twice within SMT-on sweep.
                continue
            seen.add(threads)
            print(f"Running {mode} with {threads} thread(s)...", flush=True)
            payload = run_demo(
                binary,
                threads,
                pin=args.pin,
                extra_args=args.demo_args,
                env=env,
            )
            raw_metrics[(mode, threads)] = payload
            summary = summarise_metrics(payload)
            summary.update({"mode": mode, "threads": threads})
            rows.append(summary)

    rows.sort(key=lambda row: (row["mode"], row["threads"]))
    compute_speedups(rows)

    write_csv(csv_path, rows)

    metadata = {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "binary": str(binary),
        "logical_threads": logical,
        "physical_threads": physical,
        "git_commit": detect_git_commit(),
        "pin": bool(args.pin),
        "extra_args": list(args.demo_args or []),
    }
    assemble_json(
        json_path,
        rows=rows,
        raw_metrics=raw_metrics,
        metadata=metadata,
        indent=args.json_indent,
    )

    print()
    print(f"Wrote CSV: {csv_path}")
    print(f"Wrote JSON: {json_path}")
    print()
    print_summary(rows)


if __name__ == "__main__":
    main()

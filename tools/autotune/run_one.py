#!/usr/bin/env python3
"""Run one autotune sample and score the result."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import subprocess
import sys
from typing import Any, Dict, Iterable, List, Optional


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the demo once and score metrics.")
    parser.add_argument("--app", required=True, help="Path to the executable to launch.")
    parser.add_argument(
        "--config",
        required=True,
        help="Path to the configuration JSON file produced from the sampled parameters.",
    )
    parser.add_argument(
        "--warmup",
        type=float,
        default=0.0,
        help="Warmup duration in seconds before collecting metrics.",
    )
    parser.add_argument(
        "--run",
        type=float,
        required=True,
        help="Measurement duration in seconds for the interval snapshot.",
    )
    parser.add_argument(
        "--extra",
        nargs=argparse.REMAINDER,
        default=[],
        help="Additional arguments forwarded verbatim to the application.",
    )
    return parser.parse_args()


def format_duration(seconds: float) -> str:
    if seconds <= 0:
        raise ValueError("Duration must be positive")
    if math.isclose(seconds, round(seconds)):
        return f"{int(round(seconds))}s"
    return f"{seconds:g}s"


def load_json(path: pathlib.Path) -> Dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError as exc:
        raise SystemExit(f"Config file not found: {path}") from exc
    except json.JSONDecodeError:
        # Not all configs must be JSON, fall back to an empty params map.
        return {}


def run_command(cmd: List[str], capture: bool = False) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(
            cmd,
            check=True,
            capture_output=capture,
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr if exc.stderr else ""
        stdout = exc.stdout if exc.stdout else ""
        msg = [f"Command failed: {' '.join(cmd)}"]
        if stdout:
            msg.append(f"stdout:\n{stdout}")
        if stderr:
            msg.append(f"stderr:\n{stderr}")
        raise SystemExit("\n".join(msg)) from exc


def find_first_json_line(output: str) -> Dict[str, Any]:
    for line in output.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if not stripped.startswith("{") or not stripped.endswith("}"):
            continue
        try:
            return json.loads(stripped)
        except json.JSONDecodeError:
            continue
    raise SystemExit("Unable to locate JSON metrics in application output")


def flatten_dict_search(data: Dict[str, Any], keys: Iterable[str]) -> Optional[float]:
    for key in keys:
        if key in data and isinstance(data[key], (int, float)):
            return float(data[key])
    for value in data.values():
        if isinstance(value, dict):
            found = flatten_dict_search(value, keys)
            if found is not None:
                return found
    return None


def extract_params(config_path: pathlib.Path, config_data: Dict[str, Any]) -> Dict[str, Any]:
    if isinstance(config_data, dict):
        params = config_data.get("params")
        if isinstance(params, dict):
            return params
    sibling = config_path.parent / "params.json"
    if sibling.is_file():
        try:
            with sibling.open("r", encoding="utf-8") as fh:
                loaded = json.load(fh)
                if isinstance(loaded, dict):
                    return loaded
        except json.JSONDecodeError:
            pass
    return {}


def extract_metrics(payload: Dict[str, Any]) -> Dict[str, Any]:
    phases = payload.get("phases", {})
    counters = payload.get("counters", {})

    def accumulate(field: str) -> float:
        total = 0.0
        if isinstance(phases, dict):
            for phase in phases.values():
                if isinstance(phase, dict) and field in phase:
                    value = phase[field]
                    if isinstance(value, (int, float)):
                        total += float(value)
        return total

    p50 = accumulate("p50_ms")
    p95 = accumulate("p95_ms")
    p99 = accumulate("p99_ms")
    stdev = 0.0
    if p95 > p50:
        stdev = (p95 - p50) / 1.645

    def counter_value(names: Iterable[str]) -> int:
        if not isinstance(counters, dict):
            return 0
        for name in names:
            value = counters.get(name)
            if isinstance(value, (int, float)):
                return int(value)
        return 0

    metrics = {
        "p50_frame_ms": p50,
        "p95_frame_ms": p95,
        "p99_frame_ms": p99,
        "stdev_frame_ms": stdev,
        "missed_frames": counter_value(["missed_frames"]),
        "watchdog_trips": counter_value(["watchdog.trips", "watchdog_trips"]),
        "log_drops": counter_value(["log_drops"]),
        "queue_max": counter_value(["worker.queue_max", "worker_pool.queue_max"]),
        "emergency_spawns": counter_value(
            ["worker.emergency_spawns", "worker_pool.emergency_spawns"]
        ),
    }
    return metrics


def compute_objective(metrics: Dict[str, Any], budget_ms: Optional[float]) -> float:
    p99 = metrics.get("p99_frame_ms", 0.0)
    if budget_ms and budget_ms > 0.0:
        return budget_ms / max(p99, 1e-9)
    return 1.0 / (1.0 + max(p99, 0.0))


def evaluate_constraints(metrics: Dict[str, Any], budget_ms: Optional[float]) -> List[str]:
    failures: List[str] = []
    if budget_ms and metrics["p99_frame_ms"] > budget_ms:
        failures.append(
            f"p99_frame_ms {metrics['p99_frame_ms']:.3f} exceeds budget {budget_ms:.3f}"
        )
    if metrics["missed_frames"] > 0:
        failures.append(f"missed_frames={metrics['missed_frames']}")
    if metrics["watchdog_trips"] > 0:
        failures.append(f"watchdog_trips={metrics['watchdog_trips']}")
    if metrics["log_drops"] > 0:
        failures.append(f"log_drops={metrics['log_drops']}")
    if metrics["emergency_spawns"] > 0:
        failures.append(f"emergency_spawns={metrics['emergency_spawns']}")
    return failures


def main() -> None:
    args = parse_args()

    app_path = pathlib.Path(args.app)
    if not app_path.exists():
        raise SystemExit(f"Application not found: {app_path}")

    config_path = pathlib.Path(args.config)
    config_data = load_json(config_path)
    params = extract_params(config_path, config_data)

    budget_ms = None
    if isinstance(config_data, dict):
        budget_ms = flatten_dict_search(config_data, ["frame_budget_ms"])
        if budget_ms is None:
            hz = flatten_dict_search(config_data, ["hz"])
            if hz and hz > 0.0:
                budget_ms = 1000.0 / hz
    if budget_ms is not None and budget_ms <= 0.0:
        budget_ms = None

    extras = list(args.extra or [])

    base_cmd = [str(app_path), "--config", str(config_path)] + extras

    if args.warmup > 0:
        warmup_cmd = base_cmd + ["--run", format_duration(args.warmup)]
        run_command(warmup_cmd, capture=False)

    run_cmd = base_cmd + ["--metrics-json-interval", "--run", format_duration(args.run)]
    completed = run_command(run_cmd, capture=True)

    payload = find_first_json_line(completed.stdout)
    metrics = extract_metrics(payload)
    if budget_ms is not None:
        metrics["frame_budget_ms"] = budget_ms

    objective = compute_objective(metrics, budget_ms)
    failures = evaluate_constraints(metrics, budget_ms)

    result: Dict[str, Any] = {
        "ok": not failures,
        "objective": objective,
        "metrics": metrics,
        "params": params,
        "config_path": str(config_path),
    }
    if failures:
        result["reason"] = "; ".join(failures)

    json.dump(result, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()

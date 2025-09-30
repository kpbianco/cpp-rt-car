#!/usr/bin/env python3
"""Synthetic app used for CI smoke tests of the autotune pipeline."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import random
import sys
from typing import Any, Dict


def parse_duration(text: str) -> float:
    if not text:
        return 0.0
    stripped = text.strip().lower()
    if stripped.endswith("s"):
        stripped = stripped[:-1]
    try:
        return float(stripped)
    except ValueError as exc:
        raise SystemExit(f"Invalid duration: {text!r}") from exc


def load_config(path: pathlib.Path) -> Dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError as exc:
        raise SystemExit(f"Config file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse config JSON: {exc}") from exc


def compute_metrics(params: Dict[str, Any], run_seconds: float) -> Dict[str, Any]:
    threads = int(params.get("threads", 1))
    mode = str(params.get("mode", "balanced"))
    prefetch = int(params.get("prefetch_distance", 0))

    base = {"fast": 3.0, "balanced": 3.6, "safe": 4.2}.get(mode, 3.6)
    improvement = 0.08 * max(threads - 1, 0)
    prefetch_bonus = 0.0005 * prefetch
    seed_material = f"{threads}:{mode}:{prefetch}".encode("utf-8")
    seed = int(hashlib.sha256(seed_material).hexdigest()[:8], 16)
    jitter = random.Random(seed).uniform(-0.05, 0.05)

    p99 = max(base - improvement - prefetch_bonus + jitter, 1.0)
    p95 = max(p99 - 0.15, 0.5)
    p50 = max(p95 - 0.1, 0.25)
    stdev = max((p95 - p50) / 1.645, 0.01)

    queue_max = max(1, 6 - threads)

    scale = max(run_seconds, 0.5) / 2.0
    counters = {
        "missed_frames": 0,
        "watchdog.trips": 0,
        "log_drops": 0,
        "worker.queue_max": queue_max,
        "worker.emergency_spawns": 0,
    }

    phases = {
        "frame": {
            "p50_ms": round(p50 * scale, 3),
            "p95_ms": round(p95 * scale, 3),
            "p99_ms": round(p99 * scale, 3),
        }
    }

    return {
        "ok": True,
        "phases": phases,
        "counters": counters,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Synthetic autotune target")
    parser.add_argument("--config", required=True, help="Path to config JSON")
    parser.add_argument("--run", required=True, help="Duration to sample (e.g. 2s)")
    parser.add_argument("--warmup", default=None, help="Warmup duration")
    parser.add_argument("--metrics-json-interval", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--scenario", default=os.environ.get("SCENARIO", "default"))
    parser.add_argument("extra", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    config_path = pathlib.Path(args.config)
    config = load_config(config_path)
    params = {}
    if isinstance(config, dict):
        params = config.get("params", {}) or {}
        if not isinstance(params, dict):
            params = {}

    run_seconds = parse_duration(args.run)
    metrics_payload = compute_metrics(params, run_seconds)

    budget_ms = None
    if isinstance(config, dict):
        budget_ms = config.get("frame_budget_ms")
        if budget_ms is None:
            budget_ms = config.get("params", {}).get("frame_budget_ms")
        if budget_ms is None:
            hz = config.get("hz")
            if isinstance(hz, (int, float)) and hz > 0:
                budget_ms = 1000.0 / float(hz)

    if args.metrics_json_interval:
        payload: Dict[str, Any] = {
            "scenario": args.scenario,
            "config": str(config_path),
            "phases": metrics_payload["phases"],
            "counters": metrics_payload["counters"],
            "metadata": {
                "params": params,
                "warmup": args.warmup,
                "run": args.run,
            },
        }
        if budget_ms is not None and isinstance(budget_ms, (int, float)) and math.isfinite(float(budget_ms)):
            payload["frame_budget_ms"] = float(budget_ms)

        json.dump(payload, sys.stdout)
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()

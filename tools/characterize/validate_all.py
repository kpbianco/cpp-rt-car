#!/usr/bin/env python3
"""Validate combined characterization artifacts."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys
from typing import Any, Dict, Iterable, List, Mapping, Optional, Tuple


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate characterization reports")
    parser.add_argument(
        "--in",
        dest="input_path",
        required=True,
        help="Directory (or summary.json file) containing run_all.py artifacts.",
    )
    return parser.parse_args()


def load_json(path: pathlib.Path) -> Mapping[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError as exc:
        raise SystemExit(f"JSON file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse JSON {path}: {exc}") from exc


def ensure(condition: bool, message: str, errors: List[str]) -> None:
    if not condition:
        errors.append(message)


def coerce_float(value: Any) -> float:
    if isinstance(value, (int, float)):
        return float(value)
    try:
        return float(str(value))
    except (TypeError, ValueError):
        return float("nan")


def coerce_int(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    try:
        return int(float(str(value)))
    except (TypeError, ValueError):
        return 0


def resolve_summary_path(raw: str) -> Tuple[pathlib.Path, pathlib.Path]:
    summary_candidate = pathlib.Path(raw)
    if summary_candidate.is_dir():
        summary_path = summary_candidate / "summary.json"
        base_dir = summary_candidate
    else:
        summary_path = summary_candidate
        base_dir = summary_candidate.parent
    return base_dir, summary_path


def validate_replay(base_dir: pathlib.Path, summary: Mapping[str, Any], errors: List[str]) -> Mapping[str, Any]:
    artifacts = summary.get("artifacts", {})
    replay_ref = artifacts.get("replay") if isinstance(artifacts, Mapping) else None
    ensure(isinstance(replay_ref, str), "Summary missing replay artifact reference", errors)
    replay_path = base_dir / replay_ref if isinstance(replay_ref, str) else None
    if replay_path is None:
        return {}
    data = load_json(replay_path)
    ensure(bool(data.get("bit_equal")), "Golden replay bit_equal flag is false", errors)
    return data


def validate_safety(base_dir: pathlib.Path, summary: Mapping[str, Any], errors: List[str]) -> Mapping[str, Any]:
    artifacts = summary.get("artifacts", {})
    ref = artifacts.get("rt_safety") if isinstance(artifacts, Mapping) else None
    ensure(isinstance(ref, str), "Summary missing rt_safety artifact reference", errors)
    path = base_dir / ref if isinstance(ref, str) else None
    if path is None:
        return {}
    data = load_json(path)
    summary_block = data.get("summary", {}) if isinstance(data, Mapping) else {}
    deadlocks = coerce_int(summary_block.get("deadlocks"))
    missed = coerce_int(summary_block.get("missed_frames"))
    allowed = coerce_int(summary_block.get("missed_frames_allowed"))
    ensure(deadlocks == 0, f"Safety run detected deadlocks: {deadlocks}", errors)
    ensure(missed <= allowed, f"Safety run missed_frames {missed} exceeds allowance {allowed}", errors)
    return data


def collect_scaling_runs(data: Mapping[str, Any]) -> List[Mapping[str, Any]]:
    runs = data.get("runs")
    if isinstance(runs, list):
        return [run for run in runs if isinstance(run, Mapping)]
    return []


def validate_scaling(base_dir: pathlib.Path, summary: Mapping[str, Any], errors: List[str], warnings: List[str]) -> Mapping[str, Any]:
    artifacts = summary.get("artifacts", {})
    scaling_info = artifacts.get("scaling") if isinstance(artifacts, Mapping) else None
    scaling_json_ref: Optional[str] = None
    if isinstance(scaling_info, Mapping):
        scaling_json_ref = scaling_info.get("json") if isinstance(scaling_info.get("json"), str) else None
    ensure(isinstance(scaling_json_ref, str), "Summary missing scaling JSON artifact reference", errors)
    scaling_path = base_dir / scaling_json_ref if scaling_json_ref else None
    if scaling_path is None:
        return {}
    data = load_json(scaling_path)
    runs = collect_scaling_runs(data)
    ensure(len(runs) >= 3, f"Scaling sweep produced {len(runs)} rows (expected >= 3)", errors)
    for run in runs:
        p99 = coerce_float(run.get("summary", {}).get("p99_frame_ms")) if isinstance(run.get("summary"), Mapping) else float("nan")
        ensure(math.isfinite(p99), "Scaling p99 value is not finite", errors)
    maybe_warn_scaling_monotonicity(runs, warnings)
    return data


def maybe_warn_scaling_monotonicity(runs: Iterable[Mapping[str, Any]], warnings: List[str]) -> None:
    per_mode: Dict[str, Dict[int, float]] = {}
    for run in runs:
        mode = str(run.get("mode", ""))
        summary = run.get("summary") if isinstance(run.get("summary"), Mapping) else {}
        threads = coerce_int(run.get("threads"))
        p99 = coerce_float(summary.get("p99_frame_ms")) if isinstance(summary, Mapping) else float("nan")
        if not math.isfinite(p99) or threads <= 0:
            continue
        per_mode.setdefault(mode, {})[threads] = p99
    for mode, values in per_mode.items():
        if 1 in values and 2 in values:
            if values[2] > values[1]:
                warnings.append(
                    f"Scaling p99 regression: mode {mode} has 2-thread p99 {values[2]:.3f} ms > 1-thread {values[1]:.3f} ms"
                )


def main() -> None:
    args = parse_args()
    base_dir, summary_path = resolve_summary_path(args.input_path)
    summary = load_json(summary_path)

    errors: List[str] = []
    warnings: List[str] = []

    replay = validate_replay(base_dir, summary, errors)
    safety = validate_safety(base_dir, summary, errors)
    scaling = validate_scaling(base_dir, summary, errors, warnings)

    if errors:
        for msg in errors:
            print(f"[ERROR] {msg}")
        raise SystemExit(1)

    for msg in warnings:
        print(f"[WARN] {msg}")

    runs_count = len(collect_scaling_runs(scaling))
    print(
        "[OK] Characterization artifacts validated: "
        f"{base_dir} (runs={runs_count}, bit_equal={bool(replay.get('bit_equal'))})"
    )


if __name__ == "__main__":
    main()

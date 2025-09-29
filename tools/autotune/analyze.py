#!/usr/bin/env python3
"""Summarize autotuning experiment results."""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
from collections import OrderedDict
from dataclasses import dataclass
from statistics import mean
from typing import Any, Dict, Iterable, List, Sequence, Tuple


@dataclass
class Experiment:
    """Container for a single experiment entry."""

    raw: Dict[str, Any]

    @property
    def ok(self) -> bool:
        return bool(self.raw.get("ok"))

    @property
    def objective(self) -> float:
        value = self.raw.get("objective")
        if isinstance(value, (int, float)):
            return float(value)
        raise ValueError("Experiment missing numeric objective")

    @property
    def params(self) -> Dict[str, Any]:
        params = self.raw.get("params", {})
        return params if isinstance(params, dict) else {}

    @property
    def metrics(self) -> Dict[str, Any]:
        metrics = self.raw.get("metrics", {})
        return metrics if isinstance(metrics, dict) else {}

    def pareto_tuple(self) -> Tuple[float, float, float, float]:
        metrics = self.metrics
        queue = _safe_metric(metrics, "queue_max")
        emergency = _safe_metric(metrics, "emergency_spawns")
        drops = _safe_metric(metrics, "log_drops")
        return (self.objective, queue, emergency, drops)


def _safe_metric(metrics: Dict[str, Any], key: str) -> float:
    value = metrics.get(key)
    if isinstance(value, (int, float)) and math.isfinite(value):
        return float(value)
    return math.inf


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize autotune experiments.")
    parser.add_argument("--spec", required=True, type=pathlib.Path, help="Spec YAML path.")
    parser.add_argument(
        "--in",
        dest="in_path",
        required=True,
        type=pathlib.Path,
        help="Input experiments JSONL file.",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        type=pathlib.Path,
        help="Output directory for the generated summaries.",
    )
    return parser.parse_args()


def load_spec(path: pathlib.Path) -> Dict[str, Any]:
    if not path.is_file():
        raise SystemExit(f"Spec file not found: {path}")

    params: "OrderedDict[str, Any]" = OrderedDict()
    inside_params = False
    params_indent = 0
    with path.open("r", encoding="utf-8") as fh:
        for raw_line in fh:
            if not raw_line.strip() or raw_line.lstrip().startswith("#"):
                continue
            indent = len(raw_line) - len(raw_line.lstrip(" "))
            stripped = raw_line.strip()
            if not inside_params:
                if stripped == "params:":
                    inside_params = True
                    params_indent = indent
                continue
            if indent <= params_indent:
                break
            if indent == params_indent + 2 and stripped.endswith(":"):
                name = stripped[:-1].strip()
                if name:
                    params[name] = {}
    return {"params": params}


def load_experiments(path: pathlib.Path) -> List[Experiment]:
    if not path.is_file():
        raise SystemExit(f"Experiments file not found: {path}")
    experiments: List[Experiment] = []
    with path.open("r", encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, start=1):
            stripped = line.strip()
            if not stripped:
                continue
            try:
                payload = json.loads(stripped)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"Invalid JSON on line {lineno}: {exc}") from exc
            if not isinstance(payload, dict):
                raise SystemExit(f"Expected object on line {lineno}")
            experiments.append(Experiment(payload))
    if not experiments:
        raise SystemExit("No experiments found in input")
    return experiments


def compute_best(experiments: Sequence[Experiment]) -> Experiment:
    ok_results = [exp for exp in experiments if exp.ok]
    if not ok_results:
        raise SystemExit("No successful experiments (ok=true) found")
    return min(ok_results, key=lambda exp: exp.objective)


def pareto_frontier(experiments: Iterable[Experiment]) -> List[Experiment]:
    frontier: List[Experiment] = []
    for exp in sorted(experiments, key=lambda e: e.objective):
        candidate = exp.pareto_tuple()
        dominated = False
        new_frontier: List[Experiment] = []
        for current in frontier:
            current_tuple = current.pareto_tuple()
            if dominates(current_tuple, candidate):
                dominated = True
                break
            if not dominates(candidate, current_tuple):
                new_frontier.append(current)
        if not dominated:
            new_frontier.append(exp)
            frontier = new_frontier
    return frontier


def dominates(left: Tuple[float, ...], right: Tuple[float, ...]) -> bool:
    return all(l <= r for l, r in zip(left, right)) and any(l < r for l, r in zip(left, right))


def build_histograms(
    params_spec: Dict[str, Any],
    experiments: Iterable[Experiment],
) -> Dict[str, Dict[str, Dict[str, float]]]:
    histograms: Dict[str, Dict[str, Dict[str, float]]] = {}
    ok_experiments = [exp for exp in experiments if exp.ok]
    for name in params_spec.keys():
        buckets: Dict[str, List[float]] = {}
        for exp in ok_experiments:
            value = exp.params.get(name, "<unset>")
            bucket_key = json.dumps(value, sort_keys=True)
            buckets.setdefault(bucket_key, []).append(exp.objective)
        histograms[name] = {
            bucket: {
                "count": float(len(values)),
                "mean": float(mean(values)) if values else math.nan,
                "min": float(min(values)) if values else math.nan,
                "max": float(max(values)) if values else math.nan,
            }
            for bucket, values in sorted(buckets.items(), key=lambda item: item[0])
        }
    return histograms


def write_best(path: pathlib.Path, best: Experiment) -> None:
    metrics = best.metrics
    excerpt_keys = [
        "p99_frame_ms",
        "p95_frame_ms",
        "p50_frame_ms",
        "stdev_frame_ms",
        "queue_max",
        "emergency_spawns",
        "log_drops",
        "missed_frames",
        "watchdog_trips",
    ]
    excerpt = {key: metrics.get(key) for key in excerpt_keys if key in metrics}
    payload = {
        "objective": best.objective,
        "params": best.params,
        "metrics_excerpt": excerpt,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_pareto(path: pathlib.Path, frontier: Sequence[Experiment]) -> None:
    serializable = []
    for exp in frontier:
        serializable.append(
            {
                "objective": exp.objective,
                "params": exp.params,
                "metrics": {
                    key: exp.metrics.get(key)
                    for key in ["queue_max", "emergency_spawns", "log_drops", "p99_frame_ms"]
                    if key in exp.metrics
                },
            }
        )
    path.write_text(json.dumps(serializable, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_summary_csv(
    path: pathlib.Path,
    params_spec: Dict[str, Any],
    experiments: Sequence[Experiment],
    limit: int = 10,
) -> None:
    columns = ["objective", "ok", "reason"]
    param_names = list(params_spec.keys())
    columns.extend(param_names)
    metric_columns = [
        "p99_frame_ms",
        "p95_frame_ms",
        "p50_frame_ms",
        "stdev_frame_ms",
        "queue_max",
        "emergency_spawns",
        "log_drops",
    ]
    columns.extend(metric_columns)

    ranked = sorted(experiments, key=lambda exp: (not exp.ok, exp.objective))[:limit]

    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=columns)
        writer.writeheader()
        for exp in ranked:
            row: Dict[str, Any] = {
                "objective": exp.objective,
                "ok": exp.ok,
                "reason": exp.raw.get("reason", ""),
            }
            for name in param_names:
                row[name] = exp.params.get(name)
            metrics = exp.metrics
            for metric in metric_columns:
                row[metric] = metrics.get(metric)
            writer.writerow(row)


def write_summary_json(
    path: pathlib.Path,
    best: Experiment,
    pareto: Sequence[Experiment],
    histograms: Dict[str, Dict[str, Dict[str, float]]],
    total_count: int,
    ok_count: int,
) -> None:
    payload = {
        "total_experiments": total_count,
        "ok_experiments": ok_count,
        "best_objective": best.objective,
        "pareto_size": len(pareto),
        "histograms": histograms,
    }
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    spec = load_spec(args.spec)
    params_spec = spec.get("params", {}) if isinstance(spec, dict) else {}
    experiments = load_experiments(args.in_path)

    args.out_dir.mkdir(parents=True, exist_ok=True)

    best = compute_best(experiments)
    pareto = pareto_frontier([exp for exp in experiments if exp.ok])
    histograms = build_histograms(params_spec, experiments)

    write_best(args.out_dir / "best.json", best)
    write_pareto(args.out_dir / "pareto.json", pareto)
    write_summary_csv(args.out_dir / "summary.csv", params_spec, experiments)
    write_summary_json(
        args.out_dir / "summary.json",
        best,
        pareto,
        histograms,
        total_count=len(experiments),
        ok_count=sum(1 for exp in experiments if exp.ok),
    )


if __name__ == "__main__":
    main()

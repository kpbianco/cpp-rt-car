#!/usr/bin/env python3
"""Run one autotune sample and score the result."""

from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import pathlib
import subprocess
import sys
from typing import Any, Dict, Iterable, List, Mapping, Optional


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune.make_config import load_simple_yaml  # noqa: E402
from tools.autotune import common_host  # noqa: E402
from tools.autotune.common_eval import (  # noqa: E402
    OBJECTIVE_ERROR_EXIT_CODE,
    ObjectiveEvaluationError,
    compute_objective,
    evaluate_constraints,
)


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
    parser.add_argument(
        "--scenario",
        default=None,
        help="Scenario name supplied when launching the application.",
    )
    parser.add_argument(
        "--spec",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("spec.yaml"),
        help="Path to the autotune specification describing hard constraints.",
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


def run_command(
    cmd: List[str],
    capture: bool = False,
    *,
    extra_env: Optional[Dict[str, str]] = None,
) -> subprocess.CompletedProcess[str]:
    try:
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            cmd,
            check=True,
            capture_output=capture,
            text=True,
            env=env,
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


def find_last_json_line(output: str) -> Dict[str, Any]:
    for line in reversed(output.splitlines()):
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


def _try_parse_int(value: Any) -> Optional[int]:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and math.isfinite(value):
        integer = int(value)
        if math.isclose(value, integer):
            return integer
    if isinstance(value, str):
        try:
            return int(value.strip())
        except ValueError:
            return None
    return None


def _try_parse_str(value: Any) -> Optional[str]:
    if isinstance(value, str):
        stripped = value.strip()
        if stripped:
            return stripped
    return None


def _find_nested_value(data: Any, keys: Iterable[str], parser) -> Optional[Any]:
    if isinstance(data, dict):
        for key in keys:
            if key in data:
                parsed = parser(data[key])
                if parsed is not None:
                    return parsed
        for value in data.values():
            found = _find_nested_value(value, keys, parser)
            if found is not None:
                return found
    elif isinstance(data, list):
        for item in data:
            found = _find_nested_value(item, keys, parser)
            if found is not None:
                return found
    return None


def extract_seed(config_data: Dict[str, Any], config_path: pathlib.Path) -> int:
    seed = _find_nested_value(config_data, ["seed", "random_seed", "rng_seed"], _try_parse_int)
    if seed is not None:
        return seed
    stem = config_path.stem
    if "_" in stem:
        candidate = stem.rsplit("_", 1)[-1]
        parsed = _try_parse_int(candidate)
        if parsed is not None:
            return parsed
    return -1


def extract_scenario(config_data: Dict[str, Any], config_path: pathlib.Path) -> str:
    scenario = _find_nested_value(config_data, ["scenario", "scenario_name"], _try_parse_str)
    if scenario is not None:
        return scenario
    return config_path.stem or "default"


def collect_env_info() -> Dict[str, Any]:
    tokens = common_host.host_tokens()
    cores = os.cpu_count() or 0
    return {**tokens, "cores": int(cores)}


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


def load_spec(path: pathlib.Path) -> Mapping[str, Any]:
    data = load_simple_yaml(path)
    if not isinstance(data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping at the top level")
    return data


def main() -> None:
    args = parse_args()

    spec_path = pathlib.Path(args.spec)
    if not spec_path.is_file():
        raise SystemExit(f"Spec file not found: {spec_path}")
    spec_data = load_spec(spec_path)
    app_path = pathlib.Path(args.app)
    if not app_path.exists():
        raise SystemExit(f"Application not found: {app_path}")

    config_path = pathlib.Path(args.config)
    config_data = load_json(config_path)
    params = extract_params(config_path, config_data)

    scenario = args.scenario or extract_scenario(config_data, config_path)
    if not scenario:
        scenario = "default"

    scenario_env = {"SCENARIO": scenario}

    budget_ms = None
    app_spec = spec_data.get("app") if isinstance(spec_data, Mapping) else None
    if isinstance(app_spec, Mapping):
        spec_budget = app_spec.get("frame_budget_ms")
        if isinstance(spec_budget, (int, float)) and spec_budget > 0.0:
            budget_ms = float(spec_budget)
    config_budget = None
    if isinstance(config_data, dict):
        config_budget = flatten_dict_search(config_data, ["frame_budget_ms"])
        if config_budget is None:
            hz = flatten_dict_search(config_data, ["hz"])
            if hz and hz > 0.0:
                config_budget = 1000.0 / hz
    if config_budget is not None and config_budget <= 0.0:
        config_budget = None
    if config_budget is not None:
        budget_ms = config_budget

    extras = list(args.extra or [])

    base_cmd = [str(app_path), "--config", str(config_path)] + extras

    if args.warmup > 0:
        warmup_cmd = base_cmd + ["--run", format_duration(args.warmup)]
        run_command(warmup_cmd, capture=False, extra_env=scenario_env)

    run_cmd = base_cmd + ["--metrics-json-interval", "--run", format_duration(args.run)]
    completed = run_command(run_cmd, capture=True, extra_env=scenario_env)

    payload = find_last_json_line(completed.stdout)
    metrics_summary = extract_metrics(payload)
    if budget_ms is not None:
        metrics_summary["frame_budget_ms"] = budget_ms

    constraint_metrics: Dict[str, Any] = {}
    if isinstance(payload, dict):
        constraint_metrics.update(payload)
    constraint_metrics.update(metrics_summary)

    ok, failures = evaluate_constraints(spec_data, constraint_metrics)
    try:
        objective = compute_objective(spec_data, constraint_metrics)
    except ObjectiveEvaluationError as exc:
        print(str(exc), file=sys.stderr)
        raise SystemExit(OBJECTIVE_ERROR_EXIT_CODE) from exc
    failure_messages = [details["message"] for details in failures.values()]
    if failures:
        objective = math.inf

    result: Dict[str, Any] = {
        "ok": ok,
        "objective": objective,
        "metrics": payload,
        "_summary": metrics_summary,
        "_params": params,
        "_seed": extract_seed(config_data, config_path),
        "_scenario": scenario,
        "_ts": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "env": collect_env_info(),
        "_schema": "v1",
    }
    if failure_messages:
        result["reason"] = "; ".join(failure_messages)
        result["constraint_failures"] = failures

    json.dump(result, sys.stdout)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()

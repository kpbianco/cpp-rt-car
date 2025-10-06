#!/usr/bin/env python3
"""Robustness validation for top autotune configurations."""

from __future__ import annotations

import argparse
import copy
import datetime
import json
import math
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Optional, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune.make_config import (  # noqa: E402
    build_config,
    load_simple_yaml,
    load_spec as load_param_spec,
    set_path,
    validate_params,
    write_json,
)
from tools.autotune.optimize import (  # noqa: E402
    AppSpec,
    ObjectiveSpec,
    parse_app_spec,
    parse_objective_spec,
)

RUN_ONE = REPO_ROOT / "tools" / "autotune" / "run_one.py"


def get_metrics_summary(payload: Mapping[str, Any]) -> Mapping[str, Any]:
    summary = payload.get("_summary")
    if isinstance(summary, Mapping):
        return summary
    metrics = payload.get("metrics")
    if isinstance(metrics, Mapping):
        return metrics
    return {}


class ExpressionEvaluator:
    def __init__(self, expression: str, *, label: str) -> None:
        self._code = compile(expression, label, "eval")

    def __call__(self, variables: Mapping[str, Any]) -> float:
        safe_globals = {"__builtins__": {}}
        safe_locals = {**variables, "min": min, "max": max, "abs": abs, "math": math}
        result = eval(self._code, safe_globals, safe_locals)
        if isinstance(result, (int, float)):
            return float(result)
        raise ValueError(f"Expression must evaluate to numeric result, got {result!r}")


class ObjectiveEvaluator:
    def __init__(self, spec: ObjectiveSpec, frame_budget_ms: Optional[float]) -> None:
        self.spec = spec
        self.frame_budget_ms = frame_budget_ms
        self._objective = ExpressionEvaluator(spec.expression, label="<objective>")
        self._tiebreakers = [
            ExpressionEvaluator(expr, label=f"<objective_tiebreaker_{index}>")
            for index, expr in enumerate(spec.tiebreakers)
        ]

    def _context(self, metrics: Mapping[str, Any]) -> Dict[str, Any]:
        context: Dict[str, Any] = dict(metrics)
        if (
            self.frame_budget_ms is not None
            and "frame_budget_ms" not in context
            and isinstance(self.frame_budget_ms, (int, float))
        ):
            context["frame_budget_ms"] = float(self.frame_budget_ms)
        return context

    def evaluate(self, metrics: Mapping[str, Any]) -> Tuple[float, List[float]]:
        context = self._context(metrics)
        objective_value = self._objective(context)
        tiebreakers = [evaluator(context) for evaluator in self._tiebreakers]
        return objective_value, tiebreakers


class ConstraintEvaluator:
    def __init__(self, metric: str, expression: str) -> None:
        self.metric = metric
        prepared = expression.strip()
        if not prepared:
            raise SystemExit(
                f"Constraint expression for metric '{metric}' must be a non-empty string"
            )
        if prepared[0] in "=<>":
            prepared = f"value {prepared}"
        self.expression = prepared
        self._code = compile(prepared, f"<constraint {metric}>", "eval")

    def evaluate(self, metrics: Mapping[str, Any], frame_budget_ms: Optional[float]) -> bool:
        if not isinstance(metrics, Mapping):
            return False
        if self.metric not in metrics:
            return False
        value = metrics[self.metric]
        try:
            numeric_value = float(value)
        except (TypeError, ValueError):
            return False
        context: Dict[str, Any] = dict(metrics)
        context[self.metric] = numeric_value
        context["value"] = numeric_value
        if (
            frame_budget_ms is not None
            and "frame_budget_ms" not in context
            and isinstance(frame_budget_ms, (int, float))
        ):
            context["frame_budget_ms"] = float(frame_budget_ms)
        safe_globals = {"__builtins__": {}}
        safe_locals = {**context, "min": min, "max": max, "abs": abs, "math": math}
        try:
            result = eval(self._code, safe_globals, safe_locals)
        except Exception:
            return False
        return bool(result)


def parse_hard_constraints(spec_data: Mapping[str, Any]) -> Tuple[ConstraintEvaluator, ...]:
    metrics_payload = ensure_mapping(spec_data.get("metrics"), "metrics")
    raw_constraints = metrics_payload.get("hard_constraints")
    if raw_constraints is None:
        return ()
    if not isinstance(raw_constraints, Mapping):
        raise SystemExit("metrics.hard_constraints must be a mapping if provided")
    constraints: List[ConstraintEvaluator] = []
    for name, expr in raw_constraints.items():
        if not isinstance(name, str) or not name:
            raise SystemExit("Constraint metric names must be non-empty strings")
        if not isinstance(expr, str):
            raise SystemExit(
                f"Constraint expression for metric '{name}' must be a string"
            )
        constraints.append(ConstraintEvaluator(name, expr))
    return tuple(constraints)


def recompute_run_objective(
    run: Mapping[str, Any],
    objective: ObjectiveEvaluator,
    constraints: Sequence[ConstraintEvaluator],
) -> Optional[float]:
    metrics = run.get("metrics")
    if not isinstance(metrics, Mapping):
        return None
    try:
        objective_value, _ = objective.evaluate(metrics)
    except (NameError, ValueError, TypeError, ZeroDivisionError, OverflowError):
        return math.inf
    frame_budget_ms = getattr(objective, "frame_budget_ms", None)
    for constraint in constraints:
        try:
            ok = constraint.evaluate(metrics, frame_budget_ms)
        except Exception:
            ok = False
        if not ok:
            return math.inf
    return float(objective_value)


@dataclass(frozen=True)
class ScenarioSpec:
    name: str
    extra_args: Tuple[str, ...] = ()
    env: Optional[Mapping[str, str]] = None
    config_overrides: Optional[Mapping[str, Any]] = None
    warmup: Optional[float] = None
    run: Optional[float] = None

    def __post_init__(self) -> None:
        object.__setattr__(self, "extra_args", tuple(self.extra_args))
        env = {} if self.env is None else dict(self.env)
        overrides = {} if self.config_overrides is None else dict(self.config_overrides)
        object.__setattr__(self, "env", env)
        object.__setattr__(self, "config_overrides", overrides)


@dataclass(frozen=True)
class RobustnessSpec:
    seeds: Tuple[Optional[int], ...]
    scenarios: Tuple[ScenarioSpec, ...]
    seed_env: Optional[str]
    seed_arg: Optional[str]
    config_seed_path: Optional[Tuple[str, ...]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate autotune candidates for robustness")
    parser.add_argument("--spec", required=True, type=pathlib.Path)
    parser.add_argument("--candidates-json", required=True, type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    return parser.parse_args()


def ensure_mapping(value: Any, label: str) -> Mapping[str, Any]:
    if value is None:
        return {}
    if isinstance(value, Mapping):
        return value
    raise SystemExit(f"{label} must be a mapping")


def parse_seed_list(payload: Any) -> Tuple[Optional[int], ...]:
    if payload is None:
        return (None,)
    if isinstance(payload, Sequence) and not isinstance(payload, (str, bytes)):
        seeds: List[Optional[int]] = []
        for item in payload:
            if item is None:
                seeds.append(None)
                continue
            try:
                seeds.append(int(item))
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"Robustness seeds must be integers, got {item!r}") from exc
        if not seeds:
            return (None,)
        return tuple(seeds)
    raise SystemExit("robustness.seeds must be a sequence of integers")


def parse_path(payload: Any) -> Optional[Tuple[str, ...]]:
    if payload is None:
        return None
    if isinstance(payload, str):
        parts = [part.strip() for part in payload.split(".") if part.strip()]
    elif isinstance(payload, Sequence) and not isinstance(payload, (str, bytes)):
        parts = []
        for item in payload:
            if not isinstance(item, str) or not item:
                raise SystemExit("config_seed_path entries must be non-empty strings")
            parts.append(item)
    else:
        raise SystemExit("config_seed_path must be a dotted string or sequence of strings")
    if not parts:
        return None
    return tuple(parts)


def parse_scenarios(payload: Any) -> Tuple[ScenarioSpec, ...]:
    if payload is None:
        return (ScenarioSpec(name="default"),)
    if not isinstance(payload, Sequence) or isinstance(payload, (str, bytes)):
        raise SystemExit("robustness.scenarios must be a sequence")

    scenarios: List[ScenarioSpec] = []
    for index, entry in enumerate(payload):
        if isinstance(entry, str):
            name = entry.strip()
            if not name:
                raise SystemExit("Scenario names must be non-empty strings")
            scenarios.append(ScenarioSpec(name=name))
            continue
        if not isinstance(entry, Mapping):
            raise SystemExit("robustness.scenarios entries must be mappings or strings")
        name = entry.get("name")
        if not isinstance(name, str) or not name:
            name = f"scenario_{index}"
        extra_raw = entry.get("extra_args", [])
        if extra_raw is None:
            extra_args: Tuple[str, ...] = ()
        elif isinstance(extra_raw, Sequence) and not isinstance(extra_raw, (str, bytes)):
            extra_args = tuple(str(item) for item in extra_raw)
        else:
            raise SystemExit(f"Scenario '{name}' extra_args must be a sequence if provided")
        env_raw = ensure_mapping(entry.get("env"), f"scenario '{name}' env")
        env = {str(k): str(v) for k, v in env_raw.items()}
        overrides_raw = ensure_mapping(entry.get("config_overrides"), f"scenario '{name}' config_overrides")
        warmup = entry.get("warmup_sec")
        run = entry.get("run_sec")
        warmup_value: Optional[float]
        run_value: Optional[float]
        if warmup is not None:
            try:
                warmup_value = float(warmup)
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"Scenario '{name}' warmup_sec must be numeric") from exc
        else:
            warmup_value = None
        if run is not None:
            try:
                run_value = float(run)
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"Scenario '{name}' run_sec must be numeric") from exc
        else:
            run_value = None
        scenarios.append(
            ScenarioSpec(
                name=name,
                extra_args=extra_args,
                env=env,
                config_overrides=overrides_raw,
                warmup=warmup_value,
                run=run_value,
            )
        )
    if not scenarios:
        return (ScenarioSpec(name="default"),)
    return tuple(scenarios)


def _resolve_robustness_scenarios(
    payload: Any, lookup: Mapping[str, ScenarioSpec]
) -> Tuple[ScenarioSpec, ...]:
    if payload is None:
        return parse_scenarios(None)
    if not isinstance(payload, Sequence) or isinstance(payload, (str, bytes)):
        raise SystemExit("robustness.scenarios must be a sequence")

    resolved: List[ScenarioSpec] = []
    for entry in payload:
        if isinstance(entry, str):
            name = entry.strip()
            if not name:
                raise SystemExit("Scenario names must be non-empty strings")
            scenario = lookup.get(name)
            if scenario is None:
                scenario = ScenarioSpec(name=name)
            resolved.append(scenario)
            continue
        if not isinstance(entry, Mapping):
            raise SystemExit("robustness.scenarios entries must be mappings or strings")
        parsed = parse_scenarios([entry])
        resolved.extend(parsed)

    if not resolved:
        return (ScenarioSpec(name="default"),)
    return tuple(resolved)


def parse_robustness(spec_data: Mapping[str, Any]) -> RobustnessSpec:
    payload = spec_data.get("robustness")
    top_level_scenarios = parse_scenarios(spec_data.get("scenarios"))
    scenario_lookup = {scenario.name: scenario for scenario in top_level_scenarios}
    if payload is None:
        return RobustnessSpec(
            seeds=(None,),
            scenarios=top_level_scenarios,
            seed_env=None,
            seed_arg=None,
            config_seed_path=None,
        )
    if not isinstance(payload, Mapping):
        raise SystemExit("Spec 'robustness' must be a mapping")
    seeds = parse_seed_list(payload.get("seeds"))
    seed_env = payload.get("seed_env")
    if seed_env is not None and (not isinstance(seed_env, str) or not seed_env):
        raise SystemExit("robustness.seed_env must be a non-empty string if provided")
    seed_arg = payload.get("seed_arg")
    if seed_arg is not None and (not isinstance(seed_arg, str) or not seed_arg):
        raise SystemExit("robustness.seed_arg must be a non-empty string if provided")
    config_seed_path = parse_path(payload.get("config_seed_path"))
    scenarios = _resolve_robustness_scenarios(payload.get("scenarios"), scenario_lookup)
    return RobustnessSpec(
        seeds=seeds,
        scenarios=scenarios,
        seed_env=seed_env,
        seed_arg=seed_arg,
        config_seed_path=config_seed_path,
    )


def load_candidates(path: pathlib.Path) -> List[Mapping[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError as exc:
        raise SystemExit(f"Candidates file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse candidates JSON: {exc}") from exc

    if isinstance(data, Mapping):
        candidates = data.get("candidates")
        if candidates is None:
            raise SystemExit("Candidates JSON missing 'candidates' list")
    else:
        candidates = data
    if not isinstance(candidates, Sequence) or isinstance(candidates, (str, bytes)):
        raise SystemExit("Candidates payload must be a sequence")
    parsed: List[Mapping[str, Any]] = []
    for entry in candidates:
        if not isinstance(entry, Mapping):
            raise SystemExit("Each candidate entry must be an object")
        parsed.append(entry)
    if not parsed:
        raise SystemExit("No candidates provided")
    return parsed


def candidate_params(entry: Mapping[str, Any]) -> Mapping[str, Any]:
    params = entry.get("params")
    if params is None:
        params = entry
    if not isinstance(params, Mapping):
        raise SystemExit("Candidate entry missing 'params' mapping")
    return params


def apply_placeholders(value: Any, seed: Optional[int]) -> Any:
    if seed is None:
        return copy.deepcopy(value)
    if isinstance(value, str):
        return value.replace("{seed}", str(seed))
    if isinstance(value, Mapping):
        return {k: apply_placeholders(v, seed) for k, v in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        return [apply_placeholders(item, seed) for item in value]
    return copy.deepcopy(value)


def merge_overrides(target: MutableMapping[str, Any], overrides: Mapping[str, Any]) -> None:
    for key, value in overrides.items():
        if isinstance(value, Mapping):
            if isinstance(key, str) and "." in key and key not in target:
                set_path(target, key.split("."), copy.deepcopy(value))
                continue
            existing = target.get(key)
            if isinstance(existing, MutableMapping):
                merge_overrides(existing, value)
            else:
                target[key] = copy.deepcopy(value)
            continue
        if isinstance(key, str) and "." in key:
            set_path(target, key.split("."), value)
        else:
            target[key] = value


def run_validation(
    app: AppSpec,
    robustness: RobustnessSpec,
    params: Mapping[str, Any],
    candidate_meta: Mapping[str, Any],
) -> Tuple[List[Dict[str, Any]], List[Dict[str, Any]]]:
    base_config = build_config(params)
    runs: List[Dict[str, Any]] = []
    raw_records: List[Dict[str, Any]] = []
    label = candidate_meta.get("id")
    if label is None:
        label = candidate_meta.get("rank")
    if label is None:
        label = candidate_meta.get("name")

    with tempfile.TemporaryDirectory(prefix="autotune_validate_") as tmpdir_str:
        tmpdir = pathlib.Path(tmpdir_str)
        for scenario_index, scenario in enumerate(robustness.scenarios):
            for seed in robustness.seeds:
                config_data = copy.deepcopy(base_config)
                if robustness.config_seed_path and seed is not None:
                    set_path(config_data, robustness.config_seed_path, seed)
                if scenario.config_overrides:
                    overrides = apply_placeholders(scenario.config_overrides, seed)
                    merge_overrides(config_data, overrides)
                seed_suffix = "none" if seed is None else str(seed)
                config_name = f"config_{scenario_index}_{seed_suffix}.json"
                config_path = tmpdir / config_name
                write_json(config_path, config_data)

                warmup = scenario.warmup if scenario.warmup is not None else app.warmup
                run_duration = scenario.run if scenario.run is not None else app.run

                extras: List[str] = list(app.extra_args)
                if robustness.seed_arg and seed is not None:
                    extras.extend([robustness.seed_arg, str(seed)])
                scenario_extras = apply_placeholders(list(scenario.extra_args), seed)
                extras.extend(str(item) for item in scenario_extras)

                scenario_name = scenario.name

                cmd = [
                    sys.executable,
                    str(RUN_ONE),
                    "--app",
                    str(app.path),
                    "--config",
                    str(config_path),
                    "--warmup",
                    str(warmup),
                    "--run",
                    str(run_duration),
                ]
                if scenario_name:
                    cmd.extend(["--scenario", scenario_name])
                if extras:
                    cmd.append("--extra")
                    cmd.extend(extras)

                env = os.environ.copy()
                if robustness.seed_env and seed is not None:
                    env[robustness.seed_env] = str(seed)
                scenario_env = apply_placeholders(dict(scenario.env), seed)
                if "SCENARIO" not in scenario_env and scenario_name:
                    scenario_env["SCENARIO"] = scenario_name
                for key, value in scenario_env.items():
                    env[str(key)] = str(value)
                if scenario_name:
                    env.setdefault("SCENARIO", scenario_name)

                try:
                    completed = subprocess.run(
                        cmd,
                        check=True,
                        capture_output=True,
                        text=True,
                        env=env,
                    )
                except subprocess.CalledProcessError as exc:
                    stdout = exc.stdout.strip() if exc.stdout else ""
                    stderr = exc.stderr.strip() if exc.stderr else ""
                    reason_parts = ["run_one execution failed"]
                    if stdout:
                        reason_parts.append(f"stdout: {stdout}")
                    if stderr:
                        reason_parts.append(f"stderr: {stderr}")
                    payload: Dict[str, Any] = {
                        "ok": False,
                        "objective": None,
                        "metrics": {},
                        "reason": "; ".join(reason_parts),
                    }
                else:
                    stdout = completed.stdout.strip()
                    line = stdout.splitlines()[-1] if stdout else ""
                    try:
                        payload = json.loads(line)
                    except json.JSONDecodeError:
                        payload = {
                            "ok": False,
                            "objective": None,
                            "metrics": {},
                            "reason": f"Failed to parse JSON output: {stdout}",
                        }

                metrics = get_metrics_summary(payload)
                objective_value = payload.get("objective")
                if isinstance(objective_value, (int, float)) and not math.isnan(objective_value):
                    objective = float(objective_value)
                else:
                    objective = None
                ok = bool(payload.get("ok", False))
                reason = payload.get("reason")

                run_record = {
                    "candidate_label": label,
                    "seed": seed,
                    "scenario": scenario.name,
                    "_scenario": scenario_name,
                    "ok": ok,
                    "objective": objective,
                    "metrics": metrics,
                    "reason": reason,
                    "params": params,
                    "extras": extras,
                }
                runs.append(run_record)

                tracked_env_keys: Iterable[str]
                env_keys = set(scenario_env.keys())
                if scenario_name:
                    env_keys.add("SCENARIO")
                if robustness.seed_env:
                    env_keys.add(robustness.seed_env)
                tracked_env_keys = sorted(env_keys)

                raw_record = {
                    "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "command": cmd,
                    "environment": {key: env[key] for key in tracked_env_keys if key in env},
                    "result": payload,
                    "candidate": {
                        "label": label,
                        "metadata": {k: v for k, v in candidate_meta.items() if k != "params"},
                        "params": params,
                    },
                    "scenario": {
                        "name": scenario.name,
                        "index": scenario_index,
                    },
                    "seed": seed,
                    "_scenario": scenario_name,
                }
                raw_records.append(raw_record)
    return runs, raw_records


def quantile_range(values: Sequence[float]) -> Tuple[Optional[float], Optional[float], Optional[float]]:
    if not values:
        return None, None, None
    if len(values) == 1:
        value = float(values[0])
        return value, value, 0.0
    q = statistics.quantiles(values, n=4, method="inclusive")
    q1 = float(q[0])
    q3 = float(q[2])
    iqr = q3 - q1
    return q1, q3, iqr


def summarise_candidate(
    index: int,
    entry: Mapping[str, Any],
    params: Mapping[str, Any],
    runs: Sequence[Mapping[str, Any]],
    objective: ObjectiveEvaluator,
    constraints: Sequence[ConstraintEvaluator],
) -> Dict[str, Any]:
    total_runs = len(runs)
    failures = sum(1 for run in runs if not run.get("ok", False))
    fail_rate = float(failures / total_runs) if total_runs else 1.0

    objectives: List[float] = []
    for run in runs:
        recomputed = recompute_run_objective(run, objective, constraints)
        if isinstance(run, MutableMapping):
            run["computed_objective"] = recomputed
        if isinstance(recomputed, (int, float)) and not math.isnan(float(recomputed)):
            objectives.append(float(recomputed))
    median_objective = float(statistics.median(objectives)) if objectives else None
    _, _, iqr_objective = quantile_range(objectives)

    metrics_samples: Dict[str, List[float]] = {}
    for run in runs:
        metrics = run.get("metrics")
        if not isinstance(metrics, Mapping):
            continue
        for key, value in metrics.items():
            if isinstance(value, (int, float)) and not math.isnan(float(value)):
                metrics_samples.setdefault(key, []).append(float(value))

    metrics_summary: Dict[str, Any] = {}
    for name, samples in sorted(metrics_samples.items()):
        if not samples:
            continue
        median_value = float(statistics.median(samples))
        p25, p75, iqr = quantile_range(samples)
        metrics_summary[name] = {
            "median": median_value,
            "p25": p25,
            "p75": p75,
            "iqr": iqr,
        }

    metadata = {k: v for k, v in entry.items() if k != "params"}

    summary = {
        "candidate_index": index,
        "params": params,
        "metadata": metadata,
        "num_runs": total_runs,
        "num_failures": failures,
        "fail_rate": fail_rate,
        "median_objective": median_objective,
        "iqr_objective": iqr_objective,
        "metrics": metrics_summary,
    }
    return summary


def select_best(
    summaries: Sequence[Mapping[str, Any]], maximize: bool
) -> Optional[Mapping[str, Any]]:
    if not summaries:
        return None

    def sort_key(summary: Mapping[str, Any]) -> Tuple[float, float, float, int]:
        fail_rate = float(summary.get("fail_rate", 1.0))
        median = summary.get("median_objective")
        if isinstance(median, (int, float)):
            median_value = float(median)
        else:
            median_value = math.inf if not maximize else -math.inf
        if maximize:
            median_value = -median_value
        iqr = summary.get("iqr_objective")
        iqr_value = float(iqr) if isinstance(iqr, (int, float)) else math.inf
        index = int(summary.get("candidate_index", 0))
        return (fail_rate, median_value, iqr_value, index)

    return min(summaries, key=sort_key)


def append_experiments_log(path: pathlib.Path, records: Sequence[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        for record in records:
            fh.write(json.dumps(record))
            fh.write("\n")
            fh.flush()
            os.fsync(fh.fileno())


def write_candidate_summaries(path: pathlib.Path, summaries: Sequence[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        for summary in summaries:
            fh.write(json.dumps(summary))
            fh.write("\n")


def write_validated_summary(
    path: pathlib.Path,
    spec_path: pathlib.Path,
    candidates_path: pathlib.Path,
    summaries: Sequence[Mapping[str, Any]],
    best: Optional[Mapping[str, Any]],
) -> None:
    payload = {
        "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "spec": str(spec_path),
        "candidates_source": str(candidates_path),
        "candidates": list(summaries),
        "best": best,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")


def main() -> None:
    args = parse_args()

    spec_data = load_simple_yaml(args.spec)
    if not isinstance(spec_data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping at the top level")
    app_spec = parse_app_spec(spec_data, args.spec.parent)
    robustness_spec = parse_robustness(spec_data)
    objective_spec = parse_objective_spec(spec_data)
    objective_evaluator = ObjectiveEvaluator(objective_spec, app_spec.frame_budget_ms)
    hard_constraints = parse_hard_constraints(spec_data)

    param_specs = load_param_spec(args.spec)

    candidate_entries = load_candidates(args.candidates_json)

    summaries: List[Dict[str, Any]] = []
    experiments_records: List[Dict[str, Any]] = []

    for index, entry in enumerate(candidate_entries):
        params_raw = candidate_params(entry)
        validated = validate_params(params_raw, param_specs)
        runs, raw_records = run_validation(app_spec, robustness_spec, validated, entry)
        summary = summarise_candidate(
            index,
            entry,
            validated,
            runs,
            objective_evaluator,
            hard_constraints,
        )
        summaries.append(summary)
        experiments_records.extend(raw_records)

    best = select_best(summaries, objective_spec.maximize)

    write_candidate_summaries(args.out, summaries)
    append_experiments_log(pathlib.Path("results") / "experiments.jsonl", experiments_records)
    write_validated_summary(pathlib.Path("results") / "validated.json", args.spec, args.candidates_json, summaries, best)


if __name__ == "__main__":
    main()

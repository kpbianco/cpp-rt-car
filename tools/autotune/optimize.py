#!/usr/bin/env python3
"""Local coordinate descent optimiser for autotune parameters."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from typing import Any, Dict, List, Mapping, MutableMapping, Optional, Sequence, Tuple


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune.make_config import (  # noqa: E402
    ParamSpec,
    build_config,
    load_simple_yaml,
    load_spec as load_param_spec,
    validate_params,
    write_json,
)


@dataclass(frozen=True)
class ObjectiveSpec:
    expression: str
    tiebreakers: Tuple[str, ...]
    maximize: bool


@dataclass(frozen=True)
class AppSpec:
    path: pathlib.Path
    warmup: float
    run: float
    extra_args: Tuple[str, ...]
    frame_budget_ms: Optional[float]


@dataclass
class EvaluationResult:
    params: Dict[str, Any]
    ok: bool
    raw_objective: float
    score_key: Tuple[float, ...]
    detail: MutableMapping[str, Any]

    def summary(self) -> Mapping[str, Any]:
        payload: Dict[str, Any] = {
            "params": dict(self.params),
            "score": self.raw_objective,
            "ok": bool(self.ok),
        }
        reason = self.detail.get("reason")
        if reason:
            payload["reason"] = reason
        return payload


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Coordinate descent autotune optimiser")
    parser.add_argument("--spec", required=True, type=pathlib.Path)
    parser.add_argument("--start-json", required=True)
    parser.add_argument("--iters", required=True, type=int)
    parser.add_argument("--replicates", required=True, type=int)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    return parser.parse_args()


def ensure_mapping(value: Any, label: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise SystemExit(f"Spec YAML missing '{label}' mapping")
    return value


def parse_app_spec(data: Mapping[str, Any], base_dir: pathlib.Path) -> AppSpec:
    app_payload = ensure_mapping(data.get("app"), "app")

    path_value = app_payload.get("path")
    if not isinstance(path_value, str) or not path_value:
        raise SystemExit("Spec 'app.path' must be a non-empty string")
    path = (base_dir / path_value).resolve()

    def parse_float(value: Any, field: str, default: float = 0.0) -> float:
        if value is None:
            return default
        try:
            return float(value)
        except (TypeError, ValueError) as exc:
            raise SystemExit(f"Spec 'app.{field}' must be a number") from exc

    warmup = parse_float(app_payload.get("warmup_sec"), "warmup_sec", default=0.0)
    run = parse_float(app_payload.get("run_sec"), "run_sec")
    if run <= 0.0:
        raise SystemExit("Spec 'app.run_sec' must be positive")

    extra_raw = app_payload.get("extra_args", [])
    if not isinstance(extra_raw, Sequence):
        raise SystemExit("Spec 'app.extra_args' must be a sequence if provided")
    extra_args: Tuple[str, ...] = tuple(str(item) for item in extra_raw)

    budget_value = app_payload.get("frame_budget_ms")
    frame_budget_ms = None
    if budget_value is not None:
        try:
            frame_budget_ms = float(budget_value)
        except (TypeError, ValueError) as exc:
            raise SystemExit("Spec 'app.frame_budget_ms' must be numeric") from exc

    return AppSpec(
        path=path,
        warmup=warmup,
        run=run,
        extra_args=extra_args,
        frame_budget_ms=frame_budget_ms,
    )


def parse_objective_spec(data: Mapping[str, Any]) -> ObjectiveSpec:
    metrics_payload = ensure_mapping(data.get("metrics"), "metrics")
    objective_payload = ensure_mapping(metrics_payload.get("objective"), "metrics.objective")

    expr = objective_payload.get("expr")
    if not isinstance(expr, str) or not expr.strip():
        raise SystemExit("Objective 'expr' must be a non-empty string")

    tiebreakers_raw = objective_payload.get("tiebreakers", [])
    if tiebreakers_raw is None:
        tiebreakers_raw = []
    if not isinstance(tiebreakers_raw, Sequence):
        raise SystemExit("Objective 'tiebreakers' must be a sequence if provided")
    tiebreakers: Tuple[str, ...] = tuple(str(item) for item in tiebreakers_raw)

    maximize = bool(objective_payload.get("maximize", False))

    return ObjectiveSpec(expression=str(expr), tiebreakers=tiebreakers, maximize=maximize)


def load_params_payload(spec_path: pathlib.Path) -> Mapping[str, ParamSpec]:
    return load_param_spec(spec_path)


def decode_start_params(payload: str) -> Dict[str, Any]:
    try:
        parsed = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse --start-json payload: {exc}") from exc
    if not isinstance(parsed, dict):
        raise SystemExit("--start-json must decode to an object")
    return parsed


def canonical_key(params: Mapping[str, Any]) -> Tuple[Tuple[str, Any], ...]:
    return tuple(sorted(params.items()))


def decimal_places(step: float) -> int:
    text = f"{step:.12f}".rstrip("0").rstrip(".")
    if "." in text:
        return min(len(text.split(".", 1)[1]), 12)
    return 0


class ParamDomain:
    def __init__(self, spec: ParamSpec) -> None:
        self.spec = spec
        self.values: Optional[Tuple[Any, ...]] = None
        if spec.allowed_values is not None:
            self.values = tuple(spec.allowed_values)

    def _numeric_range(self) -> Tuple[float, float, Optional[float]]:
        numeric = self.spec.numeric
        if numeric is None:
            raise SystemExit(f"Parameter '{self.spec.name}' lacks numeric bounds")
        step = numeric.step
        if step is None:
            span = numeric.maximum - numeric.minimum
            if span <= 0:
                step = 1.0
            else:
                step = span / 10.0
        return numeric.minimum, numeric.maximum, step

    def _align_numeric(self, value: float) -> float:
        minimum, maximum, step = self._numeric_range()
        clamped = min(max(value, minimum), maximum)
        if step is None:
            return clamped
        steps = round((clamped - minimum) / step)
        aligned = minimum + steps * step
        if aligned < minimum:
            aligned = minimum
        if aligned > maximum:
            aligned = maximum
        if self.spec.kind == "int":
            return float(int(round(aligned)))
        places = decimal_places(step)
        return round(aligned, places)

    def neighbors(self, current: Any) -> List[Any]:
        if self.spec.kind == "bool":
            return [not bool(current)]

        if self.values is not None:
            try:
                idx = self.values.index(current)
            except ValueError as exc:
                raise SystemExit(
                    f"Current value '{current}' for parameter '{self.spec.name}' not in allowed set"
                ) from exc
            candidates: List[Any] = []
            if idx > 0:
                candidates.append(self.values[idx - 1])
            if idx < len(self.values) - 1:
                candidates.append(self.values[idx + 1])
            # For categorical parameters without natural ordering fall back to all others.
            if self.spec.kind == "categorical" and len(candidates) < len(self.values) - 1:
                for value in self.values:
                    if value == current or value in candidates:
                        continue
                    candidates.append(value)
            return candidates

        minimum, maximum, step = self._numeric_range()
        if self.spec.kind == "int":
            current_val = int(current)
        else:
            current_val = float(current)

        candidates: List[Any] = []
        for direction in (-1.0, 1.0):
            candidate = current_val + direction * (step if step is not None else 1.0)
            aligned = self._align_numeric(candidate)
            if self.spec.kind == "int":
                aligned_value: Any = int(round(aligned))
            else:
                aligned_value = aligned
            if minimum <= float(aligned) <= maximum and aligned_value != current:
                if aligned_value not in candidates:
                    candidates.append(aligned_value)
        return candidates


class ExpressionEvaluator:
    def __init__(self, expression: str) -> None:
        self._code = compile(expression, "<objective>", "eval")

    def __call__(self, variables: Mapping[str, Any]) -> float:
        safe_globals = {"__builtins__": {}}
        safe_locals = {**variables, "min": min, "max": max, "abs": abs, "math": math}
        result = eval(self._code, safe_globals, safe_locals)
        if isinstance(result, (int, float)):
            return float(result)
        raise SystemExit(f"Objective expression evaluated to non-numeric result: {result!r}")


class Evaluator:
    def __init__(
        self,
        app: AppSpec,
        objective: ObjectiveSpec,
        replicates: int,
    ) -> None:
        self.app = app
        self.objective = objective
        self.replicates = max(1, replicates)
        self.run_one = REPO_ROOT / "tools" / "autotune" / "run_one.py"
        self._cache: Dict[Tuple[Tuple[str, Any], ...], EvaluationResult] = {}
        self._objective_eval = ExpressionEvaluator(objective.expression)
        self._tiebreakers_eval = [ExpressionEvaluator(expr) for expr in objective.tiebreakers]

    def _score_tuple(
        self, objective_value: float, tiebreaker_values: Sequence[float]
    ) -> Tuple[float, ...]:
        values = (objective_value, *tiebreaker_values)
        if self.objective.maximize:
            return tuple(-value for value in values)
        return tuple(values)

    def _write_config(self, params: Mapping[str, Any], directory: pathlib.Path) -> pathlib.Path:
        config_data = build_config(params)
        config_path = directory / "config.json"
        write_json(config_path, config_data)
        return config_path

    def _run_single(self, config_path: pathlib.Path) -> Mapping[str, Any]:
        cmd = [
            sys.executable,
            str(self.run_one),
            "--app",
            str(self.app.path),
            "--config",
            str(config_path),
            "--warmup",
            str(self.app.warmup),
            "--run",
            str(self.app.run),
        ]
        if self.app.extra_args:
            cmd.append("--extra")
            cmd.extend(self.app.extra_args)

        completed = subprocess.run(cmd, check=True, capture_output=True, text=True)
        output = completed.stdout.strip()
        try:
            return json.loads(output)
        except json.JSONDecodeError as exc:
            raise SystemExit(f"Failed to parse run_one output:\n{output}") from exc

    def _evaluate_metrics(self, metrics: Mapping[str, Any]) -> Tuple[float, List[float]]:
        context = dict(metrics)
        if self.app.frame_budget_ms is not None and "frame_budget_ms" not in context:
            context["frame_budget_ms"] = self.app.frame_budget_ms
        objective_value = self._objective_eval(context)
        tiebreakers = [evaluator(context) for evaluator in self._tiebreakers_eval]
        return objective_value, tiebreakers

    def _aggregate(self, runs: Sequence[Mapping[str, Any]]) -> Tuple[bool, float, List[float], Dict[str, Any]]:
        ok = True
        objective_samples: List[float] = []
        tiebreaker_samples: List[List[float]] = [
            [] for _ in range(len(self._tiebreakers_eval))
        ]
        failure_reason: Optional[str] = None

        for result in runs:
            run_ok = bool(result.get("ok", False))
            if not run_ok:
                ok = False
                if failure_reason is None:
                    failure_reason = str(result.get("reason", "unspecified failure"))
            metrics = result.get("metrics")
            if not isinstance(metrics, Mapping):
                continue
            objective_value, tiebreakers = self._evaluate_metrics(metrics)
            objective_samples.append(objective_value)
            for idx, value in enumerate(tiebreakers):
                tiebreaker_samples[idx].append(value)

        if objective_samples:
            aggregated_objective = statistics.median(objective_samples)
        else:
            aggregated_objective = math.inf

        aggregated_tiebreakers: List[float] = []
        for samples in tiebreaker_samples:
            if samples:
                aggregated_tiebreakers.append(statistics.median(samples))
            else:
                aggregated_tiebreakers.append(math.inf)

        detail: Dict[str, Any] = {}
        if failure_reason:
            detail["reason"] = failure_reason
        return ok, aggregated_objective, aggregated_tiebreakers, detail

    def evaluate(self, params: Mapping[str, Any]) -> EvaluationResult:
        key = canonical_key(params)
        cached = self._cache.get(key)
        if cached is not None:
            return cached

        with tempfile.TemporaryDirectory(prefix="autotune_opt_") as tmpdir_str:
            tmpdir = pathlib.Path(tmpdir_str)
            config_path = self._write_config(params, tmpdir)
            runs: List[Mapping[str, Any]] = []
            for _ in range(self.replicates):
                runs.append(self._run_single(config_path))

        ok, objective_value, tiebreakers, detail = self._aggregate(runs)
        score_key = self._score_tuple(objective_value, tiebreakers)
        result = EvaluationResult(
            params=dict(params),
            ok=ok,
            raw_objective=objective_value,
            score_key=score_key,
            detail=detail,
        )
        self._cache[key] = result
        return result


class CoordinateDescent:
    def __init__(
        self,
        param_specs: Mapping[str, ParamSpec],
        evaluator: Evaluator,
        max_iterations: int,
    ) -> None:
        if max_iterations <= 0:
            raise SystemExit("--iters must be positive")
        self.param_names = list(param_specs.keys())
        self.domains = {name: ParamDomain(spec) for name, spec in param_specs.items()}
        self.evaluator = evaluator
        self.max_iterations = max_iterations

    @staticmethod
    def _is_better(lhs: EvaluationResult, rhs: EvaluationResult) -> bool:
        return lhs.score_key < rhs.score_key

    def optimise(self, start_params: Mapping[str, Any]) -> Tuple[List[Mapping[str, Any]], EvaluationResult]:
        current = self.evaluator.evaluate(start_params)
        path: List[Mapping[str, Any]] = [current.summary()]

        if current.ok:
            best: Optional[EvaluationResult] = current
        else:
            best = None

        for _ in range(self.max_iterations):
            improved_any = False
            for name in self.param_names:
                domain = self.domains[name]
                while True:
                    neighbors: List[EvaluationResult] = []
                    current_value = current.params[name]
                    for candidate_value in domain.neighbors(current_value):
                        candidate_params = dict(current.params)
                        candidate_params[name] = candidate_value
                        neighbors.append(self.evaluator.evaluate(candidate_params))

                    improving_neighbor: Optional[EvaluationResult] = None
                    for neighbor in neighbors:
                        if not neighbor.ok:
                            continue
                        if not current.ok or self._is_better(neighbor, current):
                            if improving_neighbor is None or self._is_better(
                                neighbor, improving_neighbor
                            ):
                                improving_neighbor = neighbor

                    if improving_neighbor is None:
                        break

                    current = improving_neighbor
                    path.append(current.summary())
                    improved_any = True

                    if current.ok and (best is None or self._is_better(current, best)):
                        best = current
                # Move on to the next dimension
            if not improved_any:
                break

        if best is None or not best.ok:
            raise SystemExit("Unable to identify a valid configuration with ok=true")

        return path, best


def main() -> None:
    args = parse_args()

    spec_data = load_simple_yaml(args.spec)
    app_spec = parse_app_spec(spec_data, args.spec.parent)
    objective_spec = parse_objective_spec(spec_data)

    param_specs = load_params_payload(args.spec)
    start_raw = decode_start_params(args.start_json)
    start_params = validate_params(start_raw, param_specs)

    evaluator = Evaluator(app_spec, objective_spec, args.replicates)
    optimiser = CoordinateDescent(param_specs, evaluator, args.iters)

    path, best = optimiser.optimise(start_params)

    output = {"path": path, "best": best.summary()}
    write_json(args.out, output)


if __name__ == "__main__":
    main()

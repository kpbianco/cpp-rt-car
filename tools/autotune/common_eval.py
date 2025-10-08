"""Shared logic for evaluating autotune metrics against a spec."""

from __future__ import annotations

import math
from typing import Any, Dict, Mapping, Optional, Tuple

SAFE_GLOBALS = {"__builtins__": {}}
SAFE_FUNCTIONS = {"min": min, "max": max, "abs": abs, "math": math}


class ExpressionEvaluator:
    def __init__(self, expression: str, *, label: str = "<expression>") -> None:
        prepared = (expression or "").strip()
        if not prepared:
            raise SystemExit(f"Expression for {label} must be a non-empty string")
        self.expression = prepared
        self._code = compile(prepared, label, "eval")

    def __call__(self, variables: Mapping[str, Any]) -> float:
        safe_locals = dict(SAFE_FUNCTIONS)
        safe_locals.update(variables)
        result = eval(self._code, SAFE_GLOBALS, safe_locals)
        if isinstance(result, (int, float)):
            return float(result)
        raise ValueError(f"Expression must evaluate to numeric result, got {result!r}")


class ConstraintEvaluator:
    def __init__(self, metric: str, expression: str) -> None:
        self.metric = metric
        prepared = (expression or "").strip()
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
        safe_locals = dict(SAFE_FUNCTIONS)
        safe_locals.update(context)
        try:
            result = eval(self._code, SAFE_GLOBALS, safe_locals)
        except Exception:
            return False
        return bool(result)


def parse_hard_constraints(spec_data: Mapping[str, Any]) -> Tuple[ConstraintEvaluator, ...]:
    metrics_payload = spec_data.get("metrics")
    if metrics_payload is None:
        return ()
    if not isinstance(metrics_payload, Mapping):
        raise SystemExit("Spec 'metrics' must be a mapping if provided")
    raw_constraints = metrics_payload.get("hard_constraints")
    if raw_constraints is None:
        return ()
    if not isinstance(raw_constraints, Mapping):
        raise SystemExit("metrics.hard_constraints must be a mapping if provided")
    constraints = []
    for name, expr in raw_constraints.items():
        if not isinstance(name, str) or not name:
            raise SystemExit("Constraint metric names must be non-empty strings")
        if not isinstance(expr, str):
            raise SystemExit(f"Constraint expression for metric '{name}' must be a string")
        constraints.append(ConstraintEvaluator(name, expr))
    return tuple(constraints)


def _coerce_float(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        numeric = float(value)
        if math.isfinite(numeric):
            return numeric
        return None
    if isinstance(value, str):
        try:
            numeric = float(value.strip())
        except ValueError:
            return None
        if math.isfinite(numeric):
            return numeric
    return None


def _resolve_frame_budget(spec: Mapping[str, Any], metrics: Mapping[str, Any]) -> Optional[float]:
    if isinstance(metrics, Mapping):
        candidate = _coerce_float(metrics.get("frame_budget_ms"))
        if candidate is not None and candidate > 0.0:
            return candidate
    app_payload = spec.get("app")
    if isinstance(app_payload, Mapping):
        candidate = _coerce_float(app_payload.get("frame_budget_ms"))
        if candidate is not None and candidate > 0.0:
            return candidate
    return None


def _objective_expression(spec: Mapping[str, Any]) -> str:
    metrics_payload = spec.get("metrics")
    if not isinstance(metrics_payload, Mapping):
        raise SystemExit("Spec 'metrics' must be a mapping")
    objective_payload = metrics_payload.get("objective")
    if not isinstance(objective_payload, Mapping):
        raise SystemExit("Spec 'metrics.objective' must be a mapping")
    expr = objective_payload.get("expr")
    if not isinstance(expr, str) or not expr.strip():
        raise SystemExit("Objective 'expr' must be a non-empty string")
    return expr


def _format_constraint_failure(metric: str, info: Mapping[str, Any]) -> str:
    expression = info.get("expression", "")
    if info.get("missing"):
        return f"missing metric '{metric}' for constraint '{expression}'"
    value = info.get("value")
    return f"constraint '{metric} {expression}' failed (value={value!r})"


def evaluate_constraints(
    spec: Mapping[str, Any], metrics: Mapping[str, Any]
) -> Tuple[bool, Dict[str, Dict[str, Any]]]:
    constraints = parse_hard_constraints(spec)
    if not constraints:
        return True, {}
    if not isinstance(metrics, Mapping):
        info = {
            "expression": "<metrics>",
            "missing": True,
        }
        info["message"] = "metrics payload is not a mapping"
        return False, {"<metrics>": info}
    base_metrics: Dict[str, Any] = dict(metrics)
    frame_budget = _resolve_frame_budget(spec, base_metrics)
    if frame_budget is not None and "frame_budget_ms" not in base_metrics:
        base_metrics["frame_budget_ms"] = frame_budget
    failures: Dict[str, Dict[str, Any]] = {}
    for constraint in constraints:
        try:
            ok = constraint.evaluate(base_metrics, frame_budget)
        except Exception:
            ok = False
        if ok:
            continue
        info: Dict[str, Any] = {"expression": constraint.expression}
        if constraint.metric in base_metrics:
            info["value"] = base_metrics[constraint.metric]
        else:
            info["missing"] = True
        info["message"] = _format_constraint_failure(constraint.metric, info)
        failures[constraint.metric] = info
    return (len(failures) == 0), failures


def compute_objective(spec: Mapping[str, Any], metrics: Mapping[str, Any]) -> float:
    context, frame_budget = prepare_metrics_context(spec, metrics)
    if context is None:
        return math.inf
    for constraint in parse_hard_constraints(spec):
        try:
            if not constraint.evaluate(context, frame_budget):
                return math.inf
        except Exception:
            return math.inf
    expr = _objective_expression(spec)
    evaluator = ExpressionEvaluator(expr, label="<objective>")
    try:
        value = evaluator(context)
    except (NameError, ValueError, TypeError, ZeroDivisionError, OverflowError):
        return math.inf
    if math.isnan(value):
        return math.inf
    return float(value)


def prepare_metrics_context(
    spec: Mapping[str, Any], metrics: Mapping[str, Any]
) -> Tuple[Optional[Dict[str, Any]], Optional[float]]:
    if not isinstance(metrics, Mapping):
        return None, None
    context: Dict[str, Any] = dict(metrics)
    frame_budget = _resolve_frame_budget(spec, context)
    if frame_budget is not None and "frame_budget_ms" not in context:
        context["frame_budget_ms"] = frame_budget
    return context, frame_budget


__all__ = [
    "ExpressionEvaluator",
    "ConstraintEvaluator",
    "parse_hard_constraints",
    "evaluate_constraints",
    "compute_objective",
    "prepare_metrics_context",
]

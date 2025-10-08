#!/usr/bin/env python3
"""End-to-end orchestration for autotune experiments."""

from __future__ import annotations

import argparse
import copy
import datetime
import json
import math
import os
import pathlib
import random
import statistics
import subprocess
import sys
import tempfile
from collections import OrderedDict
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

import hashlib
from dataclasses import dataclass, replace

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune import screening  # noqa: E402
from tools.autotune import common_host  # noqa: E402
from tools.autotune.common_io import append_jsonl as append_jsonl_record  # noqa: E402
from tools.autotune.make_config import (  # noqa: E402
    build_config,
    load_simple_yaml,
    load_spec as load_param_spec,
    validate_params,
    write_json,
)
from tools.autotune.optimize import AppSpec, ObjectiveSpec, parse_app_spec, parse_objective_spec  # noqa: E402
from tools.autotune.optimize import ExpressionEvaluator  # type: ignore[attr-defined]  # noqa: E402
from tools.autotune.common_eval import (  # noqa: E402
    OBJECTIVE_ERROR_EXIT_CODE,
    ObjectiveEvaluationError,
)
from tools.autotune.validate import (  # noqa: E402
    RobustnessSpec,
    parse_robustness,
    run_validation,
    select_best,
    summarise_candidate,
)
from tools.autotune.run_one import extract_seed, extract_scenario  # noqa: E402

RUN_ONE = REPO_ROOT / "tools" / "autotune" / "run_one.py"
OPTIMIZE = REPO_ROOT / "tools" / "autotune" / "optimize.py"
ANALYZE = REPO_ROOT / "tools" / "autotune" / "analyze.py"
MAKE_CONFIG = REPO_ROOT / "tools" / "autotune" / "make_config.py"


@dataclass(frozen=True)
class BaselineSource:
    description: str
    config_path: Optional[pathlib.Path] = None
    params: Optional[Mapping[str, Any]] = None


class ExperimentLog:
    """Handle reading/writing experiment log entries with resume support."""

    def __init__(self, path: pathlib.Path, objective: ObjectiveSpec):
        self.path = path
        self.objective = objective
        self.entries: "OrderedDict[Tuple[str, str], Dict[str, Any]]" = OrderedDict()
        self._stage_cache: Dict[str, Dict[str, Dict[str, Any]]] = {}
        self._run_cache: Dict[str, Dict[str, Any]] = {}
        self._load_existing()

    @staticmethod
    def _canonical_params(params: Mapping[str, Any]) -> str:
        return json.dumps(params, sort_keys=True)

    @staticmethod
    def _normalise_component(component: Any) -> str:
        try:
            return json.dumps(component, sort_keys=True)
        except TypeError:
            return json.dumps(str(component))

    @staticmethod
    def _hash_components(
        spec_digest: str, canonical_params: str, seed_component: str, scenario_component: str
    ) -> str:
        digest = hashlib.sha256()
        digest.update(spec_digest.encode("utf-8"))
        digest.update(b"|")
        digest.update(canonical_params.encode("utf-8"))
        digest.update(b"|")
        digest.update(seed_component.encode("utf-8"))
        digest.update(b"|")
        digest.update(scenario_component.encode("utf-8"))
        return digest.hexdigest()

    def _run_hash(
        self,
        params: Mapping[str, Any],
        seed: Any,
        scenario: Any,
        spec_digest: Optional[str],
    ) -> Optional[str]:
        if not isinstance(params, Mapping):
            return None
        if not isinstance(spec_digest, str) or not spec_digest:
            return None
        canonical_params = self._canonical_params(params)
        seed_component = self._normalise_component(seed)
        scenario_component = self._normalise_component(scenario)
        return self._hash_components(spec_digest, canonical_params, seed_component, scenario_component)

    def compute_run_hash(
        self,
        params: Mapping[str, Any],
        seed: Any,
        scenario: Any,
        spec_digest: Optional[str],
    ) -> Optional[str]:
        return self._run_hash(params, seed, scenario, spec_digest)

    @staticmethod
    def _extract_run_identity(
        run: Mapping[str, Any]
    ) -> Optional[Tuple[Mapping[str, Any], Any, Any, Optional[str]]]:
        params = run.get("_params")
        if not isinstance(params, Mapping):
            candidate_params = run.get("params")
            params = candidate_params if isinstance(candidate_params, Mapping) else None
        if not isinstance(params, Mapping):
            return None
        seed = run.get("_seed")
        if seed is None:
            seed = run.get("seed")
        scenario = run.get("_scenario")
        if scenario is None:
            scenario = run.get("scenario")
        spec_digest = run.get("_spec_digest") or run.get("spec_digest")
        return params, seed, scenario, spec_digest

    def _index_run(self, run: Mapping[str, Any]) -> None:
        if not isinstance(run, Mapping):
            return
        run_hash = run.get("_run_hash")
        if not isinstance(run_hash, str) or not run_hash:
            run_hash = run.get("run_hash")
        identity = self._extract_run_identity(run)
        if run_hash is None and identity is None:
            return
        if run_hash is None and identity is not None:
            params, seed, scenario, spec_digest = identity
            run_hash = self._run_hash(params, seed, scenario, spec_digest)
        if not isinstance(run_hash, str) or not run_hash:
            return
        if isinstance(run, dict):
            run.setdefault("_run_hash", run_hash)
            run.setdefault("run_hash", run_hash)
        cached = copy.deepcopy(dict(run))
        cached.setdefault("_run_hash", run_hash)
        cached.setdefault("run_hash", run_hash)
        self._run_cache[run_hash] = cached

    def _index_runs_from_record(self, record: Mapping[str, Any]) -> None:
        runs = record.get("runs")
        if isinstance(runs, Sequence):
            for run in runs:
                if isinstance(run, Mapping):
                    self._index_run(run)
        result = record.get("result")
        if isinstance(result, Mapping):
            self._index_run(result)

    def _load_existing(self) -> None:
        if not self.path.is_file():
            return
        with self.path.open("r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                stage = record.get("stage")
                params = record.get("params")
                if not stage or not isinstance(stage, str) or not isinstance(params, Mapping):
                    continue
                key = (stage, self._canonical_params(params))
                self.entries[key] = record
                stage_map = self._stage_cache.setdefault(stage, {})
                stage_map[self._canonical_params(params)] = record
                self._index_runs_from_record(record)

    def get(self, stage: str, params: Mapping[str, Any]) -> Optional[Dict[str, Any]]:
        stage_map = self._stage_cache.get(stage)
        if not stage_map:
            return None
        return stage_map.get(self._canonical_params(params))

    def lookup_run(
        self,
        params: Mapping[str, Any],
        seed: Any,
        scenario: Any,
        spec_digest: str,
    ) -> Optional[Dict[str, Any]]:
        run_hash = self._run_hash(params, seed, scenario, spec_digest)
        if run_hash is None:
            return None
        cached = self._run_cache.get(run_hash)
        if cached is None:
            return None
        result = copy.deepcopy(cached)
        result.setdefault("_run_hash", run_hash)
        result.setdefault("run_hash", run_hash)
        return result

    def remember_run(self, run: Mapping[str, Any]) -> None:
        self._index_run(run)

    def register(self, stage: str, params: Mapping[str, Any], record: Dict[str, Any]) -> None:
        key = (stage, self._canonical_params(params))
        if key not in self.entries:
            self.entries[key] = record
        self._stage_cache.setdefault(stage, {})[self._canonical_params(params)] = record
        self._index_runs_from_record(record)

    def append(self, record: Mapping[str, Any]) -> None:
        append_jsonl_record(self.path, record, sort_keys=True)

    def all_entries(self) -> List[Dict[str, Any]]:
        return list(self.entries.values())


def _read_json_mapping(path: pathlib.Path) -> Optional[Mapping[str, Any]]:
    try:
        with path.open("r", encoding="utf-8") as fh:
            data = json.load(fh)
    except FileNotFoundError:
        return None
    except json.JSONDecodeError:
        return None
    if isinstance(data, Mapping):
        return data
    return None


def _extract_params_from_payload(payload: Mapping[str, Any]) -> Optional[Dict[str, Any]]:
    params = payload.get("params") if isinstance(payload, Mapping) else None
    if isinstance(params, Mapping):
        return dict(params)
    return None


def _baseline_directories(app: AppSpec) -> List[pathlib.Path]:
    candidates: List[pathlib.Path] = []
    profiles_dir = REPO_ROOT / "profiles"
    configs_dir = REPO_ROOT / "configs"
    app_configs = app.path.parent / "configs"
    build_configs = app.path.parent.parent / "configs"
    for directory in (profiles_dir, configs_dir, app_configs, build_configs):
        if directory and directory not in candidates:
            candidates.append(directory)
    return candidates


def detect_baseline_source(app: AppSpec, results_dir: pathlib.Path) -> Optional[BaselineSource]:
    directories = _baseline_directories(app)

    env_profile = os.environ.get("RTFW_PROFILE")
    if env_profile:
        env_candidates: List[pathlib.Path] = []
        env_path = pathlib.Path(env_profile)
        env_candidates.append(env_path)
        if not env_path.suffix:
            env_candidates.append(env_path.with_suffix(".json"))
        for directory in directories:
            env_candidates.append(directory / env_profile)
            if not env_profile.endswith(".json"):
                env_candidates.append(directory / f"{env_profile}.json")
        for candidate in env_candidates:
            if candidate.is_file():
                payload = _read_json_mapping(candidate)
                params = _extract_params_from_payload(payload) if payload else None
                return BaselineSource(
                    description=f"RTFW_PROFILE={env_profile}",
                    config_path=candidate,
                    params=params,
                )

    tokens = common_host.host_tokens()
    cpu_slug = tokens["cpu_slug"]
    os_name = tokens["os_name"]
    machine_candidates = [
        f"{cpu_slug}-{os_name}.json",
        f"{cpu_slug}-{os_name}",
    ]
    for name in machine_candidates:
        for directory in directories:
            candidate = directory / name
            if candidate.is_file():
                payload = _read_json_mapping(candidate)
                params = _extract_params_from_payload(payload) if payload else None
                return BaselineSource(
                    description="machine profile",
                    config_path=candidate,
                    params=params,
                )

    for name in ("default_safe.json", "default_fast.json", "default.json"):
        for directory in directories:
            candidate = directory / name
            if candidate.is_file():
                payload = _read_json_mapping(candidate)
                params = _extract_params_from_payload(payload) if payload else None
                return BaselineSource(
                    description=name,
                    config_path=candidate,
                    params=params,
                )

    best_path = results_dir / "best.json"
    best_payload = _read_json_mapping(best_path)
    if isinstance(best_payload, Mapping):
        params = _extract_params_from_payload(best_payload)
        if params:
            return BaselineSource(description="results/best.json", params=params)

    return None


def _is_simple_param_value(value: Any) -> bool:
    if isinstance(value, (str, int, float, bool)):
        return True
    return False


def _iter_profile_default_payloads(payload: Any, description: str) -> Iterable[Tuple[str, Mapping[str, Any]]]:
    stack: List[Tuple[str, Any]] = [(description, payload)]
    seen: set[int] = set()
    while stack:
        desc, current = stack.pop()
        if isinstance(current, Mapping):
            identity = id(current)
            if identity in seen:
                continue
            seen.add(identity)
            yield desc, current
            for key, value in current.items():
                if isinstance(value, Mapping) or (
                    isinstance(value, Sequence)
                    and not isinstance(value, (str, bytes, bytearray))
                ):
                    child_desc = f"{desc}.{key}" if desc else str(key)
                    stack.append((child_desc, value))
        elif isinstance(current, Sequence) and not isinstance(current, (str, bytes, bytearray)):
            identity = id(current)
            if identity in seen:
                continue
            seen.add(identity)
            for index, value in enumerate(current):
                if isinstance(value, Mapping) or (
                    isinstance(value, Sequence)
                    and not isinstance(value, (str, bytes, bytearray))
                ):
                    child_desc = f"{desc}[{index}]" if desc else f"[{index}]"
                    stack.append((child_desc, value))


def _extract_profile_params(
    payload: Mapping[str, Any], param_specs: Mapping[str, Any]
) -> Optional[Dict[str, Any]]:
    candidates: List[Mapping[str, Any]] = []
    params_section = payload.get("params")
    if isinstance(params_section, Mapping):
        candidates.append(params_section)
    candidates.append(payload)

    for candidate in candidates:
        filtered: Dict[str, Any] = {}
        for name, value in candidate.items():
            if name not in param_specs:
                continue
            if isinstance(value, Mapping) or (
                isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray))
            ):
                continue
            if not _is_simple_param_value(value):
                continue
            filtered[name] = value
        if filtered:
            return filtered
    return None


def _coerce_profile_params(
    params: Mapping[str, Any],
    param_specs: Mapping[str, Any],
    description: str,
) -> Dict[str, Any]:
    coerced: Dict[str, Any] = {}
    for name, value in params.items():
        spec = param_specs.get(name)
        if spec is None:
            continue
        try:
            coerced[name] = spec.coerce(value)
        except SystemExit as exc:
            raise SystemExit(
                f"Baseline parameter '{name}' from {description} failed validation: {exc}"
            ) from exc
    return coerced


def baseline_source_from_spec_defaults(
    spec_data: Mapping[str, Any],
    param_specs: Mapping[str, Any],
) -> Optional[BaselineSource]:
    defaults_payload = spec_data.get("profile_defaults") if isinstance(spec_data, Mapping) else None
    if defaults_payload is None:
        return None

    any_mapping = False
    for description, mapping in _iter_profile_default_payloads(
        defaults_payload, "spec profile defaults"
    ):
        any_mapping = True
        params = _extract_profile_params(mapping, param_specs)
        if not params:
            continue
        coerced = _coerce_profile_params(params, param_specs, description)
        return BaselineSource(description=description, params=coerced)

    if any_mapping:
        return BaselineSource(description="spec profile defaults", params={})
    return None


def run_baseline_gate(
    app: AppSpec,
    param_specs: Mapping[str, Any],
    spec_data: Mapping[str, Any],
    spec_path: pathlib.Path,
    results_dir: pathlib.Path,
) -> None:
    source = baseline_source_from_spec_defaults(spec_data, param_specs)
    if source is None:
        source = detect_baseline_source(app, results_dir)
    allow_partial_params = False
    if source is None:
        source = BaselineSource(description="empty parameter set", params={})
        allow_partial_params = True
    elif source.description.startswith("spec profile defaults"):
        allow_partial_params = True

    params_for_diag: Optional[Dict[str, Any]] = None
    config_path_str: Optional[str] = None

    with tempfile.TemporaryDirectory(prefix="autotune_baseline_") as tmpdir_str:
        tmpdir = pathlib.Path(tmpdir_str)
        config_path = source.config_path
        if config_path is None:
            params = source.params
            if not isinstance(params, Mapping):
                params = {}
            validated = params
            if validated:
                try:
                    validated = validate_params(validated, param_specs)
                except SystemExit:
                    if not allow_partial_params:
                        raise
                    # Allow partial parameter sets derived from defaults; they were already coerced.
                    validated = dict(params)
            params_for_diag = dict(validated)
            config_data = build_config(validated)
            config_path = tmpdir / "baseline_config.json"
            write_json(config_path, config_data)
        else:
            params_for_diag = (
                dict(source.params)
                if isinstance(source.params, Mapping)
                else params_for_diag
            )

        config_path_str = str(config_path)
        result = run_single(app, config_path, spec_path)

    if not isinstance(result, Mapping):
        raise SystemExit("Baseline run did not produce a valid result payload")

    if result.get("ok"):
        return

    reason = result.get("reason")
    metrics_payload = result.get("metrics")
    metrics = metrics_payload if isinstance(metrics_payload, Mapping) else None
    command = result.get("command")
    diag_lines = [
        "Baseline configuration failed sanity gate.",
        f"Source: {source.description}",
    ]
    if config_path_str:
        diag_lines.append(f"Config: {config_path_str}")
    if params_for_diag:
        diag_lines.append(f"Params: {json.dumps(params_for_diag, sort_keys=True)}")
    if reason:
        diag_lines.append(f"Reason: {reason}")
    if metrics:
        diag_lines.append(f"Metrics: {json.dumps(metrics, sort_keys=True)}")
    if isinstance(command, Sequence):
        diag_lines.append("Command: " + " ".join(str(part) for part in command))
    message = "\n".join(diag_lines)
    print(message, file=sys.stderr)
    raise SystemExit("Baseline configuration failed, aborting autotune")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run full autotune workflow")
    parser.add_argument("--spec", required=True, type=pathlib.Path)
    parser.add_argument("--screen", required=True, type=int, help="Number of screening candidates")
    parser.add_argument("--replicates", required=True, type=int, help="Replicates per evaluation")
    parser.add_argument("--local-iters", required=True, type=int, help="Iterations for local search")
    parser.add_argument("--topk", required=True, type=int, help="Top K candidates to validate")
    parser.add_argument("--seed", type=int, default=0, help="Seed for screening candidate generation")
    parser.add_argument(
        "--warmup-sec",
        type=float,
        default=None,
        help="Override warmup duration from the spec (seconds)",
    )
    parser.add_argument(
        "--run-sec",
        type=float,
        default=None,
        help="Override run duration from the spec (seconds)",
    )
    return parser.parse_args()


def ensure_directories(paths: Iterable[pathlib.Path]) -> None:
    for path in paths:
        path.mkdir(parents=True, exist_ok=True)


def safe_float(value: Any) -> Optional[float]:
    if isinstance(value, (int, float)):
        try:
            numeric = float(value)
        except (TypeError, ValueError):
            return None
        if math.isfinite(numeric):
            return numeric
    return None


def get_metrics_summary(run: Mapping[str, Any]) -> Mapping[str, Any]:
    summary = run.get("_summary")
    if isinstance(summary, Mapping):
        return summary
    metrics = run.get("metrics")
    if isinstance(metrics, Mapping):
        return metrics
    return {}


def aggregate_runs(
    runs: Sequence[Mapping[str, Any]],
    objective: ObjectiveSpec,
    objective_eval: ExpressionEvaluator,
    tiebreakers_eval: Sequence[ExpressionEvaluator],
    frame_budget: Optional[float],
) -> Dict[str, Any]:
    ok = True
    reasons: List[str] = []
    objective_samples: List[float] = []
    tiebreaker_samples: List[List[float]] = [
        [] for _ in tiebreakers_eval
    ]
    metrics_samples: Dict[str, List[float]] = {}
    success_count = 0

    for run in runs:
        run_ok = bool(run.get("ok", False))
        if run_ok:
            success_count += 1
        else:
            reason = run.get("reason")
            if isinstance(reason, str) and reason:
                reasons.append(reason)
        ok = ok and run_ok
        metrics = get_metrics_summary(run)
        if not metrics:
            continue
        context: Dict[str, Any] = dict(metrics)
        if frame_budget is not None and "frame_budget_ms" not in context:
            context["frame_budget_ms"] = frame_budget
        try:
            obj_value = float(objective_eval(context))
            if math.isfinite(obj_value):
                objective_samples.append(obj_value)
                for idx, evaluator in enumerate(tiebreakers_eval):
                    tb_value = float(evaluator(context))
                    if math.isfinite(tb_value):
                        tiebreaker_samples[idx].append(tb_value)
        except Exception:
            pass
        for name, value in metrics.items():
            numeric = safe_float(value)
            if numeric is None:
                continue
            metrics_samples.setdefault(name, []).append(numeric)

    penalty = 1e12
    if objective.maximize:
        penalty = -penalty

    if objective_samples:
        aggregated_objective = float(statistics.median(objective_samples))
    else:
        aggregated_objective = float(penalty)

    aggregated_tiebreakers: List[float] = []
    for samples in tiebreaker_samples:
        if samples:
            aggregated_tiebreakers.append(float(statistics.median(samples)))
        else:
            aggregated_tiebreakers.append(float(penalty))

    aggregated_metrics: Dict[str, float] = {
        name: float(statistics.median(values))
        for name, values in sorted(metrics_samples.items())
        if values
    }

    reason_text = " ; ".join(sorted(set(reasons))) if reasons else None

    score_key = [
        value if math.isfinite(value) else penalty for value in [aggregated_objective, *aggregated_tiebreakers]
    ]

    return {
        "ok": ok,
        "objective": aggregated_objective,
        "tiebreakers": aggregated_tiebreakers,
        "metrics": aggregated_metrics,
        "reason": reason_text,
        "score_key": score_key,
        "total_runs": len(runs),
        "successful_runs": success_count,
    }


def run_single(app: AppSpec, config_path: pathlib.Path, spec_path: pathlib.Path) -> Dict[str, Any]:
    cmd: List[str] = [
        sys.executable,
        str(RUN_ONE),
        "--app",
        str(app.path),
        "--config",
        str(config_path),
        "--warmup",
        str(app.warmup),
        "--run",
        str(app.run),
        "--spec",
        str(spec_path),
    ]
    if app.extra_args:
        cmd.append("--extra")
        cmd.extend(str(arg) for arg in app.extra_args)

    try:
        completed = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        stdout = exc.stdout.strip() if exc.stdout else ""
        stderr = exc.stderr.strip() if exc.stderr else ""
        if exc.returncode == OBJECTIVE_ERROR_EXIT_CODE:
            message = stderr or stdout or "Objective evaluation failed"
            raise ObjectiveEvaluationError(message) from exc
        reason_parts = ["run_one failed"]
        if stdout:
            reason_parts.append(f"stdout: {stdout}")
        if stderr:
            reason_parts.append(f"stderr: {stderr}")
        return {
            "ok": False,
            "objective": None,
            "metrics": {},
            "reason": "; ".join(reason_parts),
            "command": cmd,
        }

    stdout = completed.stdout.strip()
    line = stdout.splitlines()[-1] if stdout else ""
    try:
        payload = json.loads(line)
    except json.JSONDecodeError:
        payload = {
            "ok": False,
            "objective": None,
            "metrics": {},
            "reason": f"unable to parse JSON output: {stdout}",
        }
    payload.setdefault("command", cmd)
    return payload


def evaluate_candidate(
    stage: str,
    params: Mapping[str, Any],
    metadata: Mapping[str, Any],
    app: AppSpec,
    param_specs: Mapping[str, Any],
    objective: ObjectiveSpec,
    objective_eval: ExpressionEvaluator,
    tiebreakers_eval: Sequence[ExpressionEvaluator],
    replicates: int,
    log: ExperimentLog,
    spec_digest: str,
    spec_path: pathlib.Path,
) -> Dict[str, Any]:
    validated = validate_params(params, param_specs)
    cached = log.get(stage, validated)
    if cached is not None:
        return cached

    runs: List[Dict[str, Any]] = []
    with tempfile.TemporaryDirectory(prefix="autotune_stage_") as tmpdir_str:
        tmpdir = pathlib.Path(tmpdir_str)
        config_path = tmpdir / "config.json"
        config_data = build_config(validated)
        write_json(config_path, config_data)
        seed = extract_seed(config_data, config_path)
        scenario_name = extract_scenario(config_data, config_path)
        for _ in range(max(1, replicates)):
            cached_run = log.lookup_run(validated, seed, scenario_name, spec_digest)
            if cached_run is not None:
                runs.append(copy.deepcopy(cached_run))
                continue
            result = run_single(app, config_path, spec_path)
            if not isinstance(result, Mapping):
                result = {}
            result.setdefault("_params", dict(validated))
            result.setdefault("params", dict(validated))
            result.setdefault("_seed", seed)
            result.setdefault("seed", seed)
            result.setdefault("_scenario", scenario_name)
            result.setdefault("scenario", scenario_name)
            result["_spec_digest"] = spec_digest
            log.remember_run(result)
            runs.append(copy.deepcopy(result))

    aggregated = aggregate_runs(runs, objective, objective_eval, tiebreakers_eval, app.frame_budget_ms)

    record: Dict[str, Any] = {
        "stage": stage,
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "params": validated,
        "ok": aggregated["ok"],
        "objective": aggregated["objective"],
        "tiebreakers": aggregated["tiebreakers"],
        "metrics": aggregated["metrics"],
        "reason": aggregated.get("reason"),
        "score_key": aggregated["score_key"],
        "runs": runs,
        "metadata": dict(metadata),
        "total_runs": aggregated["total_runs"],
        "successful_runs": aggregated["successful_runs"],
        "spec_digest": spec_digest,
    }

    log.register(stage, validated, record)
    log.append(record)
    return record


def score_tuple(entry: Mapping[str, Any], maximize: bool) -> Tuple[float, ...]:
    raw = entry.get("score_key")
    if isinstance(raw, Sequence) and not isinstance(raw, (str, bytes)):
        components: List[float] = []
        for value in raw:
            numeric = safe_float(value)
            if numeric is None:
                numeric = -1e12 if maximize else 1e12
            components.append(numeric)
        return tuple(components)
    objective_value = entry.get("objective")
    numeric = safe_float(objective_value)
    if numeric is None:
        numeric = -1e12 if maximize else 1e12
    return (numeric,)


def select_unique_best(
    entries: Iterable[Mapping[str, Any]], limit: int, maximize: bool
) -> List[Dict[str, Any]]:
    ordered = sorted(entries, key=lambda entry: score_tuple(entry, maximize), reverse=maximize)
    seen: set[str] = set()
    results: List[Dict[str, Any]] = []
    for entry in ordered:
        if not entry.get("ok"):
            continue
        params = entry.get("params")
        if not isinstance(params, Mapping):
            continue
        key = json.dumps(params, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        results.append(dict(entry))
        if len(results) >= limit:
            break
    return results


def write_json_file(path: pathlib.Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")


def append_jsonl(path: pathlib.Path, records: Sequence[Mapping[str, Any]]) -> None:
    if not records:
        return
    for record in records:
        append_jsonl_record(path, record, sort_keys=True)


def sanitise_filename(text: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "-" for ch in text)
    return cleaned.strip("-") or "unknown"


def compute_spec_digest(
    path: pathlib.Path, overrides: Optional[Mapping[str, Any]] = None
) -> str:
    digest = hashlib.sha256()
    digest.update(path.read_bytes())
    if overrides:
        payload = json.dumps(overrides, sort_keys=True).encode("utf-8")
        digest.update(payload)
    return digest.hexdigest()


def _run_pipeline(
    args: argparse.Namespace,
    spec_path: pathlib.Path,
    error_context: Dict[str, Optional[str]],
) -> None:
    if not spec_path.is_file():
        raise SystemExit(f"Spec file not found: {spec_path}")

    spec_data = load_simple_yaml(spec_path)
    if not isinstance(spec_data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping at the top level")

    app_spec = parse_app_spec(spec_data, spec_path.parent)

    app_overrides: Dict[str, float] = {}
    if args.warmup_sec is not None:
        if args.warmup_sec < 0:
            raise SystemExit("--warmup-sec must be non-negative")
        app_overrides["warmup"] = float(args.warmup_sec)
    if args.run_sec is not None:
        if args.run_sec <= 0:
            raise SystemExit("--run-sec must be positive")
        app_overrides["run"] = float(args.run_sec)
    if app_overrides:
        app_spec = replace(app_spec, **app_overrides)

    metadata_overrides = {"app_overrides": dict(app_overrides)} if app_overrides else {}

    objective_spec = parse_objective_spec(spec_data)
    error_context["objective_expr"] = objective_spec.expression
    objective_eval = ExpressionEvaluator(objective_spec.expression)
    tiebreakers_eval = [ExpressionEvaluator(expr) for expr in objective_spec.tiebreakers]

    param_specs = load_param_spec(spec_path)
    robustness_spec = parse_robustness(spec_data)

    results_dir = REPO_ROOT / "results"
    profiles_dir = REPO_ROOT / "profiles"
    reports_dir = REPO_ROOT / "reports"
    ensure_directories([results_dir, profiles_dir, reports_dir])

    run_baseline_gate(app_spec, param_specs, spec_data, spec_path, results_dir)

    experiments_log_path = results_dir / "experiments.jsonl"
    experiments_log_path.touch(exist_ok=True)
    log = ExperimentLog(experiments_log_path, objective_spec)
    spec_digest = compute_spec_digest(spec_path, app_overrides if app_overrides else None)

    rng = random.Random(args.seed)

    screen_candidates = screening.generate_candidates(
        *screening.parse_params(screening.load_spec(spec_path)),
        count=args.screen,
        rng=rng,
    )
    screen_candidates_path = results_dir / "screening_candidates.json"
    write_json_file(
        screen_candidates_path,
        {
            "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "candidates": screen_candidates,
        },
    )

    for index, candidate in enumerate(screen_candidates):
        metadata = {**metadata_overrides, "source": "screen", "index": index}
        evaluate_candidate(
            stage="screen",
            params=candidate,
            metadata=metadata,
            app=app_spec,
            param_specs=param_specs,
            objective=objective_spec,
            objective_eval=objective_eval,
            tiebreakers_eval=tiebreakers_eval,
            replicates=args.replicates,
            log=log,
            spec_digest=spec_digest,
            spec_path=spec_path,
        )

    all_entries = log.all_entries()
    screen_entries = [entry for entry in all_entries if entry.get("stage") == "screen"]
    best_screen = select_unique_best(screen_entries, 1, objective_spec.maximize)
    if not best_screen:
        raise SystemExit("No successful screening results recorded")
    best_params = best_screen[0]["params"]

    start_json = json.dumps(best_params, sort_keys=True)
    local_output_path = results_dir / "local_opt.json"
    if not local_output_path.exists():
        subprocess.run(
            [
                sys.executable,
                str(OPTIMIZE),
                "--spec",
                str(spec_path),
                "--start-json",
                start_json,
                "--iters",
                str(args.local_iters),
                "--replicates",
                str(args.replicates),
                "--out",
                str(local_output_path),
            ],
            check=True,
        )

    local_payload = json.loads(local_output_path.read_text(encoding="utf-8"))
    local_best = local_payload.get("best", {})
    local_params = local_best.get("params") if isinstance(local_best, Mapping) else None
    if not isinstance(local_params, Mapping):
        local_params = best_params

    evaluate_candidate(
        stage="local",
        params=local_params,
        metadata={**metadata_overrides, "source": "local_opt", "path": str(local_output_path)},
        app=app_spec,
        param_specs=param_specs,
        objective=objective_spec,
        objective_eval=objective_eval,
        tiebreakers_eval=tiebreakers_eval,
        replicates=args.replicates,
        log=log,
        spec_digest=spec_digest,
        spec_path=spec_path,
    )

    all_entries = log.all_entries()
    top_candidates = select_unique_best(all_entries, args.topk, objective_spec.maximize)
    top_candidates_path = results_dir / "top_candidates.json"
    write_json_file(top_candidates_path, {"candidates": top_candidates})

    validation_runs_path = results_dir / "validation_runs.jsonl"
    validation_runs_path.touch(exist_ok=True)
    validation_summaries_path = results_dir / "validation_summary.json"

    validation_summaries: List[Dict[str, Any]] = []
    validation_raw_records: List[Dict[str, Any]] = []

    for rank, candidate in enumerate(top_candidates, start=1):
        params = candidate.get("params")
        if not isinstance(params, Mapping):
            continue
        metadata = {**metadata_overrides, "rank": rank, "source_stage": candidate.get("stage")}
        cached = log.get("validate", params)
        if cached is not None and cached.get("runs"):
            runs = cached.get("runs", [])
            run_records = []
            for run in runs:
                if not isinstance(run, Mapping):
                    continue
                if "_spec_digest" not in run:
                    run["_spec_digest"] = spec_digest
                log.remember_run(run)
                run_records.append(run)
        else:
            run_records_map: Dict[Tuple[str, Optional[int]], Dict[str, Any]] = {}
            missing: Dict[str, Dict[str, Any]] = {}
            for scenario in robustness_spec.scenarios:
                scenario_name = scenario.name
                for seed in robustness_spec.seeds:
                    cached_run = log.lookup_run(params, seed, scenario_name, spec_digest)
                    if cached_run is not None:
                        if "_spec_digest" not in cached_run:
                            cached_run["_spec_digest"] = spec_digest
                        log.remember_run(cached_run)
                        run_records_map[(scenario_name, seed)] = cached_run
                    else:
                        record = missing.setdefault(
                            scenario_name,
                            {"spec": scenario, "seeds": []},
                        )
                        record["seeds"].append(seed)

            raw_records: List[Dict[str, Any]] = []
            if missing:
                for record in missing.values():
                    scenario = record["spec"]
                    seeds = record["seeds"]
                    partial_spec = RobustnessSpec(
                        seeds=tuple(seeds),
                        scenarios=(scenario,),
                        seed_env=robustness_spec.seed_env,
                        seed_arg=robustness_spec.seed_arg,
                        config_seed_path=robustness_spec.config_seed_path,
                    )
                    new_runs, new_raw_records = run_validation(
                        app_spec,
                        partial_spec,
                        params,
                        {**metadata_overrides, "rank": rank, "stage": candidate.get("stage")},
                    )
                    for run in new_runs:
                        if not isinstance(run, Mapping):
                            continue
                        run["_spec_digest"] = spec_digest
                        log.remember_run(run)
                        scenario_key = str(run.get("scenario", scenario.name))
                        seed_key = run.get("seed")
                        run_records_map[(scenario_key, seed_key)] = run
                    for raw in new_raw_records:
                        result = raw.get("result") if isinstance(raw, Mapping) else None
                        if isinstance(result, Mapping):
                            result["_spec_digest"] = spec_digest
                        raw_records.append(raw)
            validation_raw_records.extend(raw_records)

            ordered_runs: List[Dict[str, Any]] = []
            for scenario in robustness_spec.scenarios:
                scenario_name = scenario.name
                for seed in robustness_spec.seeds:
                    key = (scenario_name, seed)
                    run = run_records_map.get(key)
                    if run is not None:
                        ordered_runs.append(run)
            run_records = ordered_runs
            runs = ordered_runs
            aggregated = aggregate_runs(
                runs,
                objective_spec,
                objective_eval,
                tiebreakers_eval,
                app_spec.frame_budget_ms,
            )
            record = {
                "stage": "validate",
                "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "params": params,
                "ok": aggregated["ok"],
                "objective": aggregated["objective"],
                "tiebreakers": aggregated["tiebreakers"],
                "metrics": aggregated["metrics"],
                "reason": aggregated.get("reason"),
                "score_key": aggregated["score_key"],
                "runs": runs,
                "metadata": {**metadata, "validated": True},
                "total_runs": aggregated["total_runs"],
                "successful_runs": aggregated["successful_runs"],
                "spec_digest": spec_digest,
            }
            log.register("validate", params, record)
            log.append(record)
            run_records = runs
        summary = summarise_candidate(
            rank - 1,
            {**metadata_overrides, "rank": rank, "stage": candidate.get("stage")},
            params,
            run_records,
            spec_data,
        )
        validation_summaries.append(summary)

    if validation_raw_records:
        append_jsonl(validation_runs_path, validation_raw_records)

    best_validation = select_best(validation_summaries, objective_spec.maximize)
    write_json_file(
        validation_summaries_path,
        {
            "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "summaries": validation_summaries,
            "best": best_validation,
        },
    )

    subprocess.run(
        [
            sys.executable,
            str(ANALYZE),
            "--spec",
            str(spec_path),
            "--in",
            str(experiments_log_path),
            "--out-dir",
            str(reports_dir),
        ],
        check=True,
    )

    best_path = reports_dir / "best.json"
    pareto_path = reports_dir / "pareto.json"
    summary_csv_path = reports_dir / "summary.csv"
    analysis_summary_path = reports_dir / "summary.json"

    best_payload = json.loads(best_path.read_text(encoding="utf-8"))
    best_params_final = best_payload.get("params") if isinstance(best_payload, Mapping) else {}
    if not isinstance(best_params_final, Mapping):
        best_params_final = {}

    tokens = common_host.host_tokens()
    profile_path = profiles_dir / f"{tokens['cpu_slug']}-{tokens['os_name']}.json"

    subprocess.run(
        [
            sys.executable,
            str(MAKE_CONFIG),
            "--spec",
            str(spec_path),
            "--params-json",
            json.dumps(best_params_final, sort_keys=True),
            "--out",
            str(profile_path),
        ],
        check=True,
    )

    summary_path = results_dir / "summary.json"
    summary_payload = {
        "spec": str(spec_path),
        "experiments_log": str(experiments_log_path),
        "screening_candidates": str(screen_candidates_path),
        "local_optimisation": str(local_output_path),
        "top_candidates": str(top_candidates_path),
        "validation_runs": str(validation_runs_path),
        "validation_summary": str(validation_summaries_path),
        "analysis": {
            "best": str(best_path),
            "pareto": str(pareto_path),
            "summary_csv": str(summary_csv_path),
            "summary_json": str(analysis_summary_path),
        },
        "profile": str(profile_path),
        "best_objective": best_payload.get("objective"),
        "best_params": best_params_final,
        "app_overrides": app_overrides,
    }
    write_json_file(summary_path, summary_payload)

    ci_summary = {
        "profile": str(profile_path),
        "best_objective": best_payload.get("objective"),
        "best_params": best_params_final,
        "analysis_dir": str(reports_dir),
        "app_overrides": app_overrides,
    }
    print(json.dumps(ci_summary, sort_keys=True))


def main() -> None:
    args = parse_args()
    spec_path = args.spec.resolve()
    error_context: Dict[str, Optional[str]] = {"objective_expr": None}
    try:
        _run_pipeline(args, spec_path, error_context)
    except ObjectiveEvaluationError as exc:
        expression = error_context.get("objective_expr") or "<unknown>"
        message = str(exc).strip() or "Objective evaluation failed"
        print(
            f"Objective evaluation error for spec '{spec_path}': expression '{expression}' failed: {message}",
            file=sys.stderr,
        )
        raise SystemExit(2) from exc


if __name__ == "__main__":
    main()

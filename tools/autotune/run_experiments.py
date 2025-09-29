#!/usr/bin/env python3
"""End-to-end orchestration for autotune experiments."""

from __future__ import annotations

import argparse
import datetime
import json
import math
import pathlib
import platform
import random
import statistics
import subprocess
import sys
import tempfile
from collections import OrderedDict
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune import screening  # noqa: E402
from tools.autotune.make_config import (  # noqa: E402
    build_config,
    load_simple_yaml,
    load_spec as load_param_spec,
    validate_params,
    write_json,
)
from tools.autotune.optimize import AppSpec, ObjectiveSpec, parse_app_spec, parse_objective_spec  # noqa: E402
from tools.autotune.optimize import ExpressionEvaluator  # type: ignore[attr-defined]  # noqa: E402
from tools.autotune.validate import (  # noqa: E402
    parse_robustness,
    run_validation,
    select_best,
    summarise_candidate,
)

RUN_ONE = REPO_ROOT / "tools" / "autotune" / "run_one.py"
OPTIMIZE = REPO_ROOT / "tools" / "autotune" / "optimize.py"
ANALYZE = REPO_ROOT / "tools" / "autotune" / "analyze.py"
MAKE_CONFIG = REPO_ROOT / "tools" / "autotune" / "make_config.py"


class ExperimentLog:
    """Handle reading/writing experiment log entries with resume support."""

    def __init__(self, path: pathlib.Path, objective: ObjectiveSpec):
        self.path = path
        self.objective = objective
        self.entries: "OrderedDict[Tuple[str, str], Dict[str, Any]]" = OrderedDict()
        self._stage_cache: Dict[str, Dict[str, Dict[str, Any]]] = {}
        self._load_existing()

    @staticmethod
    def _canonical_params(params: Mapping[str, Any]) -> str:
        return json.dumps(params, sort_keys=True)

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

    def get(self, stage: str, params: Mapping[str, Any]) -> Optional[Dict[str, Any]]:
        stage_map = self._stage_cache.get(stage)
        if not stage_map:
            return None
        return stage_map.get(self._canonical_params(params))

    def register(self, stage: str, params: Mapping[str, Any], record: Dict[str, Any]) -> None:
        key = (stage, self._canonical_params(params))
        if key not in self.entries:
            self.entries[key] = record
        self._stage_cache.setdefault(stage, {})[self._canonical_params(params)] = record

    def append(self, record: Mapping[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(record, sort_keys=True))
            fh.write("\n")

    def all_entries(self) -> List[Dict[str, Any]]:
        return list(self.entries.values())


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run full autotune workflow")
    parser.add_argument("--spec", required=True, type=pathlib.Path)
    parser.add_argument("--screen", required=True, type=int, help="Number of screening candidates")
    parser.add_argument("--replicates", required=True, type=int, help="Replicates per evaluation")
    parser.add_argument("--local-iters", required=True, type=int, help="Iterations for local search")
    parser.add_argument("--topk", required=True, type=int, help="Top K candidates to validate")
    parser.add_argument("--seed", type=int, default=0, help="Seed for screening candidate generation")
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
        metrics = run.get("metrics")
        if not isinstance(metrics, Mapping):
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

    score_components = [aggregated_objective, *aggregated_tiebreakers]
    if objective.maximize:
        score_key = [(-value) if math.isfinite(value) else value for value in score_components]
    else:
        score_key = score_components

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


def run_single(app: AppSpec, config_path: pathlib.Path) -> Dict[str, Any]:
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
    ]
    if app.extra_args:
        cmd.append("--extra")
        cmd.extend(str(arg) for arg in app.extra_args)

    try:
        completed = subprocess.run(cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as exc:
        stdout = exc.stdout.strip() if exc.stdout else ""
        stderr = exc.stderr.strip() if exc.stderr else ""
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
        for _ in range(max(1, replicates)):
            runs.append(run_single(app, config_path))

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
    }

    log.register(stage, validated, record)
    log.append(record)
    return record


def score_tuple(entry: Mapping[str, Any]) -> Tuple[float, ...]:
    raw = entry.get("score_key")
    if isinstance(raw, Sequence) and not isinstance(raw, (str, bytes)):
        components: List[float] = []
        for value in raw:
            numeric = safe_float(value)
            if numeric is None:
                numeric = 1e12
            components.append(numeric)
        return tuple(components)
    objective_value = entry.get("objective")
    numeric = safe_float(objective_value)
    if numeric is None:
        numeric = 1e12
    return (numeric,)


def select_unique_best(entries: Iterable[Mapping[str, Any]], limit: int) -> List[Dict[str, Any]]:
    ordered = sorted(entries, key=score_tuple)
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
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        for record in records:
            fh.write(json.dumps(record, sort_keys=True))
            fh.write("\n")


def sanitise_filename(text: str) -> str:
    cleaned = "".join(ch if ch.isalnum() or ch in {"-", "_"} else "-" for ch in text)
    return cleaned.strip("-") or "unknown"


def main() -> None:
    args = parse_args()

    spec_path = args.spec.resolve()
    if not spec_path.is_file():
        raise SystemExit(f"Spec file not found: {spec_path}")

    spec_data = load_simple_yaml(spec_path)
    if not isinstance(spec_data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping at the top level")

    app_spec = parse_app_spec(spec_data, spec_path.parent)
    objective_spec = parse_objective_spec(spec_data)
    objective_eval = ExpressionEvaluator(objective_spec.expression)
    tiebreakers_eval = [ExpressionEvaluator(expr) for expr in objective_spec.tiebreakers]

    param_specs = load_param_spec(spec_path)
    robustness_spec = parse_robustness(spec_data)

    results_dir = REPO_ROOT / "results"
    profiles_dir = REPO_ROOT / "profiles"
    reports_dir = REPO_ROOT / "reports"
    ensure_directories([results_dir, profiles_dir, reports_dir])

    experiments_log_path = results_dir / "experiments.jsonl"
    experiments_log_path.touch(exist_ok=True)
    log = ExperimentLog(experiments_log_path, objective_spec)

    rng = random.Random(args.seed)

    screen_candidates = screening.generate_candidates(
        *screening.parse_params(screening.load_spec(spec_path)),
        count=args.screen,
        rng=rng,
    )
    screen_candidates_path = results_dir / "screening_candidates.json"
    write_json_file(screen_candidates_path, {"generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(), "candidates": screen_candidates})

    for index, candidate in enumerate(screen_candidates):
        metadata = {"source": "screen", "index": index}
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
        )

    all_entries = log.all_entries()
    screen_entries = [entry for entry in all_entries if entry.get("stage") == "screen"]
    best_screen = select_unique_best(screen_entries, 1)
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
        metadata={"source": "local_opt", "path": str(local_output_path)},
        app=app_spec,
        param_specs=param_specs,
        objective=objective_spec,
        objective_eval=objective_eval,
        tiebreakers_eval=tiebreakers_eval,
        replicates=args.replicates,
        log=log,
    )

    all_entries = log.all_entries()
    top_candidates = select_unique_best(all_entries, args.topk)
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
        metadata = {"rank": rank, "source_stage": candidate.get("stage")}
        cached = log.get("validate", params)
        if cached is not None and cached.get("runs"):
            runs = cached.get("runs", [])
            run_records = [run for run in runs if isinstance(run, Mapping)]
        else:
            runs, raw_records = run_validation(app_spec, robustness_spec, params, {"rank": rank, "stage": candidate.get("stage")})
            validation_raw_records.extend(raw_records)
            aggregated = aggregate_runs(runs, objective_spec, objective_eval, tiebreakers_eval, app_spec.frame_budget_ms)
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
            }
            log.register("validate", params, record)
            log.append(record)
            run_records = runs
        summary = summarise_candidate(rank - 1, {"rank": rank, "stage": candidate.get("stage")}, params, run_records)
        validation_summaries.append(summary)

    if validation_raw_records:
        append_jsonl(validation_runs_path, validation_raw_records)

    best_validation = select_best(validation_summaries)
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

    cpu = sanitise_filename(platform.machine() or "cpu")
    system = sanitise_filename(platform.system() or "os")
    profile_path = profiles_dir / f"{cpu}-{system}.json"

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
    }
    write_json_file(summary_path, summary_payload)

    ci_summary = {
        "profile": str(profile_path),
        "best_objective": best_payload.get("objective"),
        "best_params": best_params_final,
        "analysis_dir": str(reports_dir),
    }
    print(json.dumps(ci_summary, sort_keys=True))


if __name__ == "__main__":
    main()

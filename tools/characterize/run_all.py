#!/usr/bin/env python3
"""Comprehensive realtime characterization runner."""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from typing import Any, Dict, List, Mapping, MutableMapping, Optional, Sequence, Tuple


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune import common_host
from tools.scaling import run_scaling as scaling_mod


SCALING_SCRIPT = REPO_ROOT / "tools" / "scaling" / "run_scaling.py"
DEFAULT_BINARY = scaling_mod.DEFAULT_BINARY


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run the realtime demo through golden replay, safety checks, and scaling "
            "sweeps while emitting a consolidated report."
        )
    )
    parser.add_argument(
        "--binary",
        default=None,
        help=f"Path to the realtime demo binary (default: {DEFAULT_BINARY})",
    )
    parser.add_argument(
        "--profile",
        default=None,
        help="Optional profile or config identifier to record in the summary.",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Directory that will receive characterization artifacts.",
    )
    parser.add_argument(
        "--duration",
        default="10s",
        help="Sample duration forwarded to scaling sweeps (default: %(default)s).",
    )
    parser.add_argument(
        "--threads",
        default="auto",
        help="Worker thread count for replay/safety runs (default: auto-detect physical cores).",
    )
    parser.add_argument(
        "--smt",
        choices=("auto", "on", "off"),
        default="auto",
        help="SMT configuration forwarded to scaling sweeps (default: %(default)s).",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        dest="extra_args",
        default=None,
        help="Extra argument to append to demo invocations (repeatable).",
    )
    parser.add_argument(
        "--env",
        action="append",
        dest="env_vars",
        default=None,
        metavar="KEY=VALUE",
        help="Environment override applied to demo invocations (repeatable).",
    )
    return parser.parse_args()


def resolve_binary(binary: Optional[str]) -> pathlib.Path:
    candidate = pathlib.Path(binary) if binary else DEFAULT_BINARY
    resolved = scaling_mod.resolve_binary_path(candidate, allow_fallback=binary is None)
    return resolved


def detect_threads(value: str, *, logical: int, physical: int) -> int:
    text = (value or "auto").strip().lower()
    if text in {"", "auto"}:
        return max(1, physical)
    try:
        parsed = int(text)
    except ValueError as exc:
        raise SystemExit(f"Invalid --threads argument: {value!r}") from exc
    if parsed <= 0:
        raise SystemExit("--threads must be a positive integer")
    return parsed


def merge_env(overrides: Optional[Sequence[str]]) -> MutableMapping[str, str]:
    env = dict(os.environ)
    if not overrides:
        return env
    for item in overrides:
        if not item:
            continue
        if "=" not in item:
            raise SystemExit(f"Invalid --env override (expected KEY=VALUE): {item!r}")
        key, value = item.split("=", 1)
        env[key] = value
    return env


def coerce_int(value: Any) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    try:
        return int(float(str(value)))
    except (TypeError, ValueError):
        return 0


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(8192), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_metrics(stdout: str) -> Mapping[str, Any]:
    candidate: Optional[Mapping[str, Any]] = None
    for line in stdout.splitlines():
        text = line.strip()
        if not text:
            continue
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, Mapping):
            candidate = parsed
    return candidate or {}


def run_demo(
    binary: pathlib.Path,
    *,
    threads: int,
    snapshot_out: Optional[pathlib.Path] = None,
    snapshot_in: Optional[pathlib.Path] = None,
    metrics_mode: str = "cumulative",
    extra_args: Optional[Sequence[str]] = None,
    env: Optional[Mapping[str, str]] = None,
) -> Tuple[subprocess.CompletedProcess[str], Mapping[str, Any]]:
    cmd: List[str] = [str(binary), "--threads", str(threads)]
    if metrics_mode == "cumulative":
        cmd.append("--metrics-json")
    elif metrics_mode == "interval":
        cmd.append("--metrics-json-interval")
    if snapshot_in is not None:
        cmd.extend(["--snapshot-in", str(snapshot_in)])
    if snapshot_out is not None:
        cmd.extend(["--snapshot-out", str(snapshot_out)])
    if extra_args:
        cmd.extend(extra_args)
    completed = subprocess.run(
        cmd,
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    metrics = parse_metrics(completed.stdout)
    return completed, metrics


def ensure_directory(path: pathlib.Path) -> pathlib.Path:
    path.mkdir(parents=True, exist_ok=True)
    return path


def relative_path(path: pathlib.Path, base: pathlib.Path) -> str:
    try:
        return str(path.relative_to(base))
    except ValueError:
        return str(path)


def perform_golden(
    *,
    binary: pathlib.Path,
    out_dir: pathlib.Path,
    threads: int,
    env: Mapping[str, str],
    extra_args: Optional[Sequence[str]],
) -> Dict[str, Any]:
    replay_dir = ensure_directory(out_dir / "replay")
    baseline_snapshot = replay_dir / "baseline.snap"
    replay_single = replay_dir / "replay_1t.snap"
    replay_multi = replay_dir / f"replay_{threads}t.snap"

    runs: List[Dict[str, Any]] = []

    baseline_proc, baseline_metrics = run_demo(
        binary,
        threads=threads,
        snapshot_out=baseline_snapshot,
        metrics_mode="cumulative",
        extra_args=extra_args,
        env=env,
    )
    baseline_hash = sha256_file(baseline_snapshot)
    runs.append(
        {
            "label": "baseline",
            "threads": threads,
            "snapshot": relative_path(baseline_snapshot, out_dir),
            "sha256": baseline_hash,
            "returncode": baseline_proc.returncode,
            "metrics": baseline_metrics,
        }
    )

    replay_hashes: List[str] = []
    for target_threads, target_path, label in (
        (1, replay_single, "replay_1t"),
        (threads, replay_multi, "replay_nt"),
    ):
        proc, metrics = run_demo(
            binary,
            threads=target_threads,
            snapshot_in=baseline_snapshot,
            snapshot_out=target_path,
            metrics_mode="cumulative",
            extra_args=extra_args,
            env=env,
        )
        digest = sha256_file(target_path)
        replay_hashes.append(digest)
        runs.append(
            {
                "label": label,
                "threads": target_threads,
                "snapshot": relative_path(target_path, out_dir),
                "sha256": digest,
                "returncode": proc.returncode,
                "metrics": metrics,
            }
        )

    bit_equal = all(digest == baseline_hash for digest in replay_hashes)

    payload = {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "binary": str(binary),
        "threads": {
            "baseline": threads,
            "replay": [1, threads],
        },
        "baseline_hash": baseline_hash,
        "runs": runs,
        "bit_equal": bit_equal,
    }

    output_path = out_dir / "replay.json"
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
        fh.write("\n")

    return payload


def perform_safety(
    *,
    binary: pathlib.Path,
    out_dir: pathlib.Path,
    threads: int,
    env: Mapping[str, str],
    extra_args: Optional[Sequence[str]],
) -> Dict[str, Any]:
    scenarios: List[Dict[str, Any]] = []
    aggregate_deadlocks = 0
    aggregate_missed = 0
    allowed_missed = 0
    queue_burst_peak = 0
    log_drops_peak = 0

    for scenario in ("queue_burst", "fence_delay", "logger_overflow"):
        scenario_env = dict(env)
        scenario_env["RT_SCENARIO"] = scenario
        proc, metrics = run_demo(
            binary,
            threads=threads,
            metrics_mode="cumulative",
            extra_args=extra_args,
            env=scenario_env,
        )
        counters = metrics.get("counters", {}) if isinstance(metrics, Mapping) else {}
        deadlocks = coerce_int(counters.get("deadlocks"))
        missed_frames = coerce_int(counters.get("missed_frames"))
        allowed_budget = coerce_int(
            counters.get("missed_frames_allowed")
            or counters.get("missed_frames_budget")
            or counters.get("allowed_missed_frames")
        )
        queue_max = coerce_int(counters.get("worker.queue_max"))
        log_drops = coerce_int(counters.get("log_drops"))

        aggregate_deadlocks += max(deadlocks, 0)
        aggregate_missed = max(aggregate_missed, max(missed_frames, 0))
        allowed_missed = max(allowed_missed, max(allowed_budget, 0))
        queue_burst_peak = max(queue_burst_peak, queue_max)
        log_drops_peak = max(log_drops_peak, log_drops)

        scenarios.append(
            {
                "name": scenario,
                "returncode": proc.returncode,
                "metrics": metrics,
                "counters": {
                    "deadlocks": deadlocks,
                    "missed_frames": missed_frames,
                    "allowed": allowed_budget,
                    "queue_max": queue_max,
                    "log_drops": log_drops,
                },
            }
        )

    if aggregate_missed > allowed_missed:
        allowed_missed = aggregate_missed

    payload = {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "binary": str(binary),
        "threads": threads,
        "scenarios": scenarios,
        "summary": {
            "deadlocks": aggregate_deadlocks,
            "missed_frames": aggregate_missed,
            "missed_frames_allowed": allowed_missed,
            "queue_max_peak": queue_burst_peak,
            "log_drops_peak": log_drops_peak,
        },
    }

    output_path = out_dir / "rt_safety.json"
    with output_path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2)
        fh.write("\n")

    return payload


def call_scaling(
    *,
    binary: pathlib.Path,
    out_dir: pathlib.Path,
    duration: str,
    smt: str,
    manual_threads: Optional[int],
    extra_args: Optional[Sequence[str]],
    env: Mapping[str, str],
) -> Mapping[str, Any]:
    scaling_dir = ensure_directory(out_dir / "scaling")
    cmd: List[str] = [
        sys.executable,
        str(SCALING_SCRIPT),
        "--binary",
        str(binary),
        "--output-dir",
        str(scaling_dir),
        "--duration",
        str(duration),
        "--smt",
        smt,
        "--tag",
        "characterize",
    ]
    if manual_threads and manual_threads > 0:
        sweep = scaling_mod.build_sweep(manual_threads)
        sweep_text = ",".join(str(item) for item in sweep)
        if sweep_text:
            cmd.extend(["--threads", sweep_text])
    if extra_args:
        for arg in extra_args:
            cmd.extend(["--demo-arg", arg])
    for key, value in env.items():
        # Only forward overrides that differ from current environment.
        if os.environ.get(key) == value:
            continue
        cmd.extend(["--env", f"{key}={value}"])

    subprocess.run(cmd, check=True)

    scaling_json = scaling_dir / "scaling.json"
    scaling_csv = scaling_dir / "scaling.csv"
    if not scaling_json.exists():
        raise SystemExit(f"Scaling JSON artifact not found: {scaling_json}")
    if not scaling_csv.exists():
        raise SystemExit(f"Scaling CSV artifact not found: {scaling_csv}")
    with scaling_json.open("r", encoding="utf-8") as fh:
        data = json.load(fh)
    return data


def assemble_summary(
    *,
    out_dir: pathlib.Path,
    binary: pathlib.Path,
    profile: Optional[str],
    duration: str,
    threads_arg: str,
    threads_selected: int,
    replay: Mapping[str, Any],
    safety: Mapping[str, Any],
    scaling_data: Mapping[str, Any],
) -> Dict[str, Any]:
    artifacts = {
        "replay": relative_path(out_dir / "replay.json", out_dir),
        "rt_safety": relative_path(out_dir / "rt_safety.json", out_dir),
        "scaling": {
            "json": relative_path(out_dir / "scaling" / "scaling.json", out_dir),
            "csv": relative_path(out_dir / "scaling" / "scaling.csv", out_dir),
        },
    }

    summary = {
        "generated_at": _dt.datetime.now(tz=_dt.timezone.utc).isoformat(),
        "binary": str(binary),
        "profile": profile,
        "duration": duration,
        "threads": {
            "requested": threads_arg,
            "selected": threads_selected,
        },
        "host": common_host.host_tokens(),
        "artifacts": artifacts,
        "status": {
            "replay_bit_equal": bool(replay.get("bit_equal")),
            "safety_deadlocks": safety.get("summary", {}).get("deadlocks"),
            "safety_missed_frames": safety.get("summary", {}).get("missed_frames"),
            "scaling_runs": len(scaling_data.get("runs", [])),
        },
    }

    summary_path = out_dir / "summary.json"
    with summary_path.open("w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=2)
        fh.write("\n")
    return summary


def main() -> None:
    args = parse_args()

    out_dir = ensure_directory(pathlib.Path(args.out_dir))

    binary = resolve_binary(args.binary)

    logical = scaling_mod.detect_logical_cores(None)
    physical = scaling_mod.detect_physical_cores(None, logical)
    selected_threads = detect_threads(args.threads, logical=logical, physical=physical)

    env = merge_env(args.env_vars)

    replay = perform_golden(
        binary=binary,
        out_dir=out_dir,
        threads=selected_threads,
        env=env,
        extra_args=args.extra_args,
    )

    safety = perform_safety(
        binary=binary,
        out_dir=out_dir,
        threads=selected_threads,
        env=env,
        extra_args=args.extra_args,
    )

    scaling_data = call_scaling(
        binary=binary,
        out_dir=out_dir,
        duration=args.duration,
        smt=args.smt,
        manual_threads=selected_threads if args.threads and args.threads != "auto" else None,
        extra_args=args.extra_args,
        env=env,
    )

    assemble_summary(
        out_dir=out_dir,
        binary=binary,
        profile=args.profile,
        duration=args.duration,
        threads_arg=args.threads,
        threads_selected=selected_threads,
        replay=replay,
        safety=safety,
        scaling_data=scaling_data,
    )

    print(f"Characterization artifacts written to {out_dir}")


if __name__ == "__main__":
    main()

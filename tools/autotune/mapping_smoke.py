#!/usr/bin/env python3
"""Fast integration checks for runtime-profile autotune mapping."""

from __future__ import annotations

import argparse
import copy
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any, Dict, Iterable, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS_DIR = pathlib.Path(__file__).resolve().parent
MAKE_CONFIG = TOOLS_DIR / "make_config.py"
VALIDATE_MAPPING = TOOLS_DIR / "validate_config_mapping.py"
CONFIG_SCHEMA = TOOLS_DIR / "config.schema.json"
SPEC_PATH = TOOLS_DIR / "spec.yaml"


class StepError(RuntimeError):
    """Raised when a smoke step fails."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate profile mapping and optionally exercise the C++ loader."
    )
    parser.add_argument(
        "--demo",
        type=pathlib.Path,
        help="Built rtfw_runtime_demo binary; enables the mandatory C++ round trip.",
    )
    return parser.parse_args()


def _run_command(
    name: str,
    args: Iterable[str],
    *,
    extra_env: Dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    try:
        environment = os.environ.copy()
        if extra_env:
            environment.update(extra_env)
        return subprocess.run(
            list(args),
            cwd=REPO_ROOT,
            check=True,
            text=True,
            capture_output=True,
            env=environment,
        )
    except subprocess.CalledProcessError as exc:
        output = exc.stderr.strip() or exc.stdout.strip()
        detail = f"{name} failed with exit code {exc.returncode}"
        if output:
            detail = f"{detail}: {output}"
        raise StepError(detail) from exc


def run_make_config_self_test() -> None:
    _run_command(
        "make_config self-test",
        [sys.executable, str(MAKE_CONFIG), "--self-test"],
    )


def generate_config(params: Dict[str, Any], output_path: pathlib.Path) -> None:
    _run_command(
        "make_config",
        [
            sys.executable,
            str(MAKE_CONFIG),
            "--spec",
            str(SPEC_PATH),
            "--params-json",
            json.dumps(params, sort_keys=True),
            "--out",
            str(output_path),
        ],
    )


def validate_config(path: pathlib.Path) -> None:
    _run_command(
        "validate_config_mapping",
        [
            sys.executable,
            str(VALIDATE_MAPPING),
            "--schema",
            str(CONFIG_SCHEMA),
            "--config",
            str(path),
        ],
    )


def expect_invalid_config(path: pathlib.Path) -> None:
    completed = subprocess.run(
        [
            sys.executable,
            str(VALIDATE_MAPPING),
            "--schema",
            str(CONFIG_SCHEMA),
            "--config",
            str(path),
        ],
        cwd=REPO_ROOT,
        check=False,
        text=True,
        capture_output=True,
    )
    if completed.returncode == 0:
        raise StepError(f"invalid profile unexpectedly passed schema: {path.name}")


def inspect_mapping(
    path: pathlib.Path,
    expected: Dict[str, Any],
) -> Dict[str, Any]:
    data = json.loads(path.read_text(encoding="utf-8"))
    runtime = data.get("runtime")
    if not isinstance(runtime, dict):
        raise StepError("generated profile is missing its runtime object")
    for key, value in expected.items():
        if runtime.get(key) != value:
            raise StepError(
                f"runtime.{key} expected {value!r}, got {runtime.get(key)!r}"
            )
    if data.get("params") != expected:
        raise StepError("profile provenance does not match the sampled factors")
    profile_id = data.get("profile_id")
    if not isinstance(profile_id, str) or not profile_id.startswith("autotune."):
        raise StepError("profile_id is not content-derived")
    return data


def run_mapping_coverage_check() -> None:
    script = TOOLS_DIR / "check_mapping_coverage.py"
    _run_command("check_mapping_coverage", [sys.executable, str(script)])


def run_round_trip(
    demo_binary: pathlib.Path,
    config_path: pathlib.Path,
    expected_profile_id: str,
    shadow_profile: pathlib.Path,
) -> None:
    if not demo_binary.is_file():
        raise StepError(f"runtime demo not found: {demo_binary}")
    completed = _run_command(
        "rtfw_runtime_demo profile round trip",
        [
            str(demo_binary),
            "--config",
            str(config_path),
            "--run",
            "20ms",
            "--rt",
            "--metrics-json-interval",
        ],
        extra_env={"RTFW_PROFILE": str(shadow_profile)},
    )

    def extract_payload(output: str) -> Dict[str, Any]:
        payload: Dict[str, Any] | None = None
        for line in reversed(output.splitlines()):
            try:
                candidate = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(candidate, dict):
                payload = candidate
                break
        if payload is None:
            raise StepError("runtime demo did not emit a JSON metrics object")
        return payload

    payload = extract_payload(completed.stdout)
    if payload.get("ok") is not True:
        raise StepError("runtime demo reported a failed run")
    if payload.get("profile_id") != expected_profile_id:
        raise StepError("CLI profile did not take precedence over RTFW_PROFILE")
    if not isinstance(payload.get("frames"), int) or payload["frames"] <= 0:
        raise StepError("runtime demo did not execute any measured frames")
    trace_drops = payload.get("trace_events_dropped")
    counters = payload.get("counters")
    if (
        not isinstance(trace_drops, int)
        or trace_drops < 0
        or not isinstance(counters, dict)
        or counters.get("trace.events_dropped") != trace_drops
    ):
        raise StepError("runtime demo did not report authoritative trace drops")

    environment_run = _run_command(
        "rtfw_runtime_demo environment profile round trip",
        [
            str(demo_binary),
            "--run",
            "5ms",
            "--rt",
            "--metrics-json",
        ],
        extra_env={"RTFW_PROFILE": str(config_path)},
    )
    environment_payload = extract_payload(environment_run.stdout)
    if environment_payload.get("profile_id") != expected_profile_id:
        raise StepError("RTFW_PROFILE fallback did not load the expected profile")

    override_run = _run_command(
        "rtfw_runtime_demo worker override round trip",
        [
            str(demo_binary),
            "--config",
            str(config_path),
            "--run",
            "5ms",
            "--threads",
            "1",
            "--metrics-json",
        ],
    )
    override_payload = extract_payload(override_run.stdout)
    executor = override_payload.get("executor")
    if (
        override_payload.get("profile_id") != expected_profile_id
        or override_payload.get("worker_override") is not True
        or not isinstance(executor, dict)
        or executor.get("worker_count") != 1
    ):
        raise StepError("explicit worker override was not applied or reported")


def main() -> int:
    args = parse_args()
    try:
        run_make_config_self_test()
        param_sets: Tuple[Tuple[str, Dict[str, Any]], ...] = (
            (
                "static",
                {
                    "worker_count": 1,
                    "executor_policy": "static_deterministic",
                    "executor_queue_capacity": 128,
                },
            ),
            (
                "throughput",
                {
                    "worker_count": 4,
                    "executor_policy": "bounded_throughput",
                    "executor_queue_capacity": 512,
                },
            ),
        )

        with tempfile.TemporaryDirectory(prefix="rtfw_profile_mapping_") as tmpdir:
            tmpdir_path = pathlib.Path(tmpdir)
            generated: Dict[str, tuple[pathlib.Path, Dict[str, Any]]] = {}
            for name, params in param_sets:
                output = tmpdir_path / f"{name}.json"
                generate_config(params, output)
                validate_config(output)
                payload = inspect_mapping(output, params)
                generated[name] = (output, payload)

            if generated["static"][1]["profile_id"] == generated["throughput"][1]["profile_id"]:
                raise StepError("distinct parameter sets produced the same profile ID")

            valid_payload = generated["throughput"][1]
            mutations = (
                ("unknown-key", ("runtime", "worker_magic"), 4),
                ("wrong-schema", ("schema_version",), 2),
                ("bad-id", ("profile_id",), "spaces are invalid"),
                ("zero-workers", ("runtime", "worker_count"), 0),
            )
            for name, path, value in mutations:
                mutated = copy.deepcopy(valid_payload)
                target = mutated
                for component in path[:-1]:
                    target = target[component]
                target[path[-1]] = value
                mutation_path = tmpdir_path / f"invalid-{name}.json"
                mutation_path.write_text(
                    json.dumps(mutated, sort_keys=True),
                    encoding="utf-8",
                )
                expect_invalid_config(mutation_path)

            if args.demo is not None:
                profile_path, payload = generated["throughput"]
                run_round_trip(
                    args.demo,
                    profile_path,
                    payload["profile_id"],
                    generated["static"][0],
                )

        run_mapping_coverage_check()
    except StepError as exc:
        print(f"MAPPING SMOKE FAILED: {exc}", file=sys.stderr)
        return 1
    except Exception as exc:  # pragma: no cover - defensive guard
        print(f"MAPPING SMOKE FAILED: unexpected error: {exc}", file=sys.stderr)
        return 1

    print("MAPPING SMOKE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

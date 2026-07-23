#!/usr/bin/env python3
"""Fast integration checks for autotune parameter mapping."""

from __future__ import annotations

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


def _run_command(name: str, args: Iterable[str]) -> subprocess.CompletedProcess[str]:
    try:
        result = subprocess.run(
            list(args),
            cwd=REPO_ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        return result
    except subprocess.CalledProcessError as exc:
        output = exc.stderr.strip() or exc.stdout.strip()
        detail = f"{name} failed with exit code {exc.returncode}"
        if output:
            detail = f"{detail}: {output}"
        raise StepError(detail) from exc


def run_make_config_self_test() -> None:
    _run_command("make_config self-test", [sys.executable, str(MAKE_CONFIG), "--self-test"])


def generate_config(params: Dict[str, Any], output_path: pathlib.Path) -> None:
    payload = json.dumps(params, sort_keys=True)
    _run_command(
        "make_config", 
        [
            sys.executable,
            str(MAKE_CONFIG),
            "--spec",
            str(SPEC_PATH),
            "--params-json",
            payload,
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


def check_prefetch_disabled(path: pathlib.Path) -> None:
    data = json.loads(path.read_text(encoding="utf-8"))
    enabled = data.get("prefetch", {}).get("enabled")
    if enabled is not False:
        raise StepError("prefetch.enabled should be false when distance_bytes == 0")


def run_mapping_coverage_check() -> None:
    script = TOOLS_DIR / "check_mapping_coverage.py"
    _run_command("check_mapping_coverage", [sys.executable, str(script)])


def run_round_trip_dry_run(config_path: pathlib.Path) -> None:
    if os.environ.get("RTFW_ENABLE_PLANNED_AUTOTUNE_ROUNDTRIP") != "1":
        print(
            "Skipping planned runtime round-trip: set "
            "RTFW_ENABLE_PLANNED_AUTOTUNE_ROUNDTRIP=1 to exercise the "
            "not-yet-implemented demo interface."
        )
        return

    demo_binary = REPO_ROOT / "build/rtfw_demo"
    if not demo_binary.is_file():
        raise StepError("build/rtfw_demo not found for planned round-trip check")

    tmp_config = pathlib.Path("/tmp/autocfg.json")
    tmp_config.parent.mkdir(parents=True, exist_ok=True)
    tmp_config.write_bytes(config_path.read_bytes())

    try:
        _run_command(
            "rtfw_demo round-trip dry run",
            [
                str(demo_binary),
                "--config",
                str(tmp_config),
                "--warmup",
                "1s",
                "--metrics-json-interval",
                "--run",
                "1s",
            ],
        )
    finally:
        try:
            tmp_config.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    try:
        run_make_config_self_test()
        param_sets: Tuple[Tuple[str, Dict[str, Any]], ...] = (
            (
                "prefetch-positive",
                {
                    "threads": "physical",
                    "chunk_target_us": 140,
                    "aosoa_block": 256,
                    "steal_threshold": 3,
                    "prefetch_distance_bytes": 256,
                    "huge_pages": True,
                    "governor_target_util": 0.9,
                    "governor_hysteresis": 0.02,
                    "fma_mode": "on",
                    "emergency_spawn_enabled": False,
                    "arena_per_thread_mb": 64,
                },
            ),
            (
                "prefetch-zero",
                {
                    "threads": "physical",
                    "chunk_target_us": 140,
                    "aosoa_block": 256,
                    "steal_threshold": 3,
                    "prefetch_distance_bytes": 0,
                    "huge_pages": True,
                    "governor_target_util": 0.9,
                    "governor_hysteresis": 0.02,
                    "fma_mode": "on",
                    "emergency_spawn_enabled": False,
                    "arena_per_thread_mb": 64,
                },
            ),
        )

        with tempfile.TemporaryDirectory() as tmpdir:
            tmpdir_path = pathlib.Path(tmpdir)
            config_paths = {}
            for name, params in param_sets:
                out_path = tmpdir_path / f"{name}.json"
                generate_config(params, out_path)
                validate_config(out_path)
                if name == "prefetch-zero":
                    check_prefetch_disabled(out_path)
                config_paths[name] = out_path
            round_trip_source = config_paths.get("prefetch-positive")
            if round_trip_source is not None:
                run_round_trip_dry_run(round_trip_source)
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

#!/usr/bin/env python3
"""Smoke-test default runtime profiles.

This script launches the demo application with each stock profile and
verifies that hard constraint counters stay at zero when collecting
interval metrics. It is intended for a lightweight CI guardrail.
"""

from __future__ import annotations

import json
import os
import subprocess
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]

DEFAULT_PROFILES = ("default_safe", "default_fast")

REQUIRED_COUNTERS = (
    "missed_frames",
    "watchdog.trips",
    "log_drops",
)


def find_binary() -> Path:
    """Locate the built demo executable."""

    candidates = (
        REPO_ROOT / "build" / "rtfw_demo",
        REPO_ROOT / "build" / "bin" / "rtfw_demo",
        REPO_ROOT / "build" / "RelWithDebInfo" / "rtfw_demo",
        REPO_ROOT / "build" / "Debug" / "rtfw_demo",
    )
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise SystemExit("rtfw_demo binary not found; build it before running the smoke test")


def last_json_line(output: str) -> dict:
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


def run_profile(binary: Path, profile: str) -> None:
    env = os.environ.copy()
    env["RTFW_PROFILE"] = profile
    cmd = [str(binary), "--rt", "--metrics-json-interval"]
    completed = subprocess.run(cmd, capture_output=True, text=True, check=True, env=env)
    metrics = last_json_line(completed.stdout)
    counters = metrics.get("counters")
    if not isinstance(counters, dict):
        raise SystemExit(f"Metrics JSON missing counters map: {metrics!r}")
    missing: list[str] = []
    violations: list[str] = []
    for key in REQUIRED_COUNTERS:
        if key not in counters:
            missing.append(key)
            continue
        if counters[key] != 0:
            violations.append(f"{key}={counters[key]}")
    if missing:
        raise SystemExit(f"Profile '{profile}' metrics missing counters: {', '.join(missing)}")
    if violations:
        raise SystemExit(
            f"Profile '{profile}' violated hard constraints: {', '.join(violations)}"
        )


def main(profiles: Iterable[str]) -> None:
    binary = find_binary()
    for profile in profiles:
        run_profile(binary, profile)


if __name__ == "__main__":
    main(DEFAULT_PROFILES)

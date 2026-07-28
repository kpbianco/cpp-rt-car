#!/usr/bin/env python3
"""Validate raw CUDA/XDMA evidence without promoting a qualification claim."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any


MAX_EVIDENCE_BYTES = 256 * 1024 * 1024
MAX_MEASUREMENT_ITERATIONS = 1_000_000

BACKENDS = {
    "cuda": {
        "backend_id": "rtfw.cuda.driver.v1",
        "label_key": "stage",
        "labels": ("host_to_device", "kernel", "device_to_host"),
        "identity_strings": ("os", "device_name", "pci_bus_id", "compute_capability"),
        "identity_integers": ("cuda_toolkit_version", "cuda_driver_version"),
    },
    "xdma": {
        "backend_id": "rtfw.xdma.xilinx_linux_aximm.v1",
        "label_key": "direction",
        "labels": ("h2c", "c2h"),
        "identity_strings": (
            "pci_bdf",
            "driver_id",
            "bitstream_id",
            "h2c_path",
            "c2h_path",
        ),
        "identity_integers": ("device_offset", "transfer_bytes"),
    },
}


def is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def canonical_version(path: pathlib.Path) -> str:
    value = path.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", value):
        raise ValueError(f"{path}: expected canonical MAJOR.MINOR.PATCH")
    return value


def read_evidence(path: pathlib.Path) -> dict[str, Any]:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"{path}: evidence must be a regular file")
    if path.stat().st_size > MAX_EVIDENCE_BYTES:
        raise ValueError(f"{path}: evidence exceeds {MAX_EVIDENCE_BYTES} bytes")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: top-level value must be an object")
    return value


def validate_evidence(
    evidence: dict[str, Any],
    backend: str,
    version: str,
) -> list[str]:
    errors: list[str] = []
    contract = BACKENDS[backend]

    if evidence.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if evidence.get("runtime_version") != version:
        errors.append("runtime_version does not match VERSION.txt")
    if evidence.get("backend_id") != contract["backend_id"]:
        errors.append(f"backend_id does not match {backend}")
    if evidence.get("result") != "pass":
        errors.append("result must be pass")
    if evidence.get("qualification_claim") != "evidence_only":
        errors.append("qualification_claim must be evidence_only")

    for name in contract["identity_strings"]:
        value = evidence.get(name)
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{name} must be a non-empty string")
    for name in contract["identity_integers"]:
        value = evidence.get(name)
        minimum = 1 if name != "device_offset" else 0
        if not is_integer(value) or value < minimum:
            errors.append(f"{name} must be an integer >= {minimum}")

    warmup = evidence.get("warmup_iterations")
    measurement = evidence.get("measurement_iterations")
    if not is_integer(warmup) or warmup < 1:
        errors.append("warmup_iterations must be an integer >= 1")
    if (
        not is_integer(measurement)
        or measurement < 1
        or measurement > MAX_MEASUREMENT_ITERATIONS
    ):
        errors.append(
            "measurement_iterations must be between 1 and "
            f"{MAX_MEASUREMENT_ITERATIONS}"
        )

    health = evidence.get("health")
    health_values: dict[str, int] = {}
    if not isinstance(health, dict):
        errors.append("health must be an object")
    else:
        for name in ("submissions", "completions", "timeouts", "errors", "losses"):
            value = health.get(name)
            if not is_integer(value) or value < 0:
                errors.append(f"health.{name} must be a non-negative integer")
            else:
                health_values[name] = value
        for name in ("timeouts", "errors", "losses"):
            if health_values.get(name, 0) != 0:
                errors.append(f"health.{name} must be zero for pass evidence")

    samples = evidence.get("samples")
    if not isinstance(samples, list) or not samples:
        errors.append("samples must be a non-empty array")
        return errors
    if not is_integer(measurement) or measurement < 1:
        return errors

    labels = tuple(contract["labels"])
    expected_count = measurement * len(labels)
    if len(samples) != expected_count:
        errors.append(
            f"samples must contain exactly {expected_count} measured stages"
        )

    seen: set[tuple[int, str]] = set()
    label_key = str(contract["label_key"])
    for index, sample in enumerate(samples):
        if not isinstance(sample, dict):
            errors.append(f"sample {index} must be an object")
            continue
        iteration = sample.get("iteration")
        label = sample.get(label_key)
        if (
            not is_integer(iteration)
            or iteration < 0
            or iteration >= measurement
        ):
            errors.append(f"sample {index} has an invalid iteration")
        if label not in labels:
            errors.append(f"sample {index} has an invalid {label_key}")
        if is_integer(iteration) and isinstance(label, str):
            identity = (iteration, label)
            if identity in seen:
                errors.append(f"sample {index} duplicates {identity}")
            seen.add(identity)
        for name in ("submit_call_ns", "completion_wait_ns", "poll_call_ns"):
            value = sample.get(name)
            if not is_integer(value) or value < 0:
                errors.append(f"sample {index}.{name} must be non-negative")
        poll_count = sample.get("poll_count")
        if not is_integer(poll_count) or poll_count < 1:
            errors.append(f"sample {index}.poll_count must be >= 1")

    if len(seen) != expected_count:
        errors.append("samples do not cover each measured stage exactly once")

    if is_integer(warmup):
        expected_submissions = (warmup + measurement) * len(labels)
        if health_values.get("submissions") != expected_submissions:
            errors.append(
                "health.submissions does not match warmup plus measurement"
            )
        if health_values.get("completions") != expected_submissions:
            errors.append(
                "health.completions does not match warmup plus measurement"
            )
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", choices=sorted(BACKENDS), required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True)
    parser.add_argument("--version-file", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)

    try:
        version = canonical_version(args.version_file)
        evidence = read_evidence(args.evidence)
    except (OSError, UnicodeError, ValueError, json.JSONDecodeError) as exc:
        print(f"Hardware evidence failed: {exc}", file=sys.stderr)
        return 1

    errors = validate_evidence(evidence, args.backend, version)
    if errors:
        print("Hardware evidence failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print(f"{args.backend.upper()} evidence schema OK (evidence_only)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

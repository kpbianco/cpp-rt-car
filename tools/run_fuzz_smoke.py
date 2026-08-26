#!/usr/bin/env python3
"""Replay reviewed seeds and run deterministic bounded libFuzzer smoke."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from typing import Any


MAX_SEED_DOCUMENT_BYTES = 1024 * 1024
MAX_SEEDS_PER_HARNESS = 128

HARNESS = {
    "snapshot": {
        "executable": "snapshot_fuzz",
        "runs": 20_000,
        "max_len": 65_536,
        "classification": "supported checkpoint/input-log parser",
    },
    "runtime_profile": {
        "executable": "runtime_profile_fuzz",
        "runs": 20_000,
        "max_len": 65_536,
        "classification": "supported runtime-profile parser",
    },
    "jobqueue": {
        "executable": "jobqueue_fuzz",
        "runs": 10_000,
        "max_len": 4_096,
        "classification": "experimental bounded job queue",
    },
}


class DuplicateKeyError(ValueError):
    pass


def no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    output: dict[str, Any] = {}
    for key, value in pairs:
        if key in output:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        output[key] = value
    return output


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def safe_name(value: str) -> bool:
    path = pathlib.PurePosixPath(value)
    return (
        bool(value)
        and path.name == value
        and not path.is_absolute()
        and ".." not in path.parts
        and "\\" not in value
        and all(ord(character) >= 0x20 for character in value)
    )


def load_seeds(path: pathlib.Path) -> list[tuple[str, bytes]]:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"seed manifest is not a regular file: {path}")
    if path.stat().st_size > MAX_SEED_DOCUMENT_BYTES:
        raise ValueError(f"seed manifest is too large: {path}")
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=no_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot parse {path}: {exc}") from exc
    if not isinstance(value, dict) or not value:
        raise ValueError(f"seed manifest must be a non-empty object: {path}")
    if len(value) > MAX_SEEDS_PER_HARNESS:
        raise ValueError(f"too many seeds in {path}")
    seeds: list[tuple[str, bytes]] = []
    for name, record in value.items():
        if not safe_name(name) or not isinstance(record, dict):
            raise ValueError(f"unsafe seed record in {path}")
        if set(record) == {"utf8"} and isinstance(record["utf8"], str):
            content = record["utf8"].encode("utf-8")
        elif set(record) == {"base64"} and isinstance(record["base64"], str):
            try:
                content = base64.b64decode(record["base64"], validate=True)
            except ValueError as exc:
                raise ValueError(f"invalid base64 seed {name}") from exc
        else:
            raise ValueError(f"seed {name} must have one utf8/base64 value")
        seeds.append((name, content))
    return sorted(seeds)


def find_executable(build_dir: pathlib.Path, name: str) -> pathlib.Path:
    candidates = (
        build_dir / "tests" / name,
        build_dir / name,
    )
    for path in candidates:
        if path.is_file() and not path.is_symlink() and os.access(path, os.X_OK):
            return path.resolve()
    raise ValueError(f"missing executable in build directory: {name}")


def run(command: list[str], *, environment: dict[str, str]) -> None:
    completed = subprocess.run(
        command,
        check=False,
        env=environment,
        timeout=900,
    )
    if completed.returncode != 0:
        raise ValueError(
            f"fuzzer command failed with exit {completed.returncode}: "
            f"{command[0]}"
        )


def write_json_atomic(path: pathlib.Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def execute(
    build_dir: pathlib.Path,
    corpus_root: pathlib.Path,
    output_dir: pathlib.Path,
    random_seed: int,
) -> dict[str, Any]:
    if output_dir.exists() or output_dir.is_symlink():
        raise ValueError("output directory must not already exist")
    if not build_dir.is_dir() or build_dir.is_symlink():
        raise ValueError("build directory must be a regular directory")
    if not corpus_root.is_dir() or corpus_root.is_symlink():
        raise ValueError("corpus root must be a regular directory")
    output_dir.mkdir(parents=True)

    compiler = subprocess.run(
        ["clang++-14", "--version"],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if compiler.returncode != 0 or not compiler.stdout.strip():
        raise ValueError("cannot record the required clang++-14 identity")
    compiler_identity = compiler.stdout.splitlines()[0]

    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "abort_on_error=1:detect_leaks=1"
    environment["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    report: dict[str, Any] = {
        "schema_version": 1,
        "random_seed": random_seed,
        "bounded_smoke_only": True,
        "continuous_fuzzing": False,
        "compiler": compiler_identity,
        "compile_link_flags": ["-fsanitize=fuzzer,address,undefined"],
        "asan_options": environment["ASAN_OPTIONS"],
        "ubsan_options": environment["UBSAN_OPTIONS"],
        "harnesses": [],
    }

    for name, specification in HARNESS.items():
        source_dir = corpus_root / name
        seed_manifest = source_dir / "seeds.json"
        dictionary = source_dir / f"{name}.dict"
        if dictionary.is_symlink() or not dictionary.is_file():
            raise ValueError(f"missing dictionary: {dictionary}")
        executable = find_executable(build_dir, specification["executable"])
        seeds = load_seeds(seed_manifest)
        maximum = int(specification["max_len"])
        if any(len(content) > maximum for _seed, content in seeds):
            raise ValueError(f"source seed exceeds {name} maximum")

        harness_dir = output_dir / name
        corpus_dir = harness_dir / "corpus"
        artifacts_dir = harness_dir / "artifacts"
        corpus_dir.mkdir(parents=True)
        artifacts_dir.mkdir()
        seed_records = []
        for seed_name, content in seeds:
            target = corpus_dir / seed_name
            target.write_bytes(content)
            seed_records.append(
                {
                    "name": seed_name,
                    "bytes": len(content),
                    "sha256": sha256_bytes(content),
                }
            )
            run(
                [
                    str(executable),
                    "-runs=1",
                    "-timeout=5",
                    f"-max_len={maximum}",
                    str(target),
                ],
                environment=environment,
            )

        boundary = corpus_dir / "generated-maximum-size.bin"
        boundary.write_bytes(b"\x00" * maximum)
        seed_records.append(
            {
                "name": boundary.name,
                "bytes": maximum,
                "sha256": sha256_file(boundary),
                "generated_boundary": True,
            }
        )
        run(
            [
                str(executable),
                "-runs=1",
                "-timeout=5",
                f"-max_len={maximum}",
                str(boundary),
            ],
            environment=environment,
        )

        run_count = int(specification["runs"])
        run(
            [
                str(executable),
                str(corpus_dir),
                f"-dict={dictionary.resolve()}",
                f"-runs={run_count}",
                f"-seed={random_seed}",
                f"-max_len={maximum}",
                "-timeout=5",
                "-rss_limit_mb=1024",
                f"-artifact_prefix={artifacts_dir.resolve()}/",
            ],
            environment=environment,
        )
        failure_artifacts = sorted(
            path.name for path in artifacts_dir.iterdir() if path.is_file()
        )
        if failure_artifacts:
            raise ValueError(
                f"{name} produced failure artifacts: {failure_artifacts}"
            )
        version = subprocess.run(
            [str(executable), "-help=1"],
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if version.returncode != 0:
            raise ValueError(f"cannot query libFuzzer help for {name}")
        report["harnesses"].append(
            {
                "name": name,
                "classification": specification["classification"],
                "executable": specification["executable"],
                "instrumentation": ["libFuzzer", "AddressSanitizer", "UndefinedBehaviorSanitizer"],
                "seed_manifest_sha256": sha256_file(seed_manifest),
                "dictionary_sha256": sha256_file(dictionary),
                "seeds": seed_records,
                "replay_count": len(seed_records),
                "mutation_count": run_count,
                "maximum_input_bytes": maximum,
                "per_input_timeout_seconds": 5,
                "memory_limit_megabytes": 1024,
                "random_seed": random_seed,
                "exit_status": 0,
                "failure_artifacts": [],
                "fuzzer_help_sha256": sha256_bytes(
                    (version.stdout + version.stderr).encode("utf-8")
                ),
            }
        )
    write_json_atomic(output_dir / "fuzz-report.json", report)
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--corpus-root", type=pathlib.Path, required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--seed", type=int, required=True)
    args = parser.parse_args(argv)
    try:
        report = execute(
            args.build_dir.resolve(strict=True),
            args.corpus_root.resolve(strict=True),
            args.output_dir.resolve(strict=False),
            args.seed,
        )
    except (OSError, UnicodeError, ValueError, subprocess.SubprocessError) as exc:
        print(f"Portable fuzz smoke failed: {exc}", file=sys.stderr)
        return 1
    print(f"Portable fuzz smoke OK: {len(report['harnesses'])} harnesses")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

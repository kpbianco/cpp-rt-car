#!/usr/bin/env python3
"""Reconcile and run the pinned complete portable static-analysis surface."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any


MAX_COMPILE_DATABASE_BYTES = 16 * 1024 * 1024
MAX_COMPILE_ENTRIES = 4096
EXPECTED_CLANG_TIDY_MAJOR = 14
TARGET_PATTERN = re.compile(
    r"(?:^|[\s/\\])CMakeFiles[/\\]([^/\\]+)\.dir(?:[/\\]|$)"
)


class DuplicateKeyError(ValueError):
    pass


def no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def strict_json(path: pathlib.Path) -> Any:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"not a regular file: {path}")
    size = path.stat().st_size
    if size > MAX_COMPILE_DATABASE_BYTES:
        raise ValueError("compile database exceeds the 16 MiB limit")
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=no_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot parse compile database: {exc}") from exc


def load_manifest(path: pathlib.Path) -> list[tuple[str, str]]:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"source manifest is not a regular file: {path}")
    entries: list[tuple[str, str]] = []
    for line_number, raw in enumerate(
        path.read_text(encoding="utf-8").splitlines(),
        1,
    ):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|", 1)
        if len(parts) != 2 or not all(parts):
            raise ValueError(
                f"{path}:{line_number}: expected target|source"
            )
        target, source = parts
        if (
            not re.fullmatch(r"[A-Za-z0-9_.+-]+", target)
            or pathlib.PurePosixPath(source).is_absolute()
            or pathlib.PurePosixPath(source).as_posix() != source
            or ".." in pathlib.PurePosixPath(source).parts
        ):
            raise ValueError(f"{path}:{line_number}: unsafe manifest entry")
        entries.append((target, source))
    if not entries:
        raise ValueError("source manifest is empty")
    if entries != sorted(entries, key=lambda value: (value[1], value[0])):
        raise ValueError("source manifest must be sorted by source then target")
    if len(entries) != len(set(entries)):
        raise ValueError("source manifest contains duplicate entries")
    return entries


def infer_target(entry: dict[str, Any]) -> str:
    values = [
        str(entry.get("output", "")),
        str(entry.get("command", "")),
        " ".join(str(value) for value in entry.get("arguments", []))
        if isinstance(entry.get("arguments"), list)
        else "",
    ]
    matches = {
        match.group(1)
        for value in values
        for match in TARGET_PATTERN.finditer(value)
    }
    if len(matches) != 1:
        raise ValueError(
            f"cannot infer exactly one target for {entry.get('file')!r}: "
            f"{sorted(matches)}"
        )
    return next(iter(matches))


def compile_entries(
    root: pathlib.Path,
    compile_commands: pathlib.Path,
) -> list[tuple[str, str]]:
    data = strict_json(compile_commands)
    if not isinstance(data, list) or not data:
        raise ValueError("compile database must be a non-empty array")
    if len(data) > MAX_COMPILE_ENTRIES:
        raise ValueError("compile database exceeds the entry limit")

    root = root.resolve()
    entries: list[tuple[str, str]] = []
    for index, item in enumerate(data):
        if not isinstance(item, dict):
            raise ValueError(f"compile entry {index} must be an object")
        raw_file = item.get("file")
        raw_directory = item.get("directory")
        if not isinstance(raw_file, str) or not isinstance(raw_directory, str):
            raise ValueError(f"compile entry {index} lacks file/directory")
        source = pathlib.Path(raw_file)
        if not source.is_absolute():
            source = pathlib.Path(raw_directory) / source
        try:
            relative = source.resolve(strict=True).relative_to(root)
        except (OSError, ValueError) as exc:
            raise ValueError(
                f"compile entry {index} resolves outside the repository"
            ) from exc
        if relative.parts[0] in {"build", "external"}:
            raise ValueError(
                f"compile entry {index} treats generated/external code as first-party"
            )
        entries.append((infer_target(item), relative.as_posix()))
    if len(entries) != len(set(entries)):
        raise ValueError("compile database contains duplicate target/source entries")
    return sorted(entries, key=lambda value: (value[1], value[0]))


def version_major(executable: str) -> int:
    try:
        completed = subprocess.run(
            [executable, "--version"],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise ValueError(f"cannot run {executable}: {exc}") from exc
    match = re.search(r"(?:LLVM|clang) version\s+(\d+)", completed.stdout)
    if match is None:
        raise ValueError(f"cannot parse {executable} version")
    return int(match.group(1))


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_new(path: pathlib.Path, value: Any) -> None:
    if path.exists() or path.is_symlink():
        raise ValueError(f"output already exists: {path}")
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text(
            json.dumps(value, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)
    except Exception:
        if temporary.exists() and not temporary.is_symlink():
            temporary.unlink()
        raise


def run_analysis(
    executable: str,
    compile_commands: pathlib.Path,
    root: pathlib.Path,
    entries: list[tuple[str, str]],
) -> None:
    if version_major(executable) != EXPECTED_CLANG_TIDY_MAJOR:
        raise ValueError(
            f"{executable} must be Clang {EXPECTED_CLANG_TIDY_MAJOR}"
        )
    config = root / ".clang-tidy"
    if config.is_symlink() or not config.is_file():
        raise ValueError(".clang-tidy must be a regular checked-in policy")
    for _target, relative in entries:
        command = [
            executable,
            "-p",
            str(compile_commands.parent),
            "--config-file",
            str(config),
            "--warnings-as-errors=*",
            str(root / relative),
        ]
        completed = subprocess.run(command, cwd=root, check=False)
        if completed.returncode != 0:
            raise ValueError(
                f"static analysis failed for {relative} with exit "
                f"{completed.returncode}"
            )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compile-commands", type=pathlib.Path, required=True)
    parser.add_argument("--source-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--clang-tidy", required=True)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument(
        "--repo-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    args = parser.parse_args(argv)

    try:
        root = args.repo_root.resolve(strict=True)
        expected = load_manifest(args.source_manifest)
        actual = compile_entries(root, args.compile_commands)
        if actual != expected:
            missing = sorted(set(expected) - set(actual))
            extra = sorted(set(actual) - set(expected))
            raise ValueError(
                "compile database/source manifest mismatch: "
                f"missing={missing}, extra={extra}"
            )
        run_analysis(args.clang_tidy, args.compile_commands, root, actual)
        result = {
            "schema_version": 1,
            "clang_tidy_major": EXPECTED_CLANG_TIDY_MAJOR,
            "warnings_as_errors": True,
            "translation_units": [
                {"target": target, "source": source}
                for target, source in actual
            ],
            "translation_unit_count": len(actual),
            "compile_database_sha256": sha256(args.compile_commands),
            "source_manifest_sha256": sha256(args.source_manifest),
            "policy_sha256": sha256(root / ".clang-tidy"),
            "diagnostic_count": 0,
        }
        if args.output:
            write_new(args.output.resolve(strict=False), result)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"Portable static analysis failed: {exc}", file=sys.stderr)
        return 1
    print(f"Portable static analysis OK: {len(actual)} translation units")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

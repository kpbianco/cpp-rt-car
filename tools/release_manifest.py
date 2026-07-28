#!/usr/bin/env python3
"""Create and verify content-addressed RTFW release artifact manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys
from typing import Any


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def version_from(path: pathlib.Path) -> str:
    version = path.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", version):
        raise ValueError(f"{path}: expected canonical MAJOR.MINOR.PATCH")
    return version


def validated_commit(value: str) -> str:
    normalized = value.lower()
    if not re.fullmatch(r"[0-9a-f]{40}", normalized):
        raise ValueError("source commit must be a complete 40-character Git SHA")
    return normalized


def relative_artifact_paths(
    artifact_dir: pathlib.Path,
    manifest_path: pathlib.Path,
) -> list[pathlib.Path]:
    artifact_dir = artifact_dir.resolve()
    manifest_path = manifest_path.resolve()
    paths: list[pathlib.Path] = []
    for path in artifact_dir.rglob("*"):
        if path.resolve() == manifest_path:
            continue
        if path.is_symlink():
            raise ValueError(f"release artifact must not be a symlink: {path}")
        if path.is_file():
            paths.append(path.relative_to(artifact_dir))
    paths.sort(key=lambda value: value.as_posix())
    if not paths:
        raise ValueError("artifact directory contains no release artifacts")
    return paths


def create_manifest(
    artifact_dir: pathlib.Path,
    manifest_path: pathlib.Path,
    version_file: pathlib.Path,
    source_commit: str,
) -> dict[str, Any]:
    artifact_dir = artifact_dir.resolve()
    manifest_path = manifest_path.resolve()
    version = version_from(version_file)
    commit = validated_commit(source_commit)
    artifacts = []
    for relative in relative_artifact_paths(artifact_dir, manifest_path):
        path = artifact_dir / relative
        artifacts.append(
            {
                "path": relative.as_posix(),
                "size_bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
        )
    return {
        "schema_version": 1,
        "project": "rtfw",
        "version": version,
        "source_commit": commit,
        "artifacts": artifacts,
    }


def safe_relative_path(raw: Any) -> pathlib.PurePosixPath | None:
    if (
        not isinstance(raw, str)
        or not raw
        or "\\" in raw
        or ":" in raw
        or any(ord(character) < 0x20 for character in raw)
    ):
        return None
    path = pathlib.PurePosixPath(raw)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        return None
    if path.as_posix() != raw:
        return None
    return path


def verify_manifest(
    manifest_path: pathlib.Path,
    artifact_dir: pathlib.Path,
    version_file: pathlib.Path,
) -> list[str]:
    errors: list[str] = []
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        return [f"manifest cannot be read: {exc}"]
    if not isinstance(manifest, dict):
        return ["manifest top-level value must be an object"]

    try:
        expected_version = version_from(version_file)
    except (OSError, UnicodeError, ValueError) as exc:
        errors.append(str(exc))
        expected_version = ""

    if manifest.get("schema_version") != 1:
        errors.append("schema_version must be 1")
    if manifest.get("project") != "rtfw":
        errors.append("project must be rtfw")
    if manifest.get("version") != expected_version:
        errors.append("manifest version does not match VERSION.txt")
    try:
        validated_commit(str(manifest.get("source_commit", "")))
    except ValueError as exc:
        errors.append(str(exc))

    entries = manifest.get("artifacts")
    if not isinstance(entries, list) or not entries:
        errors.append("artifacts must be a non-empty array")
        return errors

    artifact_dir = artifact_dir.resolve()
    manifest_resolved = manifest_path.resolve()
    seen: set[str] = set()
    listed: set[str] = set()
    previous = ""
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict):
            errors.append(f"artifact {index} must be an object")
            continue
        relative = safe_relative_path(entry.get("path"))
        if relative is None:
            errors.append(f"artifact {index} has an unsafe path")
            continue
        raw = relative.as_posix()
        if raw in seen:
            errors.append(f"duplicate artifact path: {raw}")
            continue
        if raw < previous:
            errors.append("artifact entries must be sorted by path")
        previous = raw
        seen.add(raw)
        listed.add(raw)

        path = artifact_dir.joinpath(*relative.parts)
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(artifact_dir)
        except (OSError, ValueError):
            errors.append(f"artifact is missing or escapes its directory: {raw}")
            continue
        if resolved == manifest_resolved:
            errors.append("manifest must not list itself as an artifact")
            continue
        if path.is_symlink() or not path.is_file():
            errors.append(f"artifact is not a regular file: {raw}")
            continue
        size = entry.get("size_bytes")
        if not isinstance(size, int) or isinstance(size, bool) or size < 0:
            errors.append(f"artifact has invalid size: {raw}")
        elif path.stat().st_size != size:
            errors.append(f"artifact size mismatch: {raw}")
        digest = entry.get("sha256")
        if not isinstance(digest, str) or not re.fullmatch(
            r"[0-9a-f]{64}",
            digest,
        ):
            errors.append(f"artifact has invalid SHA-256: {raw}")
        elif sha256(path) != digest:
            errors.append(f"artifact digest mismatch: {raw}")

    try:
        present = {
            relative.as_posix()
            for relative in relative_artifact_paths(
                artifact_dir,
                manifest_path,
            )
        }
    except ValueError as exc:
        errors.append(str(exc))
    else:
        missing = sorted(present - listed)
        extra = sorted(listed - present)
        if missing:
            errors.append(f"unlisted release artifacts: {missing}")
        if extra:
            errors.append(f"listed artifacts not present: {extra}")
    return errors


def write_manifest(path: pathlib.Path, manifest: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(
        json.dumps(manifest, indent=2, sort_keys=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create")
    create.add_argument("--artifact-dir", type=pathlib.Path, required=True)
    create.add_argument("--output", type=pathlib.Path, required=True)
    create.add_argument("--version-file", type=pathlib.Path, required=True)
    create.add_argument("--source-commit", required=True)

    verify = subparsers.add_parser("verify")
    verify.add_argument("--artifact-dir", type=pathlib.Path, required=True)
    verify.add_argument("--manifest", type=pathlib.Path, required=True)
    verify.add_argument("--version-file", type=pathlib.Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "create":
            manifest = create_manifest(
                args.artifact_dir,
                args.output,
                args.version_file,
                args.source_commit,
            )
            write_manifest(args.output, manifest)
            print(
                f"Release manifest created: {len(manifest['artifacts'])} artifacts"
            )
            return 0

        errors = verify_manifest(
            args.manifest,
            args.artifact_dir,
            args.version_file,
        )
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"Release manifest failed: {exc}", file=sys.stderr)
        return 1

    if errors:
        print("Release manifest failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1
    print("Release manifest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

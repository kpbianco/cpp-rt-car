#!/usr/bin/env python3
"""Create and verify content-addressed RTFW release artifact manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import sys
from typing import Any


MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_ARTIFACTS = 256
MAX_ARTIFACT_BYTES = 8 * 1024 * 1024 * 1024


class DuplicateKeyError(ValueError):
    pass


def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise DuplicateKeyError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def strict_json(path: pathlib.Path) -> Any:
    if path.is_symlink() or not path.is_file():
        raise ValueError("manifest must be a regular non-symlink file")
    if path.stat().st_size > MAX_MANIFEST_BYTES:
        raise ValueError("manifest exceeds the 4 MiB limit")
    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicates,
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"manifest cannot be read: {exc}") from exc


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
            if len(paths) > MAX_ARTIFACTS:
                raise ValueError(
                    f"artifact directory exceeds {MAX_ARTIFACTS} files"
                )
    paths.sort(key=lambda value: value.as_posix())
    if not paths:
        raise ValueError("artifact directory contains no release artifacts")
    total = sum((artifact_dir / path).stat().st_size for path in paths)
    if total > MAX_ARTIFACT_BYTES:
        raise ValueError("artifact directory exceeds the 8 GiB byte limit")
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
    expected_source_commit: str | None = None,
) -> list[str]:
    errors: list[str] = []
    try:
        manifest = strict_json(manifest_path)
    except ValueError as exc:
        return [str(exc)]
    if not isinstance(manifest, dict):
        return ["manifest top-level value must be an object"]
    expected_top_level = [
        "schema_version",
        "project",
        "version",
        "source_commit",
        "artifacts",
    ]
    if list(manifest) != expected_top_level:
        errors.append("manifest top-level inventory differs from schema version 1")
    expected_encoding = (
        json.dumps(manifest, indent=2, sort_keys=False) + "\n"
    ).encode("utf-8")
    if manifest_path.read_bytes() != expected_encoding:
        errors.append("manifest JSON is not in canonical schema order/encoding")

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
        manifest_commit = validated_commit(str(manifest.get("source_commit", "")))
    except ValueError as exc:
        errors.append(str(exc))
        manifest_commit = ""
    try:
        expected_commit = validated_commit(expected_source_commit or "")
    except ValueError:
        errors.append("an explicit expected source commit is required")
        expected_commit = ""
    if manifest_commit and expected_commit and manifest_commit != expected_commit:
        errors.append("manifest source commit does not match expected source commit")

    entries = manifest.get("artifacts")
    if not isinstance(entries, list) or not entries:
        errors.append("artifacts must be a non-empty array")
        return errors
    if len(entries) > MAX_ARTIFACTS:
        errors.append(f"artifacts exceed the {MAX_ARTIFACTS}-entry limit")
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
        if list(entry) != ["path", "size_bytes", "sha256"]:
            errors.append(f"artifact {index} has unknown or missing fields")
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
    if path.exists() or path.is_symlink():
        raise ValueError(f"manifest output already exists: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    try:
        temporary.write_text(
            json.dumps(manifest, indent=2, sort_keys=False) + "\n",
            encoding="utf-8",
        )
        temporary.replace(path)
    except Exception:
        if temporary.exists() and not temporary.is_symlink():
            temporary.unlink()
        raise


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
    verify.add_argument("--expected-source-commit")
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
            args.expected_source_commit or os.environ.get("GITHUB_SHA"),
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

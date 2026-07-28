#!/usr/bin/env python3
"""Stage exactly one CPack archive and its verified SHA-256 sidecar."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import shutil
import sys


SUFFIXES = {
    "TGZ": ".tar.gz",
    "ZIP": ".zip",
}


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def canonical_version(path: pathlib.Path) -> str:
    value = path.read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)", value):
        raise ValueError(f"{path}: expected canonical MAJOR.MINOR.PATCH")
    return value


def stage(
    cpack_dir: pathlib.Path,
    artifact_dir: pathlib.Path,
    generator: str,
    version_file: pathlib.Path,
) -> list[pathlib.Path]:
    suffix = SUFFIXES[generator]
    version = canonical_version(version_file)
    if cpack_dir.is_symlink():
        raise ValueError("CPack output must be a regular directory")
    if artifact_dir.is_symlink():
        raise ValueError("artifact directory must not be a symlink")
    cpack_dir = cpack_dir.resolve()
    artifact_dir = artifact_dir.resolve()
    if not cpack_dir.is_dir():
        raise ValueError("CPack output must be a regular directory")
    if artifact_dir.exists():
        raise ValueError("artifact directory must not already exist")

    top_level_files = [
        path
        for path in cpack_dir.iterdir()
        if path.is_file() or path.is_symlink()
    ]
    if any(path.is_symlink() for path in top_level_files):
        raise ValueError("CPack output must not contain top-level symlinks")
    unexpected_directories = sorted(
        path.name
        for path in cpack_dir.iterdir()
        if path.is_dir()
        and not path.is_symlink()
        and path.name != "_CPack_Packages"
    )
    if unexpected_directories:
        raise ValueError(
            "unexpected top-level CPack directories: "
            f"{unexpected_directories}"
        )

    archives = [
        path
        for path in top_level_files
        if path.name.startswith(f"rtfw-{version}-")
        and path.name.endswith(suffix)
    ]
    if len(archives) != 1:
        raise ValueError(
            f"expected exactly one {generator} archive, found {len(archives)}"
        )
    archive = archives[0]
    sidecar = archive.with_name(archive.name + ".sha256")
    expected_files = {archive, sidecar}
    unexpected = sorted(
        path.name
        for path in top_level_files
        if path not in expected_files
    )
    if unexpected:
        raise ValueError(f"unexpected top-level CPack files: {unexpected}")
    if not sidecar.is_file() or sidecar.is_symlink():
        raise ValueError("CPack SHA-256 sidecar is missing or not regular")

    words = sidecar.read_text(encoding="utf-8").strip().split()
    if not words or not re.fullmatch(r"[0-9A-Fa-f]{64}", words[0]):
        raise ValueError("CPack SHA-256 sidecar is malformed")
    if words[0].lower() != sha256(archive):
        raise ValueError("CPack SHA-256 sidecar does not match the archive")
    if len(words) > 1 and pathlib.PurePath(words[-1]).name != archive.name:
        raise ValueError("CPack SHA-256 sidecar names a different archive")

    artifact_dir.mkdir(parents=True)
    staged = []
    for source in (archive, sidecar):
        target = artifact_dir / source.name
        shutil.copy2(source, target)
        staged.append(target)
    return staged


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpack-dir", type=pathlib.Path, required=True)
    parser.add_argument("--artifact-dir", type=pathlib.Path, required=True)
    parser.add_argument("--generator", choices=sorted(SUFFIXES), required=True)
    parser.add_argument("--version-file", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)

    try:
        staged = stage(
            args.cpack_dir,
            args.artifact_dir,
            args.generator,
            args.version_file,
        )
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"Release artifact staging failed: {exc}", file=sys.stderr)
        return 1
    print(f"Release artifact staging OK: {len(staged)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

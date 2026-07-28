#!/usr/bin/env python3
"""Safely extract the single staged RTFW release archive for testing."""

from __future__ import annotations

import argparse
import pathlib
import shutil
import stat
import sys
import tarfile
import zipfile
from typing import BinaryIO, Iterable


MAX_MEMBERS = 100_000
MAX_UNCOMPRESSED_BYTES = 8 * 1024 * 1024 * 1024


def safe_member_path(raw: str) -> pathlib.PurePosixPath | None:
    normalized = raw.rstrip("/")
    if (
        not normalized
        or "\\" in normalized
        or ":" in normalized
        or any(ord(character) < 0x20 for character in normalized)
    ):
        return None
    path = pathlib.PurePosixPath(normalized)
    if path.is_absolute() or ".." in path.parts or "." in path.parts:
        return None
    if path.as_posix() != normalized:
        return None
    return path


def safe_link_target(
    member: pathlib.PurePosixPath,
    raw_target: str,
) -> pathlib.PurePosixPath | None:
    if (
        not raw_target
        or "\\" in raw_target
        or ":" in raw_target
        or any(ord(character) < 0x20 for character in raw_target)
    ):
        return None
    target = pathlib.PurePosixPath(raw_target)
    if target.is_absolute():
        return None
    parts = list(member.parent.parts)
    for part in target.parts:
        if part == ".":
            continue
        if part == "..":
            if not parts:
                return None
            parts.pop()
        else:
            parts.append(part)
    return pathlib.PurePosixPath(*parts) if parts else None


def checked_members(
    values: Iterable[tuple[str, int]],
) -> dict[str, pathlib.PurePosixPath]:
    paths: dict[str, pathlib.PurePosixPath] = {}
    total_size = 0
    for index, (raw_name, size) in enumerate(values):
        if index >= MAX_MEMBERS:
            raise ValueError(f"archive exceeds {MAX_MEMBERS} members")
        path = safe_member_path(raw_name)
        if path is None:
            raise ValueError(f"archive contains unsafe path: {raw_name!r}")
        key = path.as_posix()
        if key in paths:
            raise ValueError(f"archive contains duplicate path: {key}")
        if size < 0:
            raise ValueError(f"archive member has negative size: {key}")
        total_size += size
        if total_size > MAX_UNCOMPRESSED_BYTES:
            raise ValueError(
                "archive exceeds the uncompressed-size safety limit"
            )
        paths[key] = path
    return paths


def copy_stream(source: BinaryIO, target: pathlib.Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("xb") as output:
        shutil.copyfileobj(source, output, length=1024 * 1024)


def extract_tar(archive: pathlib.Path, destination: pathlib.Path) -> None:
    with tarfile.open(archive, mode="r:*") as source:
        members = source.getmembers()
        paths = checked_members(
            (member.name, member.size if member.isfile() else 0)
            for member in members
        )
        links: dict[str, str] = {}
        for member in members:
            if not (member.isdir() or member.isfile() or member.issym()):
                raise ValueError(
                    f"archive contains unsupported member: {member.name}"
                )
            if member.issym():
                member_path = paths[safe_member_path(member.name).as_posix()]
                link_target = safe_link_target(member_path, member.linkname)
                if (
                    link_target is None
                    or link_target.as_posix() not in paths
                ):
                    raise ValueError(
                        f"archive contains unsafe link: {member.name}"
                    )
                links[member_path.as_posix()] = link_target.as_posix()

        for link in links:
            visited: set[str] = set()
            current = link
            while current in links:
                if current in visited:
                    raise ValueError(
                        f"archive contains cyclic link: {link}"
                    )
                visited.add(current)
                current = links[current]

        for member in members:
            relative = paths[safe_member_path(member.name).as_posix()]
            target = destination.joinpath(*relative.parts)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                stream = source.extractfile(member)
                if stream is None:
                    raise ValueError(
                        f"archive member cannot be read: {member.name}"
                    )
                with stream:
                    copy_stream(stream, target)
                target.chmod(member.mode & 0o777)

        for member in members:
            if not member.issym():
                continue
            relative = paths[safe_member_path(member.name).as_posix()]
            target = destination.joinpath(*relative.parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(member.linkname)


def extract_zip(archive: pathlib.Path, destination: pathlib.Path) -> None:
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
        paths = checked_members(
            (member.filename, member.file_size if not member.is_dir() else 0)
            for member in members
        )
        for member in members:
            unix_mode = member.external_attr >> 16
            if stat.S_IFMT(unix_mode) == stat.S_IFLNK:
                raise ValueError(
                    f"ZIP archive contains unsupported link: {member.filename}"
                )
            relative = paths[safe_member_path(member.filename).as_posix()]
            target = destination.joinpath(*relative.parts)
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            with source.open(member, mode="r") as stream:
                copy_stream(stream, target)
            permissions = unix_mode & 0o777
            if permissions:
                target.chmod(permissions)


def find_archive(artifact_dir: pathlib.Path) -> pathlib.Path:
    if artifact_dir.is_symlink():
        raise ValueError("artifact directory must be a regular directory")
    artifact_dir = artifact_dir.resolve()
    if not artifact_dir.is_dir():
        raise ValueError("artifact directory must be a regular directory")
    archives = [
        path
        for path in artifact_dir.iterdir()
        if path.is_file()
        and not path.is_symlink()
        and (
            path.name.endswith(".tar.gz")
            or path.name.endswith(".zip")
        )
    ]
    if len(archives) != 1:
        raise ValueError(
            f"expected exactly one staged archive, found {len(archives)}"
        )
    return archives[0]


def extract(
    artifact_dir: pathlib.Path,
    destination: pathlib.Path,
) -> pathlib.Path:
    if destination.exists() or destination.is_symlink():
        raise ValueError("extraction destination must not already exist")
    archive = find_archive(artifact_dir)
    destination.mkdir(parents=True)
    try:
        if archive.name.endswith(".zip"):
            extract_zip(archive, destination)
        else:
            extract_tar(archive, destination)
    except Exception:
        shutil.rmtree(destination)
        raise
    return archive


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact-dir", type=pathlib.Path, required=True)
    parser.add_argument("--destination", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)

    try:
        archive = extract(args.artifact_dir, args.destination)
    except (
        OSError,
        ValueError,
        tarfile.TarError,
        zipfile.BadZipFile,
    ) as exc:
        print(f"Release archive extraction failed: {exc}", file=sys.stderr)
        return 1
    print(f"Release archive extraction OK: {archive.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

"""Helpers for detecting and normalising host CPU/OS information."""

from __future__ import annotations

import os
import pathlib
import platform
import re
from functools import lru_cache
from typing import Dict


def _read_cpuinfo() -> list[str]:
    cpuinfo_path = pathlib.Path("/proc/cpuinfo")
    if not cpuinfo_path.exists():
        return []
    try:
        return cpuinfo_path.read_text(encoding="utf-8", errors="ignore").splitlines()
    except OSError:
        return []


def detect_cpu_model() -> str:
    """Attempt to detect a descriptive CPU model string."""

    candidates: list[str] = []

    if (platform.system() or "").lower() == "linux":
        for line in _read_cpuinfo():
            key, sep, value = line.partition(":")
            if not sep:
                continue
            lower_key = key.strip().lower()
            candidate = value.strip()
            if not candidate:
                continue
            if lower_key in {"model name", "hardware", "cpu part", "cpu model"}:
                return candidate
            if lower_key == "processor" and not candidate.isdigit():
                candidates.append(candidate)

    for getter in (
        platform.processor,
        platform.machine,
        lambda: platform.uname().processor,
        lambda: os.uname().machine if hasattr(os, "uname") else "",
    ):
        try:
            value = getter()
        except OSError:
            continue
        if value:
            candidates.append(str(value))

    for candidate in candidates:
        cleaned = str(candidate).strip()
        if cleaned:
            return cleaned
    return "unknown-cpu"


def _normalise_os_token(value: str) -> str:
    token = (value or "").strip().lower()
    if token.startswith("linux") or token == "posix":
        return "linux"
    if token.startswith("win"):
        return "windows"
    if token.startswith("mac") or token.startswith("darwin") or token.startswith("osx"):
        return "darwin"
    if token == "nt":
        return "windows"
    if token in {"msys", "cygwin"}:
        return "windows"
    return token or "unknown-os"


def detect_os_name() -> str:
    """Return a normalised operating-system token."""

    try:
        primary = platform.system()
    except OSError:
        primary = ""
    token = _normalise_os_token(primary)
    if token != "unknown-os":
        return token

    if hasattr(os, "name"):
        token = _normalise_os_token(os.name)
        if token != "unknown-os":
            return token

    if hasattr(os, "uname"):
        try:
            uname_token = _normalise_os_token(os.uname().sysname)  # type: ignore[attr-defined]
            if uname_token != "unknown-os":
                return uname_token
        except OSError:
            pass

    return "unknown-os"


_slug_re = re.compile(r"[^a-z0-9_]+")
_repeat_re = re.compile(r"_+")


def normalise_os_name(name: str) -> str:
    """Normalise an arbitrary OS string to a canonical token."""

    return _normalise_os_token(name)


def slugify_cpu(cpu_model: str) -> str:
    """Convert a CPU model string to a filesystem-safe slug."""

    text = (cpu_model or "unknown-cpu").lower()
    text = text.replace("-", "_").replace(" ", "_")
    text = _slug_re.sub("_", text)
    text = _repeat_re.sub("_", text).strip("_")
    return text or "unknown_cpu"


@lru_cache(maxsize=1)
def host_tokens() -> Dict[str, str]:
    """Return the detected host CPU/OS identifiers used across tools."""

    cpu_model = detect_cpu_model()
    os_name = detect_os_name()
    cpu_slug = slugify_cpu(cpu_model)
    return {
        "cpu_model": cpu_model,
        "cpu_slug": cpu_slug,
        "os_name": os_name,
    }


__all__ = [
    "detect_cpu_model",
    "detect_os_name",
    "slugify_cpu",
    "normalise_os_name",
    "host_tokens",
]

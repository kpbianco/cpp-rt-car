
#!/usr/bin/env python3
"""Install the best autotune profile for the current host."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import re
import sys
from typing import Any, Iterable, Mapping, Sequence

if __package__ is None or __package__ == "":
    sys.path.append(str(pathlib.Path(__file__).resolve().parents[2]))
    from tools.autotune.make_config import build_config
else:
    from .make_config import build_config



def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Install the best autotune profile for the current host."
    )
    parser.add_argument(
        "--results",
        type=pathlib.Path,
        default=pathlib.Path("results"),
        help="Directory containing best.json / summary.json output from autotune.",
    )
    parser.add_argument(
        "--reports",
        type=pathlib.Path,
        default=pathlib.Path("reports"),
        help="Directory containing aggregated analytics (best.json).",
    )
    parser.add_argument(
        "--profiles-dir",
        type=pathlib.Path,
        default=pathlib.Path("profiles"),
        help="Destination directory for installed profiles.",
    )
    parser.add_argument(
        "--cpu",
        dest="cpu_override",
        help="Override detected CPU identifier.",
    )
    parser.add_argument(
        "--os",
        dest="os_override",
        help="Override detected operating system identifier.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show the resolved destination without writing files.",
    )
    return parser.parse_args()


def detect_cpu_model() -> str:
    candidates: list[str] = []

    cpuinfo = pathlib.Path("/proc/cpuinfo")
    if cpuinfo.exists():
        try:
            for line in cpuinfo.read_text(encoding="utf-8", errors="ignore").splitlines():
                key, sep, value = line.partition(":")
                if not sep:
                    continue
                lower_key = key.strip().lower()
                candidate = value.strip()
                if lower_key in {"model name", "hardware", "cpu part"} and candidate:
                    return candidate
                if lower_key == "processor" and candidate and not candidate.isdigit():
                    candidates.append(candidate)
        except OSError:
            pass

    fallbacks = (
        platform.processor,
        platform.machine,
        lambda: platform.uname().processor,
    )
    for getter in fallbacks:
        try:
            value = getter()
        except OSError:
            continue
        if value:
            candidates.append(str(value))

    if candidates:
        return candidates[0]
    return "unknown-cpu"


def detect_os_name() -> str:
    try:
        name = platform.system()
    except OSError:
        name = ""
    if name:
        return name
    try:
        return os.uname().sysname
    except OSError:
        return "unknown-os"


def normalise_token(value: str) -> str:
    cleaned = value.strip().lower()
    cleaned = re.sub(r"[^a-z0-9_]+", "-", cleaned)
    cleaned = re.sub(r"-+", "-", cleaned).strip("-")
    return cleaned or "unknown"


def extract_params(payload: Any) -> Mapping[str, Any] | None:
    if isinstance(payload, Mapping):
        for key in ("params", "profile", "best_profile", "best_params"):
            data = payload.get(key)
            if isinstance(data, Mapping):
                return data

        if {
            "threads",
            "chunk_target_us",
            "aosoa_block",
            "steal_threshold",
        }.issubset(payload.keys()):
            return payload

        for key in ("best", "best_run", "best_experiment"):
            nested = payload.get(key)
            params = extract_params(nested)
            if params:
                return params

    if isinstance(payload, Sequence) and not isinstance(payload, (str, bytes, bytearray)):
        for item in payload:
            params = extract_params(item)
            if params:
                return params

    return None


def load_best_params(paths: Iterable[pathlib.Path]) -> Mapping[str, Any]:
    last_error: Exception | None = None
    for path in paths:
        if not path.exists():
            continue
        try:
            payload = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            last_error = exc
            continue
        params = extract_params(payload)
        if params is not None:
            return params
    if last_error is not None:
        raise SystemExit(f"Failed to load profile data: {last_error}")
    raise SystemExit(
        "Unable to locate autotune parameters in any of the provided result files."
    )


def main() -> None:
    args = parse_args()

    search_paths: list[pathlib.Path] = []
    for directory in {args.results, args.reports}:
        if directory is None:
            continue
        search_paths.extend(
            [
                directory / "best_profile.json",
                directory / "best.json",
                directory / "summary.json",
            ]
        )

    params = load_best_params(search_paths)
    config = build_config(params)

    cpu_name = args.cpu_override or detect_cpu_model()
    os_name = args.os_override or detect_os_name()

    cpu_token = normalise_token(cpu_name)
    os_token = normalise_token(os_name)

    destination = args.profiles_dir / f"{cpu_token}-{os_token}.json"

    if args.dry_run:
        print(destination)
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("w", encoding="utf-8") as fh:
        json.dump(config, fh, indent=2, sort_keys=True)
        fh.write("\n")

    print(f"Installed profile to {destination}")


if __name__ == "__main__":
    main()

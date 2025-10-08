#!/usr/bin/env python3
"""Validate that every autotune spec parameter is mapped into the runtime config.

When introducing new knobs in spec.yaml, extend tools.autotune.make_config.MAPPED_PARAMS
so this check continues to pass (or add well-justified entries to IGNORED_PARAMS).
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Set

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune.make_config import MAPPED_PARAMS, load_spec  # noqa: E402


DEFAULT_SPEC_PATH = pathlib.Path(__file__).with_name("spec.yaml")

# Parameters that are intentionally left unmapped. Keep this list small and
# document the reason for each ignore entry if used.
IGNORED_PARAMS: Set[str] = set()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Ensure autotune spec parameters have config mappings"
    )
    parser.add_argument(
        "--spec",
        type=pathlib.Path,
        default=DEFAULT_SPEC_PATH,
        help=f"Path to spec.yaml (default: {DEFAULT_SPEC_PATH})",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    specs = load_spec(args.spec)
    spec_params = set(specs.keys())
    mapped_params = set(MAPPED_PARAMS)

    extra_ignored = IGNORED_PARAMS - spec_params
    if extra_ignored:
        print(
            "IGNORED_PARAMS references names not present in the spec: "
            f"{', '.join(sorted(extra_ignored))}",
            file=sys.stderr,
        )
        sys.exit(1)

    uncovered = spec_params - mapped_params - IGNORED_PARAMS
    if uncovered:
        print("Unmapped autotune parameters detected:", file=sys.stderr)
        for name in sorted(uncovered):
            print(f"  - {name}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()


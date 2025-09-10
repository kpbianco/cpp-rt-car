"""Utility for differential testing of kernel outputs.

Reads two files containing newline-separated numeric values and
computes the maximum absolute difference.  Fails if the drift exceeds
an allowed threshold.

This is intended for use in CI to detect behavioural drift between an
"old" and a "new" kernel implementation.
"""

from __future__ import annotations

import math
from typing import Iterable, List


def _load_values(path: str) -> List[float]:
    """Load newline separated floats from *path*."""
    with open(path, "r", encoding="utf-8") as f:
        return [float(line.strip()) for line in f if line.strip()]


def max_drift(old: Iterable[float], new: Iterable[float]) -> float:
    """Return the maximum absolute difference between two iterables."""
    return max((abs(a - b) for a, b in zip(old, new)), default=0.0)


def compare_outputs(old_path: str, new_path: str, threshold: float) -> bool:
    """Compare two output files and check drift against *threshold*.

    Returns ``True`` if the maximum drift is within ``threshold``,
    otherwise returns ``False``.
    """
    old_vals = _load_values(old_path)
    new_vals = _load_values(new_path)
    drift = max_drift(old_vals, new_vals)
    return drift <= threshold


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Differential output test")
    parser.add_argument("old", help="path to baseline output")
    parser.add_argument("new", help="path to new output")
    parser.add_argument("--threshold", type=float, default=0.0, help="allowed drift")
    args = parser.parse_args()

    if compare_outputs(args.old, args.new, args.threshold):
        print("PASS: drift within threshold")
    else:
        print("FAIL: drift exceeds threshold")
        raise SystemExit(1)

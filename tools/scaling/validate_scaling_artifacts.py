#!/usr/bin/env python3
"""Validate scaling artifacts for schema and basic sanity checks."""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import sys
from typing import Dict, List, Mapping, Sequence, Tuple


REQUIRED_COLUMNS = ("threads", "smt", "p50_ms", "p95_ms", "p99_ms", "variance")
NUMERIC_COLUMNS = ("threads", "p50_ms", "p95_ms", "p99_ms", "variance")
SUMMARY_FIELDS = ("p50_ms", "p95_ms", "p99_ms", "variance")


class ValidationError(Exception):
    """Raised when validation fails."""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate generated scaling artifacts")
    parser.add_argument(
        "--in",
        dest="input_dir",
        required=True,
        help="Directory containing scaling.csv and scaling.json",
    )
    return parser.parse_args()


def require_file(path: pathlib.Path) -> pathlib.Path:
    if not path.exists():
        raise ValidationError(f"Expected artifact missing: {path}")
    if not path.is_file():
        raise ValidationError(f"Artifact is not a file: {path}")
    return path


def load_csv(path: pathlib.Path) -> Tuple[List[Dict[str, str]], Sequence[str]]:
    require_file(path)
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise ValidationError("CSV artifact is missing a header row")
        missing = [field for field in REQUIRED_COLUMNS if field not in reader.fieldnames]
        if missing:
            raise ValidationError(
                f"CSV artifact missing required columns: {', '.join(sorted(missing))}"
            )
        rows = list(reader)
    if len(rows) < 2:
        raise ValidationError("CSV artifact must contain at least two rows")
    return rows, reader.fieldnames


def parse_positive_float(value: object, field: str, context: str) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as exc:
        raise ValidationError(f"{context}: field '{field}' is not numeric: {value!r}") from exc
    if not math.isfinite(parsed):
        raise ValidationError(f"{context}: field '{field}' is not finite: {value!r}")
    if parsed <= 0:
        raise ValidationError(f"{context}: field '{field}' must be > 0 (got {parsed})")
    return parsed


def validate_csv_rows(rows: Sequence[Mapping[str, str]], fieldnames: Sequence[str]) -> Dict[Tuple[str, int], Dict[str, float]]:
    seen: Dict[Tuple[str, int], Dict[str, float]] = {}
    for idx, row in enumerate(rows, start=1):
        smt_value = (row.get("smt") or "").strip().lower()
        if smt_value not in {"on", "off"}:
            raise ValidationError(
                f"Row {idx}: column 'smt' must be either 'on' or 'off' (got {row.get('smt')!r})"
            )
        threads_raw = row.get("threads")
        try:
            threads = int(str(threads_raw))
        except (TypeError, ValueError) as exc:
            raise ValidationError(
                f"Row {idx}: column 'threads' is not an integer: {threads_raw!r}"
            ) from exc
        if threads <= 0:
            raise ValidationError(f"Row {idx}: column 'threads' must be > 0 (got {threads})")

        metrics: Dict[str, float] = {"threads": float(threads)}
        for field in (f for f in NUMERIC_COLUMNS if f != "threads"):
            metrics[field] = parse_positive_float(row.get(field, ""), field, f"Row {idx}")

        if "objective" in fieldnames:
            parse_positive_float(row.get("objective", ""), "objective", f"Row {idx}")

        key = (smt_value, threads)
        if key in seen:
            raise ValidationError(
                f"Duplicate entry for SMT={smt_value} threads={threads} detected in CSV"
            )
        metrics["smt"] = smt_value
        seen[key] = metrics
    return seen


def validate_json(path: pathlib.Path, csv_index: Mapping[Tuple[str, int], Mapping[str, float]]) -> None:
    require_file(path)
    with path.open("r", encoding="utf-8") as handle:
        payload = json.load(handle)

    runs = payload.get("runs")
    if not isinstance(runs, list) or not runs:
        raise ValidationError("JSON artifact must contain a non-empty 'runs' array")
    if len(runs) < 2:
        raise ValidationError("JSON artifact must contain at least two runs")

    for idx, run in enumerate(runs, start=1):
        if not isinstance(run, Mapping):
            raise ValidationError(f"Run {idx}: entry is not an object")
        threads = run.get("threads")
        smt_value = (str(run.get("smt") or "")).strip().lower()
        if smt_value not in {"on", "off"}:
            raise ValidationError(f"Run {idx}: 'smt' must be 'on' or 'off'")
        if not isinstance(threads, int) or threads <= 0:
            raise ValidationError(f"Run {idx}: 'threads' must be a positive integer")

        summary = run.get("summary")
        if not isinstance(summary, Mapping):
            raise ValidationError(f"Run {idx}: missing 'summary' object")
        for field in SUMMARY_FIELDS:
            if field not in summary:
                raise ValidationError(f"Run {idx}: summary missing '{field}' field")
            parse_positive_float(summary[field], field, f"Run {idx}")

        key = (smt_value, threads)
        if key not in csv_index:
            raise ValidationError(
                f"Run {idx}: no matching CSV entry for SMT={smt_value} threads={threads}"
            )

        if "objective" in summary:
            parse_positive_float(summary["objective"], "objective", f"Run {idx}")


def main() -> None:
    args = parse_args()
    input_dir = pathlib.Path(args.input_dir)
    csv_rows, fieldnames = load_csv(input_dir / "scaling.csv")
    csv_index = validate_csv_rows(csv_rows, fieldnames)
    validate_json(input_dir / "scaling.json", csv_index)


if __name__ == "__main__":
    try:
        main()
    except ValidationError as exc:
        sys.stderr.write(f"Validation failed: {exc}\n")
        raise SystemExit(1) from exc

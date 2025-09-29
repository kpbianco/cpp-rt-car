#!/usr/bin/env python3
"""Generate a human readable autotuning summary report."""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
from dataclasses import dataclass
from typing import Iterable, List, Mapping, MutableMapping, Optional, Sequence, Tuple


BEST_METRIC_KEYS: Tuple[str, ...] = (
    "p99_frame_ms",
    "p95_frame_ms",
    "p50_frame_ms",
    "stdev_frame_ms",
    "queue_max",
    "emergency_spawns",
    "log_drops",
    "missed_frames",
    "watchdog_trips",
)

PARETO_METRIC_KEYS: Tuple[str, ...] = (
    "p99_frame_ms",
    "queue_max",
    "emergency_spawns",
    "log_drops",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render an autotune Markdown report")
    parser.add_argument(
        "--results-dir",
        type=pathlib.Path,
        default=pathlib.Path("results"),
        help="Directory containing best.json, pareto.json, and summary.csv",
    )
    parser.add_argument(
        "--spec",
        type=pathlib.Path,
        default=pathlib.Path("tools/autotune/spec.yaml"),
        help="Path to the autotuning specification YAML",
    )
    parser.add_argument(
        "--output",
        type=pathlib.Path,
        default=pathlib.Path("reports/autotune_summary.md"),
        help="Destination Markdown report",
    )
    return parser.parse_args()


def load_json(path: pathlib.Path) -> Mapping[str, object]:
    if not path.is_file():
        raise SystemExit(f"JSON file not found: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse JSON from {path}: {exc}") from exc
    if not isinstance(payload, Mapping):
        raise SystemExit(f"Expected object at top level in {path}")
    return payload


def load_json_sequence(path: pathlib.Path) -> Sequence[Mapping[str, object]]:
    if not path.is_file():
        raise SystemExit(f"JSON file not found: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse JSON from {path}: {exc}") from exc
    if not isinstance(payload, list):
        raise SystemExit(f"Expected list at top level in {path}")
    sequence: List[Mapping[str, object]] = []
    for item in payload:
        if not isinstance(item, Mapping):
            raise SystemExit(f"Expected objects inside list in {path}")
        sequence.append(item)
    return sequence


def load_summary_rows(path: pathlib.Path) -> List[Mapping[str, str]]:
    if not path.is_file():
        raise SystemExit(f"CSV file not found: {path}")
    rows: List[Mapping[str, str]] = []
    with path.open("r", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)
    if not rows:
        raise SystemExit(f"Summary CSV is empty: {path}")
    return rows


def load_spec(path: pathlib.Path) -> Mapping[str, object]:
    if not path.is_file():
        raise SystemExit(f"Spec file not found: {path}")
    try:
        import yaml  # type: ignore
    except ImportError:
        return simple_yaml_load(path.read_text(encoding="utf-8"))
    with path.open("r", encoding="utf-8") as fh:
        data = yaml.safe_load(fh)
    if not isinstance(data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping")
    return data


@dataclass
class _Context:
    indent: int
    container: object
    parent: Optional[MutableMapping[str, object]]
    key: Optional[str]


def simple_yaml_load(text: str) -> Mapping[str, object]:
    root: MutableMapping[str, object] = {}
    stack: List[_Context] = [_Context(-1, root, None, None)]

    lines = text.splitlines()
    for raw_line in lines:
        if not raw_line.strip() or raw_line.lstrip().startswith("#"):
            continue
        line_no_comment = raw_line.split("#", 1)[0].rstrip("\n")
        stripped = line_no_comment.strip()
        if not stripped:
            continue
        indent = len(line_no_comment) - len(line_no_comment.lstrip(" "))
        while len(stack) > 1 and indent <= stack[-1].indent:
            stack.pop()
        context = stack[-1]
        container = context.container

        if stripped.startswith("- "):
            if not isinstance(container, list):
                if isinstance(container, dict) and context.parent is not None and context.key is not None:
                    new_list: List[object] = []
                    context.parent[context.key] = new_list
                    stack[-1] = _Context(context.indent, new_list, context.parent, context.key)
                    container = new_list
                else:
                    raise SystemExit("Unexpected list item without list context in spec")
            value = stripped[2:].strip()
            container.append(_parse_scalar(value))
            continue

        if ":" in stripped:
            key, value = stripped.split(":", 1)
            key = key.strip()
            value = value.strip()

            if isinstance(container, list):
                new_dict: MutableMapping[str, object] = {}
                container.append(new_dict)
                container = new_dict
                stack.append(_Context(indent, container, None, None))

            if value:
                if not isinstance(container, MutableMapping):
                    raise SystemExit("Expected mapping container in spec")
                container[key] = _parse_scalar(value)
            else:
                if not isinstance(container, MutableMapping):
                    raise SystemExit("Expected mapping container in spec")
                new_container: MutableMapping[str, object] = {}
                container[key] = new_container
                stack.append(_Context(indent, new_container, container, key))
            continue

        raise SystemExit(f"Unable to parse line in spec: {raw_line}")

    return root


def _parse_scalar(value: str) -> object:
    if not value:
        return ""
    value = value.strip()
    if (value.startswith("\"") and value.endswith("\"")) or (
        value.startswith("'") and value.endswith("'")
    ):
        return value[1:-1]
    lowered = value.lower()
    if lowered in {"true", "yes"}:
        return True
    if lowered in {"false", "no"}:
        return False
    if lowered in {"null", "~"}:
        return None
    try:
        if value.startswith("0") and value != "0" and not value.startswith("0."):
            # preserve leading zeros as strings
            raise ValueError
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value


def collect_env(summary_rows: Iterable[Mapping[str, str]]) -> List[Tuple[str, List[str]]]:
    env_values: MutableMapping[str, List[str]] = {}
    for row in summary_rows:
        for key, raw_value in row.items():
            if not key:
                continue
            lowered = key.lower()
            if not (lowered.startswith("env") or lowered.startswith("environment")):
                continue
            value = (raw_value or "").strip()
            if not value:
                continue
            bucket = env_values.setdefault(key, [])
            if value not in bucket:
                bucket.append(value)
    return sorted(((key, values) for key, values in env_values.items()), key=lambda item: item[0])


def format_value(value: object) -> str:
    if value is None:
        return "—"
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, (int,)):
        return f"{value}"
    if isinstance(value, float):
        if math.isnan(value):
            return "nan"
        if abs(value) >= 100 or value == 0:
            return f"{value:.2f}"
        return f"{value:.4f}".rstrip("0").rstrip(".")
    if isinstance(value, (list, tuple)):
        return ", ".join(format_value(item) for item in value)
    if isinstance(value, Mapping):
        return json.dumps(value, sort_keys=True)
    return str(value)


def render_table(headers: Sequence[str], rows: Sequence[Sequence[str]]) -> List[str]:
    header_line = "| " + " | ".join(headers) + " |"
    separator = "| " + " | ".join("---" for _ in headers) + " |"
    body = ["| " + " | ".join(row) + " |" for row in rows]
    return [header_line, separator, *body]


def extract_params_order(spec: Mapping[str, object]) -> List[str]:
    params = spec.get("params", {})
    if isinstance(params, Mapping):
        return list(params.keys())
    return []


def build_best_rows(
    best_payload: Mapping[str, object],
    param_order: Sequence[str],
) -> List[Tuple[str, str]]:
    rows: List[Tuple[str, str]] = []
    rows.append(("objective", format_value(best_payload.get("objective"))))

    params = best_payload.get("params", {})
    if isinstance(params, Mapping):
        for key in param_order:
            if key in params:
                rows.append((key, format_value(params[key])))
        for key, value in sorted(params.items()):
            if key not in param_order:
                rows.append((key, format_value(value)))

    metrics = best_payload.get("metrics_excerpt", {})
    if isinstance(metrics, Mapping):
        for key in BEST_METRIC_KEYS:
            if key in metrics:
                rows.append((key, format_value(metrics[key])))
        for key, value in sorted(metrics.items()):
            if key not in BEST_METRIC_KEYS:
                rows.append((key, format_value(value)))
    return rows


def build_pareto_rows(
    pareto_payload: Sequence[Mapping[str, object]],
    param_order: Sequence[str],
) -> List[Sequence[str]]:
    rows: List[Sequence[str]] = []
    for index, entry in enumerate(pareto_payload[:10], start=1):
        objective = format_value(entry.get("objective"))
        params = entry.get("params", {})
        metrics = entry.get("metrics", {})

        params_parts: List[str] = []
        if isinstance(params, Mapping):
            for key in param_order:
                if key in params:
                    params_parts.append(f"{key}={format_value(params[key])}")
            for key, value in sorted(params.items()):
                if key not in param_order:
                    params_parts.append(f"{key}={format_value(value)}")
        params_summary = "; ".join(params_parts) if params_parts else "—"

        metric_values: List[str] = []
        if isinstance(metrics, Mapping):
            for key in PARETO_METRIC_KEYS:
                metric_values.append(format_value(metrics.get(key)))
        else:
            metric_values = ["—" for _ in PARETO_METRIC_KEYS]

        row = [
            str(index),
            objective,
            *metric_values,
            params_summary,
        ]
        rows.append(row)
    return rows


def render_budget_section(spec: Mapping[str, object]) -> List[str]:
    lines: List[str] = []
    app = spec.get("app", {})
    metrics = spec.get("metrics", {})
    if isinstance(app, Mapping):
        path = app.get("path")
        frame_budget = app.get("frame_budget_ms")
        warmup = app.get("warmup_sec")
        run_sec = app.get("run_sec")
        extra_args = app.get("extra_args") if isinstance(app.get("extra_args"), list) else []

        lines.append(f"* **Executable**: `{path}`")
        lines.append(f"* **Warmup / run**: {format_value(warmup)}s warmup, {format_value(run_sec)}s sampled")
        lines.append(f"* **Frame budget**: {format_value(frame_budget)} ms")
        if extra_args:
            args_rendered = " ".join(str(arg) for arg in extra_args)
            lines.append(f"* **Default arguments**: `{args_rendered}`")

    hard_constraints = []
    objective_line = None
    if isinstance(metrics, Mapping):
        hard = metrics.get("hard_constraints", {})
        if isinstance(hard, Mapping):
            for key, expr in hard.items():
                hard_constraints.append(f"  * `{key}` {expr}")
        objective = metrics.get("objective", {})
        if isinstance(objective, Mapping):
            expr = objective.get("expr")
            maximize = objective.get("maximize")
            direction = "maximize" if maximize else "minimize"
            objective_line = f"* **Objective**: {direction} `{expr}`"
            tiebreakers = objective.get("tiebreakers")
            if isinstance(tiebreakers, list) and tiebreakers:
                rendered = ", ".join(f"`{item}`" for item in tiebreakers)
                lines.append(f"* **Tiebreakers**: {rendered}")

    if objective_line:
        lines.append(objective_line)
    if hard_constraints:
        lines.append("* **Hard constraints:**")
        lines.extend(hard_constraints)

    return lines


def main() -> None:
    args = parse_args()

    results_dir = args.results_dir
    best_path = results_dir / "best.json"
    pareto_path = results_dir / "pareto.json"
    summary_csv_path = results_dir / "summary.csv"

    spec = load_spec(args.spec)
    best = load_json(best_path)
    pareto = load_json_sequence(pareto_path)
    summary_rows = load_summary_rows(summary_csv_path)

    param_order = extract_params_order(spec)

    env_rows = collect_env(summary_rows)
    best_rows = build_best_rows(best, param_order)
    pareto_rows = build_pareto_rows(pareto, param_order)
    budget_lines = render_budget_section(spec)

    lines: List[str] = []
    lines.append("# Autotuning Summary")
    lines.append("")
    lines.append(
        f"Generated from `{results_dir}` using spec `{args.spec}`."
    )

    lines.append("")
    lines.append("## Hardware and environment")
    lines.append("")
    if env_rows:
        env_table_rows = [[f"`{key}`", ", ".join(values)] for key, values in env_rows]
        lines.extend(render_table(["Variable", "Observed values"], env_table_rows))
    else:
        lines.append("No environment metadata was found in the summary CSV.")

    lines.append("")
    lines.append("## Budget and constraints")
    lines.append("")
    if budget_lines:
        lines.extend(budget_lines)
    else:
        lines.append("Spec file did not contain budget or constraint information.")

    lines.append("")
    lines.append("## Best configuration")
    lines.append("")
    if best_rows:
        best_table_rows = [[f"`{key}`", value] for key, value in best_rows]
        lines.extend(render_table(["Setting", "Value"], best_table_rows))
    else:
        lines.append("Best configuration payload did not include parameter or metric data.")

    lines.append("")
    lines.append("## Pareto frontier (top 10)")
    lines.append("")
    if pareto_rows:
        headers = [
            "Rank",
            "Objective",
            *[key for key in PARETO_METRIC_KEYS],
            "Parameters",
        ]
        lines.extend(render_table(headers, pareto_rows))
    else:
        lines.append("Pareto frontier data was empty.")

    lines.append("")
    lines.append("## How to use")
    lines.append("")
    lines.append("```bash")
    lines.append("./build/bin/rtfw_demo --config profiles/<cpu>-<os>.json --rt --metrics-json")
    lines.append("```")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()

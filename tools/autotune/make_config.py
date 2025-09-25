#!/usr/bin/env python3
"""Convert autotune parameters into a runtime configuration JSON."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Mapping, MutableMapping, Tuple


@dataclass(frozen=True)
class NumericRange:
    minimum: float
    maximum: float
    step: float | None = None

    def validate(self, value: float, label: str) -> None:
        if value < self.minimum or value > self.maximum:
            raise SystemExit(
                f"{label}: value {value} outside range {self.minimum}..{self.maximum}"
            )
        if self.step is not None:
            span = value - self.minimum
            steps = span / self.step
            # Allow for minor floating point noise when checking the step grid.
            if not math.isclose(round(steps), steps, rel_tol=1e-9, abs_tol=1e-9):
                raise SystemExit(
                    f"{label}: value {value} not aligned to step {self.step}"
                )


@dataclass(frozen=True)
class ParamSpec:
    name: str
    kind: str
    allowed_values: Iterable[Any] | None = None
    numeric: NumericRange | None = None

    def coerce(self, raw: Any) -> Any:
        label = f"parameter '{self.name}'"
        if self.kind == "categorical":
            if not isinstance(raw, str):
                raise SystemExit(f"{label}: expected string value, got {type(raw).__name__}")
            if self.allowed_values is not None and raw not in self.allowed_values:
                raise SystemExit(
                    f"{label}: value '{raw}' not in allowed set {list(self.allowed_values)}"
                )
            return raw

        if self.kind == "bool":
            if isinstance(raw, bool):
                return raw
            if isinstance(raw, str):
                lowered = raw.lower()
                if lowered in {"true", "1"}:
                    return True
                if lowered in {"false", "0"}:
                    return False
            raise SystemExit(f"{label}: expected boolean value, got {raw!r}")

        if self.kind == "int":
            try:
                value = int(raw)
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"{label}: expected integer value, got {raw!r}") from exc
            if isinstance(raw, float) and not raw.is_integer():
                raise SystemExit(f"{label}: expected integer value, got {raw!r}")
            if self.numeric is not None:
                self.numeric.validate(float(value), label)
            if self.allowed_values is not None and value not in self.allowed_values:
                raise SystemExit(
                    f"{label}: value {value!r} not in allowed set {list(self.allowed_values)}"
                )
            return value

        if self.kind == "float":
            try:
                value = float(raw)
            except (TypeError, ValueError) as exc:
                raise SystemExit(f"{label}: expected float value, got {raw!r}") from exc
            if self.numeric is not None:
                self.numeric.validate(value, label)
            # Normalise to the closest grid point if a step was defined to avoid
            # tiny floating point drift in the emitted JSON.
            if self.numeric and self.numeric.step is not None:
                steps = round((value - self.numeric.minimum) / self.numeric.step)
                value = self.numeric.minimum + steps * self.numeric.step
            if self.allowed_values is not None and value not in self.allowed_values:
                raise SystemExit(
                    f"{label}: value {value!r} not in allowed set {list(self.allowed_values)}"
                )
            return value

        raise SystemExit(f"Unsupported parameter type '{self.kind}' for {self.name}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert autotune parameters into runtime configuration JSON."
    )
    parser.add_argument("--spec", required=True, type=pathlib.Path)
    parser.add_argument("--params-json", required=True)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    return parser.parse_args()


def parse_scalar(token: str) -> Any:
    if token == "":
        return ""
    if token.startswith("\"") and token.endswith("\""):
        return token[1:-1]
    lowered = token.lower()
    if lowered == "true":
        return True
    if lowered == "false":
        return False
    try:
        if token.startswith("0") and token not in {"0", "0.0"} and not token.startswith("0."):
            # Treat as string to avoid octal-like interpretation.
            raise ValueError
        value = int(token)
        return value
    except ValueError:
        try:
            return float(token)
        except ValueError:
            return token


def preprocess_yaml(text: str) -> List[Tuple[int, str]]:
    entries: List[Tuple[int, str]] = []
    for raw_line in text.splitlines():
        if not raw_line.strip() or raw_line.lstrip().startswith("#"):
            continue
        indent = len(raw_line) - len(raw_line.lstrip(" "))
        entries.append((indent, raw_line.strip()))
    return entries


def parse_sequence(entries: List[Tuple[int, str]], index: int, indent: int) -> Tuple[List[Any], int]:
    items: List[Any] = []
    while index < len(entries):
        current_indent, content = entries[index]
        if current_indent < indent or not content.startswith("- "):
            break
        value_part = content[2:].strip()
        index += 1
        if value_part:
            items.append(parse_scalar(value_part))
        else:
            if index >= len(entries):
                items.append({})
                break
            next_indent, next_content = entries[index]
            if next_indent <= current_indent:
                items.append({})
                continue
            if next_content.startswith("- "):
                value, index = parse_sequence(entries, index, next_indent)
            else:
                value, index = parse_mapping(entries, index, next_indent)
            items.append(value)
    return items, index


def parse_mapping(entries: List[Tuple[int, str]], index: int, indent: int) -> Tuple[Dict[str, Any], int]:
    mapping: Dict[str, Any] = {}
    while index < len(entries):
        current_indent, content = entries[index]
        if current_indent < indent:
            break
        if current_indent != indent:
            raise SystemExit(f"Invalid indentation near: {content}")
        if ":" not in content:
            raise SystemExit(f"Expected ':' in line: {content}")
        key_part, _, value_part = content.partition(":")
        key = key_part.strip()
        value_part = value_part.strip()
        index += 1
        if value_part:
            mapping[key] = parse_scalar(value_part)
            continue
        if index >= len(entries) or entries[index][0] <= current_indent:
            mapping[key] = {}
            continue
        next_indent, next_content = entries[index]
        if next_content.startswith("- "):
            value, index = parse_sequence(entries, index, next_indent)
        else:
            value, index = parse_mapping(entries, index, next_indent)
        mapping[key] = value
    return mapping, index


def load_simple_yaml(path: pathlib.Path) -> Mapping[str, Any]:
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise SystemExit(f"Spec file not found: {path}") from exc
    entries = preprocess_yaml(text)
    if not entries:
        return {}
    data, index = parse_mapping(entries, 0, entries[0][0])
    if index != len(entries):
        raise SystemExit("Failed to parse entire YAML file")
    return data


def load_spec(path: pathlib.Path) -> Mapping[str, ParamSpec]:
    data = load_simple_yaml(path)

    if not isinstance(data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping at the top level")

    params = data.get("params")
    if not isinstance(params, Mapping):
        raise SystemExit("Spec YAML missing 'params' mapping")

    parsed: Dict[str, ParamSpec] = {}
    for name, payload in params.items():
        if not isinstance(payload, Mapping):
            raise SystemExit(f"Param '{name}' must map to a dictionary")
        kind = payload.get("type")
        if kind not in {"categorical", "int", "float", "bool"}:
            raise SystemExit(f"Param '{name}' has unsupported type '{kind}'")

        allowed = None
        numeric = None
        if kind == "categorical":
            values = payload.get("values")
            if not isinstance(values, list) or not values:
                raise SystemExit(f"Param '{name}' requires a non-empty 'values' list")
            allowed = tuple(values)
        elif kind in {"int", "float"}:
            minimum = payload.get("min")
            maximum = payload.get("max")
            step = payload.get("step")
            values = payload.get("values")
            if values is not None:
                if not isinstance(values, list) or not values:
                    raise SystemExit(
                        f"Param '{name}' provides invalid 'values' for numeric type"
                    )
                allowed = tuple(values)
            if minimum is not None or maximum is not None:
                if minimum is None or maximum is None:
                    raise SystemExit(
                        f"Param '{name}' must define both 'min' and 'max' or neither"
                    )
                numeric = NumericRange(float(minimum), float(maximum), float(step) if step is not None else None)
        parsed[name] = ParamSpec(name=name, kind=kind, allowed_values=allowed, numeric=numeric)

    return parsed


def parse_params(payload: str) -> Dict[str, Any]:
    try:
        parsed = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse --params-json payload: {exc}")
    if not isinstance(parsed, dict):
        raise SystemExit("--params-json must decode to an object")
    return parsed


def validate_params(params: Mapping[str, Any], specs: Mapping[str, ParamSpec]) -> Dict[str, Any]:
    validated: Dict[str, Any] = {}
    for name, spec in specs.items():
        if name not in params:
            raise SystemExit(f"Missing required parameter '{name}'")
        validated[name] = spec.coerce(params[name])

    extra = set(params.keys()) - set(specs.keys())
    if extra:
        raise SystemExit(f"Unknown parameters provided: {', '.join(sorted(extra))}")
    return validated


def set_path(target: MutableMapping[str, Any], path: Iterable[str], value: Any) -> None:
    current: MutableMapping[str, Any] = target
    *parents, leaf = list(path)
    for key in parents:
        node = current.get(key)
        if not isinstance(node, MutableMapping):
            node = {}
            current[key] = node
        current = node  # type: ignore[assignment]
    current[leaf] = value


def build_config(params: Mapping[str, Any]) -> Dict[str, Any]:
    config: Dict[str, Any] = {}
    mapping = {
        "threads": ("threads",),
        "chunk_target_us": ("chunking", "target_p90_us"),
        "aosoa_block": ("layout", "aosoa_block"),
        "steal_threshold": ("scheduler", "steal_threshold"),
        "prefetch_distance_bytes": ("prefetch", "distance_bytes"),
        "fma_mode": ("numerics", "fma"),
        "arena_per_thread_mb": ("memory", "arena_per_thread_mb"),
        "huge_pages": ("memory", "huge_pages"),
        "emergency_spawn_enabled": ("emergency", "enabled"),
        "governor_target_util": ("governor", "target_util"),
        "governor_hysteresis": ("governor", "hysteresis"),
    }

    for name, path in mapping.items():
        if name not in params:
            continue
        set_path(config, path, params[name])

    # Emit the original params alongside the resolved config for traceability.
    config["params"] = dict(params)
    return config


def write_json(path: pathlib.Path, data: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")


def main() -> None:
    args = parse_args()
    specs = load_spec(args.spec)
    params = parse_params(args.params_json)
    validated = validate_params(params, specs)
    config = build_config(validated)
    write_json(args.out, config)


if __name__ == "__main__":
    main()

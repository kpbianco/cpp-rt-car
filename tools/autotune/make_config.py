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
    parser.add_argument("--spec", type=pathlib.Path)
    parser.add_argument("--params-json")
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument(
        "--dump-mapped-params",
        action="store_true",
        help=(
            "Print the names of parameters mapped by build_config and exit. "
            "Useful for coverage checks."
        ),
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run internal sanity checks and exit",
    )

    args = parser.parse_args()

    if not args.dump_mapped_params and not args.self_test:
        missing = [
            flag
            for flag in ("spec", "params_json", "out")
            if getattr(args, flag) is None
        ]
        if missing:
            parser.error(
                "--spec, --params-json and --out are required unless "
                "--dump-mapped-params is provided"
            )

    return args


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
            if ":" in value_part:
                key_part, _, value_token = value_part.partition(":")
                key = key_part.strip()
                value_token = value_token.strip()
                item: Dict[str, Any] = {}
                if value_token:
                    item[key] = parse_scalar(value_token)
                else:
                    if index >= len(entries) or entries[index][0] <= current_indent:
                        item[key] = {}
                    else:
                        next_indent, next_content = entries[index]
                        if next_content.startswith("- "):
                            value, index = parse_sequence(entries, index, next_indent)
                        else:
                            value, index = parse_mapping(entries, index, next_indent)
                        item[key] = value
                while index < len(entries):
                    next_indent, next_content = entries[index]
                    if next_indent <= current_indent or next_content.startswith("- "):
                        break
                    if ":" not in next_content:
                        raise SystemExit(f"Expected ':' in line: {next_content}")
                    field_key_part, _, field_value_token = next_content.partition(":")
                    field_key = field_key_part.strip()
                    field_value_token = field_value_token.strip()
                    index += 1
                    if field_value_token:
                        item[field_key] = parse_scalar(field_value_token)
                        continue
                    if index >= len(entries) or entries[index][0] <= next_indent:
                        item[field_key] = {}
                        continue
                    deeper_indent, deeper_content = entries[index]
                    if deeper_content.startswith("- "):
                        value, index = parse_sequence(entries, index, deeper_indent)
                    else:
                        value, index = parse_mapping(entries, index, deeper_indent)
                    item[field_key] = value
                items.append(item)
            else:
                items.append(parse_scalar(value_part))
            continue
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


PARAM_TO_CONFIG_PATH: Mapping[str, Tuple[str, ...]] = {
    "threads": ("threads",),
    "chunk_target_us": ("chunking", "target_p90_us"),
    "aosoa_block": ("layout", "aosoa_block"),
    "steal_threshold": ("scheduler", "steal_threshold"),
    "prefetch_distance_bytes": ("prefetch", "distance_bytes"),
    "fma_mode": ("numerics", "fma"),
    "ftz_daz": ("numerics", "ftz_daz"),
    "arena_per_thread_mb": ("memory", "arena_per_thread_mb"),
    "huge_pages": ("memory", "huge_pages"),
    "emergency_spawn_enabled": ("scheduler", "emergency_spawn"),
    "priority_policy": ("scheduler", "priority_policy"),
    "governor_target_util": ("governor", "target_util"),
    "governor_hysteresis": ("governor", "hysteresis"),
}

#: Names of autotune parameters that build_config knows how to translate. When
#: adding new knobs to spec.yaml, extend this set (and PARAM_TO_CONFIG_PATH) so
#: coverage checks continue to pass.
MAPPED_PARAMS = frozenset(PARAM_TO_CONFIG_PATH.keys())


def build_config(params: Mapping[str, Any]) -> Dict[str, Any]:
    config: Dict[str, Any] = {}

    for name, path in PARAM_TO_CONFIG_PATH.items():
        if name not in params:
            continue
        set_path(config, path, params[name])

    chunking = config.setdefault("chunking", {})
    chunking.setdefault("min_items", 1)
    chunking.setdefault("max_items", 1024)

    layout = config.setdefault("layout", {})
    layout.setdefault("align_bytes", 64)
    layout.setdefault("pad_to_simd", True)

    prefetch = config.setdefault("prefetch", {})
    if "prefetch_distance_bytes" in params:
        enabled = params["prefetch_distance_bytes"] > 0
        set_path(config, ("prefetch", "enabled"), enabled)
    prefetch.setdefault("distance_bytes", 0)
    prefetch.setdefault("enabled", prefetch.get("distance_bytes", 0) > 0)

    scheduler = config.setdefault("scheduler", {})
    scheduler.setdefault("priority_policy", "normal")
    scheduler.setdefault("emergency_spawn", True)

    numerics = config.setdefault("numerics", {})
    numerics.setdefault("ftz_daz", True)

    memory = config.setdefault("memory", {})
    memory.setdefault("numa", "first_touch")
    memory.setdefault("pretouch", True)

    tracing = config.setdefault("tracing", {})
    tracing.setdefault("bintrace", True)

    # Emit the original params alongside the resolved config for traceability.
    config["params"] = dict(params)
    return config


def write_json(path: pathlib.Path, data: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")


def run_self_test() -> None:
    canonical_params = {
        "threads": "physical",
        "chunk_target_us": 100,
        "aosoa_block": 256,
        "prefetch_distance_bytes": 128,
        "steal_threshold": 4,
        "priority_policy": "normal",
        "emergency_spawn_enabled": True,
        "governor_target_util": 0.95,
        "governor_hysteresis": 0.02,
        "ftz_daz": True,
        "fma_mode": "auto_no_when_deterministic",
        "huge_pages": True,
        "arena_per_thread_mb": 64,
    }

    config = build_config(canonical_params)

    def require(path: Tuple[str, ...], expected: Any) -> None:
        node: Any = config
        for key in path:
            if not isinstance(node, Mapping):
                raise SystemExit(
                    f"SELF-TEST FAILED: {'.'.join(path)} missing (encountered non-mapping)"
                )
            if key not in node:
                raise SystemExit(f"SELF-TEST FAILED: {'.'.join(path)} not present in config")
            node = node[key]
        if node != expected:
            raise SystemExit(
                f"SELF-TEST FAILED: {'.'.join(path)} expected {expected!r}, got {node!r}"
            )

    require(("threads",), "physical")
    require(("chunking", "target_p90_us"), 100)
    require(("chunking", "min_items"), 1)
    require(("chunking", "max_items"), 1024)
    require(("layout", "aosoa_block"), 256)
    require(("layout", "align_bytes"), 64)
    require(("layout", "pad_to_simd"), True)
    require(("prefetch", "distance_bytes"), 128)
    require(("prefetch", "enabled"), True)
    require(("scheduler", "steal_threshold"), 4)
    require(("scheduler", "priority_policy"), "normal")
    require(("scheduler", "emergency_spawn"), True)
    require(("governor", "target_util"), 0.95)
    require(("governor", "hysteresis"), 0.02)
    require(("numerics", "ftz_daz"), True)
    require(("numerics", "fma"), "auto_no_when_deterministic")
    require(("memory", "huge_pages"), True)
    require(("memory", "arena_per_thread_mb"), 64)
    require(("memory", "numa"), "first_touch")
    require(("memory", "pretouch"), True)
    require(("tracing", "bintrace"), True)

    if config.get("params") != canonical_params:
        raise SystemExit("SELF-TEST FAILED: params block did not round-trip canonical values")

    print("SELF-TEST OK")


def main() -> None:
    args = parse_args()
    if args.self_test:
        run_self_test()
        return
    if args.dump_mapped_params:
        for name in sorted(MAPPED_PARAMS):
            print(name)
        return
    specs = load_spec(args.spec)
    params = parse_params(args.params_json)
    validated = validate_params(params, specs)
    config = build_config(validated)
    write_json(args.out, config)


if __name__ == "__main__":
    main()

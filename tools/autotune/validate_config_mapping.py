#!/usr/bin/env python3
"""Validate generated mapping configs against a JSON schema."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any, Iterable, Mapping

DEFAULT_SCHEMA_PATH = pathlib.Path(__file__).with_name("config.schema.json")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate mapping configuration files against a schema",
    )
    parser.add_argument(
        "--schema",
        type=pathlib.Path,
        default=DEFAULT_SCHEMA_PATH,
        help=f"Path to config schema (default: {DEFAULT_SCHEMA_PATH})",
    )
    parser.add_argument(
        "--config",
        type=pathlib.Path,
        required=True,
        help="Path to the generated configuration JSON",
    )
    return parser.parse_args()


def load_json(path: pathlib.Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as fh:
            return json.load(fh)
    except FileNotFoundError as exc:
        raise SystemExit(f"File not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"Failed to parse JSON from {path}: {exc}") from exc


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _type_matches(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, Mapping)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return _is_integer(value)
    if expected == "number":
        return _is_number(value)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    raise ValueError(f"Unsupported schema type '{expected}'")


def _format_expected(types: Iterable[str]) -> str:
    items = list(types)
    if not items:
        return "unknown"
    if len(items) == 1:
        return items[0]
    return ", ".join(items[:-1]) + f" or {items[-1]}"


def validate(instance: Any, schema: Mapping[str, Any], path: str = "$") -> list[str]:
    errors: list[str] = []

    schema_type = schema.get("type")
    allowed_types: list[str] | None
    if isinstance(schema_type, list):
        allowed_types = [str(t) for t in schema_type]
    elif isinstance(schema_type, str):
        allowed_types = [schema_type]
    else:
        allowed_types = None

    if allowed_types:
        if not any(_type_matches(instance, candidate) for candidate in allowed_types):
            errors.append(
                f"{path}: expected {_format_expected(allowed_types)}, got {type(instance).__name__}"
            )
            return errors

    if "enum" in schema:
        options = schema["enum"]
        if instance not in options:
            errors.append(f"{path}: expected one of {options!r}, got {instance!r}")
            return errors

    if (allowed_types and allowed_types == ["array"]) or (
        allowed_types is None and "items" in schema
    ):
        if not isinstance(instance, list):
            errors.append(f"{path}: expected array, got {type(instance).__name__}")
            return errors
        item_schema = schema.get("items")
        if isinstance(item_schema, Mapping):
            for index, item in enumerate(instance):
                child_path = f"{path}[{index}]"
                errors.extend(validate(item, item_schema, child_path))
        return errors

    if (allowed_types and allowed_types == ["object"]) or (
        allowed_types is None and "properties" in schema
    ):
        if not isinstance(instance, Mapping):
            errors.append(f"{path}: expected object, got {type(instance).__name__}")
            return errors
        required = schema.get("required", [])
        for name in required:
            if name not in instance:
                errors.append(f"{path}.{name}: missing required property")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, value in instance.items():
            if key in properties:
                child_path = f"{path}.{key}"
                errors.extend(validate(value, properties[key], child_path))
            else:
                if isinstance(additional, Mapping):
                    child_path = f"{path}.{key}"
                    errors.extend(validate(value, additional, child_path))
                elif additional is False:
                    errors.append(f"{path}.{key}: additional properties are not allowed")
        return errors

    return errors


def main() -> None:
    args = parse_args()
    schema_payload = load_json(args.schema)
    config_payload = load_json(args.config)

    errors = validate(config_payload, schema_payload, "$")
    if errors:
        print("Validation failed:", file=sys.stderr)
        for message in errors:
            print(f" - {message}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

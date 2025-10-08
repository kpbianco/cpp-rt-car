#!/usr/bin/env python3
"""Validate generated mapping configs against a JSON schema."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from collections.abc import Iterable, Mapping
from typing import Any

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

def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value: Any) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool))


def _describe_type(value: Any) -> str:
    if isinstance(value, bool):
        return "boolean"
    if value is None:
        return "null"
    if isinstance(value, Mapping):
        return "object"
    if isinstance(value, list):
        return "array"
    if _is_integer(value):
        return "integer"
    if _is_number(value):
        return "number"
    if isinstance(value, str):
        return "string"
    return type(value).__name__


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


def _as_type_list(schema_type: Any) -> Iterable[str]:
    if isinstance(schema_type, str):
        return [schema_type]
    if isinstance(schema_type, Iterable):
        return [str(item) for item in schema_type]
    return []


def validate(instance: Any, schema: Mapping[str, Any], path: str = "$") -> list[str]:
    errors: list[str] = []

    schema_types = list(_as_type_list(schema.get("type")))
    if schema_types:
        if not any(_type_matches(instance, expected) for expected in schema_types):
            expected_label = " or ".join(schema_types)
            errors.append(f"{path} expected {expected_label}, got {_describe_type(instance)}")
            return errors

    if schema.get("enum") is not None:
        options = list(schema["enum"])
        if instance not in options:
            errors.append(f"{path} must be one of {options!r}")
            return errors

    is_object = "object" in schema_types or (
        not schema_types and "properties" in schema
    )
    if is_object:
        if not isinstance(instance, Mapping):
            errors.append(f"{path} expected object, got {_describe_type(instance)}")
            return errors

        required = schema.get("required", [])
        for name in required:
            if name not in instance:
                errors.append(f"Missing required property: {path}.{name}")

        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, value in instance.items():
            child_path = f"{path}.{key}"
            if key in properties:
                errors.extend(validate(value, properties[key], child_path))
            else:
                if isinstance(additional, Mapping):
                    errors.extend(validate(value, additional, child_path))
                elif additional is False:
                    errors.append(f"{child_path} is not allowed")
        return errors

    is_array = "array" in schema_types or (
        not schema_types and "items" in schema
    )
    if is_array:
        if not isinstance(instance, list):
            errors.append(f"{path} expected array, got {_describe_type(instance)}")
            return errors
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(instance):
                child_path = f"{path}[{index}]"
                errors.extend(validate(item, item_schema, child_path))
        return errors

    return errors


def main() -> None:
    args = parse_args()
    schema_payload = load_json(args.schema)
    config_payload = load_json(args.config)

    errors = validate(config_payload, schema_payload, "$")
    if errors:
        print("Validation failed:", file=sys.stderr)
        for issue in errors:
            print(f" - {issue}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

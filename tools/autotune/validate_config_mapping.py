#!/usr/bin/env python3
"""Validate generated mapping configs against a JSON schema."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any, Mapping

DEFAULT_SCHEMA_PATH = pathlib.Path(__file__).with_name("config.schema.json")


class ValidationError(Exception):
    """Raised when a payload does not satisfy the schema."""


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


def _validate_type(value: Any, expected: str, path: str) -> None:
    if expected == "object":
        if not isinstance(value, Mapping):
            raise ValidationError(f"{path} must be an object")
        return
    if expected == "array":
        if not isinstance(value, list):
            raise ValidationError(f"{path} must be an array")
        return
    if expected == "string":
        if not isinstance(value, str):
            raise ValidationError(f"{path} must be a string")
        return
    if expected == "integer":
        if not _is_integer(value):
            raise ValidationError(f"{path} must be an integer")
        return
    if expected == "number":
        if not _is_number(value):
            raise ValidationError(f"{path} must be a number")
        return
    if expected == "boolean":
        if not isinstance(value, bool):
            raise ValidationError(f"{path} must be a boolean")
        return
    if expected == "null":
        if value is not None:
            raise ValidationError(f"{path} must be null")
        return
    raise ValidationError(f"{path} uses unsupported schema type '{expected}'")


def validate(instance: Any, schema: Mapping[str, Any], path: str = "$") -> None:
    schema_type = schema.get("type")
    if isinstance(schema_type, str):
        _validate_type(instance, schema_type, path)
    if "enum" in schema:
        options = schema["enum"]
        if instance not in options:
            raise ValidationError(f"{path} must be one of {options!r}")
    if schema_type == "object" or (
        schema_type is None and "properties" in schema
    ):
        if not isinstance(instance, Mapping):
            raise ValidationError(f"{path} must be an object")
        required = schema.get("required", [])
        for name in required:
            if name not in instance:
                raise ValidationError(f"{path}.{name} is a required property")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, value in instance.items():
            if key in properties:
                child_path = f"{path}.{key}"
                validate(value, properties[key], child_path)
            else:
                if isinstance(additional, Mapping):
                    child_path = f"{path}.{key}"
                    validate(value, additional, child_path)
                elif additional is False:
                    raise ValidationError(f"{path}.{key} is not an allowed property")
        return
    if schema_type == "array" or (
        schema_type is None and "items" in schema
    ):
        if not isinstance(instance, list):
            raise ValidationError(f"{path} must be an array")
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(instance):
                child_path = f"{path}[{index}]"
                validate(item, item_schema, child_path)
        return
    if schema_type in {"string", "integer", "number", "boolean", "null"}:
        return
    if schema_type is None:
        return
    raise ValidationError(f"{path} uses unsupported schema construction")


def main() -> None:
    args = parse_args()
    schema_payload = load_json(args.schema)
    config_payload = load_json(args.config)

    try:
        validate(config_payload, schema_payload, "$")
    except ValidationError as exc:
        print(f"Validation failed: {exc}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

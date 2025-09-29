#!/usr/bin/env python3
"""Generate screening candidates for the autotuner."""

from __future__ import annotations

import argparse
import json
import pathlib
import random
import sys
from dataclasses import dataclass
from typing import Any, Dict, List, Mapping, Sequence, Tuple

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.autotune.make_config import load_simple_yaml


@dataclass(frozen=True)
class NumericParam:
    name: str
    kind: str  # "int" or "float"
    minimum: float
    maximum: float
    step: float | None

    def _decimal_places(self) -> int:
        if self.step is None:
            return 12
        text = f"{self.step:.12f}".rstrip("0").rstrip(".")
        if "." in text:
            fraction = text.split(".", 1)[1]
            return min(len(fraction), 12)
        return 0

    def clamp(self, value: float) -> float:
        if value < self.minimum:
            return self.minimum
        if value > self.maximum:
            return self.maximum
        return value

    def snap(self, value: float) -> float:
        value = self.clamp(value)
        if self.step is None:
            return value
        steps = round((value - self.minimum) / self.step)
        snapped = self.minimum + steps * self.step
        # Guard against floating point drift pushing us slightly out of range.
        if snapped < self.minimum:
            snapped = self.minimum
        if snapped > self.maximum:
            snapped = self.maximum
        return snapped

    def convert(self, sample: float) -> Any:
        span = self.maximum - self.minimum
        scaled = self.minimum + sample * span
        snapped = self.snap(scaled)
        if self.kind == "int":
            return int(round(snapped))
        return round(snapped, self._decimal_places())


@dataclass(frozen=True)
class EnumParam:
    name: str
    values: Tuple[Any, ...]

    def sample(self, rng: random.Random) -> Any:
        return rng.choice(self.values)


@dataclass(frozen=True)
class BoolParam:
    name: str

    def sample(self, rng: random.Random) -> bool:
        return rng.choice([True, False])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate screening samples for autotune")
    parser.add_argument("--spec", required=True, type=pathlib.Path)
    parser.add_argument("--n", required=True, type=int)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument(
        "--seed",
        type=int,
        default=None,
        help="Optional random seed for reproducibility.",
    )
    return parser.parse_args()


def load_spec(path: pathlib.Path) -> Mapping[str, Any]:
    data = load_simple_yaml(path)
    if not isinstance(data, Mapping):
        raise SystemExit("Spec YAML must contain a mapping at the top level")
    params = data.get("params")
    if not isinstance(params, Mapping):
        raise SystemExit("Spec YAML missing 'params' mapping")
    return params


def parse_params(params: Mapping[str, Any]) -> Tuple[List[NumericParam], List[EnumParam], List[BoolParam]]:
    numeric: List[NumericParam] = []
    enums: List[EnumParam] = []
    bools: List[BoolParam] = []

    for name, payload in params.items():
        if not isinstance(payload, Mapping):
            raise SystemExit(f"Param '{name}' must map to a dictionary")
        kind = payload.get("type")
        if kind not in {"categorical", "int", "float", "bool"}:
            raise SystemExit(f"Param '{name}' has unsupported type '{kind}'")

        if kind == "categorical":
            values = payload.get("values")
            if not isinstance(values, Sequence) or not values:
                raise SystemExit(f"Param '{name}' requires non-empty 'values'")
            enums.append(EnumParam(name=name, values=tuple(values)))
            continue

        if kind == "bool":
            bools.append(BoolParam(name=name))
            continue

        values = payload.get("values")
        if isinstance(values, Sequence) and values:
            enums.append(EnumParam(name=name, values=tuple(values)))
            continue

        minimum = payload.get("min")
        maximum = payload.get("max")
        if minimum is None or maximum is None:
            raise SystemExit(f"Param '{name}' must define 'min' and 'max'")
        step = payload.get("step")
        numeric.append(
            NumericParam(
                name=name,
                kind=kind,
                minimum=float(minimum),
                maximum=float(maximum),
                step=float(step) if step is not None else None,
            )
        )

    return numeric, enums, bools


def latin_hypercube(n: int, dimensions: int, rng: random.Random) -> List[List[float]]:
    if n <= 0:
        return []
    samples = [[0.0 for _ in range(dimensions)] for _ in range(n)]
    for dim in range(dimensions):
        intervals = list(range(n))
        rng.shuffle(intervals)
        for idx, interval in enumerate(intervals):
            low = interval / n
            high = (interval + 1) / n
            samples[idx][dim] = rng.uniform(low, high)
    return samples


def random_numeric_sample(param: NumericParam, rng: random.Random) -> Any:
    raw = rng.uniform(0.0, 1.0)
    return param.convert(raw)


def build_candidate(
    numeric_params: List[NumericParam],
    enum_params: List[EnumParam],
    bool_params: List[BoolParam],
    numeric_values: Sequence[float],
    rng: random.Random,
) -> Dict[str, Any]:
    params: Dict[str, Any] = {}

    for param, sample in zip(numeric_params, numeric_values):
        params[param.name] = param.convert(sample)

    for param in enum_params:
        params[param.name] = param.sample(rng)

    for param in bool_params:
        params[param.name] = param.sample(rng)

    return params


def ensure_directory(path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def write_output(path: pathlib.Path, candidates: List[Mapping[str, Any]]) -> None:
    ensure_directory(path)
    payload: Dict[str, Any] = {"candidates": candidates}
    with path.open("w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")


def generate_candidates(
    numeric_params: List[NumericParam],
    enum_params: List[EnumParam],
    bool_params: List[BoolParam],
    count: int,
    rng: random.Random,
) -> List[Dict[str, Any]]:
    dimension = len(numeric_params)
    lhs_samples = latin_hypercube(count, dimension, rng) if dimension > 0 else []

    candidates: List[Dict[str, Any]] = []
    seen: set[str] = set()

    for idx in range(count):
        numeric_values = lhs_samples[idx] if dimension > 0 else []
        candidate = build_candidate(numeric_params, enum_params, bool_params, numeric_values, rng)
        key = json.dumps(candidate, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        candidates.append(candidate)

    attempts = 0
    max_attempts = count * 50
    while len(candidates) < count and attempts < max_attempts:
        attempts += 1
        numeric_values = [random_numeric_sample(param, rng) for param in numeric_params]
        candidate: Dict[str, Any] = {}
        for param, value in zip(numeric_params, numeric_values):
            candidate[param.name] = value
        for param in enum_params:
            candidate[param.name] = param.sample(rng)
        for param in bool_params:
            candidate[param.name] = param.sample(rng)
        key = json.dumps(candidate, sort_keys=True)
        if key in seen:
            continue
        seen.add(key)
        candidates.append(candidate)

    if len(candidates) < count:
        raise SystemExit("Unable to generate enough unique candidates within constraints")

    return candidates[:count]


def main() -> None:
    args = parse_args()
    rng = random.Random(args.seed)

    raw_params = load_spec(args.spec)
    numeric_params, enum_params, bool_params = parse_params(raw_params)

    candidates = generate_candidates(numeric_params, enum_params, bool_params, args.n, rng)
    write_output(args.out, candidates)


if __name__ == "__main__":
    main()

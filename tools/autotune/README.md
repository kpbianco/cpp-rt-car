# Autotune Specification

This directory contains configuration and orchestration scripts for the runtime autotuning workflow.  The `spec.yaml` file is the machine-readable source of truth for the tuner describing how to execute the application, what metrics to enforce, and the parameter space to explore.

## File: `spec.yaml`

The YAML document is organized into three top-level sections:

### `app`
Describes how to run the realtime application when gathering interval metrics.

| Field | Description |
| --- | --- |
| `path` | Executable path to the demo binary that emits metrics. |
| `warmup_sec` | Seconds to run before samples are collected, allowing the app to reach steady state. |
| `run_sec` | Seconds of metric sampling after warmup. |
| `frame_budget_ms` | Target frame duration used in objective normalization. |
| `extra_args` | Additional CLI arguments always passed to the executable. |

### `metrics`
Defines success criteria and the optimization objective derived from the reported metrics.

* `hard_constraints` — Map of metric names to comparison expressions that must hold for a run to be considered valid.
* `objective` — Specifies how scores are computed:
  * `expr` — Primary scalar objective expression (lower is better when `maximize: false`).
  * `tiebreakers` — Ordered list of secondary expressions used when two configurations share the same primary objective.
  * `maximize` — Boolean indicating whether higher (`true`) or lower (`false`) values are preferred.

### `params`
Enumerates the tunable configuration parameters. Each key defines a parameter with one of the following types:

* `categorical` — Explicit list of `values` to choose from.
* `int` — Integer parameter with inclusive `min`/`max` bounds and an optional `step` size.
* `float` — Floating-point parameter with inclusive bounds and step size.
* `bool` — Boolean toggle (no additional fields required).

These definitions drive the design-of-experiments sampling, local search bounds, and downstream validation.

## JSONL result schema

Every invocation of `run_one.py` produces a single JSON object describing the sampled run. These rows are appended verbatim to
`results/experiments.jsonl` and `results/validation_runs.jsonl`. Each record follows the stable schema below:

```
{
  "ok": bool,
  "objective": float | Infinity,
  "metrics": { ... raw interval JSON from the application ... },
  "_summary": { ... derived metrics such as p50_frame_ms ... },
  "_params": { ... resolved parameter values ... },
  "_seed": int,
  "_scenario": str,
  "_ts": "<ISO8601 timestamp>",
  "env": { "cpu": str, "cores": int, "os": str },
  "_schema": "v1"
}
```

`objective` is set to `Infinity` when hard constraints fail. `_summary` mirrors the previous aggregate metrics payload and is used
by the optimisation routines, while `metrics` keeps the raw interval JSON emitted by the realtime application for traceability.
Append operations are crash-safe: each line is flushed and `fsync`'d before the next record is written so that interrupted runs
cannot corrupt the log.

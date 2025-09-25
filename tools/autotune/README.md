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

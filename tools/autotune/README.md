# Autotune Tooling

This directory implements a design-of-experiments, scoring, search, validation,
and reporting pipeline. `spec_smoke.yaml` plus `smoke_app.py` are the supported
end-to-end test target in RTFW 0.1.

`spec.yaml` is a target integration design, not an operational `rtfw_demo`
configuration. The demo does not yet implement the `--config`, `--run`, or
`--rt` arguments issued by `run_one.py`, and it does not load the generated
profile. See the [status guide](../../docs/DOE_AUTOTUNE.md).

## Specification

A spec has these main sections:

- `app`: executable path, warm-up and measurement durations, frame budget, and
  extra arguments;
- `metrics`: hard constraints and an ordered scalar objective;
- `params`: categorical, integer, floating-point, or Boolean factors;
- optional scenarios and robustness seeds.

The current factor mapper can emit configuration fields for threads, chunking,
layout, scheduler experiments, prefetch, numerical settings, arenas, huge
pages, emergency helpers, and governor controls. Schema-valid output does not
mean the production runtime consumes those fields.

## Supported smoke

```bash
tools/autotune/run_smoke.sh
python3 tools/autotune/make_config.py --self-test
python3 tools/autotune/mapping_smoke.py
```

The first command targets the synthetic app. The latter commands verify
mapping coverage and schema behavior. The planned real-demo dry run is disabled
unless `RTFW_ENABLE_PLANNED_AUTOTUNE_ROUNDTRIP=1`; enabling it in 0.1 is
expected to fail because the runtime interface is not implemented.

## Result records

Each `run_one.py` invocation emits a JSON object containing:

- `ok` and `objective`;
- raw application `metrics`;
- derived `_summary`;
- sampled `_params`, `_seed`, `_scenario`, and `_ts`;
- host `env` metadata;
- `_schema: "v1"`.

JSONL append operations flush and `fsync` each record. This protects the log
from a partially buffered final record; it does not make a trial valid or
reproducible. Consumers must verify provenance, schema, constraints, and dropped
telemetry.

## Runtime integration gate

Before switching `spec.yaml` from planned to supported, CI must build
`rtfw_demo`, run at least one generated config through it, prove every tunable
changes the resolved runtime policy, verify bounded warm-up/measurement
semantics, and validate direct frame/deadline metrics. This is tracked in the
[roadmap](../../docs/roadmap.md).

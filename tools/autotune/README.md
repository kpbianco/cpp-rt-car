# Autotune Tooling

This directory implements design-of-experiments generation, scoring, search,
robustness validation, persistence, analysis, profile installation, and
reporting.

- `spec_smoke.yaml` plus `smoke_app.py` are the fast synthetic pipeline test.
- `spec.yaml` plus `rtfw_runtime_demo` are the operational target-runtime
  integration in RTFW 1.1.

See the [status and claim guide](../../docs/DOE_AUTOTUNE.md) before interpreting
results.

## Specification

A spec contains:

- `app`: executable, warm-up/measurement durations, frame budget, and
  arguments;
- `metrics`: hard constraints and ordered scalar objective;
- `params`: categorical, integer, floating-point, or Boolean factors;
- optional scenarios and robustness seeds.

The production spec deliberately maps only `worker_count`,
`executor_policy`, and `executor_queue_capacity`. `check_mapping_coverage.py`
fails if a production factor is not explicitly represented by
`make_config.py`.

`make_config.py` emits a complete resolved profile described by
`config.schema.json`. The runtime fields are authoritative configuration;
`params` is opaque experiment provenance. A schema pass is necessary but not
sufficient: the C++ loader performs exact runtime type, value, power-of-two,
cross-field, schema, and package-version checks.

## Checked commands

```bash
tools/autotune/run_smoke.sh
python3 tools/autotune/make_config.py --self-test
python3 tools/autotune/mapping_smoke.py
python3 tools/autotune/mapping_smoke.py --demo build/rtfw_runtime_demo
```

The first command targets the synthetic app. The no-argument mapping smoke
checks generation, schema validation, distinct content-derived identities, and
factor coverage. Supplying `--demo` additionally makes the real C++ profile
round trip mandatory; CI builds the executable and runs this form.

Run one production sample:

```bash
python3 tools/autotune/run_one.py \
  --app build/rtfw_runtime_demo \
  --config configs/default.json \
  --warmup 1 \
  --run 5 \
  --spec tools/autotune/spec.yaml \
  --extra --rt
```

## Result records

Each `run_one.py` invocation emits:

- `ok` and scalar `objective`;
- raw application `metrics`;
- derived `_summary`;
- sampled `_params`, `_seed`, `_scenario`, and `_ts`;
- host `env` metadata;
- `_schema: "v1"`.

The demo payload includes profile/runtime/config identity, direct frame
percentiles and standard deviation, deadline/watchdog/log counters, and
executor statistics. JSONL appends flush and `fsync` each record to reduce
partial-tail risk; that does not authenticate a record or make the experiment
representative.

`install_profile.py` accepts only the exact production factor set. Legacy or
partially mapped results fail instead of being silently converted with default
values.

## Interpretation boundary

A generated profile is a runtime configuration candidate, not a hardware
qualification record. Validate it with the real embedding workload and retain
environment/build provenance before deployment. The standalone demo cannot
attach a `host_adapter`, register application devices, or predict CUDA/XDMA
completion behavior.

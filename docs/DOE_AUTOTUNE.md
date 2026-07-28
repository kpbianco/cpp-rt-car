# DOE and Autotune Status

RTFW 1.2 connects the design-of-experiments pipeline to the supported
`rt::Runtime` path. The default production spec generates complete runtime
profiles and drives `rtfw_runtime_demo`; the separate synthetic target remains
the fast end-to-end tooling test.

This is operational measurement tooling, not automatic optimization of an
embedding application. Results apply only to the tested binary, workload,
host, operating-system state, and measurement procedure. They do not establish
a portable latency limit, RT1 result, RT2 qualification, or CUDA/XDMA
qualification.

## Verified paths

The synthetic CI-sized workflow exercises candidate generation, constraints,
objectives, search, validation, JSONL persistence, analysis, and reporting:

```bash
tools/autotune/run_smoke.sh
```

The production mapping checks exercise the exact profile schema and can drive
the real C++ loader:

```bash
python3 tools/autotune/make_config.py --self-test
python3 tools/autotune/mapping_smoke.py
python3 tools/autotune/mapping_smoke.py --demo build/rtfw_runtime_demo
```

CI builds `rtfw_runtime_demo` before the final command, so a generated profile
must pass the bounded parser, configure/finalize/start the target runtime,
execute its concurrent physics graph, and emit a metrics object with the same
profile identity.

## Production factor space

[`tools/autotune/spec.yaml`](../tools/autotune/spec.yaml) exposes only public,
consumed `RuntimeConfig` fields:

- `worker_count`;
- `executor_policy` (`static_deterministic` or `bounded_throughput`);
- `executor_queue_capacity`.

Every candidate becomes a complete schema-v1 profile with runtime-config
schema 7 and package compatibility `1.1+`. The generator resolves all other
runtime values, retains the sampled factors under `params`, and derives
`profile_id` from the complete resolved payload. Unknown factors fail mapping
coverage instead of appearing as inert optimization knobs.

The removed pre-1.1 factor set—huge pages, scheduler steal thresholds,
emergency helpers, AoSoA layout, prefetch distance, and governor settings—did
not configure the supported target runtime and is not part of the production
spec.

## Measurement path

`run_one.py` launches a separate warm-up process, then a measured process with
`--metrics-json-interval`. In `--rt` mode, `rtfw_runtime_demo` uses the
runtime-owned absolute periodic loop at a 1 ms period. It records
start-to-finish frame samples, deadline misses, watchdog events, trace-event
drops, and executor queue rejections. It reads drops from the runtime metric
schema rather than synthesizing a zero (`log_drops` remains a compatibility
alias in the result record). The output also binds runtime version,
runtime-config schema, effective config identity, profile identity, executor
policy, worker count, and seed.

The default hard constraints reject any deadline miss, watchdog event, or
trace-event drop. A trial can still be unrepresentative because of host
contention, frequency scaling, thermal behavior, virtualisation, interrupts,
or an unrepresentative demo workload.

## Valid use

A deployment-quality experiment should:

1. build a pinned optimized artifact;
2. record the exact source, compiler, runtime profile, host, kernel, power, and
   affinity state;
3. use a workload representative of the embedding application;
4. warm up outside the measurement interval;
5. retain raw frame samples and all rejection/loss counters;
6. repeat across declared seeds, scenarios, and environmental conditions;
7. reject invalid or dropped-data trials;
8. validate finalists independently before installation.

The checked `configs/`, `profiles/example-linux.json`, `results/`, and
`reports/` files are examples or archived fixtures unless their own provenance
states otherwise. They are not recommendations.

Schema, result-record, and command details are in
[`tools/autotune/README.md`](../tools/autotune/README.md), and the loader
contract is in [runtime profiles](runtime_profiles.md).

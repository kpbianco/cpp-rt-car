# Observability Status

The current trace and metrics facilities are useful for development
experiments. They are not yet a versioned, allocation-free RT telemetry
contract.

## Binary trace

`bintrace::Trace` owns fixed-capacity per-thread event rings. Events can be
exported offline as Chrome trace JSON, CSV shaped for ETW-oriented analysis, or
line-oriented text for eBPF-oriented pipelines. The CSV and text exporters are
file formats; they do not register a native ETW provider or load an eBPF
program.

Queue instrumentation records push/pop depth. Emergency helper attempts and GPU
mock fence waits have event codes. Ring overflow increments `trace.dropped`.

The trace currently uses process-global registration and a low-level timestamp
domain. Schema/version metadata and stable clock conversion are milestone M6.

## Metrics JSON

The demo exposes two output modes:

| Mode | Phase histogram | Resettable counters |
| --- | --- | --- |
| `--metrics-json` | Latest samples retained by each rolling histogram; default capacity 120 | Absolute current values |
| `--metrics-json-interval` | Same rolling contents at emission, then cleared | Selected values are emitted relative to their baseline, then rebased |

Neither mode contains lifetime phase percentiles once more than 120 samples
have been recorded. The demo emits once at exit, so “interval” currently means
the final snapshot plus a reset that has no subsequent visible interval.

The registry uses a mutex and constructs strings, maps, vectors, and JSON
streams during update/export. It must not be called an RT-safe hot-path
facility. Milestone M6 introduces pre-registered identifiers, fixed event
records, versioned schemas, and a non-RT export lane.

## Provisional names

- `worker.steals[*]` and `worker.steals_total` count empty global-queue polls
  while work remains; they are not successful steals.
- `worker.queue_max` is a high-water mark.
- `worker.emergency_spawns` counts helpers actually launched.
- `log_drops` is `logger.dropped + trace.dropped`.

These definitions can change before the M6 schema is frozen.

## Code anchors

- Trace rings: `bintrace::Trace`; `include/simcore/bintrace.hpp`
- Export adapters: `tools/trace_export.hpp`
- Metrics registry and rolling histogram: `metrics::Registry`,
  `metrics::RollingHistogram`; `include/simcore/metrics.hpp`
- Demo switches: `src/main.cpp`
- Worker counters: `WorkerPool::stats`; `include/simcore/worker_pool.hpp`

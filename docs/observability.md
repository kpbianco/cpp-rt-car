# Versioned Observability Contract

Release 0.7 completes the M6 RT0 observability surface for `rt::Runtime`.
Emission is bounded and allocation-free on runtime lanes; inspection and
serialization are explicit non-RT host operations. This is a telemetry
contract, not a latency qualification or a native OpenTelemetry, ETW, or eBPF
integration.

The legacy `SimCore` binary trace and mutex-backed metrics registry remain
experimental and do not inherit this contract.

## Schema and provenance

Observability schema version 1 has fixed numeric trace-event and metric IDs.
The version/build/config/workload identifiers make each exported stream
self-describing and correlatable.
Every metadata record, metric snapshot, and trace read reports:

- observability schema version;
- runtime semantic version;
- fixed trace-record and metric-sample sizes;
- metric count and trace capacity;
- build identifier;
- canonical configuration identifier;
- workload identifier;
- process-local runtime identifier.

The build identifier defaults to `rtfw-<version>` and can be set at build time
with `-DRTFW_BUILD_ID=<token>`. Build and workload identifiers are restricted
to 1–63 characters from `A-Za-z0-9._:/@-`; the runtime rejects malformed or
unterminated workload IDs.

`config_id` is a 64-bit FNV-1a fingerprint of configuration schema version 5
and every behavioral `RuntimeConfig` field. It deliberately excludes
`workload_id`, because workload provenance and runtime behavior are separate
dimensions. The identifier is useful for correlation and equality checks; it
is not a cryptographic digest.

## Trace emission

`trace_capacity` commits a global fixed slot count at finalization. Each
attempted event receives a monotonically increasing sequence number. Producers
make one nonblocking slot-claim attempt:

- a free slot is populated and atomically committed;
- reuse of a committed slot increments `trace.events_overwritten`;
- contention or zero configured capacity drops the new event and increments
  `trace.events_dropped`;
- producers never wait, allocate, invoke host code, or fall back to another
  queue.

Payload fields are atomic so a reader cannot observe a C++ data race. Public
records are fixed at 64 bytes and include schema/size, sequence, event type,
status, runtime timestamp, frame, producer, callback/worker indices, and an
event-specific value. Callback events carry registration index, worker index,
and task index. Periodic wake events carry the corresponding absolute release;
watchdog and degradation records carry their configured timeout or applied
level.

| ID | Event name |
| ---: | --- |
| 1 | `runtime.finalized` |
| 2 | `runtime.started` |
| 3 | `periodic.release` |
| 4 | `periodic.wake` |
| 5 | `frame.begin` |
| 6 | `callback.begin` |
| 7 | `callback.end` |
| 8 | `watchdog.fired` |
| 9 | `degradation.applied` |
| 10 | `frame.end` |
| 11 | `runtime.stopped` |

Trace slots are cache-line aligned. The implementation fails compilation on a
target whose 16-, 32-, or 64-bit standard atomics are not always lock-free,
rather than silently routing an RT-lane operation through a library lock.

`RuntimeTraceCursor` and `rtfw_trace_cursor` are caller-owned. A fresh cursor
must be default initialized (or initialized by the matching C function),
starts at the oldest event currently retained, and does not call earlier
history “lost.” If an established cursor falls behind, the next read advances
to the oldest retained sequence and reports the exact skipped sequence count.
Dropped sequence holes encountered inside a batch are also included in
`lost_events`. Cursors are bound to one runtime and are rejected by another.

The older `trace_event_count()` and `trace_event()` methods remain compatibility
views over the latest retained window. Cursor reads are the production
interface.

## Metric schema

Schema version 1 publishes these ordered samples:

| ID | Name | Kind |
| ---: | --- | --- |
| 0 | `runtime.frames_started` | Counter |
| 1 | `runtime.frames_completed` | Counter |
| 2 | `runtime.frames_failed` | Counter |
| 3 | `runtime.callbacks_started` | Counter |
| 4 | `runtime.callbacks_completed` | Counter |
| 5 | `runtime.callback_failures` | Counter |
| 6 | `runtime.deadline_misses` | Counter |
| 7 | `runtime.watchdog_events` | Counter |
| 8 | `runtime.degradation_events` | Counter |
| 9 | `runtime.periodic_releases` | Counter |
| 10 | `runtime.periodic_wakes` | Counter |
| 11 | `trace.events_emitted` | Counter |
| 12 | `trace.events_overwritten` | Counter |
| 13 | `trace.events_dropped` | Counter |
| 14 | `executor.submitted_tasks` | Counter |
| 15 | `executor.local_executions` | Counter |
| 16 | `executor.steal_attempts` | Counter |
| 17 | `executor.successful_steals` | Counter |
| 18 | `executor.queue_rejections` | Counter |
| 19 | `executor.scratch_exhaustions` | Counter |
| 20 | `executor.worker_starts` | Counter |
| 21 | `runtime.degradation_level` | Gauge |

Counter values are monotonic from finalization. A completed frame includes a
failed frame; `frames_failed` identifies the failed subset. Callback and
executor counts describe accepted/executed runtime work, not application-level
entities.

## Window semantics

`cumulative` snapshots report current values since finalization and do not
require or mutate a cursor.

`interval` snapshots require a caller-owned metric cursor:

1. A default-initialized fresh cursor covers finalization through the first
   snapshot.
2. Each later snapshot starts at the previous successful window end.
3. Counters are current minus that cursor's prior values.
4. Gauges are sampled at the window end and are never differenced.
5. A failed snapshot does not advance the cursor.

Therefore, adjacent intervals from one cursor partition each cumulative
counter. Multiple exporters do not reset global state or affect each other.
Metric cursors are runtime-bound and cross-instance use is rejected.

## Host export APIs

C++:

- `Runtime::observability_metadata()`;
- `Runtime::metrics_snapshot()`;
- `Runtime::read_trace()`;
- `write_observability_json()` in
  `<rt/observability_export.hpp>`.

C ABI v5:

- `rtfw_get_observability_metadata()`;
- `rtfw_get_metrics()`;
- `rtfw_read_trace()`;
- matching cursor/result initialization functions.

The JSON helper gathers a complete bounded snapshot before writing, may
allocate staging memory, and emits schema/version/build/config/workload/runtime
identifiers, fixed record/sample sizes and capacities, metric definitions and
values, and retained trace records. Interval JSON export requires a persistent
metric cursor. It is not an RT callback facility.

All inspection/export calls reject an active step or periodic loop. Control
operations and export remain single-host-thread operations. The runtime does
not create a background exporter, perform file or network I/O, or invoke a
host sink.

## Evidence

- fixed trace schema, loss accounting, metric windows, provenance, runtime
  isolation, JSON, and contended nonblocking emission:
  `tests/test_observability.cpp`;
- C ABI v5 symbol, structure, cursor, metric, and trace coverage:
  `tests/test_cabi_dlopen.c`;
- complete post-start CPU frames with tracing and counters under allocation
  instrumentation: `tests/test_trace_noalloc.cpp`;
- ThreadSanitizer coverage includes `Observability.*`;
- implementation: `rt/src/telemetry.cpp`,
  `rt/src/host_runtime.cpp`, and
  `rt/src/observability_export.cpp`.

## Explicit boundaries

- Schema version 1 applies only to the target `rt::Runtime` path.
- The legacy per-thread binary trace, rolling histograms, and demo JSON retain
  their existing experimental semantics.
- A trace capacity can overwrite unread events; the cursor reports loss rather
  than applying backpressure to runtime lanes.
- Export can allocate, block in the destination stream, or fail with
  `resource_exhausted`; it belongs on a non-RT host lane.
- Observability correctness does not establish RT1/RT2 timing behavior.

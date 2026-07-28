# Host Runtime Lifecycle

`rt::Runtime` is the target-path embedding surface introduced in M1 and
extended with the M2 compiled graph, M3 unified executor, M4 finalized memory
plan, M5 time/platform controls, M6 versioned observability, M7
determinism/replay, the M8 bounded device ABI and deterministic mock, and the
M11 stable C ABI plus host job-system adapter, M12 portable distribution, and
the M13 complete runtime-profile loader. It
provides explicit host-driven and finite self-paced operation without adopting
the legacy `SimCore` scheduler or pacing loop.

The surface is RT0 functional behavior, not qualified latency.
`query_capabilities()` and `rt_query_capabilities()` report host-driven time,
compiled-graph validation, the unified CPU executor, the target-path bounded
memory plan, self-paced time, the frame watchdog, and strict platform preflight
as available. They also report versioned observability and deterministic replay
as available; the latter means the implemented D0/D1 surface, not D2 or D3.
They report `host_executor_adapter` for the borrowed engine job-system policy
and `bounded_device_backend` for the poll-only M8 runtime path. Those
capabilities do not imply engine, CUDA, Vulkan, or XDMA qualification. The
optional M9 CUDA and M10 XDMA candidates are separately linked backends with
independent support matrices and evidence gates.

## Lifecycle

| Current state | Allowed operation | Resulting state |
| --- | --- | --- |
| `configuring` | typed configuration, optional host-executor attachment, graph/state/device registration and declarations | `configuring` |
| `configuring` | `finalize()` | `finalized` |
| `finalized` | `start()`, preflight, and create configured runtime-owned lanes or bind the already-running host team | `running` |
| `running` | `step(frame)` or synchronous `replay(checkpoint, input_log)` | `running` |
| `running` | `run_periodic(config)` | `running` |
| `finalized` or `running` | `stop()` | `stopped` |
| `stopped` | repeated `stop()` | `stopped` |

Other transitions return `rt::Status::invalid_state`. A stopped runtime is
terminal. Destruction requires that no host call or callback is active; hosts
should call `stop()` so their own resource lifecycle is explicit.
Strict preflight failure leaves the runtime `finalized` without creating a
runtime thread, so the host can inspect the report or retry after external
setup.

Control operations are single-host-thread operations in 1.2. `step()`,
`run_periodic()`, checkpoint/restore, input-log export, and replay are
non-reentrant with execution. A periodic observer cannot recursively step, and
`stop()` called from inside a callback, periodic loop, or replay is rejected.

## Typed configuration

`rt::RuntimeConfig` has twenty-five schema keys:

| Key | Type/default | Runtime behavior |
| --- | --- | --- |
| `callback_capacity` | positive integer, `64` | Maximum registrations accepted before finalization |
| `scratch_bytes` | nonnegative integer, `65536` | Bytes of distinct phase-local scratch exposed to each callback |
| `trace_capacity` | nonnegative integer, `1024` | Capacity of the instance-local lifecycle/callback trace ring; zero disables it |
| `numerical_mode` | `precise` | Selects `precise` or `fused_multiply_add` behavior for the callback numerical helper |
| `executor_policy` | `static_deterministic` | Selects `static_deterministic`, `bounded_throughput`, or `host_adapter` |
| `worker_count` | positive integer, `1` | Fixed runtime workers or exact borrowed host-worker count; accepted range is 1–256 |
| `executor_queue_capacity` | power-of-two integer, `1024` | Fixed capacity of each runtime-local queue, or total accepted-job reservation for `host_adapter`; accepted range is 2–1,048,576 |
| `scratch_alignment` | power-of-two integer, `64` | Alignment shared by phase/task scratch; accepted range is `alignof(max_align_t)`–4096 |
| `task_scratch_bytes` | nonnegative integer, `4096` | Callback-local bytes owned by each accepted graph/range/reduction context; accepted maximum is 1,048,576 |
| `task_scratch_slots` | positive integer, `1024` | Fixed simultaneous task-context reservations; accepted maximum is 1,048,576 |
| `memory_budget_bytes` | positive integer, `268435456` | Maximum reported target-runtime plan accepted by finalization; ceiling is 1 TiB on 64-bit hosts and addressable `size_t` on 32-bit hosts |
| `overload_policy` | `reject_submission` | Selects `reject_submission` or `fail_frame` for queue/scratch rejection |
| `watchdog_timeout_ns` | nonnegative integer, `0` | Nanoseconds from measured step start to watchdog expiry; zero disables the watchdog and the maximum is 24 hours |
| `watchdog_max_degradation_level` | nonnegative integer, `0` | Caps frame-thread degradation increments; accepted range is 0–255 |
| `platform_preflight_mode` | `disabled` | Selects `disabled` or fail-closed, read-only `strict` prerequisite checks |
| `workload_id` | identifier, `unspecified` | Labels observability and replay artifacts with 1–63 characters from `A-Za-z0-9._:/@-`; it affects configuration and replay identity |
| `determinism_tier` | `d0_unspecified` | Selects D0 unspecified behavior or D1 schedule-independent replay; D2 and D3 are rejected in 1.2 |
| `state_capacity` | nonnegative integer, `64` | Maximum canonical state regions accepted before finalization; zero disables registration |
| `snapshot_max_bytes` | positive integer, `1048576` | Per-runtime upper bound for encoded checkpoint bytes |
| `replay_input_capacity` | nonnegative integer, `4096` | Maximum records accepted in one encoded or replayed input log |
| `input_log_max_bytes` | positive integer, `1048576` | Per-runtime upper bound for encoded input-log bytes |
| `device_backend_capacity` | positive integer, `1` | Maximum backend tables accepted before finalization; accepted range is 1–256 |
| `device_buffer_capacity` | nonnegative integer, `64` | Maximum borrowed device buffers accepted before finalization; accepted maximum is 65,536 |
| `device_outstanding_capacity` | positive integer, `64` | Runtime-wide preallocated outstanding submission slots; every backend must report at least this capacity |
| `device_completion_batch` | positive integer, `16` | Preallocated completion records supplied to each bounded poll; it cannot exceed outstanding capacity |

The typed structure can be supplied with `configure()`. Dynamic callers can use
`configure_key()` or the C `rtfw_config_set()` equivalent. Unknown keys,
partially parsed values, invalid enum values, and out-of-range capacities fail;
there is no ignored-key path.

`host_adapter` additionally requires `Runtime::set_host_executor()` or
`rtfw_set_host_executor()` before finalization. Its copied callback table must
declare capacities exactly equal to the typed configuration. The detailed
submission, scratch, completion, helping, and ownership rules are in the
[executor contract](executor.md).

`<rt/profile.hpp>` maps the complete schema into `RuntimeConfig` through a
bounded, allocation-free, transactional JSON parser. It rejects missing,
unknown, duplicate, incompatible, and invalid contract fields before
`Runtime::configure()`. Profile file I/O remains a non-RT host responsibility.
See [runtime profiles](runtime_profiles.md).

## Host-driven steps

The host supplies:

- a frame index;
- a simulation delta;
- an optional absolute deadline in that runtime clock's nanosecond domain.

`Runtime::step()` submits dependency-ready callbacks to the already-started
executor and waits for the graph to quiesce. It never sleeps, advances no
hidden frame counter, or creates a thread. The canonical registration-index
topological order remains available for introspection, but independent
callback completion is not a total order. `StepResult` reports callback count,
start/finish timestamps, deadline miss, watchdog event, and committed
degradation level.

A device command provider is scheduled like a CPU callback but does not finish
its graph phase when it returns. An accepted submission retains the phase
token until the runtime-owned service lane polls its completion. Independent
CPU phases remain runnable; dependent phases are released only by completion.
Submission saturation or a device failure becomes the typed step result. See
the [device backend contract](device_backend.md).

Use `Runtime::now_ns()` or `rtfw_now_ns()` to form a deadline in the correct
clock domain. The default clock epoch is local to the runtime instance. C++ tests
can inject a `RuntimeClock`, including a fake clock.

If a callback returns an error or throws through the C++ surface, the step
returns `callback_failed`. Dependent and not-yet-invoked work is skipped;
already-running independent callbacks may finish. The runtime remains running
so the host can inspect the error and decide whether to retry or stop. A C
callback must not throw across the language boundary.

## Canonical state and replay

State registration is allowed only while configuring. Each registration
supplies a unique stable name, a nonzero schema version, and one non-empty,
non-overlapping caller-owned byte region. Its address and size are frozen by
finalization and must remain valid until runtime destruction. Those bytes must
already be a canonical representation; native object layout, padding,
pointers, and platform-endian fields are not portable state formats.

Checkpoint and input-log APIs are non-RT control operations. Callers provide
the output storage, and short buffers report the exact required size without a
partial artifact. Restore fully validates format, bounds, checksums, runtime
identity, registered schema, and every destination size before changing any
registered byte. Replay validates both artifacts before restore, applies each
input callback immediately before its frame, and invokes the ordinary
synchronous `step()` path. See the
[determinism/replay contract](determinism_replay.md).

## Self-paced frames

`Runtime::run_periodic()` runs a finite frame count on the calling frame
thread. It waits for `first_release + i * period`, never shifts the release
epoch after a late frame, and supplies `release + relative_deadline` to each
step. Results expose release, wake, start, finish, signed slack, miss,
watchdog, and degradation fields. The C equivalent is `rtfw_run_periodic()`.

The injected clock must implement absolute waiting; otherwise the call returns
`clock_failure` without inventing a fallback cadence. Complete arithmetic is
validated before the first frame. The exact late-frame, observer, watchdog,
and overflow semantics are in the
[time/platform contract](time_platform.md).

## Watchdog, degradation, and preflight

A nonzero watchdog timeout creates one service lane during `start()`. One arm
can produce at most one event. The service never invokes host code or mutates
degradation; the frame thread consumes the event after graph quiescence and
increments the capped level for following frames. The watchdog cannot abort a
stuck callback.

Strict platform preflight is disabled by default. When enabled, it inspects all
six Linux prerequisites before any runtime thread starts and fails closed with
`platform_preflight_failed`. A complete report remains inspectable while the
runtime stays finalized. The native probe reads host state but does not change
memory locking, affinity, scheduling, limits, or system policy. Passing these
checks is not RT2 qualification.

## Ownership and isolation

Each runtime owns:

- its callback registry and copied callback names;
- its logical resources, dependency declarations, and compiled phase order;
- one fixed scratch block per registered phase;
- a fixed pool of aligned task-scratch blocks reserved per accepted work item;
- its trace ring and write cursor;
- its numerical-helper policy;
- its clock object or explicitly borrowed C++ clock;
- its watchdog state and degradation level;
- its fixed-capacity platform-preflight report;
- its copied canonical-state registry metadata and replay identities.
- its copied backend tables, device registration metadata, outstanding slots,
  completion batch, service lane, and device telemetry counters.

Callback user data remains host-owned and must outlive every step that can use
it. A phase's scratch contents may persist across frames, but no phase sees
another phase's block. Cross-phase state belongs in host-owned memory covered
by resource declarations. Callers must not treat scratch as durable
application state.
Registered canonical state storage also remains host-owned and must remain
stable for the runtime lifetime. Checkpoint and input-log buffers are
caller-owned and need exist only for the duration of the relevant call.
Backend instances, registered device-buffer bytes, and device command
`user_data` are borrowed through backend shutdown. The service lane is joined
before buffers are unregistered and backend shutdown returns.

The new surface does not modify the legacy process-global `HighResClock`,
`bintrace` registration, or `rt::set_use_fma` flag used by `SimCore`. This is
what allows multiple `rt::Runtime` instances to remain isolated. Arbitrary
floating-point expressions inside user callbacks are still outside the
numerical helper's control.

## C ABI

Stable C ABI v8 mirrors the lifecycle:

- `rtfw_config_init` / `rtfw_config_set`;
- `rtfw_create`;
- `rtfw_register_phase` / `rtfw_register_callback`;
- `rtfw_register_resource`;
- `rtfw_add_dependency` / `rtfw_declare_resource_access`;
- `rtfw_parallel_for` / `rtfw_parallel_reduce` from a callback-local task
  context;
- `rtfw_task_scratch` for callback-local task storage;
- `rtfw_finalize`;
- `rtfw_get_memory_plan`;
- `rtfw_start`;
- `rtfw_step`;
- `rtfw_run_periodic`;
- `rtfw_get_platform_preflight_report`;
- `rtfw_get_degradation_level`;
- `rtfw_get_observability_metadata`;
- `rtfw_get_metrics`;
- `rtfw_read_trace`;
- `rtfw_register_state`;
- `rtfw_checkpoint_size` / `rtfw_checkpoint_write` /
  `rtfw_checkpoint_inspect` / `rtfw_checkpoint_restore`;
- `rtfw_input_log_write` / `rtfw_input_log_inspect`;
- `rtfw_replay` / `rtfw_registered_state_hash`;
- `rtfw_register_device_backend`;
- `rtfw_register_device_buffer`;
- `rtfw_register_device_phase`;
- `rtfw_get_device_health` / `rtfw_reset_device`;
- `rtfw_get_abi_info` / `rtfw_check_abi`;
- `rtfw_set_host_executor`;
- `rtfw_stop`;
- `rtfw_destroy`.

Public configuration, frame, callback, result, and memory-plan structures carry
sizes, and configuration carries stable `RTFW_C_ABI_VERSION` 8 in release
1.2. Periodic, preflight, observability, checkpoint, input-log,
replay, and device structures follow the same initialized-output rule. Call
the supplied structure initializers and leave reserved fields zero.
`rtfw_status_message()` provides status text even when no runtime handle was
created; `rtfw_last_error()` adds handle-specific context. Export, compatibility,
ownership, SONAME, and change-policy details are in the
[stable C ABI contract](c_abi.md).

## Compiled graph

Graph construction, resource-hazard rules, deterministic tie-breaking,
diagnostics, and allocation boundaries are specified in the
[compiled graph contract](compiled_graph.md).

Finalization rejects cycles and unordered read/write or write/write access to a
shared logical resource. Once finalization succeeds, every topology mutation
is frozen. Independent phases may run concurrently, which is why resource
conflicts must be ordered explicitly instead of relying on an incidental
traversal.

## Current boundary

M4 finalizes aligned phase/task scratch, queue/control, and trace storage under
a configured memory budget. The target CPU frame path has a multi-frame
zero-allocation gate and explicit queue/scratch overload behavior. Plan scope
and exclusions are specified in the
[memory-plan contract](memory_plan.md); policy, queue, and nesting details are
in the [executor contract](executor.md). M5 adds absolute cadence, one-shot
watchdog/degradation, and fail-closed prerequisite reporting. It does not
preempt callbacks or qualify a deployment; see the
[time/platform contract](time_platform.md). M6 adds bounded versioned
trace/counter emission and non-RT cursor/export APIs; see the
[observability contract](observability.md). M7 adds bounded canonical-state
checkpoint and replay control operations; see the
[determinism/replay contract](determinism_replay.md). M8 adds bounded
submission/poll, graph-held completions, health/reset/shutdown, and a
fault-injectable mock; see the
[device backend contract](device_backend.md).

## Code and evidence

- C++ API: `rt/include/rt/runtime.hpp`
- Implementation: `rt/src/host_runtime.cpp`
- Graph compiler: `rt/src/compiled_graph.cpp`
- Unified executor: `rt/src/executor.cpp`
- C ABI: `rt/include/rt/c_api.h`, `src/c_abi.cpp`
- C++ lifecycle tests: `tests/test_host_runtime.cpp`
- C++ graph tests: `tests/test_compiled_graph.cpp`
- Executor tests: `tests/test_executor.cpp`
- Memory-plan tests: `tests/test_memory_plan.cpp`
- Time/watchdog tests: `tests/test_periodic_runtime.cpp`
- Platform-preflight tests: `tests/test_platform_preflight.cpp`
- Observability tests: `tests/test_observability.cpp`
- Determinism/replay tests: `tests/test_determinism_replay.cpp`
- Device ABI/runtime tests: `tests/test_device_runtime.cpp`
- Artifact parser fuzz target: `tests/snapshot_fuzz.cpp`
- Dynamic C ABI test: `tests/test_cabi_dlopen.c`
- C sample: `samples/embed_c/mini_app.c`
- C++ sample: `samples/embed_cpp/mini_app.cpp`
- Device sample: `samples/device_mock.cpp`

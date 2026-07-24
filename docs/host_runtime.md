# Host Runtime Lifecycle

`rt::Runtime` is the target-path embedding surface introduced in M1 and
extended with the M2 compiled graph, M3 unified executor, and M4 finalized
memory plan. It provides an explicit, host-driven lifecycle without adopting
the legacy `SimCore` scheduler or pacing loop.

The surface is RT0 functional behavior, not qualified latency.
`query_capabilities()` and `rt_query_capabilities()` report host-driven time,
compiled-graph validation, the unified CPU executor, and the target-path
bounded memory plan as available.

## Lifecycle

| Current state | Allowed operation | Resulting state |
| --- | --- | --- |
| `configuring` | typed configuration, graph registration and declarations | `configuring` |
| `configuring` | `finalize()` | `finalized` |
| `finalized` | `start()` and create the fixed worker team | `running` |
| `running` | `step(frame)` | `running` |
| `finalized` or `running` | `stop()` | `stopped` |
| `stopped` | repeated `stop()` | `stopped` |

Other transitions return `rt::Status::invalid_state`. A stopped runtime is
terminal. Destruction requires that no host call or callback is active; hosts
should call `stop()` so their own resource lifecycle is explicit.

Control operations are single-host-thread operations in 0.5. `step()` is
non-reentrant, and `stop()` called from inside a callback is rejected.

## Typed configuration

`rt::RuntimeConfig` has twelve schema keys:

| Key | Type/default | Runtime behavior |
| --- | --- | --- |
| `callback_capacity` | positive integer, `64` | Maximum registrations accepted before finalization |
| `scratch_bytes` | nonnegative integer, `65536` | Bytes of distinct phase-local scratch exposed to each callback |
| `trace_capacity` | nonnegative integer, `1024` | Capacity of the instance-local lifecycle/callback trace ring; zero disables it |
| `numerical_mode` | `precise` | Selects `precise` or `fused_multiply_add` behavior for the callback numerical helper |
| `executor_policy` | `static_deterministic` | Selects `static_deterministic` or `bounded_throughput` |
| `worker_count` | positive integer, `1` | Fixed workers created by `start()`; accepted range is 1–256 |
| `executor_queue_capacity` | power-of-two integer, `1024` | Fixed capacity of each worker-local queue; accepted range is 2–1,048,576 |
| `scratch_alignment` | power-of-two integer, `64` | Alignment shared by phase/task scratch; accepted range is `alignof(max_align_t)`–4096 |
| `task_scratch_bytes` | nonnegative integer, `4096` | Callback-local bytes owned by each accepted graph/range/reduction context; accepted maximum is 1,048,576 |
| `task_scratch_slots` | positive integer, `1024` | Fixed simultaneous task-context reservations; accepted maximum is 1,048,576 |
| `memory_budget_bytes` | positive integer, `268435456` | Maximum reported target-runtime plan accepted by finalization; ceiling is 1 TiB on 64-bit hosts and addressable `size_t` on 32-bit hosts |
| `overload_policy` | `reject_submission` | Selects `reject_submission` or `fail_frame` for queue/scratch rejection |

The typed structure can be supplied with `configure()`. Dynamic callers can use
`configure_key()` or the C `rtfw_config_set()` equivalent. Unknown keys,
partially parsed values, invalid enum values, and out-of-range capacities fail;
there is no ignored-key path.

This schema is distinct from the unattached autotune JSON format. Loading those
profiles into the demo remains planned.

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
start/finish timestamps, and whether the optional deadline was missed.

Use `Runtime::now_ns()` or `rtfw_now_ns()` to form a deadline in the correct
clock domain. The default clock epoch is local to the runtime instance. C++ tests
can inject a `RuntimeClock`, including a fake clock.

If a callback returns an error or throws through the C++ surface, the step
returns `callback_failed`. Dependent and not-yet-invoked work is skipped;
already-running independent callbacks may finish. The runtime remains running
so the host can inspect the error and decide whether to retry or stop. A C
callback must not throw across the language boundary.

## Ownership and isolation

Each runtime owns:

- its callback registry and copied callback names;
- its logical resources, dependency declarations, and compiled phase order;
- one fixed scratch block per registered phase;
- a fixed pool of aligned task-scratch blocks reserved per accepted work item;
- its trace ring and write cursor;
- its numerical-helper policy;
- its clock object or explicitly borrowed C++ clock.

Callback user data remains host-owned and must outlive every step that can use
it. A phase's scratch contents may persist across frames, but no phase sees
another phase's block. Cross-phase state belongs in host-owned memory covered
by resource declarations. Callers must not treat scratch as durable
application state.

The new surface does not modify the legacy process-global `HighResClock`,
`bintrace` registration, or `rt::set_use_fma` flag used by `SimCore`. This is
what allows multiple `rt::Runtime` instances to remain isolated. Arbitrary
floating-point expressions inside user callbacks are still outside the
numerical helper's control.

## C ABI

The experimental C ABI mirrors the lifecycle:

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
- `rtfw_stop`;
- `rtfw_destroy`.

Public configuration, frame, callback, result, and memory-plan structures carry
sizes, and configuration carries `RTFW_C_ABI_VERSION` (version 3 in release
0.5). Call the supplied structure initializers and leave reserved fields zero.
`rtfw_status_message()` provides status text even when no runtime handle was
created; `rtfw_last_error()` adds handle-specific context. The ABI remains
unfrozen before M11; incompatible pre-1.0 changes require a repository version
increment.

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
in the [executor contract](executor.md). Self-paced time and platform preflight
remain M5.

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
- Dynamic C ABI test: `tests/test_cabi_dlopen.c`
- C sample: `samples/embed_c/mini_app.c`
- C++ sample: `samples/embed_cpp/mini_app.cpp`

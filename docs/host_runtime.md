# Host Runtime Lifecycle

`rt::Runtime` is the target-path embedding surface introduced in M1 and
extended with the M2 compiled graph. It provides an explicit, host-driven
lifecycle without adopting the legacy `SimCore` scheduler or pacing loop.

The surface is RT0 functional behavior. It does not yet claim a parallel
executor, complete zero-allocation callback lane, or qualified latency.
`query_capabilities()` and `rt_query_capabilities()` report that boundary
directly: host-driven time and compiled-graph validation are available; the
bounded-memory-plan capability remains false until M4.

## Lifecycle

| Current state | Allowed operation | Resulting state |
| --- | --- | --- |
| `configuring` | typed configuration, graph registration and declarations | `configuring` |
| `configuring` | `finalize()` | `finalized` |
| `finalized` | `start()` | `running` |
| `running` | `step(frame)` | `running` |
| `finalized` or `running` | `stop()` | `stopped` |
| `stopped` | repeated `stop()` | `stopped` |

Other transitions return `rt::Status::invalid_state`. A stopped runtime is
terminal. Destruction is always safe, but hosts should call `stop()` so their
own resource lifecycle is explicit.

Control operations are single-host-thread operations in 0.3. `step()` is
non-reentrant, and `stop()` called from inside a callback is rejected.

## Typed configuration

`rt::RuntimeConfig` has four schema keys:

| Key | Type/default | Runtime behavior |
| --- | --- | --- |
| `callback_capacity` | positive integer, `64` | Maximum registrations accepted before finalization |
| `scratch_bytes` | nonnegative integer, `65536` | Bytes of instance-local scratch exposed to each callback |
| `trace_capacity` | nonnegative integer, `1024` | Capacity of the instance-local lifecycle/callback trace ring; zero disables it |
| `numerical_mode` | `precise` | Selects `precise` or `fused_multiply_add` behavior for the callback numerical helper |

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

`Runtime::step()` calls registered callbacks synchronously in compiled
topological order. Ready-phase ties use registration order. It never sleeps,
advances no hidden frame counter, and creates no threads. `StepResult` reports
callback count, start/finish timestamps, and whether the optional deadline was
missed.

Use `Runtime::now_ns()` or `rtfw_now_ns()` to form a deadline in the correct
clock domain. The default clock epoch is local to the runtime instance. C++ tests
can inject a `RuntimeClock`, including a fake clock.

If a callback returns an error or throws through the C++ surface, the step stops
before later callbacks and returns `callback_failed`. The runtime remains
running so the host can inspect the error and decide whether to retry or stop.
A C callback must not throw across the language boundary.

## Ownership and isolation

Each runtime owns:

- its callback registry and copied callback names;
- its logical resources, dependency declarations, and compiled phase order;
- its fixed scratch block;
- its trace ring and write cursor;
- its numerical-helper policy;
- its clock object or explicitly borrowed C++ clock.

Callback user data remains host-owned and must outlive every step that can use
it. Scratch contents are runtime-local but may persist between callbacks and
frames; callers must not use scratch as persistent application state.

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
- `rtfw_finalize`;
- `rtfw_start`;
- `rtfw_step`;
- `rtfw_stop`;
- `rtfw_destroy`.

Public configuration, frame, callback, and result structures carry sizes, and
configuration carries `RTFW_C_ABI_VERSION`. Call the supplied structure
initializers and leave reserved fields zero. `rtfw_status_message()` provides
status text even when no runtime handle was created; `rtfw_last_error()` adds
handle-specific context. The ABI remains unfrozen before M11; incompatible
pre-1.0 changes require a repository version increment.

## Compiled graph

Graph construction, resource-hazard rules, deterministic tie-breaking,
diagnostics, and allocation boundaries are specified in the
[compiled graph contract](compiled_graph.md).

Finalization rejects cycles and unordered read/write or write/write access to a
shared logical resource. Once finalization succeeds, every topology mutation is
frozen. The current executor remains synchronous; M3 may run independent
phases concurrently, which is why resource conflicts must be ordered explicitly
instead of relying on the current serial traversal.

## Current boundary

M2 compiles phase/resource dependencies before start. M3 will place independent
compiled work behind one bounded executor. M4 will replace the simple shared
scratch block with the complete allocation plan and prove the full RT-lane
allocation gate.

## Code and evidence

- C++ API: `rt/include/rt/runtime.hpp`
- Implementation: `rt/src/host_runtime.cpp`
- Graph compiler: `rt/src/compiled_graph.cpp`
- C ABI: `rt/include/rt/c_api.h`, `src/c_abi.cpp`
- C++ lifecycle tests: `tests/test_host_runtime.cpp`
- C++ graph tests: `tests/test_compiled_graph.cpp`
- Dynamic C ABI test: `tests/test_cabi_dlopen.c`
- C sample: `samples/embed_c/mini_app.c`
- C++ sample: `samples/embed_cpp/mini_app.cpp`

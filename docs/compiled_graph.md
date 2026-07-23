# Compiled Graph Contract

Release 0.3 added the M2 compiled graph to the target-path `rt::Runtime`.
The graph is an RT0 functional surface: it validates dependency and logical
resource ordering before start, supplies an immutable deterministic phase order
for introspection, and provides dependency tables to the M3 executor.

Release 0.4 adds parallel execution without changing the M2 validation rules.
A complete bounded memory plan and latency qualification remain M4 and
deployment-specific work.

## Graph vocabulary

Each registered callback is one **phase**. Registration can return a
`rt::PhaseHandle`, which is used to add directed dependencies and resource
access declarations. `rt::ResourceHandle` identifies a logical application
resource; the runtime does not own or inspect the application memory described
by it. `write` means the phase may mutate the resource and may also read it;
there is no separate read/write declaration.

Handles carry a runtime-owner token, a phase/resource kind tag, and an object
index. They remain stable for the lifetime of that runtime. Default, forged,
cross-kind, out-of-range, and foreign-runtime handles return
`rt::Status::invalid_handle`.

The C ABI exposes the same encoded values as `rtfw_phase_id` and
`rtfw_resource_id`. Handle encodings are opaque and process-local; they are not
snapshot, replay, or interchange identifiers.

## Construction and finalization

While the runtime is `configuring`, a host may:

1. register callbacks and receive phase handles;
2. register uniquely named logical resources;
3. add a directed `prerequisite -> dependent` edge;
4. declare one `read` or `write` access per phase/resource pair.

Duplicate names, edges, and phase/resource declarations are rejected. A
self-dependency is rejected immediately. Longer cycles are diagnosed by
`finalize()`.

The 0.3 safety ceiling is 65,536 registered resources; callback phases are
bounded by `callback_capacity`, whose accepted maximum is also 65,536.
Dependency and access-declaration storage can still grow during configuration,
so this is not the M4 bounded memory plan.

Finalization is the only graph compile point. It:

1. validates every stored handle and access mode;
2. computes a topological order with registration index as the ready-phase
   tie-break;
3. rejects a cycle;
4. validates every conflicting resource-access pair;
5. commits the compiled order only after all validation and finalization-time
   allocations succeed.

A failed finalization leaves the runtime in `configuring`. A host can add
missing dependency paths and retry. There is no removal API in 0.3, so a host
that submitted a longer cycle must recreate its runtime.

After successful finalization, callback/resource registration, dependency
addition, and access declaration all return `invalid_state`. The compiled
order remains available through `compiled_phase_at()` in finalized, running,
and stopped states.

## Resource ordering

Resource declarations prevent an incidental traversal from becoming an unsafe
parallel graph. For every pair of phases that names the same resource:

| First access | Second access | Required ordering |
| --- | --- | --- |
| Read | Read | None |
| Read | Write | A dependency path in either direction |
| Write | Read | A dependency path in either direction |
| Write | Write | A dependency path in either direction |

The path may be direct or transitive. A deterministic topological tie-break is
not an ordering declaration: if neither phase reaches the other, `finalize()`
returns `resource_conflict` and identifies the resource and both phases.

This is a validation contract, not automatic synchronization. Hosts must
declare every logical resource that can race, and callbacks must honor those
declarations when touching application memory.

## Execution and allocation boundary

`step()` submits dependency-ready phases to the M3 worker team and waits
synchronously from the host. Trace callback indices remain phase registration
indices. `compiled_phase_at()` still returns the registration-index
tie-broken canonical topological order, but independent callbacks may overlap
and their completion order is not that canonical total order.

Graph compilation, adjacency construction, cycle detection, and resource-path
analysis may allocate and may have data-dependent cost during `finalize()`.
The committed step path does not build, sort, or mutate topology. An allocation
instrumentation test covers the first frame of a representative compiled graph
and observes no heap allocation.

That test closes only the M2 topology gate. Arbitrary callbacks can allocate,
and the complete scratch, overload, and RT-lane allocation proof remains M4.
`bounded_memory_plan` therefore remains false. Executor semantics are specified
in the [M3 executor contract](executor.md).

## C and C++ entry points

The preferred C++ graph calls are:

- `Runtime::register_callback(registration, out_phase)`;
- `Runtime::register_resource(name, out_resource)`;
- `Runtime::add_dependency(prerequisite, dependent)`;
- `Runtime::declare_resource_access(phase, resource, access)`;
- `Runtime::finalize()`.

The C equivalents are:

- `rtfw_register_phase`;
- `rtfw_register_resource`;
- `rtfw_add_dependency`;
- `rtfw_declare_resource_access`;
- `rtfw_finalize`.

The M1 `register_callback` / `rtfw_register_callback` forms remain available
for independent phases that do not need a returned handle. The experimental C
ABI remains unfrozen until M11.

## Evidence

- Compiler: `rt/src/compiled_graph.cpp`
- Runtime integration: `rt/src/host_runtime.cpp`
- C++ graph tests: `tests/test_compiled_graph.cpp`
- First-frame allocation instrumentation: `tests/test_trace_noalloc.cpp`
- Dynamic C ABI coverage: `tests/test_cabi_dlopen.c`
- C and C++ embedding samples: `samples/embed_c/mini_app.c`,
  `samples/embed_cpp/mini_app.cpp`

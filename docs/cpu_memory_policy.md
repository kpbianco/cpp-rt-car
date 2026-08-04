# CPU and Memory Policy Model

M15-01 adds the additive C++ policy and report model used by later M15 startup
transactions. It is portable RT0 modeling and validation only. It does not set
affinity or scheduling, create custom stacks, lock or pin pages, select huge
pages, bind NUMA memory, or call another native policy-mutation API.

`Runtime::set_cpu_memory_policy()` copies one bounded `CpuMemoryPolicy` while
the runtime is configuring. `Runtime::finalize()` validates the complete model,
builds the ordinary `MemoryPlan`, and transactionally resolves an immutable
`CpuMemoryPolicyReport`. A failed finalization publishes no partial report and
leaves the runtime configuring so the host can replace the policy and retry.
The report remains available through running and stopped states.

The model is deliberately separate from runtime-config schema 7 and its 25
JSON/profile keys. It is an additive C++ source API in the already installed
`<rt/config.hpp>` and `<rt/runtime.hpp>` headers. Stable C ABI v8 and device ABI
v1 are unchanged.

## Bounded requests

A policy contains at most 16 thread-role requests and 16 memory-region
requests. CPU sets contain at most 256 explicit CPU identifiers and thread
names contain at most 31 characters plus a terminating NUL. Names use
`A-Za-z0-9._-`.

Thread policy covers:

- CPU set;
- inherited, normal, FIFO, or round-robin scheduling class and priority;
- inherited or explicit NUMA node;
- inherited, spin, yield, or park wait strategy;
- stack and guard byte requests;
- bounded thread name; and
- best-effort or strict requirement.

Memory policy covers:

- runtime, host, backend, borrowed, or inherited provider ownership;
- alignment, base-page rounding, and before/after guards;
- prefault, locking, pinning, and residency verification;
- huge-page preference and explicit fallback;
- NUMA placement and first-touch ownership; and
- rollback intent.

## Thread role inventory

Role identifiers and accounting keys are stable numeric identities. Reports
carry a role-level accounting key; they do not use a process thread ID,
pointer, or global counter.

| Role | Stable name | Portable cardinality and ownership |
| --- | --- | --- |
| frame | `thread.frame` | One caller-owned logical lane; external and verify-only |
| executor worker | `thread.executor-worker` | Exactly `worker_count`; runtime-owned for native policies, host-owned and verify-only for `host_adapter` |
| watchdog | `thread.watchdog` | Zero or one runtime-owned lane from `watchdog_timeout_ns` |
| device service | `thread.device-service` | Zero or one runtime-owned lane for the complete runtime, not one per backend |
| XDMA I/O | `thread.xdma-io` | Backend-owned and verify-only; device ABI v1 does not disclose physical worker cardinality, so `cardinality_known` is false |
| custom accelerator/service | `thread.custom.<id>` | IDs at or above `thread_role_custom_first`; external/vendor-owned and verify-only until a later runtime recognizes the role |

The XDMA row truthfully inventories the policy role without inferring a worker
count from an opaque device ABI instance. Native XDMA objects own their fixed
worker teams, but the unchanged device ABI v1 exposes only aggregate backend
capabilities. Later accounting integration must supply an additive C++
description before a runtime-level report can claim a concrete XDMA count.
Host-adapter and vendor-owned roles therefore remain
externally owned and verify-only at this boundary.

## Memory accounting inventory

Every finalized report contains exactly one row for each stable category,
including categories with zero resources. Planned rows are a non-overlapping
projection of the existing memory-plan equation:

```text
runtime control + executor control + device control + phase scratch +
task scratch + trace storage = planned_bytes
```

| Category | Scope | Meaning |
| --- | --- | --- |
| `memory.runtime-control` | planned | Runtime object, copied configuration/graph/registration metadata, names, and telemetry control |
| `memory.executor-control` | planned | Runtime-owned executor graph, queue/completion, thread-handle, and scratch-control storage |
| `memory.device-control` | planned | Device manager registrations, outstanding/completion state, and service-lane control |
| `memory.phase-scratch` | planned | One backing pool with `phase_count` logical slices |
| `memory.task-scratch` | planned | One backing pool with `task_scratch_slots` logical slices |
| `memory.trace-storage` | planned | Fixed telemetry slot storage |
| `memory.registered-state` | informational external | Borrowed canonical state bytes |
| `memory.backend-control` | informational external | Backend-reported private control bytes |
| `memory.registered-device-buffer` | informational external | Sum of borrowed registered buffer spans, checked at finalization |
| `memory.runtime-thread-stack` | excluded | Known runtime-owned thread count; standard-library stack bytes are not observable in M15-01 |
| `memory.external-thread-stack` | excluded | Known caller/host lanes plus explicitly unknown vendor cardinality when a backend or requested unknown-cardinality external role is present |
| `memory.host-provider` | excluded | Reserved model row for later host-provider allocations; zero in M15-01 |

`accounted_bytes` retains the existing requested plan/informational semantics;
it is not RSS, committed-page, or residency evidence. `committed_bytes`,
`resident_bytes`, `locked_bytes`, `pinned_bytes`, and huge-page fallback remain
zero, while `applied` and `verified` remain `not_attempted`.

## Resolution and validation

Portable defaults resolve to current 1.2.1 behavior. Runtime-owned executor
workers resolve to yield waiting, watchdog/device services resolve to park,
external lanes inherit their owner behavior, scratch retains its finalized
alignment, and memory mutation features resolve disabled. Finalization does
not change startup, callback, steady-state, or teardown behavior.

Structurally valid best-effort non-default requests are retained as requested,
resolve to the portable no-op state, and report `unsupported_best_effort`.
Strict requests fail finalization because native apply-and-verify is not part of
M15-01. Finalization also rejects:

- out-of-capacity request/CPU-set counts;
- unknown non-extension role or memory identifiers;
- duplicate roles or memory regions;
- duplicate CPU identifiers and unterminated/invalid names;
- invalid enum discriminators, NUMA nodes, scheduler class/priority pairs,
  stack/guard pairs, and huge-page fallback combinations; and
- overflow in stack cardinality, guards, page rounding, registered-buffer
  totals, or the exact-once ledger sum.

These checks occur before allocation is committed or a runtime thread starts.

## Evidence and claim boundary

`tests/test_cpu_memory_policy.cpp` covers default and best-effort resolution,
malformed/duplicate/contradictory/strict inputs, bounded arithmetic, exact
inventory and accounting keys, external ownership, failed-finalize recovery,
custom-cardinality propagation, callback compatibility, and two-runtime
isolation.

Portable policy configuration, an inspectable report, strict validation, and
passing CI do not establish native policy application, physical residency,
RT1, or RT2. Native apply/verify, rollback barriers, providers, page residency,
and complete physical accounting remain later M15 batches.

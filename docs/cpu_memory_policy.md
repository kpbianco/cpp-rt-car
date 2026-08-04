# CPU and Memory Policy Model

M15-01 added the additive C++ policy and report model, and M15-02 applied and
read back runtime-owned thread policy through a fail-closed startup
transaction. M15-03 adds a bounded resident-region transaction for exactly
phase scratch, task scratch, and trace storage. This is portable RT0 and
named-host Linux functional behavior, not hardware, latency, RT1, or RT2
qualification.

`Runtime::set_cpu_memory_policy()` copies one bounded `CpuMemoryPolicy` while
the runtime is configuring. `Runtime::finalize()` validates the complete model,
builds the ordinary `MemoryPlan`, resolves an immutable
`CpuMemoryPolicyReport`, and transactionally acquires the selected backing
regions. A failed finalization publishes no partial report, destroys any
constructed owners, releases provisional tokens in reverse acquisition order,
and leaves the runtime configuring so the host can replace the policy or
provider and retry.

The model is separate from runtime-config schema 7 and its 25 JSON/profile
keys. It is an additive C++ source API in the installed `<rt/config.hpp>` and
`<rt/runtime.hpp>` headers. Stable C ABI v8 and device ABI v1 are unchanged.

## Bounded requests

A policy contains at most 16 thread-role requests and 16 memory-region
requests. CPU sets contain at most 256 explicit CPU identifiers and thread
names contain at most 31 characters plus a terminating NUL. Names use
`A-Za-z0-9._-`.

Thread policy covers CPU sets, scheduler class/priority, NUMA selection, wait
strategy, stack/guard size, bounded name, and best-effort or strict
requirements. Memory policy covers provider ownership, alignment, base-page
rounding, guards, prefault, locking, pinning, huge-page preference and explicit
fallback, NUMA placement, first-touch ownership, residency verification, and
rollback intent.

## Memory-provider contract

`Runtime::set_memory_provider()` can be called only while configuring. It
copies one `MemoryProvider` table with exact `struct_size`, API version 1,
known capability bits, zero reserved fields, borrowed `user_data`, and five
required nonthrowing callbacks: `acquire`, `apply`, `observe`, `rollback`, and
`release`. Provider callbacks must not throw or reenter the same runtime. The
host must retain `user_data` through checked stop or, if checked cleanup was
not possible, best-effort destruction.

Only active `memory.phase-scratch`, `memory.task-scratch`, and
`memory.trace-storage` rows invoke the provider. Each acquisition receives the
stable region identity, logical payload bytes, required alignment, page
rounding and guard request, huge-page preference and fallback rule, NUMA node,
and release rollback intent. The runtime rejects null or duplicate live tokens,
undersized/misaligned/overflowed spans, containment failures, overlapping
allocation extents, and capability/outcome overclaims. Token and extent checks
cover every live runtime instance, not only the three regions of one runtime;
an alias rejected against an existing owner is not released through the
malformed second acquisition. Inactive zero-byte rows remain visible without
invoking a provider.

Every provider `apply` invocation is rollback-eligible before the callback is
entered. The runtime therefore calls `rollback` once for that attempt before a
retry or release even if `apply` reports failure; providers must make rollback
safe for partially applied state. If rollback itself fails, the runtime retains
the token and operation as pending and may retry rollback from a later checked
`start()` or `stop()`; providers must make failed rollback retryable.

The default path retains aligned-new allocation for ordinary policy. On Linux,
base-page rounding, guards, or explicit huge-page preference use a
process-local mapping. Guard spans are base-page rounded and left inaccessible,
the usable base is rounded to the required alignment, and `MAP_HUGETLB` is
attempted only for the explicit preference. Failure uses an ordinary anonymous
mapping only when huge-page fallback was enabled; transparent-huge-page advice
is never reported as explicit huge-page success. A locking request uses its own
page-backed mapping even without other page policy, preventing page-granular
`mlock` or `munlock` from changing an unrelated runtime allocation.
The native path does not infer physical NUMA placement from a successful
memory-policy setter or policy readback. M15-03 therefore rejects a strict
runtime-provider NUMA request and resolves best effort to the usable default;
an injected provider may satisfy it only with NUMA-binding and independent-
observation capabilities.

## Thread role inventory

Role identifiers and accounting keys are stable numeric identities. Reports
carry role-level accounting keys rather than process thread IDs, pointers, or
global counters.

| Role | Stable name | Portable cardinality and ownership |
| --- | --- | --- |
| frame | `thread.frame` | One caller-owned logical lane; external and verify-only |
| executor worker | `thread.executor-worker` | Exactly `worker_count`; runtime-owned for native policies, host-owned and verify-only for `host_adapter` |
| watchdog | `thread.watchdog` | Zero or one runtime-owned lane from `watchdog_timeout_ns` |
| device service | `thread.device-service` | Zero or one runtime-owned lane for the complete runtime, not one per backend |
| XDMA I/O | `thread.xdma-io` | Backend-owned and verify-only; device ABI v1 does not disclose physical worker cardinality, so `cardinality_known` is false |
| custom accelerator/service | `thread.custom.<id>` | IDs at or above `thread_role_custom_first`; external/vendor-owned and verify-only until a later runtime recognizes the role |

The unchanged device ABI v1 exposes only aggregate backend capabilities, so
the runtime does not infer a concrete XDMA worker count. Host-adapter and
vendor-owned roles likewise remain externally owned and verify-only.

## Memory accounting inventory

Every finalized report contains exactly one row for each stable category,
including zero-resource categories. Planned rows are a non-overlapping
projection of the existing equation:

```text
runtime control + executor control + device control + phase scratch +
task scratch + trace storage = planned_bytes
```

| Category | Scope | Meaning |
| --- | --- | --- |
| `memory.runtime-control` | planned | Runtime object, copied configuration/graph/registration metadata, names, and telemetry control |
| `memory.executor-control` | planned | Runtime-owned executor graph, queue/completion, thread-handle, and scratch-control storage |
| `memory.device-control` | planned | Device manager registrations, outstanding/completion state, and service-lane control |
| `memory.phase-scratch` | planned/provider-capable | One backing pool with `phase_count` logical slices |
| `memory.task-scratch` | planned/provider-capable | One backing pool with `task_scratch_slots` logical slices |
| `memory.trace-storage` | planned/provider-capable | Fixed telemetry slot storage |
| `memory.registered-state` | informational external | Borrowed canonical state bytes; never mutated by memory policy |
| `memory.backend-control` | informational external | Backend-reported private control bytes; never mutated by memory policy |
| `memory.registered-device-buffer` | informational external | Checked sum of borrowed registered buffer spans; never mutated by memory policy |
| `memory.runtime-thread-stack` | excluded | Known runtime-owned thread count; stack/guard attributes are reported per thread, but committed/resident bytes remain outside the memory ledger |
| `memory.external-thread-stack` | excluded | Known caller/host lanes plus explicitly unknown vendor cardinality |
| `memory.host-provider` | excluded | Reserved accounting row; provider outcomes are reported on selected backing rows and are not double-counted here |

`accounted_bytes` retains finalized plan or informational payload identity, and
the six planned rows still sum exactly to `MemoryPlan::planned_bytes`.
`committed_bytes` separately reports provider usable commit after rounding;
resident, verified-locked, and provider-confirmed pinned bytes are distinct
observations. Actual guards/page size, explicit huge-page outcome, fallback,
acquired/applied/verified states, and provider/native errors remain on the same
row. These fields do not change or double count the plan.

## Resolution and validation

Portable defaults preserve 1.2.1 behavior. Linux finalization resolves
process-allowed CPU affinity, optional NUMA-constrained CPUs, scheduler
class/priority, names, wait strategy, and pthread stack/guard creation
attributes. Runtime-owned executor, watchdog, and device lanes apply and read
back resolved policy before the shared startup gate commits. The caller frame
lane is read back only. Host-adapter, XDMA, and custom/vendor lanes remain
externally owned and verify-only.

Finalization rejects out-of-capacity counts, unknown identities, duplicates,
invalid names/discriminators, contradictory scheduler/stack/guard/huge-page
requests, and arithmetic overflow. A strict non-default request for fragmented
runtime/executor/device control, either stack row, borrowed registered state or
device buffers, backend control, or the reserved host-provider row fails
instead of claiming application. Best-effort unsupported fields retain usable
default allocation with explicit fallback or unsupported state. Owner-thread
first touch and any other unobservable strict request are rejected rather than
inferred.

Strict thread creation, apply, or read-back failures leave the runtime
finalized and retryable. Best-effort unsupported, failed, or mismatched
operations remain visible and are never promoted to success. Portable
non-Linux builds retain default behavior and report unsupported non-default
native policy truthfully.

## Transaction and idle behavior

Finalization acquires phase scratch, then task scratch, then trace storage. Any
later acquisition, validation, construction, or finalization failure destroys
constructed owners and releases each acquired token exactly once in reverse
order. Startup applies and observes those three rows in stable order before the
M15-02 caller/thread transaction begins.

Caller first touch and prefault touch each committed page. Native Linux locking
uses `mlock`, while residency is independently sampled with `mincore`; setter
success is not independent lock readback, and `mlock` never establishes
provider, CUDA, device, or DMA pinning. Provider pinning is reportable only
from an advertised capability and independent provider observation.

A strict memory apply, observation, or mismatch failure rolls back attempted
memory operations in reverse region order and leaves the runtime finalized and
retryable. A rollback error remains inspectable and blocks a new apply or token
release until a checked `start()` or `stop()` retry completes it. A later
thread/device-start failure first quiesces the applicable device/executor/
watchdog lanes, then rolls back memory. No phase callback, device command
provider, or periodic observer runs before commit. Existing M14.1 retryable
device ownership markers remain authoritative when backend cleanup cannot
complete.

Successful stop first quiesces device, executor, and watchdog lanes, rolls
back resident-region operations, destroys provider-backed trace/executor
objects, and releases trace, task, then phase tokens. Provider-backed trace is
therefore unavailable after checked stop releases its token. Destruction is a
best-effort fallback. A rollback failure makes checked stop fail without
releasing tokens; retrying stop must complete rollback first. Provider callbacks
occur only on configure/finalize/start/stop control paths, never from
steady-state phase, task, device, watchdog, or periodic-observer execution.

Resolved `spin`, `yield`, or `park` controls only its matching worker/service
idle loop. Bounded wake sequences prevent lost work/stop notifications. No
detached, spill, emergency, per-frame, or extra worker is introduced.

## Evidence and claim boundary

Focused policy tests cover malformed and unsupported input, accounting keys,
provider-table validation/copying, stable acquisition/reverse release, invalid
spans and token aliasing, failure rollback/retry, truthful observations,
default and Linux native allocation, selected backing behavior, zero-byte rows,
no-allocation frames, and two-runtime isolation. Thread-policy tests cover
role sequencing, failure rollback/retry, wait strategies, external ownership,
and Linux application/readback on already allowed resources.

The earlier thread-policy evidence establishes named-host Linux native functional application/readback only; it is not a deployment or latency result.

These tests can establish injected-provider semantics and named-host Linux
functional allocation/OS-observed residency for the selected rows only. They
do not establish privileged lock/NUMA/huge-page availability, device or DMA
pinning, complete M15 byte closure, worst-case latency, physical hardware or
HIL behavior, field results, RT1, RT2, signing, release, deployment, or
production validation. Fragmented control allocations, runtime stack
residency, and exact external/backend accounting remain M15-04 work.

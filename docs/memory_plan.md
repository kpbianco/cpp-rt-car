# Finalized Memory and Overload Contract

Release 1.2 retains the M4 memory closure for the target-path runtime,
`rt::Runtime`. This is portable RT0 functionality: it defines and tests
bounded storage and nonblocking overload behavior, but it is not a latency or
hard-real-time qualification.

The legacy `SimCore`, frame arenas, `WorkerPool`, `rt::Scheduler`, `FiberPool`,
detached-thread GPU stub, plugins, and legacy snapshots are outside this
contract. M7 target-path checkpoint/replay registry metadata and M8
device-manager storage are included; caller-owned canonical state, artifacts,
device buffers, and backend-private storage are explicitly reported or bounded
but not allocated by the runtime.

## Finalization-time plan

`Runtime::finalize()` compiles the graph, calculates a `MemoryPlan`, compares
`planned_bytes` with `memory_budget_bytes`, and allocates the committed
storage. M15-03 obtains phase scratch, task scratch, and trace storage through
the default allocator or a configured provider before constructing their
owners. An overflow, invalid provider span/token/capability, allocation or
construction failure, or plan above budget returns an error while the runtime
remains configurable; provisional ownership is released in reverse order.

After successful finalization, `Runtime::memory_plan()` returns the immutable
plan. C hosts initialize `rtfw_memory_plan` with `rtfw_memory_plan_init()` and
read it with `rtfw_get_memory_plan()`.

The accounting equation is:

```text
planned_bytes =
    runtime_control_bytes +
    executor_control_bytes +
    device_control_bytes +
    phase_scratch_total_bytes +
    task_scratch_total_bytes +
    trace_storage_bytes
```

The plan reports requested payload/control bytes, not a process resident-set
size. It excludes allocator metadata and rounding internal to the C++ runtime,
OS thread stacks and implementation-owned thread state, executable and shared
library pages, host-owned callback/resource data and registered canonical state
bytes, caller-owned checkpoint/input-log buffers, registered device-buffer
payload bytes, backend-reported private storage, the small C ABI adapter
handle, and every legacy subsystem outside `rt::Runtime`.
Configuration/finalization temporaries are also excluded because they are
released before the running state.

`state_count` and `registered_state_bytes` report the frozen borrowed state.
`snapshot_max_bytes`, `replay_input_capacity`, and `input_log_max_bytes` expose
the enforced artifact bounds. State-registry entries themselves are included
in `runtime_control_bytes`.

`device_backend_count`, `device_buffer_count`,
`device_outstanding_capacity`, and `device_completion_batch` report the frozen
device plan. `device_control_bytes` includes copied registrations, the
outstanding/early-completion table, completion batch, counters, and service-lane
object. `device_backend_reported_bytes` is informational and excluded because
each backend owns that storage.

With the M11 `host_adapter` executor policy, `queue_slots` equals the declared
total host reservation rather than `worker_count * executor_queue_capacity`,
but `executor_control_bytes` includes only runtime-owned
completion/scratch-slot records and graph state. Host queue storage, worker
objects, stacks, affinity state, and job-system telemetry remain borrowed
host-owned memory and are excluded for the same reason as backend-owned
storage. The adapter must reserve the declared capacities before runtime
start; the runtime does not allocate or resize a host queue.

## M15 policy and resident-region projection

M15 leaves `MemoryPlan` and its equation source-compatible and adds a separate
`CpuMemoryPolicyReport`. Its six planned accounting rows map one-for-one to
`runtime_control_bytes`, `executor_control_bytes`, `device_control_bytes`,
`phase_scratch_total_bytes`, `task_scratch_total_bytes`, and
`trace_storage_bytes`; their `accounted_bytes` sum must equal `planned_bytes`.
Separate informational rows report registered state, backend-reported control,
and the checked sum of registered device-buffer spans. Stack and reserved
provider rows remain excluded.

M15-03 makes exactly the active phase-scratch, task-scratch, and trace-storage
rows provider-capable. They are acquired in that order with the logical plan
bytes and existing alignment, then used as backing for the same scratch slices
and telemetry slots. A zero-byte selected row remains observable and does not
invoke a provider. The default path preserves aligned-new allocation unless
requested Linux page rounding, guards, or explicit huge pages require a
process-local mapping.

`accounted_bytes` remains the requested plan identity. `committed_bytes`
reports validated usable commitment after page rounding and therefore may
exceed the logical payload without changing `planned_bytes`. Resident,
verified-locked, and provider-confirmed pinned bytes are separate observations;
actual guard spans, actual page bytes, explicit huge-page outcome, fallback,
and provider/native errors are separate too. No physical field is added to the
plan equation or counted again in `memory.host-provider`.

M15-02 thread stack/guard creation attributes remain per-thread readback, not
committed or resident stack evidence. Fragmented runtime/executor/device
control allocation, runtime-owned stack residency, exact external/backend
accounting, and complete cross-category byte closure remain deferred to
M15-04. Borrowed registered state/device buffers and backend-owned storage are
not mutated. See [the CPU/memory policy contract](cpu_memory_policy.md).

## Configuration

| Key | Default | Contract |
| --- | ---: | --- |
| `scratch_alignment` | 64 | Power-of-two alignment for phase and task scratch; accepted range is `alignof(max_align_t)` through 4096 |
| `task_scratch_bytes` | 4096 | Bytes exposed to each accepted execution context; zero creates a valid empty span and the maximum is 1,048,576 |
| `task_scratch_slots` | 1024 | Maximum simultaneously accepted execution contexts; must be at least the compiled phase count |
| `memory_budget_bytes` | 268435456 | Upper bound applied to `planned_bytes` at finalization; accepted ceiling is 1 TiB on 64-bit hosts and addressable `size_t` on 32-bit hosts |
| `overload_policy` | `reject_submission` | Selects `reject_submission` or `fail_frame` |
| `state_capacity` | 64 | Maximum registered canonical state regions; registry metadata contributes to runtime control bytes |
| `snapshot_max_bytes` | 1048576 | Maximum encoded checkpoint size; output storage is caller-owned |
| `replay_input_capacity` | 4096 | Maximum input records accepted by input-log write/replay |
| `input_log_max_bytes` | 1048576 | Maximum encoded input-log size; output storage is caller-owned |
| `device_backend_capacity` | 1 | Maximum backend registrations; copied table/control metadata is budgeted |
| `device_buffer_capacity` | 64 | Maximum borrowed buffer registrations; payload bytes are excluded |
| `device_outstanding_capacity` | 64 | Fixed runtime-wide outstanding-slot count and backend minimum |
| `device_completion_batch` | 16 | Fixed poll-result batch; cannot exceed outstanding capacity |

`scratch_bytes`, `trace_capacity`, `worker_count`, and
`executor_queue_capacity` also contribute directly to the plan. Queue slots
equal `worker_count * executor_queue_capacity`. M6 trace storage is
`trace_capacity * trace_slot_bytes`; an internal slot is larger than the
64-byte exported record because it contains atomic commit and producer-claim
state.

## Scratch ownership

Phase scratch has one aligned stride per registered phase. A phase receives
exactly `scratch_bytes`, its address is stable across frames, and no other
phase receives the same block. Contents are initialized during finalization
but are not cleared between invocations.

Every accepted graph, range, or reduction work item reserves one task-scratch
slot before it enters a worker queue. The slot:

- is aligned to `scratch_alignment`;
- exposes exactly `task_scratch_bytes` through `TaskContext::scratch()` or
  `rtfw_task_scratch()`;
- is exclusive to that callback, including while the callback synchronously
  submits and helps nested work;
- is released only after the callback returns.

This ownership rule prevents active parent, child, and grandchild contexts
from aliasing even when one worker helps all three. Task-scratch contents are
unspecified on entry and must not be retained after the callback returns.

Provider-backed storage preserves these payload sizes, strides, alignment,
lifetime, and non-aliasing rules. The runtime validates each provider usable
span before object construction. Checked stop first quiesces all execution
lanes, then destroys executor/trace objects and releases trace, task, and phase
tokens in reverse acquisition order. If rollback fails, checked stop retains
the owners and tokens and fails until a retry completes rollback. A
provider-backed trace ring is unavailable after its token is released.

## Bounded overload behavior

Queue reservation and scratch-slot reservation use bounded compare/exchange
attempts. Submission can therefore reject either physical exhaustion or
persistent contention; it never waits for capacity, allocates a spill record,
executes rejected work inline, or creates a helper thread.

| Condition | Status |
| --- | --- |
| Worker queue cannot accept the item | `queue_full` / `RTFW_STATUS_QUEUE_FULL` |
| No task-scratch slot can be reserved within the attempt bound | `scratch_exhausted` / `RTFW_STATUS_SCRATCH_EXHAUSTED` |
| No outstanding device slot or backend queue space | `device_queue_full` / `RTFW_STATUS_DEVICE_QUEUE_FULL` |

With `reject_submission`, a nested call returns the status to its caller. Any
accepted prefix completes before that call returns; the caller decides whether
to fail its phase. If root graph submission itself is rejected, the step
fails.

With `fail_frame`, the same status is returned and the active graph is also
marked failed. The step returns the overload status even if a user callback
ignores a failed nested submission. Already-running or accepted child work is
allowed to quiesce before `step()` returns.

`ExecutorStats::queue_full_rejections` and
`ExecutorStats::scratch_exhaustions` remain direct functional counters. M6 also
publishes them through the versioned metric schema without adding an RT-lane
allocation or output sink.

## Checkpoint and replay storage

Canonical state registrations are borrowed fixed byte spans. The runtime copies
only each stable identifier, schema version, pointer, and length. Registrations
with overlapping storage are rejected so restore destinations are independent.

The core C++ checkpoint and input-log codecs accept caller-provided buffers and
perform no heap allocation. The non-RT C input-log adapter may allocate a
capacity-bounded descriptor vector while validating C records. Output buffers
may not overlap registered state, input descriptors, or encoded input
payloads. A short buffer returns `capacity_exceeded` with
`required_bytes` and writes no partial artifact. Restore validates the entire
source before copying any state, so malformed or incompatible checkpoints
leave all registered bytes unchanged.

## Running-state boundary

Before `start()` creates a thread, M15-03 applies and observes resident-memory
policy for phase scratch, task scratch, and trace storage. It then creates the
configured fixed worker team, an optional M5 watchdog lane, and one M8 device
service lane when backends exist. After it returns, the
target CPU/device frame path uses preallocated graph, queue, scratch, trace,
outstanding, and completion storage. It contains no file I/O, blocking mutex,
hidden per-frame thread creation, heap fallback, or intentional heap
allocation. M6 trace producers make one atomic slot-claim attempt and drop on
contention rather than waiting. Watchdog waiting and device polling are
confined to runtime-owned service lanes. CPU device submission and completion
publication use fixed-capacity backend/manager state without waiting.
Workers use bounded queue operations and yield when idle or waiting for nested
work.

This boundary covers runtime and C-adapter code. User callbacks can still
allocate, block, perform I/O, submit to unrelated systems, or retain invalid
pointers; the runtime cannot make arbitrary host code real-time safe.

## Evidence

- plan, alignment, nested ownership, budget, and overload tests:
  `tests/test_memory_plan.cpp`;
- 64 complete frames under each executor policy with concurrent phases, range
  work, fixed-tree reductions, tracing, an armed watchdog, plus 64
  mock-device frames, under allocation instrumentation:
  `tests/test_trace_noalloc.cpp`;
- C ABI plan, scratch, malformed discriminator, and reserved-field checks:
  `tests/test_cabi_dlopen.c`;
- canonical-state accounting, overlap rejection, allocation-free artifact
  writing/restoring, corrupt-input transactionality, and parser mutation tests:
  `tests/test_memory_plan.cpp`, `tests/test_trace_noalloc.cpp`,
  `tests/test_determinism_replay.cpp`;
- allocation/deallocation pairing:
  `rt/src/aligned_storage.hpp`;
- provider/native resident-region acquisition, observation, rollback, and
  release: `rt/src/memory_policy.cpp`, `tests/test_memory_policy.cpp`;
- implementation:
  `rt/src/host_runtime.cpp`, `rt/src/executor.cpp`,
  `rt/src/device_manager.cpp`.

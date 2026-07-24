# Finalized Memory and Overload Contract

Release 0.5 completes the M4 memory closure for the target-path CPU runtime,
`rt::Runtime`. This is portable RT0 functionality: it defines and tests
bounded storage and nonblocking overload behavior, but it is not a latency or
hard-real-time qualification.

The legacy `SimCore`, frame arenas, `WorkerPool`, `rt::Scheduler`, `FiberPool`,
GPU mock, plugins, snapshots, and device experiments are outside this
contract.

## Finalization-time plan

`Runtime::finalize()` compiles the graph, calculates a `MemoryPlan`, compares
`planned_bytes` with `memory_budget_bytes`, and allocates the committed
storage. An overflow in any size calculation or a plan above the configured
budget returns `invalid_config` while the runtime remains configurable.

After successful finalization, `Runtime::memory_plan()` returns the immutable
plan. C hosts initialize `rtfw_memory_plan` with `rtfw_memory_plan_init()` and
read it with `rtfw_get_memory_plan()`.

The accounting equation is:

```text
planned_bytes =
    runtime_control_bytes +
    executor_control_bytes +
    phase_scratch_total_bytes +
    task_scratch_total_bytes +
    trace_storage_bytes
```

The plan reports requested payload/control bytes, not a process resident-set
size. It excludes allocator metadata and rounding internal to the C++ runtime,
OS thread stacks and implementation-owned thread state, executable and shared
library pages, host-owned callback/resource data, the small C ABI adapter
handle, and every legacy or device subsystem outside `rt::Runtime`.
Configuration/finalization temporaries are also excluded because they are
released before the running state.

## Configuration

| Key | Default | Contract |
| --- | ---: | --- |
| `scratch_alignment` | 64 | Power-of-two alignment for phase and task scratch; accepted range is `alignof(max_align_t)` through 4096 |
| `task_scratch_bytes` | 4096 | Bytes exposed to each accepted execution context; zero creates a valid empty span and the maximum is 1,048,576 |
| `task_scratch_slots` | 1024 | Maximum simultaneously accepted execution contexts; must be at least the compiled phase count |
| `memory_budget_bytes` | 268435456 | Upper bound applied to `planned_bytes` at finalization; accepted ceiling is 1 TiB on 64-bit hosts and addressable `size_t` on 32-bit hosts |
| `overload_policy` | `reject_submission` | Selects `reject_submission` or `fail_frame` |

`scratch_bytes`, `trace_capacity`, `worker_count`, and
`executor_queue_capacity` also contribute directly to the plan. Queue slots
equal `worker_count * executor_queue_capacity`.

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

## Bounded overload behavior

Queue reservation and scratch-slot reservation use bounded compare/exchange
attempts. Submission can therefore reject either physical exhaustion or
persistent contention; it never waits for capacity, allocates a spill record,
executes rejected work inline, or creates a helper thread.

| Condition | Status |
| --- | --- |
| Worker queue cannot accept the item | `queue_full` / `RTFW_STATUS_QUEUE_FULL` |
| No task-scratch slot can be reserved within the attempt bound | `scratch_exhausted` / `RTFW_STATUS_SCRATCH_EXHAUSTED` |

With `reject_submission`, a nested call returns the status to its caller. Any
accepted prefix completes before that call returns; the caller decides whether
to fail its phase. If root graph submission itself is rejected, the step
fails.

With `fail_frame`, the same status is returned and the active graph is also
marked failed. The step returns the overload status even if a user callback
ignores a failed nested submission. Already-running or accepted child work is
allowed to quiesce before `step()` returns.

`ExecutorStats::queue_full_rejections` and
`ExecutorStats::scratch_exhaustions` expose functional counters. They are not
the versioned production telemetry surface planned for M6.

## Running-state boundary

`start()` creates the configured fixed worker team. After it returns, the
target CPU frame path uses preallocated graph, queue, scratch, and trace
storage. It contains no file I/O, condition-variable wait, blocking mutex,
hidden thread creation, heap fallback, or intentional heap allocation.
Workers use bounded queue operations and yield when idle or waiting for nested
work.

This boundary covers runtime and C-adapter code. User callbacks can still
allocate, block, perform I/O, submit to unrelated systems, or retain invalid
pointers; the runtime cannot make arbitrary host code real-time safe.

## Evidence

- plan, alignment, nested ownership, budget, and overload tests:
  `tests/test_memory_plan.cpp`;
- 64 complete frames under each executor policy with concurrent phases, range
  work, fixed-tree reductions, tracing, and allocation instrumentation:
  `tests/test_trace_noalloc.cpp`;
- C ABI plan, scratch, malformed discriminator, and reserved-field checks:
  `tests/test_cabi_dlopen.c`;
- allocation/deallocation pairing:
  `rt/src/aligned_storage.hpp`;
- implementation:
  `rt/src/host_runtime.cpp`, `rt/src/executor.cpp`.

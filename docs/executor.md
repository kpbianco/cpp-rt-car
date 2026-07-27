# Unified CPU Executor

Release 0.11 carries the M3 CPU-execution surface in `rt::Runtime` through the
M8 release. The M4 memory contract still governs executor storage. The
compiled graph, independent phase callbacks, nested range work, and nested
reductions all use one runtime-owned worker team and one internal immutable work
record. The legacy `SimCore`, `WorkerPool`, `rt::Scheduler`, and `FiberPool`
remain compatibility experiments; `rt::Runtime` does not call them.

This is portable RT0 functionality. It is not a hard-real-time or
worst-case-latency claim.

## Lifecycle and ownership

Configuration selects an executor policy, worker count, and fixed per-worker
queue capacity. `finalize()` validates that configuration, compiles successor
and indegree tables, precomputes static phase assignments, allocates the queue
and aligned phase/task scratch storage, and commits the memory plan. `start()`
creates
exactly the configured worker count. No executor thread is created after
`start()` returns. `stop()` rejects calls during a step, stops the team, and
joins every worker. If the M5 watchdog is configured, `start()` also creates
one separate service lane; it never executes graph or nested CPU work.

Workers use bounded lock-free local rings. They yield when idle; there is no
condition-variable service thread, emergency spawn, detached task, or
unsubmitted inline fallback in this executor. A worker waiting for nested work
may execute an already-enqueued task. That work-helping rule is part of normal
execution and prevents nested-pool deadlock.

## Policies

| Policy | Placement and execution |
| --- | --- |
| `static_deterministic` | Phase `i` is preassigned to worker `i % worker_count`; range/reduction task `j` from phase `i` is assigned to `(i + j) % worker_count`. Workers consume only their own queue. |
| `bounded_throughput` | Graph phases retain a stable initial queue, while nested work is submitted to the current worker's local queue. An idle worker makes at most one pass over the other configured queues before yielding. A successful pop from another worker's queue is counted as a steal. |

`Runtime::static_phase_assignment_at()` exposes the frozen phase metadata for
the static policy. `Runtime::executor_stats()` reports the selected policy,
configured worker and queue counts, accepted submissions, local executions,
steal attempts, successful steals, queue rejections, scratch exhaustions, and
worker starts. These
counters are functional M3 evidence, not the versioned production
versioned observability counters completed in M6.

The graph compiler's registration-index topological order remains the canonical
introspection order returned by `compiled_phase_at()`. Runtime callback
completion order is intentionally not a total order: independent ready phases
may overlap, and either policy may produce any order that satisfies every
declared dependency. Hosts must not infer synchronization from registration or
completion order.

## Nested range and reduction work

Every phase callback receives a short-lived `TaskContext` as
`CallbackContext::tasks`. `parallel_for()` partitions `[0, item_count)` into
fixed contiguous grains. Each range callback receives its begin/end interval,
stable task index, phase index, and actual worker index.

`parallel_reduce()` first runs the same fixed leaves, then invokes the supplied
combine callback in deterministic binary-tree stages. The host owns partial
storage; a combine for `(left, right)` must merge the right partial into the
left partial. All callbacks in one stage finish before the next stage starts.
An empty input succeeds without invoking either callback.

Both operations are synchronous and may be called from range or reduction
callbacks again. Child work is always submitted to the same executor. The
calling worker helps its selected policy while waiting, so nesting does not
create another scheduler or block the complete worker team behind child work.

The C ABI exposes the same behavior through the opaque callback-local
`rtfw_task_context`, `rtfw_parallel_for()`, and `rtfw_parallel_reduce()`.

## Bounded submission

`executor_queue_capacity` is a per-worker power of two in `[2, 1048576]`.
Queue operations make at most 64 compare/exchange attempts. A physically full
queue, or one that remains contended for that bounded attempt budget, returns
`rt::Status::queue_full` / `RTFW_STATUS_QUEUE_FULL`.

A range or reduction call can therefore produce an accepted prefix of child
tasks before a later submission is rejected. Every accepted child finishes
before the call returns, and the rejection is then reported. There is no
retry-until-success, heap spill, detached helper, or direct execution of the
rejected task.

Each accepted work item also reserves one slot from the finalized task-scratch
pool. Reservation uses the same bounded-attempt rule. Exhaustion or persistent
contention returns `scratch_exhausted`. `reject_submission` reports queue or
scratch rejection to the immediate caller; `fail_frame` also marks the active
frame failed even when a callback ignores that returned status. Root graph
submission failure always fails the step. The complete accounting and
ownership rules are in the [memory-plan contract](memory_plan.md).

Finalization also rejects a queue capacity too small to hold the compiled
graph's worst static per-worker phase count.

## Failure and synchronization

The queue release/acquire boundary and dependency release establish the
happens-before relationship from a completed prerequisite to its dependents.
A range/reduction wait establishes the same relationship from all accepted
children back to their parent callback.

If a phase returns an error or throws, the step records
`callback_failed`. Work not yet invoked observes cancellation and is skipped;
already-running independent phases may finish. The reported phase is the lowest
registration index among observed failures. Nested callback errors are returned
to the calling phase, which must propagate them if the frame should fail.

Phase scratch remains phase-local and stable across frames. Every graph,
range, and reduction callback additionally receives exclusive aligned task
scratch through `TaskContext::scratch()`. Its slot remains owned through nested
helping and is released only after that callback returns. Contents are
unspecified on entry and pointers must not escape the invocation. Cross-phase
state belongs in host-owned memory described by graph resource declarations.

## Evidence

- implementation: `rt/src/executor.cpp`, `rt/src/executor.hpp`;
- runtime integration: `rt/src/host_runtime.cpp`;
- policy, saturation, nested-work, reduction, stress, and steal tests:
  `tests/test_executor.cpp`;
- plan, task-scratch, nested ownership, and overload tests:
  `tests/test_memory_plan.cpp`;
- graph/reference and allocation regression tests:
  `tests/test_compiled_graph.cpp`, `tests/test_trace_noalloc.cpp`;
- dynamic C ABI coverage: `tests/test_cabi_dlopen.c`;
- sanitizer configuration: `.github/workflows/ci.yml`.

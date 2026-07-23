# Scheduler Status

RTFW 0.3 does not have one production scheduler. It has several experimental
execution components that must be consolidated under
[ADR-0001](adr/0001-one-executor-boundary.md).

## `SimCore` internal workers

`SimCore` creates a fixed internal worker team and dispatches chunked range
work through shared atomic dispatch state. This is the path used by
`rtfw_demo`. It is coupled to phase execution, pacing, frame arenas, metrics,
and adaptation.

## `WorkerPool`

`WorkerPool` stores `std::function` jobs in one bounded MPMC FIFO queue.
Submission can spin until queue space or an outstanding-work slot becomes
available. The current priority and category fields are metadata on the normal
path; they do not select a priority lane.

When emergency spawning is enabled, a high-priority submission can execute
inline for a one-thread pool or create a rate-limited detached helper thread
under load. That path is incompatible with a bounded fixed-thread RT lane and
is scheduled for removal. Defining `RTFW_DISABLE_EMERGENCY_SPAWN=1` prevents
helper creation, but it does not add priority scheduling.

The metrics named `steals` count times a worker finds the global queue empty
while outstanding work exists. There are no per-worker queues in this class,
so those values are idle/contention polls, not successful steals.

## `rt::Scheduler`

`rt::Scheduler` is a separate research component. It distributes pending tasks
round-robin to per-worker PMR queues, sorts those queues by deadline or period
before a run, selects local tasks with a priority-plus-age helper, and can steal
from another worker queue.

This is not full EDF admission or deadline scheduling: deadlines are only used
for initial local ordering, worker threads use ordinary OS scheduling, and no
deadline-miss contract is enforced. The monotonic PMR arenas are not supplied
with fixed backing storage, so upstream allocation remains possible.

## Target

Milestone M3 replaces the competing paths with one task representation and one
executor boundary. The first two policies are:

- `static_deterministic`: precomputed worker/chunk assignment;
- `bounded_throughput`: fixed local queues with bounded steal attempts.

Queue-full behavior, cancellation, nesting, priority semantics, and telemetry
will be explicit and invariant-tested. Periodic OS scheduling is a later,
deployment-qualified policy.

## Scaling tool

`tools/scaling/run_scaling.py` can sweep the current demo's worker count and
write JSON/CSV artifacts. Its reported phase percentiles come from rolling
histograms, and its “frame” summary is the sum of phase percentiles rather than
a directly measured end-to-end frame percentile. Treat it as a regression
smoke tool, not a schedulability analysis.

## Code anchors

- Current demo executor: `SimCore::initThreads`, `SimCore::workerLoop`;
  `include/simcore/SimCore.hpp`
- Global FIFO pool and emergency path: `WorkerPool`;
  `include/simcore/worker_pool.hpp`
- Research scheduler: `rt::Scheduler`; `rt/include/rt/scheduler.hpp`,
  `rt/src/scheduler.cpp`
- Target decision: [ADR-0001](adr/0001-one-executor-boundary.md)

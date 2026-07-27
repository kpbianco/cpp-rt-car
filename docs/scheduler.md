# Scheduler Status

RTFW 0.11 has one target-path CPU executor owned by `rt::Runtime`. It implements
the first two policies accepted by
[ADR-0001](adr/0001-one-executor-boundary.md). Several older execution
components remain for source compatibility but are not used by that runtime.

## `rt::Runtime` unified executor

The M3 executor owns fixed-capacity per-worker queues and creates its exact
worker set during `start()`. Static policy uses precomputed worker assignments.
Throughput policy submits nested work locally and makes one bounded victim pass
when a worker is idle. Its steal counter records only successful cross-worker
queue removals.

Graph callbacks, nested ranges, and fixed-tree reductions use the same task
representation and worker team. A full or persistently contended queue returns
`queue_full`; unavailable task scratch returns `scratch_exhausted`. Neither
path spins until capacity, executes a rejected task inline, or creates a
helper. See the [executor contract](executor.md) and
[memory-plan contract](memory_plan.md).

## Legacy compatibility paths

## `SimCore` internal workers

`SimCore` creates a fixed internal worker team and dispatches chunked range
work through shared atomic dispatch state. This is the path used by
`rtfw_demo`. It is coupled to phase execution, pacing, frame arenas, metrics,
and adaptation.

### `WorkerPool`

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

### `rt::Scheduler`

`rt::Scheduler` is a separate research component. It distributes pending tasks
round-robin to per-worker PMR queues, sorts those queues by deadline or period
before a run, selects local tasks with a priority-plus-age helper, and can steal
from another worker queue.

This is not full EDF admission or deadline scheduling: deadlines are only used
for initial local ordering, worker threads use ordinary OS scheduling, and no
deadline-miss contract is enforced. The monotonic PMR arenas are not supplied
with fixed backing storage, so upstream allocation remains possible.

## Remaining target work

M3 completes the runtime-owned `static_deterministic` and
`bounded_throughput` policies, and M4 closes their target-path memory, scratch,
and frame-level overload plan. The legacy classes are quarantined rather than
silently presented as equivalent implementations. A host job-system adapter
remains later. M5's caller-thread absolute cadence and prerequisite preflight
do not constitute a separately qualified periodic OS scheduling policy.

## Scaling tool

`tools/scaling/run_scaling.py` can sweep the current demo's worker count and
write JSON/CSV artifacts. Its reported phase percentiles come from rolling
histograms, and its “frame” summary is the sum of phase percentiles rather than
a directly measured end-to-end frame percentile. Treat it as a regression
smoke tool, not a schedulability analysis.

## Code anchors

- Unified executor: `rt/src/executor.cpp`, `rt/include/rt/runtime.hpp`
- Current demo executor: `SimCore::initThreads`, `SimCore::workerLoop`;
  `include/simcore/SimCore.hpp`
- Global FIFO pool and emergency path: `WorkerPool`;
  `include/simcore/worker_pool.hpp`
- Research scheduler: `rt::Scheduler`; `rt/include/rt/scheduler.hpp`,
  `rt/src/scheduler.cpp`
- Target decision: [ADR-0001](adr/0001-one-executor-boundary.md)

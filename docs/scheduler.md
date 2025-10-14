# Scheduler

This module provides a simple work-stealing scheduler used for experiments with
real‑time task management. The scheduler supports two policies:

* **EDF (Earliest Deadline First)** – tasks are ordered by their deadline and
  run accordingly. This is useful for multi‑rate subsystems such as fast feedback
  loops and sensor processing.
* **Fixed‑Frame** – tasks are sorted by their period, emulating a fixed frame
  rate scheduler where shorter period tasks run first.

Each worker thread owns a local queue backed by a thread‑local memory arena to
reduce remote NUMA traffic. Tasks have priorities and an aging counter; every
time a task waits in the queue its age increases, effectively boosting its
priority so that low priority tasks will eventually run and avoid starvation.

## Thread-scaling experiments

Run `python tools/scaling/run_scaling.py` after building `rtfw_demo` to measure
how the worker pool scales with the available cores. The helper sweeps powers of
two worker counts with simultaneous multithreading disabled and enabled,
capturing the interval `p99` frame time along with a variance estimate. Each run
prints a compact summary table and writes plotting inputs to
`results/scaling/<timestamp>.csv` plus a companion JSON blob for raw metrics.

These artefacts make it easy to spot when additional threads stop improving the
frame tail latency, and the JSON payload keeps the phase-level metrics available
for deeper dives.

### Emergency path

High priority tasks have access to an emergency execution path that bypasses
the shared queue when the pool is saturated. If the number of outstanding jobs
is at least the worker count, the scheduler attempts to spawn a detached helper
thread to run the task immediately. Spawns are guarded by a token bucket with a
capacity of two and a refill rate of two per second, ensuring that short bursts
of overload can be absorbed without allowing unbounded thread creation. When
the pool only contains a single worker thread the fallback executes the task
inline to avoid additional allocations. Emergency launches are logged to the
trace ring and counted in `worker.emergency_spawns` so monitoring can alert on
sustained overload.

The emergency path can be disabled at build time via the
`RTFW_DISABLE_EMERGENCY_SPAWN` switch (set it to `1` to disable). When disabled,
high priority submissions remain on the shared queue even under saturation.
Instead of `EV_EmergencySpawn`, the trace emits `EV_PriorityEnqueue` events to
indicate that the job was redirected through the priority lane of the queue.

Tasks are represented by the nested `Scheduler::Task` struct. The scheduler
exposes a helper `Scheduler::pop_next_with_aging` function that selects the next
task based on the `(priority + age)` heuristic. This function is used by unit
tests to verify the aging behaviour.

## Code anchors

- Task representation: `rt::Scheduler::Task`; `rt/include/rt/scheduler.hpp`
- Aging heuristic: `rt::Scheduler::pop_next_with_aging`; `rt/include/rt/scheduler.hpp`
- Worker execution: `rt::Scheduler::run`; `rt/include/rt/scheduler.hpp`, `rt/src/scheduler.cpp`
- Emergency handling: `WorkerPool::handleEmergency`, `WorkerPool::tryConsumeEmergencyToken`,
  `WorkerPool::logEmergencySpawn`; `include/simcore/worker_pool.hpp`


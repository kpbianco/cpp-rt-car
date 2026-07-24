# ADR-0001: One CPU executor boundary

- Status: Accepted
- Date: 2026-07-23
- Implementation milestone: M3
- Implemented: 0.4 target-path `rt::Runtime`

## Context

The prototype currently contains `SimCore` internal range workers, an optional
`WorkerPool` for phase-level execution, a separate `rt::Scheduler`, and a
`FiberPool`. Concurrent phase execution can invoke range work through one
shared `SimCore` dispatcher, so unrelated phases overwrite common dispatch
state.

The components also expose conflicting meanings for work stealing, priority,
deadlines, and execution ownership.

## Decision

RTFW will expose one executor boundary and one immutable task representation.
The compiled graph will submit all CPU work through that boundary.

Different policies may implement the boundary:

- static deterministic assignment;
- bounded local-queue throughput scheduling;
- a host job-system adapter;
- a qualified periodic policy.

Child/range work must use executor-owned task-group state. A phase cannot start
an unrelated nested scheduler.

Submission from an RT lane is bounded and returns a status. No executor policy
may create an unplanned helper thread after runtime start.

## Consequences

- `SimCore` range dispatch and the optional phase `WorkerPool` will be replaced
  or adapted behind the common executor.
- `rt::Scheduler` remains experimental until removed or integrated.
- Real work stealing requires local queues and explicit steal attempt/success
  semantics; a shared FIFO cannot use that name.
- Executor policy is selected before finalization and is immutable while
  running.
- Scheduler telemetry is defined by the selected policy rather than reused
  across incompatible implementations.

## Rejected alternatives

- Keep the current nested pools and add more locking. This retains conflicting
  ownership and increases RT-lane blocking.
- Make every phase own a pool. This prevents global capacity planning and
  predictable shutdown.
- Require one universal policy. Static RT and dynamic throughput goals require
  different policies behind the same contract.

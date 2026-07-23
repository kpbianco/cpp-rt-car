# ADR-0002: Host-driven time is the default embedding mode

- Status: Accepted
- Date: 2026-07-23
- Implementation milestone: M1/M5

## Context

Game engines and simulation hosts normally own their frame clock. The current
`SimCore::run()` owns cadence, while the C ABI bypasses normal schedule
initialization and calls a private step through an accessor workaround.

A single API cannot ambiguously mean both "execute one frame now" and "sleep
until the next runtime-owned release."

## Decision

The default embedding operation is a host-driven step:

- the host supplies frame index, simulation delta, and optional deadline;
- the operation never sleeps;
- wall-clock time is read only for telemetry/deadline accounting;
- replay can supply a synthetic clock and recorded inputs.

Self-paced execution is a separate mode that owns an absolute periodic release
schedule. It must use a per-runtime clock abstraction and expose release,
wake-up, start, finish, slack, and miss data.

## Consequences

- Engines can integrate without a competing frame loop.
- C and C++ APIs share the same explicit time model.
- Tests can use a fake clock without waiting on wall time.
- The private-step C ABI path is replaced by the M1 lifecycle surface.
- A borrowed host executor can be paired with host-driven time, but its latency
  is controlled by the host and cannot inherit an RTFW qualification.

## Implementation status

M1 implements host-driven `rt::Runtime::step()` and its C equivalent. Each
runtime has a local clock domain, and tests verify that a large simulation
delta does not pace the calling thread. Self-paced absolute release control is
still M5.

## Rejected alternatives

- Make every `step()` sleep. This conflicts with host frame ownership and makes
  deterministic tests slow.
- Infer cadence initialization on the first step. This hides ownership and
  makes deadlines ambiguous.
- Use a process-global clock. It prevents isolated runtimes and reliable tests.

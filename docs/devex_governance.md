# Developer Experience and Governance

`tools/new_system.py` is a small scaffolding helper. Public behavior is governed
by the [product contract](product_contract.md), accepted
[ADRs](adr/README.md), tests, and semantic versioning.

## Contribution workflow

1. Create a focused branch.
2. State whether the change is current behavior, experimental behavior, or
   target architecture.
3. Add tests for present-tense behavior and update affected contract docs.
4. Run the documented configure, build, and test commands.
5. Call out API/ABI, allocation, scheduling, determinism, and telemetry-schema
   impact in the pull request.

Repository branch-protection and reviewer rules are managed on GitHub. This
repository does not currently define a two-maintainer quorum, so documentation
must not invent one.

## ABI policy

The plugin and C surfaces are experimental in 0.10. The M1–M8 C configuration,
frame, callback, result, graph, task-scratch, memory-plan, periodic, watchdog,
degradation, preflight, metric-window, trace-cursor, provenance, canonical
state, checkpoint, input-log, replay, backend, buffer, device-phase,
health, and reset surfaces carry their current validation metadata,
and configuration carries
`RTFW_C_ABI_VERSION` 7; those checks do not yet constitute a stable ABI
promise.
Milestone M11 freezes exported symbols, ownership, structure sizing,
capabilities, errors, and compatibility policy.

## Thread-creation guard

`RTFW_ENFORCE_NO_RAW_THREADS` defaults on when assertions are enabled and is
also forced for a negative compile test (`simcore_thread_violation`). It is a
macro-based diagnostic, not a complete prohibition: it catches direct
`std::thread(...)` constructor expressions after `SimCore.hpp`, but not every
declaration pattern, and several runtime components intentionally create their
own threads. The target lifecycle replaces this partial guard with explicit
executor and service-lane ownership.

## Code anchors

- Scaffolding helper: `tools/new_system.py`
- Plugin ABI draft: `rt/include/rt/plugin_api.h`
- Negative raw-thread test: `tests/thread_violation.cpp`,
  `tests/CMakeLists.txt`
- ABI milestone: [roadmap](roadmap.md)

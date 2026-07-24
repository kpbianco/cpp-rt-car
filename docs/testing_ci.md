# Testing and CI Evidence

CI is regression evidence for selected builds and behaviors. It is not a
latency qualification and does not prove the complete product contract.

## Current coverage

- GoogleTest unit and integration tests cover phase execution, queues, arenas,
  numerics, snapshots, trace, metrics, mock devices, plugins, and utilities.
- Host-runtime tests cover lifecycle transitions, strict configuration,
  no-pacing host steps, callback failure containment, bounded traces, and
  isolation of two runtime instances.
- Compiled-graph tests cover deterministic order, cycles, invalid and
  foreign-runtime handles, direct/transitive resource ordering, immutable
  topology, and randomized DAG agreement with an independent reference
  executor.
- Allocation instrumentation observes no heap allocation during the first
  frame of a representative compiled graph (the M2 topology gate) and during
  64 complete M4 target-path frames under each executor policy with independent
  phases, range work, fixed-tree reductions, aligned task scratch, tracing,
  and an armed M5 watchdog.
- Fake-clock tests prove exact absolute releases/deadlines, no epoch drift after
  late frames, clock-failure propagation, one-shot watchdog events, and
  frame-thread degradation.
- Injected platform probes prove strict pass/fail reporting, rejection before
  worker creation, duplicate-prerequisite rejection, and disabled-mode
  non-interference.
- Observability tests prove stable provenance, adjacent-interval partitioning
  of cumulative counters, gauge sampling, exact trace-cursor loss, runtime
  isolation, JSON schema fields, and drop-without-wait behavior under slot
  contention.
- Determinism/replay tests compare D1 canonical state across one, two, and four
  workers; transfer checkpoints across worker counts; reproduce final state
  from checkpoint plus input log; require transactional rejection of corrupt
  and foreign artifacts; and exercise 5,000 deterministic parser mutations.
- Allocation instrumentation also covers checkpoint sizing/writing,
  inspection, restore, and input-log writing against caller-owned buffers.
- Unified-executor tests stress independent phases with nested ranges and
  fixed-tree reductions under both policies, verify stable static assignment
  metadata, force deterministic queue saturation, and require real local
  execution and successful cross-worker steals.
- A focused GCC ThreadSanitizer job runs the unified-executor, M4 memory-plan,
  M5 time/platform, M6 observability, and M7 determinism/replay suites.
- `test_differential_output.cpp` compares a sample numerical kernel with a
  checked-in golden result under an absolute drift threshold.
- fault-injection tests exercise selected allocator, delay, and transient-error
  paths.
- the determinism matrix runs a filtered integration test and emits the same
  integer-only D1 checkpoint artifact under GCC/Clang and FMA on/off. A
  separate exchange job byte-compares all four artifacts.
- a Clang/libFuzzer smoke job runs the allocation-free checkpoint and input-log
  inspectors for 20,000 generated inputs.
- Linux and Windows jobs exercise selected Debug/RelWithDebInfo builds.
- Executable shared/static C and C++ compiled-graph samples, dynamic C ABI
  loading, autotune-tooling, scaling-artifact, and documentation-contract jobs
  provide focused smoke coverage.

## What CI does not currently establish

- No job measures a statistically controlled worst-case or confidence-bound
  latency gate on dedicated hardware.
- Sanitizer jobs do not cover every sanitizer, platform, or code path.
- The libFuzzer smoke target is not a continuous fuzzing service and cannot
  establish parser safety for every input.
- M7 artifact checksums detect accidental corruption; they are not
  authentication and do not protect against maliciously rewritten artifacts.
- Cross-compiler artifact equality currently covers one canonical integer
  workload on Linux, not arbitrary callback code, floating-point behavior,
  architectures, standard libraries, or D2/D3 determinism.
- Passing mock-GPU tests do not exercise a hardware device.
- Best-effort host-hardening steps on shared runners are not RT evidence.

## Adding evidence

A present-tense feature claim should point to a test that fails when the
behavior disappears. A determinism claim must name its tier and comparison
matrix. A performance or RT claim must retain raw data, environment identity,
predeclared thresholds, and the measurement procedure.

## Code anchors

- Main workflows: `.github/workflows/ci.yml`
- Documentation contract: `.github/workflows/docs-contract.yml`
- Differential test: `tests/test_differential_output.cpp`
- Fault injection: `tests/test_fault_injection.cpp`
- Determinism integration: `tests/integration/test_determinism.cpp`
- Optional fuzz harnesses: `tests/jobqueue_fuzz.cpp`,
  `tests/snapshot_fuzz.cpp`
- Cross-build artifact producer: `tests/determinism_artifact.cpp`
- M1–M7 lifecycle, graph, executor, memory, time, platform, observability, and
  replay
  tests:
  `tests/test_host_runtime.cpp`,
  `tests/test_compiled_graph.cpp`, `tests/test_executor.cpp`,
  `tests/test_memory_plan.cpp`,
  `tests/test_periodic_runtime.cpp`,
  `tests/test_platform_preflight.cpp`,
  `tests/test_observability.cpp`,
  `tests/test_determinism_replay.cpp`,
  `tests/test_trace_noalloc.cpp`,
  `tests/test_cabi_dlopen.c`

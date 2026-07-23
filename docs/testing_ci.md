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
  frame of a representative compiled graph. This is the M2 topology gate, not
  the complete M4 RT-lane allocation proof.
- `test_differential_output.cpp` compares a sample numerical kernel with a
  checked-in golden result under an absolute drift threshold.
- fault-injection tests exercise selected allocator, delay, and transient-error
  paths.
- the determinism matrix runs a filtered integration test across selected
  compiler and AVX2/FMA build variants. Each job validates behavior within its
  own binary; CI does not compare hashes between compiler jobs.
- Linux and Windows jobs exercise selected Debug/RelWithDebInfo builds.
- Executable shared/static C and C++ compiled-graph samples, dynamic C ABI
  loading, autotune-tooling, scaling-artifact, and documentation-contract jobs
  provide focused smoke coverage.

## What CI does not currently establish

- No job measures a statistically controlled worst-case or confidence-bound
  latency gate on dedicated hardware.
- Sanitizer jobs do not cover every sanitizer, platform, or code path.
- The libFuzzer target is optional and is not a continuous fuzzing service.
- Passing snapshot tests do not establish a safe, stable interchange format.
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
- Optional fuzz harness: `tests/jobqueue_fuzz.cpp`
- M1/M2 lifecycle and graph tests: `tests/test_host_runtime.cpp`,
  `tests/test_compiled_graph.cpp`, `tests/test_trace_noalloc.cpp`,
  `tests/test_cabi_dlopen.c`

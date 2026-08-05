# Testing and CI Evidence

CI is regression evidence for selected builds and behaviors. It is not a
latency qualification and does not prove the complete product contract.

## Current coverage

- GoogleTest unit and integration tests cover phase execution, queues, arenas,
  numerics, snapshots, trace, metrics, mock devices, plugins, and utilities.
- Host-runtime tests cover lifecycle transitions, strict configuration,
  no-pacing host steps, callback failure containment, bounded traces, and
  isolation of two runtime instances.
- Runtime-profile tests cover complete schema-v7 application, allocation-free
  parsing, transactional failure, exact compatibility, bounded size/nesting,
  malformed UTF-8/JSON, wrong types, unknown/duplicate/missing keys, invalid
  cross-field values, and bounded error paths.
- Compiled-graph tests cover deterministic order, cycles, invalid and
  foreign-runtime handles, direct/transitive resource ordering, immutable
  topology, and randomized DAG agreement with an independent reference
  executor.
- M16-01 tests compare complete harmonic and non-harmonic `[0, lcm)` timelines
  field-by-field with an independent integer generator. They cover exact
  simultaneous-release ordering, reduced ratios, malformed/foreign/rebound/
  missing ownership, checked arithmetic and capacity boundaries,
  transactional retry, identity sensitivity, runtime isolation, unchanged
  host/periodic/mock-device dispatch, exact MemoryPlan reconciliation, and
  allocation-free inspection plus CPU/device frames.
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
- Device-runtime tests cover bounded queue/outstanding saturation,
  cancellation, delayed dependency release while independent CPU work
  proceeds, timeout, error, loss,
  health/reset, shutdown ownership, malformed backend tables, memory
  accounting, trace ordering, all 32 metrics, and concurrent state isolation
  between independent runtime/backend/buffer instances. A named isolation gate
  also runs explicitly in every supported Linux build tuple.
- Allocation instrumentation covers 64 post-warmup mock-device frames, and the
  dynamic C ABI test defines and drives a backend solely through loaded v7
  symbols.
- CUDA candidate tests use an injected CPU-only driver to cover fixed queue
  saturation, concurrent submitters, host/device and device/device copies,
  byte fills and range rejection, external allocation ownership, failed
  registration cleanup ownership, fixed kernel payloads, timeout quarantine,
  enqueue/query-failure drain/reset, context loss, retryable drain-before-free
  shutdown, runtime graph integration, and steady-state submit/poll
  allocation.
- XDMA candidate tests use an injected CPU-only blocking driver to cover H2C
  and C2H integrity, malformed registration/submission rejection, bounded
  saturation, timeout quarantine until physical worker return, reset-required
  health and soft recovery, retryable shutdown, concurrent submit/poll, and
  steady-state no-allocation behavior.
- Unified-executor tests stress independent phases with nested ranges and
  fixed-tree reductions under both policies, verify stable static assignment
  metadata, force deterministic queue saturation, and require real local
  execution and successful cross-worker steals.
- A focused GCC ThreadSanitizer job runs the unified-executor, M4 memory-plan,
  M5 time/platform, M6 observability, M7 determinism/replay, and M8 device
  suites plus the CPU-only M9 CUDA state machine.
  It also runs the M16 reference-plan, two-runtime, inspection, host/periodic,
  and mock-device regressions; this does not make the plan an active dispatcher.
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
  loading, real generated-profile runtime round trips, autotune-tooling,
  scaling-artifact, and documentation-contract jobs
  provide focused smoke coverage.
- The named Ubuntu GCC/Clang and Windows MSVC support tuples build and run
  relocated consumers from the exact extracted CPack archives. Pull-request
  CI also checks the 1.2 support/compatibility contract and creates then
  verifies each archive manifest.
- A tag release waits for all three supported-tuple archive consumers, requires
  exactly nine uniquely named assets, verifies the existing tag, and creates
  the GitHub Release once. Manual release-workflow runs do not publish.
- Release-tool unit tests cover manifest round trips, corruption, unlisted
  files, unsafe paths, incomplete commit identities, empty artifact sets, and
  negative mutations of the portable support and contract digests. CPack
  staging tests reject corrupt sidecars, unexpected/stale outputs, and
  accidental publication of its internal staging tree. Extraction tests cover
  flat TGZ/ZIP layouts, safe SONAME links, traversal rejection, and cleanup of
  a rejected partial destination. The same suite
  validates CUDA/XDMA `evidence_only` schemas and rejects promoted, duplicate,
  or unhealthy raw records.

## What CI does not currently establish

- No job measures a statistically controlled worst-case or confidence-bound
  latency gate on dedicated hardware.
- Sanitizer jobs do not cover every sanitizer, platform, or code path.
- The libFuzzer smoke target is not a continuous fuzzing service and cannot
  establish parser safety for every input.
- The profile parser has deterministic negative/mutation-style cases but is
  not yet attached to a continuous libFuzzer service.
- M7 artifact checksums detect accidental corruption; they are not
  authentication and do not protect against maliciously rewritten artifacts.
- Cross-compiler artifact equality currently covers one canonical integer
  workload on Linux, not arbitrary callback code, floating-point behavior,
  architectures, standard libraries, or D2/D3 determinism.
- Passing deterministic mock-device tests do not exercise a hardware device,
  driver, DMA path, or completion-latency bound.
- Passing fake-driver CUDA tests establish the backend state machine, not a
  CUDA toolkit build, physical GPU behavior, resource stability, recovery
  behavior, or completion-latency bound. The opt-in self-hosted CUDA workflow
  emits raw evidence but does not automatically qualify a tuple.
- Passing fake-driver XDMA tests establish queue and lifetime behavior, not an
  XDMA kernel-module build, PCIe/FPGA integrity, kernel page-pin/resource
  stability, failure recovery, or completion-latency bound. The destructive
  opt-in self-hosted XDMA workflow emits raw evidence but does not
  automatically qualify a tuple.
- Best-effort host-hardening steps on shared runners are not RT evidence.
- SHA-256 artifact manifests are integrity metadata, not signatures,
  provenance attestations, or proof that two builds are reproducible.

## Adding evidence

A present-tense feature claim should point to a test that fails when the
behavior disappears. A determinism claim must name its tier and comparison
matrix. A performance or RT claim must retain raw data, environment identity,
predeclared thresholds, and the measurement procedure.

## Code anchors

- Main workflows: `.github/workflows/ci.yml`
- Opt-in CUDA hardware evidence:
  `.github/workflows/cuda-qualification.yml`
- Opt-in XDMA hardware evidence:
  `.github/workflows/xdma-qualification.yml`
- Documentation contract: `.github/workflows/docs-contract.yml`
- Portable release workflow: `.github/workflows/release.yml`
- Release contract and manifest tests: `tools/check_release_contract.py`,
  `tools/stage_release_artifacts.py`, `tools/release_manifest.py`,
  `tools/extract_release_archive.py`, `tools/check_hardware_evidence.py`,
  `tests/test_release_tools.py`
- Differential test: `tests/test_differential_output.cpp`
- Fault injection: `tests/test_fault_injection.cpp`
- Determinism integration: `tests/integration/test_determinism.cpp`
- Optional fuzz harnesses: `tests/jobqueue_fuzz.cpp`,
  `tests/snapshot_fuzz.cpp`
- Cross-build artifact producer: `tests/determinism_artifact.cpp`
- M1–M12 lifecycle, graph, executor/host adapter, memory, time, platform,
  observability, replay, and device
  tests:
  `tests/test_host_runtime.cpp`,
  `tests/test_compiled_graph.cpp`, `tests/test_executor.cpp`,
  `tests/test_memory_plan.cpp`,
  `tests/test_periodic_runtime.cpp`,
  `tests/test_platform_preflight.cpp`,
  `tests/test_observability.cpp`,
  `tests/test_determinism_replay.cpp`,
  `tests/test_device_runtime.cpp`,
  `tests/test_cuda_backend.cpp`,
  `tests/xdma_backend_tests.cpp`,
  `tests/host_adapter_tests.cpp`,
  `tests/test_trace_noalloc.cpp`,
  `tests/test_cabi_dlopen.c`
- M16-01 rate model and reference compiler:
  `rt/src/rate_timeline.cpp`, `tests/test_rate_timeline.cpp`
- Stable ABI/export and installed-package gates:
  `tools/check_c_abi.py`, `abi/rtfw_c_abi_v8.exports`,
  `tests/package_consumer`

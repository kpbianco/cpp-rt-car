# RTFW Completion Roadmap

The roadmap converts the [product contract](product_contract.md) into
dependency-ordered milestones. A milestone is complete only when its exit gates
pass; file presence or a passing smoke test is not sufficient.

## Status legend

- **Complete** — deliverables and exit gates are represented in the repository.
- **Next** — next dependency-ordered implementation batch.
- **Planned** — blocked on earlier milestones.
- **Qualified separately** — not part of portable 1.0.

## Milestones

| ID | Status | Outcome |
| --- | --- | --- |
| M0 | Complete | Product contract, truthful documentation, versioning, and license |
| M1 | Complete | Runtime lifecycle, typed configuration, host-driven API, C ABI draft |
| M2 | Complete | Compiled graph, cycle rejection, and resource hazards |
| M3 | Complete | Unified CPU executor with honest selectable policies |
| M4 | Complete | Memory plan and zero-allocation RT-lane closure |
| M5 | Complete | Self-paced absolute cadence, watchdog, and platform preflight |
| M6 | Complete | RT-safe, versioned observability |
| M7 | Complete | Determinism tiers, safe snapshots, and replay |
| M8 | Next | Device ABI and deterministic fault-injectable mock |
| M9 | Planned | Real CUDA backend |
| M10 | Planned | Real XDMA backend for one named host/FPGA stack |
| M11 | Planned | Stable ABI, package/export cleanup, and engine adapters |
| M12 | Qualified separately | Portable 1.0, PREEMPT_RT, CUDA, and XDMA evidence |

## M0 — Product contract and truth reset

Deliverables:

- accepted product contract, RT/determinism tiers, lifecycle, support matrix,
  non-goals, and release gates;
- accepted ADRs for executor, time ownership, and device boundaries;
- README and supporting docs that distinguish implemented, experimental, and
  planned behavior;
- Apache-2.0 license;
- one repository version source;
- automated documentation/claim checks.

Exit gates:

- every runnable README command is exercised by CI;
- no present-tense README claim describes an absent CUDA/XDMA/config feature;
- no unqualified latency, determinism, or production-readiness guarantee;
- version and license checks pass.

## M1 — Lifecycle, configuration, and host API

Delivered in 0.2:

- `rt::Runtime` with explicit configure/finalize/start/step/stop states;
- strict typed configuration with callback, scratch, trace, and numerical
  policy behavior;
- a synchronous host-driven callback path with runtime-local time, trace,
  numerical helper, and scratch ownership;
- a size/version-checked experimental C ABI;
- executable C and C++ callback samples and dynamic-library coverage.

Exit gates:

- explicit configure/finalize/start/run/stop state machine;
- unknown configuration and CLI keys fail;
- every schema key maps to runtime behavior;
- host-driven steps never sleep;
- two runtime instances do not share clock, trace, numerical, or scratch
  state;
- C and C++ samples register and execute real user callbacks.

## M2 — Compiled graph

Delivered in 0.3:

- instance-owned phase and logical-resource handles across the C++ and C APIs;
- explicit dependency and read/write resource declarations;
- deterministic finalization-time topological compilation;
- pre-start cycle, invalid/foreign-handle, and unordered resource-conflict
  diagnostics;
- immutable compiled topology consumed by the synchronous host executor;
- randomized DAG/reference-executor and first-frame allocation evidence.

Exit gates:

- cycles, invalid handles, and conflicting resource access are diagnosed before
  start;
- graph topology is immutable after finalization;
- randomized DAG tests agree with a reference executor;
- no topology allocation occurs on the first frame.

## M3 — Unified executor

Delivered in 0.4:

- one runtime-owned fixed worker team for graph phases and nested CPU work;
- `static_deterministic` precomputed phase assignment and
  `bounded_throughput` per-worker queues with real successful-steal counters;
- synchronous nested range work and fixed-tree deterministic reductions;
- bounded queue submission with an explicit `queue_full` result;
- phase-local scratch isolation for concurrent callbacks;
- fixed thread creation in `start()` and joining in `stop()`, with no
  emergency, detached, or inline-bypass execution path in the unified
  executor.

Exit gates:

- independent phases with nested range/reduction work pass stress and TSAN;
- queue-full submission returns within a tested bound;
- no emergency/detached execution path;
- static policy produces stable assignment metadata;
- throughput policy demonstrates real local execution and successful steals.

## M4 — Memory closure

Delivered in 0.5:

- a finalization-time budget and inspectable immutable memory plan covering
  target-runtime control structures, queues, aligned phase/task scratch, and
  the trace ring;
- exclusive scratch slots reserved before graph/range/reduction work is
  accepted and retained through callback completion and nested helping;
- explicit `reject_submission` and `fail_frame` overload policies for bounded
  queue and scratch reservation failure;
- matching aligned allocation/deallocation ownership;
- a multi-frame allocation gate covering concurrent phases, nested range work,
  fixed-tree reductions, and tracing after start.

Exit gates:

- a complete finalized CPU frame performs zero heap allocation on RT lanes;
- no RT-lane file I/O, hidden thread creation, condition-variable wait, or
  blocking mutex;
- overflow policy is explicit and identical in Debug and Release;
- every execution context has valid scratch storage;
- allocation origin and deallocation are paired.

## M5 — Time and platform

Delivered in 0.6:

- a finite self-paced C++/C loop using epoch-based absolute releases and
  explicit relative deadlines;
- per-frame release, wake, start, finish, slack, miss, watchdog, and
  degradation results;
- a one-shot watchdog service lane that never invokes host code, plus
  frame-thread application of capped runtime degradation state;
- disabled-by-default, read-only strict Linux preflight covering the runtime
  clock, PREEMPT_RT, memory lock limit/current locked memory, isolated
  affinity, and realtime scheduling;
- fixed-capacity inspectable failure reports, deterministic fake-clock/probe
  tests, C ABI v4 coverage, and an armed-watchdog allocation gate.

Exit gates:

- fake-clock tests prove exact release/deadline behavior;
- self-paced mode uses absolute releases;
- one watchdog arm produces at most one event;
- degradation is applied on the frame thread;
- strict platform preflight explains and rejects unmet prerequisites.

The precise surface and qualification boundary are in
[the time/platform contract](time_platform.md).

## M6 — Observability

Delivered in 0.7:

- a fixed-width schema-v1 runtime trace with monotonic sequence numbers,
  nonblocking slot claims, and explicit overwrite/drop accounting;
- caller-owned, runtime-bound trace cursors with exact retained-window loss
  reporting and bounded caller-provided output;
- 22 stable counter/gauge definitions with cumulative and independent
  cursor-based interval windows;
- build, runtime-version, configuration, workload, and runtime-instance
  provenance on every snapshot/read;
- experimental C ABI v5 access and a non-RT JSON export helper;
- isolation, window-partition, loss, contention, C ABI, no-allocation, and
  ThreadSanitizer evidence.

Exit gates:

- production trace/counter emission preserves the M4 RT-lane gate;
- metric window semantics are invariant-tested;
- two runtime instances have isolated traces;
- output schemas include version/build/config/workload identifiers.

The exact schema, cursor behavior, and legacy boundary are in
[the observability contract](observability.md).

## M7 — Determinism and replay

Delivered in 0.8:

- explicit D0/D1 configuration with D2/D3 rejected until their evidence
  profiles exist;
- fixed-size caller-owned canonical state registration, frozen graph/state
  schema identities, and worker-count-independent D1 replay compatibility;
- versioned little-endian checkpoint and input-log formats with complete
  bounds, reserved-field, identity, and checksum validation;
- allocation-free inspectors and codecs that never allocate from encoded
  lengths, plus transactional registered-byte restore;
- checkpoint-plus-input replay through the existing synchronous `step()` path,
  with validation-before-restore and a final registered-state hash;
- C ABI v6, embedding samples, deterministic mutation tests, libFuzzer smoke,
  and compiler-exchanged artifact comparison.

Exit gates:

- D1 workloads match across supported worker counts;
- cross-compiler claims compare exchanged artifacts;
- snapshot fuzzing finds no crash or unbounded allocation;
- checkpoint plus input log reproduces registered state.

The supported tier boundary, exact artifact behavior, callback obligations, and
legacy exclusions are in
[the determinism/replay contract](determinism_replay.md).

## M8 — Device ABI and mock

Exit gates:

- bounded queue saturation, delay, timeout, error, loss, reset, and shutdown
  pass through the mock;
- CPU compute workers never block on a device;
- backend callbacks cannot outlive runtime/plugin ownership.

## M9/M10 — CUDA and XDMA

Each backend requires functional hardware tests, steady-state resource tests,
failure/recovery tests, latency decomposition, and a versioned support matrix.
Neither backend inherits an RT2 claim from the portable core.

## M11/M12 — Distribution and qualification

Portable 1.0 requires a stable C ABI, controlled symbols, clean
`find_package()` consumption, compatibility checks, and the release gates in
the product contract. PREEMPT_RT, CUDA, and XDMA qualifications publish raw
evidence for declared deployment tuples.

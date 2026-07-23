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
| M2 | Next | Compiled graph, cycle rejection, and resource hazards |
| M3 | Planned | Unified CPU executor with honest selectable policies |
| M4 | Planned | Memory plan and zero-allocation RT-lane closure |
| M5 | Planned | Self-paced absolute cadence, watchdog, and platform preflight |
| M6 | Planned | RT-safe, versioned observability |
| M7 | Planned | Determinism tiers, safe snapshots, and replay |
| M8 | Planned | Device ABI and deterministic fault-injectable mock |
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

Exit gates:

- cycles, invalid handles, and conflicting resource access are diagnosed before
  start;
- graph topology is immutable after finalization;
- randomized DAG tests agree with a reference executor;
- no topology allocation occurs on the first frame.

## M3 — Unified executor

Exit gates:

- independent phases with nested range/reduction work pass stress and TSAN;
- queue-full submission returns within a tested bound;
- no emergency/detached execution path;
- static policy produces stable assignment metadata;
- throughput policy demonstrates real local execution and successful steals.

## M4 — Memory closure

Exit gates:

- a complete finalized CPU frame performs zero heap allocation on RT lanes;
- no RT-lane file I/O, hidden thread creation, condition-variable wait, or
  blocking mutex;
- overflow policy is explicit and identical in Debug and Release;
- every execution context has valid scratch storage;
- allocation origin and deallocation are paired.

## M5 — Time and platform

Exit gates:

- fake-clock tests prove exact release/deadline behavior;
- self-paced mode uses absolute releases;
- one watchdog arm produces at most one event;
- degradation is applied on the frame thread;
- strict platform preflight explains and rejects unmet prerequisites.

## M6 — Observability

Exit gates:

- production trace/counter emission preserves the M4 RT-lane gate;
- metric window semantics are invariant-tested;
- two runtime instances have isolated traces;
- output schemas include version/build/config/workload identifiers.

## M7 — Determinism and replay

Exit gates:

- D1 workloads match across supported worker counts;
- cross-compiler claims compare exchanged artifacts;
- snapshot fuzzing finds no crash or unbounded allocation;
- checkpoint plus input log reproduces registered state.

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

# RTFW Completion Roadmap

The roadmap converts the [product contract](product_contract.md) into
dependency-ordered milestones. A milestone is complete only when its exit gates
pass; file presence or a passing smoke test is not sufficient.

## Status legend

- **Complete** — deliverables and exit gates are represented in the repository.
- **Candidate** — implementation and portable tests exist, but a milestone
  requiring external hardware evidence has not passed its qualification gates.
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
| M8 | Complete | Device ABI and deterministic fault-injectable mock |
| M9 | Candidate | Real CUDA backend; hardware qualification pending |
| M10 | Candidate | Xilinx Linux XDMA AXI-MM backend; hardware qualification pending |
| M11 | Complete | Stable ABI, package/export cleanup, and engine adapters |
| M12 | Complete | Portable 1.0 release contract; deployment qualification remains separate |
| M13 | Complete | Strict runtime profiles and operational target-runtime autotune integration |
| M14 | Complete | Professional SDK target, package boundary, and consumer-isolation contract |
| M14.1 | Complete | Recoverable device initialization and teardown safety closure |
| M15 | Active | M15-01 through M15-03 are published; M15-04 exact accounting/compatibility closure is complete locally and awaiting review/publication/required CI |
| M16 | Planned | Multi-rate deterministic simulation domains and release coordination |
| M17 | Planned | HAL v2, heterogeneous memory, and isolated accelerator submission lanes |
| M18 | Planned | Named CUDA, XDMA, RT1, and RT2 hardware qualification |
| M19 | Planned | Game-engine, simulation-host, and extension integration kits |
| M20 | Planned | Operational, security, release, and long-duration hardening |

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

Delivered in 0.9:

- C-compatible size/versioned backend ABI for capabilities, initialization,
  registered buffers, bounded submit/poll, cancel, health, reset, and shutdown;
- compiled device phases whose providers return without waiting while a
  runtime-owned completion lane retains and releases graph dependency tokens;
- preallocated outstanding/completion storage in the finalized memory plan;
- stable device statuses, schema-v2 trace/metrics, and experimental C ABI v7;
- deterministic fixed-capacity CPU mock with scripted delay, timeout, error,
  loss, saturation, reset, and shutdown behavior;
- C++ sample, dynamic C ABI backend test, steady-frame allocation gate, and
  sanitizer-focused device coverage.

Exit gates:

- bounded queue saturation, delay, timeout, error, loss, reset, and shutdown
  pass through the mock;
- CPU compute workers never block on a device;
- backend callbacks cannot outlive runtime/plugin ownership.

The exact ABI, ownership, scheduling, fault, and qualification boundaries are
in [the device backend contract](device_backend.md).

## M9 — CUDA backend candidate

Delivered in 0.10:

- an optional CUDA Driver API adapter around a caller-owned context and fixed
  caller-owned stream set;
- fixed-capacity event, submission, registered-buffer, external-device-buffer,
  and kernel registries;
- setup-time host pinning and device-mirror allocation with explicit external
  ownership alternatives;
- async host/device and device/device copies, byte fill, and fixed-payload
  kernel launch;
- nonblocking event-query completion, timeout quarantine until physical
  completion, enqueue-failure quarantine, soft reset, context-loss mapping,
  and drain-before-release shutdown;
- CPU-only fake-driver functional, concurrency, runtime-integration,
  no-allocation, sanitizer, and recovery tests;
- an opt-in real-GPU functional/latency evidence executable and self-hosted
  workflow;
- a schema-v1 support matrix with no qualified tuple.

Remaining exit gates:

- compile and run the adapter against a named CUDA toolkit/driver/GPU/OS
  tuple;
- publish repeated functional, steady-state resource, failure/recovery, and
  raw latency-decomposition evidence;
- select thresholds before the run and review the result;
- add the passing tuple to the versioned support matrix.

M9 remains Candidate until those hardware gates pass. The exact ownership,
timeout, evidence, and claim boundaries are in
[the CUDA contract](cuda_backend.md).

## M10 — XDMA backend candidate

Delivered in 0.11:

- a portable, injectable XDMA state machine implementing device ABI version 1;
- bounded registered-buffer and transfer-slot tables with fixed initialization-
  time I/O workers;
- nonblocking, allocation-free submit/poll paths that never invoke a character-
  device transfer on runtime CPU or completion lanes;
- validated H2C/C2H buffer access, channel, range, device offset, transfer
  limit, and power-of-two alignment;
- timeout quarantine until a running blocking driver transfer physically
  returns, plus stable health, soft reset, device-loss, and retryable shutdown
  behavior;
- an opt-in Linux adapter for official Xilinx XDMA AXI-MM character devices,
  anchored to one named upstream driver and example-design-compatible
  bitstream contract;
- portable fake-driver functional, saturation, concurrency, timeout, recovery,
  no-allocation, sanitizer, and ThreadSanitizer evidence;
- an opt-in destructive H2C/C2H integrity and raw-latency evidence executable,
  self-hosted workflow, and schema-v1 empty support matrix.

Remaining exit gates:

- compile and run against a declared x86-64 Linux, XDMA driver, PCI function,
  FPGA part, XDMA IP configuration, and bitstream tuple;
- publish repeated integrity, steady-state resource, saturation, timeout,
  device-loss, reset/rebind, and shutdown evidence;
- select latency thresholds before measurement and retain raw submit, poll, and
  completion-wait samples;
- review the evidence and add only a passing tuple to the versioned support
  matrix.

M10 remains Candidate until those hardware gates pass. Character-device
blocking and kernel page-pin/allocation behavior prevent any inherited RT1 or
RT2 claim. The exact contract is in
[the XDMA backend contract](xdma_backend.md).

## M11 — Stable distribution and engine adapters

Delivered in 0.12:

- C ABI v8 as the first stable binary boundary, including current/minimum
  compatibility metadata, a reviewed public-surface fingerprint, and a typed
  incompatibility status;
- hidden internal visibility, an exact checked-in C export allowlist,
  ABI-numbered ELF SONAME, and Linux dynamic-symbol verification;
- CMake package components for shared C, static C, C++ runtime, and optional
  CUDA/XDMA targets with pre-1.0 same-minor package-version compatibility;
- clean shared/static/C++ consumers configured against a relocated install tree
  on Linux and Windows;
- a real `host_adapter` executor policy exposed in C++ and stable C, using a
  borrowed capacity-matched host job system, runtime-owned scratch,
  generation-tagged completion contexts, bounded overload, and nested-work
  helping;
- functional, saturation, stale-completion, no-allocation, dynamic ABI, and
  ThreadSanitizer gates.

Exit gates:

- header/library ABI mismatch fails before runtime creation;
- the built shared-library export set equals the reviewed allowlist;
- no runtime CPU worker is created for the host-adapter policy;
- accepted host jobs complete exactly once without stale-token reuse;
- shared C, static C, and C++ imported targets configure and run after install
  relocation.

The exact promise and change rules are in
[the stable C ABI contract](c_abi.md) and
[the executor contract](executor.md).

## M12 — Portable release and deployment-qualification boundary

Delivered in 1.0:

- an explicit 1.x SemVer, deprecation, stable-surface, support-level, security,
  and release policy;
- a machine-readable portable support matrix naming Ubuntu 22.04 GCC 11,
  Ubuntu 22.04 Clang 14, and Windows Server 2022 MSVC v143 as supported RT0
  tuples;
- same-major CMake package compatibility while preserving C ABI v8 as the
  independent stable binary boundary and explicitly declining a C++ binary
  ABI;
- a dedicated concurrent two-runtime gate proving independent device backends,
  borrowed buffers, metrics, health, and shutdown state;
- deterministic release-contract validation with reviewed SHA-256 identities
  for stable headers, ABI manifests, licensing, support policy, package
  configuration, release tools, and required workflows;
- full-commit pins for every third-party workflow action, including the
  release publisher's artifact download;
- CPack install archives consumed directly from fresh extracted prefixes,
  content-addressed artifact manifests, negative staging/extraction/manifest/
  contract tests, and tag/version validation on every supported tuple;
- all-tuple tag publication with unique assets and fail-if-existing release
  semantics; manual workflow runs do not publish;
- validated CUDA/XDMA `evidence_only` schemas and source-commit-bound raw
  evidence manifests that cannot promote a hardware support tuple;
- a changelog and supported-version security policy;
- final evidence review that leaves RT1, RT2, CUDA, and XDMA claims unqualified
  unless their separate tuple procedures pass.

Exit gates:

- every named portable tuple is represented by a pinned required CI runner and
  exact declared compiler;
- version, changelog, package metadata, support matrices, stable surface, and
  release tag agree;
- every package archive is relocatable and covered by a source-commit,
  byte-length, and SHA-256 manifest;
- corrupt, missing, unlisted, duplicate, or path-escaping release artifacts
  fail validation;
- concurrent runtimes cannot share device or observability state;
- release documentation never converts portable CI or raw hardware evidence
  into an RT1, RT2, CUDA, or XDMA qualification claim.

M9 and M10 remain Candidate until real hardware records pass their independent
gates. PREEMPT_RT deployment records are also reviewed separately from portable
1.0. The exact release and support promise is in
[the release policy](release_policy.md) and
[portable support matrix](portable_support_matrix.json).

## M13 — Strict runtime profiles and operational autotune

Delivered in 1.1:

- installed `<rt/profile.hpp>` with a 64 KiB, allocation-free,
  transactional UTF-8/JSON parser and bounded diagnostics;
- complete resolved profiles containing all 25 runtime-config schema-v7
  fields, explicit envelope/runtime compatibility, and optional opaque
  experiment provenance;
- fail-closed rejection of malformed input, unknown/duplicate contract keys,
  missing fields, invalid types/cross-field values, incompatible versions, and
  trailing bytes;
- `rtfw_runtime_demo`, which loads a profile or `RTFW_PROFILE`, builds four
  independent physics systems plus a dependency barrier on `rt::Runtime`, and
  reports direct frame/deadline/executor metrics;
- an operational production autotune spec restricted to supported
  `worker_count`, `executor_policy`, and `executor_queue_capacity` factors;
- generated default/example profiles, content-derived profile IDs, schema and
  mapping checks, strict parser tests, and a mandatory generated-profile C++
  round trip in CI;
- unchanged stable C ABI v8, device ABI, and Apache-2.0 license.

Exit gates:

- parsing succeeds without allocation and never partially updates outputs;
- every runtime field is resolved and validated by the authoritative typed
  runtime contract;
- incompatible schema/runtime versions and negative parser mutations fail
  closed with bounded diagnostics;
- generated factor changes reach the effective `RuntimeConfig`;
- the real target-runtime demo accepts a generated profile and emits the
  metrics consumed by `run_one.py`;
- documentation does not convert autotune output into an RT1, RT2, CUDA, XDMA,
  or portable latency claim.

The profile format and precedence rules are in
[the runtime profile contract](runtime_profiles.md).

## M14 — Professional SDK and package boundary

Delivered in 1.2:

- canonical `runtime` component and `rtfw::runtime` C++20 target, with the
  existing `cpp_runtime` component and `rtfw::simcore_rt` target retained as
  1.x compatibility paths;
- a supported runtime archive containing only graph, executor, memory,
  time/platform, observability, replay/profile, and device-runtime code;
- default-off broad research targets for SimCore, plugins, scheduler,
  crashdump, and utilities, with a narrow separate 1.x compatibility archive
  retaining the accidental demo/fiber link path outside the supported runtime;
- an exact default installed-header allowlist plus focused status,
  configuration, and canonical-byte headers;
- isolated optional CUDA/XDMA adapter exports and build-local project warning,
  Werror, logging, and profiling policy;
- relocated consumers that validate every supported header alone, exact SDK
  inventory, automatic C++20 propagation, independent components,
  compatibility aliases, policy isolation, and the installed license digest;
- release-contract schema 2, with explicit preferred/compatibility targets,
  default/conditional headers, forbidden default surfaces, and experiment
  defaults.

Exit gates:

- stable C ABI v8, SONAME 8, device ABI v1, and existing schemas are unchanged;
- `rtfw::runtime` has no plugin, scheduler, fiber, crashdump, SimCore, HAL
  stub, GPU stub, `dl`, warning-policy, or feature-macro dependency;
- the default package exports only contracted targets and headers;
- optional vendor dependencies are resolved only for requested adapter
  components;
- old target names still compile and run supported runtime consumers;
- source and installed Apache-2.0 digests remain canonical.

## M14.1 — Recoverable device lifecycle safety

Delivered in 1.2.1:

- status-bearing, deterministic reverse buffer/backend cleanup that retains
  failed ownership markers and retries only unresolved operations;
- ownership-uncertain handling for failed initialization, including a checked
  shutdown attempt and recoverable failed rollback;
- runtime quiescence and fail-closed execution/state-mutation gates while
  cleanup is pending, without falsely reporting the public `stopped` state;
- ABI-v8-compatible C destruction that preserves a handle when implicit device
  cleanup fails, plus an explicit checked-stop integration contract;
- injected multi-backend, failed-start, CUDA/XDMA partial-initialization,
  dynamic-C, warnings-as-errors, sanitizer, ABI, and release-contract
  evidence.

Exit gates:

- no registered buffer or initialized backend marker is cleared after its
  release callback fails;
- no backend shutdown runs while one of its registered buffers remains;
- first cleanup failure is preserved while independent reverse cleanup
  continues;
- repeated stop touches only unresolved ownership and becomes idempotent after
  success;
- execution, restore, and replay cannot mutate state during cleanup-pending;
- C ABI v8, 70 exports, SONAME 8, device ABI v1, schemas, package inventory,
  qualification claims, and Apache-2.0 remain unchanged.

## M15 — CPU and memory policy

M15-01 delivered:

- additive C++ thread and memory policy request types without changing
  runtime-config schema 7 or stable C ABI v8;
- stable role/category identifiers and unique accounting keys for every
  current runtime-owned or borrowed thread lane and memory-plan/excluded
  region;
- transactional finalization validation for malformed, contradictory,
  duplicate, unsupported-required, overflow, and capacity-invalid requests;
- portable default/fallback resolution with distinct requested, resolved,
  applied, and verified report stages;
- host-adapter, frame, XDMA, and future accelerator lanes represented as
  externally owned and verify-only;
- focused default, boundary, ownership, accounting, device-memory, and
  two-runtime isolation tests.

M15-02 delivered locally:

- an additive injectable C++ current-thread provider plus Linux affinity,
  normal/FIFO/RR scheduler, and bounded-name apply/readback;
- deterministic per-runtime startup commit/abort barriers for executor,
  watchdog, and device-service lanes, with no callback or service work before
  commit;
- executor `spin` and `yield` wait-policy application, with unsupported
  `park`, `adaptive`, NUMA-node, and custom-stack fields retaining explicit
  best-effort fallback or strict rejection;
- external frame verification without mutation and unchanged verify-only
  host-adapter/vendor ownership;
- retained apply/readback status, native system errors, rolled-back state,
  strict failure cleanup, retry, native Linux, and two-runtime tests.

M15-03 delivered locally:

- an additive injectable C++ memory-region provider with opaque allocation
  handles, capability-based resolution, verification, and reverse release;
- provider-created phase scratch, task scratch, trace storage, and requested
  runtime-owned executor/watchdog/device-service stacks;
- Linux page rounding, guards, prefault/frame first touch, lock/pin,
  huge-page preference/requirement and fallback, NUMA binding where available,
  residency readback, and supplied-stack thread creation;
- strict failed-finalization/start rollback with inspectable reports and clean
  retry, plus truthful best-effort fallback and external verify-only behavior;
- injected-provider and unprivileged Linux guard/residency/custom-stack tests.

M15-04 delivered and verified locally:

- additive `MemoryAccountingSnapshot` totals the unique region inventory by
  ownership and accounting scope;
- live runtime commitment combines logical non-provider control bytes, actual
  phase/task/trace provider commitment, and active custom stacks exactly once;
- host/backend registrations, provider-reported resident/locked/pinned payload, and
  resolution/allocation/huge-page fallback are separately totaled once;
- finalization/startup overflow or closure mismatch fails before callbacks and
  uses the existing reverse cleanup; stop and rollback remove live stacks;
- installed C++ consumers compile the additive surface while C ABI v8, device
  ABI v1, package targets, the M4 budget, and RT0 claims remain unchanged.

Future extensions outside M15:

- provider-backed verification handles for any future externally owned lane
  that elects to expose one; current host-adapter and vendor lanes remain
  verify-only without mutation;
- `park` and `adaptive` executor wait implementations if later product work
  accepts their wake/boundedness contract.

M15-01 performs no native mutation. M15-02 owns thread apply/verify and startup
rollback. M15-03 owns provider-created contiguous regions, custom stacks, and
residency behavior. M15-04 owns exact accounting and compatibility closure.
See [the policy model](cpu_memory_policy.md).

Exit gates:

- strict policy fails closed before callbacks run and rolls back every partial
  thread or memory commitment;
- all runtime-owned threads and regions are represented exactly once;
- portable defaults retain current behavior and all zero-allocation,
  determinism, sanitizer, ABI, and package gates pass;
- no RT1 or RT2 claim is made from policy configuration alone.

## M16 — Multi-rate simulation

Planned outcome:

- explicit rate domains, rational release relationships, phase ownership, and
  deterministic cross-rate data-transfer semantics;
- bounded overrun policy, release/deadline observability, and host-driven plus
  self-paced coordination without constructing hidden worker pools.

Exit gates:

- harmonic and non-harmonic schedules match a reference timeline;
- cross-rate snapshots are race-free and deterministic at their declared tier;
- overload never creates unbounded catch-up work or silently skips mandatory
  releases.

## M17 — HAL v2 and heterogeneous memory

Planned outcome:

- a versioned HAL v2 alongside a complete device-ABI-v1 compatibility shim;
- explicit host, pinned, device, shared, and imported memory domains with
  alignment, coherency, ownership, and synchronization contracts;
- fixed-capacity per-backend submission lanes so executor workers never invoke
  potentially blocking vendor APIs;
- capability discovery, completion/error/timestamp domains, CUDA and XDMA v2
  adapters, and lifecycle-safe plugin/factory ownership.

Exit gates:

- no vendor callback executes on an executor worker;
- submit, worker, and poll steady state is allocation-free and bounded;
- blocked-driver, queue failure, quarantine, coherency, cross-instance,
  sanitizer, and v1-compatibility tests pass.

## M18 — Hardware and real-time qualification

Planned outcome:

- retained evidence for named CPU/kernel/BIOS/compiler/runtime/workload tuples;
- CUDA driver/GPU and XDMA driver/firmware/bitstream/BDF tuples with functional,
  recovery, resource-stability, and latency-decomposition evidence;
- RT1/RT2 promotion only against thresholds selected before measurement.

Exit gates:

- qualification tools reject incomplete provenance, post-selected thresholds,
  unstable resources, or missing failure/recovery trials;
- only reviewed tuples enter support matrices; portable builds retain no
  implied latency or hardware claim.

## M19 — Engine and extension integration

Planned outcome:

- maintained adapters and examples for engine job systems, allocators,
  clocks, telemetry, state serialization, and external GPU/FPGA resources;
- lifecycle-safe extension registration, version negotiation, and unload
  rules with no dependency on the experimental plugin path.

Exit gates:

- at least one representative game-engine-style host and one independent
  simulation host pass relocated integration, hot-reload/unload, saturation,
  and long-run tests;
- adapters do not bypass runtime ownership, capacity, determinism, or
  qualification boundaries.

## M20 — Operational and release hardening

Planned outcome:

- fuzzing and sanitizer matrix expansion, static analysis, SBOM/provenance,
  signed reproducible releases, vulnerability response, deprecation tooling,
  long-duration soak/fault campaigns, and supported-version maintenance rules.

Exit gates:

- every shipped artifact is reproducible, attributable, install-tested, and
  covered by the declared support/security policy;
- release promotion is blocked by ABI, docs, package, license, evidence,
  sanitizer, soak, or unresolved critical-security failures.

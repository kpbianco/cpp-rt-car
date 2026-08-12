# Real-Time Readiness Gates

This checklist is a release/qualification gate, not a feature inventory. RTFW
1.2 has not completed any end-to-end RT2 qualification.

## Portable runtime gates

- [x] Target `rt::Runtime` configuration is finalized into an immutable graph
  and bounded CPU resource plan (M4 functional gate).
- [x] Invalid/foreign handles, cyclic graphs, and declared resource conflicts
  fail before start (M2 functional gate; omitted declarations remain host
  error).
- [x] The target CPU runtime lane creates no hidden threads and performs no
  intentional heap allocation, file I/O, condition-variable wait, or blocking
  mutex after start (M4 instrumented functional gate; user callbacks and
  legacy paths remain outside it; M8 adds an instrumented target device path).
- [x] Every target-runtime queue/scratch overflow, device timeout,
  cancellation, and shutdown path has explicit bounded behavior and tests
  (legacy `SimCore` arena overflow remains outside the supported path).
- [x] Queue and task-scratch saturation are bounded, expose
  `reject_submission` / `fail_frame`, and create no emergency or detached
  helper (M4 functional gate).
- [x] Target device submission uses fixed outstanding/completion storage,
  returns on outstanding-slot or backend queue saturation, and never parks a CPU worker on
  completion (M8 functional gate).
- [x] Host-driven steps never sleep; finite self-paced operation uses absolute
  epoch-based release times (M5 fake-clock functional gate).
- [x] One watchdog arm produces at most one event, and capped degradation is
  committed by the frame thread after graph quiescence (M5 functional gate;
  no callback preemption).
- [x] Optional strict platform preflight is read-only, runs before runtime
  threads start, and fails closed with per-check explanations (M5 functional
  gate; not deployment qualification).
- [x] M15 functional closure for fragmented controls, runtime-owned stacks,
  declared external/backend accounting, and retryable rollback is retained at
  the audited baseline.
- [x] M16-01 compiles a bounded exact epoch-zero rate reference plan with
  checked arithmetic, phase ownership, immutable inspection, identity, and
  accounting tests. Reference compilation is a functional gate, not timing
  qualification.
- [x] M16-02 compiles exact initial/wrap/held/fresh/stale cross-rate selections
  and constructs a two-slot exact-generation SPSC primitive with synthetic
  ThreadSanitizer coverage.
- [x] M16-03 adds opt-in D0 mandatory-CPU conservative admission, bounded
  exact-order active dispatch, cross-rate transfer, explicit late actions,
  functional summaries, canonical checkpoint state, active replay rejection,
  allocation instrumentation, and ThreadSanitizer coverage. These are portable
  RT0 functional gates, not measured WCET, latency, HIL, field, RT1, or RT2.
- [x] M16-04 adds mandatory-driven bounded optional shedding/recovery,
  transactional policy checkpoint state, a separate fixed-capacity versioned
  rate-action ring with exact loss/cursors/counters, accounting reconciliation,
  allocation instrumentation, and synthetic concurrency coverage. Mandatory CI
  and human review remain completion gates.
- [x] M17-01 adds a versioned C++ HAL v2 core and routes native-v2 and every
  unchanged device-ABI-v1 backend through one canonical bounded manager path.
  Adapter/table/context storage is fixed and exactly accounted; compatibility,
  malformed/failure, lifecycle-retry, isolation, sanitizer, and no-allocation
  tests are portable RT0 evidence only. M17-01 adds no heterogeneous memory,
  command batch, timeline completion, isolated vendor lane, or hardware claim.
- [x] M17-02 adds bounded six-domain memory/topology/timestamp records,
  explicit heterogeneous registration and inspection, declared correlation,
  exact identity/accounting, fail-closed malformed input, rollback/retry,
  isolation, compatibility, and resource-bound tests. These are portable RT0
  contract gates only; no physical allocation, DMA, coherency, topology, clock,
  vendor-lane, command-batch, timeline, or hardware behavior is qualified.
- [ ] A named deployment independently verifies requested memory locking,
  NUMA/huge-page outcomes, residency under load, and worst-case behavior;
  M15 provider/declaration injection, `mlock`, `mincore`, preflight, and hosted CI do not
  satisfy this gate or prove device/DMA pinning.
- [x] Multiple runtime instances have isolated clocks, numerical policy,
  allocator state, trace state, and device state, including concurrent
  independent mock backends, borrowed buffers, metrics, health, and shutdown
  (M12 functional gate).
- [x] Target trace/metrics emission uses fixed planned storage, schema-v2
  identifiers, explicit loss/window semantics, and per-runtime cursors (M6
  functional gate; export remains non-RT).
- [x] Target checkpoint/input-log input is bounds-checked, versioned, fuzzed,
  and tied to graph, registered-state schema, workload, and D0/D1 replay
  identity (M7 functional gate; legacy `SimCore` snapshots remain outside it).
- [x] The C and C++ embedding APIs can register and execute real host work with
  typed lifecycle/graph/executor/memory/time/platform/observability errors
  plus device and host-adapter/ABI compatibility errors (M1–M12 functional
  gate; not an RT qualification).
- [x] Named portable support tuples, 1.x compatibility rules, release
  archives, and content-addressed artifact manifests are machine-checked (M12
  release gate).

## Deployment qualification gates

- [ ] A named hardware, firmware, kernel, driver, toolchain, configuration, and
  workload tuple is frozen.
- [ ] CPU isolation, IRQ placement, affinity, scheduling, memory locking, power
  policy, and device topology pass a fail-closed preflight.
- [ ] Release, wake-up, compute, device-completion, slack, and miss samples are
  captured for a predeclared duration.
- [ ] Pass/fail thresholds are selected before measurement and raw artifacts
  are retained.
- [ ] Overload, thermal, device loss/reset, and shutdown behavior pass on the
  target.
- [ ] The qualification record is reproducible from a clean checkout.

M18-01 supplies machine-readable version-1 plan, record, review, and proposal
schemas plus bounded offline validation. Its synthetic fixtures pass only the
software contract. Every unchecked deployment gate above remains unchecked;
reviewer attribution and timestamps are unauthenticated, external pre-run plan
chronology remains a human gate, and no matrix entry is changed. A real combined
proposal also remains blocked on M17-05.

M15 implements process-local provider/resident handling for phase scratch,
task scratch, and trace storage only, plus logical control accounting and live
runtime-stack observation. Declarations are trusted metadata, not independent
observation. None of this changes any unchecked deployment gate above.
Provider-backed checked stop releases trace backing, so post-stop trace export
must occur before that cleanup or be treated as unavailable.
M16 declared-budget admission and fake-clock late/catch-up/shedding/recovery
results are not measured WCET, latency, RT1, or RT2 evidence. M17-01/M17-02
mock, fake-driver, synthetic-memory, compatibility-adapter, hosted CI, and
documentation results likewise do not prove physical accelerator memory,
pinning, DMA, coherency, topology, clock accuracy, HIL, field, latency, RT1, or
RT2 behavior. Mandatory M17-02 CI and human API, compatibility, concurrency,
accounting, lifecycle, and claim-boundary review remain required; M17 and
CAP-M17 remain incomplete.

See [the product contract](product_contract.md) for RT tiers and
[the roadmap](roadmap.md) for implementation ownership. M5 behavior and its
limits are specified in the [time/platform contract](time_platform.md); M6
schema and exporter boundaries are in the
[observability contract](observability.md); M7 artifact and replay boundaries
are in the [determinism/replay contract](determinism_replay.md); M8 backend and
mock boundaries are in the [device backend contract](device_backend.md). The
native-v2/v1-adapter boundary is in the [HAL v2 contract](hal_v2.md).

## M17-03 portable command/timeline status

- [x] Fixed command, wait, signal, timeline, queue, and completion capacities
  are validated and preallocated before start.
- [x] Each opted-in backend has one isolated Runtime submission lane; executor
  workers perform provider validation/copy but no backend call.
- [x] Explicit flush/invalidate/copy ordering and whole timeline completion are
  covered by synthetic failure-injected tests.
- [x] Timeout, cancellation, malformed completion, blocked stop request,
  reverse join, compatibility, isolation, and exact accounting have portable
  functional gates.
- [ ] Native CUDA/XDMA-v2 latency, physical memory coherence, hardware/HIL,
  field, worst-case timing, RT1, and RT2 remain unqualified and require named
  later evidence.

## M17-04 portable vendor-control status

- [ ] Native CUDA and XDMA command registrations do not yet reach the bounded
  M17-03 submission/service paths: canonical capability discovery supplies a
  zeroed header that both candidate callbacks reject. Backend-local tests do
  prove that direct candidate operations stay off executor workers.
- [x] CUDA Graph identifiers, copied bindings, one-stream ordering, one-event
  completion, malformed input, timeout, stop, and checked cleanup have
  fake-driver functional gates.
- [x] XDMA control aperture/width/access and event index/timeout/stop validation
  have portable fixture gates before any driver entry.
- [x] Device-ABI-v1 behavior, stable C ABI v8, schemas, package inventory,
  MemoryPlan equation, release, and support matrices remain unchanged.
- [ ] Physical Graph execution, pinning/device memory/coherency, design-safe
  MMIO, interrupts, hardware/HIL, field, bounded latency, RT1, and RT2 remain
  unqualified and require named later evidence.

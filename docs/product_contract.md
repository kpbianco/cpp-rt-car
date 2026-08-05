# RTFW Product Contract

Status: accepted contract for the supported 1.x portable runtime

Current release: 1.2.1 portable RT0 product with stable C ABI v8

## Product definition

RTFW is a bounded-resource C++20 simulation execution runtime with explicit
determinism tiers. A host defines a phase and resource graph, finalizes it
before execution, and runs it on a preallocated CPU executor. The target
runtime supports host-driven and self-paced frames and integrates asynchronous
device backends through bounded submission and completion interfaces.

The 1.2 target-path runtime satisfies the M1 lifecycle/callback, M2
compiled-graph, M3 unified
CPU-executor, M4 bounded-memory, M5 time/platform, and M6 observability
contracts at RT0 and the M7 registered-state D1/checkpoint/replay contract,
plus the M8 bounded device ABI and deterministic mock contract at RT0. M9 adds
an optional CUDA Driver API backend candidate with CPU-only evidence, and M10
adds the named XDMA candidate, but neither has a qualified hardware tuple. M11
adds the third host-adapter executor policy, stable C ABI v8, controlled
exports, and relocated installed-package consumers. M12 adds the named
portable support matrix, cross-instance device-isolation gate, 1.x
compatibility/release policy, and checked package-manifest workflow. No RT2,
CUDA, or XDMA qualification is implied by portable 1.2.
M13 adds the strict complete-profile parser, profile-driven target-runtime
demo, and operational runtime autotune mapping without changing C ABI v8.
M14 adds the canonical `rtfw::runtime` target, exact default SDK inventory,
consumer-policy isolation, and optional adapter dependency boundaries without
changing C ABI v8 or device ABI v1.
Release 1.2.1 closes the device lifecycle error path: failed initialization
rollback, buffer unregistration, and backend shutdown retain explicit ownership
state and are retried through checked `stop()` without changing either ABI.
M15-01 adds a bounded additive C++ CPU/memory policy model, and M15-02 adds
Linux native resolution, application, and readback for runtime-owned thread
roles plus a fail-closed startup barrier. M15-03 adds one copied,
size/versioned, five-callback C++ memory provider and a resident transaction for
exactly phase scratch, task scratch, and trace storage. Caller/external roles
remain verify-only, deferred and borrowed memory is not mutated, and schema 7,
C ABI v8, device ABI v1, release, package, and qualification claims remain
unchanged. M15-04 adds an exact logical extent ledger for constructed runtime,
executor, and device controls; live runtime-owned stack commitment and
supported observation; copied bounded declarations for opaque external and
backend accounting; and status-bearing retryable cleanup across device, lane,
stack, control, and selected-region ownership. M15 is complete at the audited
baseline after mandatory CI and maintainer merge. M16-01 adds an additive C++
rate-domain and phase-ownership model plus an immutable epoch-zero reference
timeline. It uses exact checked integer arithmetic and changes graph/replay
compatibility identity only for an explicit rate model. It does not yet change
callback dispatch or implement cross-rate storage, admission, late behavior,
shedding/recovery, or telemetry fields.

## Claim policy

Repository documentation and releases use the following terms precisely:

- **Implemented** means the behavior exists and has an automated functional
  test.
- **Experimental** means the behavior exists but its API, semantics, safety, or
  performance contract can change.
- **Planned** means the behavior is tracked in
  [the roadmap](roadmap.md) but is not part of the current runtime.
- **Qualified** means a named acceptance procedure passed on a declared
  hardware, operating-system, toolchain, configuration, and workload tuple.

An implementation test is not a real-time qualification. A benchmark result is
not a portable latency guarantee. No document may use an unqualified
"guarantee" for scheduling, latency, memory, determinism, or device completion.

## Target runtime contract

### Lifecycle

The 1.2 lifecycle is:

1. **Configure** — parse configuration, register phases, resources, callbacks,
   canonical state, executor policy, memory provider, and device backends.
   Ordinary allocation is allowed.
2. **Finalize** — validate and freeze the graph/state schema, compute replay
   identities, intern telemetry identifiers, calculate memory and queue
   capacities, and allocate bounded storage.
3. **Start** — run platform preflight checks, apply and observe selected
   resident-memory policy, create the fixed thread set, establish execution
   contexts, register device buffers, and warm memory.
4. **Run** — execute host-driven steps or a self-paced loop without graph
   mutation, hidden thread creation, or unbounded submission on an RT lane.
5. **Stop** — reject new submissions, drain or cancel according to policy,
   stop service lanes, reverse resident-memory operations, release provider
   tokens, and release other resources. A device cleanup failure keeps
   the prior public lifecycle state, gates execution and state mutation, and
   requires `stop()` retry before the host releases borrowed storage.

`rt::Runtime` now enforces the configure/finalize/start/step/stop state machine,
compiles dependencies and logical resource access at finalization, and freezes
the complete graph topology and static assignment metadata. `start()` performs
an optional fail-closed platform preflight, then creates the configured fixed
CPU team or borrows an explicitly attached host job system, then starts any
optional watchdog/device service lane. The legacy `SimCore`
constructor/run/destructor path remains experimental and does not inherit that
lifecycle.

### Time ownership

Host-driven time is the default embedding contract:

- `step(frame_context)` receives frame index, simulation delta, and an optional
  deadline from the host.
- A host-driven step does not sleep or advance a hidden wall clock.
- A separate self-paced mode owns an absolute periodic release schedule.

The 1.2 `rt::Runtime` implements both explicit modes. The host supplies frame
index, delta, and optional deadline to synchronous `step()` without runtime
pacing. `run_periodic()` executes a finite caller-thread loop using absolute
epoch-based releases; late frames never shift that epoch. Per-frame results
include release, wake, start, finish, signed slack, deadline miss, watchdog,
and degradation state.

See [ADR-0002](adr/0002-host-driven-time.md) and the
[time/platform contract](time_platform.md).

### CPU execution

The target has one executor boundary and one task representation. Executor
policies may differ:

| Policy | Intended behavior |
| --- | --- |
| `static_deterministic` | Precomputed worker/chunk assignment for reproducibility and low scheduling variance |
| `bounded_throughput` | Fixed-capacity local queues and documented, bounded steal attempts |
| `host_adapter` | Execution through a host job-system contract with explicit scratch and completion contexts |
| `periodic_rt` | Static execution plus qualified absolute release/deadline control |

Release 1.2 implements `static_deterministic`, `bounded_throughput`, and
`host_adapter`. Graph phases, nested ranges, and reductions share one task
representation; the first two policies own a runtime team while the third
borrows a capacity-matched host job system with explicit scratch and completion
tokens. The current `SimCore` workers, optional `WorkerPool`, `rt::Scheduler`,
and `FiberPool` are separate compatibility experiments, not target policies.
See [ADR-0001](adr/0001-one-executor-boundary.md) and the
[executor contract](executor.md).

The target C++ runtime is a source API governed by the 1.x release policy;
recompilation is required and no C++ binary ABI is promised. C plugins and
language/engine bindings that need a cross-release binary boundary use stable
ABI v8 and the installed `c_shared`/`c_static` package components described by
the [C ABI contract](c_abi.md).

### Memory and overload

The target running state has:

- a finalization-time memory and queue-capacity plan;
- explicit scratch storage for every execution context;
- bounded, nonblocking RT-lane submission;
- no hidden heap fallback, thread creation, file I/O, or blocking mutex on an
  RT lane;
- configured behavior for capacity exhaustion: reject, drop optional work,
  fail the frame, or terminate.

M4 finalizes aligned phase/task scratch, queue/control storage, and the trace
ring under an explicit budget. M8 adds bounded device-manager, outstanding,
and completion-batch storage. Bounded CPU queue/scratch and device submission
paths reject overload instead of spilling. Representative complete CPU and
mock-device frames observe no heap allocation after start. The plan's exact
accounting scope and exclusions are in [the memory contract](memory_plan.md).

M15 preserves the memory-plan equation while allowing exactly phase
scratch, task scratch, and trace storage to use a copied C++ provider table.
Finalization acquires them in stable order; startup applies and independently
observes policy before threads commit; failure and checked stop reverse
operations and token ownership. Default allocations retain aligned-new
behavior. Linux page policy uses process-local mappings with rounded guards,
explicit `MAP_HUGETLB` attempt/fallback, caller/prefault touch, `mlock`, and
`mincore` residency sampling. `mlock` is not lock readback or device/DMA
pinning. M15-04 validates fragmented control allocations as non-overlapping
logical extents against the three existing control terms. Runtime-owned stack
commitment is aggregated exactly once from live executor, watchdog, and
device-service lanes; observable Linux residency is sampled only while the
mapping is live. Opaque host/vendor/backend facts may be copied through bounded
configuring-time declarations and remain `declared_only`, never independently
verified or qualified. Missing facts remain `partial` or `unknown`.

### Rate-domain reference plan

While configuring, a host may register at most 64 copied rate domains and bind
every CPU or device phase to one instance-owned domain. A domain has a stable
1–63 byte identifier, positive integral-nanosecond period, 1–64 same-timestamp
substeps, relative deadline, non-binding budget/WCET estimate, criticality, and
optionality. The model is an additive C++ source API and is not runtime-profile
schema 7 or stable C ABI v8.

Finalization compiles the half-open relative interval `[0, lcm(periods))` with
checked gcd/lcm, multiplication, addition, and release-count arithmetic. The
65,536-entry ceiling is checked before construction. Each fixed-copy release
record exposes time, domain/phase identity, domain sequence, substep,
deadline, budget, criticality, and optionality. Equal timestamps order by
domain registration, compiled phase, then substep. Reduced period ratios use
registration-order domain zero as their exact reference.

This is a reference plan, not an active dispatcher. Host-driven and periodic
frames still execute the complete graph once. Budget/WCET is not admission or
timing proof, and no freshness, late, catch-up, shedding, or recovery behavior
is implied.

### Observability

The target runtime uses pre-registered numeric event/metric definitions,
fixed-capacity RT-lane emission, explicit loss accounting, runtime-isolated
cursors, and version/build/config/workload provenance. Serialization and
external transport remain non-RT host responsibilities.

Release 1.2 implements observability schema version 2 for `rt::Runtime`.
Schema 2 preserves IDs 0–21 and adds device events and metrics.
Metric cursors independently partition monotonic counters into intervals
without resetting global state; gauges are sampled rather than differenced.
Trace cursors report exact sequence loss after overwrite or contention. The
legacy `SimCore` registry and binary trace are outside this schema. See
[the observability contract](observability.md).

### Device execution

Devices are optional backends outside the core scheduler. A backend contract
must define capabilities, buffer registration, bounded submission, completion,
timeout, cancellation where supported, health, reset, and shutdown.

Release 1.2 retains the size/versioned, poll-only device ABI, registered
buffers, graph-integrated device phases, health/reset/shutdown control, and a
fixed-capacity deterministic CPU mock. The runtime owns one completion-service
lane; CPU workers submit without waiting for completion, and the ABI has no
backend-to-runtime callback. The legacy detached-thread GPU stub remains
outside `rt::Runtime`. M9 adds a separate, optional CUDA Driver API backend
candidate around host-owned contexts/streams, pinned registered host spans,
device mirrors or external allocations, fixed kernel tokens, async operations,
event-query completion, and drain-before-release recovery. M10 adds a
fixed-capacity XDMA state machine plus an opt-in Linux character-device adapter
for one named Xilinx AXI-MM stack. Its blocking transfers run only on fixed
backend I/O workers, and timed-out work remains quarantined until the driver
call physically returns. No CUDA or XDMA hardware tuple and no Vulkan backend
is qualified. See [the device contract](device_backend.md),
[the CUDA contract](cuda_backend.md),
[the XDMA contract](xdma_backend.md), and
[ADR-0003](adr/0003-device-backend-boundary.md).

## Real-time tiers

| Tier | Name | Contract |
| --- | --- | --- |
| RT0 | Portable functional | Correctness and bounded-capacity APIs; no latency claim |
| RT1 | Best-effort low latency | Tuned portable/Linux execution with measured distributions; no worst-case claim |
| RT2 | Qualified Linux PREEMPT_RT | Declared hardware/kernel/BIOS/driver/workload tuple passes strict preflight and published deadline tests |

Windows and stock desktop Linux can be RT0 or RT1. RT2 is never inferred from
successful compilation, thread priority requests, `mlockall`, or CI.

An RT2 qualification record must include:

- CPU, motherboard, BIOS/firmware, memory, and relevant PCIe topology;
- kernel version/configuration and boot parameters;
- driver and device firmware versions;
- CPU isolation, IRQ placement, affinity, scheduling policy, and power policy;
- runtime build, complete resolved configuration, and workload identifier;
- warm-up and measurement duration;
- raw release, wake-up, compute, completion, slack, and miss samples;
- thresholds chosen before the run and the final pass/fail result.

Release 1.2 has a strict, read-only Linux prerequisite preflight. It fails
closed on missing PREEMPT_RT, lock coverage, isolated affinity, realtime
scheduling, or absolute clock support, but it does not validate the full
deployment record or measured deadlines. No RT2 qualification exists in 1.2.

## Determinism tiers

| Tier | Contract |
| --- | --- |
| D0 — Unspecified | No bitwise promise; normal floating-point and scheduling behavior |
| D1 — Schedule-independent | Same binary, ISA, config, seed, and inputs produce identical registered state across supported worker counts |
| D2 — Reproducible build profile | D1 plus pinned compiler, flags, dependencies, and hardware class |
| D3 — Portable deterministic | Only approved fixed-point or explicitly specified math kernels; arbitrary floating-point code is excluded |

Release 1.2 supports D0 and an explicit D1 contract for registered canonical
state. D1 tests cover 1, 2, and 4 workers and exchange one integer-only
checkpoint artifact between GCC/Clang and FMA-on/FMA-off CI jobs. That narrow
artifact comparison does not establish D2 for arbitrary callbacks or
floating-point kernels, and D2/D3 configuration is rejected. The legacy
`SimCore` FMA control is process-global and only affects explicit `rt::fma`
calls; the target runtime's instance-local helper does not constrain arbitrary
callback expressions.

## Current support matrix

| Surface | 1.2 status | Notes |
| --- | --- | --- |
| `rt::Runtime` lifecycle | Implemented RT0 surface | Strict configure/finalize/start/step/stop state machine with frozen topology and recoverable, fail-closed device cleanup |
| Compiled graph | Implemented RT0 surface | Deterministic topological order; invalid/foreign handles, cycles, and unordered conflicting resource access fail before start |
| Host-driven callbacks | Implemented RT0 surface | Synchronous host wait; dependency-ready callbacks may overlap without step-time pacing or worker creation |
| Unified CPU executor | Implemented RT0 surface | Static assignments, bounded local-queue throughput, and a borrowed host job-system adapter share one graph/range/reduction representation |
| Finalized memory plan | Implemented RT0 surface | Budgeted runtime/device control, queues, aligned phase/task scratch, trace, outstanding slots, and completion batches; explicit overload results |
| CPU/memory policy and resident backing | M15 complete | Stable role/region identities, exact logical control ledger, live runtime-stack aggregation, declared-only opaque accounting, three-region provider transaction, and retryable reverse cleanup; no hardware, latency, RT1, or RT2 claim |
| Rate-domain reference plan | M16-01 implemented; external gates pending | Bounded copied domains and ownership, exact epoch-zero supercycle, immutable inspection, identity/accounting integration, and unchanged complete-graph dispatch; later M16 semantics remain incomplete |
| Self-paced time | Implemented RT0 surface | Finite absolute-release loop with no epoch drift, explicit deadlines, and per-frame timing results |
| Frame watchdog/degradation | Implemented RT0 surface | One-shot event per arm; service lane never invokes host code and degradation is committed by the frame thread |
| Strict platform preflight | Implemented RT0 surface | Disabled by default; read-only Linux prerequisite checks fail closed with a fixed-capacity report |
| Versioned observability | Implemented RT0 surface | Schema-v2 fixed trace records and 32 metrics; bounded nonblocking emission, cursor loss/window semantics, provenance, stable C ABI v8 access, and non-RT JSON export |
| Determinism/checkpoint/replay | Implemented D0/D1 surface | Frozen canonical state registry, worker-count-independent D1 compatibility identity, transactional schema-v1 checkpoint restore, bounded input logs, and synchronous replay |
| Device ABI and mock | Implemented RT0 surface | Poll-only backend ABI v1, registered borrowed buffers, graph dependency release, stable failures, health/reset/shutdown, and deterministic fault injection |
| CUDA Driver API backend | Candidate; unqualified | Optional host-owned context/stream adapter with fixed registries, pinned host spans, async transfer/kernel operations, timeout quarantine, CPU-only tests, and raw-evidence tooling; support matrix has no qualified tuple |
| GCC/Clang C++20 build | Supported RT0 on named tuples | Ubuntu 22.04 GCC 11 and Clang 14 are required build/test/package gates |
| MSVC build | Supported RT0 on named tuple | Windows Server 2022 with MSVC v143 is a required build/test/package gate |
| `SimCore` phase/range API | Experimental | Graph cycles and nested phase/range concurrency require redesign |
| Bounded MPMC queue | Implemented primitive | `WorkerPool` uses one global FIFO queue |
| `WorkerPool` priority/work stealing | Experimental/misnamed | Priority is not honored by normal dequeue; "steal" telemetry is not a successful-steal count |
| Frame arenas | Experimental | Release overflow can fall back to heap allocation |
| Target runtime trace ring | Implemented RT0 surface | Fixed instance-local schema-v2 records with monotonic sequences, runtime-local timestamps, and explicit overwrite/drop accounting |
| Legacy binary trace ring | Experimental | `SimCore` retains process-global registration and separate event definitions outside the M6 schema |
| Metrics JSON | Experimental | Phase histograms are rolling windows, default capacity 120 |
| Legacy snapshot helpers | Experimental compatibility surface | Bounds checks prevent truncated reads and attacker-sized vector allocation, but native-layout `SimCore` snapshots are outside checkpoint schema v1 |
| C ABI and distribution | Stable ABI v8 / M11 complete | Exact symbol allowlist, header/library fingerprint handshake, ABI-numbered SONAME, C/C++ host adapter, package components, and relocated Linux/Windows consumers |
| Portable release contract | Supported RT0 / M12 complete | Named support tuples, same-major CMake compatibility, cross-instance device isolation, CPack archives, and content-addressed release manifests |
| JSON profiles/runtime autotune | Implemented RT0 host tooling | Complete resolved schema, bounded allocation-free transactional C++ parser, explicit runtime/schema compatibility, real target-runtime demo, and generated-profile CI round trip; no portable tuning or latency claim |
| Product SDK boundary | Supported / M14 complete | Preferred `rtfw::runtime`, exact public-header inventory, isolated optional adapters, compatibility target names, and no exported project warning/feature policy |
| GPU | CUDA candidate; no qualified tuple | Real Driver API adapter exists behind `RTFW_ENABLE_CUDA`; deterministic mock remains the portable gate and the legacy detached-thread stub is excluded |
| Xilinx XDMA AXI-MM | Candidate; unqualified | Portable bounded backend and opt-in Linux character-device adapter exist for one named stack; the support matrix has no qualified tuple |
| Portable hard real time | Non-goal | Only a named RT2 deployment can be qualified |

## Non-goals

- Portable hard-real-time behavior on general-purpose operating systems.
- Bounded completion time for arbitrary vendor GPU drivers.
- Bit-identical arbitrary floating-point user code across all platforms.
- Transparent optimization of arbitrary application data and kernels.
- Replacing GPU, FPGA, DMA, or operating-system drivers.
- Privileged system-wide policy mutation as a hidden library side effect.
- Hot-unloading a plugin while code, callbacks, or submissions can reference it.
- A built-in general-purpose physics engine.

## Release gates

The portable 1.2 contract is complete because the following gates are required:

1. C and C++ hosts can configure, finalize, start, execute real user callbacks,
   stop, and inspect typed errors.
2. Invalid/cyclic graphs are rejected before running.
3. One executor safely runs independent phases with nested range/reduction
   work.
4. Queue saturation returns a bounded result and never creates an emergency
   thread.
5. A complete finalized CPU frame performs zero heap allocation, hidden thread
   creation, file I/O, or blocking lock on declared RT lanes.
6. Host-driven steps never sleep; self-paced releases use absolute deadlines.
7. Clock, trace, numerical policy, provider tokens/backing, reports, rollback,
   and allocator state are isolated per runtime.
8. Telemetry definitions and schemas are versioned and invariant-tested.
9. D1 passes representative dependency/reduction workloads.
10. The mock device backend passes saturation, timeout, loss, reset, and
    shutdown tests.
11. Snapshots are bounds-checked, versioned, and fuzzed.
12. Installed CMake packages work from a clean external consumer.
13. Two runtimes concurrently using independent device backends and borrowed
    buffers cannot share device health, completion, telemetry, or shutdown
    state.
14. Named support tuples, stable surfaces, package archives, and every archive
    digest are machine-checked by the release contract.

RT2, CUDA, and XDMA qualification gates are separate from portable 1.2.
The exact supported tuples and change rules are in the
[portable support matrix](portable_support_matrix.json) and
[release policy](release_policy.md).

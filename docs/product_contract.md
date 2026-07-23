# RTFW Product Contract

Status: accepted target contract for the pre-1.0 development line

Current release: 0.2.0 experimental

## Product definition

RTFW is intended to become a deterministic, bounded-resource C++20 simulation
execution runtime. A host defines a phase and resource graph, finalizes it
before execution, and runs it on a preallocated CPU executor. The target
runtime supports host-driven and self-paced frames and integrates asynchronous
device backends through bounded submission and completion interfaces.

The current 0.2 implementation is a research prototype. Its new host runtime
satisfies the M1 lifecycle and callback contract at RT0, alongside useful
primitives and experiments, but it does not yet satisfy the complete contract
in this document.

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

The 1.0 lifecycle is:

1. **Configure** — parse configuration, register phases, resources, callbacks,
   executor policy, and device backends. Ordinary allocation is allowed.
2. **Finalize** — validate and freeze the graph, intern telemetry identifiers,
   calculate memory and queue capacities, and allocate bounded storage.
3. **Start** — create the fixed thread set, establish execution contexts,
   register device buffers, warm memory, and run platform preflight checks.
4. **Run** — execute host-driven steps or a self-paced loop without graph
   mutation, hidden thread creation, or unbounded submission on an RT lane.
5. **Stop** — reject new submissions, drain or cancel according to policy,
   stop service lanes, and release resources.

`rt::Runtime` now enforces the M1 configure/finalize/start/step/stop state
machine and freezes callback registration at finalization. The legacy
`SimCore` constructor/run/destructor path remains experimental and does not
inherit that lifecycle.

### Time ownership

Host-driven time is the default embedding contract:

- `step(frame_context)` receives frame index, simulation delta, and an optional
  deadline from the host.
- A host-driven step does not sleep or advance a hidden wall clock.
- A separate self-paced mode owns an absolute periodic release schedule.

The 0.2 `rt::Runtime` implements the host-driven half: the host supplies frame
index, delta, and optional deadline, and callbacks execute synchronously without
runtime pacing. Self-paced absolute releases remain milestone M5.

See [ADR-0002](adr/0002-host-driven-time.md).

### CPU execution

The target has one executor boundary and one task representation. Executor
policies may differ:

| Policy | Intended behavior |
| --- | --- |
| `static_deterministic` | Precomputed worker/chunk assignment for reproducibility and low scheduling variance |
| `bounded_throughput` | Fixed-capacity local queues and documented, bounded steal attempts |
| `host_adapter` | Execution through a host job-system contract with explicit scratch and completion contexts |
| `periodic_rt` | Static execution plus qualified absolute release/deadline control |

The current `SimCore` workers, optional `WorkerPool`, `rt::Scheduler`, and
`FiberPool` are separate experimental paths. They are not the target executor.
See [ADR-0001](adr/0001-one-executor-boundary.md).

### Memory and overload

The target running state has:

- a finalization-time memory and queue-capacity plan;
- explicit scratch storage for every execution context;
- bounded, nonblocking RT-lane submission;
- no hidden heap fallback, thread creation, file I/O, or blocking mutex on an
  RT lane;
- configured behavior for capacity exhaustion: reject, drop optional work,
  fail the frame, or terminate.

The current frame arena and telemetry paths do not yet satisfy this contract.

### Device execution

Devices are optional backends outside the core scheduler. A backend contract
must define capabilities, buffer registration, bounded submission, completion,
timeout, cancellation where supported, health, reset, and shutdown.

The current GPU implementation is a CPU mock that launches detached threads.
There is no CUDA, Vulkan, or XDMA backend in 0.2. See
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

No RT2 qualification exists in 0.2.

## Determinism tiers

| Tier | Contract |
| --- | --- |
| D0 — Unspecified | No bitwise promise; normal floating-point and scheduling behavior |
| D1 — Schedule-independent | Same binary, ISA, config, seed, and inputs produce identical registered state across supported worker counts |
| D2 — Reproducible build profile | D1 plus pinned compiler, flags, dependencies, and hardware class |
| D3 — Portable deterministic | Only approved fixed-point or explicitly specified math kernels; arbitrary floating-point code is excluded |

Current tests exercise parts of D1 within one build. They do not establish D2
across GCC and Clang or D3 across machines. The legacy `SimCore` FMA control is
process-global and only affects explicit `rt::fma` calls; the M1 runtime's
instance-local helper does not constrain arbitrary callback expressions.

## Current support matrix

| Surface | 0.2 status | Notes |
| --- | --- | --- |
| `rt::Runtime` lifecycle | Implemented RT0 surface | Strict configure/finalize/start/step/stop state machine with frozen registration |
| Host-driven callbacks | Implemented RT0 surface | Synchronous registration-order execution; no sleeping or worker creation |
| GCC/Clang C++20 build | Experimental | Linux CI covers selected compiler/build combinations |
| MSVC build | Experimental | Windows CI is a portability check, not an RT qualification |
| `SimCore` phase/range API | Experimental | Graph cycles and nested phase/range concurrency require redesign |
| Bounded MPMC queue | Implemented primitive | `WorkerPool` uses one global FIFO queue |
| `WorkerPool` priority/work stealing | Experimental/misnamed | Priority is not honored by normal dequeue; "steal" telemetry is not a successful-steal count |
| Frame arenas | Experimental | Release overflow can fall back to heap allocation |
| M1 runtime trace ring | Implemented RT0 surface | Fixed instance-local lifecycle/callback ring with runtime-local timestamps |
| Legacy binary trace ring | Experimental | Fixed storage exists; schema/time-domain integration is not final |
| Metrics JSON | Experimental | Phase histograms are rolling windows, default capacity 120 |
| Snapshot helpers | Experimental | Input validation and stable interchange compatibility are incomplete |
| C ABI | Experimental M1 surface | Size/version-checked configuration and frame types plus callback lifecycle; ABI freezes at M11 |
| JSON profiles/runtime autotune | Planned integration | Generators exist; `rtfw_demo` does not load their output |
| GPU | CPU mock only | Detached-thread stub; no hardware backend |
| XDMA | Planned | No implementation |
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

The portable 1.0 contract is complete only when:

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
7. Clock, trace, numerical policy, and allocator state are isolated per runtime.
8. Telemetry definitions and schemas are versioned and invariant-tested.
9. D1 passes representative dependency/reduction workloads.
10. The mock device backend passes saturation, timeout, loss, reset, and
    shutdown tests.
11. Snapshots are bounds-checked, versioned, and fuzzed.
12. Installed CMake packages work from a clean external consumer.

RT2, CUDA, and XDMA qualification gates are separate from portable 1.0.

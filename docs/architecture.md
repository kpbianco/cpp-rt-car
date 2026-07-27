# Architecture

This page separates the 0.9 implementation from the accepted target
architecture. The normative target is the
[product contract](product_contract.md); the decisions behind it are recorded
in [ADRs](adr/README.md).

## Current 0.9 implementation

### M1–M8 host runtime

`rt::Runtime` is the first target-path component. It owns a strict
configure/finalize/start/step/stop state machine, a finalization-time graph
compiler, frozen phase/resource topology, a finalized aligned scratch/queue/
trace memory plan, a runtime-local clock, an explicit numerical helper policy,
one fixed CPU worker team, optional watchdog/degradation state, and a
fixed-capacity platform-preflight report.

M6 replaces the target path's shared trace lock with fixed atomic telemetry
slots. It adds stable event/metric IDs, cumulative and caller-cursor interval
windows, exact trace-sequence loss reporting, and immutable
version/build/config/workload metadata. Export is explicitly a non-RT host
operation; the runtime creates no exporter thread and performs no output I/O.

M7 adds an explicit D0/D1 determinism contract, caller-owned canonical state
registration, bounded versioned checkpoint and input-log artifacts, validated
transactional restore, and synchronous replay. Artifact parsing and runtime
identity checks allocate nothing. D2 reproducible-build and D3 portable
determinism remain unsupported and fail configuration rather than silently
weakening the requested tier.

M8 adds a C-compatible backend ABI, borrowed registered buffers, graph device
phases, a preallocated outstanding table, and one runtime-owned completion
service lane. A command provider runs on the CPU team and returns after bounded
submission. Its graph token remains pending until the poll lane publishes a
completion, so dependent phases are released without parking a compute worker.
The ABI has no completion callback, and stop joins the service lane before
unregistering buffers and shutting down backends. The included deterministic
CPU mock injects saturation, delay, timeout, error, and loss/reset behavior;
it is not a hardware backend.

Host-driven `step()` receives frame index, simulation delta, and an optional
deadline. It waits synchronously without pacing while dependency-ready phases
run on either a static-assignment or bounded-throughput policy. Finalization
rejects invalid/foreign handles, cycles, unordered conflicting resource access,
an undersized graph queue plan, task-scratch underprovisioning, and a plan over
the configured memory budget. Nested ranges and deterministic-tree reductions
use the same executor.

Finite self-paced `run_periodic()` uses absolute epoch-based releases on the
calling frame thread and reports release, wake, start, finish, slack, deadline
miss, watchdog, and degradation data. A configured watchdog has one service
lane but never invokes host code; the frame thread applies degradation after
the graph returns. Optional strict Linux preflight is read-only and runs before
runtime threads start. See the
[time/platform contract](time_platform.md).

The experimental C ABI mirrors this lifecycle with size/version-checked
structures and encoded graph handles. See the
[host runtime contract](host_runtime.md) and
[compiled graph contract](compiled_graph.md), the
[executor contract](executor.md), and the
[memory-plan contract](memory_plan.md), the
[observability contract](observability.md), and the
[determinism/replay contract](determinism_replay.md), and the
[device backend contract](device_backend.md).

### Legacy simulation path

`SimCore` owns a phase graph, an internal range-worker team, frame arenas,
metrics hooks, tracing, pacing, adaptation, snapshots, and a separate
`FiberPool`. It creates worker threads in its constructor and `run()` owns a
self-paced loop.

Phases are grouped into topological levels. Dependencies order those levels,
while enabled phases in one level may run concurrently. A phase can contain:

- serial callbacks;
- chunked range callbacks;
- ordinary reductions;
- deterministic range reductions.

The demo exercises independent input, physics, AI, and audio work followed by
dependent constraint, integration, and telemetry phases.

### Important limitations

- `buildTopoLevels()` does not report a cycle as a configuration error.
- `SimCore`, `WorkerPool`, `rt::Scheduler`, and `FiberPool` remain compatibility
  experiments with different task and lifetime rules. `rt::Runtime` does not
  invoke them.
- `WorkerPool` has one bounded global FIFO queue. Its normal dequeue path does
  not honor `Job::priority`; its “steal” counter measures unsuccessful polling
  while work is outstanding, not successful cross-worker steals.
- `SimCore::run()` sleeps and advances its own wall-clock schedule, so it is
  not yet suitable as a host-driven engine step.
- construction and some running paths allocate, lock, perform file I/O, or
  create threads. The frame arena can use heap fallback after Release overflow.
- legacy global trace and floating-point state prevent clean isolation of
  multiple `SimCore` instances. The M1 `rt::Runtime` does not use those globals.
- the legacy HAL memory flags are no-ops, and its GPU path is a detached
  CPU-thread mock outside `rt::Runtime`.

These are tracked in the [roadmap](roadmap.md), not hidden behind feature
claims.

## Target architecture

The complete target lifecycle is configure, finalize, start, run, and stop:

```mermaid
flowchart TD
  A["Configure graph and backends"] --> B["Finalize and validate"]
  B --> C["Allocate bounded plan"]
  C --> D["Start fixed execution contexts"]
  D --> E["Step compiled graph"]
  E --> F["Stop and drain"]
```

Finalization compiles phase dependencies and resource hazards into an immutable
execution plan. One executor boundary runs CPU work under a selected policy.
Device backends receive bounded submissions and publish completions; CPU
workers do not block on device futures.

M1 implements the state machine and host-driven callback path. M2 compiles and
freezes dependency/resource topology. M3 creates the fixed team in `start()`
and routes graph and nested CPU work through it. M4 creates the finalized
memory plan, aligned execution-context scratch, and bounded frame-overload
policy. M5 adds the distinct absolute-release loop, one-shot watchdog with
frame-thread degradation, and fail-closed prerequisite reporting. M6 adds
bounded schema-v1 trace/counter emission and isolated non-RT export cursors.
M7 adds D1 schedule-independent identity, canonical registered state, bounded
checkpoint/input-log codecs, transactional restore, and synchronous replay.
M8 adds the poll-only device ABI, graph-held device completion tokens,
preallocated device-manager storage, and deterministic mock.
See the [determinism/replay contract](determinism_replay.md),
[device backend contract](device_backend.md),
[ADR-0001](adr/0001-one-executor-boundary.md),
[ADR-0002](adr/0002-host-driven-time.md), and
[ADR-0003](adr/0003-device-backend-boundary.md).

## Memory layout utilities

The repository includes SoA/AoSoA containers and SIMD-oriented kernels. They
are optional utilities, not a required application data model and not evidence
that arbitrary host data is automatically optimized.

## Code anchors

- M1–M8 host runtime: `rt::Runtime`; `rt/include/rt/runtime.hpp`,
  `rt/src/host_runtime.cpp`
- M2 graph compiler: `rt/src/compiled_graph.cpp`
- M3 executor: `rt/src/executor.cpp`
- M4 memory plan: `rt/src/aligned_storage.hpp`,
  `docs/memory_plan.md`
- M5 time/platform controls: `rt/src/watchdog_monitor.cpp`,
  `rt/src/native_platform_preflight.cpp`, `docs/time_platform.md`
- M6 observability: `rt/src/telemetry.cpp`,
  `rt/src/observability_export.cpp`, `docs/observability.md`
- M7 determinism/replay: `rt/src/snapshot_codec.cpp`,
  `docs/determinism_replay.md`
- M8 device ABI/manager/mock: `rt/include/rt/device_abi.h`,
  `rt/src/device_manager.cpp`, `rt/src/mock_device.cpp`,
  `docs/device_backend.md`
- Experimental C lifecycle ABI: `rt/include/rt/c_api.h`, `src/c_abi.cpp`
- Phase registration and graph: `SimCore::addPhase`,
  `SimCore::addDependency`, `SimCore::buildTopoLevels`;
  `include/simcore/SimCore.hpp`
- Internal range execution: `SimCore::initThreads`,
  `SimCore::executeFrame`; `include/simcore/SimCore.hpp`
- Optional global-queue pool: `WorkerPool`; `include/simcore/worker_pool.hpp`
- Separate research scheduler: `rt::Scheduler`; `rt/include/rt/scheduler.hpp`
- Data-layout utilities: `include/simcore/car_soa.hpp`,
  `include/simcore/soa/`

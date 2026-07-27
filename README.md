# RTFW — Experimental Simulation Runtime

[![CI](https://github.com/kpbianco/cpp-rt-car/actions/workflows/ci.yml/badge.svg)](https://github.com/kpbianco/cpp-rt-car/actions/workflows/ci.yml)
[![Documentation contract](https://github.com/kpbianco/cpp-rt-car/actions/workflows/docs-contract.yml/badge.svg)](https://github.com/kpbianco/cpp-rt-car/actions/workflows/docs-contract.yml)

RTFW is a C++20 research prototype for deterministic, bounded-resource
simulation execution. The repository explores phase graphs, parallel range
work, fixed-capacity queues, frame arenas, numerical controls, tracing, and
asynchronous device patterns.

> **Status: 0.10.0 experimental.** This release is not production-ready and has
> no hard-real-time, worst-case-latency, cross-platform bitwise-determinism,
> CUDA-hardware, or XDMA qualification. See the
> [product contract](docs/product_contract.md)
> and [roadmap](docs/roadmap.md) before integrating it.

## Current implementation

| Area | Status | What exists today |
| --- | --- | --- |
| Host runtime lifecycle | Implemented RT0 surface | `rt::Runtime` enforces configure/finalize/start/step/stop and freezes graph topology at finalization |
| Compiled graph | Implemented RT0 surface | C and C++ hosts declare phase dependencies and logical resource access; finalization rejects invalid handles, cycles, and unordered conflicts |
| Host-driven callbacks | Implemented RT0 surface | `step()` waits synchronously without pacing; dependency-ready callbacks may run concurrently on the fixed team |
| Unified CPU executor | Implemented RT0 surface | Static deterministic assignment and bounded local-queue throughput policies run graph, range, and fixed-tree reduction work |
| Nested CPU work | Implemented RT0 surface | C and C++ callbacks can submit synchronous range/reduction work through their callback-local task context |
| Target-path memory plan | Implemented RT0 surface | Finalization budgets aligned phase/task scratch, CPU/device queue/control storage, and the trace ring; post-start CPU/device-frame tests observe zero runtime heap allocation |
| Self-paced time | Implemented RT0 surface | A finite caller-thread loop uses absolute epoch-based releases and reports release/wake/start/finish/slack without drifting after late frames |
| Frame watchdog/degradation | Implemented RT0 surface | One arm produces at most one event; the service lane never invokes host code and the frame thread commits capped degradation for following frames |
| Strict platform preflight | Implemented RT0 surface | Disabled by default; read-only Linux prerequisite checks fail closed with a fixed-capacity report before runtime threads start |
| Target-path observability | Implemented RT0 surface | Schema-v2 fixed records and 32 metrics, bounded nonblocking emission, runtime-bound trace/metric cursors, explicit loss, provenance metadata, and non-RT JSON export |
| Target-path checkpoint/replay | Implemented D0/D1 surface | Canonical state registration, stable little-endian checkpoints/input logs, transactional restore, worker-count-independent D1 identity, and synchronous input replay |
| Target-path device ABI/mock | Implemented RT0 surface | Size/versioned poll-only backend ABI, registered buffers, nonblocking device phases, a runtime-owned completion lane, and deterministic fault-injectable CPU mock |
| CUDA Driver API backend | Candidate; not hardware-qualified | Optional caller-owned context/stream adapter with fixed event/buffer/kernel registries, pinned-host registration, async copies/kernel launch, timeout quarantine, fake-driver tests, and a raw-evidence tool |
| Legacy phase/range execution | Experimental | `SimCore` retains its separate phase, range, reduction, and pacing path for compatibility |
| Legacy `SimCore` graph | Experimental | Topological levels exist, but this path does not inherit the target runtime's cycle/resource validation |
| Legacy memory utilities | Experimental | Per-thread frame arenas and NUMA helpers remain outside the target plan; the Release arena overflow path can fall back to heap allocation |
| Numerics | Experimental | FTZ/DAZ setup, explicit FMA gating, fixed reductions, fixed point, and counter PRNG utilities |
| Legacy trace | Experimental | `SimCore` retains process-global per-thread binary rings and offline adapters outside the M6 contract |
| Legacy metrics | Experimental | Demo JSON uses mutex-backed counters and rolling phase histograms with a default 120-sample capacity |
| Legacy snapshots | Experimental compatibility surface | Demo/native-layout helpers are bounds checked but remain outside target checkpoint schema v1 |
| C ABI | Experimental ABI v7 surface | Size/version-checked lifecycle, graph, nested-work, memory, time/platform, observability, replay, and device calls use fixed-width discriminators and typed status codes |
| Runtime configuration | Implemented M1–M8 schema | Twenty-five strict typed keys include bounded execution/device capacities, time/platform, provenance, determinism, and artifact limits; unknown keys fail |
| Autotune/profile integration | Tooling prototype | Profile generators and synthetic smoke tests exist; `rtfw_demo` does not load JSON profiles |
| Legacy GPU stub | Experimental compatibility path | Detached CPU-thread stub outside `rt::Runtime`; superseded for new CUDA work by the separate M9 candidate |
| XDMA | Planned | No backend exists |

`rt::Runtime` does not use the legacy `WorkerPool`, `rt::Scheduler`, or
`FiberPool`. Those compatibility experiments retain different lifetime and
task rules and are not alternate policies of the M3 executor.

## Quick start

Prerequisites are CMake 3.20+, a C++20 compiler, and a recursive checkout of
the GoogleTest submodule when tests are enabled.

The commands below are executed verbatim by the documentation-contract CI job.

<!-- ci-verified: .github/workflows/docs-contract.yml -->
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DSIM_WERROR=OFF
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/rtfw_demo --threads 2 --metrics-json
```

The demo runs 3,000 frames at a nominal 1 kHz and emits one JSON snapshot at
exit. Successful execution is a functional smoke test, not an RT
qualification.

The CUDA candidate is opt-in and uses the Driver API, so the host owns CUDA
initialization, context, stream, module, and kernel lifetimes:

```bash
cmake -S . -B build-cuda -DRTFW_ENABLE_CUDA=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cuda --target sample_cuda_qualification --parallel 2
./build-cuda/samples/sample_cuda_qualification \
  --warmup 1000 --iterations 10000 > cuda-evidence.json
```

That command produces raw functional and latency-decomposition evidence. It
does not by itself add a tuple to the versioned support matrix or establish
real-time qualification.

## Demo command line

These are the options implemented by `src/main.cpp`. Unknown options are
rejected.

<!-- cli-options:start -->
| Option | Behavior |
| --- | --- |
| `--threads <n>` | Set a positive internal worker count |
| `--pin` | Request worker affinity using the current Linux implementation |
| `--metrics-json` | Emit counters and phase percentiles at exit without resetting |
| `--metrics-json-interval` | Emit metrics, then reset rolling histograms and resettable counter baselines |
| `--snapshot-out <file>` | Write the final demo snapshot |
| `--snapshot-in <file>` | Load a demo snapshot before running |
| `--version` | Print the repository/runtime version |
| `--help`, `-h` | Print command help |
<!-- cli-options:end -->

Phase percentiles are **not cumulative for the whole process**. They cover the
latest samples retained by a rolling histogram, whose default capacity is 120.
Several worker telemetry names are also provisional; see
[observability](docs/observability.md).

The demo does not implement `--config`, `--rt`, `--run`, `--duration`, or
`RTFW_PROFILE`. Typed configuration and M5 self-paced execution belong to the
embedding runtime; demo/profile integration remains planned.

## Embedding lifecycle

Release 0.10 provides the M1 lifecycle, M2 compiled graph, M3 executor, M4
memory closure, M5 time/platform controls, M6 observability, and M7
checkpoint/replay plus the M8 device ABI/mock in
`<rt/runtime.hpp>` and `<rt/c_api.h>`:

1. configure a typed runtime;
2. register callback/device phases, logical resources, canonical replay state,
   optional device backends, and borrowed device buffers while configuring;
3. declare phase dependencies and read/write resource access;
4. finalize to validate and compile the graph, reject an over-budget memory
   plan, allocate aligned phase/task scratch, trace, and fixed queue storage,
   and freeze topology and static assignments;
5. optionally require strict read-only platform preflight, then start the fixed
   worker team and configured watchdog/device service lanes;
6. submit host-owned frame index, simulation delta, and optional deadline to
   `step()`, or run a finite absolute-cadence loop with `run_periodic()`;
7. inspect per-frame timing, watchdog/degradation, and preflight results;
8. export versioned counters and trace records from a non-RT host lane;
9. write/restore bounded checkpoints or replay a validated input log from a
   non-RT host lane;
10. inspect/reset device health from a non-RT host lane when applicable;
11. stop.

`step()` remains synchronous to the host, but dependency-ready phases may run
concurrently. The static policy freezes worker placement; the throughput policy
uses local queues and bounded steals. Both use the same team for nested
`parallel_for()` and deterministic-tree `parallel_reduce()` calls. See the
[host runtime contract](docs/host_runtime.md), the
[compiled graph contract](docs/compiled_graph.md), the
[executor contract](docs/executor.md), the
[memory-plan contract](docs/memory_plan.md), the
[time/platform contract](docs/time_platform.md), the
[observability contract](docs/observability.md), the
[determinism/replay contract](docs/determinism_replay.md), the
[device backend contract](docs/device_backend.md), and the working
[C](samples/embed_c/mini_app.c) and
[C++](samples/embed_cpp/mini_app.cpp) examples plus the
[mock-device sample](samples/device_mock.cpp). The optional M9 CUDA candidate
has a separate [backend and qualification contract](docs/cuda_backend.md).

## Architecture direction

The target is a domain-neutral runtime rather than a car or physics engine:

1. A host configures phases, resources, callbacks, executor policy, and optional
   device backends.
2. Finalization validates and freezes the graph and creates a bounded memory
   and queue plan.
3. A unified executor runs the compiled graph.
4. Host-driven steps never sleep; finite self-paced execution uses a separate
   absolute-release mode.
5. Devices use a versioned backend interface and release dependent CPU work
   through completion events.

Accepted architecture decisions:

- [ADR-0001: one CPU executor boundary](docs/adr/0001-one-executor-boundary.md)
- [ADR-0002: host-driven time by default](docs/adr/0002-host-driven-time.md)
- [ADR-0003: bounded device backend ABI](docs/adr/0003-device-backend-boundary.md)

The M1–M8 host runtime now implements lifecycle, both explicit time modes,
compiled graph validation, the first two CPU policies, the bounded target-path
memory plan, watchdog/degradation, platform preflight, versioned
observability, bounded registered-state replay, and the bounded poll-only
device path. M9 adds an optional CUDA Driver API backend candidate without
changing the core device ABI or claiming a qualified deployment. The existing
`SimCore` demo and legacy scheduler components remain outside that target
path. The [architecture guide](docs/architecture.md) distinguishes current
and target paths.

## Real-time and determinism language

RTFW separates portable functionality from deployment qualification:

- **RT0:** portable functional behavior; no latency claim.
- **RT1:** best-effort low latency with measured distributions.
- **RT2:** a named Linux PREEMPT_RT hardware/kernel/driver/workload tuple that
  passes strict preflight and published deadline tests.

No RT2 record exists yet.

Determinism is tiered from D0 (unspecified) through D3 (portable approved
fixed-point/specified math). Release 0.10 supports D0 and an explicit D1
contract for registered canonical state. Its worker-count and compiler-artifact
fixtures do not prove D2, arbitrary floating-point identity, or cross-machine
D3 behavior. Definitions and evidence requirements are in the
[product contract](docs/product_contract.md) and
[determinism/replay contract](docs/determinism_replay.md).

## Repository map

| Path | Purpose |
| --- | --- |
| `include/simcore/` | Current phase runtime, queues, memory, trace, metrics, data-layout, and physics utilities |
| `rt/include/rt/`, `rt/src/` | M1–M8 host runtime plus the M9 CUDA candidate, graph compiler, unified executor, memory/time/platform/observability/replay/device controls, and experimental legacy scheduler/fiber/plugin components |
| `hal/`, `gpu/` | HAL and CPU-only device/frame-graph experiments |
| `api/` | Compatibility include for the pre-M1 C header path |
| `src/` | Demo, C shim, platform setup, metrics, and trace utility |
| `tests/` | GoogleTest, integration, and optional fuzz coverage |
| `tools/` | Trace, characterization, scaling, SBOM, and autotune tooling |
| `docs/` | Product contract, ADRs, roadmap, and component notes |

## Build options

The following top-level CMake options are implemented:

| Option | Default | Notes |
| --- | --- | --- |
| `ENABLE_LOG` | `ON` | Compile logging support |
| `ENABLE_PROF` | `ON` | Compile profiler support |
| `ENABLE_TESTS` | `ON` | Build the test targets |
| `ENABLE_RAPIDCHECK` | `OFF` | Add RapidCheck property tests when available |
| `SIM_WERROR` | `OFF` | Treat project warnings as errors; CI enables it explicitly |
| `SIM_ENABLE_AVX2` | `OFF` | Experimental global AVX2/FMA compilation; runtime dispatch is planned |
| `SIM_SANITIZERS` | empty | Semicolon-separated compatible sanitizer list |
| `SIM_ENABLE_LTO` | `OFF` | Enable interprocedural optimization |
| `SIM_LTO_THIN` | `OFF` | Request ThinLTO where supported |
| `SIM_PGO` | empty | Experimental `gen`/`use` PGO mode |
| `SIM_BUILD_FUZZERS` | `OFF` | Build the Clang job-queue and snapshot-parser libFuzzer harnesses |
| `RTFW_ENABLE_CUDA` | `OFF` | Build the CUDA Driver API adapter and hardware qualification executable; requires CUDAToolkit |
| `RTFW_BUILD_ID` | `rtfw-<version>` | Stable 1–63 character build identifier included in observability and checkpoint provenance |

Platform and optional dependency behavior is described in the
[build guide](docs/build_tooling.md).

## Tests and evidence

CI currently provides:

- Linux GCC/Clang Debug and RelWithDebInfo build/test matrices;
- selected FMA/AVX2 build variants;
- Windows portability builds;
- randomized graph/reference-order, cycle/resource validation, and first-frame
  topology-allocation tests;
- static-assignment, queue-saturation, nested range/reduction, throughput-steal,
  stress, and ThreadSanitizer executor gates;
- aligned phase/task-scratch, memory-budget, overload-policy, and multi-frame
  zero-allocation target-path gates;
- fake-clock absolute-release/deadline/no-drift tests, one-shot watchdog and
  frame-thread degradation tests, and fail-closed preflight tests;
- schema/provenance, interval-partition, trace-loss, instance-isolation,
  contended-emission, C ABI v7, JSON-export, and ThreadSanitizer observability
  gates;
- D1 equality across 1/2/4 workers, cross-worker checkpoint transfer,
  transactional corruption rejection, checkpoint/input replay, bounded
  mutation and libFuzzer parser gates, plus exchanged GCC/Clang/FMA artifacts;
- deterministic device saturation/delay/timeout/error/loss/reset/shutdown,
  dependency-release, no-allocation, dynamic C ABI, and sanitizer gates;
- CPU-only fake-driver CUDA queue, transfer, kernel, timeout-quarantine,
  recovery, shutdown, runtime-integration, no-allocation, and TSAN gates;
- an opt-in self-hosted NVIDIA workflow that emits raw per-stage CUDA
  submission/poll/completion samples without automatically creating a support
  claim;
- shared/static C and C++ compiled-graph samples plus dynamic C ABI loading;
- autotune mapping and synthetic autotune smoke;
- scaling artifact smoke;
- documentation/version/claim checks.

These jobs provide regression evidence, not real-time qualification. Missing
high-risk gates are tracked in [the roadmap](docs/roadmap.md).

## Experimental tooling

The autotune analysis pipeline is usable with its synthetic smoke target. Its
default production-style spec currently invokes command-line/configuration
interfaces that `rtfw_demo` does not implement, so it must not be used to claim
runtime tuning results. See [DOE/autotune status](docs/DOE_AUTOTUNE.md).

The platform hardening scripts make privileged, best-effort host changes. They
are deployment experiments, not library behavior and not evidence of RT2.
Review [real-time hardening](docs/real_time_hardening.md) before running them.

## Contributing

Read the [contributor guide](docs/contributor_guide.md), the
[product contract](docs/product_contract.md), and relevant ADRs before changing
public semantics. New present-tense feature claims need an implementation test;
latency or qualification claims need a named evidence procedure.

## License

Licensed under the [Apache License 2.0](LICENSE).

# cpp-rt-car — Realtime Simulation Runtime (DAG + Work-Steal + NUMA + Determinism + Trace)

A modular, production-grade realtime runtime for physics/simulation workloads. It guarantees bounded latency and reproducible outcomes by combining:

- **Phase-DAG frame loop** with **work-stealing** workers over a **bounded MPMC** queue  
- **AoSoA/SoA** data layout with SIMD-friendly kernels and parallel first-touch  
- **Per-thread NUMA-local arenas** for hot allocations  
- Strict **numerics & determinism** (FTZ/DAZ, fixed reductions, counter PRNG)  
- First-class **observability** (binary trace → Chrome, metrics JSON cumulative/interval)  
- Optional **async device offload** with **fences** & **fibers** (GPU stub today; real devices later)  
- **Platform RT hardening** (PREEMPT_RT-friendly init)  
- **C-ABI** for embedding in other apps

---

## Table of Contents

- [1. Quick Start](#1-quick-start)  
- [2. Build & Toolchain Matrix](#2-build--toolchain-matrix)  
- [3. Config & Profiles](#3-config--profiles)  
- [4. Running, CLI & Modes](#4-running-cli--modes)  
- [5. Architecture & Data Flow](#5-architecture--data-flow)  
- [6. Scheduling & Concurrency](#6-scheduling--concurrency)  
- [7. Memory, NUMA & Layout](#7-memory-numa--layout)  
- [8. Numerics & Determinism](#8-numerics--determinism)  
- [9. Async Devices (GPU Stub) & Fibers](#9-async-devices-gpu-stub--fibers)  
- [10. Observability: Traces & Metrics](#10-observability-traces--metrics)  
- [11. Logging & Backpressure](#11-logging--backpressure)  
- [12. Examples & Benches](#12-examples--benches)  
- [13. Adding Your Own Phase](#13-adding-your-own-phase)  
- [14. Testing & CI](#14-testing--ci)  
- [15. DOE / Autotune Workflow](#15-doe--autotune-workflow)  
- [16. C-ABI Embedding](#16-c-abi-embedding)  
- [17. Developer Modes (Sanitizers, Static, Profilers)](#17-developer-modes-sanitizers-static-profilers)  
- [18. Troubleshooting](#18-troubleshooting)  
- [19. Code Anchors (Where features live)](#19-code-anchors-where-features-live)  
- [20. License](#20-license)

---

## 1. Quick Start

```bash
# 1) Configure
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 2) Build everything (runtime, tests, tools)
cmake --build build -j

# 3) Run the demo app with robust defaults
RTFW_PROFILE=default_safe ./build/bin/rtfw_demo --rt --metrics-json

# 4) Export a Chrome trace (open in chrome://tracing or Speedscope)
./build/bin/trace_dump --input /tmp/rtfw.trace --output trace.json

# 5) Run all tests (unit + integration)
ctest --test-dir build --output-on-failure
```

**Profiles**:  
- `default_safe` (conservative, no surprises),  
- `default_fast` (more aggressive: SMT, hugepages for hot arrays, prefetch on known BW-bound phases).

---

## 2. Build & Toolchain Matrix

Supported compilers: **gcc** ≥ 11, **clang** ≥ 15, MSVC (experimental for some SIMD paths).  
Platforms: Linux (PREEMPT_RT friendly), Windows (limited RT gate).

Common presets:

```bash
# Release, assertions off
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release

# RelWithDebInfo (good for profilers)
cmake -S . -B build/relwithdebinfo -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Debug (assertions on)
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
```

**Optional flags** (via CMake or environment):
- `-DRTFW_DISABLE_EMERGENCY_SPAWN=ON` — enforce single scheduling surface (no emergency OS thread).  
- `-DRTFW_ENABLE_NUMA=ON` — enable NUMA binding/tests.  
- `-DRTFW_ENABLE_TRACING=ON` — binary trace ring.  
- `-DRTFW_DETERMINISM=ON` — determinism mode (e.g., FMA gate off).

---

## 3. Config & Profiles

Profiles live in `configs/` (JSON). Startup resolves:

1. Built-in defaults →  
2. Profile (`RTFW_PROFILE=default_safe|default_fast` or `--config path.json`) →  
3. Machine profile (if you later run autotune) →  
4. CLI/env overrides.

Key knobs (per profile):

- `threads: "physical" | "physical_plus_smt" | int`  
- `scheduler: { type:"work_steal", aging:true, steal_threshold:int }`  
- `chunking: { target_p90_us:int, min_items:int, max_items:int }`  
- `layout: { aosoa_block:int, align_bytes:64, pad_to_simd:true }`  
- `numerics: { ftz_daz:true, fma:"on|off|auto_no_when_deterministic" }`  
- `memory: { arena_per_thread_mb:int, huge_pages:bool, numa:"first_touch", pretouch:true }`  
- `prefetch: { enabled:bool, distance_bytes:int, phases:[...] }`  
- `governor: { target_util:0.90..0.98, hysteresis:0.02..0.03, degrade_ladder:[...] }`  
- `tracing: { bintrace:bool }`

---

## 4. Running, CLI & Modes

Common CLI flags (demo app):

```bash
# Real-time platform gate (mlockall, SCHED_FIFO, affinity, THP policy)
./build/bin/rtfw_demo --rt

# Dump metrics (cumulative) as one JSON line
./build/bin/rtfw_demo --metrics-json

# Dump metrics (interval mode) and reset counters/histograms
./build/bin/rtfw_demo --metrics-json-interval

# Select config (overrides profile)
./build/bin/rtfw_demo --config configs/default_safe.json

# Export trace to JSON via standalone tool
./build/bin/trace_dump --input /tmp/rtfw.trace --output trace.json
```

**Modes:**
- **Determinism**: build with `-DRTFW_DETERMINISM=ON` or profile sets `numerics.fma=auto_no_when_deterministic`.  
- **Tracing**: `tracing.bintrace=true` (profiles) or `-DRTFW_ENABLE_TRACING=ON`.  
- **Emergency path**: default **enabled** (rate-limited & traced). Disable at build- or run-time per profile.

---

## 5. Architecture & Data Flow

**Frame** (SimCore):

1. Platform RT init (optional)  
2. Arm **Watchdog**  
3. Traverse **DAG** → split phases into chunks (target p90 µs) → submit jobs to **MPMC**  
4. Workers run: **work-steal**, AoSoA kernels, NUMA-local arenas  
5. GPU stub: submit → **Fence** → **fiber** co_await (thread not blocked)  
6. **RateGovernor** decides rung (`None / DropVisuals / ReduceSubsteps / CoarsenPhase`)  
7. **Sleep to cadence** (CLOCK_MONOTONIC_RAW)  
8. **Metrics** update; **Trace** events already emitted in hot path

---

## 6. Scheduling & Concurrency

- **Bounded MPMC** ring (Vyukov) — single push/pop trace source (`EV_QueuePush/Pop`, with depth).  
- **WorkerPool** — per-thread deque + **work-stealing**; priorities (Hi/Normal/Low); **no raw threads** from phases.  
- **Emergency spawn** — rare, rate-limited, traced (`EV_EmergencySpawn` with {outstanding, priority, category}); disable-able.

---

## 7. Memory, NUMA & Layout

- **AoSoA** (blocks 256–512) → contiguous, SIMD-friendly tiles; pad tails to avoid masked epilogues.  
- **Per-thread arenas** bound to NUMA node; **parallel first-touch**; hugepage toggle.  
- **Debug assert** if a thread allocates scratch before binding its arena.

---

## 8. Numerics & Determinism

- **FTZ/DAZ** on; **FE_TONEAREST**; optional FMA gate (determinism).  
- **Deterministic reductions** — fixed binary tree (`simcore/deterministic_reduce.hpp`).  
- **Counter PRNG** — per chunk/entity, thread-order independent.  
- **Matrix tests** ensure bit-identity: 1 vs N threads, FMA on/off, gcc/clang.

---

## 9. Async Devices (GPU Stub) & Fibers

- **gpu_stub.submit([&]{ kernel(); }) → Fence**  
- In phase (fiber context): `co_await fence.ready()`  
- Overlap visible in trace (CPU & “GPU” spans concurrent). Same pattern for real CUDA/Vulkan via a HAL backend.

---

## 10. Observability: Traces & Metrics

### Trace (binary ring → Chrome/Speedscope)
- **Zero allocations** in hot path.  
- Events: phase/chunk begin/end, **queue push/pop** (depth), **steals**, **governor rung**, **watchdog trip**, **GPU fence wait**, **worker meta** (worker id & NUMA node).  
- Export:
  ```bash
  ./build/bin/trace_dump --input /tmp/rtfw.trace --output trace.json
  ```

### Metrics (JSON, single line)
- Per-phase **p50/p95/p99**, cumulative counters: `missed_frames`, `watchdog_trips`, `queue_max`, `steals_total`, `log_drops` (logger + trace), `emergency_spawns`, device wait times.
- Modes:
  ```bash
  # Cumulative (since process start)
  ./build/bin/rtfw_demo --metrics-json

  # Interval (delta since last call; resets counters/histograms)
  ./build/bin/rtfw_demo --metrics-json-interval
  ```

**Window semantics**: `--metrics-json` is cumulative; `--metrics-json-interval` resets counters/histograms after emission (per-window p50/p95/p99).

---

## 11. Logging & Backpressure

- Bounded async sinks; **token backpressure**; **drops counted**.  
- Never blocks the frame loop; drop spikes show up in metrics.

---

## 12. Examples & Benches

- **Particles on Plane** (AoSoA phase) — shows SIMD tiles + worker submission.  
- **AoSoA vs SoA Bench**:
  ```bash
  ./build/bin/bench_aosoa_vs_soa --sizes 1e3,1e4,1e5,1e6 --json
  ```

---

## 13. Adding Your Own Phase

1) **Data**: define components in SoA/AoSoA (alignas(64), padded to SIMD width).  
2) **Kernel**: implement tile kernel; no dynamic allocations; use per-thread **arena** for scratch.  
3) **DAG**: register phase with read/write sets so the scheduler knows dependencies.  
4) **Chunking**: choose `target_p90_us` (e.g., 100µs).  
5) **Submit**: use **WorkerPool** (no raw threads).  
6) **Trace**: call phase begin/end (or wrap with phase scope).  
7) **Tests**: unit (functional), integration (phase visible in metrics & trace).

---

## 14. Testing & CI

- **Unit**: queue correctness, numerics flags, deterministic reduction, logger backpressure, trace no-alloc.  
- **Integration**:  
  - **RT pipeline** (tight budget): rungs, steals, queue depth, GPU overlap, watchdog non-fatal.  
  - **NUMA integration**: arena binding + first-touch + page locality (skips on single-node).  
  - **Determinism matrix**: 1 vs N threads; FMA on/off; gcc/clang.  
  - **C-ABI dlopen**: shared lib loads & steps frames.

```bash
ctest --test-dir build -j --output-on-failure
```

---

## 15. DOE / Autotune Workflow

### ⚡ 60-second Quickstart

```bash
# assumes you already built ./build/bin/rtfw_demo
python3 tools/autotune/run_experiments.py \
  --spec tools/autotune/spec.yaml \
  --screen 32 \
  --replicates 3 \
  --local-iters 40 \
  --topk 5
```

The orchestration script screens the parameter space, runs local search, validates the best candidates, and writes the final machine profile. All metrics are gathered in **interval mode** (`--metrics-json-interval`) so every sample represents a fresh window; cumulative outputs (`--metrics-json`) are ignored by design to keep the statistics comparable.

### What lands where

- **Profiles** → `profiles/<cpu>-<os>.json` (drop-in machine profile for runtime startup).
- **Results log** → `results/experiments.jsonl` plus helpers such as `results/top_candidates.json` and `results/summary.json` for CI-friendly summaries.
- **Reports** → `reports/` contains Pareto front JSON/CSV and the best-scoring candidate breakdown for offline analysis.

### Want the long version?

See [`docs/DOE_AUTOTUNE.md`](docs/DOE_AUTOTUNE.md) for pre-flight checks, interpreting objectives, and extending the factor space.

---

## 16. C-ABI Embedding

```c
#include "api/cabi.h"

void* h = rtfw_create(/*frame_budget_ms=*/5.0);
for (int i=0;i<1000;++i) {
  if (!rtfw_step(h)) break;
}
rtfw_destroy(h);
```

Link against the built shared library (see CMake target `rtfw_shared`).

---

## 17. Developer Modes (Sanitizers, Static, Profilers)

**Sanitizers** (Debug or RelWithDebInfo builds recommended):

```bash
# Address + Undefined
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRTFW_ASAN=ON -DRTFW_UBSAN=ON

# Thread sanitizer (worker/lock-free testing)
cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRTFW_TSAN=ON
```

**Static analysis**:
```bash
cmake -S . -B build/clangtidy -DRTFW_CLANG_TIDY=ON
cmake --build build/clangtidy
```

**Profilers**:
- **Built-in trace** for *timeline & critical path*. Use `trace_dump` → Chrome/Speedscope.  
- **Linux perf** (optional): run the app, then `perf record -g ./build/bin/rtfw_demo ...`.  
- **PMU counters in trace** (optional extension): integrate via a tiny sampler; current repo focuses on timings & queue/steal dynamics.

---

## 18. Troubleshooting

- **Missing queue events** in trace → ensure `RTFW_ENABLE_TRACING=ON` and `tracing.bintrace=true`.  
- **Determinism test failures** → verify FTZ/DAZ on, FMA gating, use canonical deterministic reductions; re-run 1 vs N thread test.  
- **NUMA not improving** → make sure parallel first-touch ran; check `EV_WorkerMeta` (numa_node) and `numactl --hardware`.  
- **Logger stalls** → should never happen; if drops explode, increase sink capacity or lower log rate.  
- **Emergency spawns seen often** → your queue is chronically saturated; increase threads, adjust chunk target, or disable the path and tune priorities.

---

## 19. Code Anchors (Where features live)

**Core & Frame Loop**
- `include/simcore/SimCore.hpp` — frame orchestration, governor & watchdog integration  
- `include/simcore/rate_governor.hpp` — rung logic  
- `include/rt/watchdog.hpp` — per-frame guard  
- `include/simcore/highres_clock.hpp` — timebase

**Scheduling & Queues**
- `include/simcore/worker_pool.hpp` — workers, work-steal, priorities, emergency policy, worker meta trace  
- `include/simcore/job_queue.hpp` — bounded MPMC ring (+ queue push/pop trace)

**Data & Memory**
- `include/simcore/soa/aosoa.hpp` — AoSoA tiles, SIMD alignment  
- `include/simcore/frame_arena.hpp`, `include/simcore/rt_memory.hpp` — per-thread arenas, hugepages, first-touch  
- `include/rt/numa.hpp` — NUMA binding helpers

**Numerics & Determinism**
- `include/rt/numerics.hpp` — FTZ/DAZ, rounding  
- `include/simcore/deterministic_reduce.hpp` — fixed reductions  
- PRNG — `include/rt/prng.hpp`

**Observability**
- `include/simcore/bintrace.hpp` — binary trace ring (alloc-free), event IDs  
- `tools/trace_export.hpp`, `src/trace_dump.cpp` — Chrome/Speedscope exporter  
- `include/simcore/metrics.hpp` — histograms & counters; JSON cumulative/interval

**Async Devices & Fibers**
- `include/hal/gpu_stub.hpp` — submit → Fence → fiber await  
- `include/rt/fiber_pool.hpp` — cooperative awaits

**Platform RT Harden**
- `src/platform_init.cpp` — mlockall, SCHED_FIFO, THP policy, clock

**Logging**
- `include/simcore/logger.hpp` — bounded sinks, token backpressure, drop counts

**Embedding**
- `api/cabi.h`, `src/c_abi.cpp` — `rtfw_create/step/destroy`

**Docs**
- `docs/scheduler.md`, `docs/observability.md`, `docs/real_time_hardening.md`, etc., each with **Code anchors**.

---

## 20. License

(Your license here.)

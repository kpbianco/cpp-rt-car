# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M14 and M14.1 are merged; M15-01
and M15-02 provide the resource inventory and runtime-owned thread policy
transaction. The repository is connected to the `portfolio-control` assurance
workflow, and `contracts/active-batch.yaml` carries the current bounded
implementation contract.

## Read first

1. `AGENTS.md`
2. `contracts/repo-profile.yaml`
3. `contracts/profile-requirements.yaml`
4. `contracts/active-batch.yaml`
5. `.agents/skills/rtfw-assurance/SKILL.md`
6. `docs/CURRENT_STATE.md`
7. `docs/product_contract.md`
8. `docs/architecture.md`
9. `docs/roadmap.md`
10. `docs/cpu_memory_policy.md`
11. `docs/memory_plan.md`
12. `docs/time_platform.md`
13. `docs/c_abi.md`

Canonical control plane:
`kpbianco/portfolio-control/products/cpp-rt-car/` at revision
`32bce8800bd3c14518f336786d3ee01e2cdeae58`.

## Current bounded objective

M15-03 adds a copied size/versioned `MemoryProvider` with acquire, apply,
observe, rollback, and nonthrowing release callbacks. It can back exactly the
active phase-scratch, task-scratch, and trace-storage regions. Finalization
acquires those regions in stable order and constructs objects in their spans;
startup applies and independently observes policy before any runtime-owned
thread can commit; failure and stop reverse operations and token ownership in
the opposite order.

The default path preserves aligned-new allocation unless page rounding,
guards, or explicit huge pages require Linux `mmap`. Linux guards and usable
storage are base-page rounded, `MAP_HUGETLB` fallback must be explicit,
caller/prefault touches precede `mincore` residency observation, and `mlock`
success is never labeled lock readback, device pinning, or DMA pinning.
Locking-only native allocations are independently page-backed, and the runtime
rejects live token or allocation-extent aliases across runtime instances.

Provider callbacks are control-path-only and must not reenter the runtime. The
runtime copies the table but borrows `user_data` through final cleanup. Checked
stop is required before releasing provider state or borrowed memory. For a
provider-backed runtime, stop destroys trace/executor objects before releasing
their tokens; trace inspection is consequently unavailable after token
release. Destruction is only a best-effort fallback.

## Protected decisions

- C ABI v8 with 70 exports, SONAME 8, and device ABI v1 remain unchanged.
- Runtime configuration schema 7 and its 25 keys remain unchanged.
- `rtfw::runtime` remains isolated from experimental legacy components and
  consumer policy leakage.
- Portable defaults preserve 1.2.1 payload sizes, scratch alignment, trace
  wrap/loss semantics, zero-capacity behavior, and steady-state allocation
  boundaries.
- Borrowed registered state/device buffers and backend-owned storage are never
  mutated by this batch.
- Fragmented control storage, stack residency, exact external/backend
  accounting, and complete byte closure remain M15-04 work.
- CUDA/XDMA/HIL/field/latency/RT1/RT2/signing/release/deployment/production
  claims remain unqualified.
- Direct GPU-to-FPGA peer DMA is not assumed, and Apache-2.0 remains the sole
  license.

## Verification

```bash
./scripts/agent-verify.sh contract
./scripts/agent-verify.sh quick
cmake -S . -B build/agent-full -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TESTS=ON -DRTFW_BUILD_EXPERIMENTAL=ON -DENABLE_RAPIDCHECK=ON -DSIM_SANITIZERS= -DSIM_WERROR=ON
cmake --build build/agent-full --parallel 2
./build/agent-full/tests/simcore_tests --gtest_filter='MemoryPolicy.*:CpuMemoryPolicy.*:MemoryPlan.*:TraceNoAlloc.CompleteCpuFramesDoNotAllocate:TraceNoAlloc.StaticCompleteCpuFramesDoNotAllocate:TraceNoAlloc.CompleteDeviceFramesDoNotAllocate'
./scripts/agent-verify.sh full
```

GitHub CI and human review remain required. Do not infer physical, latency, or
qualification evidence from a local result.

## Completion output

Retain `docs/evidence/M15-03-<date>.md` with acceptance mapping, exact commands
and results, changed and preserved invariants, compatibility status, rollback,
residual risks, and all unperformed hardware and qualification validation.

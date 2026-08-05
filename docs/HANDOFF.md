# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M14 and M14.1 are merged; M15-01
through M15-03 are retained at the pinned baseline, and the current worktree
implements M15-04 accounting, stack, rollback, and compatibility closure.
`contracts/active-batch.yaml` is the bounded implementation contract.

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
12. `docs/host_runtime.md`
13. `docs/executor.md`
14. `docs/device_backend.md`
15. `docs/time_platform.md`
16. `docs/c_abi.md`

Canonical control plane:
`kpbianco/portfolio-control/products/cpp-rt-car/` at the revision recorded in
`contracts/repo-profile.yaml`.

## Current bounded objective

M15-04 retains every stable M15 accounting identity and adds append-only C++
closure metadata. Finalization validates actual constructed runtime, executor,
and device control extents against the existing `MemoryPlan`. Configuring-time
declarations may supply bounded logical cardinality/bytes for external or
backend-owned resources, but remain declared-only metadata and never authorize
mutation or qualification.

Runtime-owned executor, watchdog, and device-service lanes publish live native
stack/guard commitment and supported observation before the startup gate
commits. Stack cleanup is requested on the owning quiescent lane before join.
Device/backend, lane/stack, control, and selected-region ownership is reversed
in order; the first error is retained, unresolved ownership prevents provider
release, and checked stop retries only unresolved work.

The provider acquisition boundary remains exactly phase scratch, task scratch,
and trace storage. Fragmented controls, external stacks, borrowed buffers, and
backend storage are not provider-backed or mutated.

## Protected decisions

- C ABI v8 with 70 exports, SONAME 8, and device ABI v1 remain unchanged.
- Runtime configuration schema 7 and its 25 keys remain unchanged.
- Observability/artifact schemas, installed headers/targets, compatibility
  aliases, support matrices, version 1.2.1, and Apache-2.0 remain unchanged.
- `rtfw::runtime` remains isolated from experimental legacy components and
  consumer policy leakage.
- Portable defaults preserve payload sizes, trace behavior, determinism,
  lifecycle, and steady-state allocation boundaries.
- Declarations are trusted logical metadata; `mlock` is not independent lock
  readback or device/DMA pinning.
- CUDA/XDMA/HIL/field/latency/RT1/RT2/signing/release/deployment/production
  claims remain unqualified.

## Verification

Every local command in `contracts/active-batch.yaml`, ending with:

```bash
./scripts/agent-verify.sh full
```

passes on the named development host, along with an installed-package consumer
build and its 11 tests. Mandatory GitHub CI and human accounting, public-API,
lifetime/concurrency, native-call, rollback, and compatibility review remain
required. Do not infer physical, latency, or qualification evidence from local
results.

## Completion output

Retain `docs/evidence/M15-04-<date>.md` with acceptance mapping, exact commands
and results, changed and preserved invariants, compatibility status, rollback,
residual risks, and every unperformed hardware and qualification validation.
Do not mark M15 or CAP-M15 complete until external gates are retained.

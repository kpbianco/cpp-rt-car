# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M14 and M14.1 are merged. The
repository has now been connected to the `portfolio-control` assurance workflow.
M15-03 is the committed and published implementation baseline. M15-04 exact
accounting and compatibility closure is implemented and verified locally under
the active bounded contract, but is not committed or published.

## Read first

1. `AGENTS.md`
2. `contracts/active-batch.yaml`
3. `.agents/skills/rtfw-assurance/SKILL.md`
4. `docs/CURRENT_STATE.md`
5. `docs/product_contract.md`
6. `docs/architecture.md`
7. `docs/roadmap.md`
8. `docs/memory_plan.md`
9. `docs/time_platform.md`
10. `docs/c_abi.md`

Control plane:
`kpbianco/portfolio-control/products/cpp-rt-car/`

## Exact next objective

Review and, when explicitly authorized, commit and publish M15-04. After that,
plan the first M16 multi-rate batch separately; do not absorb M16 into the
active M15-04 contract.

## Protected decisions

- C ABI v8 / SONAME 8 and device ABI v1 remain unchanged.
- `rtfw::runtime` remains isolated from experimental legacy components and
  consumer policy leakage.
- Portable defaults preserve 1.2.1 runtime behavior.
- CUDA/XDMA/RT1/RT2 claims remain unqualified.
- Direct GPU-to-FPGA peer DMA is not assumed.
- Apache-2.0 remains the sole license.

## Verification

```bash
./scripts/agent-verify.sh contract
./scripts/agent-verify.sh quick
./scripts/agent-verify.sh full
```

GitHub CI is still required for the complete compiler/OS, sanitizer,
determinism, package, archive, and release-contract matrix.

## Completion output

The local completion record is `docs/evidence/M15-04-2026-08-01.md`. It records
exact equations, lifecycle transitions, acceptance mapping, commands,
compatibility status, residual risk, and all unperformed physical-RSS,
privileged, NUMA, huge-page, locked-memory, hardware, and RT validation.

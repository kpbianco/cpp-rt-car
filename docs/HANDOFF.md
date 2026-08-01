# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. M14 and M14.1 are merged. The
repository has now been connected to the `portfolio-control` assurance workflow.
M15-01 is the committed local baseline and M15-02 is the active bounded
implementation contract.

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

Implement M15-02: resolve supported thread fields against an injectable/native
provider, apply and read them back on each runtime-owned lane behind a startup
barrier, verify the external frame lane without mutation, and reverse partial
startup on a strict failure. Do not add memory providers, NUMA memory placement,
custom stack ownership, or stable-ABI surface.

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

Create `docs/evidence/M15-02-<date>.md` with acceptance mapping, commands,
results, changed invariants, compatibility status, residual risks, and all
unperformed privileged/hardware/RT validation.

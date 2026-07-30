# Current state

Last audited: 2026-07-29  
Audited repository state: `main@c5220de1ccfceca4d1584c3df5e0ddb17da171eb`

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8, SONAME 8, device ABI v1.
- License: Apache-2.0.
- M14 professional SDK/package boundary: complete.
- M14.1 recoverable device lifecycle: complete.
- Active milestone: M15 explicit CPU and memory policy.
- Active batch: `contracts/active-batch.yaml` (`M15-01`).

## Repository health

The audited baseline documents a supported `rtfw::runtime` target, exact default
SDK inventory, compatibility aliases, portable compiler/OS matrix, deterministic
and sanitizer coverage, package relocation, release contracts, candidate CUDA
and XDMA adapters, and recoverable device cleanup.

This harness change is infrastructure and documentation only. It does not rerun
the repository build matrix and does not change source, ABI, package, release,
or support claims. GitHub CI on the harness PR remains the authoritative
repository validation.

## Active objective

Define the additive C++ CPU/memory policy and report model, complete role/region
inventory, portable no-op resolution, and strict validation without applying
native OS policy or changing runtime steady-state behavior.

## Blockers and limitations

- M18 requires the exact production CPU, motherboard, GPU, FPGA, kernel,
  drivers, bitstream, PCIe/IOMMU topology, and workload.
- Local/hosted CI cannot establish hardware, RT1, RT2, thermal, or endurance
  qualification.
- Direct GPU-to-FPGA peer DMA is not available through the current official
  XDMA character-driver path.
- Exact controlled-hardware performance thresholds and signing identity remain
  future decisions.

## Next action

Implement `M15-01` on an isolated branch, add focused tests, run
`./scripts/agent-verify.sh full`, record evidence, and open a draft PR. Stop
before native policy application; that is M15-02/M15-03.

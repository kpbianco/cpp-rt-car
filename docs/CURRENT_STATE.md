# Current state

Last audited: 2026-08-04
Batch baseline: `b7e6ebcccef144f433950b66efb3866519de449b`

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8, SONAME 8, device ABI v1.
- License: Apache-2.0.
- M14 professional SDK/package boundary: complete.
- M14.1 recoverable device lifecycle: complete.
- Active milestone: M15 explicit CPU and memory policy.
- Active batch: `contracts/active-batch.yaml` (`M15-02`).

## Repository health

The audited baseline documents a supported `rtfw::runtime` target, exact default
SDK inventory, compatibility aliases, portable compiler/OS matrix, deterministic
and sanitizer coverage, package relocation, release contracts, candidate CUDA
and XDMA adapters, and recoverable device cleanup.

M15-02 applies and reads back Linux policy for runtime-owned executor,
watchdog, and device-service lanes through a startup barrier. Caller and
external lanes remain verify-only. Stable ABI, package, release, and support
claims are unchanged; mandatory GitHub CI remains authoritative.

## Active objective

Apply and verify per-role runtime-owned thread policy with truthful aggregate
reports, fail-closed startup, reverse rollback, wait-strategy behavior, and
preserved M14.1 cleanup recovery.

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

Complete M15-02 verification, retain `docs/evidence/M15-02-2026-08-04.md`, and
obtain mandatory CI plus human compatibility/concurrency review before merge.
M15 memory application/residency remains later work.

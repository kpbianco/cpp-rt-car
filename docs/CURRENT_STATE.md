# Current state

Last audited: 2026-08-01
Implementation baseline: `03088b37e4321ccf5bd5e9efa2624a6179b4adf9` (`M15-01`)

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8, SONAME 8, device ABI v1.
- License: Apache-2.0.
- M14 professional SDK/package boundary: complete.
- M14.1 recoverable device lifecycle: complete.
- Active milestone: M15 explicit CPU and memory policy.
- M15-01 policy model and exact inventory: complete locally.
- Active batch: `contracts/active-batch.yaml` (`M15-02`).

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

Apply and verify supported per-thread policy behind a startup transaction for
the frame, executor, watchdog, and device-service lanes. Preserve portable
fallback, external verify-only ownership, checked reverse rollback, stable
ABIs, and the RT0 claim boundary. Memory providers and custom stack ownership
remain M15-03.

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

Complete `M15-02` on its isolated branch, run focused and full verification,
record `docs/evidence/M15-02-<date>.md`, and retain the result for review. Do
not begin memory-provider, residency, or custom-stack work; that is M15-03.

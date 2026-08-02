# Current state

Last audited: 2026-08-01
Implementation baseline: `146aa38f433649a74e925c6f4ea908c30dbd4b1f` (`M15-02`)

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8, SONAME 8, device ABI v1.
- License: Apache-2.0.
- M14 professional SDK/package boundary: complete.
- M14.1 recoverable device lifecycle: complete.
- Active milestone: M15 explicit CPU and memory policy.
- M15-01 policy model and exact inventory: complete.
- M15-02 thread application and verification: complete and published on its
  scoped branch.
- M15-03 memory providers and region residency: implemented and verified
  locally on its scoped branch; not yet committed or published.
- Active batch: `contracts/active-batch.yaml` (`M15-03`).

## Repository health

The audited baseline documents a supported `rtfw::runtime` target, exact default
SDK inventory, compatibility aliases, portable compiler/OS matrix, deterministic
and sanitizer coverage, package relocation, release contracts, candidate CUDA
and XDMA adapters, and recoverable device cleanup.

The M15-03 implementation passed the repository contract, quick, and full
local gates plus focused stress, AddressSanitizer/UndefinedBehaviorSanitizer,
and ThreadSanitizer runs. Exact C ABI v8 remains 70 exports with fingerprint
`0xd0e7a5a14bf35f97`. Required GitHub compiler/OS, sanitizer, determinism, and
package CI has not run for this uncommitted branch and remains authoritative.

## Active objective

Retain the completed M15-03 implementation and evidence for review. The next
bounded milestone batch is M15-04 exact accounting and compatibility closure;
do not begin it until a separate active-batch contract is approved.

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

Review and, when explicitly authorized, commit and publish M15-03. Then plan
M15-04 exact accounting and compatibility closure as a separate bounded batch.

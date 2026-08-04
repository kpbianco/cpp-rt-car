# Current state

Last audited: 2026-08-04
Batch baseline: `088b84edcb3099743ed9aee1c2df2371e5618b7f`

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8, SONAME 8, device ABI v1.
- License: Apache-2.0.
- M14 professional SDK/package boundary: complete.
- M14.1 recoverable device lifecycle: complete.
- Active milestone: M15 explicit CPU and memory policy.
- Current implementation contract: `contracts/active-batch.yaml` (`M15-03`).

## Repository health

The baseline includes the M15-01 bounded CPU/memory policy inventory and the
M15-02 fail-closed Linux runtime-owned thread policy transaction. M15-03 adds
one copied, size/versioned, five-callback C++ memory-provider table and resident
backing for exactly phase scratch, task scratch, and trace storage. Finalization
acquires active regions in that order; startup applies and observes their
policy before the thread startup gate; failures roll back in reverse order; and
checked stop quiesces runtime lanes before rollback and reverse token release.
Every live allocation token and extent is registered across runtime instances,
so a malformed shared provider cannot alias either ownership or backing memory.

Without an injected provider, ordinary requests retain aligned-new allocation.
Linux page/guard/explicit-huge requests use process-local `mmap`, rounded
base-page guards, an explicit `MAP_HUGETLB` attempt with only the requested
fallback, caller/prefault page touches, `mlock` application, and `mincore`
residency observation. Locking-only requests also use independent page-backed
mappings so one runtime's page-level lock/unlock cannot affect another
allocation. `mlock` is neither independent lock readback nor provider/device/
DMA pinning.

Stable ABI, package, release, support, and qualification claims remain
unchanged. Local verification, mandatory GitHub CI, and human API,
ownership/concurrency, native-call, rollback, and compatibility review remain
merge gates; this document does not record those results.

## Current objective

Complete and verify the M15-03 three-region resident-memory transaction while
preserving default 1.2.1 behavior, reverse exactly-once ownership cleanup,
multi-runtime isolation, and truthful requested/resolved/acquired/applied/
verified reporting.

## Blockers and limitations

- M15-04 retains fragmented runtime/executor/device control allocation,
  runtime-owned stack residency, exact external/backend accounting, and full
  cross-category byte closure.
- Borrowed registered state, registered device buffers, external stacks, and
  backend-owned storage are not mutated or promoted by M15-03.
- M18 requires the exact production CPU, motherboard, GPU, FPGA, kernel,
  drivers, bitstream, PCIe/IOMMU topology, and workload.
- Local or hosted CI, injected providers, preflight, and one-host Linux
  observations cannot establish hardware, HIL, field, latency, RT1, RT2,
  thermal, endurance, signing, release, deployment, or production evidence.
- Direct GPU-to-FPGA peer DMA is not available through the current official
  XDMA character-driver path.

## Next action

Finish focused M15-03 validation, run the complete repository verification,
retain the batch evidence record, and obtain mandatory CI plus human review
before merge. Do not advance M15 accounting/residency claims beyond the three
selected backing regions until an approved later batch closes them.

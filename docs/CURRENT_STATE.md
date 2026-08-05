# Current state

Last audited: 2026-08-05
Batch baseline: `6d28d6d5eed7cd2c64cf2a0d362d36f59aceeeab`

## Product state

- Release: 1.2.1 portable RT0.
- Stable boundary: C ABI v8, SONAME 8, device ABI v1.
- License: Apache-2.0.
- M14 professional SDK/package boundary: complete.
- M14.1 recoverable device lifecycle: complete.
- Active milestone: M15 explicit CPU and memory policy.
- Current implementation contract: `contracts/active-batch.yaml` (`M15-04`).

## Repository health

The worktree contains the M15-04 implementation on the pinned M15-03 plus
live-guard baseline. It retains the twelve stable memory identities and the
six-row `MemoryPlan` equation while adding append-only C++ closure metadata,
bounded copied declarations for external/backend facts, exact logical control
extents, live runtime-owned stack accounting and observation, and status-
bearing owning-lane stack cleanup.

Finalization reconciles constructed runtime, executor, and device control
extents against the existing plan terms and rejects malformed, duplicate,
overlapping, overflowing, missing, or estimate-mismatched inventories before
publishing a report. Rows and aggregates distinguish exact, declared-only,
partial, unknown, and not-applicable logical facts. Declarations do not
authorize mutation or establish residency, locking, pinning, or qualification.

The configured memory provider remains limited to active phase scratch, task
scratch, and trace storage. Runtime-owned executor, watchdog, and
device-service stacks are observed only while live; supported stack policy
participates in the startup barrier. Cleanup preserves device/backend
ownership, then device-service, reverse executor instances, and watchdog stack
ownership before lower control and trace/task/phase rollback. Failed cleanup
retains ownership, prevents token release, and remains retryable through
checked stop.

Stable ABI, schema, package, release, support, and qualification claims remain
unchanged. The required deterministic local commands and a clean installed-
package consumer build pass on the named development host. Mandatory GitHub
CI and human accounting/API/lifetime/concurrency/native-call/compatibility
review remain completion gates; this document does not claim those external
gates passed.

## Current objective

Retain the completed M15-04 local evidence and obtain mandatory CI and human
review without broadening the provider, ABI, package, schema, or qualification
boundary.

## Blockers and limitations

- External/backend declarations are trusted logical metadata, not independent
  observation or qualification evidence.
- Fragmented controls are logically inventoried; allocator commitment and
  physical residency are not inferred from their extents.
- Borrowed registered state/device buffers and backend-owned storage remain
  unmodified.
- Named-host `mlock` application is not independent lock readback or
  device/DMA pinning.
- M18 still requires exact production CPU, motherboard, GPU, FPGA, kernel,
  drivers, bitstream, PCIe/IOMMU topology, and workload evidence.
- Local or hosted CI, injected declarations/providers, preflight, and one-host
  Linux observations cannot establish hardware, HIL, field, latency, RT1,
  RT2, thermal, endurance, signing, release, deployment, or production
  evidence.

## Next action

Submit the bounded changes for mandatory CI and human review. Do not mark M15
or CAP-M15 complete before those external gates are retained.

# Handoff

## Restart context

RTFW 1.2.1 is a portable RT0 C++20 runtime. The exact M17-02 baseline is
`bea813f08ea43c3415d3af1ff25a713affd423d4`; merged M17-01 is
`25ea1950d993bf7fcc170ccaafa32181d734ce4e`. M14, M14.1, M15, and M16 are
complete in target history. M17 is active and remains incomplete.
`contracts/active-batch.yaml` is binding.

Canonical control artifacts are under
`/home/kbianco/.local/share/portfolio-control/worktrees/control/cpp-rt-car/products/cpp-rt-car`
at revision `2206fb22691b3af0d9d6a39e582e0e9516a24c50`.

## Implemented boundary

M17-02 appends one optional memory/topology extension pointer to the additive
C++ HAL v2 backend registration. The extension has its own version and copied
function table and publishes fixed-capacity memory-domain, topology-node,
topology-link, and timestamp-domain records. Counts are bounded at 16, 32, 64,
and 8 respectively. The exact six domain kinds are host, pinned host, CUDA
device, imported, DMA mapped, and peer; no seventh category is inferred.

Native discovery is configuring-only, transactional, and fail closed. Core-only
native v2 and adapted-v1 backends receive the same implicit borrowed-host,
host-coherent, no-sync domain. The M17-01 HAL core table, complete v1 adapter,
legacy buffer surface, identifiers, statuses, call counts, and cleanup behavior
are preserved.

A new `register_device_buffer()` overload accepts a same-instance domain and
one host span or opaque handle, then validates size, backing, ownership,
access, coherency, synchronization, alignment, granularity, capacity, overlap,
and reserved bytes against the copied domain. Inspection methods expose
instance-owned domain/node/timestamp handles, links, completion-domain choice,
and memory-object facts without transferring ownership. Correlation queries
are running-state control operations and require a declared relationship.

The runtime registers native memory during start and unregisters it in reverse
order during checked stop. Ownership is pessimistically retained across any
failed or exceptional register/unregister call. Backend shutdown cannot pass
unresolved memory ownership; a later checked stop retries only unresolved
cleanup. No post-start dynamic allocation is introduced.

Copied extension state, snapshot records, memory specifications, native tokens,
and fixed translation storage are counted once in `device_control_bytes` and
the existing M15 device-control extent. The six `MemoryPlan` rows and three
provider regions remain unchanged. Adapted-v1/core-only legacy identity bytes
remain exact; native extension semantics and explicit heterogeneous-memory
declarations are hashed conditionally, excluding pointer and callback values.

The existing core submission path remains bounded. It accepts a heterogeneous
memory reference only when the object is device accessible and requires no
explicit synchronization. M17-02 adds no command-batch vocabulary, timeline
completion, vendor control, device-rate execution, callback from poll, or new
submission/I/O lane.

## Protected decisions

- Preserve C ABI v8, 70 exports/fingerprint, SONAME 8, every device-ABI-v1
  declaration/layout/value, and every M17-01 HAL core behavior.
- Preserve Runtime statuses, profile schema 7/25 keys, observability schema 2
  and IDs, checkpoint/input-log schema 1, and rate-action schema 1.
- Preserve installed headers/targets and aliases, support matrices, release
  1.2.1, and Apache-2.0.
- Keep borrowed storage and opaque handles externally owned unless the explicit
  domain ownership contract says backend-owned; never infer CUDA/XDMA peer DMA.
- Do not add batches, timelines, vendor facilities, plugins/factories, another
  lane, device-rate execution, or physical adapters in M17-02.
- Do not claim hardware, HIL, field, controlled latency, RT1, RT2, Unreal,
  signing, release, staging, deployment, or production evidence.

## Completion output

Run the exact local commands in `contracts/active-batch.yaml`, ending with
`./scripts/agent-verify.sh full`, C ABI verification, and the SONAME check.
Retain acceptance mapping, exact commands/results, storage and identity facts,
lifecycle and rollback behavior, residual risks, and all unperformed validation
in `docs/evidence/M17-02-2026-08-06.md`. Mandatory CI and human review remain
external gates. Do not commit, push, open a pull request, release, or deploy.

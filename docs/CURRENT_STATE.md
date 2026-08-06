# Current state

Last audited: 2026-08-06
Batch baseline: `bea813f08ea43c3415d3af1ff25a713affd423d4`

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1 and the M17-01 HAL v2 core contract are unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
- The installed header/target inventory and 1.x compatibility aliases remain
  unchanged. The license remains Apache-2.0.
- M14, M14.1, M15, and M16 are complete in merged default-branch history.
  M17-01 is merged at `25ea1950d993bf7fcc170ccaafa32181d734ce4e`.
- M17 is active and incomplete. The approved batch is M17-02, heterogeneous
  memory and topology contracts.

## M17-02 implementation

The installed C++ `rt/device.hpp` extends, rather than replaces, the M17-01
HAL v2 core. A separately size/version-checked extension table adds bounded
discovery, registration, unregistration, and timestamp-correlation operations.
It uses fixed-capacity records for up to 16 memory domains, 32 topology nodes,
64 directed topology links, and 8 timestamp domains. The exact memory-domain
taxonomy is host, pinned host, CUDA device, imported, DMA mapped, and peer.
Every record gives ownership, access, coherency, synchronization, alignment,
granularity, capacity, topology, or correlation semantics explicitly; reserved
tails, enums, counts, identities, references, and arithmetic fail closed.

`Runtime::register_device_backend()` copies and validates the optional
extension while configuring. A native extension must publish one complete,
self-consistent snapshot before the backend registration becomes visible.
Core-only HAL v2 and adapted device-ABI-v1 backends instead receive one
implicit borrowed-host, host-coherent, no-synchronization domain. The complete
device-ABI-v1 adapter and every M17-01 call, status, identity, and cleanup
behavior remain unchanged.

The legacy borrowed-host `DeviceBufferRegistration` path keeps its source and
runtime meaning. A new configuring-only overload accepts exactly one bounded
host span or opaque handle, a domain handle from the same runtime and backend,
declared bytes, ownership, access, coherency, and synchronization requirements.
The runtime validates those declarations against the discovered domain and
rejects foreign, stale, malformed, overlapping, misaligned, over-capacity, or
unsupported registrations before publication. Native registration tokens are
retained until reverse-order cleanup succeeds; failed cleanup remains
retryable and prevents backend shutdown.

The runtime exposes non-owning, instance-local inspection of discovered memory
domains, topology nodes and links, timestamp domains, the completion timestamp
domain, and registered memory objects. Running-state correlation queries are
bounded, fail closed on malformed or undeclared relationships, and publish no
partial output. These timestamps are backend-domain observations, not runtime
monotonic time and not new observability-schema fields.

All copied tables, snapshots, semantic records, handles, registrations, native
tokens, and fixed translation state are reserved before start and counted once
in `device_control_bytes` and the existing M15 logical device-control extent.
The six-row `MemoryPlan` equation and three provider-backed regions do not
change. Adapted-v1 and core-only legacy registrations retain their exact
M17-01 identity path; native extension semantics and explicit heterogeneous
registrations enter graph/replay identity conditionally. Pointer values,
callback addresses, and opaque tail bytes outside the declared size do not.

The existing canonical manager still owns submission and polling. M17-02 adds
no command batch, timeline completion, vendor control, new executor lane, or
device-rate execution. A memory reference can enter the existing core
submission only when the object is device accessible and requires no explicit
synchronization; later batches own synchronization commands and isolated
submission lanes.

## Boundary and remaining work

M17-02 establishes portable RT0 contract and synthetic functional behavior
only. M17 and CAP-M17 remain incomplete. M17-03 owns command batches, timeline
completions, and isolated submission lanes; M17-04 owns CUDA Graph and XDMA
control/MMIO/event facilities; M17-05 owns the combined sample. M18 owns named
physical hardware and RT qualification.

Local builds, mocks, fake drivers, fixtures, sanitizers, package consumers,
preflight, and documentation are not physical hardware, HIL, field, latency,
RT1, RT2, Unreal, signing, release, deployment, or production evidence.
Mandatory GitHub CI and human public-API, compatibility, lifetime, concurrency,
accounting, lifecycle, and claim-boundary review remain external merge gates.

## Next action

Complete every M17-02 local validation command, including
`./scripts/agent-verify.sh full`, and retain exact results in
`docs/evidence/M17-02-2026-08-06.md`. Repair only in-scope failures and submit
the bounded diff for external gates only when separately authorized. Do not
mark M17 or CAP-M17 complete.

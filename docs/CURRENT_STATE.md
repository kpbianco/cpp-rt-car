# Current state

Last audited: 2026-08-05
Batch baseline: `e264278557866acbf3bda1e6b4a436a6e0fd704f`

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1 is unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
- The installed header/target inventory and 1.x compatibility aliases remain
  unchanged. The license remains Apache-2.0.
- M14, M14.1, M15, and M16 are complete in merged default-branch history.
  The retained M16 evidence contains its original pre-merge gate language;
  merge history is authoritative, and no absent CI identifier, check count, or
  separate human-review object is inferred.
- M17 is active and incomplete. The approved batch is M17-01, HAL v2 core and
  device-ABI-v1 compatibility.

## M17-01 implementation

The already installed `rt/device.hpp` now contains an additive C++ HAL API
version 2 core contract. Distinct fixed-width records and a copied function
table cover capabilities, initialization, borrowed host-buffer registration,
one bounded core submission, bounded polling, cancellation, health, reset, and
shutdown. The core keeps the 64-byte identifier, 128-byte inline payload, and
eight-reference limits. Tables and records are size/version checked where
applicable and require zero reserved tails.

`Runtime::register_device_backend()` has a configuring-only overload for a
native `HalV2BackendRegistration`. It preserves the existing handle, naming,
capacity, freeze, graph-phase, health/reset, and borrowed-lifetime semantics.
The pre-M17 `DeviceBackendRegistration` aggregate and overload retain their
source meaning.

Every accepted device-ABI-v1 table is copied once into a Runtime-owned,
address-stable compatibility object. Capability discovery and every later
operation traverse that adapter into the same HAL v2 table path used by a
native backend. The device manager contains no direct v1 table call. The
adapter maps all core capability, initialization, buffer, submission,
completion, health, status, cancellation, reset, and shutdown fields, catches
callback exceptions, preserves `UNSUPPORTED`, and publishes no partial output
for malformed results.

The existing submission/early-ready handshake, runtime-assigned nonzero IDs,
logical-to-native buffer-token translation, range/access checks, completion
matching, service-lane-only polling, graph dependency release, reset gating,
and cross-instance isolation remain in the canonical manager. Completion
batches are validated as a whole before publication. Startup and checked stop
retain their existing ordering, reverse cleanup, first-error retention, and
unresolved-only retry behavior.

Adapter/table/context and translation-scratch storage is fixed before start,
owned by one Runtime, and counted exactly once in `device_control_bytes` and
the M15 device-control extent. Backend-private bytes remain informational.
The existing six-row MemoryPlan equation and three provider regions do not
change. Adapted-v1 backends take the exact legacy graph/replay hash path;
native HAL v2 registrations add a conditional kind/API-version marker.

The unchanged deterministic mock, CUDA candidate, and XDMA candidate continue
to register as device ABI v1 backends and therefore exercise the compatibility
path. Native-v2 and adapted-v1 functional, malformed-input, exception,
lifecycle, isolation, identity, accounting, package, and boundedness tests are
the local evidence boundary.

## Boundary and remaining work

M17-01 implements only the portable RT0 HAL v2 core envelope and v1
compatibility. M17 remains incomplete. M17-02 owns heterogeneous memory,
topology, coherency, and timestamp correlation; M17-03 owns command batches,
timeline completions, and isolated submission lanes; M17-04 owns CUDA Graph
and XDMA control/MMIO/event facilities; M17-05 owns the combined sample. M18
owns named physical hardware and RT qualification.

Local builds, mocks, fake drivers, fixtures, sanitizers, package consumers,
preflight, and documentation are not physical hardware, HIL, field, latency,
RT1, RT2, signing, release, deployment, or production evidence. Mandatory
GitHub CI and human public-API, compatibility-map, lifetime, concurrency,
accounting, lifecycle, and claim-boundary review remain external merge gates.

## Next action

Complete every M17-01 local validation command, retain exact results in
`docs/evidence/M17-01-2026-08-05.md`, and submit the bounded diff for external
gates only when separately authorized. Do not mark M17 or CAP-M17 complete.

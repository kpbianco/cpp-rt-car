# HAL v2 Core, Memory/Topology, and Device-ABI-v1 Compatibility Contract

M17-01 adds an additive C++ HAL v2 core contract to the already installed
`rt/device.hpp` header. The HAL API version is exactly 2. It is a source API:
applications must recompile, and RTFW makes no C++ binary ABI promise. Stable C
ABI v8, SONAME 8, and every declaration, value, and layout in
`rt/device_abi.h` version 1 remain unchanged.

M17-02 preserves that core and appends one optional, separately versioned C++
memory/topology extension pointer to `HalV2BackendRegistration`. The extension
adds bounded memory-domain, topology, timestamp-domain, native-memory, and
correlation contracts; it does not change HAL API version 2 or any M17-01 core
record/table behavior. Both batches establish portable RT0 functional behavior
only. M17 and CAP-M17 remain incomplete. Command batches, timeline
completions, isolated vendor submission lanes, CUDA Graph, XDMA
control/MMIO/event facilities, physical peer DMA, plugin or factory loading,
device-rate execution, and combined CPU/GPU/FPGA execution remain deferred.

## Public core shape

`rt::HalV2BackendApi` is copied during configuring. Its non-null `instance` is
borrowed until checked stop succeeds. The table contains `struct_size`, API
version 2, the instance, ten required operations, and an eight-word reserved
tail:

1. `get_capabilities`;
2. `initialize`;
3. `register_buffer`;
4. `unregister_buffer`;
5. `submit`;
6. `poll`;
7. `cancel`;
8. `get_health`;
9. `reset`;
10. `shutdown`.

Every operation is required even when a capability is unsupported. Such an
operation returns `HalV2Status::unsupported`; a null function does not encode
optional behavior. Operations must not throw. The runtime catches an exception
at either the native-v2 or adapted-v1 boundary and fails closed. A thrown
cleanup operation never establishes that ownership was released.

The core records use fixed-width fields and zero-initializing default
constructors. Records with an API field use version 2. Reserved bytes and words
must be zero. The core retains these existing limits:

- backend identifier capacity: 64 bytes including its terminator;
- inline submission payload: 128 bytes;
- buffer references per submission: eight.

Capability discovery reports only the backend identifier, maximum in-flight
work, maximum registered-buffer count, maximum buffer bytes, inline and
reference capacities, cancellation/reset support, deterministic-mock
declaration, and backend-owned control-storage bytes. Boolean capability bytes
are exactly zero or one. M17-01 does not infer any unreported v2 capability.

## Optional memory/topology extension

`HalV2MemoryTopologyExtension` has extension version 1, a borrowed non-null
instance, four required operations, and a zero reserved tail. It is copied
during configuring independently of the HAL core table:

1. `discover` publishes one complete `HalV2MemoryTopologySnapshot`;
2. `register_memory` converts one validated declaration into a nonzero private
   token;
3. `unregister_memory` releases that token;
4. `query_timestamp_correlation` samples one declared timestamp relation.

All four operations are required when the extension is present. Unsupported
runtime calls return `HalV2Status::unsupported`; a null function never denotes
optional behavior. Output records publish only after whole-record validation,
and callback exceptions fail closed. A failed or exceptional cleanup call does
not prove ownership release.

The copied snapshot has hard capacities of 16 memory domains, 32 topology
nodes, 64 directed links, and 8 timestamp domains. Identities are positive and
unique within their kind, references resolve within the same snapshot, counts
cannot exceed their public capacities, and reserved storage is zero.
The exact memory kinds are host, pinned host, CUDA device, imported, DMA
mapped, and peer. Ownership, host/device access, coherency, required
synchronization, maximum bytes, alignment, byte granularity, and opaque-handle
shape are explicit domain properties. The complete rules are in the
[heterogeneous-memory contract](heterogeneous_memory.md).

A core-only native v2 backend and every adapted-v1 backend expose one runtime
synthetic borrowed-host domain: identity 1, borrowed-host ownership,
host-coherent behavior, no synchronization requirement, byte/alignment
granularity 1, and the core capability's maximum buffer bytes. This synthetic
mapping is not a call into the backend and adds no topology or timestamp
assertion. The legacy buffer API continues through the unchanged core
`register_buffer`/`unregister_buffer` operations.

Initialization receives the runtime's requested in-flight and per-backend
registered-buffer counts. Buffer registration borrows one nonempty host span,
its stable name, byte count, and existing access flags and returns a nonzero
private token. One submission carries a runtime-assigned nonzero submission
ID, frame ID, positive timeout, opcode, zero flags, bounded payload, and up to
eight translated token/range/access references. Poll writes at most the
caller-supplied completion capacity and never invokes a callback.

A completion carries its exact submission ID, status, device timestamp, and
value. Health carries its complete state, last status, generation, submission,
completion, rejection, timeout, error, loss, cancellation, reset, and
outstanding counters. Malformed size/version/status/state/reserved output is
rejected without publishing a partial completion or health record.

## Registration and canonical path

`rt::HalV2BackendRegistration` and the corresponding
`Runtime::register_device_backend()` overload are additive beside the existing
`rt::DeviceBackendRegistration`. Both are configuring-only, copy their table
and name, enforce the same stable identifier, duplicate-name, configured
capacity, and freeze rules, and return the same instance-owned
`DeviceBackendHandle`. That handle continues to identify buffer registrations,
device phases, health, reset, and graph ownership. Existing positional
initialization of `DeviceBackendRegistration` retains its meaning.

Every accepted device-ABI-v1 table is copied into one runtime-owned,
address-stable compatibility adapter. The adapter exposes a HAL v2 table, and
the supported device manager stores and invokes HAL v2 records for both native
v2 and adapted v1 backends. Direct calls through `rtfw_device_backend_api` are
confined to the adapter. There is no parallel v1 manager path.

Adapter tables and contexts remain owned by one `Runtime`. Their self pointers
do not refer to relocatable configuring-vector elements, and later backend
registration cannot invalidate an already published context. Native tables
remain borrowed according to the same checked-stop lifetime rule. Backend,
buffer, outstanding, completion, and cleanup state is not shared across
runtime instances.

## Exact v1 translation

The adapter validates the complete frozen v1 table and all v1 outputs before
translation. It performs each requested operation once, without retry or
success promotion.

| HAL v2 core field or operation | Device ABI v1 mapping |
| --- | --- |
| API version | HAL version 2 is adapter-facing; the borrowed table and ABI-bearing v1 records retain `RTFW_DEVICE_ABI_VERSION` 1 |
| capabilities | Backend ID, all capacity limits, cancellation/reset booleans, deterministic-mock byte, and `control_storage_bytes` copy field for field |
| initialize | Requested in-flight and registered-buffer counts copy exactly |
| register buffer | Name, borrowed pointer, bytes, and access flags copy exactly; a successful token must be nonzero |
| unregister buffer | The translated private token and returned status copy exactly |
| submission | Submission/frame IDs, timeout, opcode, flags, payload size and all 128 payload bytes, reference count, and every token/access/offset/byte field copy exactly |
| poll | The adapter uses fixed completion scratch, preserves the bounded output count, and copies every status, ID, timestamp, and value |
| cancel | Submission ID and returned status copy exactly |
| health | State, last status, generation, and every counter copy exactly |
| reset and shutdown | Each result maps once; failure retains the existing ownership marker for checked retry |

The numeric v1 and HAL v2 status sets have the same meanings. The runtime map
remains:

| HAL v2 or device-v1 result | Runtime result |
| --- | --- |
| `ok` | `ok` |
| `invalid_argument` | `invalid_argument` |
| `invalid_state` | `invalid_state` |
| `queue_full` | `device_queue_full` |
| `timeout` | `device_timeout` |
| `error`, `unsupported`, `internal_error` | `device_error` |
| `lost` | `device_lost` |
| `canceled` | `device_canceled` |
| `resource_exhausted` | `resource_exhausted` |
| `reset_required` | `device_reset_required` |

An unknown status is an internal/device error, never success. In particular,
`unsupported` is not promoted to success. Invalid identifiers, enums,
booleans, capacities, ranges, records, output counts, tokens, reserved values,
or callback exceptions fail without publishing a partial backend, handle,
completion, health result, or ownership transition.

## Lifecycle and causal ordering

Capability discovery occurs while configuring. Finalization remains
transactional. Start initializes backends and registers buffers behind the
existing startup barrier, then starts the one existing runtime-owned device
service lane. Executor workers make the same bounded, nonblocking single-core
submission; only the service lane polls.

The existing outstanding-slot `submitting`/`early-ready` handshake remains the
causal boundary. A synchronous completion is retained until successful
submission accounting and the `device.submitted` event are visible. It cannot
release the wrong graph token, precede acceptance, or apply twice. Completion
matching remains by nonzero submission ID and backend, and independent graph
work can progress while a device phase is outstanding.

Checked stop quiesces graph and device work, unregisters buffers in reverse
order, and shuts backends down in reverse order. It retains the first failure
while continuing independent cleanup, clears only successful ownership
markers, and retries only unresolved operations. A backend is not shut down
while it still owns a registered buffer. Failed initialization remains
ownership-uncertain until shutdown returns success or the established
no-ownership result. The destructor is only a best-effort fallback; integrations
must use checked stop before releasing borrowed objects.

## Memory, identity, and observability

Adapter context, copied v1 table, HAL v2 table, and fixed translation scratch
are runtime-owned device controls. They are counted exactly once in
`MemoryPlan::device_control_bytes` and the M15 logical device-control extent.
The existing six-row plan equation and three provider-capable regions do not
change. Borrowed buffer bytes and backend-owned `control_storage_bytes` remain
informational and excluded from `planned_bytes`.

M17-02 also counts the copied extension table and snapshot, bounded semantic
records, heterogeneous registration specifications, native tokens, and fixed
translation state exactly once in `device_control_bytes`. Declared host spans
and opaque/device storage remain borrowed or backend-owned according to their
domain and are not converted into Runtime-owned payload bytes.

An adapted-v1 registration follows the exact pre-M17 graph/replay hash byte
path, preserving checkpoint and input-log compatibility for an otherwise
identical configuration. A native-v2 registration conditionally contributes
its backend kind and API version 2 to graph identity, so it cannot impersonate
an adapted v1 backend. Table/context addresses and control capacities are not
compatibility semantics. Checkpoint and input-log schemas remain version 1.

When a native extension exists, its semantic domain/topology/timestamp snapshot
and every explicit heterogeneous-memory declaration are appended
conditionally to graph/replay identity. Callback, instance, pointer, and
runtime-private token values are excluded. Core-only and adapted-v1 legacy
configurations retain their exact M17-01 identity bytes.

Native and adapted backends use the same existing observability meanings.
Global schema 2, trace IDs 1-14, metric IDs 0-31, producers, counters, and
cursor behavior remain unchanged. Adapter activity does not create another
telemetry schema or callback.

After successful start, submission, early and polled completion, health,
reset, checked cleanup, and complete CPU-plus-device frames use fixed storage.
They add no ordinary heap allocation, hidden thread, blocking mutex, unbounded
wait or retry, spill path, poll callback, or file/network I/O. M17-01 and
M17-02 create no new submission lane; potentially blocking vendor-operation
isolation belongs to M17-03. A heterogeneous object reaches the existing core
submission only if it is device accessible and requires no explicit
synchronization.

## Evidence and claim boundary

Portable tests may establish record validation, exact translation, native-v2
and adapted-v1 functional equivalence, malformed and injected-failure behavior,
causal ordering, cleanup retry, cross-instance isolation, source/package
compatibility, exact accounting, sanitizers, and steady-state allocation
freedom. Existing mock, CUDA fake-driver, and portable XDMA suites remain v1
compatibility evidence without modifying those backends.

Synthetic tests can establish the M17-02 memory/topology record contract,
registration and cleanup behavior, bounded correlation validation, identity,
accounting, compatibility, and cross-instance isolation. They do not establish
physical CUDA/XDMA allocation, coherency, synchronization, topology, peer DMA,
device-clock behavior, command batching, timeline completion, isolated
blocking-vendor execution, CUDA Graph, XDMA controls, HIL, field performance,
worst-case latency, RT1, RT2, signing, release, deployment, or production
readiness. Mocks, fixtures, hosted CI, preflight, and documentation remain
non-physical evidence. M17 and CAP-M17 stay incomplete; later M17 batches and
named M18 qualification evidence own those claims.

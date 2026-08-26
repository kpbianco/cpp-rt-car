# Heterogeneous Memory and Topology Contract

M21-01 reuses the copied memory-domain and timestamp-domain snapshot when
validating device-rate declarations. Every retained payload reference names an
existing same-backend logical buffer slice with checked bounds, access, byte
granularity, and offset granularity. The compiled report retains only logical
handles and integral descriptors; it never retains host addresses, opaque
native handles, or payload bytes. No transfer or coherency operation is added
by this batch.

M17-02 adds the optional HAL-v2 memory/topology extension version 1 beside the
unchanged HAL-v2 core API version 2. The extension is a C++ source contract in
the already installed `<rt/device.hpp>` header. It adds no installed header,
package target, C symbol, C status, or device-ABI-v1 declaration, and it carries
no C++ binary ABI promise.

This is portable RT0 functional contract evidence only. It does not establish
physical pinning, CUDA device allocation, imported-handle interoperability, DMA
mapping, peer reachability, real PCIe/NUMA topology, timestamp accuracy,
latency, HIL, RT1, or RT2 behavior.

## Public bounds and records

`hal_v2_memory_topology_extension_version` is exactly 1. One backend snapshot
has fixed public capacities:

- 16 memory domains;
- 32 topology nodes;
- 64 directed topology links;
- eight timestamp domains; and
- 64 opaque-handle bytes in each `HalV2OpaqueHandle`.

`HalV2MemoryTopologyExtension` contains a borrowed instance and required
discovery, memory-register, memory-unregister, and timestamp-correlation
operations. `HalV2BackendRegistration::memory_topology` is appended after the
M17-01 aggregate prefix. A null pointer selects the exact core-only path. When
present, Runtime copies the extension table and a completely validated
`HalV2MemoryTopologySnapshot` before publishing the backend handle. Callback
output and the registration pointer are borrowed only for their calls.

The extension records use fixed-width fields, default initialization, explicit
`struct_size`, extension version 1 where present, and zero reserved tails. A callback may
populate only the fixed caller-provided snapshot or result. It cannot return a
pointer-backed array or transfer ownership through discovery. A count above a
public capacity is rejected rather than truncated.

## Memory-domain model

`HalV2MemoryDomainKind` has six explicit values:

| Kind | Required truthful backing property |
| --- | --- |
| `host` | Host-addressable memory with borrowed-host ownership support |
| `pinned_host` | Host-addressable memory whose backend declares a distinct pinned-host domain |
| `cuda_device` | Device-accessible CUDA-domain storage |
| `imported` | A bounded borrowed opaque/import handle |
| `dma_mapped` | Device-accessible storage represented by a bounded borrowed opaque handle |
| `peer` | Device-accessible peer-domain storage represented by a bounded borrowed opaque handle and a peer topology link |

A domain has a nonzero stable backend-local identity, supported ownership-mode
bits, positive maximum bytes, positive power-of-two byte/alignment/offset
granularities, host/device access bits, one coherency model, required
synchronization bits, and nonzero topology-node and timestamp-domain
identities. Those referenced identities must exist in the same copied backend
snapshot.

Ownership has three distinct meanings: `borrowed_host` for host storage,
`borrowed_opaque` for an opaque/import handle, and `backend` for backend-private
payload ownership. These describe payload lifetime. Runtime separately owns
its copied descriptor and the registration token returned by the backend until
checked unregister succeeds.

Coherency is one of `host_coherent`, `explicit_flush_invalidate`,
`staged_copy`, or `device_only`. A host-coherent domain requires no
synchronization action. Explicit-flush/invalidate and staged-copy domains must
declare the matching action bits. Device-only domains cannot advertise host
access. Unknown flags or enums, contradictory access/ownership/coherency facts,
zero or non-power-of-two granularities, overflow, duplicate identities, and
nonzero reserved data reject the complete snapshot transactionally.

These declarations report a backend contract; they are not independent proof
that an OS or vendor operation pinned, mapped, imported, or made memory
coherent. In particular, `mlock`, an M15 provider pin observation, and an
injected extension callback do not establish a CUDA, DMA, or peer property.

## Topology

`HalV2TopologyNode` represents a stable backend-local host, NUMA, device, DMA
endpoint, or peer endpoint identity. `HalV2TopologyLink` represents one stable
directed local, host-access, device-access, DMA, or peer edge. A reverse route
requires a separate directed link.

Every endpoint must exist exactly once. Duplicate identities and duplicate
source/destination/kind triples are rejected. A self-link is valid only for the
`local` kind. A peer domain and a peer link must either both be present or both
be absent. Runtime-facing domain, node, and timestamp handles also contain the
instance-owned backend handle, so a handle from another backend or Runtime
cannot be substituted.

Topology identities are opaque numbers, not pointers or buffer addresses. A
node or link makes no bandwidth, distance, latency, IOMMU, physical-route, or
direct GPU-to-FPGA peer-DMA claim.

## Completion timestamps and correlation

Each native snapshot names the timestamp domain of
`HalV2Completion::device_timestamp_ns`. A timestamp-domain descriptor declares
its backend-local identity, kind, rational nanoseconds-per-tick numerator and
denominator, wrap interval, monotonic byte, backend-reset behavior, and optional
single destination for explicit correlation.

A populated device timestamp is never interpreted as Runtime monotonic time by
itself. `Runtime::device_completion_timestamp_domain()` returns its declared
domain. `Runtime::query_device_timestamp_correlation()` is an explicit fixed-
output host-control call available only while Runtime is running and idle. It
rejects an active step, periodic run, replay, cleanup-pending state, foreign
handle, undeclared relation, unsupported backend, callback error, exception, or
malformed output.

A valid `HalV2TimestampCorrelation` repeats the requested source and
destination identities and contains a nonzero generation, sampled values, and
finite uncertainty. Runtime neither invents a sample nor caches, extrapolates,
or converts one across reset generations. A sample and its uncertainty are
diagnostic facts, not a device-latency or real-time result.

## Registration and ownership

The pre-M17-02 `DeviceBufferRegistration` prefix and overload keep their source
meaning: a named, nonempty, borrowed, host-addressable span with the existing
device-ABI-v1 access flags. Runtime maps it to the backend's coherent borrowed-
host domain and retains the M17-01 core `register_buffer` and
`unregister_buffer` operations.

`HeterogeneousDeviceBufferRegistration` is a separate configuring-only
overload. It supplies a backend and instance-owned domain handle, exactly one
of a borrowed host span or bounded opaque handle, a positive declared byte
count, one ownership value, access, exact domain coherency, and exact required
synchronization. Runtime copies the descriptor and opaque bytes; it never copies
or adopts the host payload, imported allocation, mapped aperture, or device
storage. The overload returns the existing `DeviceBufferHandle`.

Registration rejects malformed or duplicate names, foreign backend/domain
handles, core-only backends, unknown domains, empty or dual backing, malformed
opaque tails, mismatched ownership/access/coherency/synchronization, byte or
backend-capacity overflow, byte-granularity violations, misaligned host bases,
and overlap with any comparable registered host span. Opaque regions have no
process address to compare; their non-aliasing and lifetime remain a trusted
backend/host obligation.

At start, backends initialize and mixed legacy/heterogeneous objects register
in deterministic registration order behind the existing startup gate. Native
heterogeneous objects use `register_memory`; legacy objects use the unchanged
HAL core operation. Runtime marks a heterogeneous registration ownership-
uncertain before entering the callback and retains any returned token on error
or exception. Failure cleans successful and uncertain registrations in reverse
order, continues independent cleanup, retains the first error, and runs no
application callback.

Checked stop unregisters unresolved memory objects in reverse order before
backend shutdown. Only successful unregister clears the copied token and
ownership marker. A failure leaves cleanup retryable and requires the host to
retain borrowed spans, opaque handles, extension instances, and backend objects.
The destructor remains only a best-effort fallback.

## Submission and synchronization boundary

Device submissions continue to reference the existing runtime-local
`DeviceBufferHandle`. Runtime validates owner, backend, declared byte range,
and device access and translates the handle to the backend's nonzero submission
token.

Legacy coherent buffers and heterogeneous objects whose declared
synchronization is `none` can use the M17-01 core submission. A reference that
requires flush, invalidate, host/device copy, or timeline synchronization
returns an explicit device error on the legacy single-submit path. M17-03 batch
phases discharge flush, invalidate, copy-to-device, or copy-from-device only
with an explicit correctly ordered command covering the referenced range. A
timeline never discharges coherence. Runtime inserts no hidden operation and
infers no cross-device migration, CUDA Graph, XDMA MMIO/event work, or direct
peer DMA.

## Inspection, identity, and accounting

Runtime provides fixed-copy count/index inspectors for memory domains,
topology nodes and links, timestamp domains, the completion timestamp domain,
and canonical memory objects. They are available after accepted backend/buffer
registration through finalized, running, and stopped states, reject foreign
handles, and do not invoke discovery or mutate the copied snapshot.

Adapted device-ABI-v1 and native core-only registrations synthesize only one
implicit borrowed-host, host-coherent, no-synchronization domain. They expose no
invented pinned-host, CUDA-device, imported, DMA-mapped, peer, topology,
timestamp, or correlation capability and retain their exact M17-01 behavior and
graph/replay identity.

For a native extension, graph/replay identity includes every semantic copied
domain, topology, timestamp, and correlation-capability field. An explicit
heterogeneous registration additionally contributes its semantic domain,
bytes, ownership, access, coherency, synchronization, and opaque-handle bytes.
Runtime addresses, callback pointers, extension instances, native registration
tokens, correlation samples, and mutable health do not participate. Checkpoint
and input-log codecs and schemas remain version 1.

Copied extension tables, complete fixed snapshots, canonical registration
descriptors, native tokens, and validation state are Runtime-owned device
controls. They are counted exactly once in `device_control_bytes` and the M15
logical device-control extent. Borrowed host payloads, opaque/imported/device
storage, mapped apertures, and backend-private bytes remain excluded from
`planned_bytes`. The six-row MemoryPlan equation and three provider-capable
regions remain unchanged.

After successful start, snapshot inspection, reference translation, coherent
submission/poll/completion, health/reset, correlation query, failure, and
checked cleanup use fixed storage. M17-02 adds no hidden thread, blocking mutex,
unbounded wait/retry, spill path, callback from poll, or file/network I/O.

## Evidence and claim boundary

Portable tests may cover exact defaults and layouts, all six synthetic domain
kinds, discovery validation, malformed topology/timestamps, correlation
failure, mixed registration rollback/retry, identity, accounting, package
source compatibility, allocation instrumentation, and multiple Runtime
instances. The unchanged mock, CUDA candidate, and XDMA candidate remain
device-ABI-v1 backends and exercise only the implicit legacy host domain through
the adapter.

Those tests, fake drivers, hosted CI, preflight, and this document establish no
physical pinning, CUDA-device memory, imported interoperability, DMA map, peer
access, real topology, timestamp accuracy, HIL, hardware, field, latency,
thermal, endurance, RT1, RT2, signing, release, deployment, or production
evidence. M17-03 adds portable synthetic batch/timeline and explicit
memory-order validation only. M17-04 adds native vendor-v2 controls and M17-06
adds portable simulated composition. M17 and CAP-M17 remain incomplete pending
external gates; physical combined execution and qualification belong to M18.

## M17-04 vendor staged domains

M17-04 preserves both CUDA and XDMA device-ABI-v1 paths while adding explicit
native-v2 registrations. The CUDA registration publishes a borrowed-host
staging domain and one staged CUDA domain. A heterogeneous registration copies
its stable name and metadata and maps to either an existing pre-bound device
address or the candidate's existing host-registration/device-mirror setup. Its
coherency is `staged_copy`, and its exact obligations are
`copy_to_device | copy_from_device`; Runtime inserts neither operation.

The XDMA native registration publishes borrowed host staging associated with
the configured AXI-MM endpoint. H2C/C2H remain explicit command operations and
do not make host pages pinned or DMA mapped. The control aperture itself is not
a generic FPGA memory domain, and user events create no memory-coherency or
interrupt-latency fact.

For both vendors, copied domains/topology/timestamp records and heterogeneous
declarations enter the existing conditional identity. Host pointers, CUDA
addresses, graph handles, Linux descriptors, and runtime-private tokens do not.
Portable fake callbacks establish record and ordering behavior only, not
physical pinning, allocation, DMA, coherency, topology, or timestamp accuracy.

## M17-06 combined host-stage boundary

The portable combined sample discovers the CUDA staged domain and XDMA
borrowed-host coherent domain, then registers two separate fixed host objects.
They have equal explicit extents but disjoint addresses and are never
dual-registered. CUDA copy-to/copy-from commands discharge only the staged
CUDA object's declared synchronization. After successful CUDA completion, an
ordinary CPU phase copies the exact bytes into the XDMA-owned host object.

No memory token, object, opaque handle, timeline, fence, or coherency promise
crosses the backend boundary. The bridge issues no vendor call or implicit
synchronization and does not represent pinned memory, DMA mapping, peer memory,
direct peer DMA, or physical topology.

# Current state

Last audited: 2026-08-15
M19-02 contractual baseline: `dbedb8ba9779b0a39676576326d689702659872f`
Review worktree head: `db8d14a523e6c79895ade4a0db003513f625312f`

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1, HAL core v2, memory/topology extension v1, and
  command/timeline extension v1 are unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
- The installed target inventory, 1.x aliases, support matrices, and
  Apache-2.0 license are unchanged. The default header inventory adds exactly
  `rt/extension_abi.h`.
- M14, M14.1, M15, and M16 are complete. M17-01 through M17-04 are merged in
  target history. M17 remains active and incomplete because M17-05 is blocked.
  M18-01 offline qualification schemas and tools are implemented from the
  completed M17-04 foundation; M18 and CAP-M18 remain incomplete. M19-01 adds
  extension ABI v1. An uncommitted M19-02 Unreal adapter candidate is under
  review; M19 and CAP-M19 remain incomplete.

## M19-02 Unreal adapter candidate

The opt-in source plugin binds the existing host-executor, three-region
memory-provider, Runtime clock, and host-frame surfaces to the pinned Unreal
Engine 5.8.1 source tuple. The runtime module is passive and adds no default
CMake target, installed header, package component, Runtime/world registry,
delegate, worker, loader, or C ABI surface. The exact ownership, task-node,
allocator, clock, build, and claim boundaries are in
`docs/unreal_adapter.md`.

This review repaired pinned-UBT rules compatibility, the Unreal workflow's
build-before-install order, retained command logging, and missing allocator
observe-failure rollback coverage. Exact engine C++ build and automation are
not claimed complete in this worktree until the approved runner passes the
mandatory workflow and human review. M19-03 still owns world lifecycle,
checked extension detach, module unload, and hot reload.

## M19-01 extension registration

The installed C11 `rt/extension_abi.h` and additive C++ Runtime surface accept
an already-resolved entry function while configuring. One bounded transaction
copies names, descriptor/table prefixes, and local relationships and may
publish CPU phases, device-ABI-v1 backends through the canonical adapter, and
host-control services. Failed attempts publish nothing and retire provisional
generations.

Services initialize before Runtime lanes and backends. Checked stop closes
admission, retains the first error while continuing independent cleanup,
retries unresolved owners, and shuts services down in reverse order only after
related backend ownership is released. Checked detach clears borrowed callable
pointers, retires the handle generation, and reports host unload readiness; it
never unloads a module. See `docs/extension_registration.md`.

The change adds no C ABI export, loader dependency, target, Runtime lane,
MemoryPlan row, schema, native HAL-v2 extension capability, or M17-05 bypass.
Extensions remain trusted in-process code, and no physical, HIL, field,
Unreal, RT1/RT2, signing, release, deployment, or production validation is
claimed.

## M17-04 implementation

The installed CUDA and XDMA candidate headers retain their exact device-ABI-v1
`api()` paths and add native `hal_v2_registration(name)` paths. Each native
registration exposes HAL core v2, memory/topology extension v1, and
command/timeline extension v1 tables. The command/timeline tables do not
currently pass canonical Runtime discovery because of the M17-05 blocker
below. Each backend object admits only one chosen registration path until
checked shutdown succeeds.

Both injectable driver tables retain their complete version-1 positional
prefix and version-1 default. Exact version 2 tables add only the native
controls: CUDA Graph launch, or XDMA 32-bit control read/write, user-event wait,
and nonblocking stop request. A version-1 table exposes none of those
capabilities.

CUDA native-v2 describes one explicit staged host/device domain and
host-monotonic completion timestamps. Up to 16 caller-owned, already
instantiated graph executables may be registered before initialization under
unique identifiers 1 through 65535, with up to eight copied stable buffer
bindings. Graph dispatch uses `0x43470000 | graph_id`, zero inline payload, and
the exact declared bindings. One configured command stream executes explicit
copies, registered kernels, and graphs in order and records one precreated
completion event after the accepted batch is fully enqueued. RTFW never
captures, instantiates, updates, clones, uploads, destroys, or owns a graph.

XDMA native-v2 describes borrowed host staging, its configured AXI-MM endpoint,
and host-monotonic completion timestamps. Stable vendor commands encode
little-endian control read as `0x58480000 | offset/4`, control write as
`0x58490000 | offset/4`, and user-event wait as `0x584A0000 | event_index`.
The aperture is explicitly enabled, four-byte aligned, nonzero, and at most
262144 bytes; at most 16 events are configured. The Linux adapter copies
optional user-BAR and event paths, opens only configured endpoints, and uses a
nonblocking stop wakeup for finite event waits. All operations remain on the
existing fixed backend I/O team.

The new graph, control, event, registry, queue, and worker storage is fixed
backend-private storage reported through existing capability fields. Runtime
adds no lane, MemoryPlan row, provider region, schema, status, implicit command,
cross-backend timeline, spill, detached work, or executor-worker vendor call.
Malformed commands fail before vendor entry; uncertain CUDA or XDMA work
retains every referenced owner until physical readiness, successful drain, or
the documented final host reclamation boundary.

## M17-05 review boundary and next action

M17-04 is merged at the exact M17-05 baseline and establishes static, unit,
malformed/failure-injected, fake CUDA Graph, fixture XDMA control/event,
package, lifecycle, compatibility, and portable RT0 protocol evidence only.

Independent M17-05 review found that canonical Runtime command/timeline
discovery poisons the capability output header before invocation while both
native CUDA and XDMA candidate callbacks require a preinitialized incoming
size. The required `hal_v2_registration()` composition therefore fails before
graph configuration. Repair requires a separately approved change under the
currently forbidden `rt/src/` paths. The combined sample and its behavioral
test are not implemented, and M17/CAP-M17 remain incomplete.

See `docs/evidence/M17-05-2026-08-12.md`. Physical CUDA/XDMA memory, MMIO,
interrupts, timing, HIL, field, RT1/RT2, support promotion, signing, release,
deployment, and production qualification remain deferred.

## M18-01 offline qualification plane

Four independent JSON Schema draft 2020-12 version-1 documents define a
separately retained campaign plan, immutable qualification record, human
promotion review, and generated promotion proposal. The Python 3.11
standard-library tool validates complete scope-specific tuple, policy,
workload, trial, threshold, resource, thermal, health, recovery, artifact, and
digest chains with bounded strict JSON and filesystem handling.

Synthetic fixtures cover NVIDIA, XDMA, combined, RT1, and RT2. They are labeled
`synthetic_fixture`, generate only `proposal_only` output, and are never
support-matrix eligible. A non-synthetic combined proposal is explicitly
blocked until M17-05 is repaired. Reviewer names and timestamps are not
authenticated, and external pre-run plan provenance remains a human gate.

See `docs/qualification.md`. M18-01 changes no runtime, ABI, installed package,
version, support matrix, hardware workflow/sample, or prior evidence. It
performs no physical hardware, controlled timing, HIL, field, RT1/RT2, signing,
release, deployment, production, or support-promotion validation.

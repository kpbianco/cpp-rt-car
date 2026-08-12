# Current state

Last audited: 2026-08-12
Batch baseline: `122f9919674b5f09e321deb8ba6701f78b2993ce`

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1, HAL core v2, memory/topology extension v1, and
  command/timeline extension v1 are unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
- The installed header and target inventory, 1.x aliases, support matrices,
  and Apache-2.0 license are unchanged.
- M14, M14.1, M15, and M16 are complete. M17-01 through M17-03 are merged in
  target history. M17 is active and incomplete; M17-04 is the approved batch.

## M17-04 implementation

The installed CUDA and XDMA candidate headers retain their exact device-ABI-v1
`api()` paths and add native `hal_v2_registration(name)` paths. Each native
registration supplies HAL core v2, memory/topology extension v1, and
command/timeline extension v1 through the existing canonical Runtime manager.
Each backend object admits only one chosen registration path until checked
shutdown succeeds.

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

## Boundary and next action

M17-04 establishes static, unit, malformed/failure-injected, fake CUDA Graph,
fixture XDMA control/event, package, lifecycle, compatibility, and portable
RT0 protocol evidence only. M17 and CAP-M17 remain incomplete. The
combined CPU-GPU-FPGA-CPU sample belongs to M17-05; physical CUDA/XDMA memory,
MMIO, interrupts, timing, HIL, field, RT1/RT2, support promotion, signing,
release, deployment, and production qualification remain deferred.

Run every command in `contracts/active-batch.yaml`, retain exact results in
`docs/evidence/M17-04-2026-08-12.md`, and leave mandatory GitHub CI and human
API, compatibility, concurrency, lifetime, MMIO-safety, accounting, security,
and claim review as external gates.

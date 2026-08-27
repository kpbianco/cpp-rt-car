# Current state

Last audited: 2026-08-27
Batch baseline: `46a69204ecccd17e7c1da76e30d4e23bde6eb9ef`

## Product state

- Release 1.2.1 remains the supported portable RT0 product.
- Stable C ABI v8 remains exactly 70 exports with SONAME 8 and its frozen
  fingerprint. Device ABI v1, HAL core v2, memory/topology extension v1, and
  command/timeline extension v1 are unchanged.
- Runtime-profile schema 7 and its 25 keys, global observability schema 2,
  checkpoint/input-log schema 1, and rate-action schema 1 are unchanged.
- The installed target inventory, 1.x aliases, support matrices, and
  Apache-2.0 license are unchanged. The retained M19 addition is
  `rt/extension_abi.h`; M21-04 adds exactly the additive C++ header
  `rt/loopback_backend.hpp`.
- M14, M14.1, M15, and M16 are complete. M17-01 through M17-04 and the
  M17-06 portable discovery/composition repair are merged. M17 and CAP-M17
  remain incomplete because named physical CUDA/XDMA, combined-hardware, RT,
  and manual qualification records do not exist. M18-01 offline qualification
  schemas and tools remain proposal-only and unpromoted. M19-01 adds extension
  ABI v1; M19 and CAP-M19 remain incomplete, and Unreal work is not part of the
  current Linux-host batch. M20-PRE-01, M21-01, and M21-02 are merged. M21-03
  is merged. M21-04 is the active approved sampled-I/O and portable-loopback
  batch.

## M21-04 sampled I/O and loopback

M21-04 adds copied fixed-capacity sampled input/output descriptors over one
exact admitted M21-03 payload endpoint. Finalization closes encoding and frame
geometry, rational scaling, units/calibration, sample interval, clock,
timestamp and trigger identities, sequence/generation, ring capacity,
stale/overrun/underrun policy, and exact initial/startup/failure/shutdown
frames. Device input publishes only after exact terminal completion and full
frame validation. Startup and checked stop submit safe outputs through the
existing backend lane and require terminal acknowledgement.

The installed `SampledIoLoopbackBackend` provides deterministic host-coherent
HAL-v2 frame transfer and bounded fault injection for portable tests. This is
RT0 software-loopback behavior only; M21-05 replay/telemetry/overload closure,
vendor/physical I/O, electrical and timing validation, and RT1/RT2 remain
unclaimed. M21 and CAP-M21 remain incomplete.

## M21-03 CPU/device cross-rate payloads

M21-02's admitted dispatch/completion path remains unchanged for channel-free
plans. M21-03 appends explicit producer/consumer device selectors to the C++
cross-rate registration. Each selector names one ordered copied M21-01 payload
reference and a positive slot stride. Finalization accepts CPU→device or
device→CPU only, proves host-coherent access and role/direction agreement, and
derives one disjoint exact payload subrange for every admitted in-flight slot.

Before a device consumer provider runs, Runtime selects and copies the exact
CPU-produced generation into its derived input subrange. The provider must
still match the complete frozen declaration; only Runtime then substitutes the
selected offset/byte count in its owned materialization. Device output remains
unpublished until M21-02 correlates terminal success. Runtime retains terminal
slot ownership, copies the exact output subrange into the SPSC store, records
release/completion timestamp metadata, publishes one generation, and only then
releases dependent work and recycles the device slot.

This is portable RT0 fake-driver payload evidence. It adds no sampled-clock,
trigger, calibration, safe-output, overrun/underrun, replay, physical CUDA/
XDMA/DAC/DAQ, HIL, RT1, or RT2 claim. M21-04, M21-05, and CAP-M21 remain
incomplete.

## M21-02 device-rate dispatch and completion

M21-01's additive device-rate model and conservative cyclic admission are now
merged. M21-02 activates only admitted HAL-v2 command-batch references in an
M16 active plan. Finalization precomputes reference-to-device indexes and
same-group device dependency slices, and allocates one exact generation-tagged
completion ticket per reference record. The provider receives the appended
nullable `DeviceCallbackContext::rate_release` view and still materializes only
values permitted by the copied M17 declaration.

Each due provider runs exactly once through the existing executor, copies its
fixed-capacity batch and exact domain/phase/cycle/sequence/substep identity into
an existing backend slot, and returns without holding the executor phase open.
The existing per-backend submission lane performs vendor submit; the existing
service lane polls completion and owns submitted timeout cancellation.
Independent same-group device references can therefore remain outstanding at
the same time. Precomputed graph dependencies consume prerequisite tickets
before a dependent CPU/device callback, and the release-group barrier consumes
all remaining tickets before a successful active step advances.

Provider timeouts are finite and clamped to the earlier checked completion-
budget or release-deadline remainder. A timeout after vendor ownership enters a
non-reusable terminal quarantine, issues at most one cancellation request, and
cannot become success when a late completion arrives. Checked backend shutdown
is the reclamation boundary. Existing device metrics/traces and active-rate
failure/action accounting are reused without schema changes.

This remains the merged portable RT0 dispatch/completion foundation. Its
channel-free behavior, schemas, lane count, and qualification boundary are
unchanged by M21-03.

## M20-PRE-01 portable assurance

The active batch adds one host-independent `scripts/verify-portable-assurance.sh`
entry point with independently runnable `dependencies`, `static`, `fuzz`,
`artifacts`, and cumulative `all` modes. It reconciles exact build/action pins,
the complete default first-party Clang 14 compilation manifest, two supported
64 KiB parser fuzzers and one explicitly experimental 4 KiB queue fuzzer, a
canonical SPDX 2.3 candidate SBOM, an unsigned in-toto Statement v1 candidate,
an expected-source final manifest, offline RSA/DSSE verification of a retained
public non-target fixture, and extraction/relocated consumption of the default
package.

The lane has repository-read-only GitHub permissions and no private-key,
signing, OIDC, attestation-creation, publication, release, hardware, privileged
host, controlled-timing, or Unreal path. Its machine reports are candidate CI
evidence only. They do not establish continuous fuzzing, vulnerability or
license clearance, reproducible builds, authenticated RTFW provenance, a SLSA
level, support promotion, hardware/RT qualification, or production readiness.
See `docs/portable_assurance.md`.

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
MemoryPlan row, schema, or native HAL-v2 extension capability.
Extensions remain trusted in-process code, and no physical, HIL, field,
Unreal, RT1/RT2, signing, release, deployment, or production validation is
claimed.

## M17-04 implementation

The installed CUDA and XDMA candidate headers retain their exact device-ABI-v1
`api()` paths and add native `hal_v2_registration(name)` paths. Each native
registration exposes HAL core v2, memory/topology extension v1, and
command/timeline extension v1 tables. M17-06 makes those tables reachable
through canonical Runtime discovery without changing either vendor callback.
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

## M17-06 Runtime repair and portable combined graph

`discover_command_timeline_extension()` now passes a value-initialized
`HalV2CommandTimelineCapabilities` to the callback. Its default member
initializers supply the exact size/version input prefix while every semantic
and reserved field starts zero. Existing callback status mapping and complete
returned-record validation remain fail closed. Focused tests cover exact input,
malformed/partial/error/exception output, transactional rejection, corrected
retry, and simultaneous native CUDA/XDMA registration in one real Runtime.

The default, non-installed `sample_cpu_gpu_fpga_cpu` composes exactly CPU
prepare, simulated CUDA upload/Graph/download, a disjoint bounded host bridge,
simulated XDMA H2C/control/event/C2H, and CPU validation. Two separate
backend-local timelines, fixed payload/device/card/control/event storage,
finite timeouts, exact call/thread/cause assertions, post-start allocation
instrumentation, failure suppression, cancellation, recovery, isolation, and
checked cleanup are covered by its focused CTest executable. No timeline or
memory object crosses backends and no direct peer DMA is represented.

This is portable RT0 simulated-protocol evidence only. The portable M17-06
software path is merged, but physical CUDA/XDMA memory, Graph execution, MMIO,
interrupts, timing,
HIL, field, RT1/RT2, support promotion, signing, release, deployment, and
production qualification remain deferred. The failed M17-05 review remains
immutable in `docs/evidence/M17-05-2026-08-12.md`; current results are retained
in `docs/evidence/M17-06-2026-08-23.md`.

## M18-01 offline qualification plane

Four independent JSON Schema draft 2020-12 version-1 documents define a
separately retained campaign plan, immutable qualification record, human
promotion review, and generated promotion proposal. The Python 3.11
standard-library tool validates complete scope-specific tuple, policy,
workload, trial, threshold, resource, thermal, health, recovery, artifact, and
digest chains with bounded strict JSON and filesystem handling.

Synthetic fixtures cover NVIDIA, XDMA, combined, RT1, and RT2. They are labeled
`synthetic_fixture`, generate only `proposal_only` output, and are never
support-matrix eligible. M18-01's unchanged qualification tool still rejects a
non-synthetic combined proposal; revising that promotion policy is outside
M17-06. Reviewer names and timestamps are not authenticated, and external
pre-run plan provenance remains a human gate.

See `docs/qualification.md`. M18-01 changes no runtime, ABI, installed package,
version, support matrix, hardware workflow/sample, or prior evidence. It
performs no physical hardware, controlled timing, HIL, field, RT1/RT2, signing,
release, deployment, production, or support-promotion validation.

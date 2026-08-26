# Current state

Last audited: 2026-08-26
Batch baseline: `d463c3a6896e54d10814f1d4ed3b05d355550900`

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
- M14, M14.1, M15, and M16 are complete. M17-01 through M17-04 and the
  M17-06 portable discovery/composition repair are merged. M17 and CAP-M17
  remain incomplete because named physical CUDA/XDMA, combined-hardware, RT,
  and manual qualification records do not exist. M18-01 offline qualification
  schemas and tools remain proposal-only and unpromoted. M19-01 adds extension
  ABI v1; M19 and CAP-M19 remain incomplete, and Unreal work is not part of the
  current Linux-host batch. M20-PRE-01 is merged. M21-01 is the active
  approved model/admission-only batch.

## M21-01 device-rate model and admission

The additive C++ `DeviceRatePhaseBinding` attaches one copied completion
budget, per-phase in-flight limit, and access-matched payload-role sequence to
an already registered HAL-v2 command-batch phase and rate domain. Runtime
continues to derive backend, commands, buffer slices, synchronization, and
backend-local wait/signal handles solely from the copied M17 declaration.

Finalization now retains command, payload-reference, timeline, phase, backend,
and per-reference interval inspection. Checked integer cyclic admission uses
half-open intervals, completion-before-release ties, previous-supercycle carry,
per-phase/backend/global in-flight bounds, and exact per-backend completion-
batch boundaries. New storage is included in `rate_plan_bytes`,
`runtime_control_bytes`, and the control ledger; mixed metadata participates in
graph/replay identity while no-device and CPU-only paths retain their prior
conditional hash path.

This batch performs no rate-triggered submit, poll, cancel, payload
publication, sampled I/O, telemetry-schema change, or lane/thread creation.
Starting an active mixed plan returns `invalid_state` before preflight,
provider, backend, or vendor callbacks. Reference-only mixed plans retain the
ordinary frame behavior; M21-02 owns active dispatch and completion. M21 and
CAP-M21 remain incomplete.

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
